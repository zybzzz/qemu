#include "checkpoint/checkpoint.h"
#include "cpu_bits.h"
#include "hw/qdev-core.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "cpu.h"
#include "qemu/typedefs.h"
#include "tcg/tcg-op.h"
#include "disas/disas.h"
#include "exec/cpu_ldst.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"

#include "exec/translator.h"
#include "exec/log.h"
#include "semihosting/semihost.h"
#include "instmap.h"
#include <stdio.h>
#include <zlib.h>
#include "internals.h"
#include "hw/intc/riscv_aclint.h"
#include "qapi/qapi-commands-machine.h"


#include "qemu/osdep.h"
#include "hw/boards.h"
#include "qapi/error.h"

#include "checkpoint/checkpoint.h"
typedef struct sync_info{
    uint8_t *workload_loaded_percpu;
    uint64_t *workload_insns;
    uint64_t cpus;
    bool *checkpoint_end;
    bool leader;
}sync_info_t;

GMutex sync_lock;
sync_info_t sync_info;
GCond sync_signal;

void multicore_checkpoint_init(void){
    MachineState *ms = MACHINE(qdev_get_machine());
    int64_t cpus=ms->smp.cpus;

    g_mutex_init(&sync_lock);

    g_mutex_lock(&sync_lock);
    sync_info.cpus=cpus;
    sync_info.workload_loaded_percpu=g_malloc0(cpus*sizeof(uint8_t));
    sync_info.workload_insns=g_malloc0(cpus*sizeof(uint64_t));
    sync_info.checkpoint_end=g_malloc0(cpus*sizeof(bool));
    sync_info.leader=false;
    g_mutex_unlock(&sync_lock);


}

//static void ncpu_load_workload(uint64_t cpu_idx){
//
//    g_mutex_lock(&sync_lock);
//    sync_info.workload_loaded_percpu[cpu_idx]=1;
//    g_mutex_unlock(&sync_lock);
//
//}

static uint64_t limit=610000000;
static uint64_t limit_instructions(void){
    return limit;
}

static bool could_take_checkpoint(uint64_t workload_exec_insns,uint64_t cpu_idx){
    g_mutex_lock(&sync_lock);
    // single cpu check
//    if (sync_info.workload_loaded_percpu[cpu_idx]!=0x1) {
//        goto failed;
//    }
//
    if (workload_exec_insns>=limit_instructions()) {
        sync_info.workload_insns[cpu_idx]=workload_exec_insns;
    }else{
        goto failed;
    }

    // all cpu check
//    for (int i = 0; i<sync_info.cpus; i++) {
//        if (sync_info.workload_loaded_percpu[i]!=0x1) {
//            goto wait;
//        }
//    }

    for (int i = 0; i<sync_info.cpus; i++) {
        if (sync_info.workload_insns[i]<limit_instructions()) {
            goto wait;
        }
    }

    if (sync_info.leader==true) {
        goto wait;
    }else{
        sync_info.leader=true;
    }

    g_mutex_unlock(&sync_lock);
    return true;


wait:
    printf("cpu %ld get wait\n",cpu_idx);
    for (int i = 0; i<sync_info.cpus; i++) {
        printf("cpu %d, insns %ld\n",i,sync_info.workload_insns[i]);
    }

    while (!sync_info.checkpoint_end[cpu_idx]) {
        g_cond_wait(&sync_signal, &sync_lock);
    }
    printf("cpu: %ld get the sync end, limit instructions: %ld\n",cpu_idx,workload_exec_insns);
    sync_info.checkpoint_end[cpu_idx]=false;
    g_mutex_unlock(&sync_lock);

    return false;

failed:
    g_mutex_unlock(&sync_lock);
    return false;
}

bool multi_core_try_take_cpt(uint64_t icount,uint64_t cpu_idx){
    if (could_take_checkpoint(icount, cpu_idx)) {
        g_mutex_lock(&sync_lock);

        for (int i = 0; i<sync_info.cpus; i++) {
            sync_info.checkpoint_end[i]=true;
        }
        printf("cpu: %ld get the broadcast, limit instructions: %ld\n",cpu_idx,icount);

        limit+=20000000;
        sync_info.checkpoint_end[cpu_idx]=false;
        sync_info.leader=false;

        g_cond_broadcast(&sync_signal);

        g_mutex_unlock(&sync_lock);
    }

//    uint64_t workload_exec_insns=icount-get_kernel_insns();
//    if (could_take_checkpoint(workload_exec_insns)) {
//        serialize(workload_exec_insns);
//        notify_taken(workload_exec_insns);
//        return true;
//    }
//    return false;

    return false;
}
