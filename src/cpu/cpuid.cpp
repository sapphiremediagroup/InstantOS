#include <cpu/cpuid.hpp>
#include <common/ports.hpp>
#include <common/string.hpp>
#include <graphics/console.hpp>
#include <cpuid.h>

CPUFeatures CPU::features = {};
bool CPU::initialized = false;
extern "C" uint64_t cpuXsaveMask = 0;
extern "C" uint8_t cpuUseXsave = 0;
extern "C" uint8_t cpuUseXsaveOpt = 0;

namespace {
constexpr uint64_t CR0_MP = 1ULL << 1;
constexpr uint64_t CR0_EM = 1ULL << 2;
constexpr uint64_t CR0_TS = 1ULL << 3;
constexpr uint64_t CR0_NE = 1ULL << 5;
constexpr uint64_t CR0_WP = 1ULL << 16;

constexpr uint64_t CR4_OSFXSR = 1ULL << 9;
constexpr uint64_t CR4_OSXMMEXCPT = 1ULL << 10;
constexpr uint64_t CR4_UMIP = 1ULL << 11;
constexpr uint64_t CR4_FSGSBASE = 1ULL << 16;
constexpr uint64_t CR4_OSXSAVE = 1ULL << 18;
constexpr uint64_t CR4_SMEP = 1ULL << 20;
constexpr uint64_t CR4_PKE = 1ULL << 22;

constexpr uint64_t XCR0_X87 = 1ULL << 0;
constexpr uint64_t XCR0_SSE = 1ULL << 1;
constexpr uint64_t XCR0_AVX = 1ULL << 2;
constexpr uint64_t XCR0_OPMASK = 1ULL << 5;
constexpr uint64_t XCR0_ZMM_HI256 = 1ULL << 6;
constexpr uint64_t XCR0_HI16_ZMM = 1ULL << 7;
constexpr uint64_t XCR0_PKRU = 1ULL << 9;
constexpr size_t EXTENDED_STATE_BUFFER_SIZE = 4096;

constexpr uint32_t MSR_EFER = 0xC0000080;
constexpr uint64_t EFER_NXE = 1ULL << 11;

constexpr uint32_t CPUID_EXT_NX = 1U << 20;
constexpr uint32_t CPUID_LEAF7_UMIP = 1U << 2;
constexpr uint32_t CPUID_LEAF7_PKU = 1U << 3;

alignas(16) const uint32_t kDefaultMxcsr = 0x1F80U;

bool cpuidSupported() {
    uint64_t originalFlags = 0;
    uint64_t toggledFlags = 0;

    asm volatile(
        "pushfq\n"
        "popq %0\n"
        : "=r"(originalFlags));

    toggledFlags = originalFlags ^ (1ULL << 21);

    asm volatile(
        "pushq %0\n"
        "popfq\n"
        :
        : "r"(toggledFlags)
        : "cc");

    asm volatile(
        "pushfq\n"
        "popq %0\n"
        : "=r"(toggledFlags));

    asm volatile(
        "pushq %0\n"
        "popfq\n"
        :
        : "r"(originalFlags)
        : "cc");

    return ((originalFlags ^ toggledFlags) & (1ULL << 21)) != 0;
}

uint64_t readXcr0() {
    uint32_t eax = 0;
    uint32_t edx = 0;
    asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<uint64_t>(edx) << 32) | eax;
}

void writeXcr0(uint64_t value) {
    uint32_t eax = static_cast<uint32_t>(value);
    uint32_t edx = static_cast<uint32_t>(value >> 32);
    asm volatile("xsetbv" : : "a"(eax), "d"(edx), "c"(0) : "memory");
}

void enableBaseFPUAndSIMD(const CPUFeatures& features) {
    uint64_t cr0 = 0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~CR0_EM;
    cr0 &= ~CR0_TS;
    cr0 |= CR0_MP | CR0_NE | CR0_WP;
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    if (features.fxsave && features.sse) {
        uint64_t cr4 = 0;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
        asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
}

void enableNoExecute() {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_NXE);
}

