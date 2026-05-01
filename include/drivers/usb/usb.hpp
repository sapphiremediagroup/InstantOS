#pragma once

#include <stdint.h>

constexpr uint8_t USB_REQUEST_GET_DESCRIPTOR = 0x06;
constexpr uint8_t USB_REQUEST_SET_ADDRESS = 0x05;
constexpr uint8_t USB_REQUEST_SET_CONFIGURATION = 0x09;
constexpr uint8_t USB_REQUEST_SET_IDLE = 0x0A;
constexpr uint8_t USB_REQUEST_SET_PROTOCOL = 0x0B;

constexpr uint8_t USB_DESCRIPTOR_DEVICE = 0x01;
constexpr uint8_t USB_DESCRIPTOR_CONFIGURATION = 0x02;
constexpr uint8_t USB_DESCRIPTOR_INTERFACE = 0x04;
constexpr uint8_t USB_DESCRIPTOR_ENDPOINT = 0x05;
constexpr uint8_t USB_DESCRIPTOR_HID = 0x21;
constexpr uint8_t USB_DESCRIPTOR_HID_REPORT = 0x22;

constexpr uint8_t USB_CLASS_HID = 0x03;
constexpr uint8_t USB_HID_SUBCLASS_BOOT = 0x01;
constexpr uint8_t USB_HID_PROTOCOL_KEYBOARD = 0x01;
constexpr uint8_t USB_HID_PROTOCOL_MOUSE = 0x02;
constexpr uint8_t USB_ENDPOINT_IN = 0x80;
constexpr uint8_t USB_ENDPOINT_TYPE_INTERRUPT = 0x03;

struct UsbSetupPacket {
    uint8_t requestType;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));
