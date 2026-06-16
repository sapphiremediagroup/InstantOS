# InstantOS Syscall Reference

This document describes every syscall number currently defined by InstantOS.
The source of truth is `SyscallNumber` in `include/cpu/syscall/syscall.hpp`.
Userland wrappers live in `outside/iUserApps/outside/ilibcxx/include/syscall.hpp`.

## ABI

InstantOS uses the x86_64 `syscall` instruction.

The C++ userland wrapper `_syscall_impl(number, arg1, arg2, arg3, arg4, arg5)`
packs arguments for the kernel as:

| Value | Register at `syscall` |
| --- | --- |
| syscall number | `rax` |
| arg1 | `rbx` |
| arg2 | `r10` |
| arg3 | `rdx` |
| arg4 | `r8` |
| arg5 | `r9` |
| return value | `rax` |

POSIX-facing syscalls return non-negative values on success and `-errno` on
failure, encoded in `rax` as a two's-complement `uint64_t`. The libc C wrappers
decode that form, set `errno`, and return the POSIX failure value (`-1`,
`nullptr`, or `SIG_ERR`). Older non-POSIX service syscalls may still return
`UINT64_MAX` (`(uint64_t)-1`) while they are migrated; libc treats that legacy
sentinel as a wrapper-specific fallback errno instead of exposing it directly.

User pointers are checked with `Syscall::isValidUserPointer`, and strings are
copied from user memory into fixed kernel buffers. Path-like strings are limited
to 255 characters plus a NUL byte by current implementations.

## Process Creation Model

InstantOS currently follows a `posix_spawn()`-first model. `fork()` is exposed
for source compatibility but returns `ENOSYS`; portable userland should call
`posix_spawn()` or `posix_spawnp()`, which map to the kernel `Spawn` syscall and
return the new child PID. File action support is intentionally reported as
`ENOSYS` until per-action spawn descriptor editing is implemented.

## Common Types

Important user-visible structs are declared in both the kernel syscall header
and the userland syscall header:

| Type | Purpose |
| --- | --- |
| `OSInfo` | OS name, current user name, CPU name, memory counters, version fields. |
| `ProcInfoEntry` | Process snapshot entry with pid, parent pid, uid/gid, session id, scheduler state, priority, flags, exit code, and name. |
| `StorageInfo` | Persistent block-device snapshot with capacity, sector size, read/write/mount flags, mount error, device name, filesystem type, and mount path. |
| `SigActionInfo` | Kernel signal action record with handler, signal mask, flags, and user restorer trampoline. |
| `SignalStackInfo` | Kernel signal alternate-stack record with stack base, size, and flags. |
| `LoginInfo` | Username and password passed to `Login`. |
| `SessionInfo` | Session id, uid, gid, leader pid, login time, and session state. |
| `Stat` | File metadata returned by `Stat`. |
| `UserInfo` | UID, GID, username, home directory, and shell. |
| `DirEntry` | Directory entry returned by `Readdir`. |
| `FBInfo` | Framebuffer address, geometry, pitch, masks, and size. |
| `SurfaceInfo` | Compositor-facing committed surface metadata. |
| `WindowInfo` | Window id, owner, state, geometry, surface, z-order, and title. |
| `IPCMessage` | Message queue payload header and up to 256 bytes of data. |
| `Event` | Typed input/window event payload embedded in `IPCMessage`. |
| `GPUCapsetInfo` | VirtIO GPU capset query/result record. |
| `GPUCapsetData` | VirtIO GPU capset data request/result record. |
| `GPUContextCreate` | VirtIO GPU context creation request/result record. |
| `GPUResourceCreate3D` | VirtIO GPU 3D resource creation request/result record. |
| `GPUResourceDestroy` | VirtIO GPU 3D resource destruction request. |
| `GPUResourceUUID` | VirtIO GPU resource UUID assignment request/result record. |
| `GPUSubmit3D` | VirtIO GPU command submission request/result record. |
| `GPUWaitFence` | VirtIO GPU fence wait request/result record. |

## Syscalls

