#include <boot/boot_config.hpp>

#include <iboot/memory.hpp>

namespace {
char gConfig[1024];

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
}  // namespace

void BootConfig::initialize(const BootInfo* bootInfo) {
    gConfig[0] = 0;
    if (!bootInfo || bootInfo->configMagic != kBootConfigMagic) {
        return;
    }
    size_t i = 0;
    for (; i + 1 < sizeof(gConfig) && i < sizeof(bootInfo->config) && bootInfo->config[i] != 0; ++i) {
        const char c = bootInfo->config[i];
        gConfig[i] = (c >= 0x20 && c <= 0x7E) ? c : ' ';
    }
    gConfig[i] = 0;
}

const char* BootConfig::text() {
    return gConfig;
}

bool BootConfig::get(const char* key, char* out, size_t size) {
    if (!key || !out || size == 0) {
        return false;
    }
    size_t keyLength = 0;
    while (key[keyLength] != 0) {
        ++keyLength;
    }
    bool found = false;
    for (size_t i = 0; gConfig[i] != 0;) {
        while (isSpace(gConfig[i])) {
            ++i;
        }
        const size_t word = i;
        while (gConfig[i] != 0 && !isSpace(gConfig[i])) {
            ++i;
        }
        bool match = i - word > keyLength && gConfig[word + keyLength] == '=';
        for (size_t k = 0; match && k < keyLength; ++k) {
            match = gConfig[word + k] == key[k];
        }
        if (match) {
            // The last one wins.
            size_t n = 0;
            for (size_t v = word + keyLength + 1; v < i && n + 1 < size; ++v) {
                out[n++] = gConfig[v];
            }
            out[n] = 0;
            found = true;
        }
    }
    return found;
}
