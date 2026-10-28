#pragma once

#include <stdint.h>

struct OSInfo {
    char     osname[10];
    char     loggedOnUser[32];
    char     cpuname[64];
    char     maxRamGB[8];
    char     usedRamGB[8];
    uint8_t  major;
    uint8_t  minor;
    uint8_t  patch;
    uint64_t buildnum;
};

// One CPU, as reported by the CpuInfo syscall.
struct CpuInfoEntry {
    uint32_t index;         // SMP index (0 = boot CPU)
    uint32_t lapicId;
    uint64_t deviceIrqs;    // device interrupts this CPU served
    uint32_t currentPid;    // 0 while idle
    uint32_t reserved;
};

// Interface configuration, read with NetGetConfig and set with NetSetConfig.
// Addresses are IPv4 in network byte order, as in sockaddr_in.
constexpr uint32_t NET_CONFIG_MAX_DNS = 3;
constexpr uint32_t NET_CONFIG_DOMAIN_SIZE = 64;

enum NetConfigFlags : uint32_t {
    NetConfigUp = 1u << 0,      // an address is configured
    NetConfigDhcp = 1u << 1,    // ... and it came from a DHCP lease
};

// NetSetConfig modes.
enum NetConfigMode : uint64_t {
    NetConfigStatic = 0,        // use address/netmask/gateway/dns from the struct
    NetConfigDhcpStart = 1,     // drop the address and ask DHCP from scratch
    NetConfigDhcpRenew = 2,     // renew the current lease now
    NetConfigDown = 3,          // no address, DHCP off
};

struct NetConfig {
    uint32_t flags;           // NetConfigFlags
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns[NET_CONFIG_MAX_DNS];
    uint32_t dnsCount;
    uint32_t dhcpServer;      // 0 unless from DHCP
    uint32_t leaseSeconds;    // granted lease, 0xFFFFFFFF = infinite
    uint32_t leaseRemaining;  // seconds left on it
    uint32_t ntpServer;       // NTP server (DHCP option 42), 0 if none
    uint64_t generation;      // changes whenever the configuration does
    char domain[NET_CONFIG_DOMAIN_SIZE];   // DHCP domain name, "" if none
    uint32_t linkUp;          // 1 while the NIC has a link
    char device[16];          // driver of the NIC in use ("e1000", "rtl8168h", ...), "" if none
};

struct ProcInfoEntry {
    uint32_t pid;
    uint32_t parentPID;
    uint32_t uid;
    uint32_t gid;
    uint32_t sessionID;
    uint32_t state;
    uint32_t priority;
    uint32_t flags;
    int32_t exitCode;
    char name[64];
};

enum StorageInfoFlags : uint32_t {
    StorageInfoPresent = 1 << 0,
    StorageInfoReadable = 1 << 1,
    StorageInfoWritable = 1 << 2,
    StorageInfoMounted = 1 << 3,
    StorageInfoFormatted = 1 << 4,
};

struct StorageInfo {
    uint64_t totalSize;
    uint32_t sectorSize;
    uint32_t flags;
    int32_t mountError;
    uint32_t reserved;
    char deviceName[32];
    char fsType[16];
    char mountPath[64];
};

struct SigActionInfo {
    uint64_t handler;
    uint64_t mask;
    uint64_t flags;
    uint64_t restorer;
};

struct SignalStackInfo {
    uint64_t sp;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
};

struct LoginInfo {
    char     username[32];
    char     password[64];
};

struct SessionInfo {
    uint32_t sessionID;
    uint32_t uid;
    uint32_t gid;
    uint32_t leaderPID;
    uint64_t loginTime;
    uint8_t  state;
};

struct Stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

// Filesystem statistics written back by the Statfs syscall. The mlibc sysdep
// translates this into struct statvfs / struct statfs.
struct KernelStatfs {
    uint64_t blockSize;
    uint64_t totalBlocks;
    uint64_t freeBlocks;
    uint64_t totalInodes;
    uint64_t freeInodes;
    uint64_t nameMax;
    uint32_t fsType;
    uint32_t reserved;
};