| No. | Name | Wrapper | Arguments | Return | Notes |
| ---: | --- | --- | --- | --- | --- |
| 0 | `OSInfo` | `osinfo` | `OSInfo* info` | `0` or `-errno` | Fills OS, user, CPU, memory, and version fields. |
| 1 | `ProcInfo` | none | `ProcInfoEntry* entries, uint64_t capacity, uint64_t* total` | entries copied or `-errno` | Snapshots scheduler processes. `capacity == 0` with a valid `total` pointer can be used to query the total process count without copying entries. |
| 2 | `Exit` | `exit` | `uint64_t code` | does not return normally | Marks the current process terminated and schedules another process. |
| 3 | `Write` | `write` | `FileHandle handle, const void* buffer, uint64_t count` | bytes written or `-errno` | Handles `stdout`/`stderr` specially by drawing text to the console. Other handles require write rights. |
| 4 | `Read` | `read` | `FileHandle handle, void* buffer, uint64_t count` | bytes read or `-errno` | `stdin` blocks until keyboard input is available. Other handles require read rights. |
| 5 | `Open` | `open` | `const char* path, uint64_t flags, uint64_t mode` | file handle or `-errno` | Uses VFS open. Access bits choose read/write handle rights. Unknown flags and access mode `3` return `-EINVAL`. `O_CLOEXEC` marks the handle for close during a successful `execve()`. `O_NOFOLLOW` rejects final-component symlinks with `-ELOOP`. |
| 6 | `Close` | `close` | `Handle handle` | `0` or `-errno` | Refuses standard handles `0`, `1`, and `2`. Closes either a file descriptor or generic handle. |
| 7 | `GetPID` | `getpid` | none | current PID, or `0` if no current process | Returns the scheduler's current process id. |
| 8 | `Fork` | `fork` | none | `-ENOSYS` | Exposed only for source compatibility; use `posix_spawn()` / `Spawn` for new child processes. |
| 9 | `Exec` | `execve` / `exec` | `const char* path, const char* const* argv, const char* const* envp` | does not return on success, or `-errno` | Replaces the current process image with a user ELF binary, preserves the process identity and non-`FD_CLOEXEC` handle table entries, resets signal handlers, and builds argv/envp/auxv on the new user stack. |
| 10 | `Wait` | `wait` / `waitpid` | `int64_t pid, int* status, uint64_t options` | child PID, `0` for `WNOHANG`, or `-errno` | Waits for a matching child to terminate, writes POSIX-style exit status bits, reaps the child, and supports `WNOHANG`. |
| 11 | `Kill` | `kill` | `uint64_t pid, uint64_t sig` | `0` or `-errno` | Sends a signal to an existing process. |
| 12 | `Mmap` | `mmap` | `void* address, uint64_t length, uint64_t protection` | mapped user address or `-errno` | Allocates zeroed physical frames and maps them with read/write/execute bits from `protection`. A zero protection value keeps legacy native-wrapper read/write no-execute behavior; POSIX `mmap(PROT_NONE)` is enforced by libc with a follow-up `mprotect()`. Libc supports file-backed `MAP_PRIVATE` as an eager private copy and writable `MAP_SHARED` with userspace `msync()` / `munmap()` writeback. |
| 13 | `Munmap` | `munmap` | `void* address, uint64_t length` | `0` or `-errno` | Unmaps present pages and frees their physical frames. |
| 14 | `Yield` | `yield` | none | `0` | Yields the CPU to the scheduler. |
| 15 | `Sleep` | `sleep` | `uint64_t ms` | `0` or `-1` | Blocks until the millisecond timer reaches the target time. |
| 16 | `GetTime` | `gettime` | none | milliseconds | Returns `Timer::get().getMilliseconds()`. |
| 17 | `Clear` | `clear` | none | `0` | Writes ANSI clear-screen escape text to the console. |
| 18 | `FBInfo` | `fb_info` | `FBInfo* info` | `0` or `-1` | Restricted to `/bin/graphics-compositor`. Maps framebuffer and fills metadata. |
| 19 | `FBMap` | `fb_map` | none | framebuffer user address or `-1` | Restricted to `/bin/graphics-compositor`. Maps framebuffer at the fixed user framebuffer base. |
| 20 | `Signal` | `signal` | `uint64_t sig, sighandler_t handler` | old handler or `-errno` | Compatibility handler install for `sig < NSIG`; rejects `SIGKILL` and clears the per-signal mask/flags. |
| 21 | `SigReturn` | `sigreturn` | none | `0` or `-errno` | Restores saved `rip`, previous blocked signal mask, and advances `rsp` by 128 bytes after signal delivery. |
| 22 | `Login` | `login` | `const LoginInfo* info` | session id or `-1` | Authenticates username/password, creates or updates the caller's session, UID, and GID. |
| 23 | `Logout` | `logout` | `uint64_t session_id` | `0` or `-1` | Destroys a session if owned by the caller or caller is privileged. Clears caller credentials if logging out of its own session. |
| 24 | `GetUID` | `getuid` | none | uid or `-1` | Returns the current process UID. |
| 25 | `GetGID` | `getgid` | none | gid or `-1` | Returns the current process GID. |
| 26 | `SetUID` | `setuid` | `uint64_t uid` | `0` or `-1` | Non-privileged callers may only set their current UID. Target user must exist. Updates leader session UID. |
| 27 | `SetGID` | `setgid` | `uint64_t gid` | `0` or `-1` | Non-privileged callers may only set their current GID. Updates leader session GID. |
| 28 | `GetSessionID` | `getsessionid` | none | session id or `-1` | Returns the current process session id. |
| 29 | `GetSessionInfo` | `getsessioninfo` | `uint64_t session_id, SessionInfo* info` | `0` or `-1` | Requires same UID as the session or a privileged caller. |
| 30 | `Chdir` | `chdir` | `const char* path` | `0` or `-1` | Opens the path for validation, closes it, then stores it as the process cwd. |
| 31 | `Getcwd` | `getcwd` | `char* buffer, size_t size` | `buffer` or `-1` | Copies the process cwd including the NUL byte. |
| 32 | `Mkdir` | `mkdir` | `const char* path, uint64_t mode` | `0` or `-1` | Calls VFS `mkdir`. |
| 33 | `Rmdir` | `rmdir` | `const char* path` | `0` or `-1` | Calls VFS `rmdir`. |
| 34 | `Unlink` | `unlink` | `const char* path` | `0` or `-1` | Calls VFS `unlink`. |
| 35 | `Stat` | `stat` | `const char* path, Stat* statbuf` | `0` or `-1` | Converts VFS file stats into the user-visible `Stat` record. Adds directory or regular-file mode bits. |
| 36 | `Dup` | `dup` | `Handle handle` | duplicated handle or `-1` | Duplicates a handle in the current process. The duplicate starts with close-on-exec disabled. |
| 37 | `Dup2` | `dup2` | `Handle oldHandle, Handle newHandle` | `newHandle` or `-1` | Duplicates `oldHandle` into the requested handle slot/value. The destination starts with close-on-exec disabled. |
| 38 | `Pipe` | `pipe` | `Handle* pipeHandles` | `0` or `-errno` | Creates readable and writable pipe file handles backed by an in-kernel ring buffer. Reads block while empty until data or writer close, writes block while full, and reads return EOF after all writers close. |
| 39 | `Getppid` | `getppid` | none | parent PID, or `0` if no current process | Returns the current process parent PID. |
| 40 | `Spawn` | `spawn` / `posix_spawn` | `const char* path, const char* const* argv, const char* const* envp` | child PID or `-1` | Loads a new ELF process with up to 64 argv and envp strings, and inherits UID/GID/session/cwd from caller. |
| 41 | `GetUserInfo` | `getuserinfo` | `uint64_t uid, UserInfo* info` | `0` or `-1` | Copies user database fields for the requested UID. |
| 42 | `Readdir` | `readdir` | `const char* path, DirEntry* entries, uint64_t count` | entries read or `-1` | Reads up to `count` directory entries into user memory. |
| 43 | `FBFlush` | `fb_flush` | `uint64_t x, uint64_t y, uint64_t w, uint64_t h` | `0` or `-1` | Restricted to `/bin/graphics-compositor`. Flushes VirtIO GPU framebuffer if available. |
| 44 | `SharedAlloc` | `shared_alloc` | `uint64_t size` | shared-memory handle or `-1` | Creates a shared memory object with map and duplicate rights. |
| 45 | `SharedMap` | `shared_map` | `Handle handle` | mapped address or `-1` | Maps a shared-memory handle. Also accepts a surface handle and maps the surface. |
| 46 | `SharedFree` | `shared_free` | `Handle handle` | `0` or `-1` | Unmaps shared memory from the caller and closes the shared-memory handle. |
| 47 | `SurfaceCreate` | `surface_create` | `uint32_t width, uint32_t height, uint32_t format` | surface handle or `-1` | Creates a compositor surface object. |
| 48 | `SurfaceMap` | `surface_map` | `Handle surface` | mapped address or `-1` | Maps a surface object into the caller. |
| 49 | `SurfaceCommit` | `surface_commit` | `Handle surface, uint32_t dirtyX, uint32_t dirtyY, packed dirtyWidth/dirtyHeight` | `0` or `-1` | Marks a surface dirty and posts a compositor service event. The wrapper packs width in high 32 bits and height in low 32 bits of arg4. |
| 50 | `SurfacePoll` | `surface_poll` | `SurfaceInfo* info` | `0` or `-1` | Restricted to `/bin/graphics-compositor`. Returns one committed surface and maps it into the compositor. |
| 51 | `CompositorCreateWindow` | `compositor_create_window` | `Handle compositor, uint32_t width, uint32_t height, uint32_t flags` | window handle or `-1` | Requires a service handle for `graphics.compositor`, creates a window, and focuses it. |
| 52 | `WindowEventQueue` | `window_event_queue` | `Handle window` | event queue handle or `-1` | Duplicates the window's event queue into the caller. |
| 53 | `WindowSetTitle` | `window_set_title` | `Handle window, const char* title` | `0` or `-1` | Sets a window title, limited to 63 bytes plus NUL. |
| 54 | `WindowAttachSurface` | `window_attach_surface` | `Handle window, Handle surface` | `0` or `-1` | Attaches a surface to a window. |
| 55 | `WindowList` | `compositor_list_windows` | `WindowInfo* windows, uint64_t capacity` | window count or `-1` | Restricted to `/bin/graphics-compositor`. `capacity` must be nonzero and no larger than `IPCManager::MaxWindows`. |
| 56 | `WindowFocus` | `compositor_focus_window` | `uint64_t windowId` | `0` or `-1` | Restricted to `/bin/graphics-compositor`. Focuses a window by id. |
| 57 | `WindowMove` | `compositor_move_window` | `uint64_t windowId, int32_t x, int32_t y` | `0` or `-1` | Restricted to `/bin/graphics-compositor`. Moves a window. |
| 58 | `WindowResize` | `compositor_resize_window` | `uint64_t windowId, int32_t width, int32_t height` | `0` or `-1` | Restricted to `/bin/graphics-compositor`. Resizes a window. |
| 59 | `WindowControl` | `compositor_control_window` | `uint64_t windowId, WindowControlAction action` | `0` or `-1` | Restricted to `/bin/graphics-compositor`. Applies restore/minimize/maximize/close. |
| 60 | `QueueCreate` | `queue_create` | none | queue handle or `-1` | Creates an IPC/event queue handle. |
| 61 | `QueueSend` | `queue_send` | `Handle queueOrService, const IPCMessage* message, bool wait` | `0` or `-1` | Enqueues an IPC message. If full, blocks only when `wait` is true. |
| 62 | `QueueReceive` | `queue_receive` | `Handle queueOrService, IPCMessage* message, bool wait` | `0` or `-1` | Dequeues an IPC message. If empty, blocks only when `wait` is true. |
| 63 | `QueueReply` | `queue_reply` | `Handle queueOrService, uint64_t requestId, const void* data, uint64_t size` | `0` or `-1` | Completes a pending request with up to `MessageQueueObject::MaxPayloadSize` bytes. |
| 64 | `QueueRequest` | `queue_request` | `Handle queueOrService, const IPCMessage* request, void* response, uint64_t responseCapacity, uint64_t* responseSize` | `0` or `-1` | Sends a request message, blocks for its reply, copies the response and optional response size. |
| 65 | `ServiceRegister` | `service_register` | `const char* name, Handle queueHandle` | service handle/id or `-1` | Registers a named service backed by an event queue. Publishing `input.manager` also flushes buffered keyboard input. |
| 66 | `ServiceConnect` | `service_connect` | `const char* name` | service handle or `-1` | Opens a handle to a registered service. |
| 67 | `NetGetMAC` | none | `uint8_t* mac` | `0` or `-errno` | Initializes the VirtIO-net driver if needed and copies the negotiated device MAC address. Returns `-ENOENT` when no supported VirtIO-net device is available. |
| 68 | `NetSend` | none | `const void* data, uint64_t len` | bytes sent or `-errno` | Sends one raw Ethernet frame through the VirtIO-net transmit queue. The driver copies the frame into a DMA buffer, notifies the device, polls briefly for completion, and reclaims completed transmit buffers. |
| 69 | `NetRecv` | none | `void* buffer, uint64_t maxlen` | bytes received or `-errno` | Receives one raw Ethernet frame from the VirtIO-net receive queue. The driver preposts receive DMA buffers, polls the used ring, copies one frame to the caller, and reposts the buffer. Empty receive returns `-EAGAIN`. |
| 70 | `NetLinkStatus` | none | none | `0` or `1` | Initializes the VirtIO-net driver if needed and reports link status. Devices without negotiated status support are treated as link-up after successful initialization. |
| 71 | `NetPing` | none | `uint64_t dest_ip, uint64_t id, uint64_t seq` | `0`, `-EAGAIN`, or `-errno` | Sends an ICMP echo request to a host-order IPv4 address. If the destination MAC is unknown, sends an ARP request, stores one pending ping, and returns `-EAGAIN`; callers should poll `NetProcessPackets` and retry or wait for ARP resolution. |
| 72 | `NetProcessPackets` | none | none | packets processed or `-errno` | Polls raw VirtIO-net receive frames, handles ARP requests/replies, sends pending ICMP echo requests after ARP resolution, records ICMP echo replies, and demuxes IPv4 UDP payloads into bound datagram sockets by destination port. |
| 73 | `NetGetPingReply` | none | `NetPingReply* reply` | `0` or `-errno` | Copies and consumes the last ICMP echo reply. The reply layout is `uint32_t src_ip; uint16_t id; uint16_t seq; uint16_t payload_size; uint16_t reserved;`. Empty reply state returns `-EAGAIN`. |
| 74 | `ThreadCreate` | `thread_create` | `ThreadStartRoutine start, void* arg, uint64_t stackSize` | thread handle or `-1` | Creates a thread-like process sharing caller state. Stack defaults to 16 pages and must be between 4 and 256 pages after alignment. |
| 75 | `ThreadExit` | `thread_exit` | `uint64_t code` | does not return normally | Exits the current thread. If called by a non-thread process, falls back to `Exit`. |
| 76 | `ThreadJoin` | `thread_join` | `ThreadHandle handle, int* status` | `0` or `-1` | Waits for a thread object to complete, copies exit status if requested, and closes the thread handle. |
| 77 | `Seek` | `seek` | `FileHandle handle, int64_t offset, uint64_t whence` | new offset or `-1` | `whence` values are `0` set, `1` current, and `2` end. Requires read or write rights. |
| 78 | `GPUCapsetInfo` | `gpu_capset_info` | `GPUCapsetInfo* info` | `0` or `-1` | Reads `index` from user memory and fills capset id, max version, and max size. |
| 79 | `GPUCapset` | `gpu_capset` | `GPUCapsetData* data` | `0` or `-1` | Copies capset data into the user buffer and updates `actualSize`. |
| 80 | `GPUContextCreate` | `gpu_context_create` | `GPUContextCreate* create` | `0` or `-1` | Creates a VirtIO GPU context and writes back `ctxId`. |
| 81 | `GPUContextDestroy` | `gpu_context_destroy` | `uint32_t ctxId` | `0` or `-1` | Destroys a VirtIO GPU context. |
| 82 | `GPUResourceCreate3D` | `gpu_resource_create_3d` | `GPUResourceCreate3D* create` | `0` or `-1` | Creates a VirtIO GPU 3D resource and writes back `resourceId`. |
| 83 | `GPUResourceDestroy` | `gpu_resource_destroy` | `GPUResourceDestroy* destroy` | `0` or `-1` | Destroys a VirtIO GPU 3D resource, optionally with backing. |
| 84 | `GPUResourceAssignUUID` | `gpu_resource_assign_uuid` | `GPUResourceUUID* uuid` | `0` or `-1` | Assigns/queries a resource UUID and copies the record back to user memory. |
| 85 | `GPUSubmit3D` | `gpu_submit_3d` | `GPUSubmit3D* submit` | `0` or `-1` | Copies command bytes from user memory, submits them, and writes transport/response/fence status back. |
| 86 | `GPUWaitFence` | `gpu_wait_fence` | `GPUWaitFence* wait` | `0` or `-1` | Waits for a fence using `timeoutIterations` or a default spin limit, then writes completion fields back. |
| 87 | `GetUnixTime` | `getunixtime` | none | unix timestamp | Returns RTC-derived Unix time. |
| 88 | `SerialWrite` | `serial_write` | `const void* buffer, uint64_t count` | bytes written or `-1` | Writes raw bytes to the serial console. |
| 89 | `Fcntl` | `fcntl` | `Handle handle, uint64_t command, uint64_t value` | command result or `-errno` | Supports `F_GETFD` and `F_SETFD` for `FD_CLOEXEC`. |
| 90 | `Mprotect` | `mprotect` | `void* address, uint64_t length, uint64_t protection` | `0` or `-1` | Updates read/write/execute protections for an existing user mapping. |
| 91 | `Poll` | `poll` / `select` | `PollFD* fds, uint64_t nfds` | ready descriptor count or `-errno` | Nonblocking readiness scan for stdin/stdout/stderr, regular files, pipes, event queues, service queues, and socket handles. Libc implements POSIX `poll()` timeouts and `select()` on top of file descriptors; native `std::poll()` may pass encoded InstantOS queue/service handles. |
| 92 | `Truncate` | `truncate` / `ftruncate` | path or handle, `uint64_t size`, by-handle flag | `0` or `-errno` | Resizes regular files through VFS. |
| 93 | `Rename` | `rename` | `const char* oldPath, const char* newPath` | `0` or `-errno` | Renames within one mounted filesystem. |
| 94 | `Chmod` | `chmod` / `fchmod` | path or handle, `uint64_t mode`, by-handle flag | `0` or `-errno` | Updates VFS mode bits. |
| 95 | `Utime` | `utime` / `utimensat` / `futimens` | path or handle, `uint64_t atime`, `uint64_t mtime`, by-handle flag | `0` or `-errno` | Updates access and modification times. Libc maps nanosecond APIs to second-resolution VFS timestamps and supports `UTIME_NOW` / `UTIME_OMIT` in userspace. |
| 96 | `Fstat` | `fstat` | `Handle handle, Stat* statbuf` | `0` or `-errno` | Converts an open file descriptor into the user-visible `Stat` record. |
| 97 | `Link` | `link` | `const char* oldPath, const char* newPath` | `0` or `-errno` | Creates a hard link within one mounted filesystem. RamFS supports regular-file hard links with shared file data, metadata, inode, and link count. |
| 98 | `Symlink` | `symlink` | `const char* target, const char* linkPath` | `0` or `-errno` | Creates a symbolic link. The link target is stored verbatim, so relative symlink targets remain relative. |
| 99 | `Readlink` | `readlink` | `const char* path, char* buffer, uint64_t size` | bytes copied or `-errno` | Reads a symlink target without appending a NUL terminator. |
| 100 | `Lstat` | `lstat` | `const char* path, Stat* statbuf` | `0` or `-errno` | Stats the final path component without following it when it is a symlink. |
| 101 | `Sigprocmask` | `sigprocmask` / `pthread_sigmask` | `uint64_t how, const sigset_t* set, sigset_t* oldset` | `0` or `-errno` | Reads and updates the current thread's signal mask. Supports `SIG_BLOCK`, `SIG_UNBLOCK`, and `SIG_SETMASK`; `SIGKILL` cannot be blocked. |
| 102 | `Socket` | `socket` | `uint64_t domain, uint64_t type, uint64_t protocol` | socket handle or `-errno` | Creates an AF_INET stream or datagram socket handle with duplicate/read/write/control rights. Raw sockets and unsupported address families return POSIX errors. |
| 103 | `Bind` | `bind` | `Handle socket, const sockaddr* address, socklen_t length` | `0` or `-errno` | Stores a local address on a socket handle. Port zero allocates an ephemeral port and duplicate local port/type binds return `-EADDRINUSE` unless both sockets allow reuse. |
| 104 | `Connect` | `connect` | `Handle socket, const sockaddr* address, socklen_t length` | `0` or `-errno` | Stores datagram peer addresses. Stream sockets connect to local listening AF_INET sockets, auto-bind an ephemeral client port when needed, and queue a server-side accepted socket. Non-local stream sockets send a TCP SYN after ARP resolution and return `-EAGAIN` while the handshake is in progress. |
| 105 | `Listen` | `listen` | `Handle socket, uint64_t backlog` | `0` or `-errno` | Marks a stream socket as listening and records a bounded local accept backlog. |
| 106 | `Accept` | `accept` | `Handle socket, sockaddr* address, socklen_t* length` | socket handle or `-errno` | Dequeues a pending local stream connection, returns `-EAGAIN` when no connection is queued, and reports queued connections through `poll()`/`select()` readability. |
| 107 | `Send` | `send` / `sendto` | `Handle socket, const void* buffer, uint64_t length, uint64_t flags` | bytes sent or `-errno` | Validates socket handles, user buffers, and shutdown state. Connected local UDP datagrams are queued to bound local peers; non-local UDP datagrams are sent as IPv4 UDP frames after ARP resolution and return `-EAGAIN` while ARP is being resolved. Connected local streams queue bytes to their peer. Connected non-local streams send TCP PSH/ACK payload frames. |
| 108 | `Recv` | `recv` / `recvfrom` | `Handle socket, void* buffer, uint64_t length, uint64_t flags` | bytes received or `-errno` | Validates socket handles, user buffers, and shutdown state. Local UDP receives dequeue one datagram or return `-EAGAIN` when empty. `NetProcessPackets` demuxes non-local UDP and TCP payloads into socket receive queues. Local and non-local stream receives support partial reads and return `-EAGAIN` when empty. |
| 109 | `Shutdown` | `shutdown` | `Handle socket, uint64_t how` | `0` or `-errno` | Marks read, write, or both directions closed for a socket handle. |
| 110 | `GetSockOpt` | `getsockopt` | `Handle socket, level, optname, void* optval, socklen_t* optlen` | `0` or `-errno` | Supports `SOL_SOCKET` options `SO_TYPE`, `SO_ERROR`, `SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`, `SO_RCVBUF`, `SO_SNDBUF`, and `SO_LINGER`. |
| 111 | `SetSockOpt` | `setsockopt` | `Handle socket, level, optname, const void* optval, socklen_t optlen` | `0` or `-errno` | Stores writable `SOL_SOCKET` options used by common POSIX probes. |
| 112 | `StorageInfo` | none | `StorageInfo* info` | `0` or `-errno` | Copies the boot-time persistent storage snapshot. Reports whether AHCI storage was detected, its capacity, 512-byte sector size, read/write capability, FAT32 mount path, and mount error. |
| 113 | `StorageFormat` | none | none | `0` or `-errno` | Formats the detected unmounted persistent AHCI disk as a minimal FAT32 volume. Refuses missing, read-only, too-small, non-512-byte-sector, or already mounted devices. |
| 114 | `StorageMount` | none | none | `0` or `-errno` | Attempts to mount the detected persistent disk as FAT32 at `/`. It is idempotent when the disk is already mounted and updates `StorageInfo` mount status/errors. |
| 115 | `Sigaction` | `sigaction` | `uint64_t sig, const SigActionInfo* act, SigActionInfo* oldact` | `0` or `-errno` | Installs or queries kernel-backed signal action state: handler, signal mask, flags, restorer trampoline, and old-action return. Rejects installing actions for `SIGKILL`. |
| 116 | `Sigaltstack` | `sigaltstack` | `const SignalStackInfo* stack, SignalStackInfo* oldstack` | `0` or `-errno` | Installs, disables, or queries the current thread's alternate signal stack. Rejects invalid flags, too-small stacks, and changes while running on the alternate stack. |
| 117 | `ThreadSignal` | `pthread_kill` | `Handle thread, uint64_t sig` | `0` or `-errno` | Queues a signal to a specific thread handle in the current process. Signal `0` is a validity check. |
| 118 | `SetThreadPointer` | mlibc sysdeps | `uint64_t pointer` | `0` or `-errno` | Sets the current thread's x86_64 FS base for libc TLS/TCB support and saves it across scheduler context switches. |

