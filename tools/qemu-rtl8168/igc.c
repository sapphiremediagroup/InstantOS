/*
 * -device igc: QEMU's igb (82576) model under the Intel I225-V's PCI IDs, for
 * InstantOS's I225/I226 support (the Igc family in src/drivers/net/e1000.cpp).
 * The I225/I226 keep the 82576's layout for everything that driver uses (the
 * queue registers at 0xc000/0xe000, SRRCTL, the RXDCTL/TXDCTL queue enables,
 * advanced descriptors, IMS/ICR, RAL/RAH, MDIC), as Linux's igc_regs.h shows,
 * so this checks the driver's I225 identification, reset and data path; it does
 * not model what only the real chip does (the GPY PHY, 2.5G, its NVM flash).
 *
 * Copied into hw/net/ by tools/qemu-rtl8168/build-qemu.sh.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "qemu/module.h"
#include "qom/object.h"

static void igc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x15f3;   /* I225-V */
    k->revision = 0x03;
    dc->desc = "Intel I225-V 2.5G (test model: QEMU's igb with the I225's IDs)";
}

static const TypeInfo igc_info = {
    .name = "igc",
    .parent = "igb",
    .class_init = igc_class_init,
};

static void igc_register_types(void)
{
    type_register_static(&igc_info);
}

type_init(igc_register_types)
