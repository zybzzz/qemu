#include "checkpoint/checkpoint.h"
#include "cpu_bits.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "hw/qdev-core.h"
#include "hw/riscv/nemu.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "qemu/typedefs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <zlib.h>
#include <zstd.h>
#include "hw/boards.h"
#include "checkpoint/checkpoint.h"
#include "checkpoint/checkpoint.pb.h"
#include "checkpoint/serializer_utils.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/runstate.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

GMutex sync_lock;
GCond sync_signal;

#define NO_PRINT

static uint64_t global_mtime = 0;
bool simpoint_checkpoint_exit = false;

void set_simpoint_checkpoint_exit(void){
    simpoint_checkpoint_exit = true; 
}

__attribute__((unused)) static void set_global_mtime(void)
{ // maybe unused
    cpu_physical_memory_read(CLINT_MMIO + CLINT_MTIME, &global_mtime, 8);
}

NEMUState *local_nemu_state;
void multicore_checkpoint_init(MachineState *machine)
{
    MachineState *ms = machine;
    NEMUState *ns = NEMU_MACHINE(ms);
    int64_t cpus = ms->smp.cpus;
    local_nemu_state = ns;

    g_mutex_init(&sync_lock);

    g_mutex_lock(&sync_lock);

    ns->sync_info.cpus = cpus;
    if (cpus == 1) {
        ns->single_core_cpt = true;
    }
    
    ns->cs_vec = g_malloc0(sizeof(CPUState*)*cpus);
    
    for (int i = 0; i< cpus; i++) {
        ns->cs_vec[i] = qemu_get_cpu(i);
    }

    ns->sync_info.workload_loaded_percpu = g_malloc0(cpus * sizeof(uint8_t));
    ns->sync_info.workload_exit_percpu= g_malloc0(cpus * sizeof(uint8_t));
    ns->sync_info.workload_insns = g_malloc0(cpus * sizeof(uint64_t));
    ns->sync_info.early_exit = g_malloc0(cpus * sizeof(bool));
    ns->sync_info.checkpoint_end = g_malloc0(cpus * sizeof(bool));

    if (ns->checkpoint_info.checkpoint_mode == SyncUniformCheckpoint) {
        const char *detail_to_qemu_fifo_name = "./detail_to_qemu.fifo";
        ns->d2q_fifo = open(detail_to_qemu_fifo_name, O_RDONLY);

        const char *qemu_to_detail_fifo_name = "./qemu_to_detail.fifo";
        ns->q2d_fifo = open(qemu_to_detail_fifo_name, O_WRONLY);

        ns->sync_control_info.info_vaild_periods = 1;
        for (int i = 0; i < 8; i++) {
            ns->sync_control_info.u_arch_info.CPI[i] = 1;
        }
    }
    g_mutex_unlock(&sync_lock);
}

static inline uint64_t relative_kernel_exec_insns(CPUState *cs, int cpu_index){
    CPURISCVState *env = cpu_env(cs);
    return env->profiling_insns - env->kernel_insns;
}

static inline uint64_t relative_last_sync_insns(CPUState *cs, int cpu_index){
    CPURISCVState *env = cpu_env(cs);
    // simpoint checkpoint never update last_seen_insns when before_workload exec
    return env->profiling_insns - env->last_seen_insns;
}

static inline uint64_t cpt_limit_instructions(NEMUState *ns)
{
    if (ns->checkpoint_info.checkpoint_mode == SyncUniformCheckpoint) {
        return ns->checkpoint_info.next_uniform_point;
    } else if (ns->checkpoint_info.checkpoint_mode == UniformCheckpointing) {
        return ns->checkpoint_info.next_uniform_point;
    } else if (ns->checkpoint_info.checkpoint_mode == SimpointCheckpointing) {
        return simpoint_get_next_instructions(ns);
    }{
        return 0;
    }
}

static inline uint64_t sync_limit_instructions(NEMUState *ns, uint64_t cpu_idx)
{
    if (ns->checkpoint_info.checkpoint_mode == SyncUniformCheckpoint) {
        // sync uniform limit insns from detail model
        return (uint64_t)((double)ns->sync_info.sync_interval /
                      ns->sync_control_info.u_arch_info.CPI[cpu_idx]);
    }else if (ns->checkpoint_info.checkpoint_mode == UniformCheckpointing) {
        // sync last insns, so using cpt interval
        return ns->checkpoint_info.cpt_interval;
    }else if (ns->checkpoint_info.checkpoint_mode == SimpointCheckpointing){
        // simpoint checkpoint sync insns is next checkpoint insns
        return simpoint_get_next_instructions(ns);
    }else {
        return 0;
    }
}

