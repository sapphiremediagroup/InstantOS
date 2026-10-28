/*
 * A small model of the Realtek RTL8168H (RTL8111H) gigabit NIC, for testing
 * InstantOS's r8169-style driver (src/drivers/net/rtl8169.cpp). QEMU has no
 * model of this chip, which is what most PC boards carry.
 *
 * It follows the r8169 programming model: C+ descriptor rings (16 bytes,
 * OWN/EOR/FS/LS in opts1), ChipCmd/TxPoll/IntrMask/IntrStatus, TxConfig with
 * the 8168H XID (0x541), RxConfig accept bits, PHY access through PHYAR and
 * through the 8168G+ GPHY_OCP window, MAC OCP and ERI, MISC.RXDV_GATED_EN and
 * MCU. It also plays the states a real board can hand over and the driver has
 * to cope with: receive gated (as Linux's rtl_hw_init_8168g leaves it), the
 * PHY powered down, and the link coming up only a while after autonegotiation
 * starts. Received frames carry 4 CRC bytes, counted in the length, like the
 * real chip. Not modelled: offloads, VLAN, the high-priority queue, counters,
 * jumbo frames, ERI/OCP side effects.
 *
 * Copied into hw/net/ by tools/qemu-rtl8168/build-qemu.sh. -device rtl8168.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "sysemu/dma.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "net/net.h"
#include "net/eth.h"
#include "qom/object.h"

#define TYPE_RTL8168 "rtl8168"
OBJECT_DECLARE_SIMPLE_TYPE(RTL8168State, RTL8168)

enum {
    R_MAC0 = 0x00, R_MAR0 = 0x08, R_TNPDS = 0x20, R_CHIPCMD = 0x37,
    R_TXPOLL = 0x38, R_IMR = 0x3c, R_ISR = 0x3e, R_TXCONFIG = 0x40,
    R_RXCONFIG = 0x44, R_CFG9346 = 0x50, R_PHYAR = 0x60, R_PHYSTATUS = 0x6c,
    R_PMCH = 0x6f, R_ERIDR = 0x70, R_ERIAR = 0x74, R_OCPDR = 0xb0,
    R_GPHY_OCP = 0xb8, R_MCU = 0xd3, R_RXMAXSIZE = 0xda, R_CPLUSCMD = 0xe0,
    R_RDSAR = 0xe4, R_MISC = 0xf0,
};

#define CMD_RESET 0x10
#define CMD_RXENB 0x08
#define CMD_TXENB 0x04
#define TXPOLL_NPQ 0x40

#define INT_RXOK 0x0001
#define INT_TXOK 0x0004
#define INT_RXOVERFLOW 0x0010
#define INT_LINKCHG 0x0020

#define RX_ACCEPT_ALLPHYS 0x01
#define RX_ACCEPT_MYPHYS 0x02
#define RX_ACCEPT_MULTICAST 0x04
#define RX_ACCEPT_BROADCAST 0x08

#define TXCFG_XID_8168H (0x541u << 20)
#define TXCFG_EMPTY (1u << 11)

#define MISC_RXDV_GATED (1u << 19)
#define MCU_NOW_IS_OOB 0x80
#define MCU_IDLE 0x32 /* TX_EMPTY | RX_EMPTY | LINK_LIST_RDY */

#define DESC_OWN (1u << 31)
#define DESC_EOR (1u << 30)
#define DESC_FS (1u << 29)
#define DESC_LS (1u << 28)
#define DESC_LEN 0x3fffu

#define BMCR_RESET 0x8000
#define BMCR_ANENABLE 0x1000
#define BMCR_PDOWN 0x0800
#define BMCR_ANRESTART 0x0200
#define BMSR_LSTATUS 0x0004
#define BMSR_ANEGCOMPLETE 0x0020

#define PHYSTATUS_LINK 0x02
#define PHYSTATUS_1000F 0x11 /* 1000 Mb/s, full duplex */

