#ifndef HW_RISCV_NEMU_H
#define HW_RISCV_NEMU_H

#include "hw/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"

#define NEMU_CPUS_MAX 128
#define NEMU_CPUS_MIN 1

#define TYPE_NEMU_MACHINE MACHINE_TYPE_NAME("nemu")
typedef struct NEMUState NEMUState;
DECLARE_INSTANCE_CHECKER(NEMUState, NEMU_MACHINE,
                         TYPE_NEMU_MACHINE)

typedef struct NEMUConfig{}NEMUConfig;

struct NEMUState{
    /*< private >*/
    MachineState parent;

    /*< public >*/
    RISCVHartArrayState soc[NEMU_CPUS_MAX];

    char* checkpoint;
    char* gcpt_restore;

    NEMUConfig cfg;

    char* memory;
    char* gcpt_memory;

    DeviceState *irqchip[NEMU_CPUS_MAX];
};


#endif
