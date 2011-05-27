#define __STDC_LIMIT_MACROS
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <epicsThread.h>
#include <epicsExport.h>
#include <asynPortDriver.h>
#include <ellLib.h>
#include <iocsh.h>

#include "classes.h"
#include "parser.h"
#include "unpack.h"
#include "rtutils.h"
#include "msgsock.h"
#include "messages.h"

#include "gadc.h"
#include "ecAsyn.h"

struct sampler_config_t
{
    ELLNODE node;
    char * port;
    int channel;
    char * sample;
    char * cycle;
};

ELLLIST sampler_configs;
void Configure_Sampler(char * port, int channel, char * sample, char * cycle)
{
    sampler_config_t * conf = (sampler_config_t *)calloc(1, sizeof(sampler_config_t));
    if(port == NULL || sample == NULL)
    {
        printf("adc configure error: port %p sample %p\n", port, sample);
    }
    conf->port = strdup(port);
    conf->channel = channel;
    conf->sample = strdup(sample);
    if(cycle != NULL)
    {
        conf->cycle = strdup(cycle);
    }
    ellAdd(&sampler_configs, &conf->node);
}

/* used in 99.9% of programs */
static char * format(const char *fmt, ...)
{
    char * buffer = NULL;
    va_list args;
    va_start(args, fmt);
    int ret = vasprintf(&buffer, fmt, args);
    assert(ret != -1);
    va_end(args);
    return buffer;
}

static char * makeParamName(EC_PDO_ENTRY_MAPPING * mapping)
{
    char * name = format("%s.%s", mapping->pdo_entry->parent->name, mapping->pdo_entry->name);
    // remove spaces
    int out = 0;
    for(int n = 0; n < (int)strlen(name); n++)
    {
        if(name[n] != ' ')
        {
            name[out++] = name[n];
        }
    }
    name[out] = '\0';
    return name;
}

static EC_PDO_ENTRY_MAPPING * mapping_by_name(EC_DEVICE * device, const char * name)
{
    for(NODE * node = listFirst(&device->pdo_entry_mappings); node; node = node->next)
    {
        EC_PDO_ENTRY_MAPPING * mapping = (EC_PDO_ENTRY_MAPPING *)node;
        char * entry_name = makeParamName(mapping);
        int match = strcmp(name, entry_name) == 0;
        free(entry_name);
        if(match)
        {
            return mapping;
        }
    }
    return NULL;
}

class Sampler
{
protected:
    ecAsyn * parent;
    EC_PDO_ENTRY_MAPPING * sample;
public:
    gadc_t * adc;
    struct Node
    {
        ELLNODE node;
        Sampler * self;
    };
    Node node;
    Sampler(ecAsyn * parent, int channel, 
               EC_PDO_ENTRY_MAPPING * sample) : 
        parent(parent), sample(sample),
        adc(gadc_new(parent, channel))
        {
            node.self = this;
        }
    virtual void on_pdo_message(PDO_MESSAGE * pdo, int size)
        {
            parent->lock();
            gadc_put_sample(adc, cast_int32(sample, pdo->buffer, 0));
            parent->unlock();
        }
    virtual ~Sampler() {}
};

class Oversampler : public Sampler
{
    EC_PDO_ENTRY_MAPPING * cycle;
    int lastCycle;
    int missed;
    int P_Missed;
public:
    Oversampler(ecAsyn * parent, int channel, 
               EC_PDO_ENTRY_MAPPING * sample, EC_PDO_ENTRY_MAPPING * cycle) : 
        Sampler(parent, channel, sample), cycle(cycle), lastCycle(0), missed(0) 
        {
            parent->createParam(format("XFC%d_MISSED", channel),
                                asynParamInt32, &P_Missed);
        }
    virtual void on_pdo_message(PDO_MESSAGE * pdo, int size)
        {
            int stride = parent->device->oversampling_rate;
            int32_t cyc = cast_int32(cycle, pdo->buffer, 0);
            if(lastCycle == cyc)
            {
                // skip duplicates
                return;
            }
            parent->lock();
            if((lastCycle + 1) % 65536 != cyc)
            {
                parent->setIntegerParam(P_Missed, missed++ & INT32_MAX);
            }
            lastCycle = cyc;
            int16_t * samples = (int16_t *)(pdo->buffer + sample->offset);
            for(int s = 0; s < stride; s++)
            {
                gadc_put_sample(adc, samples[s]);
            }
            parent->unlock();
        }
};

LIST * ethercat_pdo_listeners = NULL;

struct ENGINE_USER
{
    ecMaster * master;
    LIST ports;
    EC_CONFIG * config;
    rtMessageQueueId config_ready;
    int count;
    rtMessageQueueId writeq;
    void * config_buffer;
    int config_size;
};