#define OCP_STD_PHY_BASE 0xa400
#define RING_DESCRIPTORS_MAX 1024

struct RTL8168State {
    PCIDevice parent_obj;
    NICState *nic;
    NICConf conf;
    MemoryRegion io;
    MemoryRegion mmio;
    QEMUTimer *link_timer;

    uint8_t mac[6];
    uint8_t mar[8];
    uint64_t tx_ring;
    uint64_t rx_ring;
    unsigned tx_index;
    unsigned rx_index;
    uint8_t chipcmd;
    uint16_t imr;
    uint16_t isr;
    uint32_t txconfig;
    uint32_t rxconfig;
    uint8_t cfg9346;
    uint8_t config[6];
    uint32_t phyar;
    uint8_t pmch;
    uint32_t eridr;
    uint32_t eriar;
    uint32_t eri[64];
    uint32_t ocpdr;
    uint16_t mac_ocp[256];
    uint32_t gphy_ocp;
    uint8_t mcu;
    uint16_t rxmaxsize;
    uint16_t cpluscmd;
    uint32_t misc;
    uint8_t other[256];

    uint16_t phy[32];
    bool link_up;          /* PHY has a link (after autonegotiation) */
    bool backend_up;       /* the QEMU netdev link (set_link) */
    bool irq_level;
    bool phy_powerdown;    /* property: PHY starts powered down */
    uint32_t link_delay_ms;
};

static void rtl8168_update_irq(RTL8168State *s)
{
    PCIDevice *d = PCI_DEVICE(s);
    bool level = (s->isr & s->imr) != 0;

    if (msi_enabled(d)) {
        if (level && !s->irq_level) {
            msi_notify(d, 0);
        }
    } else {
        pci_set_irq(d, level);
    }
    s->irq_level = level;
}

static void rtl8168_raise(RTL8168State *s, uint16_t cause)
{
    s->isr |= cause;
    rtl8168_update_irq(s);
}

