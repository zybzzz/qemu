#include "checkpoint/checkpoint.h"
#include "cpu_bits.h"
#include "qemu/error-report.h"
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
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
#include <zlib.h>
#include "internals.h"
#include "hw/intc/riscv_aclint.h"
#include "qapi/qapi-commands-machine.h"

static void serializeRegs(void) {
    CPUState *cs = qemu_get_cpu(0);

    for(int i = 0 ; i < 32; i++) {
        cpu_physical_memory_write(INT_REG_CPT_ADDR + i*8, &cs->env_ptr->gpr[i], 8);
    }
    info_report("Writting int registers to checkpoint memory");

    // F extertion
    for(int i = 0 ; i < 32; i++) {
        cpu_physical_memory_write(FLOAT_REG_CPT_ADDR + i*8, &cs->env_ptr->fpr[i], 8);
    }
    info_report("Writting float registers to checkpoint memory");

    cpu_physical_memory_write(PC_CPT_ADDR, &cs->env_ptr->pc, 8);

    // V extertion
    if(cs->env_ptr->vill) {
        for(int i = 0; i < 32 * RV_VLEN_MAX / 64; i++) {
            cpu_physical_memory_write(VECTOR_REG_CPT_ADDR + i*8, &cs->env_ptr->vreg[i], 8);
        }
        info_report("Writting vector registers to checkpoint memory");
    }

    // CSR registers
    for(int i = 0; i < CSR_TABLE_SIZE; i++) {
        if(csr_ops[i].read != NULL) {
            target_ulong val;
            csr_ops[i].read(cs->env_ptr, i, &val);
            cpu_physical_memory_write(CSR_REG_CPT_ADDR + i * 8, &val, 8);
        }
    }
    info_report("Writting csr registers to checkpoint memory");

    uint64_t val;
    val = CPT_MAGIC_BUMBER;
    cpu_physical_memory_write(BOOT_FLAG_ADDR, &val, 8);

    uint64_t mstatus=cs->env_ptr->mstatus;
    uint64_t mstatus_mie=get_field(mstatus, MSTATUS_MIE);
    mstatus=set_field(mstatus, MSTATUS_MPIE,mstatus_mie);
    mstatus=set_field(mstatus, MSTATUS_MIE,0);
    mstatus=set_field(mstatus, MSTATUS_MPP,cs->env_ptr->priv);
    cpu_physical_memory_write(CSR_REG_CPT_ADDR + 0x300 * 8, &mstatus, 8);
    info_report("Writting mstatus registers to checkpoint memory: %lx",mstatus);

    uint64_t mepc=cs->env_ptr->mepc;
    cpu_physical_memory_write(CSR_REG_CPT_ADDR + 0x341 * 8, &mepc, 8);
    info_report("Writting mepc registers to checkpoint memory: %lx",mepc);


    uint64_t mtime_cmp;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIMECMP, &mtime_cmp, 8);
    cpu_physical_memory_write(MTIME_CPT_ADDR, &mtime_cmp, 8);
    info_report("Writting mtime_cmp registers to checkpoint memory: %lx %x",mtime_cmp,CLINT_MMIO+CLINT_MTIMECMP);

    uint64_t mtime;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIME, &mtime, 8);
    cpu_physical_memory_write(MTIME_CMP_CPT_ADDR, &mtime, 8);
    info_report("Writting mtime registers to checkpoint memory: %lx %x",mtime,CLINT_MMIO+CLINT_MTIME);
}