static const int N_RESERVED_PARAMS = 10;

ecMaster::ecMaster(char * name) :
    asynPortDriver(name,
                   1, /* maxAddr */
                   NUM_MASTER_PARAMS, /* max parameters */
                   asynInt32Mask | asynDrvUserMask, /* interface mask*/
                   asynInt32Mask, /* interrupt mask */
                   0, /* non-blocking, no addresses */
                   1, /* autoconnect */
                   0, /* default priority */
                   0) /* default stack size */,
    lastCycle(0), missed(0)
{
    printf("create master %s\n", name);
    createParam("Cycle", asynParamInt32, &P_Cycle);
    createParam("WorkingCounter", asynParamInt32, &P_WorkingCounter);
    createParam("Missed", asynParamInt32, &P_Missed);
    createParam("WcState", asynParamInt32, &P_WcState);
}

ecAsyn::ecAsyn(EC_DEVICE * device, int pdos, rtMessageQueueId writeq, int devid) :
    asynPortDriver(device->name,
                   1, /* maxAddr */
                   pdos + N_RESERVED_PARAMS + gadc_get_num_parameters() * 8, /* max parameters */
                   asynInt32Mask | asynInt32ArrayMask | asynDrvUserMask, /* interface mask*/
                   asynInt32Mask, /* interrupt mask */
                   0, /* non-blocking, no addresses */
                   1, /* autoconnect */
                   0, /* default priority */
                   0) /* default stack size */,
    pdos(pdos), devid(devid), writeq(writeq), device(device)
{

    ellInit(&samplers);

    printf("ecAsyn INIT %s PDOS %d\n", device->name, pdos);
    int * PdoParam = new int[pdos]; /* leak */
    mappings = new EC_PDO_ENTRY_MAPPING *[pdos]; /* leak */
    int n = 0;
    
    printf("device type %s\n", device->type_name);

    for(ELLNODE * node = ellFirst(&sampler_configs); node; node = ellNext(node))
    {
        sampler_config_t * conf = (sampler_config_t *)node;
        if(strcmp(conf->port, device->name) == 0)
        {
            Sampler * s = NULL;
            EC_PDO_ENTRY_MAPPING * sample_mapping = mapping_by_name(device, conf->sample);
            if(sample_mapping != NULL)
            {
                if(conf->cycle == NULL)
                {
                    s = new Sampler(this, conf->channel, sample_mapping);
                }
                else
                {
                    EC_PDO_ENTRY_MAPPING * cycle_mapping = mapping_by_name(device, conf->cycle);
                    if(cycle_mapping != NULL)
                    {
                        s = new Oversampler(this, conf->channel, sample_mapping, cycle_mapping);
                    }
                }
            }
            if(s != NULL)
            {
                ellAdd(&samplers, &s->node.node);                
            }
        }
    }
    
    for(NODE * node = listFirst(&device->pdo_entry_mappings); node; node = node->next)
    {
        EC_PDO_ENTRY_MAPPING * mapping = (EC_PDO_ENTRY_MAPPING *)node;
        char * name = makeParamName(mapping);
        printf("createParam %s\n", name);
        createParam(name, asynParamInt32, PdoParam + n);
        mapping->pdo_entry->parameter = PdoParam[n];
        mappings[n] = mapping;
        n++;
    }

    createParam("AL_STATE", asynParamInt32, &P_AL_STATE);
    createParam("ERROR_FLAG", asynParamInt32, &P_ERROR_FLAG);

}

void ecMaster::on_pdo_message(PDO_MESSAGE * pdo, int size)
{
    lock();
    if(pdo->cycle != lastCycle + 1)
    {
        missed++;
    }
    lastCycle = pdo->cycle;
    setIntegerParam(P_Cycle, pdo->cycle);
    setIntegerParam(P_WorkingCounter, pdo->working_counter);
    setIntegerParam(P_WcState, pdo->wc_state);
    setIntegerParam(P_Missed, missed & INT32_MAX);
    callParamCallbacks();
    unlock();
}

void ecAsyn::on_pdo_message(PDO_MESSAGE * pdo, int size)
{
    lock();
    for(ELLNODE * node = ellFirst(&samplers); node; node = ellNext(node))
    {
        ((Sampler::Node *)node)->self->on_pdo_message(pdo, size);
    }
    for(NODE * node = listFirst(&device->pdo_entry_mappings); node; node = node->next)
    {
        EC_PDO_ENTRY_MAPPING * mapping = (EC_PDO_ENTRY_MAPPING *)node;
        int32_t val = cast_int32(mapping, pdo->buffer, 0);
        setIntegerParam(mapping->pdo_entry->parameter, val);
    }
    char * meta = pdo->buffer + pdo->size + 2 * devid;
    assert(meta + 1 - pdo->buffer < size);
    setIntegerParam(P_AL_STATE, meta[0]);
    setIntegerParam(P_ERROR_FLAG, meta[1]);
    callParamCallbacks();
    unlock();
}

