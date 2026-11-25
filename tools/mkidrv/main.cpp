// mkidrv: turns a driver linked as a PE32+ DLL (lld-link /DLL, no imports) into
// an InstantOS .idrv module (include/idrv/idrv.h, docs/idrv.md).
//
//   mkidrv driver.dll driver.idrv     convert
//   mkidrv --info driver.idrv         print what a module holds
//
// Host tool, plain C++17.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define IDRV_BUILDING_HOST_TOOL 1
#include "../../include/idrv/idrv.h"

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "mkidrv: %s\n", message.c_str());
    std::exit(1);
}

std::vector<uint8_t> readFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        fail(std::string("cannot open ") + path);
    }
    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        data.insert(data.end(), chunk, chunk + n);
    }
    std::fclose(f);
    return data;
}

uint16_t rd16(const std::vector<uint8_t>& d, size_t at) {
    if (at + 2 > d.size()) fail("truncated file");
    return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}
uint32_t rd32(const std::vector<uint8_t>& d, size_t at) {
    if (at + 4 > d.size()) fail("truncated file");
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}
uint64_t rd64(const std::vector<uint8_t>& d, size_t at) {
    return rd32(d, at) | (static_cast<uint64_t>(rd32(d, at + 4)) << 32);
}

uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

struct Section {
    std::string name;
    uint32_t virtualSize, virtualAddress, rawSize, rawOffset, characteristics;
};

constexpr uint32_t SCN_DISCARDABLE = 0x02000000u;

std::string cString(const std::vector<uint8_t>& image, uint32_t rva) {
    std::string s;
    while (rva < image.size() && image[rva] != 0) {
        s += static_cast<char>(image[rva++]);
    }
    return s;
}

int convert(const char* inPath, const char* outPath) {
    const std::vector<uint8_t> pe = readFile(inPath);
    if (rd16(pe, 0) != 0x5A4D) fail("not a PE file (no MZ)");
    const uint32_t peOff = rd32(pe, 0x3C);
    if (rd32(pe, peOff) != 0x00004550) fail("not a PE file (no PE signature)");
    const size_t coff = peOff + 4;
    if (rd16(pe, coff) != 0x8664) fail("not an x86-64 image");
    const uint16_t sectionCount = rd16(pe, coff + 2);
    const uint16_t optSize = rd16(pe, coff + 16);
    const size_t opt = coff + 20;
    if (rd16(pe, opt) != 0x20B) fail("not PE32+");
    const uint64_t imageBase = rd64(pe, opt + 24);
    const uint32_t sizeOfImage = rd32(pe, opt + 56);
    const uint32_t dirCount = rd32(pe, opt + 108);
    auto dir = [&](uint32_t i, uint32_t& rva, uint32_t& size) {
        rva = size = 0;
        if (i < dirCount) {
            rva = rd32(pe, opt + 112 + i * 8);
            size = rd32(pe, opt + 112 + i * 8 + 4);
        }
    };

    std::vector<Section> sections;
    const size_t sectionTable = opt + optSize;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t at = sectionTable + i * 40u;
        Section s;
        char name[9] = {};
        for (int c = 0; c < 8; ++c) name[c] = static_cast<char>(pe.at(at + c));
        s.name = name;
        s.virtualSize = rd32(pe, at + 8);
        s.virtualAddress = rd32(pe, at + 12);
        s.rawSize = rd32(pe, at + 16);
        s.rawOffset = rd32(pe, at + 20);
        s.characteristics = rd32(pe, at + 36);
        sections.push_back(s);
    }

    // The image as it lies in memory (headers aside).
    std::vector<uint8_t> image(sizeOfImage, 0);
    uint32_t fileSize = 0;
    uint32_t textRva = 0, textSize = 0;
    for (const Section& s : sections) {
        const uint32_t copy = s.rawSize < s.virtualSize ? s.rawSize : s.virtualSize;
        if (static_cast<uint64_t>(s.virtualAddress) + s.virtualSize > sizeOfImage) {
            fail("section " + s.name + " lies outside the image");
        }
        if (s.characteristics & SCN_DISCARDABLE) {
            continue;   // .reloc
        }
        if (copy) {
            if (static_cast<uint64_t>(s.rawOffset) + copy > pe.size()) fail("section " + s.name + " truncated");
            std::memcpy(&image[s.virtualAddress], &pe[s.rawOffset], copy);
            if (s.virtualAddress + copy > fileSize) fileSize = s.virtualAddress + copy;
        }
        if (s.name == ".text") {
            textRva = s.virtualAddress;
            textSize = s.virtualSize;
        }
    }
    // Copy the images' needed reads (export/import tables) from the raw
    // sections even when discardable, through a full mapping.
    std::vector<uint8_t> mapped(sizeOfImage, 0);
    for (const Section& s : sections) {
        const uint32_t copy = s.rawSize < s.virtualSize ? s.rawSize : s.virtualSize;
        if (copy) std::memcpy(&mapped[s.virtualAddress], &pe[s.rawOffset], copy);
    }

    // No imports: a module reaches the kernel only through IdrvKernelApi.
    uint32_t importRva, importSize;
    dir(1, importRva, importSize);
    if (importSize) {
        std::string names;
        for (uint32_t at = importRva; at + 20 <= mapped.size(); at += 20) {
            const uint32_t nameRva = rd32(mapped, at + 12);
            if (nameRva == 0) break;
            names += " " + cString(mapped, nameRva);
        }
        fail("the DLL imports from" + names +
             ": a driver may only use the kernel through IdrvKernelApi (and its own runtime)");
    }

    // The descriptor, by its export.
    uint32_t exportRva, exportSize;
    dir(0, exportRva, exportSize);
    if (!exportSize) fail("the DLL exports nothing: it needs IDRV_EXPORT const IdrvModule idrv_module");
    const uint32_t nameCount = rd32(mapped, exportRva + 24);
    const uint32_t functions = rd32(mapped, exportRva + 28);
    const uint32_t names = rd32(mapped, exportRva + 32);
    const uint32_t ordinals = rd32(mapped, exportRva + 36);
    uint32_t moduleRva = 0;
    for (uint32_t i = 0; i < nameCount; ++i) {
        if (cString(mapped, rd32(mapped, names + i * 4)) == "idrv_module") {
            moduleRva = rd32(mapped, functions + rd16(mapped, ordinals + i * 2) * 4u);
        }
    }
    if (!moduleRva) fail("no idrv_module export");
    if (moduleRva + sizeof(IdrvModule) > fileSize || rd32(image, moduleRva) != IDRV_MODULE_MAGIC) {
        fail("idrv_module does not start with IDRV_MODULE_MAGIC");
    }
    if (rd32(image, moduleRva + 4) != IDRV_API_VERSION) fail("idrv_module.apiVersion is not IDRV_API_VERSION");

    // Base relocations: only 64-bit absolute pointers occur on x86-64.
    std::vector<uint32_t> relocs;
    uint32_t relocRva, relocSize;
    dir(5, relocRva, relocSize);
    for (uint32_t at = relocRva; at + 8 <= relocRva + relocSize;) {
        const uint32_t page = rd32(mapped, at);
        const uint32_t blockSize = rd32(mapped, at + 4);
        if (blockSize < 8) break;
        for (uint32_t e = at + 8; e + 2 <= at + blockSize; e += 2) {
            const uint16_t entry = rd16(mapped, e);
            const uint16_t type = entry >> 12;
            if (type == 0) continue;   // padding
            if (type != 10) fail("unsupported base relocation type " + std::to_string(type));
            const uint32_t rva = page + (entry & 0xFFF);
            if (rva + 8 > fileSize) fail("a relocation points outside the stored image");
            relocs.push_back(rva);
        }
        at += blockSize;
    }

    IdrvFileHeader header = {};
    header.magic = IDRV_FILE_MAGIC;
    header.formatVersion = IDRV_FORMAT_VERSION;
    header.headerSize = sizeof(IdrvFileHeader);
    header.apiVersion = IDRV_API_VERSION;
    header.linkedBase = imageBase;
    header.imageSize = (sizeOfImage + 4095u) & ~4095u;
    header.imageFileSize = fileSize;
    header.relocCount = static_cast<uint32_t>(relocs.size());
    header.relocOffset = sizeof(IdrvFileHeader);
    header.imageOffset = (header.relocOffset + header.relocCount * 4u + 15u) & ~15u;
    header.moduleRva = moduleRva;
    header.textRva = textRva;
    header.textSize = textSize;

    std::vector<uint8_t> out(header.imageOffset + fileSize, 0);
    for (size_t i = 0; i < relocs.size(); ++i) {
        std::memcpy(&out[header.relocOffset + i * 4], &relocs[i], 4);
    }
    std::memcpy(&out[header.imageOffset], image.data(), fileSize);
    header.crc32 = crc32(out.data() + sizeof(header), out.size() - sizeof(header));
    std::memcpy(out.data(), &header, sizeof(header));

    FILE* f = std::fopen(outPath, "wb");
    if (!f || std::fwrite(out.data(), 1, out.size(), f) != out.size() || std::fclose(f) != 0) {
        fail(std::string("cannot write ") + outPath);
    }
    std::printf("mkidrv: %s: %u bytes, image %u KiB, %zu relocations\n", outPath,
                static_cast<unsigned>(out.size()), header.imageSize / 1024, relocs.size());
    return 0;
}

