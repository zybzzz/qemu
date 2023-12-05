#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#define STRING_LEN 100
#define SIMPOINT_IDX_MAX 100
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
    ManualOneShotCheckpointing,
    ManualUniformCheckpointing,
};
enum ProfilingState{
    NoProfiling =0,
    SimpointProfiling,
};

typedef struct PathManager{
    GString *base_dir;
    GString *workload_name;
    GString *config_name;
    GString *output_path;

    GString *simpoint_path;

    GList *checkpoint_path_list;
}PathManager;

typedef struct Serializer{
    uint64_t cpt_interval;
    uint64_t nextUniformPoint;
    GList *cpt_instructions;
    GList *weights;
}Serializer;

extern int checkpoint_state;
extern Serializer serializer;
extern PathManager path_manager;

bool try_take_cpt(uint64_t icount);
#endif