asynStatus ecAsyn::readInt32Array(asynUser *pasynUser, epicsInt32 *value,
                                  size_t nElements, size_t *nIn)
{
    for(ELLNODE * node = ellFirst(&samplers); node; node = ellNext(node))
    {
        gadc_t * adc = ((Sampler::Node *)node)->self->adc;
        if(gadc_has_parameter(adc, pasynUser->reason))
        {
            lock();
            asynStatus result = gadc_readInt32Array(
                adc, pasynUser->reason, value, nElements, nIn);
            unlock();
            return result;
        }
    }
    *nIn = 0;
    return asynError;
}

asynStatus ecAsyn::readInt32(asynUser *pasynUser, epicsInt32 *value)
{
    int reason = pasynUser->reason;
    for(ELLNODE * node = ellFirst(&samplers); node; node = ellNext(node))
    {
        gadc_t * adc = ((Sampler::Node *)node)->self->adc;
        if(gadc_has_parameter(adc, reason))
        {
            epicsInt32 new_value;
            if(gadc_readInt32(adc, reason, &new_value) == asynSuccess)
            {
                setIntegerParam(reason, new_value);
            }
            break;
        }
    }
    return asynPortDriver::readInt32(pasynUser, value);
}

asynStatus ecAsyn::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    asynStatus status = asynPortDriver::writeInt32(pasynUser, value);
    // offer the parameter to the ADC drivers
    for(ELLNODE * node = ellFirst(&samplers); node; node = ellNext(node))
    {
        gadc_t * adc = ((Sampler::Node *)node)->self->adc;
        if(gadc_has_parameter(adc, pasynUser->reason))
        {
            return gadc_writeInt32(adc, pasynUser->reason, value);
        }
    }
    if(pasynUser->reason >= 0 && pasynUser->reason < pdos)
    {
        EC_PDO_ENTRY_MAPPING * mapping = mappings[pasynUser->reason];
        WRITE_MESSAGE write;
        write.tag = MSG_WRITE;
        write.offset = mapping->offset;
        write.bit_position = mapping->bit_position;
        write.bits = mapping->pdo_entry->bits;
        write.value = value;
        rtMessageQueueSend(writeq, &write, sizeof(WRITE_MESSAGE));
    }
    return status;
}

int init_unpack(ENGINE_USER * usr, char * buffer, int size)
{
    EC_CONFIG * cfg = usr->config;
    int ofs = 0;
    int tag = unpack_int(buffer, &ofs);
    assert(tag == MSG_CONFIG);
    int scanner_config_size = unpack_int(buffer, &ofs);
    read_config2(buffer + ofs, scanner_config_size, cfg);
    ofs += scanner_config_size;
    int mapping_config_size = unpack_int(buffer, &ofs);
    parseEntriesFromBuffer(buffer + ofs, mapping_config_size, cfg);

    NODE * node;
    for(node = listFirst(&cfg->devices); node; node = node->next)
    {
        EC_DEVICE * device = (EC_DEVICE *)node;
        printf("%s\n", device->name);
        NODE * node1;
        for(node1 = listFirst(&device->pdo_entry_mappings); node1; node1 = node1->next)
        {
            EC_PDO_ENTRY_MAPPING * mapping = (EC_PDO_ENTRY_MAPPING *)node1;
            if(strcmp(mapping->pdo_entry->name, mapping->pdo_entry->parent->name) == 0)
            {
                printf("  %s\n", mapping->pdo_entry->name);
            }
            else
            {
                printf("  %s %s\n", mapping->pdo_entry->parent->name, mapping->pdo_entry->name);
            }
        }
    }
    return 0;
}

static void readConfig(ENGINE_USER * usr)
{
    EC_CONFIG * cfg = usr->config;
    NODE * node;
    int ndev = 0;
    for(node = listFirst(&cfg->devices); node; node = node->next)
    {
        EC_DEVICE * device = (EC_DEVICE *)node;
        int pdos = 0;
        NODE * node1;
        for(node1 = listFirst(&device->device_type->sync_managers); node1; node1 = node1->next)
        {
            EC_SYNC_MANAGER * sync = (EC_SYNC_MANAGER *)node1;
            NODE * node2;
            for(node2 = listFirst(&sync->pdos); node2; node2 = node2->next)
            {
                pdos++;
            }
        }
        PORT_NODE * pn = (PORT_NODE *)calloc(1, sizeof(PORT_NODE));
        pn->port = new ecAsyn(device, pdos, usr->writeq, ndev);
        ndev++;
        listAdd(&usr->ports, &pn->node);
    }
}

