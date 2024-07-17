#include "checkpoint/checkpoint.h"
#include "checkpoint/serializer_utils.h"
#include "cpu_bits.h"
#include "hw/boards.h"
#include "hw/riscv/nemu.h"
#include "qemu/error-report.h"
#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/typedefs.h"
#include <zlib.h>
#include <zstd.h>

__attribute_maybe_unused__
static int get_env_cpu_mode(void){
    CPUState *cs = qemu_get_cpu(0);
    CPURISCVState *env = cpu_env(cs);
    return env->priv;
}

static int64_t get_kernel_insns(NEMUState *ns, int cpu_idx){
    return ns->sync_info.kernel_insns[cpu_idx];
}

static bool instrsCouldTakeCpt(NEMUState *ns, uint64_t icount) {
    uint64_t limit_instructions = simpoint_get_next_instructions(ns);
    if (limit_instructions==0) {
//        error_printf("simpoint file or weight file error, get limit_instructions 0\n");
        return false;
    }
    limit_instructions += 100000;

    switch (ns->checkpoint_info.checkpoint_mode) {
    case SimpointCheckpointing:
        if (icount >= limit_instructions) {
            info_report("Should take cpt now: %lu limit_instructions: %lu", icount,limit_instructions);
            return true;
        } else if (icount % ns->checkpoint_info.cpt_interval == 0) {
            info_report("Next cpt @ %lu, now: %lu",
                        limit_instructions, icount);
            break;
        } else {
            break;
        }
    case UniformCheckpointing:
        if (icount >= ns->checkpoint_info.next_uniform_point) {
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

static void serialize(NEMUState *ns, uint64_t icount) {
    serializeRegs(0, ns->memory, &single_core_rvgcvh_default_memlayout, 1, 0);
    serialize_pmem(icount, false, NULL, 0);
}

static bool could_take_checkpoint(NEMUState *ns, uint64_t icount){
    if (ns->checkpoint_info.checkpoint_mode==NoCheckpoint) {
        return false;
    }
    if (!ns->checkpoint_info.workload_loaded) {
        return false;
    }
    if (!instrsCouldTakeCpt(ns, icount)) {
        return false;
    }
    return true;
}

static void update_cpt_limit_instructions(NEMUState *ns, uint64_t icount){
    info_report("Taking checkpoint @ instruction count %lu", icount);
    guint cpt_insns_list_length=g_list_length(ns->simpoint_info.cpt_instructions);
    if (cpt_insns_list_length!=0) {
        ns->simpoint_info.cpt_instructions=g_list_remove(ns->simpoint_info.cpt_instructions,g_list_first(ns->simpoint_info.cpt_instructions)->data);
        ns->path_manager.checkpoint_path_list=g_list_remove(ns->path_manager.checkpoint_path_list,g_list_first(ns->path_manager.checkpoint_path_list)->data);
    }
    info_report("left checkpoint numbers: %d",g_list_length(ns->simpoint_info.cpt_instructions));
}

void single_core_try_take_cpt(NEMUState* ns, uint64_t icount, int cpu_idx, bool exit_sync_period) {
    uint64_t workload_exec_insns = icount - get_kernel_insns(ns, 0);
    if (could_take_checkpoint(ns, workload_exec_insns)) {
        serialize(ns, workload_exec_insns);
        update_cpt_limit_instructions(ns, workload_exec_insns);
    }
}