__attribute_maybe_unused__ static int get_env_cpu_mode(uint64_t cpu_idx)
{
    CPUState *cs = qemu_get_cpu(cpu_idx);
    CPURISCVState *env = cpu_env(cs);
    return env->priv;
}



static bool sync_and_check_take_checkpoint(NEMUState *ns,
                                           uint64_t profiling_insns,
                                           uint64_t cpu_idx,
                                           bool *should_take_cpt,
                                           bool early_exit)
{
#ifndef NO_PRINT
    __attribute__((unused)) int r = rand() % 100000;
    if (r < 10) {
        fprintf(stderr, "%s: cpu %ld trying to obtain lock\n", __func__,
                cpu_idx);
    }
#endif
    g_mutex_lock(&sync_lock);

    // all cpu check, do not wait other core exec before workload
    int all_loaded_cpus = 0;
    for (int i = 0; i < ns->sync_info.cpus; i++) {
        all_loaded_cpus += ns->sync_info.workload_loaded_percpu[i];

    }

    if (all_loaded_cpus!=ns->sync_info.cpus) {
        goto failed;
    }

    // set halt flag manually
    if (early_exit) {
        CPUState *cs = ns->cs_vec[cpu_idx];
        cs->halted = 1;
        cs->exception_index = EXCP_HLT;
    }

    int wait_cpus = 0; // executed enough instructions or halt
    int online_cpus = 0; // not exited
    __attribute_maybe_unused__ int halt_cpus = 0; //  not exited but halt

    for (int i = 0; i < ns->sync_info.cpus; i++) {
        online_cpus += ns->sync_info.workload_exit_percpu[i] ^ 1;

        // count halt
        CPUState *cs = ns->cs_vec[i];
        bool halt = cs->halted == 1;
        halt_cpus += (ns->sync_info.workload_exit_percpu[i] ^ 1) & halt;
        wait_cpus += (ns->sync_info.workload_exit_percpu[i] ^ 1) & (halt || relative_last_sync_insns(ns->cs_vec[i], i) >= sync_limit_instructions(ns, i));
    }

    // if this cpu get early wait, could go on
    if (early_exit) {
        ns->sync_info.early_exit[cpu_idx] = true;
    // if not get early wait but get limit instructions, could go on
    } else if (relative_last_sync_insns(ns->cs_vec[cpu_idx], cpu_idx) >= sync_limit_instructions(ns, cpu_idx)) {
        ns->sync_info.workload_insns[cpu_idx] = relative_last_sync_insns(ns->cs_vec[cpu_idx], cpu_idx);
    // else must goto failed
    } else {
#ifndef NO_PRINT
        if (r < 10 || (wait_cpus == 1)) {
            CPUState *cs = ns->cs_vec[cpu_idx];
            CPURISCVState *env = cpu_env(cs);
            fprintf(
                stderr,
                "%s: cpu %ld: has not reached limit insns: %lu at pc: %#lx\n",
                __func__, cpu_idx, workload_insns(cpu_idx), env->pc);
        }
#endif
        goto failed;
    }

    if (wait_cpus < online_cpus) {
        goto wait;
    }

    uint64_t limit_instructions = cpt_limit_instructions(ns);
    if (limit_instructions == 0) {
        *should_take_cpt = 0;
    } else if(relative_kernel_exec_insns(ns->cs_vec[cpu_idx], cpu_idx) >= limit_instructions){
        info_report("should take cpt");
        *should_take_cpt = 1;
    } else {
        *should_take_cpt = 0;
    }

    g_mutex_unlock(&sync_lock);
    // all hart get sync node
    return true;

wait:
//#ifndef NO_PRINT
    fprintf(stderr,
            "cpu %ld get wait with insns: %lu, sync point: %lu, early exit "
            "this period: %i, online: %i, wait cpus: %i, halt cpus: %i\n",
            cpu_idx, relative_last_sync_insns(ns->cs_vec[cpu_idx], cpu_idx), sync_limit_instructions(ns, cpu_idx),
            ns->sync_info.early_exit[cpu_idx], online_cpus, wait_cpus, halt_cpus);
//#endif


    if (wait_cpus == 1) {
        cpu_disable_ticks();
        set_global_mtime();
    }

    // wait for checkpoint thread set flag true
    while (!ns->sync_info.checkpoint_end[cpu_idx]) {
        g_cond_wait(&sync_signal, &sync_lock);
    }

#ifndef NO_PRINT
    fprintf(stderr, "cpu: %ld get the sync end, core0: %lu, core1: %lu\n",
            cpu_idx, relative_last_sync_insns(0), relative_last_sync_insns(1));
#endif

    // reset status
    ns->sync_info.checkpoint_end[cpu_idx] = false;
    g_mutex_unlock(&sync_lock);

    return false;

failed:
    g_mutex_unlock(&sync_lock);
    return false;
}

