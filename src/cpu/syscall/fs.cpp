#include <cpu/syscall/syscall.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/process/scheduler.hpp>
#include <drivers/usb/ohci.hpp>
#include <fs/vfs/vfs.hpp>
#include <graphics/console.hpp>
#include <interrupts/keyboard.hpp>
#include <common/string.hpp>

namespace {
constexpr uint64_t kOpenAccessModeMask = 0x3;
constexpr uint64_t kOpenWriteOnly = 0x1;
constexpr uint64_t kOpenReadWrite = 0x2;

void traceStr(const char* text) {
    Cereal::get().write(text);
}

void traceDec(uint64_t value) {
    char buf[21];
    int pos = 0;
    if (value == 0) {
        Cereal::get().write('0');
        return;
    }

    while (value > 0 && pos < static_cast<int>(sizeof(buf))) {
        buf[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        Cereal::get().write(buf[--pos]);
    }
}

void tracePrintableChar(char c) {
    if (c == '\n') {
        Cereal::get().write("\\n");
    } else if (c == '\r') {
        Cereal::get().write("\\r");
    } else if (c == '\b') {
        Cereal::get().write("\\b");
    } else if (c == '\t') {
        Cereal::get().write("\\t");
    } else {
        Cereal::get().write(c);
    }
}

uint32_t fileRightsFromOpenFlags(uint64_t flags) {
    uint32_t rights = HandleRightDuplicate;

    switch (flags & kOpenAccessModeMask) {
        case kOpenWriteOnly:
            rights |= HandleRightWrite;
            break;
        case kOpenReadWrite:
            rights |= HandleRightRead | HandleRightWrite;
            break;
        default:
            rights |= HandleRightRead;
            break;
    }

    return rights;
}
}

uint64_t Syscall::sys_write(uint64_t fileHandle, uint64_t buf, uint64_t count) {
    if (fileHandle == 1 || fileHandle == 2) {
        if (count == 0) return count;
        if (!isValidUserPointer(buf, count)) return -1;
        
        char chunk[256];
        uint64_t total = 0;
        while (total < count) {
            uint64_t toCopy = count - total;
            if (toCopy > sizeof(chunk)) {
                toCopy = sizeof(chunk);
            }
            if (!copyFromUser(chunk, buf + total, toCopy)) {
                return static_cast<uint64_t>(-1);
            }

            for (uint64_t i = 0; i < toCopy; i++) {
                char temp[2] = { chunk[i], '\0' };
                Console::get().drawText(temp);
            }
            total += toCopy;
        }
        
        return count;
    }
    
    if (!isValidUserPointer(buf, count)) {
        return -1;
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return -1;
    
    FileDescriptor* fileFd = current->getFD(fileHandle, HandleRightWrite);
    if (!fileFd) return -1;
    
    char chunk[512];
    uint64_t total = 0;
    while (total < count) {
        uint64_t toCopy = count - total;
        if (toCopy > sizeof(chunk)) {
            toCopy = sizeof(chunk);
        }
        if (!copyFromUser(chunk, buf + total, toCopy)) {
            return static_cast<uint64_t>(-1);
        }

        int64_t written = VFS::get().write(fileFd, chunk, toCopy);
        if (written < 0) {
            return total > 0 ? total : static_cast<uint64_t>(-1);
        }
        total += static_cast<uint64_t>(written);
        if (static_cast<uint64_t>(written) != toCopy) {
            break;
        }
    }
    
    return total;
}

uint64_t Syscall::sys_read(uint64_t fileHandle, uint64_t buf, uint64_t count) {
    if (fileHandle == 0) {
        if (!isValidUserPointer(buf, count)) {
            traceStr("[stdin] read invalid user buffer count=");
            traceDec(count);
            traceStr("\n");
            return -1;
        }

        Process* current = Scheduler::get().getCurrentProcess();
        traceStr("[stdin] read begin pid=");
        traceDec(current ? current->getPID() : 0);
        traceStr(" name=");
        traceStr(current ? current->getName() : "<none>");
        traceStr(" count=");
        traceDec(count);
        traceStr("\n");

        size_t bytesRead = 0;

        while (bytesRead < count) {
            USBInput::get().poll();
            Keyboard::get().servicePendingInput();

            if (!Keyboard::get().hasKey()) {
                current = Scheduler::get().getCurrentProcess();
                if (!current) {
                    traceStr("[stdin] read no current process while blocking\n");
                    return static_cast<uint64_t>(-1);
                }

                traceStr("[stdin] read blocking pid=");
                traceDec(current->getPID());
                traceStr(" name=");
                traceStr(current->getName());
                traceStr(" bytes=");
                traceDec(bytesRead);
                traceStr("\n");
                current->setState(ProcessState::Blocked);
                Scheduler::get().scheduleFromSyscall();
                traceStr("[stdin] read woke pid=");
                traceDec(current->getPID());
                traceStr(" name=");
                traceStr(current->getName());
                traceStr("\n");
                continue;
            }

            char c = Keyboard::get().getKey();

            if (!copyToUser(buf + bytesRead, &c, 1)) {
                traceStr("[stdin] read copy_to_user failed byte=");
                traceDec(bytesRead);
                traceStr("\n");
                return static_cast<uint64_t>(-1);
            }
            bytesRead++;
            traceStr("[stdin] read char=");
            tracePrintableChar(c);
            traceStr(" bytes=");
            traceDec(bytesRead);
            traceStr("\n");

            if (!Keyboard::get().hasKey()) {
                break;
            }
        }

        traceStr("[stdin] read end bytes=");
        traceDec(bytesRead);
        traceStr("\n");
        return bytesRead;
    }
    
    if (!isValidUserPointer(buf, count)) {
        return -1;
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return -1;
    
    FileDescriptor* fileFd = current->getFD(fileHandle, HandleRightRead);
    if (!fileFd) return -1;
    
    char chunk[512];
    uint64_t total = 0;
    while (total < count) {
        uint64_t toRead = count - total;
        if (toRead > sizeof(chunk)) {
            toRead = sizeof(chunk);
        }

        int64_t bytesRead = VFS::get().read(fileFd, chunk, toRead);
        if (bytesRead < 0) {
            return total > 0 ? total : static_cast<uint64_t>(-1);
        }
        if (bytesRead == 0) {
            break;
        }
        if (!copyToUser(buf + total, chunk, static_cast<size_t>(bytesRead))) {
            return static_cast<uint64_t>(-1);
        }
        total += static_cast<uint64_t>(bytesRead);
        if (static_cast<uint64_t>(bytesRead) != toRead) {
            break;
        }
    }
    
    return total;
}

uint64_t Syscall::sys_open(uint64_t path, uint64_t flags, uint64_t mode) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return -1;
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return -1;
    
    FileDescriptor* fd = nullptr;
    int result = VFS::get().open(pathname, flags, &fd);
    
    if (result != 0 || !fd) {
        return -1;
    }
    
    uint64_t fileHandle = current->allocateFD(fd, fileRightsFromOpenFlags(flags));
    if (fileHandle == static_cast<uint64_t>(-1)) {
        VFS::get().close(fd);
        return -1;
    }
    
    return fileHandle;
}

uint64_t Syscall::sys_close(uint64_t handle) {
    if (handle < 3) return -1;
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return -1;

    if (current->getFD(handle)) {
        current->closeFD(handle);
        return 0;
    }

    return current->closeHandle(handle) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_chdir(uint64_t path) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return (uint64_t)-1;
    }
    
    FileDescriptor* fd = nullptr;
    int result = VFS::get().open(pathname, 0, &fd);
    if (result != 0 || !fd) {
        return (uint64_t)-1;
    }
    VFS::get().close(fd);
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;
    
    current->setCwd(pathname);
    return 0;
}

uint64_t Syscall::sys_getcwd(uint64_t buf, uint64_t size) {
    if (!isValidUserPointer(buf, size)) {
        return (uint64_t)-1;
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;
    
    const char* cwd = current->getCwd();
    size_t cwdLen = 0;
    while (cwd[cwdLen]) cwdLen++;
    
    if (cwdLen + 1 > size) {
        return (uint64_t)-1;
    }
    
    if (!copyToUser(buf, cwd, cwdLen + 1)) {
        return (uint64_t)-1;
    }
    
    return buf;
}

uint64_t Syscall::sys_mkdir(uint64_t path, uint64_t mode) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return (uint64_t)-1;
    }
    