static void rtl8168_set_link(RTL8168State *s, bool up)
{
    if (s->link_up == up) {
        return;
    }
    s->link_up = up;
    if (up) {
        s->phy[1] |= BMSR_LSTATUS | BMSR_ANEGCOMPLETE;
    } else {
        s->phy[1] &= ~(BMSR_LSTATUS | BMSR_ANEGCOMPLETE);
    }
    rtl8168_raise(s, INT_LINKCHG);
    if (up) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

/* Autonegotiation "finishes" a while after it is started on a powered PHY. */
static void rtl8168_link_timer(void *opaque)
{
    RTL8168State *s = opaque;
    rtl8168_set_link(s, s->backend_up && !(s->phy[0] & BMCR_PDOWN));
}

static void rtl8168_phy_reset(RTL8168State *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[0] = BMCR_ANENABLE | (s->phy_powerdown ? BMCR_PDOWN : 0);
    s->phy[1] = 0x7949;            /* 10/100 capable, AN able, ext status */
    s->phy[2] = 0x001c;            /* Realtek OUI */
    s->phy[3] = 0xc800;
    s->phy[4] = 0x01e1;
    s->phy[9] = 0x0300;
    s->phy[15] = 0x2000;           /* 1000BASE-T full duplex capable */
}

static void rtl8168_phy_write(RTL8168State *s, unsigned reg, uint16_t value)
{
    reg &= 31;
    if (reg == 0) {
        if (value & BMCR_RESET) {
            value &= ~BMCR_RESET;
        }
        bool restart = (value & BMCR_ANRESTART) ||
                       ((value & BMCR_ANENABLE) && !(s->phy[0] & BMCR_ANENABLE)) ||
                       (!(value & BMCR_PDOWN) && (s->phy[0] & BMCR_PDOWN));
        s->phy[0] = value & ~BMCR_ANRESTART;
        if (value & BMCR_PDOWN) {
            timer_del(s->link_timer);
            rtl8168_set_link(s, false);
        } else if (restart && (value & BMCR_ANENABLE)) {
            rtl8168_set_link(s, false);
            timer_mod(s->link_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->link_delay_ms);
        }
        return;
    }
    if (reg == 1 || reg == 2 || reg == 3 || reg == 15) {
        return;   /* read-only */
    }
    s->phy[reg] = value;
}

static uint16_t rtl8168_phy_read(RTL8168State *s, unsigned reg)
{
    return s->phy[reg & 31];
}

static uint8_t rtl8168_phystatus(RTL8168State *s)
{
    return s->link_up ? (PHYSTATUS_LINK | PHYSTATUS_1000F) : 0;
}

static void rtl8168_soft_reset(RTL8168State *s)
{
    memcpy(s->mac, s->conf.macaddr.a, 6);   /* reloaded from the EEPROM */
    memset(s->mar, 0, sizeof(s->mar));
    s->tx_index = 0;
    s->rx_index = 0;
    s->chipcmd = 0;
    s->imr = 0;
    s->isr = 0;
    s->rxconfig &= ~0x3fu;
    rtl8168_update_irq(s);
}

static void rtl8168_reset(DeviceState *dev)
{
    RTL8168State *s = RTL8168(dev);

    rtl8168_soft_reset(s);
    s->tx_ring = 0;
    s->rx_ring = 0;
    s->txconfig = TXCFG_XID_8168H | TXCFG_EMPTY;
    s->rxconfig = 0;
    s->cfg9346 = 0;
    memset(s->config, 0, sizeof(s->config));
    s->config[2] = 0x80;          /* ClkReqEn, as firmware tends to leave it */
    s->config[5] = 0x01;          /* ASPM_en */
    s->pmch = 0;
    s->mcu = MCU_IDLE | MCU_NOW_IS_OOB;
    s->misc = MISC_RXDV_GATED;    /* receive gated until the driver opens it */
    s->rxmaxsize = 0;
    s->cpluscmd = 0;
    memset(s->eri, 0, sizeof(s->eri));
    memset(s->mac_ocp, 0, sizeof(s->mac_ocp));
    /* The backup MAC address the r8169 driver reads from ERI 0xe0/0xe4. */
    s->eri[0xe0 / 4] = ldl_le_p(s->conf.macaddr.a);
    s->eri[0xe4 / 4] = lduw_le_p(s->conf.macaddr.a + 4);
    s->link_up = false;
    rtl8168_phy_reset(s);
    if (!s->phy_powerdown) {
        timer_mod(s->link_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->link_delay_ms);
    }
    rtl8168_update_irq(s);
}

static void rtl8168_read_desc(RTL8168State *s, uint64_t addr, uint32_t *opts1,
                              uint32_t *opts2, uint64_t *buf)
{
    uint8_t d[16];
    pci_dma_read(PCI_DEVICE(s), addr, d, sizeof(d));
    *opts1 = ldl_le_p(d);
    *opts2 = ldl_le_p(d + 4);
    *buf = ldq_le_p(d + 8);
}

static void rtl8168_write_opts(RTL8168State *s, uint64_t addr, uint32_t opts1, uint32_t opts2)
{
    uint8_t d[8];
    stl_le_p(d, opts1);
    stl_le_p(d + 4, opts2);
    pci_dma_write(PCI_DEVICE(s), addr, d, sizeof(d));
}

static void rtl8168_transmit(RTL8168State *s)
{
    static uint8_t frame[16384];
    size_t len = 0;
    bool sent = false;

    if (!(s->chipcmd & CMD_TXENB) || s->tx_ring == 0) {
        return;
    }
    for (unsigned n = 0; n < RING_DESCRIPTORS_MAX; ++n) {
        uint64_t daddr = s->tx_ring + 16ull * s->tx_index;
        uint32_t opts1, opts2;
        uint64_t buf;
        rtl8168_read_desc(s, daddr, &opts1, &opts2, &buf);
        if (!(opts1 & DESC_OWN)) {
            break;
        }
        uint32_t flen = opts1 & DESC_LEN;
        if (opts1 & DESC_FS) {
            len = 0;
        }
        if (len + flen <= sizeof(frame)) {
            pci_dma_read(PCI_DEVICE(s), buf, frame + len, flen);
            len += flen;
        }
        if ((opts1 & DESC_LS) && len > 0) {
            if (s->link_up) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, len);
            }
            len = 0;
            sent = true;
        }
        rtl8168_write_opts(s, daddr, opts1 & ~DESC_OWN, opts2);
        s->tx_index = (opts1 & DESC_EOR) ? 0 : s->tx_index + 1;
        if (s->tx_index >= RING_DESCRIPTORS_MAX) {
            s->tx_index = 0;
        }
    }
    if (sent) {
        rtl8168_raise(s, INT_TXOK);
    }
}

