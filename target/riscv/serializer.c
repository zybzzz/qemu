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
#include "checkpoint/checkpoint.h"
#include "qapi/qapi-commands-machine.h"
extern RISCVAclintMTimerState *my_riscv_mtimer;

// 反转字符串的辅助函数
static void reverse(char* str, int length) {
    int start = 0;
    int end = length - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

// 将整数转换为字符串的itoa函数
static char* itoa(uint64_t num, char* str, int base) {
    int i = 0;

    // 处理0作为特殊情况
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return str;
    }

    // 处理负数（仅适用于十进制）
    if (base == 10) {
    }

    // 转换整数为字符串
    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }

    // 不处理负数


    str[i] = '\0'; // 字符串终止

    // 反转字符串
    reverse(str, i);

    return str;
}

static int find_minlocation(void) {
    uint64_t min = -1, idx;
    for (int i = 0; i < SIMPOINT_IDX_MAX; i++){
        if(serializer.simpoints[i] && serializer.simpoints[i] < min){
            min = serializer.simpoints[i];
            idx = i;
        }
    }
    if(min != -1){
        return idx+1;
    }
    return 0;
}

static void serializeRegs(void){
    CPUState *cs = qemu_get_cpu(0);
    for(int i = 0 ; i < 32; i++){
        cpu_physical_memory_write(INT_REG_CPT_ADDR + i*8, &cs->env_ptr->gpr[i], 8);
    }
    // F extertion
    for(int i = 0 ; i < 32; i++){
        cpu_physical_memory_write(FLOAT_REG_CPT_ADDR + i*8, &cs->env_ptr->fpr[i], 8);
    }

    cpu_physical_memory_write(PC_CPT_ADDR, &cs->env_ptr->pc, 8);
    // V extertion
    if(cs->env_ptr->vill){
        for(int i = 0; i < 32 * RV_VLEN_MAX / 64; i++){
            cpu_physical_memory_write(VECTOR_REGS_CPT_ADDR + i*8, &cs->env_ptr->vreg[i], 8);
        }        
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
    uint64_t mtimecmp = riscv_aclint_mtimer_read(my_riscv_mtimer, my_riscv_mtimer->timecmp_base, 8);
    cpu_physical_memory_write(BOOT_FLAGS+16, &mtimecmp, 8);
    uint64_t mtime = riscv_aclint_mtimer_read(my_riscv_mtimer, my_riscv_mtimer->time_base, 8);
    cpu_physical_memory_write(BOOT_FLAGS+24, &mtime, 8);
    
}


static void serializePMem(uint64_t inst_count)
{
    uint32_t l;
    uint8_t buf[4096];
    MemoryInfo * info = qmp_query_memory_size_summary(NULL);
    int min_locat_idx = find_minlocation();
    uint64_t addr = 0x80000000;
    uint64_t size = info->base_memory;
    char filepath[200], str_num[100];
    if (checkpoint_state == SimpointCheckpointing) {
        strcpy(filepath, pathmanger.outputPath);
        strcat(filepath, "_");
        memset(str_num, 0, 100);
        strcat(filepath, itoa(serializer.simpoints[min_locat_idx-1], str_num, 10));
        strcat(filepath, "_");
        strcat(filepath, itoa(serializer.weights[min_locat_idx-1], str_num, 10));
        strcat(filepath, "_.gz");
        serializer.simpoints[min_locat_idx-1] = 0;
    } else {
        strcpy(filepath, pathmanger.outputPath);
        strcat(filepath, "_");
        strcat(filepath, itoa(inst_count, str_num, 10));
        strcat(filepath, "_.gz");
    }
    gzFile compressed_mem = gzopen(filepath, "wb");
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


static bool instrsCouldTakeCpt(uint64_t num_insts) {
    int min_locat_idx = find_minlocation();
    switch (checkpoint_state) {
        case SimpointCheckpointing:
        if (!min_locat_idx) {
            break;
        }else{
            uint64_t next_point = serializer.simpoints[min_locat_idx - 1] * serializer.intervalSize + 100000;
            if (num_insts >= next_point) {
            info_report("Should take cpt now: %lu", num_insts);
            return true;
            } else if (num_insts % serializer.intervalSize == 0) {
            info_report("First cpt @ %lu, now: %lu",
            next_point, num_insts);
            break;
            }else{
            break;
            }
        }
        case ManualOneShotCheckpointing:
        return true;
        case ManualUniformCheckpointing:
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
