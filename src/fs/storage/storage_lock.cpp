#include <fs/storage/storage_lock.hpp>
#include <common/spinlock.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/klock.hpp>
#include <cpu/percpu.hpp>
#include <cpu/process/process.hpp>
#include <fs/vfs/vfs.hpp>

namespace {

Spinlock s_lock("storage");
volatile uint32_t s_owner = 0;   // owning CPU index + 1; 0 while free
uint32_t s_depth = 0;            // only the owner touches it

// The original entries of each wrapped table. Written at mount time, before
// any VNode of that filesystem is handed out, and never removed.
struct WrappedTable {
    VNodeOps* table;
    VNodeOps inner;
};
constexpr uint32_t kMaxWrappedTables = 8;
WrappedTable s_tables[kMaxWrappedTables];
uint32_t s_tableCount = 0;

const VNodeOps& innerOps(VNode* node) {
    for (uint32_t i = 0; i < s_tableCount; ++i) {
        if (s_tables[i].table == node->ops) {
            return s_tables[i].inner;
        }
    }
    // A wrapped entry is only reachable through a wrapped table.
    static const VNodeOps kNone{};
    Cereal::get().write("[storage] op on an unregistered VNode table\n");
    return kNone;
}

// Whether a Section opened now may release the BKL: the running thread's
// syscall opted in, and this is not a nested entry (e.g. a page fault taken
// inside the syscall) whose caller may rely on the lock.
bool mayDropBkl() {
    Process* running = getPerCPU()->currentProcess;
    if (!running) {
        return false;
    }
    const uint32_t depth = running->getStorageUnlockDepth();
    return depth != 0 && KernelLock::depth() == depth;
}

// One wrapper per VNodeOps entry; each runs the original inside a Section.
#define STORAGE_OP(name, Ret, params, args)                 \
    Ret locked_##name params {                              \
        StorageLock::Section section;                       \
        return innerOps(n).name args;                       \
    }

STORAGE_OP(open, int, (VNode* n, int flags), (n, flags))
STORAGE_OP(close, int, (VNode* n), (n))
STORAGE_OP(read, int64_t, (VNode* n, void* buf, uint64_t size, uint64_t off), (n, buf, size, off))
STORAGE_OP(write, int64_t, (VNode* n, const void* buf, uint64_t size, uint64_t off), (n, buf, size, off))
STORAGE_OP(stat, int, (VNode* n, FileStats* st), (n, st))
STORAGE_OP(readdir, int, (VNode* n, DirEntry* e, uint64_t count, uint64_t* read), (n, e, count, read))
STORAGE_OP(lookup, VNode*, (VNode* n, const char* name), (n, name))
STORAGE_OP(create, int, (VNode* n, const char* name, uint32_t mode, VNode** out), (n, name, mode, out))
STORAGE_OP(mkdir, int, (VNode* n, const char* name, uint32_t mode, VNode** out), (n, name, mode, out))
STORAGE_OP(unlink, int, (VNode* n, const char* name), (n, name))
STORAGE_OP(rmdir, int, (VNode* n, const char* name), (n, name))
STORAGE_OP(truncate, int, (VNode* n, uint64_t size), (n, size))
STORAGE_OP(rename, int, (VNode* n, const char* a, VNode* np, const char* b), (n, a, np, b))
STORAGE_OP(chmod, int, (VNode* n, uint32_t mode), (n, mode))
STORAGE_OP(utime, int, (VNode* n, uint64_t atime, uint64_t mtime), (n, atime, mtime))
STORAGE_OP(link, int, (VNode* n, const char* a, VNode* np, const char* b), (n, a, np, b))
STORAGE_OP(symlink, int, (VNode* n, const char* name, const char* target, VNode** out), (n, name, target, out))
STORAGE_OP(readlink, int64_t, (VNode* n, char* buf, uint64_t size), (n, buf, size))
STORAGE_OP(statfs, int, (VNode* n, FsStats* st), (n, st))
STORAGE_OP(chown, int, (VNode* n, uint32_t uid, uint32_t gid), (n, uid, gid))
STORAGE_OP(mknod, int, (VNode* n, const char* name, uint32_t mode, uint64_t dev, VNode** out), (n, name, mode, dev, out))
STORAGE_OP(fsync, int, (VNode* n), (n))

#undef STORAGE_OP

} // namespace

namespace StorageLock {

void wrapOps(VNodeOps* ops) {
    if (!ops) {
        return;
    }
    for (uint32_t i = 0; i < s_tableCount; ++i) {
        if (s_tables[i].table == ops) {
            return;
        }
    }
    if (s_tableCount == kMaxWrappedTables) {
        Cereal::get().write("[storage] too many filesystems to wrap\n");
        return;
    }
    WrappedTable& entry = s_tables[s_tableCount];
    entry.table = ops;
    entry.inner = *ops;

#define WRAP(name) if (ops->name) ops->name = locked_##name
    WRAP(open); WRAP(close); WRAP(read); WRAP(write); WRAP(stat);
    WRAP(readdir); WRAP(lookup); WRAP(create); WRAP(mkdir); WRAP(unlink);
    WRAP(rmdir); WRAP(truncate); WRAP(rename); WRAP(chmod); WRAP(utime);
    WRAP(link); WRAP(symlink); WRAP(readlink); WRAP(statfs); WRAP(chown);
    WRAP(mknod); WRAP(fsync);
#undef WRAP

    // Publish the entry only after it is complete; wrappers read it unlocked.
    __atomic_store_n(&s_tableCount, s_tableCount + 1, __ATOMIC_RELEASE);
}


bool heldByThisCpu() {
    return __atomic_load_n(&s_owner, __ATOMIC_RELAXED) == currentCpuIndex() + 1;
}

Section::Section() : savedBklDepth(0) {
    const uint32_t me = currentCpuIndex() + 1;
    if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) == me) {
        ++s_depth;
        return;
    }
    if (mayDropBkl()) {
        savedBklDepth = KernelLock::suspend();
    }
    s_lock.lock();
    __atomic_store_n(&s_owner, me, __ATOMIC_RELAXED);
    s_depth = 1;
}

Section::~Section() {
    if (--s_depth == 0) {
        __atomic_store_n(&s_owner, 0, __ATOMIC_RELAXED);
        s_lock.unlock();
    }
    // Retaken only after the storage lock is free (lock order BKL -> storage).
    KernelLock::resume(savedBklDepth);
}

AllowUnlocked::AllowUnlocked(bool allow)
    : process(getPerCPU()->currentProcess), savedDepth(0) {
    if (!process) {
        return;
    }
    savedDepth = process->getStorageUnlockDepth();
    process->setStorageUnlockDepth(allow ? KernelLock::depth() : 0);
}

AllowUnlocked::~AllowUnlocked() {
    if (process) {
        process->setStorageUnlockDepth(savedDepth);
    }
}

} // namespace StorageLock
