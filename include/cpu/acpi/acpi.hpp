#pragma once

#include <stdint.h>
#include <stddef.h>
#include <cpu/acpi/aml.hpp>

class ACPI {
public:
    using TableCallback = void (*)(const char* signature, void* table, void* context);

    static ACPI& get();
    
    bool initialize(uint64_t rsdpAddr);
    void shutdown();
    
    void* findTable(const char* signature);
    void forEachTable(TableCallback callback, void* context);
    void* findDsdt();
    AML::Interpreter& aml();
    bool evaluateAml(const char* path, AML::Object* result);

    void enumerate();
    void reboot();
    void sysShutdown();
    
private:
    ACPI() = default;

    void* rsdp = nullptr;
    void* rsdt = nullptr;
    bool rootUsesXsdt = false;
    bool initialized = false;
    bool amlInitialized = false;
    AML::Interpreter amlInterpreter;
};