struct PollFD {
    int64_t fd;
    int16_t events;
    int16_t revents;
};

struct UserInfo {
    uint32_t uid;
    uint32_t gid;
    char     username[32];
    char     homeDir[256];
    char     shell[256];
};

struct SurfaceInfo {
    uint64_t id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t pitch;
    uint32_t dirtyX;
    uint32_t dirtyY;
    uint32_t dirtyWidth;
    uint32_t dirtyHeight;
    uint64_t address;
};

enum WindowStateFlags : uint32_t {
    WindowStateNone = 0,
    WindowStateFocused = 1 << 0,
    WindowStateMinimized = 1 << 1,
    WindowStateMaximized = 1 << 2,
    WindowStateClosed = 1 << 3
};

enum class WindowControlAction : uint32_t {
    Restore = 0,
    Minimize = 1,
    Maximize = 2,
    Close = 3
};

struct WindowInfo {
    uint64_t id;
    uint32_t ownerPID;
    uint32_t flags;
    uint32_t state;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint64_t surfaceID;
    uint32_t zOrder;
    char title[64];
};

struct IPCMessage {
    uint64_t id;
    uint32_t senderPID;
    uint16_t flags;
    uint16_t reserved;
    uint64_t size;
    uint8_t  data[256];
};

inline constexpr uint16_t IPC_MESSAGE_REQUEST = 1 << 0;
inline constexpr uint16_t IPC_MESSAGE_EVENT = 1 << 1;

enum class EventType : uint16_t {
    None = 0,
    Key,
    Pointer,
    Window
};

enum class KeyEventAction : uint16_t {
    Press = 0,
    Release,
    Repeat
};

enum KeyModifiers : uint16_t {
    KeyModifierNone = 0,
    KeyModifierShift = 1 << 0,
    KeyModifierControl = 1 << 1,
    KeyModifierAlt = 1 << 2,
    KeyModifierCapsLock = 1 << 3,
    KeyModifierSuper = 1 << 4,
    KeyModifierNumLock = 1 << 5
};

// Key codes: the USB HID keyboard usage (page 0x07) of the physical key,
// whichever keyboard it came from (PS/2 scancodes are translated). They name
// keys, not characters: KeyEvent.text carries what the key typed.
enum KeyCode : uint16_t {
    KeyNone = 0x00,
    KeyA = 0x04, KeyB, KeyC, KeyD, KeyE, KeyF, KeyG, KeyH, KeyI, KeyJ, KeyK, KeyL, KeyM,
    KeyN, KeyO, KeyP, KeyQ, KeyR, KeyS, KeyT, KeyU, KeyV, KeyW, KeyX, KeyY, KeyZ,
    Key1 = 0x1E, Key2, Key3, Key4, Key5, Key6, Key7, Key8, Key9, Key0,
    KeyEnter = 0x28, KeyEscape, KeyBackspace, KeyTab, KeySpace,
    KeyMinus = 0x2D, KeyEqual, KeyLeftBracket, KeyRightBracket, KeyBackslash,
    KeySemicolon = 0x33, KeyApostrophe, KeyGrave, KeyComma, KeyPeriod, KeySlash,
    KeyCapsLock = 0x39,
    KeyF1 = 0x3A, KeyF2, KeyF3, KeyF4, KeyF5, KeyF6, KeyF7, KeyF8, KeyF9, KeyF10, KeyF11, KeyF12,
    KeyPrintScreen = 0x46, KeyScrollLock, KeyPause, KeyInsert, KeyHome, KeyPageUp,
    KeyDelete, KeyEnd, KeyPageDown, KeyRight, KeyLeft, KeyDown, KeyUp,
    KeyNumLock = 0x53, KeypadSlash, KeypadAsterisk, KeypadMinus, KeypadPlus, KeypadEnter,
    Keypad1 = 0x59, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9, Keypad0,
    KeypadPeriod = 0x63, KeyNonUsBackslash, KeyApplication,
    KeyLeftControl = 0xE0, KeyLeftShift, KeyLeftAlt, KeyLeftSuper,
    KeyRightControl, KeyRightShift, KeyRightAlt, KeyRightSuper,
};

