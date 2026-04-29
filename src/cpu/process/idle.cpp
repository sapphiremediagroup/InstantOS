#include <drivers/usb/ohci.hpp>
#include <interrupts/keyboard.hpp>

extern "C" void idleLoop() {
    while (true) {
        USBInput::get().poll();
        Keyboard::get().servicePendingInput();
        asm volatile("sti; pause");
    }
}
