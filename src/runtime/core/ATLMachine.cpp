/*===--------------------------------------------------------------------------
 *              ATMI (Asynchronous Task and Memory Interface)
 *
 * This file is distributed under the MIT License. See LICENSE.txt for details.
 *===------------------------------------------------------------------------*/
#include "ATLMachine.h"
#include <stdio.h>
#include <stdlib.h>
#include <hsa.h>
#include <hsa_ext_amd.h>
#include <vector>
#include <cassert>
#include "atmi_runtime.h"
#include "atl_internal.h"
using namespace std;
extern ATLMachine g_atl_machine;
extern hsa_region_t atl_cpu_kernarg_region;

void *ATLMemory::alloc(size_t sz) {
    void *ret;
    hsa_status_t err = hsa_amd_memory_pool_allocate(_memory_pool, sz, 0, &ret);
    ErrorCheck(Allocate from memory pool, err);
    return ret;
}

void ATLMemory::free(void *ptr) {
    hsa_status_t err = hsa_amd_memory_pool_free(ptr);
    ErrorCheck(Allocate from memory pool, err);
}

/*atmi_task_handle_t ATLMemory::copy(void *dest, void *ATLMemory &m, bool async) {
    atmi_task_handle_t ret_task;

    if(async) atmi_task_wait(ret_task);
    return ret_task;
}*/


void ATLProcessor::addMemory(ATLMemory &mem) {
    for(std::vector<ATLMemory>::iterator it = _memories.begin();
                it != _memories.end(); it++) {
        // if the memory already exists, then just return
        if(mem.getMemory().handle == it->getMemory().handle)
            return;
    }
    _memories.push_back(mem);
}

std::vector<ATLMemory> &ATLProcessor::getMemories() {
    return _memories;
}

template <> std::vector<ATLCPUProcessor> &ATLMachine::getProcessors() { 
    return _cpu_processors; 
}

template <> std::vector<ATLGPUProcessor> &ATLMachine::getProcessors() { 
    return _gpu_processors; 
}

template <> std::vector<ATLDSPProcessor> &ATLMachine::getProcessors() { 
    return _dsp_processors; 
}

hsa_amd_memory_pool_t get_memory_pool(ATLProcessor &proc, const int mem_id) {
    hsa_amd_memory_pool_t pool;
    vector<ATLMemory> &mems = proc.getMemories();
    assert(mems.size() && mem_id >=0 && mem_id < mems.size() && "Invalid memory pools for this processor");
    pool = mems[mem_id].getMemory(); 
    return pool;
}

template <> void ATLMachine::addProcessor(ATLCPUProcessor &p) {
    _cpu_processors.push_back(p);
}

template <> void ATLMachine::addProcessor(ATLGPUProcessor &p) {
    _gpu_processors.push_back(p);
}

template <> void ATLMachine::addProcessor(ATLDSPProcessor &p) {
    _dsp_processors.push_back(p);
}

void ATLGPUProcessor::createQueues(const int count) {
    hsa_status_t err;
    /* Query the maximum size of the queue.  */
    uint32_t queue_size = 0;
    err = hsa_agent_get_info(_agent, HSA_AGENT_INFO_QUEUE_MAX_SIZE, &queue_size);
    ErrorCheck(Querying the agent maximum queue size, err);
    /* printf("The maximum queue size is %u.\n", (unsigned int) queue_size);  */

    /* Create queues for each device. */
    int qid;
    for (qid = 0; qid < count; qid++){
        hsa_queue_t *this_Q;
        err=hsa_queue_create(_agent, queue_size, HSA_QUEUE_TYPE_MULTI, NULL, NULL, UINT32_MAX, UINT32_MAX, &this_Q);
        ErrorCheck(Creating the queue, err);
        err = hsa_amd_profiling_set_profiler_enabled(this_Q, 1); 
        ErrorCheck(Enabling profiling support, err);
        _queues.push_back(this_Q);
        DEBUG_PRINT("Queue[%d]: %p\n", qid, this_Q);
    }
}

agent_t *ATLCPUProcessor::getThreadAgent(const int index) {
    if(index < 0 || index >= _agents.size()) 
        DEBUG_PRINT("CPU Agent index out of bounds!\n");
    return _agents[index];
}