## Aliases and Helper Wrappers

The userland header also exposes convenience helpers that do not have distinct
syscall numbers:

| Helper | Uses |
| --- | --- |
| `duplicate_handle` | `Dup` |
| `duplicate_handle_to` | `Dup2` |
| `event_queue_create` | `QueueCreate` |
| `event_push` | `QueueSend` |
| `event_wait` | `QueueReceive` with blocking wait |
| `event_poll` | `QueueReceive` without blocking wait |
| `make_event_message` | Pure userland helper for packing an `Event` into `IPCMessage` |
| `event_from_message` | Pure userland helper for unpacking an `Event` from `IPCMessage` |

## Implementation Gaps

- `Fork` is a compatibility stub that returns `-ENOSYS`.
- `ProcInfo` snapshots the scheduler process list into fixed-size
  `ProcInfoEntry` records. `/bin/process-manager` registers `process.manager`
  and periodically reports the real process snapshot through that syscall.
- Signal action state is kernel-backed for `signal()` and `sigaction()`:
  handlers, masks, flags, restorer trampolines, and old-action returns are stored
  per process/thread image. Pending signal delivery runs on syscall return or
  scheduler return, honors per-thread blocked masks and delivered action masks,
  supports alternate stacks via `sigaltstack()`, and restores saved context via
  the libc restorer calling `SigReturn`. `SIGKILL` cannot be blocked or handled;
  `SIGCHLD` defaults to ignored; other default actions terminate. `read()` and
  `waitpid()` return `EINTR` for delivered signals and libc retries those calls
  when an installed action uses `SA_RESTART`.
