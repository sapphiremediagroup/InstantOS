#include <debug/diag.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <common/spinlock.hpp>
#include <graphics/console.hpp>

static constexpr uint64_t HEADER_MAGIC = 0xC001CAFEDEADBEEF;
static constexpr uint64_t FOOTER_MAGIC = 0xFEE1DEADBAADF00D;
static constexpr uint8_t ALLOC_POISON = 0xAA;
static constexpr uint8_t FREE_POISON = 0xDD;
static constexpr int MIN_ORDER = 6; 
static constexpr int MAX_ORDER = 30;

// Growable heap: the initial arena is installed by heap_init(); when an
// allocation cannot be satisfied, additional physically-contiguous arenas are
// pulled from the PMM and carved into the shared buddy free-lists. Each arena
// is a self-contained buddy region (its own base for XOR-buddy math), so a
// block only ever coalesces within the arena it came from.
static constexpr int MAX_ARENAS = 1024;                 // ceiling: 1024 * 16 MiB = 16 GiB
static constexpr size_t GROW_CHUNK = 16ULL * 1024 * 1024; // default arena growth (16 MiB)

struct HeapArena {
    uintptr_t base;
    size_t    size;
};

struct alignas(16) BlockHeader {
    uint64_t magic;
    uint16_t order;
    bool is_free;
    size_t user_size;
    uintptr_t caller;   // who allocated it (heap_dump_callers)
};

struct alignas(16) BlockFooter {
    uint64_t magic;
};

struct FreeNode {
    FreeNode* next;
    FreeNode* prev;
};


static uintptr_t heap_base_addr = 0;   // first arena base (for heap_base())
static size_t heap_total_size = 0;     // first arena size (for heap_size())
static FreeNode* free_lists[MAX_ORDER + 1] = {nullptr};
static HeapStats stats = {0, 0, 0};
static IrqSpinlock heap_lock("heap");

static HeapArena g_arenas[MAX_ARENAS];
static int g_arenaCount = 0;
// Grows refused because nothing was free above the non-PIE window (grow_locked).
static int g_lowArenas = 0;

int heap_arena_count() { return g_arenaCount; }
int heap_low_arena_count() { return g_lowArenas; }

// Locate the arena that owns `addr` (a block header address). Returns nullptr
// if the address belongs to no arena (i.e. not a heap pointer).
static HeapArena* find_arena(uintptr_t addr) {
    for (int i = 0; i < g_arenaCount; ++i) {
        if (addr >= g_arenas[i].base && addr < g_arenas[i].base + g_arenas[i].size) {
            return &g_arenas[i];
        }
    }
    return nullptr;
}

// Carve [base, base+size) into maximal aligned power-of-two free blocks and add
// them to the shared free-lists. Offsets are relative to `base` so XOR-buddy
// coalescing in kfree() (which uses the owning arena's base) stays consistent.
static void carve_region(uintptr_t base, size_t size) {
    uintptr_t current = base;
    size_t remaining = size;

    while (remaining > 0) {
        int order = MAX_ORDER;
        while (order >= MIN_ORDER) {
            size_t block_size = 1ULL << order;
            size_t offset = current - base;
            if (block_size <= remaining && (offset % block_size) == 0) {
                break;
            }
            order--;
        }
        if (order < MIN_ORDER) {
            break;
        }

        size_t block_size = 1ULL << order;
        BlockHeader* hdr = (BlockHeader*)current;
        hdr->magic = HEADER_MAGIC;
        hdr->order = order;
        hdr->is_free = true;
        hdr->user_size = 0;

        FreeNode* node = (FreeNode*)(current + sizeof(BlockHeader));
        node->next = free_lists[order];
        node->prev = nullptr;
        if (free_lists[order]) {
            free_lists[order]->prev = node;
        }
        free_lists[order] = node;

        current += block_size;
        remaining -= block_size;
        stats.free_block_count++;
    }
}

// Register a new arena and carve it. Caller holds heap_lock (except heap_init,
// which runs before any concurrency). Returns false if the arena table is full.
static bool register_arena(uintptr_t base, size_t size) {
    if (g_arenaCount >= MAX_ARENAS) {
        return false;
    }
    g_arenas[g_arenaCount].base = base;
    g_arenas[g_arenaCount].size = size;
    g_arenaCount++;
    carve_region(base, size);
    return true;
}