static bool rtl8168_can_receive(NetClientState *nc)
{
    RTL8168State *s = qemu_get_nic_opaque(nc);
    return s->link_up;
}

static bool rtl8168_accepts(RTL8168State *s, const uint8_t *buf)
{
    if (s->rxconfig & RX_ACCEPT_ALLPHYS) {
        return true;
    }
    if (is_broadcast_ether_addr(buf)) {
        return s->rxconfig & RX_ACCEPT_BROADCAST;
    }
    if (is_multicast_ether_addr(buf)) {
        return s->rxconfig & RX_ACCEPT_MULTICAST;
    }
    return (s->rxconfig & RX_ACCEPT_MYPHYS) && memcmp(buf, s->mac, 6) == 0;
}

static ssize_t rtl8168_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    RTL8168State *s = qemu_get_nic_opaque(nc);
    uint8_t frame[16384 + 4];

    /* Gated, stopped or unconfigured: the frame is lost, as on the wire. */
    if (!(s->chipcmd & CMD_RXENB) || (s->misc & MISC_RXDV_GATED) ||
        s->rx_ring == 0 || size < 14 || size > 16384) {
        return size;
    }
    if (!rtl8168_accepts(s, buf)) {
        return size;
    }
    size_t padded = size < 60 ? 60 : size;
    memset(frame, 0, padded + 4);
    memcpy(frame, buf, size);
    size_t total = padded + 4;       /* the chip passes the CRC on */
    if (s->rxmaxsize != 0 && total > s->rxmaxsize) {
        return size;
    }

    /* Check that the whole frame has descriptors before using any. */
    size_t offset = 0;
    unsigned index = s->rx_index;
    for (unsigned n = 0; offset < total; ++n) {
        uint32_t opts1, opts2;
        uint64_t daddr = s->rx_ring + 16ull * index;
        uint64_t bufaddr;
        rtl8168_read_desc(s, daddr, &opts1, &opts2, &bufaddr);
        uint32_t room = opts1 & DESC_LEN;
        if (!(opts1 & DESC_OWN) || room == 0 || n >= RING_DESCRIPTORS_MAX) {
            rtl8168_raise(s, INT_RXOVERFLOW);
            return size;
        }
        offset += room;
        index = (opts1 & DESC_EOR) ? 0 : index + 1;
    }

    offset = 0;
    while (offset < total) {
        uint32_t opts1, opts2;
        uint64_t daddr = s->rx_ring + 16ull * s->rx_index;
        uint64_t bufaddr;
        rtl8168_read_desc(s, daddr, &opts1, &opts2, &bufaddr);
        uint32_t room = opts1 & DESC_LEN;
        size_t chunk = total - offset < room ? total - offset : room;
        pci_dma_write(PCI_DEVICE(s), bufaddr, frame + offset, chunk);
        uint32_t status = (opts1 & DESC_EOR) | (uint32_t)chunk;
        if (offset == 0) {
            status |= DESC_FS;
        }
        offset += chunk;
        if (offset == total) {
            status |= DESC_LS;
        }
        rtl8168_write_opts(s, daddr, status, 0);
        s->rx_index = (opts1 & DESC_EOR) ? 0 : s->rx_index + 1;
        if (s->rx_index >= RING_DESCRIPTORS_MAX) {
            s->rx_index = 0;
        }
    }
    rtl8168_raise(s, INT_RXOK);
    return size;
}

