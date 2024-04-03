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

__attribute_maybe_unused__ static void serializeRegs(int cpu_index, char *buffer)  {
    CPUState *cs = qemu_get_cpu(cpu_index);
    RISCVCPU *cpu = RISCV_CPU(&cs->parent_obj);
    CPURISCVState *env = cpu_env(cs);
    uint64_t buffer_offset=0;

    buffer_offset=INT_REG_CPT_ADDR-BOOT_CODE;
    for(int i = 0 ; i < 32; i++) {
        memcpy(buffer+buffer_offset+i*8,&env->gpr[i],8);
        printf("gpr %04d value %016lx ",i,env->gpr[i]);
        if ((i+1)%4==0) {
            printf("\n");
        }
    }
    info_report("Writting int registers to checkpoint memory");

    // F extertion
    buffer_offset=FLOAT_REG_CPT_ADDR-BOOT_CODE;
    for(int i = 0 ; i < 32; i++) {
        memcpy(buffer+buffer_offset+i*8,&env->fpr[i],8);
    }
    info_report("Writting float registers to checkpoint memory");

    // V extertion
//    if(env->virt_enabled) {
    buffer_offset=VECTOR_REG_CPT_ADDR-BOOT_CODE;
    for(int i = 0; i < 32 * cpu->cfg.vlen / 64; i++) {
        memcpy(buffer+buffer_offset+i*8,&env->vreg[i],8);
        if ((i+1)%(2)==0) {
            info_report("[%lx]: 0x%016lx_%016lx",(uint64_t)VECTOR_REG_CPT_ADDR+(i-1)*8,env->vreg[i-1],env->vreg[i]);
        }
    }
    info_report("Writting 32 * %d vector registers to checkpoint memory\n",cpu->cfg.vlen /64);
//    }

    // CSR registers
    buffer_offset=CSR_REG_CPT_ADDR-BOOT_CODE;
    for(int i = 0; i < CSR_TABLE_SIZE; i++) {
        if(csr_ops[i].read != NULL) {
            target_ulong val;
            csr_ops[i].read(env, i, &val);
            memcpy(buffer+buffer_offset+i*8,&val,8);
            if (val!=0) {
                info_report("csr id %x name %s value %lx",i,csr_ops[i].name,val);
            }
        }
    }
    info_report("Writting csr registers to checkpoint memory");

    uint64_t tmp_mstatus=env->mstatus;
    tmp_mstatus=set_field(tmp_mstatus, MSTATUS_MPIE, get_field(tmp_mstatus, MSTATUS_MIE));
    tmp_mstatus=set_field(tmp_mstatus, MSTATUS_MIE, 0);
    tmp_mstatus=set_field(tmp_mstatus, MSTATUS_MPP, env->priv);
    buffer_offset = CSR_REG_CPT_ADDR - BOOT_CODE + 0x300 * 8;
    memcpy(buffer+buffer_offset,&tmp_mstatus,8);
    info_report("Writting mstatus registers to checkpoint memory: %lx mpp %lx",tmp_mstatus,env->priv);

    uint64_t tmp_mideleg=env->mideleg;
    buffer_offset = CSR_REG_CPT_ADDR - BOOT_CODE + 0x303 * 8;
    memcpy(buffer+buffer_offset, &tmp_mideleg, 8);
    info_report("Writting mideleg registers to screen: %lx",tmp_mideleg);

    uint64_t tmp_mie = env->mie;
    buffer_offset = CSR_REG_CPT_ADDR - BOOT_CODE + 0x304 * 8;
    memcpy(buffer+buffer_offset, &tmp_mie, 8);
    info_report("Writting mie registers to screen: %lx",tmp_mie);

    // turn off mip
    uint64_t tmp_mip=env->mip;
//    tmp_mip=set_field(tmp_mip, MIP_MTIP, 0);
//    tmp_mip=set_field(tmp_mip, MIP_STIP, 0);
    buffer_offset = CSR_REG_CPT_ADDR-BOOT_CODE + 0x344 * 8;
    memcpy(buffer + buffer_offset,&tmp_mip,8);
    info_report("Writting mip registers to checkpoint memory: %lx",tmp_mip);

    uint64_t tmp_hideleg=env->hideleg;
    buffer_offset = CSR_REG_CPT_ADDR - BOOT_CODE + 0x603 * 8;
    memcpy(buffer+buffer_offset, &tmp_hideleg, 8);
    info_report("Writting hideleg registers to screen: %lx",tmp_hideleg);

    //uint64_t tmp_hie=env->hie 604;
    //uint64_t tmp_hip=env->hip 644;
    //uint64_t tmp_vsip=env->vsip 244;
    uint64_t tmp_hvip=env->hvip;
    buffer_offset = CSR_REG_CPT_ADDR - BOOT_CODE + 0x645 * 8;
    memcpy(buffer+buffer_offset, &tmp_hvip, 8);
    info_report("Writting hvip registers to screen: %lx",tmp_hvip);

    uint64_t tmp_vsie=env->vsie;
    buffer_offset = CSR_REG_CPT_ADDR - BOOT_CODE + 0x204 * 8;
    memcpy(buffer+buffer_offset, &tmp_vsie, 8);
    info_report("Writting vsie registers to screen: %lx",tmp_vsie);

    uint64_t tmp_satp=0;
    buffer_offset = CSR_REG_CPT_ADDR - BOOT_CODE + 0x180 * 8;
    tmp_satp=*(uint64_t*)(buffer+buffer_offset);
    info_report("Satp from env %lx, Satp from memory %lx",env->satp, tmp_satp);

    uint64_t flag_val;
    flag_val = CPT_MAGIC_BUMBER;
    buffer_offset = BOOT_FLAG_ADDR-BOOT_CODE;
    memcpy(buffer+buffer_offset,&flag_val,8);

    uint64_t tmp_mepc=env->pc;
    buffer_offset = CSR_REG_CPT_ADDR-BOOT_CODE+ 0x341 * 8;
    memcpy(buffer+buffer_offset,&tmp_mepc,8);
    info_report("Writting mepc registers to checkpoint memory: %lx",tmp_mepc);

    buffer_offset=PC_CPT_ADDR-BOOT_CODE;
    memcpy(buffer+buffer_offset,&env->pc,8);
    buffer_offset=MODE_CPT_ADDR-BOOT_CODE;
    memcpy(buffer+buffer_offset,&env->priv,8);

    uint64_t tmp_mtime_cmp;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIMECMP, &tmp_mtime_cmp, 8);
    buffer_offset=MTIME_CPT_ADDR-BOOT_CODE;
    memcpy(buffer+buffer_offset,&tmp_mtime_cmp,8);
    info_report("Writting mtime_cmp registers to checkpoint memory: %lx %x",tmp_mtime_cmp,CLINT_MMIO+CLINT_MTIMECMP);

    uint64_t tmp_mtime;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIME, &tmp_mtime, 8);
    buffer_offset=MTIME_CPT_ADDR-BOOT_CODE;
    memcpy(buffer+buffer_offset,&tmp_mtime,8);
    info_report("Writting mtime registers to checkpoint memory: %lx %x",tmp_mtime,CLINT_MMIO+CLINT_MTIME);

    uint64_t tmp_vstart;
    csr_ops[0x008].read(env, 0x008, &tmp_vstart);
    info_report("vstart registers check: env %lx csr read %lx",env->vstart,tmp_vstart);
    uint64_t tmp_vxsat;
    csr_ops[0x009].read(env, 0x009, &tmp_vxsat);
    info_report("vxsat registers check: env %lx csr read %lx",env->vxsat,tmp_vxsat);
    uint64_t tmp_vxrm;
    csr_ops[0x00a].read(env, 0x00a, &tmp_vxrm);
    info_report("vxrm registers check: csr read %lx",tmp_vxrm);
    uint64_t tmp_vcsr;
    csr_ops[0x00f].read(env, 0x00f, &tmp_vcsr);
    info_report("vcsr registers check: csr read %lx",tmp_vcsr);
    uint64_t tmp_vl;
    csr_ops[0xc20].read(env, 0xc20, &tmp_vl);
    info_report("vl registers check: env %lx csr read %lx",env->vl,tmp_vl);
    uint64_t tmp_vtype;
    csr_ops[0xc21].read(env, 0xc21, &tmp_vtype);
    info_report("vtype registers check: env %lx csr read %lx",env->vtype,tmp_vtype);
    uint64_t tmp_vlenb;
    csr_ops[0xc22].read(env, 0xc22, &tmp_vlenb);
    info_report("vlenb registers check: csr read %lx",tmp_vlenb);

}

