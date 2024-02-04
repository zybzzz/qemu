#include "checkpoint/checkpoint.h"
#include "cpu_bits.h"
#include "disas/dis-asm.h"
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
#include <stdint.h>
#include <stdio.h>
#include <sys/cdefs.h>
#include <zlib.h>
#include "internals.h"
#include "hw/intc/riscv_aclint.h"
#include "qapi/qapi-commands-machine.h"


#include "qemu/osdep.h"
#include "hw/boards.h"
#include "qapi/error.h"

#include "checkpoint/checkpoint.h"

GMutex sync_lock;
sync_info_t sync_info;
GCond sync_signal;

void multicore_checkpoint_init(void) {
    MachineState *ms = MACHINE(qdev_get_machine());
    int64_t cpus=ms->smp.cpus;

    g_mutex_init(&sync_lock);

    g_mutex_lock(&sync_lock);
    sync_info.cpus=cpus;
    sync_info.workload_loaded_percpu=g_malloc0(cpus*sizeof(uint8_t));
    sync_info.workload_insns=g_malloc0(cpus*sizeof(uint64_t));
    sync_info.checkpoint_end=g_malloc0(cpus*sizeof(bool));
    g_mutex_unlock(&sync_lock);


}

static uint64_t limit_interval=20000000;

static uint64_t workload_insns(int cpu_index) {
    CPUState *cs=qemu_get_cpu(cpu_index);
    CPURISCVState *env = cpu_env(cs);
    return env->profiling_insns-env->kernel_insns;
}

static uint64_t limit_instructions(void) {
    return limit_interval;
}


static bool could_take_checkpoint(uint64_t workload_exec_insns,uint64_t cpu_idx) {
    g_mutex_lock(&sync_lock);
    // single cpu check
    if (sync_info.workload_loaded_percpu[cpu_idx]!=0x1) {
        goto failed;
    }

    if (workload_exec_insns>=limit_instructions()) {
        sync_info.workload_insns[cpu_idx]=workload_exec_insns;
    } else {
//        sync_info.workload_insns[cpu_idx]=workload_exec_insns;
        goto failed;
    }

    // all cpu check, do not wait for before workload
    for (int i = 0; i<sync_info.cpus; i++) {
        if (sync_info.workload_loaded_percpu[i]!=0x1) {
            goto failed;
        }
    }

    for (int i = 0; i<sync_info.cpus; i++) {
        if (sync_info.workload_insns[i]<limit_instructions()) {
            goto wait;
        }
    }

    g_mutex_unlock(&sync_lock);
    return true;


wait:
    printf("cpu %ld get wait\n",cpu_idx);

    // wait for checkpoint thread set flag true
    while (!sync_info.checkpoint_end[cpu_idx]) {
        g_cond_wait(&sync_signal, &sync_lock);
    }
//    printf("cpu: %ld get the sync end, limit instructions: %ld\n",cpu_idx,workload_exec_insns);
    printf("cpu: %ld get the sync end\n",cpu_idx);

    //reset status
    sync_info.checkpoint_end[cpu_idx]=false;
    g_mutex_unlock(&sync_lock);

    return false;

failed:
    g_mutex_unlock(&sync_lock);
    return false;
}

