#pragma once

#include <stddef.h>
#include <stdint.h>

struct BootInfo;

// iboot's settings for the kernel (BootInfo::config): words of key=value, from
// \EFI\InstantOS\iboot.cfg and then iboot's load options (a Limine entry's
// cmdline), so a later word overrides an earlier one. Keys so far:
//   root=<GPT partition GUID>   the partition to mount as / (it must still be an
//                               InstantOS partition: the GPT rule in
//                               partition.hpp keeps other systems' alone)
class BootConfig {
public:
    // Copies the settings out of BootInfo (an older iboot sends none).
    static void initialize(const BootInfo* bootInfo);

    // The value of the last `key=` word, copied into out; false if absent.
    static bool get(const char* key, char* out, size_t size);

    // Everything iboot passed, for the log ("" when nothing).
    static const char* text();
};
