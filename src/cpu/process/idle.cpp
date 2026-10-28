#include <drivers/usb/ohci.hpp>
#include <drivers/hid/i2c_hid.hpp>
#include <interrupts/keyboard.hpp>
#include <cpu/klock.hpp>
#include <cpu/percpu.hpp>

// Each CPU's idle process. It is entered fresh by a context switch made while
// holding the big kernel lock, so it starts by dropping it: an idle CPU sits in
// hlt without the lock and retakes it for each interrupt (irqHandler).
extern "C" void idleLoop() {
    KernelLock::releaseAll();
    const bool bootCpu = getPerCPU()->cpuIndex == 0;
    while (true) {
        // Input devices are polled from the boot CPU only (their IRQs are
        // routed there as well); other CPUs just wait for work. Skipped while
        // another CPU holds the lock: the next wake-up polls again.
        if (bootCpu) {
            asm volatile("cli");
            if (KernelLock::tryEnter(KernelLock::kSiteIdlePoll)) {
                USBInput::get().poll();
                I2CHIDController::get().poll();
                Keyboard::get().servicePendingInput();
                KernelLock::leave();
            }
        }
        asm volatile("sti; hlt");
    }
}