__attribute_maybe_unused__ static void serializeRegs(int cpu_index,char *buffer)  {
    CPUState *cs = qemu_get_cpu(cpu_index);
    RISCVCPU *cpu = RISCV_CPU(&cs->parent_obj);
    CPURISCVState *env = cpu_env(cs);


    for(int i = 0 ; i < 32; i++) {
        memcpy(buffer+(INT_REG_CPT_ADDR-BOOT_CODE)+i*8,&env->gpr[i],8);
        printf("gpr %04d value %016lx ",i,env->gpr[i]);
        if ((i+1)%4==0) {
            printf("\n");
        }
    }
    info_report("Writting int registers to checkpoint memory");

    // F extertion
    for(int i = 0 ; i < 32; i++) {
        memcpy(buffer+(FLOAT_REG_CPT_ADDR-BOOT_CODE)+i*8,&env->fpr[i],8);
    }
    info_report("Writting float registers to checkpoint memory");


    // V extertion
//    if(env->virt_enabled) {
    for(int i = 0; i < 32 * cpu->cfg.vlen / 64; i++) {
        memcpy(buffer+(VECTOR_REG_CPT_ADDR-BOOT_CODE)+i*8,&env->vreg[i],8);
        if ((i+1)%(2)==0) {
            info_report("[%lx]: 0x%016lx_%016lx",(uint64_t)VECTOR_REG_CPT_ADDR+(i-1)*8,env->vreg[i-1],env->vreg[i]);
        }
    }
    info_report("Writting 32 * %d vector registers to checkpoint memory",cpu->cfg.vlen /64);
//    }

    // CSR registers
    for(int i = 0; i < CSR_TABLE_SIZE; i++) {
        if(csr_ops[i].read != NULL) {
            target_ulong val;
            csr_ops[i].read(env, i, &val);
            memcpy(buffer+(CSR_REG_CPT_ADDR-BOOT_CODE)+i*8,&val,8);
            if (val!=0) {
                info_report("csr id %x name %s value %lx",i,csr_ops[i].name,val);
            }
        }
    }
    info_report("Writting csr registers to checkpoint memory");
    uint64_t tmp_satp=0;
    cpu_physical_memory_read(CSR_REG_CPT_ADDR + 0x180 * 8, &tmp_satp, 8);
    info_report("Satp from env %lx, Satp from memory %lx",env->satp, tmp_satp);

    uint64_t flag_val;
    flag_val = CPT_MAGIC_BUMBER;

    memcpy(buffer+(BOOT_FLAG_ADDR-BOOT_CODE),&flag_val,8);

    uint64_t tmp_mip=env->mip;
    tmp_mip=set_field(tmp_mip, MIP_MTIP, 0);
    tmp_mip=set_field(tmp_mip, MIP_STIP, 0);

    memcpy(buffer+(CSR_REG_CPT_ADDR-BOOT_CODE)+ 0x344 * 8,&tmp_mip,8);
    info_report("Writting mip registers to checkpoint memory: %lx",tmp_mip);

    uint64_t tmp_mstatus=env->mstatus;
    tmp_mstatus=set_field(tmp_mstatus, MSTATUS_MPIE, get_field(tmp_mstatus, MSTATUS_MIE));
    tmp_mstatus=set_field(tmp_mstatus, MSTATUS_MIE, 0);
    tmp_mstatus=set_field(tmp_mstatus, MSTATUS_MPP, env->priv);
    memcpy(buffer+(CSR_REG_CPT_ADDR-BOOT_CODE)+ 0x300 * 8,&tmp_mstatus,8);
    info_report("Writting mstatus registers to checkpoint memory: %lx mpp %lx",tmp_mstatus,env->priv);

    uint64_t tmp_mepc=env->pc;
    memcpy(buffer+(CSR_REG_CPT_ADDR-BOOT_CODE)+ 0x341 * 8,&tmp_mepc,8);
    info_report("Writting mepc registers to checkpoint memory: %lx",tmp_mepc);

    memcpy(buffer+(PC_CPT_ADDR-BOOT_CODE),&env->pc,8);
    memcpy(buffer+(MODE_CPT_ADDR-BOOT_CODE),&env->priv,8);

    uint64_t tmp_mtime_cmp;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIMECMP, &tmp_mtime_cmp, 8);
    memcpy(buffer+(MTIME_CPT_ADDR-BOOT_CODE),&tmp_mtime_cmp,8);
    info_report("Writting mtime_cmp registers to checkpoint memory: %lx %x",tmp_mtime_cmp,CLINT_MMIO+CLINT_MTIMECMP);

    uint64_t tmp_mtime;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIME, &tmp_mtime, 8);
    memcpy(buffer+(MTIME_CPT_ADDR-BOOT_CODE),&tmp_mtime,8);
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

__attribute_maybe_unused__ static void serialize(int cpu_index)  {
// regbuffer=g_malloc_n(1M,cpus);
//    for i in cpus:
//        char * buffer=malloc(1000000);
//        serializeRegs(i,buffer);
//    *((uint64_t*)buffer)="magic";

}


//bool could_take_cpt(uint64_t cpu_idx){
//    if (checkpoint.checkpoint_mode==NoCheckpoint) {
//        return false;
//    }
//    if (get_env_cpu_mode()==PRV_M) {
//        return false;
//    }
//    if (!could_take_checkpoint(workload_insns(cpu_idx), cpu_idx)) {
//        return false;
//    }
//    return true;
//}

bool multi_core_try_take_cpt(uint64_t icount,uint64_t cpu_idx) {

    if (could_take_checkpoint(workload_insns(cpu_idx), cpu_idx)) {
        g_mutex_lock(&sync_lock);

        //start checkpoint
        int temp_i=2000000000;
        while (temp_i) {
            temp_i--;
        }

        // checkpoint end, set all flags
        for (int i = 0; i<sync_info.cpus; i++) {
            sync_info.checkpoint_end[i]=true;
        }

        printf("cpu: %ld get the broadcast, limit instructions: %ld\n",cpu_idx,limit_instructions());
        for (int i = 0; i<sync_info.cpus; i++) {
            printf("cpu %d, insns %ld\n",i,sync_info.workload_insns[i]);
        }

        limit_interval+=20000000;
        // reset self flag
        sync_info.checkpoint_end[cpu_idx]=false;

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




