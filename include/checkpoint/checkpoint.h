#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#define STRING_LEN              100
#define SIMPOINT_IDX_MAX        100
#include "qemu/osdep.h"

#define CPT_MAGIC_BUMBER        0xbeef
#define BOOT_CODE               0x80000000

#define BOOT_FLAG_ADDR          0x800ECDB0
#define PC_CPT_ADDR             0x800ECDB8
#define MODE_CPT_ADDR           0x800ECDC0
#define MTIME_CPT_ADDR          0x800ECDC8
#define MTIME_CMP_CPT_ADDR      0x800ECDD0
#define MISC_DONE_CPT_ADDR      0x800ECDD8
#define MISC_RESERVE            0x800ECDE0

#define INT_REG_CPT_ADDR        0x800EDDE0
#define INT_REG_DONE            0x800EDEE0

#define FLOAT_REG_CPT_ADDR      0x800EDEE8
#define FLOAT_REG_DONE          0x800EDFE8

#define CSR_REG_CPT_ADDR        0x800EDFF0
#define CSR_REG_DONE            0x800F5FF0
#define CSR_RESERVE             0x800F5FF8

#define VECTOR_REG_CPT_ADDR     0x800FDFF8
#define VECTOR_REG_DONE         0x800FFFF8

#ifndef RESET_VECTOR
    #define RESET_VECTOR        0x80100000
#endif

#define CLINT_MMIO              0x38000000
#define CLINT_MTIMECMP          0x4000
#define CLINT_MTIME             0xBFF8


enum CheckpointState{
    NoCheckpoint=0,
    SimpointCheckpointing,
    UniformCheckpointing,
};

typedef struct PathManager{
    GString *base_dir;

    GString *workload_name;
    GString *config_name;
    GString *output_path;

    GString *simpoint_path;
    GString *uniform_path;
    GList *checkpoint_path_list;
}PathManager;

typedef struct SimpointInfo{
    GList *cpt_instructions;
    GList *weights;
}SimpointInfo;

typedef struct Checkpoint{
    uint64_t cpt_interval;
    uint64_t next_uniform_point;
    uint64_t checkpoint_mode;
    bool workload_loaded;
    bool workload_exit;
}Checkpoint;

typedef struct sync_info{
    uint8_t *workload_loaded_percpu;
    uint8_t *workload_exit_percpu;
    uint64_t *workload_insns;
    uint64_t cpus;
    bool *checkpoint_end;
}sync_info_t;

extern SimpointInfo simpoint_info;
extern PathManager path_manager;
extern Checkpoint checkpoint;

bool single_core_try_take_cpt(uint64_t icount);
bool multi_core_try_take_cpt(uint64_t icount,uint64_t cpu_idx);
bool try_take_cpt(uint64_t inst_count, uint64_t cpu_idx);
void multicore_checkpoint_init(void);
void checkpoint_gen_empty_callback(void);
void try_set_mie(void *env);
#endif