static int receive_config_on_connect(ENGINE * engine, int sock)
{
    printf("getting config\n");
    ENGINE_USER * usr = (ENGINE_USER *)engine->usr;
    int ack = 0;
    int size = rtSockReceive(sock, engine->receive_buffer, engine->max_message);
    if(size > 0)
    {
        if(usr->config_buffer == NULL)
        {
            usr->config_size = size;
            usr->config_buffer = calloc(size, sizeof(char));
            memcpy(usr->config_buffer, engine->receive_buffer, size);
            init_unpack(usr, engine->receive_buffer, size);
            readConfig(usr);
            rtMessageQueueSend(usr->config_ready, &ack, sizeof(int));
        }
        else
        {
            // check that the config hasn't changed
            assert(size == usr->config_size && 
                   memcmp(usr->config_buffer, engine->receive_buffer, size) == 0);
        }
    }
    return size > 0;
}

int pdo_data(ENGINE_USER * usr, char * buffer, int size)
{
    EC_MESSAGE * msg = (EC_MESSAGE *)buffer;
    assert(msg->tag == MSG_PDO);
    usr->master->on_pdo_message(&msg->pdo, size);
    // lock list
    for(NODE * node = listFirst(&usr->ports); node; node = node->next)
    {
        PORT_NODE * pn = (PORT_NODE *)node;
        pn->port->on_pdo_message(&msg->pdo, size);
    }
    // unlock list
    return 0;
}

static int ioc_send(ENGINE * server, int size)
{
    ENGINE_USER * usr = (ENGINE_USER *)server->usr;
    pdo_data(usr, server->receive_buffer, size);
    return 0;
}

static int ioc_receive(ENGINE * server)
{
    ENGINE_USER * usr = (ENGINE_USER *)server->usr;
    int size = rtMessageQueueReceive(usr->writeq, server->send_buffer, server->max_message);
    return size;
}

void makePorts(char * path, int max_message)
{
    ENGINE_USER * usr = (ENGINE_USER *)calloc(1, sizeof(ENGINE_USER));
    usr->master = new ecMaster("MASTER0");
    usr->config_ready = rtMessageQueueCreate(1, sizeof(int));
    usr->config = (EC_CONFIG *)calloc(1, sizeof(EC_CONFIG));
    usr->writeq = rtMessageQueueCreate(1, max_message);
    ethercat_pdo_listeners = &usr->ports;
    
    ENGINE * engine = new_engine(max_message);
    engine->path = strdup(path);
    engine->connect = client_connect;
    engine->on_connect = receive_config_on_connect;
    engine->send_message = ioc_send;
    engine->receive_message = ioc_receive;
    engine->usr = usr;
    engine_start(engine);

    new_timer(1000000000, usr->writeq, 0, MSG_HEARTBEAT);

    int ack;
    rtMessageQueueReceive(usr->config_ready, &ack, sizeof(int));

}

extern "C"
{
    /* EPICS iocsh shell commands */
    static const iocshArg InitArg[] = { { "path",       iocshArgString },
                                        { "maxmessage", iocshArgInt    } };
    static const iocshArg * const InitArgs[] = { &InitArg[0], &InitArg[1] };
    static const iocshFuncDef InitFuncDef = {"ecAsynInit", 2, InitArgs};
    static void InitCallFunc(const iocshArgBuf * args)
    {
        makePorts(args[0].sval, args[1].ival);
    }

    static const iocshArg SamplerArg[] = { { "port",       iocshArgString },
                                           { "channel",    iocshArgInt    },
                                           { "sample",     iocshArgString },
                                           { "cycle",      iocshArgString }};

    static const iocshArg * const SamplerArgs[] = { &SamplerArg[0], 
                                                    &SamplerArg[1], 
                                                    &SamplerArg[2], 
                                                    &SamplerArg[3] };
    
    static const iocshFuncDef SamplerFuncDef = {"ADC_Ethercat_Sampler", 4, SamplerArgs};
    static void SamplerCallFunc(const iocshArgBuf * args)
    {
        Configure_Sampler(args[0].sval, args[1].ival, args[2].sval, args[3].sval);
    }

    void ecAsynRegistrar(void)
    {
        ellInit(&sampler_configs);
        iocshRegister(&InitFuncDef,InitCallFunc);
        iocshRegister(&SamplerFuncDef,SamplerCallFunc);
    }
    epicsExportRegistrar(ecAsynRegistrar);
}