enum class PointerEventAction : uint16_t {
    Move = 0,
    Button,
    Scroll
};

enum class WindowEventAction : uint16_t {
    None = 0,
    FocusGained,
    FocusLost,
    CloseRequested,
    Resized,
    Moved
};

// action: Press, Release, or Repeat while held. keycode: a KeyCode.
// modifiers: KeyModifiers held or locked at the time. text: the UTF-8 the key
// typed, NUL-terminated, "" for keys that type nothing printable (Enter,
// Backspace, Tab and Escape included: tell those by keycode). With Control
// held, text is still the plain character ("c" for Ctrl+C).
struct KeyEvent {
    KeyEventAction action;
    uint16_t modifiers;
    uint16_t keycode;
    uint16_t reserved;
    char text[8];
};

struct PointerEvent {
    PointerEventAction action;
    uint16_t buttons;
    int32_t x;
    int32_t y;
    int32_t deltaX;
    int32_t deltaY;
};

struct WindowEvent {
    WindowEventAction action;
    uint16_t reserved0;
    uint32_t windowId;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct Event {
    EventType type;
    uint16_t reserved0;
    uint32_t reserved1;
    union {
        KeyEvent key;
        PointerEvent pointer;
        WindowEvent window;
        uint8_t raw[48];
    };
};

struct GPUCapsetInfo {
    uint32_t index;
    uint32_t capsetId;
    uint32_t capsetMaxVersion;
    uint32_t capsetMaxSize;
};

struct GPUCapsetData {
    uint32_t capsetId;
    uint32_t capsetVersion;
    uint32_t bufferSize;
    uint32_t actualSize;
    uint64_t buffer;
};

struct GPUContextCreate {
    uint32_t ctxId;
    uint32_t capsetId;
    uint32_t contextInit;
    uint8_t ringIdx;
    uint8_t useRingIdx;
    uint8_t reserved[2];
    char debugName[64];
};

struct GPUResourceCreate3D {
    uint32_t ctxId;
    uint32_t resourceId;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t arraySize;
    uint32_t lastLevel;
    uint32_t nrSamples;
    uint32_t flags;
};

struct GPUResourceDestroy {
    uint32_t ctxId;
    uint32_t resourceId;
    uint8_t hasBacking;
    uint8_t reserved[3];
};

struct GPUResourceUUID {
    uint32_t resourceId;
    uint8_t uuid[16];
};

struct GPUSubmit3D {
    uint32_t ctxId;
    uint32_t size;
    uint64_t commands;
    uint8_t transportOk;
    uint8_t responseOk;
    uint8_t reserved[6];
    uint32_t responseType;
    uint64_t submittedFence;
    uint64_t completedFence;
};

struct GPUWaitFence {
    uint64_t fenceId;
    uint64_t timeoutIterations;
    uint64_t completedFence;
    uint32_t responseType;
    uint8_t completed;
    uint8_t reserved[3];
};

// Result of the Venus (Vulkan over virtio-gpu) availability + round-trip probe
// exposed to userspace. See Syscall::sys_gpu_venus_probe.
struct GPUVenusProbe {
    uint8_t available;        // out: host advertises a compatible Venus capset
    uint8_t replyOk;          // out: vkEnumerateInstanceVersion round trip ok
    uint8_t reserved[2];
    uint32_t capsetVersion;   // out: VK_MESA_venus_protocol spec version
    uint32_t wireFormatVersion;
    uint32_t vkXmlVersion;
    uint32_t instanceVersion; // out: Vulkan instance version (if replyOk)
    uint32_t responseType;    // out: virtio-gpu response type of the submission
};

// Result of a fuller Venus Vulkan bring-up over the async ring (instance +
// physical device enumeration + properties + logical device). See
// Syscall::sys_gpu_venus_vulkan.
struct GPUVenusVulkan {
    uint8_t ringOk;           // out: async command ring created
    uint8_t instanceOk;       // out: vkCreateInstance succeeded
    uint8_t physDevOk;        // out: >=1 physical device enumerated
    uint8_t propsOk;          // out: device 0 properties read
    uint8_t deviceOk;         // out: vkCreateDevice succeeded
    uint8_t computeOk;        // out: compute dispatch ran + readback verified
    uint8_t reserved[2];
    uint32_t physDevCount;    // out: number of physical devices
    uint32_t apiVersion;      // out: device 0 apiVersion
    uint32_t driverVersion;   // out: device 0 driverVersion
    uint32_t vendorId;        // out: device 0 vendorID
    uint32_t deviceId;        // out: device 0 deviceID
    uint32_t deviceType;      // out: device 0 VkPhysicalDeviceType
    uint32_t computeElements; // out: elements dispatched/verified
    uint32_t computeMismatches; // out: readback mismatches (0 = success)
    uint64_t instanceHandle;  // out: Venus VkInstance object id
    uint64_t deviceHandle;    // out: Venus VkDevice object id
    char deviceName[256];     // out: device 0 name
};
#include <fs/vfs/vfs.hpp>

// Arguments for the file-backed mmap syscall (SyscallNumber::MmapFile). Passed
// by pointer because the value set (six 64-bit words) exceeds the 5-argument
// syscall convention. The field order is shared verbatim with the mlibc sysdep
// (VmMap), so it must not be reordered.
struct MmapFileArgs {
    uint64_t addr;    // requested address (0 = kernel chooses)
    uint64_t length;  // mapping length in bytes
    uint64_t prot;    // MemoryProt* bits
    uint64_t flags;   // MAP_* bits (MAP_SHARED/MAP_PRIVATE/MAP_ANONYMOUS/...)
    uint64_t fd;      // kernel file handle to back the mapping
    uint64_t offset;  // byte offset into the file (page-aligned)
};

enum class SyscallNumber : uint64_t {
    OSInfo,
    ProcInfo,
    Exit,
    Write,
    Read,
    Open,
    Close,
    GetPID,
    Fork,
    Exec,
    Wait,
    Kill,
    Mmap,
    Munmap,
    Yield,
    Sleep,
    GetTime,
    Clear,
    FBInfo,
    FBMap,
    Signal,
    SigReturn,
    Login,
    Logout,
    GetUID,
    GetGID,
    SetUID,
    SetGID,
    GetSessionID,
    GetSessionInfo,
    Chdir,
    Getcwd,
    Mkdir,
    Rmdir,
    Unlink,
    Stat,
    Dup,
    Dup2,
    Pipe,
    Getppid,
    Spawn,
    GetUserInfo,
    Readdir,
    FBFlush,
    SharedAlloc,
    SharedMap,
    SharedFree,
    SurfaceCreate,
    SurfaceMap,
    SurfaceCommit,
    SurfacePoll,
    CompositorCreateWindow,
    WindowEventQueue,
    WindowSetTitle,
    WindowAttachSurface,
    WindowList,
    WindowFocus,
    WindowMove,
    WindowResize,
    WindowControl,
    QueueCreate,
    QueueSend,
    QueueReceive,
    QueueReply,
    QueueRequest,
    ServiceRegister,
    ServiceConnect,
    NetGetMAC,
    NetSend,
    NetRecv,
    NetLinkStatus,
    NetPing,
    NetProcessPackets,
    NetGetPingReply,
    ThreadCreate,
    ThreadExit,
    ThreadJoin,
    Seek,
    GPUCapsetInfo,
    GPUCapset,
    GPUContextCreate,
    GPUContextDestroy,
    GPUResourceCreate3D,
    GPUResourceDestroy,
    GPUResourceAssignUUID,
    GPUSubmit3D,
    GPUWaitFence,
    GetUnixTime,
    SerialWrite,
    Fcntl,
    Mprotect,
    Poll,
    Truncate,
    Rename,
    Chmod,
    Utime,
    Fstat,
    Link,
    Symlink,
    Readlink,
    Lstat,
    Sigprocmask,
    Socket,
    Bind,
    Connect,
    Listen,
    Accept,
    Send,
    Recv,
    Shutdown,
    GetSockOpt,
    SetSockOpt,
    StorageInfo,
    StorageFormat,
    StorageMount,
    Sigaction,
    Sigaltstack,
    ThreadSignal,
    SetThreadPointer,
    Ioctl,
    Access,
    Statfs,
    Chown,
    Mknod,
    GetEntropy,
    GetSockName,
    GetPeerName,
    GPUVenusProbeCall,
    GPUVenusVulkanCall,
    Pipe2,
    Fsync,
    MmapFile,
    Msync,
    FdPath,
    FdTableStash,
    FdTableFetch,
    ThreadClone,
    SchedSetAffinity,
    SchedGetAffinity,
    GetCpu,
    CpuInfo,
    NetGetConfig,
    NetSetConfig,
    SendTo,
    RecvFrom,
    GetTimeNs,
    SetItimer,
    GetItimer,
    GetUnixTimeNs,
    SetUnixTime,
    WindowPostEvent,
    KernelLog,
};

enum MemoryProtection : uint64_t {
    MemoryProtRead = 1ULL << 0,
    MemoryProtWrite = 1ULL << 1,
    MemoryProtExecute = 1ULL << 2,
};

enum SyscallErrno : int {
    SysErrPermission = 1,
    SysErrNoEntry = 2,
    SysErrNoProcess = 3,
    SysErrInterrupted = 4,
    SysErrBadFile = 9,
    SysErrNoChild = 10,
    SysErrAgain = 11,
    SysErrNoMemory = 12,
    SysErrAccess = 13,
    SysErrExists = 17,
    SysErrCrossDevice = 18,
    SysErrNotDirectory = 20,
    SysErrIsDirectory = 21,
    SysErrInvalid = 22,
    SysErrNoSpace = 28,
    SysErrPipe = 29,
    SysErrBrokenPipe = 32,
    SysErrRange = 34,
    SysErrNameTooLong = 36,
    SysErrNoSys = 38,
    SysErrNotEmpty = 39,
    SysErrLoop = 40,
    SysErrNotSocket = 88,
    SysErrMessageSize = 90,
    SysErrProtoOpt = 92,
    SysErrProtocolNotSupported = 93,
    SysErrOperationNotSupported = 95,
    SysErrAddressFamilyNotSupported = 97,
    SysErrAddressInUse = 98,
    SysErrNetUnreachable = 101,
    SysErrConnectionReset = 104,
    SysErrIsConnected = 106,
    SysErrTimedOut = 110,
    SysErrConnectionRefused = 111,
    SysErrHostDown = 112,
    SysErrHostUnreachable = 113,
    SysErrAlready = 114,
    SysErrInProgress = 115,
    SysErrNotConnected = 107,
};

constexpr uint64_t syscall_error(SyscallErrno error) {
    return static_cast<uint64_t>(-static_cast<int64_t>(error));
}

struct SyscallFrame {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} __attribute__((packed));

class Syscall {
public:
    Syscall() : initialized(false) {}
    