hsa_signal_t *ATLCPUProcessor::get_worker_sig(hsa_queue_t *q) {
    hsa_signal_t *ret = NULL;
    for(std::vector<agent_t *>::iterator it = _agents.begin(); it != _agents.end(); it++) {
        agent_t *agent = *it;
        if(agent->queue == q) {
            ret = &(agent->worker_sig);
            break;
        }
    }
    return ret;
}

void *agent_worker(void *agent_args);

void ATLCPUProcessor::createQueues(const int count) {
    hsa_status_t err;
    for(int qid = 0; qid < count; qid++) {
        agent_t *agent = new agent_t;
        agent->id = qid;
        // signal between the host thread and the CPU tasking queue thread
        err = hsa_signal_create(IDLE, 0, NULL, &(agent->worker_sig));
        ErrorCheck(Creating a HSA signal for agent dispatch worker threads, err);

        hsa_signal_t db_signal;
        err = hsa_signal_create(1, 0, NULL, &db_signal);
        ErrorCheck(Creating a HSA signal for agent dispatch db signal, err);

        hsa_queue_t *this_Q;
        const int capacity = 1024 * 1024;
        hsa_amd_memory_pool_t cpu_pool = get_memory_pool(*this, 0);
        // FIXME: How to convert hsa_amd_memory_pool_t to hsa_region_t? 
        // Using fine grained system memory REGION for now
        err = hsa_soft_queue_create(atl_cpu_kernarg_region, capacity, 
                HSA_QUEUE_TYPE_SINGLE,
                HSA_QUEUE_FEATURE_AGENT_DISPATCH, db_signal, &this_Q);
        ErrorCheck(Creating an agent queue, err);
        _queues.push_back(this_Q);
        agent->queue = this_Q;

        hsa_queue_t *q = this_Q;
        //err = hsa_ext_set_profiling( q, 1); 
        //check(Enabling CPU profiling support, err); 
        //profiling does not work for CPU queues
        /* FIXME: Looks like a HSA bug. The doorbell signal that we pass to the 
         * soft queue creation API never seems to be set. Workaround is to 
         * manually set it again like below.
         */
        q->doorbell_signal = db_signal;
        _agents.push_back(agent);
        int last_index = _agents.size() - 1;
        pthread_create(&(agent->thread), NULL, 
                    agent_worker, (void *)agent);
    }
}

void ATLDSPProcessor::createQueues(const int count) {
}

void ATLProcessor::destroyQueues() {
    hsa_status_t err;
    for(std::vector<hsa_queue_t *>::iterator it = _queues.begin(); it != _queues.end(); it++) {
        err = hsa_queue_destroy(*it);
        ErrorCheck(Destroying the queue, err);
    }
}

hsa_queue_t *ATLProcessor::getBestQueue(atmi_scheduler_t sched) {
    hsa_queue_t *ret = NULL;
    switch(sched) {
        case ATMI_SCHED_NONE:
            ret = getQueue(__atomic_load_n(&_next_best_queue_id, __ATOMIC_ACQUIRE) % _queues.size());
            break;
        case ATMI_SCHED_RR:
            ret = getQueue(__atomic_fetch_add(&_next_best_queue_id, 1, __ATOMIC_ACQ_REL) % _queues.size());
            break;
    }
    return ret;
}

hsa_queue_t *ATLProcessor::getQueue(const int index) {
    return _queues[index % _queues.size()];
}

int ATLProcessor::getNumCUs() const {
    hsa_status_t err;
    /* Query the number of compute units.  */
    uint32_t num_cus = 0;
    err = hsa_agent_get_info(_agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT, &num_cus);
    ErrorCheck(Querying the agent number of compute units, err);

    return num_cus;
}

int ATLProcessor::getWavefrontSize() const {
    hsa_status_t err;
    /* Query the number of compute units.  */
    uint32_t w_size = 0;
    err = hsa_agent_get_info(_agent, (hsa_agent_info_t)HSA_AGENT_INFO_WAVEFRONT_SIZE, &w_size);
    ErrorCheck(Querying the agent wavefront size, err);

    return w_size;
}