// Grow the heap by pulling a physically-contiguous chunk from the PMM. `need`
// is the minimum block size (bytes) the pending allocation requires. Prefers a
// GROW_CHUNK-sized arena, backing off toward `need` under fragmentation.
// Caller holds heap_lock.
static bool grow_locked(size_t need) {
    size_t chunk = GROW_CHUNK;
    if (chunk < need) {
        chunk = need;   // GROW_CHUNK and (1<<order) are both powers of two
    }

    // Arenas go above the non-PIE user-image window (PMM::KERNEL_HIGH_ALLOC_MIN):
    // heap memory is reached through the low identity map, which a low ET_EXEC
    // image (cc1, at 0x400000) shadows while it runs. Every size down to `need`
    // is tried up there first; after hours of a build no 16 MiB run is left
    // above, and the old fallback to "anywhere" could land in the window.
    for (size_t c = chunk; c >= need && c >= PMM::PAGE_SIZE; c >>= 1) {
        uint64_t phys = PMM::AllocFramesAbove(c / PMM::PAGE_SIZE, PMM::KERNEL_HIGH_ALLOC_MIN);
        if (phys) {
            return register_arena((uintptr_t)phys, c);
        }
        if (c == need) {
            break;
        }
    }
    // Never inside the window itself: a heap block there is the running
    // program's own memory (cc1's code read as a free-list pointer, P7.1).
    // Failing the allocation is better than that. Below 0x400000 is fine.
    for (size_t c = chunk; c >= need && c >= PMM::PAGE_SIZE; c >>= 1) {
        uint64_t phys = PMM::AllocFramesBelow(c / PMM::PAGE_SIZE, 0x400000ULL);
        if (phys) {
            return register_arena((uintptr_t)phys, c);
        }
        if (c == need) {
            break;
        }
    }
    g_lowArenas++;   // counts refused grows (heap_low_arena_count)
    return false;
}

static void poisonRange(void* ptr, size_t size, uint8_t value) {
    if (!ptr || size == 0) {
        return;
    }

    uint8_t* bytes = reinterpret_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = value;
    }
}

static inline void kernel_panic(const char* reason) {
    Debug::panic(reason);
}

void heap_init(void* base, size_t size) {
    heap_base_addr = (uintptr_t)base;
    heap_total_size = size;

    for (int i = 0; i <= MAX_ORDER; ++i) {
        free_lists[i] = nullptr;
    }
    stats.total_allocated = 0;
    stats.peak_usage = 0;
    stats.free_block_count = 0;
    g_arenaCount = 0;

    register_arena((uintptr_t)base, size);
}

// Satisfy an allocation of the given order from the free-lists. Caller holds
// heap_lock. Returns the user pointer, or nullptr if no block is available.
static void* alloc_locked(int order, size_t size, size_t align, uintptr_t caller) {
    int found_order = order;
    while (found_order <= MAX_ORDER && !free_lists[found_order]) {
        found_order++;
    }

    if (found_order > MAX_ORDER) {
        return nullptr;
    }

    FreeNode* node = free_lists[found_order];
    free_lists[found_order] = node->next;
    if (free_lists[found_order]) free_lists[found_order]->prev = nullptr;
    stats.free_block_count--;

    uintptr_t current = (uintptr_t)node - sizeof(BlockHeader);

    while (found_order > order) {
        found_order--;
        size_t half_size = 1ULL << found_order;
        uintptr_t buddy_addr = current + half_size;

        BlockHeader* buddy = (BlockHeader*)buddy_addr;
        buddy->magic = HEADER_MAGIC;
        buddy->order = found_order;
        buddy->is_free = true;
        buddy->user_size = 0;

        FreeNode* buddy_node = (FreeNode*)(buddy_addr + sizeof(BlockHeader));
        buddy_node->next = free_lists[found_order];
        buddy_node->prev = nullptr;
        if (free_lists[found_order]) free_lists[found_order]->prev = buddy_node;
        free_lists[found_order] = buddy_node;

        stats.free_block_count++;
    }

    BlockHeader* hdr = (BlockHeader*)current;
    hdr->magic = HEADER_MAGIC;
    hdr->order = order;
    hdr->is_free = false;
    hdr->user_size = size;
    hdr->caller = caller;

    size_t block_size = 1ULL << order;
    BlockFooter* ftr = (BlockFooter*)(current + block_size - sizeof(BlockFooter));
    ftr->magic = FOOTER_MAGIC;

    stats.total_allocated += size;
    if (stats.total_allocated > stats.peak_usage) {
        stats.peak_usage = stats.total_allocated;
    }

    uintptr_t base_p = current + sizeof(BlockHeader) + sizeof(BlockHeader*);
    uintptr_t p = (base_p + align - 1) & ~(align - 1);

    *((BlockHeader**)(p - sizeof(BlockHeader*))) = hdr;
    poisonRange(reinterpret_cast<void*>(p), size, ALLOC_POISON);

    return (void*)p;
}