static void rtl8168_link_status_changed(NetClientState *nc)
{
    RTL8168State *s = qemu_get_nic_opaque(nc);
    s->backend_up = !nc->link_down;
    if (!s->backend_up) {
        timer_del(s->link_timer);
        rtl8168_set_link(s, false);
    } else if (!(s->phy[0] & BMCR_PDOWN)) {
        timer_mod(s->link_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->link_delay_ms);
    }
}

static uint32_t rtl8168_reg_read(RTL8168State *s, hwaddr addr, unsigned size)
{
    switch (addr) {
    case R_TNPDS: return (uint32_t)s->tx_ring;
    case R_TNPDS + 4: return (uint32_t)(s->tx_ring >> 32);
    case R_RDSAR: return (uint32_t)s->rx_ring;
    case R_RDSAR + 4: return (uint32_t)(s->rx_ring >> 32);
    case R_CHIPCMD: return s->chipcmd;
    case R_TXPOLL: return 0;
    case R_IMR: return s->imr;
    case R_ISR: return s->isr;
    case R_TXCONFIG: return s->txconfig;
    case R_RXCONFIG: return s->rxconfig;
    case R_CFG9346: return s->cfg9346;
    case R_PHYAR: return s->phyar;
    case R_PHYSTATUS: return rtl8168_phystatus(s);
    case R_PMCH: return s->pmch;
    case R_ERIDR: return s->eridr;
    case R_ERIAR: return s->eriar;
    case R_OCPDR: return s->ocpdr;
    case R_GPHY_OCP: return s->gphy_ocp;
    case R_MCU: return s->mcu;
    case R_RXMAXSIZE: return s->rxmaxsize;
    case R_CPLUSCMD: return s->cpluscmd;
    case R_MISC: return s->misc;
    }
    if (addr < 6) {
        return s->mac[addr];
    }
    if (addr >= R_MAR0 && addr < R_MAR0 + 8) {
        return s->mar[addr - R_MAR0];
    }
    if (addr >= 0x51 && addr <= 0x56) {
        return s->config[addr - 0x51];
    }
    return addr < sizeof(s->other) ? s->other[addr] : 0;
}

static void rtl8168_eri_access(RTL8168State *s, uint32_t value)
{
    unsigned index = (value & 0xfff) / 4;
    s->eriar = value;
    if (index >= ARRAY_SIZE(s->eri) || ((value >> 16) & 3) != 0) {
        s->eriar ^= 1u << 31;   /* finish, ignore other address spaces */
        return;
    }
    uint32_t mask = 0;
    for (int i = 0; i < 4; ++i) {
        if (value & (1u << (12 + i))) {
            mask |= 0xffu << (8 * i);
        }
    }
    if (value & (1u << 31)) {
        s->eri[index] = (s->eri[index] & ~mask) | (s->eridr & mask);
        s->eriar &= ~(1u << 31);
    } else {
        s->eridr = s->eri[index] & mask;
        s->eriar |= 1u << 31;
    }
}

