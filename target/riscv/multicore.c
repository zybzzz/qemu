#include "checkpoint/checkpoint.h"
#include "cpu_bits.h"
#include "exec/cpu-common.h"
#include "hw/qdev-core.h"
#include "hw/riscv/nemu.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "qemu/typedefs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <zlib.h>
#include <zstd.h>
#include "hw/boards.h"
#include "checkpoint/checkpoint.h"
#include "checkpoint/serializer_utils.h"
#include "sysemu/runstate.h"


GMutex sync_lock;
sync_info_t sync_info;
GCond sync_signal;

static bool try_take_single_core_checkpoint = false;
void multicore_checkpoint_init(void) {
    MachineState *ms = MACHINE(qdev_get_machine());
    int64_t cpus=ms->smp.cpus;

    g_mutex_init(&sync_lock);

    g_mutex_lock(&sync_lock);

    sync_info.cpus=cpus;
    if (cpus == 1) {
        try_take_single_core_checkpoint = true;
    }

    sync_info.workload_exit_percpu=g_malloc0(cpus*sizeof(uint8_t));
    sync_info.workload_loaded_percpu=g_malloc0(cpus*sizeof(uint8_t));
    sync_info.workload_insns=g_malloc0(cpus*sizeof(uint64_t));
    sync_info.checkpoint_end=g_malloc0(cpus*sizeof(bool));
    g_mutex_unlock(&sync_lock);

}

static uint64_t workload_insns(int cpu_index) {
    CPUState *cs=qemu_get_cpu(cpu_index);
    CPURISCVState *env = cpu_env(cs);
    return env->profiling_insns-env->kernel_insns;
}

// prepare merge single core checkpoint
static uint64_t get_next_instructions(void){
    return 0;
}

static uint64_t limit_instructions(void) {
    if (checkpoint.checkpoint_mode==UniformCheckpointing) {
        return checkpoint.next_uniform_point;
    }else {
        return get_next_instructions();
    }
}

static void update_uniform_limit_inst(void){
    checkpoint.next_uniform_point+=checkpoint.cpt_interval;
}

static int get_env_cpu_mode(uint64_t cpu_idx){
    CPUState *cs = qemu_get_cpu(cpu_idx);
    CPURISCVState *env = cpu_env(cs);
    return env->priv;
}

static bool could_take_checkpoint(uint64_t workload_exec_insns,uint64_t cpu_idx) {
    g_mutex_lock(&sync_lock);
    // single cpu check
    if (sync_info.workload_loaded_percpu[cpu_idx]!=0x1) {
        goto failed;
    }

    if (sync_info.workload_exit_percpu[cpu_idx]==0x1) {
        goto failed;
    }

    if (get_env_cpu_mode(cpu_idx)==PRV_M) {
        goto failed;
    }

    // all cpu check, do not wait for before workload
    for (int i = 0; i<sync_info.cpus; i++) {
        if (sync_info.workload_loaded_percpu[i]!=0x1) {
            goto failed;
        }
    }

    // when set limit instructions, hart must goto wait
    if (workload_exec_insns>=limit_instructions()) {
        sync_info.workload_insns[cpu_idx]=workload_exec_insns;
    } else {
        goto failed;
    }

    for (int i = 0; i<sync_info.cpus; i++) {
        // idx i hart less than limit instructions and workload not exit, this hart could wait
        if (sync_info.workload_insns[i]<limit_instructions()) {
            if (sync_info.workload_exit_percpu[i]!=0x1) {
                goto wait;
            }
        }
    }

    g_mutex_unlock(&sync_lock);
    // all hart get sync node
    return true;

wait:
    printf("cpu %ld get wait\n",cpu_idx);

    // wait for checkpoint thread set flag true
    while (!sync_info.checkpoint_end[cpu_idx]) {
        g_cond_wait(&sync_signal, &sync_lock);
    }

    // printf("cpu: %ld get the sync end, limit instructions: %ld\n",cpu_idx,workload_exec_insns);
    printf("cpu: %ld get the sync end\n",cpu_idx);

    //reset status
    sync_info.checkpoint_end[cpu_idx]=false;
    g_mutex_unlock(&sync_lock);

    return false;

failed:
    g_mutex_unlock(&sync_lock);
    return false;
}

