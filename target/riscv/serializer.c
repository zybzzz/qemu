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

uint64_t simpoint_get_next_instructions(NEMUState *ns) {

    GList* first_insns_item=g_list_first(ns->simpoint_info.cpt_instructions);
    if (first_insns_item==NULL) {
        set_simpoint_checkpoint_exit();
        return 0;
    } else {
        if (first_insns_item->data==0) {
            ns->simpoint_info.cpt_instructions=g_list_remove(ns->simpoint_info.cpt_instructions,g_list_first(ns->simpoint_info.cpt_instructions)->data);
            ns->path_manager.checkpoint_path_list=g_list_remove(ns->path_manager.checkpoint_path_list,g_list_first(ns->path_manager.checkpoint_path_list)->data);
            return 0;
        }
        return GPOINTER_TO_UINT(first_insns_item->data) * ns->checkpoint_info.cpt_interval;
    }
}

__attribute_maybe_unused__
static int get_env_cpu_mode(void){
    CPUState *cs = qemu_get_cpu(0);
    CPURISCVState *env = cpu_env(cs);
    return env->priv;
}

static uint64_t get_kernel_insns(void){
    CPUState *cs = qemu_get_cpu(0);
    CPURISCVState *env = cpu_env(cs);
    return env->last_seen_insns;
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
        if (limit_instructions==0) {
            break;
        } else {
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

bool single_core_try_take_cpt(NEMUState *ns, uint64_t icount) {
    uint64_t workload_exec_insns = icount - get_kernel_insns();
    if (could_take_checkpoint(ns, workload_exec_insns)) {
        serialize(ns, workload_exec_insns);
        update_cpt_limit_instructions(ns, workload_exec_insns);
        return true;
    }
    return false;
}
