#include "exec/memory.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/riscv_hart.h"
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/boards.h"
#include "hw/riscv/nemu.h"
#include "target/riscv/cpu.h"
#include "hw/display/ramfb.h"


#include "hw/sysbus.h"
#include <stdio.h>


static void nemu_load_firmware(MachineState *machine){
    char *firmware_name;
    NEMUState *s = NEMU_MACHINE(machine);
    uint64_t firmware_end_addr;

    /* Find firmware */
    firmware_name = riscv_find_firmware(machine->firmware,
                        riscv_default_firmware_name(&s->soc[0]));

    if (firmware_name) {
       firmware_end_addr = riscv_find_and_load_firmware(machine, firmware_name,
                                                         0x80000000, NULL);
       g_free(firmware_name);
    }
    printf("%s %d\n","firmware_end_addr",firmware_end_addr);

}


static void nemu_machine_init(MachineState *machine)
{
    MemoryRegion *system_memory = get_system_memory();
    /* register system main memory (actual RAM) */
    memory_region_add_subregion(system_memory, 0x80000000,
        machine->ram);

    nemu_load_firmware(machine);
}

static void nemu_machine_instance_init(Object *obj)
{
}


static void nemu_machine_class_init(ObjectClass *oc, void *data)
{
    char str[128];
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V NEMU board";
    mc->init = nemu_machine_init;
    mc->max_cpus = NEMU_CPUS_MAX;
    mc->min_cpus = NEMU_CPUS_MAX;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->default_ram_id = "riscv.nemu.ram";
    mc->no_parallel=1;
    mc->no_cdrom=1;
    mc->no_floppy=1;

}

static const TypeInfo nemu_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("nemu"),
    .parent     = TYPE_MACHINE,
    .class_init = nemu_machine_class_init,
    .instance_init = nemu_machine_instance_init,
    .instance_size = sizeof(NEMUState),
};

static void nemu_machine_init_register_types(void)
{
    type_register_static(&nemu_machine_typeinfo);
}

type_init(nemu_machine_init_register_types)