static void rtl8168_reg_write(RTL8168State *s, hwaddr addr, uint32_t value, unsigned size)
{
    switch (addr) {
    case R_TNPDS: s->tx_ring = (s->tx_ring & ~0xffffffffull) | value; s->tx_index = 0; return;
    case R_TNPDS + 4: s->tx_ring = (s->tx_ring & 0xffffffffull) | ((uint64_t)value << 32); return;
    case R_RDSAR: s->rx_ring = (s->rx_ring & ~0xffffffffull) | value; s->rx_index = 0; return;
    case R_RDSAR + 4: s->rx_ring = (s->rx_ring & 0xffffffffull) | ((uint64_t)value << 32); return;
    case R_CHIPCMD:
        if (value & CMD_RESET) {
            rtl8168_soft_reset(s);
            return;
        }
        s->chipcmd = value & (CMD_RXENB | CMD_TXENB);
        return;
    case R_TXPOLL:
        if (value & TXPOLL_NPQ) {
            rtl8168_transmit(s);
        }
        return;
    case R_IMR:
        s->imr = value;
        rtl8168_update_irq(s);
        return;
    case R_ISR:
        s->isr &= ~value;
        rtl8168_update_irq(s);
        return;
    case R_TXCONFIG:
        s->txconfig = TXCFG_XID_8168H | TXCFG_EMPTY | (value & 0x0f0ff7ff & ~TXCFG_EMPTY);
        return;
    case R_RXCONFIG: s->rxconfig = value; return;
    case R_CFG9346: s->cfg9346 = value; return;
    case R_PHYAR:
        /* Pre-8168G MDIO: bit 31 set = write, completion flips bit 31. */
        if (value & (1u << 31)) {
            rtl8168_phy_write(s, (value >> 16) & 31, value & 0xffff);
            s->phyar = value & ~(1u << 31);
        } else {
            s->phyar = (value & 0x7fff0000) | rtl8168_phy_read(s, (value >> 16) & 31) | (1u << 31);
        }
        return;
    case R_PMCH: s->pmch = value; return;
    case R_ERIDR: s->eridr = value; return;
    case R_ERIAR: rtl8168_eri_access(s, value); return;
    case R_OCPDR: {
        /* MAC OCP: address (even) in bits 30:16 as reg << 15. */
        unsigned reg = ((value >> 15) & 0xfffe);
        unsigned idx = (reg / 2) & 0xff;
        if (value & (1u << 31)) {
            s->mac_ocp[idx] = value & 0xffff;
        }
        s->ocpdr = (value & 0x7fff0000) | s->mac_ocp[idx];
        return;
    }
    case R_GPHY_OCP: {
        /* 8168G+ PHY OCP: the standard registers sit at 0xa400 + 2 * reg. */
        unsigned reg = (value >> 15) & 0xfffe;
        bool std = reg >= OCP_STD_PHY_BASE && reg < OCP_STD_PHY_BASE + 64;
        if (value & (1u << 31)) {
            if (std) {
                rtl8168_phy_write(s, (reg - OCP_STD_PHY_BASE) / 2, value & 0xffff);
            }
            s->gphy_ocp = value & ~(1u << 31);
        } else {
            uint16_t data = std ? rtl8168_phy_read(s, (reg - OCP_STD_PHY_BASE) / 2) : 0;
            s->gphy_ocp = (value & 0x7fff0000) | data | (1u << 31);
        }
        return;
    }
    case R_MCU:
        s->mcu = MCU_IDLE | (value & MCU_NOW_IS_OOB);
        return;
    case R_RXMAXSIZE: s->rxmaxsize = value; return;
    case R_CPLUSCMD: s->cpluscmd = value; return;
    case R_MISC:
        s->misc = value;
        if (!(value & MISC_RXDV_GATED)) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    }
    if (addr < 6) {
        s->mac[addr] = value;
        return;
    }
    if (addr >= R_MAR0 && addr < R_MAR0 + 8) {
        s->mar[addr - R_MAR0] = value;
        return;
    }
    if (addr >= 0x51 && addr <= 0x56) {
        s->config[addr - 0x51] = value;
        return;
    }
    if (addr < sizeof(s->other)) {
        s->other[addr] = value;
    }
}

/* Registers are 8, 16 or 32 bits wide at their natural offsets; anything else
 * is split into bytes. */
