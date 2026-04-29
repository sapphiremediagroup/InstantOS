#include <stdint.h>


extern "C" void jumpToUsermode(uint64_t entry, uint64_t stack);

enum class SegmentSelectors : uint16_t {
    KernelCode = 0x08,
    KernelData = 0x10,
    UserData   = 0x18,
    UserCode   = 0x20,
    TaskState  = 0x28
};

struct GDTEntry {
    uint16_t limitLow;
    uint16_t baseLow;
    uint8_t baseMiddle;
    uint8_t access;
    uint8_t granularity;
    uint8_t baseHigh;
} __attribute__((packed));

struct GDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct TSS {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
} __attribute__((packed));

struct TSSEntry {
    uint16_t limitLow;
    uint16_t baseLow;
    uint8_t  baseMiddle1;
    uint8_t  access;
    uint8_t  limitHigh : 4;
    uint8_t  flags : 4;
    uint8_t  baseMiddle2;

    uint32_t baseHigh;
    uint32_t reserved;
} __attribute__((packed));

class GDT {
private:
    GDTEntry gdt[8];
    GDTPointer gdtp;
    TSS tss;
public:
    static GDT& get();
    void initialize();

    void setTSS(int index, uint64_t base, uint32_t limit);
    void setGate64(int num, uint8_t access, uint8_t gran);
    void setGate32(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran);
    
    void setKernelStack(uint64_t stack);
    TSS* getTSS() { return &tss; }

    ~GDT();
};