__attribute_maybe_unused__ static void
serialize(uint64_t memory_addr, int cpu_index, int cpus, uint64_t inst_count)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    NEMUState *ns = NEMU_MACHINE(ms);
    checkpoint_header cpt_header = default_cpt_header;
    single_core_rvgc_rvv_rvh_memlayout cpt_percpu_layout =
        single_core_rvgcvh_default_memlayout;
    uint64_t serialize_reg_base_addr;
    bool using_gcpt_mmio = false;
    char *hardware_status_buffer = (char *)ns->gcpt_memory;

#ifdef USING_PROTOBUF


    serialize_reg_base_addr =
        (uint64_t)cpt_header.cpt_offset + (uint64_t)hardware_status_buffer;

    cpt_header_encode(hardware_status_buffer, &cpt_header, &cpt_percpu_layout);
#else
    serialize_reg_base_addr = (uint64_t)hardware_status_buffer;

#endif

    for (int i = 0; i < cpus; i++) {
        serializeRegs(i, (char *)(serialize_reg_base_addr + i * (1024 * 1024)),
                      &cpt_percpu_layout, cpt_header.cpu_num, global_mtime);
        info_report("buffer %d serialize success, start addr %lx\n", i,
                    serialize_reg_base_addr + (i * 1024 * 1024));
    }

    if (!using_gcpt_mmio) {
        cpu_physical_memory_write(memory_addr, hardware_status_buffer,
                                  cpt_header.cpt_offset + 1024 * 1024 * cpus);
    }
    serialize_pmem(inst_count, false, hardware_status_buffer,
                   cpt_header.cpt_offset + 1024 * 1024 * cpus);
}

static bool all_cpu_exit(NEMUState *ns, uint64_t cpu_idx)
{
#ifndef NO_PRINT
    fprintf(stderr, "%s: cpu %ld trying to obtain lock\n", __func__, cpu_idx);
#endif
    g_mutex_lock(&sync_lock);

    int load_worload_num = 0;
    int exit_worload_num = 0;

    for (int i = 0; i < ns->sync_info.cpus; i++) {
        if (ns->sync_info.workload_loaded_percpu[i] == 0x1) {
            load_worload_num++;
            // only loaded workload could set exit
            if (ns->sync_info.workload_exit_percpu[i] == 0x1) {
                exit_worload_num++;
            }
        }
    }

    g_mutex_unlock(&sync_lock);

    if (load_worload_num == exit_worload_num) {
        return true;
    } else {
        return false;
    }
}

static inline void send_fifo(NEMUState *ns){
    ns->q2d_buf.cpt_ready = true;
    // q2d_buf.cpt_id = 0;
    // q2d_buf.total_inst_count = checkpoint.next_uniform_point;
    write(ns->q2d_fifo, &ns->q2d_buf, sizeof(Qemu2Detail));
}

static inline void read_fifo(NEMUState *ns){
    ns->sync_control_info.info_vaild_periods -= 1;
    if (ns->sync_control_info.info_vaild_periods <= 0) {
        read(ns->d2q_fifo, &ns->sync_control_info.u_arch_info,
             sizeof(Detail2Qemu));
    }
}

