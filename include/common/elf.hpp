#pragma once

#include <stdint.h>

namespace Elf {
static constexpr uint8_t Magic0 = 0x7F;
static constexpr uint8_t Magic1 = 'E';
static constexpr uint8_t Magic2 = 'L';
static constexpr uint8_t Magic3 = 'F';

static constexpr uint8_t Class64 = 2;
static constexpr uint8_t DataLittleEndian = 1;
static constexpr uint8_t VersionCurrent = 1;
static constexpr uint8_t AbiSystemV = 0;

static constexpr uint16_t TypeExecutable = 2;
static constexpr uint16_t TypeDynamic = 3;
static constexpr uint16_t MachineX86_64 = 0x3E;

static constexpr uint32_t ProgramLoad = 1;
static constexpr uint32_t ProgramDynamic = 2;
static constexpr uint32_t ProgramInterp = 3;
static constexpr uint32_t ProgramPhdr = 6;

static constexpr uint32_t FlagExecute = 1;
static constexpr uint32_t FlagWrite = 2;
static constexpr uint32_t FlagRead = 4;

static constexpr uint64_t DynamicNull = 0;
static constexpr uint64_t DynamicNeeded = 1;
static constexpr uint64_t DynamicPltRelSize = 2;
static constexpr uint64_t DynamicPltGot = 3;
static constexpr uint64_t DynamicHash = 4;
static constexpr uint64_t DynamicStrTab = 5;
static constexpr uint64_t DynamicSymTab = 6;
static constexpr uint64_t DynamicRela = 7;
static constexpr uint64_t DynamicRelaSize = 8;
static constexpr uint64_t DynamicRelaEnt = 9;
static constexpr uint64_t DynamicStrSize = 10;
static constexpr uint64_t DynamicSymEnt = 11;
static constexpr uint64_t DynamicInit = 12;
static constexpr uint64_t DynamicFini = 13;
static constexpr uint64_t DynamicSoname = 14;
static constexpr uint64_t DynamicRel = 17;
static constexpr uint64_t DynamicRelSize = 18;
static constexpr uint64_t DynamicRelEnt = 19;
static constexpr uint64_t DynamicJmpRel = 23;
static constexpr uint64_t DynamicBindNow = 24;
static constexpr uint64_t DynamicInitArray = 25;
static constexpr uint64_t DynamicFiniArray = 26;
static constexpr uint64_t DynamicInitArraySize = 27;
static constexpr uint64_t DynamicFlags = 30;
static constexpr uint64_t DynamicFlags1 = 0x6FFFFFFB;
static constexpr uint64_t DynamicRelACount = 0x6FFFFFF9;
static constexpr uint64_t DynamicFlags1Now = 0x1;

static constexpr uint32_t RelocationX86_64None = 0;
static constexpr uint32_t RelocationX86_64_64 = 1;
static constexpr uint32_t RelocationX86_64GlobDat = 6;
static constexpr uint32_t RelocationX86_64JumpSlot = 7;
static constexpr uint32_t RelocationX86_64Relative = 8;

static constexpr uint64_t AuxNull = 0;
static constexpr uint64_t AuxPhdr = 3;
static constexpr uint64_t AuxPhent = 4;
static constexpr uint64_t AuxPhnum = 5;
static constexpr uint64_t AuxPagesz = 6;
static constexpr uint64_t AuxBase = 7;
static constexpr uint64_t AuxEntry = 9;
static constexpr uint64_t AuxUid = 11;
static constexpr uint64_t AuxEuid = 12;
static constexpr uint64_t AuxGid = 13;
static constexpr uint64_t AuxEgid = 14;
static constexpr uint64_t AuxExecFn = 31;

struct Header64 {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t programHeaderOffset;
    uint64_t sectionHeaderOffset;
    uint32_t flags;
    uint16_t headerSize;
    uint16_t programHeaderEntrySize;
    uint16_t programHeaderCount;
    uint16_t sectionHeaderEntrySize;
    uint16_t sectionHeaderCount;
    uint16_t sectionNameIndex;
} __attribute__((packed));

struct ProgramHeader64 {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtualAddress;
    uint64_t physicalAddress;
    uint64_t fileSize;
    uint64_t memorySize;
    uint64_t align;
} __attribute__((packed));

struct DynamicEntry64 {
    int64_t tag;
    union {
        uint64_t value;
        uint64_t pointer;
    };
} __attribute__((packed));

struct Symbol64 {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t sectionIndex;
    uint64_t value;
    uint64_t size;
} __attribute__((packed));

struct Rela64 {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
} __attribute__((packed));

struct AuxEntry64 {
    uint64_t type;
    uint64_t value;
} __attribute__((packed));

inline uint32_t relocationType(uint64_t info) {
    return static_cast<uint32_t>(info & 0xFFFFFFFFULL);
}

inline uint32_t relocationSymbol(uint64_t info) {
    return static_cast<uint32_t>(info >> 32);
}

inline uint8_t symbolBinding(uint8_t info) {
    return info >> 4;
}
}
