#pragma once

#include <stdint.h>

struct CPUFeatures {
    bool cpuidSupported;
    bool fpu;
    bool tsc;
    bool fxsave;
    bool nx;
    bool nxEnabled;
    bool mmx;
    bool sse;
    bool sse2;
    bool sse3;
    bool ssse3;
    bool sse41;
    bool sse42;
    bool xsave;
    bool osxsave;
    bool xsaveEnabled;
    bool xsaveopt;
    bool avx;
    bool avx2;
    bool avxEnabled;
    bool avx512;
    bool avx512Enabled;
    bool pku;
    bool pkuEnabled;
    bool writeProtectEnabled;
    bool umip;
    bool umipEnabled;
    bool fsgsbase;
    bool fsgsbaseEnabled;
    bool smep;
    bool smepEnabled;
    bool longMode;
    uint64_t xcr0SupportedMask;
    uint64_t xsaveMask;
};

class CPU {
public:
    static bool initialize();
    static const CPUFeatures& getFeatures();
    static bool hasLegacySIMD();
    static bool hasAVXHardware();
    static bool avxEnabledByOS();
    static bool usesXsave();
    static void initializeExtendedState(void* state);
    static void saveExtendedState(void* state);
    static void restoreExtendedState(const void* state);

private:
    static CPUFeatures features;
    static bool initialized;
};

extern "C" uint64_t cpuXsaveMask;
extern "C" uint8_t cpuUseXsave;
extern "C" uint8_t cpuUseXsaveOpt;