__attribute_maybe_unused__ static void serialize(uint64_t memory_addr,int cpu_index, int cpus, uint64_t inst_count)  {
    MachineState *ms = MACHINE(qdev_get_machine());
    NEMUState *ns=NEMU_MACHINE(ms);
    checkpoint_header cpt_header = default_cpt_header;
    single_core_rvgc_rvv_rvh_memlayout cpt_percpu_layout = single_core_rvgcvh_default_memlayout;
    uint64_t serialize_reg_base_addr;
    bool using_gcpt_mmio = false;
    char *hardware_status_buffer = (char*)ns->gcpt_memory;

#ifdef USING_PROTOBUF


    serialize_reg_base_addr = (uint64_t)cpt_header.cpt_offset + (uint64_t)hardware_status_buffer;

    cpt_header_encode(hardware_status_buffer, &cpt_header, &cpt_percpu_layout);
#else
    serialize_reg_base_addr = (uint64_t)hardware_status_buffer;

#endif

    for (int i = 0; i < cpus; i++) {
        serializeRegs(i, (char*)(serialize_reg_base_addr + i * (1024 * 1024)), &cpt_percpu_layout, cpt_header.cpu_num, global_mtime);
        info_report("buffer %d serialize success, start addr %lx\n", i, serialize_reg_base_addr + (i*1024*1024));
    }

    if (!using_gcpt_mmio) {
        cpu_physical_memory_write(memory_addr, hardware_status_buffer, cpt_header.cpt_offset + 1024 * 1024 * cpus);
    }
    serialize_pmem(inst_count, false, hardware_status_buffer, cpt_header.cpt_offset + 1024 * 1024 *cpus);
}

static bool all_cpu_exit(void){
    g_mutex_lock(&sync_lock);
    int load_worload_num=0;
    int exit_worload_num=0;
    for (int i = 0; i<sync_info.cpus; i++) {
        if (sync_info.workload_loaded_percpu[i]==0x1) {
            load_worload_num++;
            // only loaded workload could set exit
            if (sync_info.workload_exit_percpu[i]==0x1) {
                exit_worload_num++;
            }
        }
    }

    g_mutex_unlock(&sync_lock);

    if (load_worload_num==exit_worload_num) {
        return true;
    }else {
        return false;
    }
}

bool multi_core_try_take_cpt(uint64_t icount, uint64_t cpu_idx) {
    if (checkpoint.checkpoint_mode==NoCheckpoint) {
        return false;
    }

    if (could_take_checkpoint(workload_insns(cpu_idx), cpu_idx)) {
        g_mutex_lock(&sync_lock);

        //start checkpoint
        serialize(0x80300000, cpu_idx, sync_info.cpus, workload_insns(cpu_idx));

        // checkpoint end, set all flags
        for (int i = 0; i < sync_info.cpus; i++) {
            sync_info.checkpoint_end[i] = true;
        }

        #ifndef NO_PRINT
        printf("cpu: %ld get the broadcast, limit instructions: %ld\n",cpu_idx,limit_instructions());
        #endif
        for (int i = 0; i<sync_info.cpus; i++) {
            #ifndef NO_PRINT
            printf("cpu %d, insns %ld\n",i,sync_info.workload_insns[i]);
            #endif
        }

        if (checkpoint.checkpoint_mode==UniformCheckpointing) {
            update_uniform_limit_inst();
        }

        // reset self flag
        sync_info.checkpoint_end[cpu_idx]=false;

        g_cond_broadcast(&sync_signal);

        g_mutex_unlock(&sync_lock);
    }

    if (checkpoint.workload_exit && all_cpu_exit()) {
        // exit;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
    }

    return false;
}

void try_set_mie(void *env){

    if (try_take_single_core_checkpoint) {
        ((CPURISCVState *)env)->mie=(((CPURISCVState *)env)->mie&(~(1<<7)));
        ((CPURISCVState *)env)->mie=(((CPURISCVState *)env)->mie&(~(1<<5)));
    }
}

bool try_take_cpt(uint64_t inst_count, uint64_t cpu_idx){
    if (try_take_single_core_checkpoint) {
        return single_core_try_take_cpt(inst_count);
    }else {
        return multi_core_try_take_cpt(inst_count, cpu_idx);
    }
}
