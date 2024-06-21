#ifndef HW_RISCV_NEMU_H
#define HW_RISCV_NEMU_H

#include "hw/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"
#include "stdint.h"

#define NEMU_CPUS_MAX 128
#define NEMU_CPUS_MIN 1

#define TYPE_NEMU_MACHINE MACHINE_TYPE_NAME("nemu")
typedef struct NEMUState NEMUState;
DECLARE_INSTANCE_CHECKER(NEMUState, NEMU_MACHINE,
                         TYPE_NEMU_MACHINE)

typedef struct NEMUConfig{} NEMUConfig;

typedef struct Checkpoint{
    uint64_t cpt_interval;
    uint64_t sync_interval;
    uint64_t warmup_interval;
    uint64_t next_uniform_point;
    uint64_t checkpoint_mode;
    bool workload_loaded;
    bool workload_exit;
}Checkpoint_t;

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
}PathManager_t;

typedef struct SimpointInfo{
    GList *cpt_instructions;
    GList *weights;
}SimpointInfo_t;

typedef struct sync_info{
    uint64_t sync_interval;
    uint8_t *workload_loaded_percpu;
    uint8_t *workload_exit_percpu;
    uint64_t *workload_insns;
    bool *early_exit;  // such as wfi
    uint64_t cpus;
    uint64_t next_sync_point;
    bool *checkpoint_end;
}SyncInfo_t;

struct NEMUState{
    /*< private >*/
    MachineState parent;

    /*< public >*/
    RISCVHartArrayState soc[NEMU_CPUS_MAX];

    Checkpoint_t checkpoint_info;
    PathManager_t path_manager;
    SimpointInfo_t simpoint_info;
    SyncInfo_t sync_info;

    char* checkpoint;
    char* gcpt_restore;
    char* simpoint_path;
    char* workload_name;
    char* cpt_interval;
    char* warmup_interval;
    char* sync_interval;
    char* output_base_dir;
    char* config_name;

    NEMUConfig cfg;

    char* memory;
    char* gcpt_memory;

    DeviceState *irqchip[NEMU_CPUS_MAX];
};


#endif
