#pragma once

#include <stdint.h>

class XHCIController {
public:
    static XHCIController& get();

    bool initialize();
    void poll();
    bool claimPendingInterrupt();
    void handleInterrupt();

    uint8_t controllerCount() const { return controllersFound; }
    uint8_t initializedControllerCount() const { return initializedControllers; }
    bool hasInitializedController() const { return initializedControllers > 0; }
    bool hasBootKeyboard() const;
    bool hasBootMouse() const;

private:
    XHCIController() = default;

    uint8_t controllersFound = 0;
    uint8_t initializedControllers = 0;
};