void enableXsave() {
    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSXSAVE;
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void enableUserModeInstructionPrevention() {
    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_UMIP;
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void enableFsgsbase() {
    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_FSGSBASE;
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void enableSmep() {
    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_SMEP;
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void enableProtectionKeys() {
    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_PKE;
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void writePkru(uint32_t value) {
    asm volatile(
        ".intel_syntax noprefix\n"
        "xor ecx, ecx\n"
        "xor edx, edx\n"
        "wrpkru\n"
        ".att_syntax prefix\n"
        :
        : "a"(value)
        : "ecx", "edx", "memory");
}

void clearVectorRegisters(bool avxEnabled, bool avx512Enabled) {
    if (avx512Enabled) {
        asm volatile(
            ".intel_syntax noprefix\n"
            "kxorq k0, k0, k0\n"
            "kxorq k1, k1, k1\n"
            "kxorq k2, k2, k2\n"
            "kxorq k3, k3, k3\n"
            "kxorq k4, k4, k4\n"
            "kxorq k5, k5, k5\n"
            "kxorq k6, k6, k6\n"
            "kxorq k7, k7, k7\n"
            "vxorps zmm0, zmm0, zmm0\n"
            "vxorps zmm1, zmm1, zmm1\n"
            "vxorps zmm2, zmm2, zmm2\n"
            "vxorps zmm3, zmm3, zmm3\n"
            "vxorps zmm4, zmm4, zmm4\n"
            "vxorps zmm5, zmm5, zmm5\n"
            "vxorps zmm6, zmm6, zmm6\n"
            "vxorps zmm7, zmm7, zmm7\n"
            "vxorps zmm8, zmm8, zmm8\n"
            "vxorps zmm9, zmm9, zmm9\n"
            "vxorps zmm10, zmm10, zmm10\n"
            "vxorps zmm11, zmm11, zmm11\n"
            "vxorps zmm12, zmm12, zmm12\n"
            "vxorps zmm13, zmm13, zmm13\n"
            "vxorps zmm14, zmm14, zmm14\n"
            "vxorps zmm15, zmm15, zmm15\n"
            "vxorps zmm16, zmm16, zmm16\n"
            "vxorps zmm17, zmm17, zmm17\n"
            "vxorps zmm18, zmm18, zmm18\n"
            "vxorps zmm19, zmm19, zmm19\n"
            "vxorps zmm20, zmm20, zmm20\n"
            "vxorps zmm21, zmm21, zmm21\n"
            "vxorps zmm22, zmm22, zmm22\n"
            "vxorps zmm23, zmm23, zmm23\n"
            "vxorps zmm24, zmm24, zmm24\n"
            "vxorps zmm25, zmm25, zmm25\n"
            "vxorps zmm26, zmm26, zmm26\n"
            "vxorps zmm27, zmm27, zmm27\n"
            "vxorps zmm28, zmm28, zmm28\n"
            "vxorps zmm29, zmm29, zmm29\n"
            "vxorps zmm30, zmm30, zmm30\n"
            "vxorps zmm31, zmm31, zmm31\n"
            ".att_syntax prefix\n"
            :
            :
            : "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7",
              "zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7",
              "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15",
              "zmm16", "zmm17", "zmm18", "zmm19", "zmm20", "zmm21", "zmm22", "zmm23",
              "zmm24", "zmm25", "zmm26", "zmm27", "zmm28", "zmm29", "zmm30", "zmm31");
        return;
    }

    if (avxEnabled) {
        asm volatile("vzeroall"
                     :
                     :
                     : "xmm0", "xmm1", "xmm2", "xmm3",
                       "xmm4", "xmm5", "xmm6", "xmm7",
                       "xmm8", "xmm9", "xmm10", "xmm11",
                       "xmm12", "xmm13", "xmm14", "xmm15");
        return;
    }

    asm volatile(
        ".intel_syntax noprefix\n"
        "pxor xmm0, xmm0\n"
        "movdqa xmm1, xmm0\n"
        "movdqa xmm2, xmm0\n"
        "movdqa xmm3, xmm0\n"
        "movdqa xmm4, xmm0\n"
        "movdqa xmm5, xmm0\n"
        "movdqa xmm6, xmm0\n"
        "movdqa xmm7, xmm0\n"
        "movdqa xmm8, xmm0\n"
        "movdqa xmm9, xmm0\n"
        "movdqa xmm10, xmm0\n"
        "movdqa xmm11, xmm0\n"
        "movdqa xmm12, xmm0\n"
        "movdqa xmm13, xmm0\n"
        "movdqa xmm14, xmm0\n"
        "movdqa xmm15, xmm0\n"
        ".att_syntax prefix\n"
        :
        :
        : "xmm0", "xmm1", "xmm2", "xmm3",
          "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11",
          "xmm12", "xmm13", "xmm14", "xmm15");
}

void printFeatureStatus(const char* label, bool supported, bool enabled) {
    Console::get().drawText(label);
    Console::get().drawText(": ");
    Console::get().setTextColor(supported ? 0x49ceee : 0x777777);
    Console::get().drawText(supported ? "yes" : "no");
    if (supported) {
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" [");
        Console::get().setTextColor(enabled ? 0x49ceee : 0xFFCC66);
        Console::get().drawText(enabled ? "enabled" : "hw-only");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText("]");
    }
    Console::get().setTextColor(0xFFFFFF);
}
}

bool CPU::initialize() {
    if (initialized) {
        return true;
    }

    features = {};
    features.cpuidSupported = cpuidSupported();
    if (!features.cpuidSupported) {
        Console::get().drawText("CPU: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("NO CPUID");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
        return false;
    }

    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    unsigned int maxBasicLeaf = 0;
    unsigned int maxExtendedLeaf = 0;

    __get_cpuid(0, &maxBasicLeaf, &ebx, &ecx, &edx);
    __get_cpuid(0x80000000, &maxExtendedLeaf, &ebx, &ecx, &edx);

    if (maxBasicLeaf >= 1 && __get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        features.fpu = (edx & bit_FPU) != 0;
        features.tsc = (edx & bit_TSC) != 0;
        features.fxsave = (edx & bit_FXSR) != 0;
        features.mmx = (edx & bit_MMX) != 0;
        features.sse = (edx & bit_SSE) != 0;
        features.sse2 = (edx & bit_SSE2) != 0;
        features.sse3 = (ecx & bit_SSE3) != 0;
        features.ssse3 = (ecx & bit_SSSE3) != 0;
        features.sse41 = (ecx & bit_SSE4_1) != 0;
        features.sse42 = (ecx & bit_SSE4_2) != 0;
        features.xsave = (ecx & bit_XSAVE) != 0;
        features.osxsave = (ecx & bit_OSXSAVE) != 0;
        features.avx = (ecx & bit_AVX) != 0;
    }

    if (maxBasicLeaf >= 7 && __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        features.avx2 = (ebx & bit_AVX2) != 0;
        features.avx512 = (ebx & bit_AVX512F) != 0;
        features.umip = (ecx & CPUID_LEAF7_UMIP) != 0;
        features.fsgsbase = (ebx & bit_FSGSBASE) != 0;
        features.smep = (ebx & bit_SMEP) != 0;
        features.pku = (ecx & CPUID_LEAF7_PKU) != 0;
    }

    if (maxBasicLeaf >= 0xD && features.xsave && __get_cpuid_count(0xD, 1, &eax, &ebx, &ecx, &edx)) {
        features.xsaveopt = (eax & bit_XSAVEOPT) != 0;
    }

    if (maxExtendedLeaf >= 0x80000001 && __get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx)) {
        features.nx = (edx & CPUID_EXT_NX) != 0;
        features.longMode = (edx & bit_LM) != 0;
    }

    if (maxBasicLeaf >= 0xD && features.xsave && __get_cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx)) {
        features.xcr0SupportedMask = (static_cast<uint64_t>(edx) << 32) | eax;
    }

    if (!features.fpu || !features.fxsave || !features.sse) {
        Console::get().drawText("CPU SIMD: [ ");
        Console::get().setTextColor(0xFF0000);
        Console::get().drawText("MISSING REQUIRED FEATURES");
        Console::get().setTextColor(0xFFFFFF);
        Console::get().drawText(" ]\n");
        return false;
    }

    enableBaseFPUAndSIMD(features);
    features.writeProtectEnabled = true;

    if (features.nx) {
        enableNoExecute();
        features.nxEnabled = true;
    }

    if (features.umip) {
        enableUserModeInstructionPrevention();
        features.umipEnabled = true;
    }

    if (features.fsgsbase) {
        enableFsgsbase();
        features.fsgsbaseEnabled = true;
    }

    features.xsaveEnabled = false;
    features.avxEnabled = false;
    features.avx512Enabled = false;
    features.pkuEnabled = false;
    features.smepEnabled = false;
    features.xsaveMask = 0;
    features.osxsave = false;
    cpuUseXsave = 0;
    cpuUseXsaveOpt = 0;
    cpuXsaveMask = 0;

    if (features.xsave &&
        (features.xcr0SupportedMask & (XCR0_X87 | XCR0_SSE)) == (XCR0_X87 | XCR0_SSE)) {
        features.xsaveMask = XCR0_X87 | XCR0_SSE;

        if (features.avx && (features.xcr0SupportedMask & XCR0_AVX)) {
            features.xsaveMask |= XCR0_AVX;
        }

        if (features.avx512 &&
            (features.xsaveMask & XCR0_AVX) &&
            (features.xcr0SupportedMask & (XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM)) ==
                (XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM)) {
            features.xsaveMask |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        }

        if (features.pku && (features.xcr0SupportedMask & XCR0_PKRU)) {
            features.xsaveMask |= XCR0_PKRU;
        }

        enableXsave();
        writeXcr0(features.xsaveMask);

        if (features.xsaveopt) {
            cpuUseXsaveOpt = 1;
        }

        if (features.pku && (features.xsaveMask & XCR0_PKRU)) {
            enableProtectionKeys();
            features.pkuEnabled = true;
        }

        cpuXsaveMask = features.xsaveMask;
        cpuUseXsave = 1;
        features.xsaveEnabled = true;
        features.osxsave = true;
        features.avxEnabled = (readXcr0() & XCR0_AVX) != 0;
        features.avx512Enabled = (readXcr0() & (XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM)) ==
                                  (XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM);

        uint32_t eax, ebx, ecx, edx;
        __cpuid_count(0x0D, 0, eax, ebx, ecx, edx);
        Console::get().drawText(", XSAVE size: ");
        Console::get().drawNumber(ebx);
    }

    if (features.smep) {
        enableSmep();
        features.smepEnabled = true;
    }

    initialized = true;

    Console::get().drawText("CPU: [ ");
    Console::get().setTextColor(0x49ceee);
    Console::get().drawText("OK");
    Console::get().setTextColor(0xFFFFFF);
    Console::get().drawText(" ] ");
    printFeatureStatus("MMX", features.mmx, features.mmx);
    Console::get().drawText(", ");
    printFeatureStatus("TSC", features.tsc, features.tsc);
    Console::get().drawText(", ");
    printFeatureStatus("WP", true, features.writeProtectEnabled);
    Console::get().drawText(", ");
    printFeatureStatus("NX", features.nx, features.nxEnabled);
    Console::get().drawText(", ");
    printFeatureStatus("SSE", features.sse, features.sse);
    Console::get().drawText(", ");
    printFeatureStatus("SSE2", features.sse2, features.sse2);
    Console::get().drawText(", ");
    printFeatureStatus("SSE3", features.sse3, features.sse3);
    Console::get().drawText(", ");
    printFeatureStatus("SSSE3", features.ssse3, features.ssse3);
    Console::get().drawText(", ");
    printFeatureStatus("SSE4.1", features.sse41, features.sse41);
    Console::get().drawText(", ");
    printFeatureStatus("SSE4.2", features.sse42, features.sse42);
    Console::get().drawText(", ");
    printFeatureStatus("XSAVE", features.xsave, features.xsaveEnabled);
    Console::get().drawText(", ");
    printFeatureStatus("XSAVEOPT", features.xsaveopt, cpuUseXsaveOpt != 0);
    Console::get().drawText(", ");
    printFeatureStatus("AVX", features.avx, features.avxEnabled);
    Console::get().drawText(", ");
    printFeatureStatus("AVX2", features.avx2, features.avxEnabled && features.avx2);
    Console::get().drawText(", ");
    printFeatureStatus("AVX512", features.avx512, features.avx512Enabled);
    Console::get().drawText(", ");
    printFeatureStatus("PKU", features.pku, features.pkuEnabled);
    Console::get().drawText(", ");
    printFeatureStatus("UMIP", features.umip, features.umipEnabled);
    Console::get().drawText(", ");
    printFeatureStatus("FSGSBASE", features.fsgsbase, features.fsgsbaseEnabled);
    Console::get().drawText(", ");
    printFeatureStatus("SMEP", features.smep, features.smepEnabled);
    Console::get().drawText("\n");

    return true;
}

const CPUFeatures& CPU::getFeatures() {
    return features;
}

bool CPU::hasLegacySIMD() {
    return features.fpu && features.fxsave && features.sse;
}

bool CPU::hasAVXHardware() {
    return features.avx;
}

bool CPU::avxEnabledByOS() {
    return features.avxEnabled;
}

bool CPU::usesXsave() {
    return features.xsaveEnabled;
}

void CPU::initializeExtendedState(void* state) {
    if (!state) {
        return;
    }

    memset(state, 0, EXTENDED_STATE_BUFFER_SIZE);
    asm volatile("fninit");
    clearVectorRegisters(features.avxEnabled, features.avx512Enabled);
    if (features.pkuEnabled) {
        writePkru(0);
    }
    asm volatile("ldmxcsr %0" : : "m"(kDefaultMxcsr));
    saveExtendedState(state);
}

void CPU::saveExtendedState(void* state) {
    if (!state) {
        return;
    }

    if (features.xsaveEnabled) {
        uint32_t lowMask = static_cast<uint32_t>(features.xsaveMask);
        uint32_t highMask = static_cast<uint32_t>(features.xsaveMask >> 32);
        if (cpuUseXsaveOpt != 0) {
            asm volatile("xsaveopt (%0)"
                         :
                         : "r"(state), "a"(lowMask), "d"(highMask)
                         : "memory");
        } else {
            asm volatile("xsave (%0)"
                         :
                         : "r"(state), "a"(lowMask), "d"(highMask)
                         : "memory");
        }
        return;
    }

    if (features.fxsave) {
        asm volatile("fxsave (%0)" : : "r"(state) : "memory");
    }
}

void CPU::restoreExtendedState(const void* state) {
    if (!state) {
        return;
    }

    if (features.xsaveEnabled) {
        uint32_t lowMask = static_cast<uint32_t>(features.xsaveMask);
        uint32_t highMask = static_cast<uint32_t>(features.xsaveMask >> 32);
        asm volatile("xrstor (%0)"
                     :
                     : "r"(state), "a"(lowMask), "d"(highMask)
                     : "memory");
        return;
    }

    if (features.fxsave) {
        asm volatile("fxrstor (%0)" : : "r"(state) : "memory");
    }
}