static bool rtl8168_wide_reg(hwaddr addr, unsigned size)
{
    switch (addr) {
    case R_TNPDS: case R_TNPDS + 4: case R_RDSAR: case R_RDSAR + 4:
    case R_TXCONFIG: case R_RXCONFIG: case R_PHYAR: case R_ERIDR:
    case R_ERIAR: case R_OCPDR: case R_GPHY_OCP: case R_MISC:
        return size == 4;
    case R_IMR: case R_ISR: case R_RXMAXSIZE: case R_CPLUSCMD:
        return size == 2;
    }
    return false;
}

static uint64_t rtl8168_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL8168State *s = opaque;
    if (size == 1 || rtl8168_wide_reg(addr, size)) {
        return rtl8168_reg_read(s, addr, size);
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        value |= (uint64_t)(rtl8168_reg_read(s, addr + i, 1) & 0xff) << (8 * i);
    }
    return value;
}

static void rtl8168_mmio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RTL8168State *s = opaque;
    if (size == 1 || rtl8168_wide_reg(addr, size)) {
        rtl8168_reg_write(s, addr, (uint32_t)value, size);
        return;
    }
    for (unsigned i = 0; i < size; ++i) {
        rtl8168_reg_write(s, addr + i, (value >> (8 * i)) & 0xff, 1);
    }
}

static const MemoryRegionOps rtl8168_ops = {
    .read = rtl8168_mmio_read,
    .write = rtl8168_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static NetClientInfo net_rtl8168_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = rtl8168_can_receive,
    .receive = rtl8168_receive,
    .link_status_changed = rtl8168_link_status_changed,
};

static void rtl8168_realize(PCIDevice *dev, Error **errp)
{
    RTL8168State *s = RTL8168(dev);
    DeviceState *d = DEVICE(dev);

    dev->config[PCI_INTERRUPT_PIN] = 1;
    memory_region_init_io(&s->io, OBJECT(s), &rtl8168_ops, s, "rtl8168-io", 0x100);
    memory_region_init_io(&s->mmio, OBJECT(s), &rtl8168_ops, s, "rtl8168-mmio", 0x1000);
    /* As on the real card: BAR0 I/O ports, BAR2 64-bit registers. */
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->mmio);
    if (msi_init(dev, 0x50, 1, true, false, errp) < 0) {
        return;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_rtl8168_info, &s->conf, object_get_typename(OBJECT(dev)),
                          d->id, &d->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    s->backend_up = !qemu_get_queue(s->nic)->link_down;
    s->link_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, rtl8168_link_timer, s);
}

static void rtl8168_exit(PCIDevice *dev)
{
    RTL8168State *s = RTL8168(dev);
    timer_free(s->link_timer);
    msi_uninit(dev);
    qemu_del_nic(s->nic);
}

static Property rtl8168_properties[] = {
    DEFINE_NIC_PROPERTIES(RTL8168State, conf),
    DEFINE_PROP_BOOL("phy-powerdown", RTL8168State, phy_powerdown, true),
    DEFINE_PROP_UINT32("link-delay-ms", RTL8168State, link_delay_ms, 1500),
    DEFINE_PROP_END_OF_LIST(),
};

static void rtl8168_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = rtl8168_realize;
    k->exit = rtl8168_exit;
    k->vendor_id = PCI_VENDOR_ID_REALTEK;
    k->device_id = 0x8168;
    k->subsystem_vendor_id = 0x103c;
    k->subsystem_id = 0x8061;
    k->revision = 0x15;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    dc->reset = rtl8168_reset;
    dc->desc = "Realtek RTL8168H Gigabit Ethernet (test model)";
    device_class_set_props(dc, rtl8168_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo rtl8168_info = {
    .name = TYPE_RTL8168,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(RTL8168State),
    .class_init = rtl8168_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void rtl8168_register_types(void)
{
    type_register_static(&rtl8168_info);
}

type_init(rtl8168_register_types)
