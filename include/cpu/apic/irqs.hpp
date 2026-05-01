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
static constexpr uint8_t VECTOR_SPURIOUS = 0xFF;