- The pthread layer supports kernel-backed thread creation/join/detach plus
  libc-managed normal, recursive, and error-checking mutexes; condition
  variables with timed waits; writer-preferred rwlocks; unnamed process-local
  semaphores; per-thread `errno`; pthread TLS keys with destructors; deferred
  cancellation; `pthread_sigmask()`; and `pthread_kill()`. Blocking pthread
  waits currently cooperate with the scheduler through yield loops rather than a
  futex-style kernel wait queue.
- An optional mlibc port scaffold lives under
  `outside/iUserApps/outside/mlibc-sysdeps/instantos`. It provides Meson sysdeps
  wiring, x86_64 syscall entry wrappers with InstantOS `-errno` translation,
  startup glue compatible with the kernel argv/envp/auxv stack, and `TcbSet`
  backed by `SetThreadPointer`. The default userland still links against
  `ilibcxx`; mlibc builds are opt-in through `INSTANTOS_ENABLE_MLIBC`.
- `StorageInfo` exposes boot-time AHCI/FAT32 state. `/bin/storage-manager`
  registers `storage.manager` and reports persistent block-device capacity,
  sector size, read/write support, mount status, filesystem type, mount path,
  and mount errors. `/bin/storage-manager format` calls `StorageFormat` for an
  unmounted persistent disk, and `/bin/storage-manager mount` calls
  `StorageMount` to mount an already formatted FAT32 disk at `/`. FAT32 metadata
  is written through the block device and reloaded by the boot-time mount path
  on later boots.