    static Syscall& get();
    
    void initialize();
    // Programs the syscall MSRs (EFER.SCE, STAR, LSTAR, SFMASK) on the
    // executing CPU; initialize() does this for the boot CPU.
    void initializeCpu();
    void setKernelStack(uint64_t stack);
    uint64_t handle(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
    
private:
    bool initialized;
    
public:
    static bool isValidUserPointer(uint64_t ptr, size_t size);
    static bool copyFromUser(void* dest, uint64_t src, size_t size);
    static bool copyToUser(uint64_t dest, const void* src, size_t size);
    static bool copyStringFromUser(uint64_t src, char* dest, size_t destSize);
    static bool copyUserString(uint64_t ptr, char* dest, size_t destSize);
    
private:
    static bool makeAbsolutePath(const char* path, char* out);
    uint64_t sys_exit(uint64_t code);
    uint64_t sys_write(uint64_t fileHandle, uint64_t buf, uint64_t count);
    uint64_t sys_serial_write(uint64_t buf, uint64_t count);
    uint64_t sys_read(uint64_t fileHandle, uint64_t buf, uint64_t count);
    uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode);
    uint64_t sys_close(uint64_t handle);
    uint64_t sys_getpid();
    uint64_t sys_fork();
    uint64_t sys_exec(uint64_t path, uint64_t argv, uint64_t envp);
    uint64_t sys_wait(uint64_t pid, uint64_t status, uint64_t options);
    uint64_t sys_kill(uint64_t pid, uint64_t sig);
    uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot);
    uint64_t sys_mmap_file(uint64_t argsPtr);
    uint64_t sys_msync(uint64_t addr, uint64_t length, uint64_t flags);
    uint64_t sys_fd_path(uint64_t handle, uint64_t buf, uint64_t size);
    uint64_t sys_fdtable_stash(uint64_t ptr, uint64_t count);
    uint64_t sys_fdtable_fetch(uint64_t ptr, uint64_t count);
    uint64_t sys_munmap(uint64_t addr, uint64_t length);
    uint64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot);
    uint64_t sys_yield();
    uint64_t sys_sched_setaffinity(uint64_t pid, uint64_t size, uint64_t maskPtr);
    uint64_t sys_sched_getaffinity(uint64_t pid, uint64_t size, uint64_t maskPtr);
    uint64_t sys_getcpu();
    uint64_t sys_cpu_info(uint64_t index, uint64_t entryPtr);
    uint64_t sys_sleep(uint64_t ms);
    uint64_t sys_gettime();
    uint64_t sys_getunixtime();
    uint64_t sys_clear();
    uint64_t sys_fb_info(uint64_t info_ptr);
    uint64_t sys_fb_map();
    uint64_t sys_signal(uint64_t sig, uint64_t handler);
    uint64_t sys_sigreturn();
    uint64_t sys_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset);
    uint64_t sys_sigaction(uint64_t sig, uint64_t act, uint64_t oldact);
    uint64_t sys_sigaltstack(uint64_t stack, uint64_t oldstack);
    uint64_t sys_thread_signal(uint64_t handle, uint64_t sig);
    uint64_t sys_set_thread_pointer(uint64_t pointer);
    uint64_t sys_osinfo(uint64_t info_ptr);
    uint64_t sys_procinfo(uint64_t entriesPtr, uint64_t capacity, uint64_t totalPtr);
    uint64_t sys_storage_info(uint64_t infoPtr);
    uint64_t sys_storage_format();
    uint64_t sys_storage_mount();

    uint64_t sys_login(uint64_t);
    uint64_t sys_logout(uint64_t);
    uint64_t sys_getuid();
    uint64_t sys_getgid();
    uint64_t sys_setuid(uint64_t);
    uint64_t sys_setgid(uint64_t);
    uint64_t sys_getsessionid();
    uint64_t sys_getsessioninfo(uint64_t, uint64_t);
    
    uint64_t sys_chdir(uint64_t path);
    uint64_t sys_getcwd(uint64_t buf, uint64_t size);
    uint64_t sys_mkdir(uint64_t path, uint64_t mode);
    uint64_t sys_rmdir(uint64_t path);
    uint64_t sys_unlink(uint64_t path);
    uint64_t sys_stat(uint64_t path, uint64_t statbuf);
    uint64_t sys_fstat(uint64_t handle, uint64_t statbuf);
    uint64_t sys_fsync(uint64_t handle);
    uint64_t sys_lstat(uint64_t path, uint64_t statbuf);
    uint64_t sys_dup(uint64_t handle);
    uint64_t sys_dup2(uint64_t oldHandle, uint64_t newHandle);
    uint64_t sys_pipe(uint64_t pipeHandles);
    uint64_t sys_pipe2(uint64_t pipeHandles, uint64_t flags);
    uint64_t sys_fcntl(uint64_t handle, uint64_t command, uint64_t value);
    uint64_t sys_ioctl(uint64_t handle, uint64_t request, uint64_t arg);
    uint64_t sys_access(uint64_t path, uint64_t mode);
    uint64_t sys_statfs(uint64_t target, uint64_t byHandle, uint64_t statbuf);
    uint64_t sys_chown(uint64_t target, uint64_t byHandle, uint64_t uid, uint64_t gid, uint64_t flags);
    uint64_t sys_mknod(uint64_t path, uint64_t mode, uint64_t dev);
    uint64_t sys_get_entropy(uint64_t buffer, uint64_t length);
    uint64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeoutMs);
    uint64_t sys_truncate(uint64_t target, uint64_t size, uint64_t byHandle);
    uint64_t sys_rename(uint64_t oldPath, uint64_t newPath);
    uint64_t sys_chmod(uint64_t target, uint64_t mode, uint64_t byHandle);
    uint64_t sys_utime(uint64_t target, uint64_t atime, uint64_t mtime, uint64_t byHandle);
    uint64_t sys_link(uint64_t oldPath, uint64_t newPath);
    uint64_t sys_symlink(uint64_t target, uint64_t linkPath);
    uint64_t sys_readlink(uint64_t path, uint64_t buffer, uint64_t size);
    uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol);
    uint64_t sys_bind(uint64_t socketHandle, uint64_t address, uint64_t addressLength);
    uint64_t sys_connect(uint64_t socketHandle, uint64_t address, uint64_t addressLength);
    uint64_t sys_listen(uint64_t socketHandle, uint64_t backlog);
    uint64_t sys_accept(uint64_t socketHandle, uint64_t address, uint64_t addressLength);
    uint64_t sys_send(uint64_t socketHandle, uint64_t buffer, uint64_t length, uint64_t flags);
    uint64_t sys_recv(uint64_t socketHandle, uint64_t buffer, uint64_t length, uint64_t flags);
    uint64_t sys_shutdown(uint64_t socketHandle, uint64_t how);
    uint64_t sys_getsockopt(uint64_t socketHandle, uint64_t level, uint64_t optionName, uint64_t optionValue, uint64_t optionLength);
    uint64_t sys_setsockopt(uint64_t socketHandle, uint64_t level, uint64_t optionName, uint64_t optionValue, uint64_t optionLength);
    uint64_t sys_getsockname(uint64_t socketHandle, uint64_t addrPtr, uint64_t addrLenPtr);
    uint64_t sys_getpeername(uint64_t socketHandle, uint64_t addrPtr, uint64_t addrLenPtr);
    uint64_t sys_getppid();
    uint64_t sys_spawn(uint64_t path, uint64_t argv, uint64_t envp);
    uint64_t sys_thread_create(uint64_t entry, uint64_t arg, uint64_t stackSize);
    uint64_t sys_thread_clone(uint64_t entry, uint64_t userStack);
    uint64_t sys_thread_exit(uint64_t code);
    uint64_t sys_thread_join(uint64_t handle, uint64_t statusPtr);
    uint64_t sys_seek(uint64_t handle, uint64_t offset, uint64_t whence);
    uint64_t sys_gpu_capset_info(uint64_t infoPtr);
    uint64_t sys_gpu_capset(uint64_t dataPtr);
    uint64_t sys_gpu_context_create(uint64_t createPtr);
    uint64_t sys_gpu_context_destroy(uint64_t ctxId);
    uint64_t sys_gpu_resource_create_3d(uint64_t createPtr);
    uint64_t sys_gpu_resource_destroy(uint64_t destroyPtr);
    uint64_t sys_gpu_resource_assign_uuid(uint64_t uuidPtr);
    uint64_t sys_gpu_submit_3d(uint64_t submitPtr);
    uint64_t sys_gpu_wait_fence(uint64_t waitPtr);
    uint64_t sys_gpu_venus_probe(uint64_t probePtr);
    uint64_t sys_gpu_venus_vulkan(uint64_t resultPtr);
    uint64_t sys_getuserinfo(uint64_t uid, uint64_t info_ptr);
    uint64_t sys_readdir(uint64_t path, uint64_t entries, uint64_t count);
    uint64_t sys_fb_flush(uint64_t x, uint64_t y, uint64_t w, uint64_t h);
    uint64_t sys_shared_alloc(uint64_t size);
    uint64_t sys_shared_map(uint64_t handle);
    uint64_t sys_shared_free(uint64_t handle);
    uint64_t sys_surface_create(uint64_t width, uint64_t height, uint64_t format);
    uint64_t sys_surface_map(uint64_t handle);
    uint64_t sys_surface_commit(uint64_t handle, uint64_t x, uint64_t y, uint64_t packedWH);
    uint64_t sys_surface_poll(uint64_t infoPtr);
    uint64_t sys_compositor_create_window(uint64_t compositorHandle, uint64_t width, uint64_t height, uint64_t flags);
    uint64_t sys_window_event_queue(uint64_t windowHandle);
    uint64_t sys_window_set_title(uint64_t windowHandle, uint64_t titlePtr);
    uint64_t sys_window_attach_surface(uint64_t windowHandle, uint64_t surfaceHandle);
    uint64_t sys_window_list(uint64_t entriesPtr, uint64_t capacity);
    uint64_t sys_window_focus(uint64_t windowId);
    uint64_t sys_window_move(uint64_t windowId, uint64_t x, uint64_t y);
    uint64_t sys_window_resize(uint64_t windowId, uint64_t width, uint64_t height);
    uint64_t sys_window_control(uint64_t windowId, uint64_t action);
    uint64_t sys_queue_create();
    uint64_t sys_queue_send(uint64_t handle, uint64_t message, uint64_t wait);
    uint64_t sys_queue_receive(uint64_t handle, uint64_t message, uint64_t wait);
    uint64_t sys_queue_reply(uint64_t handle, uint64_t requestID, uint64_t data, uint64_t size);
    uint64_t sys_queue_request(uint64_t handle, uint64_t request, uint64_t response, uint64_t responseCapacity, uint64_t responseSize);
    uint64_t sys_service_register(uint64_t name, uint64_t queueHandle);
    uint64_t sys_service_connect(uint64_t name);
    
    uint64_t sys_net_get_mac(uint64_t mac_ptr);
    uint64_t sys_net_send(uint64_t data, uint64_t len);
    uint64_t sys_net_recv(uint64_t buffer, uint64_t maxlen);
    uint64_t sys_net_link_status();
    uint64_t sys_net_ping(uint64_t dest_ip, uint64_t id, uint64_t seq);
    uint64_t sys_net_process_packets();
    uint64_t sys_net_get_ping_reply(uint64_t reply_ptr);
    uint64_t sys_net_get_config(uint64_t config_ptr, uint64_t known_generation, uint64_t timeout_ms);
    uint64_t sys_net_set_config(uint64_t config_ptr, uint64_t mode);
    uint64_t sys_sendto(uint64_t socket, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address);
    uint64_t sys_recvfrom(uint64_t socket, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address_out);
    uint64_t sys_gettime_ns();
    uint64_t sys_setitimer(uint64_t which, uint64_t new_value, uint64_t old_value);
    uint64_t sys_getitimer(uint64_t which, uint64_t value);
    uint64_t sys_get_unix_time_ns();
    uint64_t sys_set_unix_time(uint64_t unix_ns);
    uint64_t sys_window_post_event(uint64_t window_id, uint64_t event_ptr);
    uint64_t sys_kernel_log(uint64_t buffer, uint64_t length);
};

extern "C" void syscallEntry();
extern "C" uint64_t syscallHandler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
