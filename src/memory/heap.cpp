#include <debug/diag.hpp>
#include <memory/heap.hpp>

static constexpr uint64_t HEADER_MAGIC = 0xC001CAFEDEADBEEF;
static constexpr uint64_t FOOTER_MAGIC = 0xFEE1DEADBAADF00D;
static constexpr uint8_t ALLOC_POISON = 0xAA;
static constexpr uint8_t FREE_POISON = 0xDD;
static constexpr int MIN_ORDER = 6; 
static constexpr int MAX_ORDER = 30;

struct alignas(16) BlockHeader {
    uint64_t magic;
    uint16_t order;
    bool is_free;
    size_t user_size;
};

struct alignas(16) BlockFooter {
    uint64_t magic;
};

struct FreeNode {
    FreeNode* next;
    FreeNode* prev;
};

struct Spinlock {
    int locked = 0;
    void lock() {
        while (__sync_lock_test_and_set(&locked, 1)) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    void unlock() {
        __sync_lock_release(&locked);
    }
};

static uintptr_t heap_base_addr = 0;
static size_t heap_total_size = 0;
static FreeNode* free_lists[MAX_ORDER + 1] = {nullptr};
static HeapStats stats = {0, 0, 0};
static Spinlock heap_lock;

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

    uintptr_t current = heap_base_addr;
    size_t remaining = size;

    while (remaining > 0) {
        int order = MAX_ORDER;
        while (order >= MIN_ORDER) {
            size_t block_size = 1ULL << order;
            size_t offset = current - heap_base_addr;
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

void* kmalloc_aligned(size_t size, size_t align) {
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

    heap_lock.lock();

    int found_order = order;
    while (found_order <= MAX_ORDER && !free_lists[found_order]) {
        found_order++;
    }

    if (found_order > MAX_ORDER) {
        heap_lock.unlock();
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

    size_t block_size = 1ULL << order;
    BlockFooter* ftr = (BlockFooter*)(current + block_size - sizeof(BlockFooter));
    ftr->magic = FOOTER_MAGIC;

    stats.total_allocated += size;
    if (stats.total_allocated > stats.peak_usage) {
        stats.peak_usage = stats.total_allocated;
    }

    heap_lock.unlock();

    uintptr_t base_p = current + sizeof(BlockHeader) + sizeof(BlockHeader*);
    uintptr_t p = (base_p + align - 1) & ~(align - 1);

    *((BlockHeader**)(p - sizeof(BlockHeader*))) = hdr;
    poisonRange(reinterpret_cast<void*>(p), size, ALLOC_POISON);

    return (void*)p;
}

void* kmalloc(size_t size) {
    return kmalloc_aligned(size, 16);
}

void kfree(void* ptr) {
    if (!ptr) return;

    BlockHeader* hdr = *((BlockHeader**)((uintptr_t)ptr - sizeof(BlockHeader*)));

    if (hdr->magic != HEADER_MAGIC || hdr->is_free) {
        kernel_panic("heap free header corruption");
    }

    uintptr_t hdr_val = (uintptr_t)hdr;
    if (hdr_val < heap_base_addr || hdr_val >= heap_base_addr + heap_total_size) {
        kernel_panic("heap free pointer out of range");
    }

    size_t block_size = 1ULL << hdr->order;
    BlockFooter* ftr = (BlockFooter*)((uintptr_t)hdr + block_size - sizeof(BlockFooter));
    if (ftr->magic != FOOTER_MAGIC) {
        kernel_panic("heap footer corruption");
    }

    heap_lock.lock();
    
    stats.total_allocated -= hdr->user_size;
    poisonRange(ptr, hdr->user_size, FREE_POISON);
    hdr->user_size = 0;
    
    uintptr_t current = (uintptr_t)hdr;
    int order = hdr->order;

    while (order < MAX_ORDER) {
        size_t current_block_size = 1ULL << order;
        uintptr_t offset = current - heap_base_addr;
        uintptr_t buddy_offset = offset ^ current_block_size;
        uintptr_t buddy_addr = heap_base_addr + buddy_offset;

        if (buddy_addr + current_block_size > heap_base_addr + heap_total_size) {
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

    heap_lock.unlock();
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
        heap_lock.lock();
        stats.total_allocated -= old_size;
        stats.total_allocated += new_size;
        hdr->user_size = new_size;
        heap_lock.unlock();
        return ptr;
    }

    uintptr_t block_end = (uintptr_t)hdr + (1ULL << hdr->order) - sizeof(BlockFooter);
    size_t capacity = block_end - (uintptr_t)ptr;

    if (new_size <= capacity) {
        heap_lock.lock();
        stats.total_allocated += (new_size - old_size);
        hdr->user_size = new_size;
        if (stats.total_allocated > stats.peak_usage) {
            stats.peak_usage = stats.total_allocated;
        }
        heap_lock.unlock();
        return ptr;
    }

    void* new_ptr = kmalloc(new_size);
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
    heap_lock.lock();
    HeapStats s = stats;
    heap_lock.unlock();
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

void* operator new(size_t size) {
    return kmalloc(size);
}

void* operator new[](size_t size) {
    return kmalloc(size);
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