- `NetGetMAC`, `NetSend`, `NetRecv`, and `NetLinkStatus` are wired to initial
  VirtIO-net discovery, feature negotiation, RX/TX queues, polling-based raw
  frame I/O, and link/MAC reporting. Ethernet, ARP, IPv4, ICMP echo, UDP, and
  a minimal TCP subset are implemented. TCP currently has no retransmission,
  window scaling, congestion control, options, or robust close/reset handling.
- `/bin/network-manager` registers `network.manager`, reports clear startup
  diagnostics for missing VirtIO-net devices, and periodically calls
  `NetProcessPackets` so ARP/ICMP/UDP/TCP demux progresses without every client
  needing its own packet pump.
- POSIX socket descriptors, socket options, local loopback delivery, basic
  VirtIO-net UDP/TCP routing, shutdown state, and poll validity are implemented.
  TCP remains a minimal subset without retransmission, options, or robust close
  handling.

## Storage Smoke

Run `tools/run-storage-smoke.sh` to build the ISO and boot QEMU against three
isolated AHCI disk images under `build/storage-smoke`: an empty disk image, a
host-formatted FAT32 image, and a formatted image with a corrupted boot sector.
Each case writes its own serial log in the same directory and leaves the normal
`build/ahci.img` untouched.

## Networking Smoke