int info(const char* path) {
    const std::vector<uint8_t> file = readFile(path);
    if (file.size() < sizeof(IdrvFileHeader)) fail("too short");
    IdrvFileHeader h;
    std::memcpy(&h, file.data(), sizeof(h));
    if (h.magic != IDRV_FILE_MAGIC) fail("not an .idrv file");
    const uint32_t crc = crc32(file.data() + sizeof(h), file.size() - sizeof(h));
    std::printf("format %u, api %u, base 0x%llx, image %u bytes (%u stored), %u relocations, crc %s\n",
                h.formatVersion, h.apiVersion, static_cast<unsigned long long>(h.linkedBase), h.imageSize,
                h.imageFileSize, h.relocCount, crc == h.crc32 ? "ok" : "BAD");
    std::vector<uint8_t> image(file.begin() + h.imageOffset, file.begin() + h.imageOffset + h.imageFileSize);
    auto str = [&](uint32_t at) {
        const uint64_t p = rd64(image, at);
        return p ? cString(image, static_cast<uint32_t>(p - h.linkedBase)) : std::string("(none)");
    };
    const uint32_t m = h.moduleRva;
    std::printf("module %s %s: %s (%s)\n", str(m + 8).c_str(), str(m + 16).c_str(), str(m + 24).c_str(),
                str(m + 32).c_str());
    const uint64_t matches = rd64(image, m + 40);
    const uint32_t count = rd32(image, m + 48);
    for (uint32_t i = 0; i < count && matches; ++i) {
        const uint32_t at = static_cast<uint32_t>(matches - h.linkedBase) + i * 12;
        std::printf("  pci %04x:%04x class %06x/%06x\n", rd16(image, at), rd16(image, at + 2),
                    rd32(image, at + 8), rd32(image, at + 4));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--info") == 0) {
        return info(argv[2]);
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: mkidrv driver.dll driver.idrv | mkidrv --info driver.idrv\n");
        return 2;
    }
    return convert(argv[1], argv[2]);
}