static void update_last_seen_insns(NEMUState *ns){
    switch (ns->checkpoint_info.checkpoint_mode) {
        case UniformCheckpointing:
            for (int i = 0; i < ns->sync_info.cpus; i++) {
                CPUState *cs = qemu_get_cpu(i);
                CPURISCVState *env = cpu_env(cs);
                env->last_seen_insns = env->profiling_insns;
            }
            break;
        case SyncUniformCheckpoint:
            for (int i = 0; i < ns->sync_info.cpus; i++) {
                CPUState *cs = qemu_get_cpu(i);
                CPURISCVState *env = cpu_env(cs);
                env->last_seen_insns = env->profiling_insns;
            }
            break;
        case SimpointCheckpointing:
            break;
        default:
            break;
    }
}

bool multi_core_try_take_cpt(NEMUState* ns, uint64_t icount, uint64_t cpu_idx,
                             bool exit_sync_period)
{
    bool should_take_cpt = false;
    if (sync_and_check_take_checkpoint(ns, icount, cpu_idx,
                                       &should_take_cpt, exit_sync_period)) {
#ifndef NO_PRINT
        fprintf(stderr, "%s: cpu %ld finished sync lastly, exit period: %i\n",
                __func__, cpu_idx, exit_sync_period);
#endif
        g_mutex_lock(&sync_lock);

        // start checkpoint
        if (should_take_cpt) {
            serialize(0x80300000, cpu_idx, ns->sync_info.cpus,
                      relative_kernel_exec_insns(ns->cs_vec[cpu_idx], cpu_idx));
        }

        // checkpoint end, set all flags
        for (int i = 0; i < ns->sync_info.cpus; i++) {
            ns->sync_info.checkpoint_end[i] = true;
        }

//#ifndef NO_PRINT
        fprintf(stderr, "cpu: %ld get the broadcast, core0: %lu, core1: %lu checkpoint limit %lu relative kernel exec insn %lu\n",
                cpu_idx, relative_last_sync_insns(ns->cs_vec[0], 0), relative_last_sync_insns(ns->cs_vec[1], 1), cpt_limit_instructions(ns), relative_kernel_exec_insns(ns->cs_vec[cpu_idx], cpu_idx));
//#endif
        for (int i = 0; i < ns->sync_info.cpus; i++) {
#ifndef NO_PRINT
            fprintf(stderr, "cpu %d, insns %ld\n", i, workload_insns(i));
#endif
        }

        if (should_take_cpt) {
            if (ns->checkpoint_info.checkpoint_mode == SyncUniformCheckpoint) {
                // send fifo message
                ns->q2d_buf.cpt_id = cpt_limit_instructions(ns);
                ns->q2d_buf.total_inst_count = relative_kernel_exec_insns(ns->cs_vec[cpu_idx], cpu_idx);
                send_fifo(ns);
                // when period <= 0, next cpi will update
                read_fifo(ns);
            }
            // update checkpoint limit instructions
            update_cpt_limit_instructions(ns, icount);
        }

        update_last_seen_insns(ns);

        // reset status
        memset(ns->sync_info.early_exit, 0, ns->sync_info.cpus * sizeof(bool));

        // reset self flag
        ns->sync_info.checkpoint_end[cpu_idx] = false;

        cpu_enable_ticks();

        g_cond_broadcast(&sync_signal);

        g_mutex_unlock(&sync_lock);
    }

    if ((ns->checkpoint_info.workload_exit && all_cpu_exit(ns, cpu_idx)) || simpoint_checkpoint_exit) {
        // exit;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
    }

    return false;
}

void try_set_mie(void *env, NEMUState *ns)
{
    if (ns->single_core_cpt) {
        ((CPURISCVState *)env)->mie =
            (((CPURISCVState *)env)->mie & (~(1 << 7)));
        ((CPURISCVState *)env)->mie =
            (((CPURISCVState *)env)->mie & (~(1 << 5)));
        info_report("Notify: disable timmer interrupr");
    }
}

bool try_take_cpt(NEMUState *ns, uint64_t inst_count, uint64_t cpu_idx, bool exit_sync_period)
{
    if (ns->checkpoint_info.checkpoint_mode == NoCheckpoint) {
        return false;
    }

    if (ns->single_core_cpt) {
        return single_core_try_take_cpt(ns, inst_count);
    } else {
        return multi_core_try_take_cpt(ns, inst_count, cpu_idx, exit_sync_period);
    }

}
