#ifndef HW_RISCV_NEMU_H
#define HW_RISCV_NEMU_H

#include "hw/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"
#include "stdint.h"
#include "checkpoint/directed_tbs.h"

#define NEMU_CPUS_MAX 128
#define NEMU_CPUS_MIN 1

#define TYPE_NEMU_MACHINE MACHINE_TYPE_NAME("nemu")
typedef struct NEMUState NEMUState;
DECLARE_INSTANCE_CHECKER(NEMUState, NEMU_MACHINE,
                         TYPE_NEMU_MACHINE)

typedef struct NEMUConfig{} NEMUConfig;

typedef struct Checkpoint{
    uint64_t cpt_interval;
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
    SyncUniformCheckpoint,
};

typedef struct PathManager{
    GString *base_dir;
    GString *workload_name;
    GString *config_name;

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
    SyncControlInfo sync_control_info;
    Qemu2Detail q2d_buf;

    bool single_core_cpt;
    char* checkpoint;
    char* gcpt_restore;

    char* simpoint_path;
    int d2q_fifo;
    int q2d_fifo;

    NEMUConfig cfg;
    char* memory;
    char* gcpt_memory;

    DeviceState *irqchip[NEMU_CPUS_MAX];
};


#endif
