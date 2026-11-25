// memfd_create: a file of shared pages with no name (memory/shared_pages.hpp).
// Its fd reads, writes, seeks, truncates and maps like any file's, is inherited
// and passed over AF_UNIX sockets like any other, and the memory lives until
// the last fd and mapping are gone. shm_open is a file in /dev/shm instead, a
// RamFS.
#include <cpu/process/scheduler.hpp>
#include <cpu/syscall/shm.hpp>
#include <cpu/syscall/syscall.hpp>
#include <fs/ramfs/ramfs.hpp>
#include <fs/vfs/vfs.hpp>
#include <memory/shared_pages.hpp>
#include <time/time.hpp>

namespace {
constexpr uint64_t kMfdCloseOnExec = 1, kMfdAllowSealing = 2;
constexpr uint32_t kSealSeal = 1, kSealShrink = 2, kSealGrow = 4, kSealWrite = 8, kSealFutureWrite = 16;
constexpr uint32_t kAllSeals = 31;

struct Memfd {
    SharedPages* pages;
    uint64_t inode;
    uint64_t created;
};

Memfd* memfdOf(VNode* node) {
    return static_cast<Memfd*>(node->getData());
}

int memfdClose(VNode* node) {
    Memfd* memfd = memfdOf(node);
    node->setData(nullptr);
    if (memfd) {
        memfd->pages->releaseFile();
        delete memfd;
    }
    return 0;
}

int64_t memfdRead(VNode* node, void* buffer, uint64_t size, uint64_t offset) {
    Memfd* memfd = memfdOf(node);
    return memfd ? memfd->pages->read(buffer, size, offset) : -1;
}

int64_t memfdWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset) {
    Memfd* memfd = memfdOf(node);
    if (!memfd || (memfd->pages->seals & (kSealWrite | kSealFutureWrite))) {
        return -1;
    }
    if ((memfd->pages->seals & kSealGrow) && offset + size > memfd->pages->size()) {
        return -1;
    }
    const int64_t n = memfd->pages->write(buffer, size, offset);
    return n < 0 ? -1 : n;
}

int memfdStat(VNode* node, FileStats* stats) {
    Memfd* memfd = memfdOf(node);
    if (!memfd || !stats) return -1;
    *stats = {};
    stats->size = memfd->pages->size();
    stats->type = FileType::Regular;
    stats->mode = 0777;
    stats->inode = memfd->inode;
    stats->links = 0;   // it has no name
    stats->atime = stats->mtime = stats->ctime = memfd->created;
    return 0;
}

int memfdTruncate(VNode* node, uint64_t size) {
    Memfd* memfd = memfdOf(node);
    if (!memfd) return -1;
    return sharedPagesMayResize(node, size) ? memfd->pages->resize(size) : -1;
}

VNodeOps gMemfdOps = {nullptr, memfdClose, memfdRead, memfdWrite, memfdStat};

SharedPages* memfdPages(VNode* node) {
    return node && node->ops == &gMemfdOps && node->getData() ? memfdOf(node)->pages : nullptr;
}
}

SharedPages* sharedPagesOf(VNode* node) {
    if (SharedPages* pages = memfdPages(node)) {
        return pages;
    }
    return RamFS::pagesOf(node);
}

bool sharedPagesMayWrite(VNode* node) {
    SharedPages* pages = memfdPages(node);
    return !pages || (pages->seals & (kSealWrite | kSealFutureWrite)) == 0;
}

bool sharedPagesMayResize(VNode* node, uint64_t size) {
    SharedPages* pages = memfdPages(node);
    return !pages || !((size < pages->size() && (pages->seals & kSealShrink)) ||
                       (size > pages->size() && (pages->seals & kSealGrow)));
}

// The name is for debugging only, as on Linux.
uint64_t Syscall::sys_memfd_create(uint64_t name, uint64_t flags) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    if (flags & ~(kMfdCloseOnExec | kMfdAllowSealing)) return syscall_error(SysErrInvalid);
    char text[256];
    if (name && !copyUserString(name, text, sizeof(text))) return syscall_error(SysErrFault);

    gMemfdOps.truncate = memfdTruncate;
    SharedPages* pages = SharedPages::create();
    auto* memfd = pages ? new Memfd() : nullptr;
    auto* node = memfd ? new VNode(nullptr, 0, FileType::Regular) : nullptr;
    auto* fd = node ? new FileDescriptor(node, 2 /* O_RDWR */) : nullptr;
    if (!fd) {
        if (node) delete node;
        if (memfd) delete memfd;
        if (pages) pages->releaseFile();
        return syscall_error(SysErrNoMemory);
    }
    static uint64_t nextInode = 1;
    memfd->pages = pages;
    memfd->inode = nextInode++;
    memfd->created = time_get_unix();
    if (!(flags & kMfdAllowSealing)) {
        pages->seals = kSealSeal;
    }
    node->ops = &gMemfdOps;
    node->setData(memfd);
    node->refCount = 1;   // the descriptor's, which deletes it (no file system owns it)

    const uint64_t handle = current->allocateFD(fd, HandleRightRead | HandleRightWrite | HandleRightDuplicate,
                                                (flags & kMfdCloseOnExec) != 0);
    if (handle == static_cast<uint64_t>(-1)) {
        VFS::get().close(fd);
        return syscall_error(SysErrTooManyFiles);
    }
    return handle;
}

// fcntl F_ADD_SEALS and F_GET_SEALS.
uint64_t memfdSeals(Process* process, uint64_t handle, bool add, uint64_t seals) {
    FileDescriptor* fd = process->getFD(handle);
    SharedPages* pages = fd ? memfdPages(fd->getNode()) : nullptr;
    if (!pages) return syscall_error(SysErrInvalid);
    if (!add) return pages->seals;
    if (seals & ~static_cast<uint64_t>(kAllSeals)) return syscall_error(SysErrInvalid);
    if (!process->getFD(handle, HandleRightWrite) || (pages->seals & kSealSeal)) {
        return syscall_error(SysErrPermission);
    }
    if ((seals & kSealWrite) && pages->mapped()) return syscall_error(SysErrBusy);
    pages->seals |= static_cast<uint32_t>(seals);
    return 0;
}