static void serialize_pmem(uint64_t inst_count)
{
#define MEM_READ_BUF_SIZE 40960
#define FILEPATH_BUF_SIZE 256
    MemoryInfo * guest_pmem_info = qmp_query_memory_size_summary(NULL);
    uint64_t guest_pmem_size = guest_pmem_info->base_memory;

    uint8_t serialize_mem_buf[MEM_READ_BUF_SIZE];
    char filepath[FILEPATH_BUF_SIZE];

    uint64_t guest_pmem_start_addr = 0x80000000;
    gzFile compressed_mem=NULL;


    //prepare path
    if (checkpoint.checkpoint_mode == SimpointCheckpointing) {
        strcpy(filepath,((GString*)(g_list_first(path_manager.checkpoint_path_list)->data))->str);
        g_mkdir_with_parents(g_path_get_dirname(filepath),0775);
        compressed_mem=gzopen(filepath,"wb");
    }

    // else {
    //     strcpy(filepath, path_manager.output_path->str);
    //     assert(g_mkdir_with_parents(path_manager.output_path,0775)==0);
    //     strcat(filepath, "_");
    //     strcat(filepath, itoa(inst_count, str_num, 10));
    //     strcat(filepath, "_.gz");
    //     compressed_mem=gzopen(filepath);
    // }

    if (!compressed_mem) {
        error_printf("filename %s can't open", filepath);
        return;
    }

    for (int i = 0; i<guest_pmem_size/MEM_READ_BUF_SIZE; i++ ) {
        cpu_physical_memory_read(guest_pmem_start_addr + i * MEM_READ_BUF_SIZE, serialize_mem_buf, MEM_READ_BUF_SIZE);
        if (gzwrite(compressed_mem, serialize_mem_buf, (uint32_t) MEM_READ_BUF_SIZE) != MEM_READ_BUF_SIZE) {
            error_printf("qmp_gzpmemsave write error");
            goto exit;
        }
    }

exit:
    gzclose(compressed_mem);
    info_report("success write into checkpoint file: %s",filepath);
}

static uint64_t get_next_instructions(void) {
    GList* first_insns_item=g_list_first(simpoint_info.cpt_instructions);
    uint64_t insns=0;
    if (first_insns_item==NULL) {
        return insns;
    } else {
        return GPOINTER_TO_UINT(first_insns_item->data) * checkpoint.cpt_interval;
    }
}

static bool instrsCouldTakeCpt(uint64_t icount) {
    uint64_t limit_instructions = get_next_instructions();
    if (limit_instructions==0) {
        error_printf("simpoint file or weight file error, get limit_instructions 0\n");
        //panic
    }
    limit_instructions += 100000;

    switch (checkpoint.checkpoint_mode) {
    case SimpointCheckpointing:
        if (limit_instructions==0) {
            break;
        } else {
            if (icount >= limit_instructions) {
                info_report("Should take cpt now: %lu", icount);
                return true;
            } else if (icount % checkpoint.cpt_interval == 0) {
                info_report("First cpt @ %lu, now: %lu",
                            limit_instructions, icount);
                break;
            } else {
                break;
            }
        }
    case UniformCheckpointing:
        if (icount >= checkpoint.next_uniform_point) {
            info_report("Should take cpt now: %lu", icount);
            return true;
        }
        break;
    case NoCheckpoint:
        break;
    default:
        break;
    }
    return false;
}

static void notify_taken(uint64_t icount) {
    info_report("Taking checkpoint @ instruction count %lu", icount);
    guint cpt_insns_list_length=g_list_length(simpoint_info.cpt_instructions);
    switch (checkpoint.checkpoint_mode) {
        case SimpointCheckpointing:

            if (cpt_insns_list_length!=0) {
                simpoint_info.cpt_instructions=g_list_remove(simpoint_info.cpt_instructions,g_list_first(simpoint_info.cpt_instructions)->data);
                path_manager.checkpoint_path_list=g_list_remove(path_manager.checkpoint_path_list,g_list_first(path_manager.checkpoint_path_list)->data);
            }

            info_report("left checkpoint numbers: %d",g_list_length(simpoint_info.cpt_instructions));
            break;
        case UniformCheckpointing:
            checkpoint.next_uniform_point += checkpoint.cpt_interval;
            break;
        default:
            break;
    }
}

static void serialize(uint64_t icount) {
    serializeRegs();
    serialize_pmem(icount);
}

bool try_take_cpt(uint64_t icount) {
    if (checkpoint.workload_loaded&&checkpoint.checkpoint_mode!=NoCheckpoint&&instrsCouldTakeCpt(icount)) {
        serialize(icount);
        notify_taken(icount);
        return true;
    }
    return false;
}
