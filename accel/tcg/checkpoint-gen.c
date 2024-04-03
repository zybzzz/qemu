#include "qemu/osdep.h"
#include "cpu.h"
#include "target/riscv/cpu.h"
#include "tcg/tcg.h"
#include "tcg/tcg-temp-internal.h"
#include "tcg/tcg-op.h"
#include "exec/exec-all.h"
#include "exec/translator.h"
#include "checkpoint/checkpoint.h"
#include "exec/helper-proto-common.h"
#include <stdio.h>

#define HELPER_H "accel/tcg/checkpoint-helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static void checkpoint_gen_empty_check_cb(void)
{
    TCGv_i32 cpu_index = tcg_temp_ebb_new_i32();
    TCGv_ptr udata = tcg_temp_ebb_new_ptr();

    tcg_gen_movi_ptr(udata, 0);
    tcg_gen_ld_i32(cpu_index, tcg_env,
                   -offsetof(ArchCPU, env) + offsetof(CPUState, cpu_index));

    gen_helper_checkpoint_sync_check(cpu_index,udata);

    tcg_temp_free_ptr(udata);
    tcg_temp_free_i32(cpu_index);
}

void checkpoint_gen_empty_callback(void){
    checkpoint_gen_empty_check_cb();
}

// static int temp_index=0;
void helper_checkpoint_sync_check(uint32_t cpu_index, void *udata) {
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPURISCVState *env = cpu_env(cs);
    try_take_cpt(env->profiling_insns, cpu_index);
}