    int result = VFS::get().mkdir(pathname, mode);
    return result == 0 ? 0 : (uint64_t)-1;
}

uint64_t Syscall::sys_rmdir(uint64_t path) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return (uint64_t)-1;
    }
    
    int result = VFS::get().rmdir(pathname);
    return result == 0 ? 0 : (uint64_t)-1;
}

uint64_t Syscall::sys_unlink(uint64_t path) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return (uint64_t)-1;
    }
    
    int result = VFS::get().unlink(pathname);
    return result == 0 ? 0 : (uint64_t)-1;
}

uint64_t Syscall::sys_stat(uint64_t path, uint64_t statbuf) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return (uint64_t)-1;
    }
    
    if (!isValidUserPointer(statbuf, sizeof(Stat))) {
        return (uint64_t)-1;
    }
    
    FileStats fileStats {};
    if (VFS::get().stat(pathname, &fileStats) != 0) {
        return (uint64_t)-1;
    }
    
    Stat stat {};
    stat.st_dev = 0;
    stat.st_ino = fileStats.inode;
    stat.st_mode = fileStats.mode;
    stat.st_nlink = fileStats.links;
    stat.st_uid = 0;
    stat.st_gid = 0;
    stat.st_rdev = 0;
    stat.st_size = fileStats.size;
    stat.st_blksize = 4096;
    stat.st_blocks = (fileStats.size + 511) / 512;
    stat.st_atime = fileStats.atime;
    stat.st_mtime = fileStats.mtime;
    stat.st_ctime = fileStats.ctime;

    if (fileStats.type == FileType::Directory) {
        stat.st_mode |= 0040000;
    } else {
        stat.st_mode |= 0100000;
    }

    return copyToUser(statbuf, &stat, sizeof(Stat)) ? 0 : (uint64_t)-1;
}

