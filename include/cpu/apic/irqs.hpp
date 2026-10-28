#pragma once

#include <stdint.h>

static constexpr uint8_t IRQ_TIMER = 0;
static constexpr uint8_t IRQ_KEYBOARD = 1;
static constexpr uint8_t IRQ_CASCADE = 2;
static constexpr uint8_t IRQ_COM2 = 3;
static constexpr uint8_t IRQ_COM1 = 4;
static constexpr uint8_t IRQ_LPT2 = 5;
static constexpr uint8_t IRQ_FLOPPY = 6;
static constexpr uint8_t IRQ_LPT1 = 7;
static constexpr uint8_t IRQ_RTC = 8;
static constexpr uint8_t IRQ_MOUSE = 12;

static constexpr uint8_t VECTOR_BASE = 32;
static constexpr uint8_t VECTOR_TIMER = 0x40;
static constexpr uint8_t VECTOR_KEYBOARD = VECTOR_BASE + IRQ_KEYBOARD;
static constexpr uint8_t VECTOR_RTC = VECTOR_BASE + IRQ_RTC;
static constexpr uint8_t VECTOR_MOUSE = VECTOR_BASE + IRQ_MOUSE;
static constexpr uint8_t VECTOR_PCI_BASE = 0x50;
static constexpr uint8_t VECTOR_MSI_BASE = 0x70;
static constexpr uint8_t VECTOR_MSI_LIMIT = 0x7F;
static constexpr uint8_t VECTOR_GPIO_BASE = 0x80;
static constexpr uint8_t VECTOR_GPIO_LIMIT = 0x8F;
// SMP: each application processor's periodic LAPIC timer, and the IPI a CPU
// sends to wake an idle one when it queues work (src/cpu/smp.cpp).
static constexpr uint8_t VECTOR_AP_TIMER = 0xF0;
static constexpr uint8_t VECTOR_RESCHEDULE = 0xF1;
// TLB shootdown IPI (src/cpu/tlb.cpp). Handled without the big kernel lock.
static constexpr uint8_t VECTOR_TLB_SHOOTDOWN = 0xF2;
static constexpr uint8_t VECTOR_SPURIOUS = 0xFF;
