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
    KeyModifierCapsLock = 1 << 3
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
#include <fs/vfs/vfs.hpp>

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
};

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
    uint64_t sys_exit(uint64_t code);
    uint64_t sys_write(uint64_t fileHandle, uint64_t buf, uint64_t count);
    uint64_t sys_read(uint64_t fileHandle, uint64_t buf, uint64_t count);
    uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode);
    uint64_t sys_close(uint64_t handle);
    uint64_t sys_getpid();
    uint64_t sys_fork();
    uint64_t sys_exec(uint64_t path, uint64_t argv, uint64_t envp);
    uint64_t sys_wait(uint64_t pid, uint64_t status);
    uint64_t sys_kill(uint64_t pid, uint64_t sig);
    uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot);
    uint64_t sys_munmap(uint64_t addr, uint64_t length);
    uint64_t sys_yield();
    uint64_t sys_sleep(uint64_t ms);
    uint64_t sys_gettime();
    uint64_t sys_clear();
    uint64_t sys_fb_info(uint64_t info_ptr);
    uint64_t sys_fb_map();
    uint64_t sys_signal(uint64_t sig, uint64_t handler);
    uint64_t sys_sigreturn();
    uint64_t sys_osinfo(uint64_t info_ptr);

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
    uint64_t sys_dup(uint64_t handle);
    uint64_t sys_dup2(uint64_t oldHandle, uint64_t newHandle);
    uint64_t sys_pipe(uint64_t pipeHandles);
    uint64_t sys_getppid();
    uint64_t sys_spawn(uint64_t path, uint64_t argv, uint64_t envp);
    uint64_t sys_thread_create(uint64_t entry, uint64_t arg, uint64_t stackSize);
    uint64_t sys_thread_exit(uint64_t code);
    uint64_t sys_thread_join(uint64_t handle, uint64_t statusPtr);
    uint64_t sys_seek(uint64_t handle, uint64_t offset, uint64_t whence);
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
};

extern "C" void syscallEntry();
extern "C" uint64_t syscallHandler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
