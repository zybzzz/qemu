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




extern RISCVAclintMTimerState *my_riscv_mtimer;

static void serializeRegs(void){
    CPUState *cs = qemu_get_cpu(0);

    for(int i = 0 ; i < 32; i++){
        cpu_physical_memory_write(INT_REG_CPT_ADDR + i*8, &cs->env_ptr->gpr[i], 8);
    }
    info_report("Writting int registers to checkpoint memory");

    // F extertion
    for(int i = 0 ; i < 32; i++){
        cpu_physical_memory_write(FLOAT_REG_CPT_ADDR + i*8, &cs->env_ptr->fpr[i], 8);
    }
    info_report("Writting float registers to checkpoint memory");

    cpu_physical_memory_write(PC_CPT_ADDR, &cs->env_ptr->pc, 8);

    // V extertion
    if(cs->env_ptr->vill){
        for(int i = 0; i < 32 * RV_VLEN_MAX / 64; i++){
            cpu_physical_memory_write(VECTOR_REGS_CPT_ADDR + i*8, &cs->env_ptr->vreg[i], 8);
        }
        info_report("Writting vector registers to checkpoint memory");
    }

    // CSR registers
    for(int i = 0; i < CSR_TABLE_SIZE; i++){
        if(csr_ops[i].read != NULL){
            target_ulong val;
            csr_ops[i].read(cs->env_ptr, i, &val);
            cpu_physical_memory_write(CSR_CPT_ADDR + i * 8, &val, 8);
        }
    }

    uint64_t val;
    val = CPT_MAGIC_BUMBER;
    cpu_physical_memory_write(BOOT_FLAGS, &val, 8);

    uint64_t mstatus=cs->env_ptr->mstatus;
    set_field(mstatus, MSTATUS_MPIE,get_field(mstatus, MSTATUS_MIE));
    set_field(mstatus, MSTATUS_MIE,0);
    set_field(mstatus, MSTATUS_MPP,cs->env_ptr->priv);
    cpu_physical_memory_write(CSR_CPT_ADDR + 0x300 * 8, &val, 8);

    uint64_t mepc=cs->env_ptr->mepc;
    cpu_physical_memory_write(CSR_CPT_ADDR + 0x341 * 8, &val, 8);


    uint32_t mtime_cmp;
    uint64_t mtime_cmp_64;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIMECMP, &mtime_cmp, 4);
    mtime_cmp_64=(uint64_t)mtime_cmp;
//    uint64_t mtimecmp = riscv_aclint_mtimer_read(my_riscv_mtimer, my_riscv_mtimer->timecmp_base, 8);
    cpu_physical_memory_write(MTIME_CPT_ADDR, ()&mtime_cmp_64, 8);

    uint32_t mtime;
    uint64_t mtime_64;
    cpu_physical_memory_read(CLINT_MMIO+CLINT_MTIME, &mtime, 4);
    mtime_64=(uint64_t)mtime;
//    uint64_t mtime = riscv_aclint_mtimer_read(my_riscv_mtimer, my_riscv_mtimer->time_base, 8);
    cpu_physical_memory_write(MTIME_CMP_CPT_ADDR, &mtime_64, 8);
}


static void serializePMem(uint64_t inst_count)
{
    uint32_t l;
    uint8_t buf[4096];
    MemoryInfo * info = qmp_query_memory_size_summary(NULL);
    uint64_t addr = 0x80000000;
    uint64_t size = info->base_memory;
    gzFile compressed_mem=NULL;
    char *path=NULL;

    char filepath[200], str_num[100];
    if (checkpoint_state == SimpointCheckpointing) {
        path=((GString*)(g_list_first(path_manager.checkpoint_path_list)->data))->str;
        assert(g_mkdir_with_parents(dirname(path),0775)==0);
        compressed_mem=gzopen(path);
    } else {
        strcpy(filepath, pathmanger.output_path);
        assert(g_mkdir_with_parents(path_manager.output_path,0775)==0);
        strcat(filepath, "_");
        strcat(filepath, itoa(inst_count, str_num, 10));
        strcat(filepath, "_.gz");
        compressed_mem=gzopen(filepath);
    }

    if (!compressed_mem) {
        error_printf("filename %s can't open", filepath);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        cpu_physical_memory_read(addr, buf, l);
        if (gzwrite(compressed_mem, buf, (uint32_t) l) != l) {
            error_printf("qmp_gzpmemsave write error");
            goto exit;
        }
        addr += l;
        size -= l;
    }
exit:
    gzclose(compressed_mem);
}

uint64_t get_next_instructions(){
    GList* first_instr=g_list_first(serializer.cpt_instructions);
    uint64_t instrs=0;
    if (first_instr==NULL) {
        return instrs;
    }else{
        return GPOINTER_TO_GINT(g_list_first(serializer.cpt_instructions)->data);
    }
    return instrs*serializer.cpt_interval;
}

static bool instrsCouldTakeCpt(uint64_t num_insts) {
    uint64_t limit_instructions=get_next_instructions()+100000;
//    int min_locat_idx = find_minlocation();
    switch (checkpoint_state) {
        case SimpointCheckpointing:
        if (limit_instructions==0) {
            break;
        }else{
            if (num_insts >= limit_instructions) {
                info_report("Should take cpt now: %lu", num_insts);
                return true;
            } else if (num_insts % serializer.intervalSize == 0) {
                info_report("First cpt @ %lu, now: %lu",
                limit_instructions, num_insts);
                break;
            }else{
                break;
            }
        }
        case UniformCheckpointing:
        // if (num_insts >= nextUniformPoint) {
        //     info_report("Should take cpt now: %lu", num_insts);
        //     return true;
        // }
        break;
        case NoCheckpoint:
        break;
        default:
        break;
    }
    return false;
}
// void notify_taken(uint64_t i) {
//   info_report("Taking checkpoint @ instruction count %lu", i);
//   if (checkpoint_state == SimpointCheckpointing) {
//     info_report("simpoint2Weights size: %ld", simpoint2Weights.size());

//     if (!simpoint2Weights.empty()) {
//       simpoint2Weights.erase(simpoint2Weights.begin());
//     }

//     if (!simpoint2Weights.empty()) {
//         pathManager.setCheckpointingOutputDir();
//     } 

//   } else if (checkpoint_state==ManualUniformCheckpointing||checkpoint_state==UniformCheckpointing) {
//     nextUniformPoint += intervalSize;
//     pathManager.setCheckpointingOutputDir();
//   }
// }



static void serialize(uint64_t icount){
    serializeRegs();
    serializePMem(icount); 
}

bool try_take_cpt(uint64_t icount) {
  if (instrsCouldTakeCpt(icount)) {
    serialize(icount);
    // notify_taken(icount);
    info_report("return true");
    return true;
  }
  return false;
}
