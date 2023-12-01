#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#define STRING_LEN 100
#define SIMPOINT_IDX_MAX 100
#include "qemu/osdep.h"

#define CPT_MAGIC_BUMBER       0xbeef
#define BOOT_CODE              0x80000000
#define BOOT_FLAGS             0x80000f00
#define INT_REG_CPT_ADDR       0x80001000
#define FLOAT_REG_CPT_ADDR     0x80001100
#define VECTOR_REGS_CPT_ADDR   0x80001200
#define PC_CPT_ADDR            0x80002200//1400
#define CSR_CPT_ADDR           0x80002300

#ifndef RESET_VECTOR
#define RESET_VECTOR        0x80200000
#endif

#define CLINT_MMIO 0x38000000
#define CLINT_MTIMECMP 0x4000
#define CLINT_MTIME 0xBFF8


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

struct PathManger{
    char statsBaseDir[STRING_LEN];
    char configName[STRING_LEN];
    char workloadName[STRING_LEN];
    int cptID;
    char workloadPath[STRING_LEN];
    char outputPath[STRING_LEN];
    char simpointPath[STRING_LEN];
};

struct Serializer{
    uint64_t intervalSize;
    uint64_t nextUniformPoint;
    uint64_t simpoints[SIMPOINT_IDX_MAX];
    double weights[SIMPOINT_IDX_MAX];
};

typedef struct PathManger PathManger;
typedef struct Serializer Serializer;

extern int checkpoint_state;
extern Serializer serializer;
extern PathManger pathmanger;

bool try_take_cpt(uint64_t icount);
#endif
