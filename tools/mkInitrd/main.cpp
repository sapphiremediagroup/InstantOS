#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

static constexpr uint32_t INITRD_MAGIC = 0x44524E49;
static constexpr size_t NAME_LEN = 64;

struct InitrdFile {
    char     name[NAME_LEN];
    uint64_t offset;
    uint64_t size;
};

struct InitrdHeader {
    uint32_t magic;
    uint32_t fileCount;
};

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mkInitrd <output> <name:file> [<name:file> ...]\n");
        return 1;
    }

    const char* outPath = argv[1];
    int fileCount = argc - 2;

    struct Entry {
        std::string name;
        std::vector<uint8_t> data;
    };

    std::vector<Entry> entries;
    entries.reserve(fileCount);

    for (int i = 0; i < fileCount; i++) {
        const char* arg = argv[2 + i];
        const char* colon = strchr(arg, ':');
        if (!colon) {
            fprintf(stderr, "error: expected <name:file>, got '%s'\n", arg);
            return 1;
        }

        std::string name(arg, colon - arg);
        const char* filePath = colon + 1;

        if (name.size() >= NAME_LEN) {
            fprintf(stderr, "error: name '%s' exceeds %zu characters\n", name.c_str(), NAME_LEN - 1);
            return 1;
        }

        FILE* f = fopen(filePath, "rb");
        if (!f) {
            fprintf(stderr, "error: cannot open '%s'\n", filePath);
            return 1;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);

        Entry e;
        e.name = name;
        e.data.resize(sz);
        if (fread(e.data.data(), 1, sz, f) != (size_t)sz) {
            fprintf(stderr, "error: failed to read '%s'\n", filePath);
            fclose(f);
            return 1;
        }
        fclose(f);

        entries.push_back(std::move(e));
    }

    FILE* out = fopen(outPath, "wb");
    if (!out) {
        fprintf(stderr, "error: cannot open output '%s'\n", outPath);
        return 1;
    }

    InitrdHeader header;
    header.magic = INITRD_MAGIC;
    header.fileCount = (uint32_t)fileCount;
    fwrite(&header, sizeof(header), 1, out);

    uint64_t dataOffset = sizeof(InitrdHeader) + sizeof(InitrdFile) * fileCount;
    uint64_t cursor = 0;

    for (auto& e : entries) {
        InitrdFile rec{};
        strncpy(rec.name, e.name.c_str(), NAME_LEN - 1);
        rec.offset = dataOffset + cursor;
        rec.size   = e.data.size();
        fwrite(&rec, sizeof(rec), 1, out);
        cursor += rec.size;
    }

    for (auto& e : entries) {
        fwrite(e.data.data(), 1, e.data.size(), out);
    }

    fclose(out);
    return 0;
}
