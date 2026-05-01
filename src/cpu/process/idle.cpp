#include <drivers/usb/ohci.hpp>
#include <drivers/hid/i2c_hid.hpp>
#include <interrupts/keyboard.hpp>

extern "C" void idleLoop() {
    while (true) {
        USBInput::get().poll();
        I2CHIDController::get().poll();
        Keyboard::get().servicePendingInput();
        asm volatile("sti; pause");
    }
}
