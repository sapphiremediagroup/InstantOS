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

Most syscalls return `0` on success and `UINT64_MAX` (`(uint64_t)-1`) on
failure. Syscalls that naturally return a handle, pointer, PID, count, or old
handler return that value on success and usually return `UINT64_MAX` on failure.

User pointers are checked with `Syscall::isValidUserPointer`, and strings are
copied from user memory into fixed kernel buffers. Path-like strings are limited
to 255 characters plus a NUL byte by current implementations.

## Common Types

Important user-visible structs are declared in both the kernel syscall header
and the userland syscall header:

| Type | Purpose |
| --- | --- |
| `OSInfo` | OS name, current user name, CPU name, memory counters, version fields. |
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
| 0 | `OSInfo` | `osinfo` | `OSInfo* info` | `0` or `-1` | Fills OS, user, CPU, memory, and version fields. |
| 1 | `ProcInfo` | none | none | `0` | Defined and dispatched, but currently returns `0` without filling process info. |
| 2 | `Exit` | `exit` | `uint64_t code` | does not return normally | Marks the current process terminated and schedules another process. |
| 3 | `Write` | `write` | `FileHandle handle, const void* buffer, uint64_t count` | bytes written or `-1` | Handles `stdout`/`stderr` specially by drawing text to the console. Other handles require write rights. |
| 4 | `Read` | `read` | `FileHandle handle, void* buffer, uint64_t count` | bytes read or `-1` | `stdin` blocks until keyboard input is available. Other handles require read rights. |
| 5 | `Open` | `open` | `const char* path, uint64_t flags, uint64_t mode` | file handle or `-1` | Uses VFS open. Access bits choose read/write handle rights. `mode` is currently ignored by the kernel open path. |
| 6 | `Close` | `close` | `Handle handle` | `0` or `-1` | Refuses standard handles `0`, `1`, and `2`. Closes either a file descriptor or generic handle. |
| 7 | `GetPID` | `getpid` | none | current PID, or `0` if no current process | Returns the scheduler's current process id. |
| 8 | `Fork` | `fork` | none | `-1` | Defined but not implemented. |
| 9 | `Exec` | `exec` | `const char* path, const char* const* argv, const char* const* envp` | `0` or `-1` | Loads a user binary with up to 64 argv strings. Current behavior creates a new process, sets parent PID, schedules, and ignores `envp`. |
| 10 | `Wait` | `wait` | `uint64_t pid, int* status` | `0` or `-1` | Validates that `pid` is a child of the caller. Currently writes status `0` if requested and does not block for termination. |
| 11 | `Kill` | `kill` | `uint64_t pid, uint64_t sig` | `0` or `-1` | Sends a signal to an existing process. |
| 12 | `Mmap` | `mmap` | `void* address, uint64_t length, uint64_t protection` | mapped user address or `-1` | Allocates zeroed physical frames and maps them user read/write no-execute. `protection` is ignored. |
| 13 | `Munmap` | `munmap` | `void* address, uint64_t length` | `0` or `-1` | Unmaps present pages and frees their physical frames. |
| 14 | `Yield` | `yield` | none | `0` | Yields the CPU to the scheduler. |
| 15 | `Sleep` | `sleep` | `uint64_t ms` | `0` or `-1` | Blocks until the millisecond timer reaches the target time. |
| 16 | `GetTime` | `gettime` | none | milliseconds | Returns `Timer::get().getMilliseconds()`. |
| 17 | `Clear` | `clear` | none | `0` | Writes ANSI clear-screen escape text to the console. |
| 18 | `FBInfo` | `fb_info` | `FBInfo* info` | `0` or `-1` | Restricted to `/bin/graphics-compositor.exe`. Maps framebuffer and fills metadata. |
| 19 | `FBMap` | `fb_map` | none | framebuffer user address or `-1` | Restricted to `/bin/graphics-compositor.exe`. Maps framebuffer at the fixed user framebuffer base. |
| 20 | `Signal` | `signal` | `uint64_t sig, sighandler_t handler` | old handler or `-1` | Installs a signal handler for `sig < NSIG`. |
| 21 | `SigReturn` | `sigreturn` | none | `0` or `-1` | Restores `rip` from the current saved user stack and advances `rsp` by 128 bytes. |
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
| 36 | `Dup` | `dup` | `Handle handle` | duplicated handle or `-1` | Duplicates a handle in the current process. |
| 37 | `Dup2` | `dup2` | `Handle oldHandle, Handle newHandle` | `newHandle` or `-1` | Duplicates `oldHandle` into the requested handle slot/value. |
| 38 | `Pipe` | `pipe` | `Handle* pipeHandles` | `-1` | Validates the output pointer, but pipe creation is not implemented. |
| 39 | `Getppid` | `getppid` | none | parent PID, or `0` if no current process | Returns the current process parent PID. |
| 40 | `Spawn` | `spawn` | `const char* path, const char* const* argv, const char* const* envp` | child PID or `-1` | Loads a new process with up to 64 argv strings, inherits UID/GID/session/cwd from caller, and ignores `envp`. |
| 41 | `GetUserInfo` | `getuserinfo` | `uint64_t uid, UserInfo* info` | `0` or `-1` | Copies user database fields for the requested UID. |
| 42 | `Readdir` | `readdir` | `const char* path, DirEntry* entries, uint64_t count` | entries read or `-1` | Reads up to `count` directory entries into user memory. |
| 43 | `FBFlush` | `fb_flush` | `uint64_t x, uint64_t y, uint64_t w, uint64_t h` | `0` or `-1` | Restricted to `/bin/graphics-compositor.exe`. Flushes VirtIO GPU framebuffer if available. |
| 44 | `SharedAlloc` | `shared_alloc` | `uint64_t size` | shared-memory handle or `-1` | Creates a shared memory object with map and duplicate rights. |
| 45 | `SharedMap` | `shared_map` | `Handle handle` | mapped address or `-1` | Maps a shared-memory handle. Also accepts a surface handle and maps the surface. |
| 46 | `SharedFree` | `shared_free` | `Handle handle` | `0` or `-1` | Unmaps shared memory from the caller and closes the shared-memory handle. |
| 47 | `SurfaceCreate` | `surface_create` | `uint32_t width, uint32_t height, uint32_t format` | surface handle or `-1` | Creates a compositor surface object. |
| 48 | `SurfaceMap` | `surface_map` | `Handle surface` | mapped address or `-1` | Maps a surface object into the caller. |
| 49 | `SurfaceCommit` | `surface_commit` | `Handle surface, uint32_t dirtyX, uint32_t dirtyY, packed dirtyWidth/dirtyHeight` | `0` or `-1` | Marks a surface dirty and posts a compositor service event. The wrapper packs width in high 32 bits and height in low 32 bits of arg4. |
| 50 | `SurfacePoll` | `surface_poll` | `SurfaceInfo* info` | `0` or `-1` | Restricted to `/bin/graphics-compositor.exe`. Returns one committed surface and maps it into the compositor. |
| 51 | `CompositorCreateWindow` | `compositor_create_window` | `Handle compositor, uint32_t width, uint32_t height, uint32_t flags` | window handle or `-1` | Requires a service handle for `graphics.compositor`, creates a window, and focuses it. |
| 52 | `WindowEventQueue` | `window_event_queue` | `Handle window` | event queue handle or `-1` | Duplicates the window's event queue into the caller. |
| 53 | `WindowSetTitle` | `window_set_title` | `Handle window, const char* title` | `0` or `-1` | Sets a window title, limited to 63 bytes plus NUL. |
| 54 | `WindowAttachSurface` | `window_attach_surface` | `Handle window, Handle surface` | `0` or `-1` | Attaches a surface to a window. |
| 55 | `WindowList` | `compositor_list_windows` | `WindowInfo* windows, uint64_t capacity` | window count or `-1` | Restricted to `/bin/graphics-compositor.exe`. `capacity` must be nonzero and no larger than `IPCManager::MaxWindows`. |
| 56 | `WindowFocus` | `compositor_focus_window` | `uint64_t windowId` | `0` or `-1` | Restricted to `/bin/graphics-compositor.exe`. Focuses a window by id. |
| 57 | `WindowMove` | `compositor_move_window` | `uint64_t windowId, int32_t x, int32_t y` | `0` or `-1` | Restricted to `/bin/graphics-compositor.exe`. Moves a window. |
| 58 | `WindowResize` | `compositor_resize_window` | `uint64_t windowId, int32_t width, int32_t height` | `0` or `-1` | Restricted to `/bin/graphics-compositor.exe`. Resizes a window. |
| 59 | `WindowControl` | `compositor_control_window` | `uint64_t windowId, WindowControlAction action` | `0` or `-1` | Restricted to `/bin/graphics-compositor.exe`. Applies restore/minimize/maximize/close. |
| 60 | `QueueCreate` | `queue_create` | none | queue handle or `-1` | Creates an IPC/event queue handle. |
| 61 | `QueueSend` | `queue_send` | `Handle queueOrService, const IPCMessage* message, bool wait` | `0` or `-1` | Enqueues an IPC message. If full, blocks only when `wait` is true. |
| 62 | `QueueReceive` | `queue_receive` | `Handle queueOrService, IPCMessage* message, bool wait` | `0` or `-1` | Dequeues an IPC message. If empty, blocks only when `wait` is true. |
| 63 | `QueueReply` | `queue_reply` | `Handle queueOrService, uint64_t requestId, const void* data, uint64_t size` | `0` or `-1` | Completes a pending request with up to `MessageQueueObject::MaxPayloadSize` bytes. |
| 64 | `QueueRequest` | `queue_request` | `Handle queueOrService, const IPCMessage* request, void* response, uint64_t responseCapacity, uint64_t* responseSize` | `0` or `-1` | Sends a request message, blocks for its reply, copies the response and optional response size. |
| 65 | `ServiceRegister` | `service_register` | `const char* name, Handle queueHandle` | service handle/id or `-1` | Registers a named service backed by an event queue. Publishing `input.manager` also flushes buffered keyboard input. |
| 66 | `ServiceConnect` | `service_connect` | `const char* name` | service handle or `-1` | Opens a handle to a registered service. |
| 67 | `NetGetMAC` | none | intended `uint8_t* mac` | `-1` through default dispatcher | Defined in headers and diagnostics, but there is no dispatcher case and no implementation in the current tree. |
| 68 | `NetSend` | none | intended `const void* data, uint64_t len` | `-1` through default dispatcher | Defined in headers and diagnostics, but there is no dispatcher case and no implementation in the current tree. |
| 69 | `NetRecv` | none | intended `void* buffer, uint64_t maxlen` | `-1` through default dispatcher | Defined in headers and diagnostics, but there is no dispatcher case and no implementation in the current tree. |
| 70 | `NetLinkStatus` | none | none | `-1` through default dispatcher | Defined in headers and diagnostics, but there is no dispatcher case and no implementation in the current tree. |
| 71 | `NetPing` | none | intended `uint64_t dest_ip, uint64_t id, uint64_t seq` | `-1` through default dispatcher | Defined in headers and diagnostics, but there is no dispatcher case and no implementation in the current tree. |
| 72 | `NetProcessPackets` | none | none | `-1` through default dispatcher | Defined in headers and diagnostics, but there is no dispatcher case and no implementation in the current tree. |
| 73 | `NetGetPingReply` | none | intended reply pointer | `-1` through default dispatcher | Defined in headers and diagnostics, but there is no dispatcher case and no implementation in the current tree. |
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

- `Fork` and `Pipe` are stubs that return `-1`.
- `ProcInfo` is dispatched but returns `0` without any process information.
- `NetGetMAC` through `NetGetPingReply` are present in the syscall enum and
  diagnostic name table, but they are not handled by `Syscall::handle` and no
  `sys_net_*` implementations are present in the current source tree.
- `Exec` and `Spawn` parse `argv`, but both ignore `envp`.
- `Mmap` ignores the protection argument and always maps user read/write,
  no-execute pages.