uint64_t Syscall::sys_dup(uint64_t handle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    return current->duplicateHandle(handle);
}

uint64_t Syscall::sys_dup2(uint64_t oldHandle, uint64_t newHandle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    if (!current->duplicateHandleTo(oldHandle, newHandle)) {
        return (uint64_t)-1;
    }
    
    return newHandle;
}

uint64_t Syscall::sys_pipe(uint64_t pipeHandles) {
    if (!isValidUserPointer(pipeHandles, sizeof(uint64_t) * 2)) {
        return (uint64_t)-1;
    }
    
    return (uint64_t)-1;
}

uint64_t Syscall::sys_seek(uint64_t handle, uint64_t offset, uint64_t whence) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    FileDescriptor* fileFd = current->getFD(handle, HandleRightRead);
    if (!fileFd) {
        fileFd = current->getFD(handle, HandleRightWrite);
    }
    if (!fileFd) {
        return static_cast<uint64_t>(-1);
    }

    SeekMode mode = SeekMode::Set;
    switch (whence) {
        case 0:
            mode = SeekMode::Set;
            break;
        case 1:
            mode = SeekMode::Current;
            break;
        case 2:
            mode = SeekMode::End;
            break;
        default:
            return static_cast<uint64_t>(-1);
    }

    const int64_t signedOffset = static_cast<int64_t>(offset);
    const int64_t result = VFS::get().seek(fileFd, signedOffset, mode);
    return result < 0 ? static_cast<uint64_t>(-1) : static_cast<uint64_t>(result);
}

uint64_t Syscall::sys_readdir(uint64_t path, uint64_t entries, uint64_t count) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return (uint64_t)-1;
    }
    
    if (count > (~0ULL / sizeof(DirEntry))) {
        return (uint64_t)-1;
    }

    if (!isValidUserPointer(entries, count * sizeof(DirEntry))) {
        return (uint64_t)-1;
    }
    
    ::DirEntry* vfsEntries = new ::DirEntry[count];
    if (!vfsEntries) {
        return (uint64_t)-1;
    }
    
    uint64_t readCount = 0;
    int result = VFS::get().readdir(pathname, vfsEntries, count, &readCount);
    
    if (result != 0) {
        delete[] vfsEntries;
        return (uint64_t)-1;
    }

    if (readCount == 0) {
        delete[] vfsEntries;
        return 0;
    }
    
    DirEntry* outEntries = new DirEntry[readCount];
    if (!outEntries) {
        delete[] vfsEntries;
        return (uint64_t)-1;
    }

    for (uint64_t i = 0; i < readCount; i++) {
        size_t j = 0;
        while (vfsEntries[i].name[j] && j < 255) {
            outEntries[i].name[j] = vfsEntries[i].name[j];
            j++;
        }
        outEntries[i].name[j] = '\0';
        
        outEntries[i].inode = vfsEntries[i].inode;
        outEntries[i].type = vfsEntries[i].type;
    }
    
    bool copied = copyToUser(entries, outEntries, readCount * sizeof(DirEntry));
    delete[] outEntries;
    delete[] vfsEntries;
    return copied ? readCount : (uint64_t)-1;
}