static void* kmalloc_from(size_t size, size_t align, uintptr_t caller) {
    if (size == 0) return nullptr;
    if (align < 16) align = 16;
    
    if ((align & (align - 1)) != 0) {
        size_t p = 1;
        while (p < align) p *= 2;
        align = p;
    }

    size_t min_required = sizeof(BlockHeader) + sizeof(BlockHeader*) + size + sizeof(BlockFooter) + align - 1;
    
    int order = MIN_ORDER;
    while (order <= MAX_ORDER && (1ULL << order) < min_required) {
        order++;
    }

    if (order > MAX_ORDER) return nullptr;

    const uint64_t heapFlags = heap_lock.lock();

    void* result = alloc_locked(order, size, align, caller);
    if (!result) {
        // Out of free blocks at/above this order: grow the heap from the PMM
        // and retry once.
        if (grow_locked(1ULL << order)) {
            result = alloc_locked(order, size, align, caller);
        }
    }

    heap_lock.unlock(heapFlags);
    return result;
}

#define CALLER reinterpret_cast<uintptr_t>(__builtin_return_address(0))

void* kmalloc_aligned(size_t size, size_t align) {
    return kmalloc_from(size, align, CALLER);
}

void* kmalloc(size_t size) {
    return kmalloc_from(size, 16, CALLER);
}

void kfree(void* ptr) {
    if (!ptr) return;

    BlockHeader* hdr = *((BlockHeader**)((uintptr_t)ptr - sizeof(BlockHeader*)));

    if (hdr->magic != HEADER_MAGIC || hdr->is_free) {
        kernel_panic("heap free header corruption");
    }

    uintptr_t hdr_val = (uintptr_t)hdr;
    HeapArena* arena = find_arena(hdr_val);
    if (!arena) {
        kernel_panic("heap free pointer out of range");
    }
    const uintptr_t arena_base = arena->base;
    const size_t arena_size = arena->size;

    size_t block_size = 1ULL << hdr->order;
    BlockFooter* ftr = (BlockFooter*)((uintptr_t)hdr + block_size - sizeof(BlockFooter));
    if (ftr->magic != FOOTER_MAGIC) {
        kernel_panic("heap footer corruption");
    }

    const uint64_t heapFlags = heap_lock.lock();
    
    stats.total_allocated -= hdr->user_size;
    poisonRange(ptr, hdr->user_size, FREE_POISON);
    hdr->user_size = 0;
    
    uintptr_t current = (uintptr_t)hdr;
    int order = hdr->order;

    while (order < MAX_ORDER) {
        size_t current_block_size = 1ULL << order;
        uintptr_t offset = current - arena_base;
        uintptr_t buddy_offset = offset ^ current_block_size;
        uintptr_t buddy_addr = arena_base + buddy_offset;

        if (buddy_addr + current_block_size > arena_base + arena_size) {
            break;
        }

        BlockHeader* buddy = (BlockHeader*)buddy_addr;
        if (buddy->magic == HEADER_MAGIC && buddy->is_free && buddy->order == order) {
            FreeNode* buddy_node = (FreeNode*)(buddy_addr + sizeof(BlockHeader));
            if (buddy_node->prev) buddy_node->prev->next = buddy_node->next;
            else free_lists[order] = buddy_node->next;
            if (buddy_node->next) buddy_node->next->prev = buddy_node->prev;

            stats.free_block_count--;

            if (buddy_addr < current) {
                current = buddy_addr;
            }
            order++;
        } else {
            break;
        }
    }

    BlockHeader* new_hdr = (BlockHeader*)current;
    new_hdr->magic = HEADER_MAGIC;
    new_hdr->order = order;
    new_hdr->is_free = true;
    new_hdr->user_size = 0;

    FreeNode* node = (FreeNode*)(current + sizeof(BlockHeader));
    node->next = free_lists[order];
    node->prev = nullptr;
    if (free_lists[order]) free_lists[order]->prev = node;
    free_lists[order] = node;

    stats.free_block_count++;

    heap_lock.unlock(heapFlags);
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return nullptr;
    }

    BlockHeader* hdr = *((BlockHeader**)((uintptr_t)ptr - sizeof(BlockHeader*)));
    if (hdr->magic != HEADER_MAGIC || hdr->is_free) {
        kernel_panic("heap realloc header corruption");
    }

    size_t old_size = hdr->user_size;
    if (new_size <= old_size) {
        const uint64_t heapFlags = heap_lock.lock();
        stats.total_allocated -= old_size;
        stats.total_allocated += new_size;
        hdr->user_size = new_size;
        heap_lock.unlock(heapFlags);
        return ptr;
    }

    uintptr_t block_end = (uintptr_t)hdr + (1ULL << hdr->order) - sizeof(BlockFooter);
    size_t capacity = block_end - (uintptr_t)ptr;

    if (new_size <= capacity) {
        const uint64_t heapFlags = heap_lock.lock();
        stats.total_allocated += (new_size - old_size);
        hdr->user_size = new_size;
        if (stats.total_allocated > stats.peak_usage) {
            stats.peak_usage = stats.total_allocated;
        }
        heap_lock.unlock(heapFlags);
        return ptr;
    }

    void* new_ptr = kmalloc_from(new_size, 16, CALLER);
    if (!new_ptr) return nullptr;

    const char* src = (const char*)ptr;
    char* dst = (char*)new_ptr;
    for (size_t i = 0; i < old_size; ++i) {
        dst[i] = src[i];
    }

    kfree(ptr);
    return new_ptr;
}