__attribute_maybe_unused__ static void serialize(uint64_t memory_addr,int cpu_index, int cpus, uint64_t inst_count)  {
    MachineState *ms = MACHINE(qdev_get_machine());
    NEMUState *ns=NEMU_MACHINE(ms);
    char *buffer=ns->gcpt_memory;
    info_report("buffer addr %p\n",buffer);
    assert(buffer);

//    char *buffer=g_malloc(1 * 1024 * 1024 * cpus);
//    assert(buffer);

    for (int i = 0; i < cpus; i++) {
        serializeRegs(i, buffer + i * (1024 * 1024));
        info_report("buffer %d serialize success, start addr %lx\n", i, memory_addr + (i*1024*1024));
    }

    cpu_physical_memory_write(memory_addr, buffer, 1*1024*1024*cpus);
    serialize_pmem(inst_count);

//    g_free(buffer);
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

        printf("cpu: %ld get the broadcast, limit instructions: %ld\n",cpu_idx,limit_instructions());
        for (int i = 0; i<sync_info.cpus; i++) {
            printf("cpu %d, insns %ld\n",i,sync_info.workload_insns[i]);
        }

        if (checkpoint.checkpoint_mode==UniformCheckpointing) {
            update_uniform_limit_inst();
        }

        // reset self flag
        sync_info.checkpoint_end[cpu_idx]=false;

        g_cond_broadcast(&sync_signal);

        g_mutex_unlock(&sync_lock);
    }

    if (checkpoint.workload_exit&&all_cpu_exit()) {
        // exit;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
    }

    return false;
}

bool try_take_cpt(uint64_t inst_count, uint64_t cpu_idx){
    if (try_take_single_core_checkpoint) {
        return single_core_try_take_cpt(inst_count);
    }else {
        return multi_core_try_take_cpt(inst_count, cpu_idx);
    }
}
