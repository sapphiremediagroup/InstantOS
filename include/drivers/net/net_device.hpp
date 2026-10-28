#pragma once

#include <stdint.h>
#include <stddef.h>

// Maximum Ethernet frame size handled by the network stack (incl. header,
// excluding FCS).  Drivers must accept frames up to this size.
constexpr size_t NET_DEVICE_MTU = 1514;

// Abstract network device interface.
//
// Every NIC driver (VirtIO-net, RTL8139, Intel e1000, ...) implements this
// interface and registers a singleton instance with NetDeviceRegistry.  The
// network stack (src/cpu/syscall/net.cpp) talks only to NetDevice and never
// references a concrete driver, so multiple NIC types can coexist.
//
// All methods operate on raw Ethernet frames (the driver is responsible for
// any device-specific framing/headers it needs internally).
class NetDevice {
public:
    virtual ~NetDevice() = default;

    // Probe for the device and bring it up.  Idempotent: returns true if the
    // device is (already) initialized.  Returns false if no matching hardware
    // is present or initialization failed.
    virtual bool initialize() = 0;

    // True once initialize() has succeeded.
    virtual bool isInitialized() const = 0;

    // True if matching hardware was detected on the PCI bus (regardless of
    // whether initialize() has fully run yet).
    virtual bool isAvailable() const = 0;

    // Copy the 6-byte MAC address into `mac`.
    virtual void getMacAddress(uint8_t* mac) = 0;

    // Transmit a single Ethernet frame.  Returns true on success.
    virtual bool sendPacket(const void* data, size_t len) = 0;

    // Receive a single Ethernet frame into `buffer` (up to `maxLen` bytes).
    // Returns the number of bytes copied, or -1 if no frame is available.
    virtual int receivePacket(void* buffer, size_t maxLen) = 0;

    // True if the physical/virtual link is up.
    virtual bool isLinkUp() const = 0;

    // True if the driver raises an interrupt for received frames and calls
    // netDeviceInterrupt from it. Otherwise the stack polls on every tick.
    virtual bool interruptDriven() const { return false; }

    // Human-readable driver name (e.g. "virtio-net", "rtl8139", "e1000").
    virtual const char* name() const = 0;

    // Probe priority: lower values are preferred when multiple NICs are
    // present.  Used to make device selection deterministic regardless of
    // static-initialization order across translation units.  Paravirtualized
    // devices (virtio) should rank ahead of emulated physical NICs.
    virtual int probePriority() const { return 100; }
};

// Global registry of NIC drivers.
//
// Each driver registers its singleton at static-init time.  The stack then
// asks the registry for the "active" device (the first registered driver whose
// hardware is present and that initializes successfully).
class NetDeviceRegistry {
public:
    static constexpr size_t MAX_DEVICES = 8;

    // Register a driver instance.  Called from each driver's static
    // self-registration.  Safe to call before any device is initialized.
    static void registerDevice(NetDevice* device);

    // Return the active network device, probing registered drivers on first
    // use.  Returns nullptr if no NIC is present.  The result is cached.
    static NetDevice* active();

    // Number of registered drivers (mostly for diagnostics/tests).
    static size_t count();

    // Registered driver at `index`, or nullptr if out of range.
    static NetDevice* deviceAt(size_t index);
};

// The kernel drives the network stack (src/cpu/syscall/net.cpp) itself:
// netEnableTimerPump brings up the active NIC at boot, after which the boot
// CPU's timer tick calls netTimerTick to process received frames and run the
// ARP and TCP timers. The tick must only call it when it did not interrupt
// kernel code, since the stack is not reentrant.
void netEnableTimerPump();
void netTimerTick();

struct InterruptFrame;
// Called by a NIC's receive interrupt handler (with the big kernel lock held,
// like any IRQ): processes the received frames. With `mayReschedule`, a
// process woken by them is switched to straight away; only for a handler that
// has already sent its EOI.
void netDeviceInterrupt(InterruptFrame* frame, bool mayReschedule);
