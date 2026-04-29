#pragma once

#include <stdint.h>
#include <stddef.h>

class ACPI {
public:
    static ACPI& get();
    
    bool initialize(uint64_t rsdpAddr);
    void shutdown();
    
    void* findTable(const char* signature);

    void enumerate();
    void reboot();
    void sysShutdown();
    
private:
    ACPI() = default;

    void* rsdp = nullptr;
    void* rsdt = nullptr;
    bool initialized = false;
};