HeapStats heap_stats() {
    const uint64_t heapFlags = heap_lock.lock();
    HeapStats s = stats;
    heap_lock.unlock(heapFlags);
    return s;
}

bool heap_is_initialized() {
    return heap_base_addr != 0 && heap_total_size != 0;
}

uintptr_t heap_base() {
    return heap_base_addr;
}

size_t heap_size() {
    return heap_total_size;
}

// True if `ptr` falls within any heap arena (initial or grown). Used by the VMM
// to distinguish heap-backed page tables from PMM frames during teardown.
bool heap_contains(const void* ptr) {
    return find_arena(reinterpret_cast<uintptr_t>(ptr)) != nullptr;
}

void* operator new(size_t size) {
    return kmalloc_from(size, 16, CALLER);
}

void* operator new[](size_t size) {
    return kmalloc_from(size, 16, CALLER);
}

// Live blocks by allocating caller (leak hunting): walks every arena block by
// block and prints the callers holding the most blocks, as kernel.map
// addresses. Run it twice apart; a leak is a caller whose count keeps rising.
namespace {
struct CallerTally {
    uintptr_t caller;
    uint32_t count;
    uint64_t bytes;
};
constexpr int kTallySlots = 1024;
CallerTally g_tally[kTallySlots];
}

extern "C" char __ImageBase;

void heap_dump_callers(int top) {
    for (int i = 0; i < kTallySlots; ++i) g_tally[i] = {0, 0, 0};
    uint32_t dropped = 0;
    const uint64_t heapFlags = heap_lock.lock();
    for (int a = 0; a < g_arenaCount; ++a) {
        uintptr_t cur = g_arenas[a].base;
        const uintptr_t end = cur + g_arenas[a].size;
        while (cur + sizeof(BlockHeader) <= end) {
            const BlockHeader* hdr = reinterpret_cast<const BlockHeader*>(cur);
            if (hdr->magic != HEADER_MAGIC || hdr->order < MIN_ORDER || hdr->order > MAX_ORDER) break;
            if (!hdr->is_free) {
                uint32_t h = static_cast<uint32_t>((hdr->caller >> 2) * 2654435761u) % kTallySlots;
                int probes = 0;
                while (g_tally[h].count && g_tally[h].caller != hdr->caller && probes < kTallySlots) {
                    h = (h + 1) % kTallySlots;
                    ++probes;
                }
                if (probes == kTallySlots) {
                    ++dropped;
                } else {
                    g_tally[h].caller = hdr->caller;
                    g_tally[h].count++;
                    g_tally[h].bytes += 1ULL << hdr->order;
                }
            }
            cur += 1ULL << hdr->order;
        }
    }
    heap_lock.unlock(heapFlags);

    const uint64_t base = reinterpret_cast<uint64_t>(&__ImageBase);
    for (int n = 0; n < top; ++n) {
        int best = -1;
        for (int i = 0; i < kTallySlots; ++i) {
            if (g_tally[i].count && (best < 0 || g_tally[i].count > g_tally[best].count)) best = i;
        }
        if (best < 0) break;
        Console::get().drawText("[heap] caller=");
        Console::get().drawHex(g_tally[best].caller - base + 0x140000000ULL);
        Console::get().drawText(" blocks=");
        Console::get().drawNumber(g_tally[best].count);
        Console::get().drawText(" KiB=");
        Console::get().drawNumber(static_cast<int64_t>(g_tally[best].bytes >> 10));
        Console::get().drawText("\n");
        g_tally[best].count = 0;
    }
    if (dropped) {
        Console::get().drawText("[heap] tally full, dropped=");
        Console::get().drawNumber(dropped);
        Console::get().drawText("\n");
    }
}

void operator delete(void* ptr) {
    kfree(ptr);
}

void operator delete[](void* ptr) {
    kfree(ptr);
}

void operator delete(void* ptr, size_t size) {
    kfree(ptr);
}

void operator delete[](void* ptr, size_t size) {
    kfree(ptr);
}
