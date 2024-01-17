/*
 * QEMU RISC-V Spike Board
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2017-2018 SiFive, Inc.
 *
 * This provides a RISC-V Board with the following devices:
 *
 * 0) HTIF Console and Poweroff
 * 1) CLINT (Timer and IPI)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/nemu.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "hw/intc/riscv_aclint.h"
#include "chardev/char.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"

#include "hw/misc/unimp.h"
#include "hw/char/xilinx_uartlite.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include <libfdt.h>

enum {
    UART0_IRQ = 10,
    RTC_IRQ = 11,
    VIRTIO_IRQ = 1, /* 1 to 8 */
    VIRTIO_COUNT = 8,
    PCIE_IRQ = 0x20, /* 32 to 35 */
    VIRT_PLATFORM_BUS_IRQ = 64, /* 64 to 95 */
};

enum{
    NEMU_MROM,
    NEMU_PLIC,
    NEMU_CLINT,
    NEMU_UARTLITE,
    NEMU_DRAM,
};

/*
 * Freedom E310 G002 and G003 supports 52 interrupt sources while
 * Freedom E310 G000 supports 51 interrupt sources. We use the value
 * of G002 and G003, so it is 53 (including interrupt source 0).
 */
#define PLIC_NUM_SOURCES 53
#define PLIC_NUM_PRIORITIES 7
#define PLIC_PRIORITY_BASE 0x00
#define PLIC_PENDING_BASE 0x1000
#define PLIC_ENABLE_BASE 0x2000
#define PLIC_ENABLE_STRIDE 0x80
#define PLIC_CONTEXT_BASE 0x200000
#define PLIC_CONTEXT_STRIDE 0x1000

static const MemMapEntry nemu_memmap[] = {
    [NEMU_MROM]     =      {         0x1000,      0xf000 },
    [NEMU_PLIC]     =      {     0x3c000000,   0x4000000 },
    [NEMU_CLINT]    =      {     0x38000000,     0x10000 },
    [NEMU_UARTLITE] =      {     0x40600000,      0x1000 },
    [NEMU_DRAM]     =      {     0x80000000,        0x00 },

};

static void nemu_load_firmware(MachineState *machine){
    const MemMapEntry *memmap = nemu_memmap;
//    char *firmware_name;
    NEMUState *s = NEMU_MACHINE(machine);
    uint64_t firmware_end_addr=0;
    uint64_t kernel_entry=0;
    uint64_t kernel_start_addr=0;

    /* Find firmware */
//    firmware_name = riscv_find_firmware(machine->firmware,
//                        riscv_default_firmware_name(&s->soc[0]));

//    if (firmware_name) {
   firmware_end_addr = riscv_find_and_load_firmware(machine,riscv_default_firmware_name(&s->soc[0]),
                                                     memmap[NEMU_DRAM].base, NULL);
   printf("%s %lx\n",machine->firmware,firmware_end_addr);
//       g_free(firmware_name);
//    }


   if (machine->kernel_filename&&!kernel_entry) {
       kernel_start_addr=riscv_calc_kernel_start_addr(&s->soc[0],firmware_end_addr);
       kernel_entry=riscv_load_kernel(machine,&s->soc[0],kernel_start_addr,true,NULL);
   }
   printf("%s %lx\n",machine->kernel_filename,kernel_entry);

    /* load the reset vector */
   riscv_setup_rom_reset_vec(machine, &s->soc[0], memmap[NEMU_DRAM].base,
                              memmap[NEMU_MROM].base,
                              memmap[NEMU_MROM].size, memmap[NEMU_DRAM].base,
                              memmap[NEMU_DRAM].base);
}

static DeviceState *nemu_create_plic(const MemMapEntry *memmap, int socket,
                                     int base_hartid, int hart_count)
{
    DeviceState *ret;
    char *plic_hart_config;

    /* Per-socket PLIC hart topology configuration string */
    plic_hart_config = riscv_plic_hart_config_string(hart_count);

    /* Per-socket PLIC */
    ret = sifive_plic_create(
            memmap[NEMU_PLIC].base + socket * memmap[NEMU_PLIC].size,
            plic_hart_config, hart_count, base_hartid,
            PLIC_NUM_SOURCES,
            PLIC_NUM_PRIORITIES,
            PLIC_PRIORITY_BASE,
            PLIC_PENDING_BASE,
            PLIC_ENABLE_BASE,
            PLIC_ENABLE_STRIDE,
            PLIC_CONTEXT_BASE,
            PLIC_CONTEXT_STRIDE,
            memmap[NEMU_PLIC].size);

    g_free(plic_hart_config);

    return ret;
}

static void nemu_machine_init(MachineState *machine)
{

    const MemMapEntry *memmap = nemu_memmap;
    NEMUState *s = NEMU_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);
    DeviceState *dev;
    char *soc_name;
    int i, base_hartid, hart_count;

    for (i = 0; i < riscv_socket_count(machine); i++) {
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_fatal);

        /* Core Local Interruptor (timer and IPI) for each socket */
        /* Per-socket SiFive CLINT */
        riscv_aclint_swi_create(
            memmap[NEMU_CLINT].base + i * memmap[NEMU_CLINT].size,
            base_hartid, hart_count, false);
        riscv_aclint_mtimer_create(memmap[NEMU_CLINT].base +
                i * memmap[NEMU_CLINT].size + RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE, base_hartid, hart_count,
            RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, true);

        /* Per-socket interrupt controller */
        s->irqchip[i] = nemu_create_plic(memmap, i,
                                         base_hartid, hart_count);
    }

    /* boot rom */
    memory_region_init_rom(mask_rom, NULL, "riscv.nemu.mrom",
                           memmap[NEMU_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[NEMU_MROM].base,
                                mask_rom);

    /* register system main memory (actual RAM) */
    memory_region_add_subregion(system_memory, memmap[NEMU_DRAM].base,
        machine->ram);


    dev = qdev_new(TYPE_XILINX_UARTLITE);
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    memory_region_add_subregion(system_memory, memmap[NEMU_UARTLITE].base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));

    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(DEVICE(s->irqchip[0]),UART0_IRQ));

    nemu_load_firmware(machine);
}

static void nemu_machine_instance_init(Object *obj)
{
}



static void nemu_machine_class_init(ObjectClass *oc, void *data)
{
//    char str[128];
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V NEMU board";
    mc->init = nemu_machine_init;
    mc->max_cpus = NEMU_CPUS_MAX;
    mc->min_cpus = NEMU_CPUS_MIN;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->possible_cpu_arch_ids = riscv_numa_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id = riscv_numa_get_default_cpu_node_id;
    mc->numa_mem_supported = true;
    mc->default_ram_size = 8 * GiB;
    /* platform instead of architectural choice */
    mc->cpu_cluster_has_numa_boundary = true;
    mc->default_ram_id = "riscv.nemu.ram";
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