The initrd includes `/bin/network-manager`, which registers the
`network.manager` service and polls packet processing, and `/bin/net-smoke`, a
small syscall-level networking probe for VirtIO-net. It checks link/MAC
discovery, raw receive empty behavior, packet processing, and an ICMP echo
attempt against QEMU user networking's gateway.

Run `tools/run-network-smoke.sh` to build the ISO and boot QEMU with
`virtio-net-pci` plus user-mode networking. Serial output is written to
`build/network-smoke.serial.log` by default. Inside the guest, run
`/bin/net-smoke` from a shell or launch it through future boot automation.

## Venus (Vulkan over virtio-gpu)

The kernel implements the guest side of Mesa's Venus protocol
(`VK_MESA_venus_protocol`, virtio-gpu capset id 4) in `src/graphics/venus.cpp`.
At boot, `runVenusProbe()` negotiates the Venus capset, creates a Venus context,
allocates a shared command/reply blob, and performs a real
`vkEnumerateInstanceVersion` round trip against the host renderer, printing
`[VENUS]` markers to serial.

Userspace can run the same probe via the `GPUVenusProbeCall` syscall
(`gpu_venus_probe(GPUVenusProbe*)`), which reports availability, the negotiated
capset/protocol versions, and the host Vulkan instance version.

Run `tools/run-venus-smoke.sh` to build the ISO and boot QEMU with a
Venus-capable `virtio-gpu-gl-pci` device; the script greps the serial log for a
successful round trip. See `docs/venus.md` for full details, host requirements,
and debugging notes.
