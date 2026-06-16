/*
 * libnsfb surface backend for the InstantOS graphics compositor.
 *
 * This is the InstantOS-specific "instant" surface. It does NOT use the Linux
 * framebuffer device or SDL; instead it talks directly to the InstantOS
 * graphics compositor over the kernel IPC/syscall ABI:
 *
 *   service_connect("graphics.compositor")
 *     -> surface_create(w, h, BGRA8)
 *     -> shared_map(surface)              (gives the pixel buffer)
 *     -> compositor_create_window(comp, w, h)
 *     -> window_set_title(window, title)
 *     -> window_attach_surface(window, surface)
 *     -> window_event_queue(window)       (gives an event queue handle)
 *
 * Presentation: draw into the mapped BGRA8 buffer, then surface_commit() the
 * dirty rectangle. Input: queue_receive() the window's event queue and decode
 * Key/Pointer/Window events into nsfb_event_t.
 *
 * Registration is EXPLICIT via nsfb_instant_register() (see libnsfb.h) because
 * the InstantOS kernel does not run ELF init_array constructors, so the usual
 * NSFB_SURFACE_DEF(...) constructor-based registration never fires.
 *
 * All compositor interaction is done with raw inline-asm syscalls so this file
 * has no dependency on ilibcxx (which is C++); the browser is plain C.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#define UNUSED(x) ((x) = (x))

/* -------------------------------------------------------------------------
 * InstantOS syscall ABI
 *
 * SysV(rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5) is shuffled to the
 * kernel calling convention (rax=num, rbx=a1, r10=a2, rdx=a3, r8=a4, r9=a5)
 * by the wrapper below. Return value in rax. Failure when result >= -4095.
 * ------------------------------------------------------------------------- */

static inline uint64_t inst_syscall(uint64_t num, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a2;
    register uint64_t r8  __asm__("r8")  = a4;
    register uint64_t r9  __asm__("r9")  = a5;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "b"(a1), "r"(r10), "d"(a3), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

static inline bool sys_failed(uint64_t r)
{
    return r >= (uint64_t)-4095;
}

/* Syscall ordinals (see include/cpu/syscall/syscall.hpp / docs/syscalls.md). */
enum {
    SYS_CLOSE                    = 6,
    SYS_YIELD                    = 14,
    SYS_SHARED_MAP               = 45,
    SYS_SURFACE_CREATE           = 47,
    SYS_SURFACE_COMMIT           = 49,
    SYS_COMPOSITOR_CREATE_WINDOW = 51,
    SYS_WINDOW_EVENT_QUEUE       = 52,
    SYS_WINDOW_SET_TITLE         = 53,
    SYS_WINDOW_ATTACH_SURFACE    = 54,
    SYS_QUEUE_RECEIVE            = 62,
    SYS_SERVICE_CONNECT          = 66,
    SYS_POLL                     = 91,
};

/* Kernel pollfd layout (see src/cpu/syscall/syscall.hpp PollFD). */
struct inst_pollfd {
    long long fd;       /* kernel handle */
    short events;
    short revents;
};
#define INST_POLLIN 0x0001

#define COMPOSITOR_SERVICE "graphics.compositor"
#define SURFACE_FORMAT_BGRA8 1u

/* -------------------------------------------------------------------------
 * IPC message + event layouts (mirror include/cpu/syscall/syscall.hpp).
 * ------------------------------------------------------------------------- */

#define IPC_MESSAGE_EVENT 0x2u

struct ipc_message {
    uint64_t id;          /* 0 */
    uint32_t sender_pid;  /* 8 */
    uint16_t flags;       /* 12 */
    uint16_t reserved;    /* 14 */
    uint64_t size;        /* 16 */
    uint8_t  data[256];   /* 24 */
};

enum event_type {
    EVT_NONE = 0,
    EVT_KEY = 1,
    EVT_POINTER = 2,
    EVT_WINDOW = 3,
};

enum key_action  { KEY_PRESS = 0, KEY_RELEASE = 1, KEY_REPEAT = 2 };
enum key_mod     { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4,
                   MOD_CAPS = 8, MOD_SUPER = 16 };
enum ptr_action  { PTR_MOVE = 0, PTR_BUTTON = 1, PTR_SCROLL = 2 };
enum win_action  { WIN_NONE = 0, WIN_FOCUS_GAINED = 1, WIN_FOCUS_LOST = 2,
                   WIN_CLOSE_REQUESTED = 3, WIN_RESIZED = 4, WIN_MOVED = 5 };

struct key_event {
    uint16_t action;     /* 0 */
    uint16_t modifiers;  /* 2 */
    uint16_t keycode;    /* 4 */
    uint16_t reserved;   /* 6 */
    char     text[8];    /* 8 */
};

struct pointer_event {
    uint16_t action;     /* 0 */
    uint16_t buttons;    /* 2 */
    int32_t  x;          /* 4 */
    int32_t  y;          /* 8 */
    int32_t  delta_x;    /* 12 */
    int32_t  delta_y;    /* 16 */
};

struct window_event {
    uint16_t action;     /* 0 */
    uint16_t reserved0;  /* 2 */
    uint32_t window_id;  /* 4 */
    int32_t  x;          /* 8 */
    int32_t  y;          /* 12 */
    int32_t  width;      /* 16 */
    int32_t  height;     /* 20 */
};

struct ev {
    uint16_t type;       /* 0 */
    uint16_t reserved0;  /* 2 */
    uint32_t reserved1;  /* 4 */
    union {              /* 8 */
        struct key_event     key;
        struct pointer_event pointer;
        struct window_event  window;
        uint8_t              raw[48];
    } u;
};

/* -------------------------------------------------------------------------
 * Per-surface private state.
 * ------------------------------------------------------------------------- */

struct instant_priv {
    uint64_t compositor;   /* service handle */
    uint64_t surface;      /* surface handle */
    uint64_t window;       /* window handle */
    uint64_t events;       /* event queue handle */
    uint32_t *pixels;      /* mapped BGRA8 buffer */
    int last_x;            /* last absolute pointer position */
    int last_y;
    bool close_requested;
};

/* -------------------------------------------------------------------------
 * Compositor helpers.
 * ------------------------------------------------------------------------- */

static uint64_t connect_compositor(void)
{
    /* The compositor may not have registered the service yet; retry briefly. */
    for (int attempt = 0; attempt < 500; attempt++) {
        uint64_t h = inst_syscall(SYS_SERVICE_CONNECT,
                                  (uint64_t)(uintptr_t)COMPOSITOR_SERVICE,
                                  0, 0, 0, 0);
        if (!sys_failed(h))
            return h;
        inst_syscall(SYS_YIELD, 0, 0, 0, 0, 0);
    }
    return (uint64_t)-1;
}

/* -------------------------------------------------------------------------
 * libnsfb surface routines.
 * ------------------------------------------------------------------------- */

static int instant_defaults(nsfb_t *nsfb)
{
    nsfb->width = 1024;
    nsfb->height = 768;
    /* The compositor surface is BGRA8: bytes B,G,R,A in memory, i.e. a
     * little-endian uint32 of 0xAARRGGBB. In libnsfb's naming (component
     * order of the uint32) that is ARGB8888. */
    nsfb->format = NSFB_FMT_ARGB8888;

    select_plotters(nsfb);
    return 0;
}

static int instant_initialise(nsfb_t *nsfb)
{
    struct instant_priv *priv;
    int w = nsfb->width;
    int h = nsfb->height;

    if (w <= 0)
        w = 1024;
    if (h <= 0)
        h = 768;
    nsfb->width = w;
    nsfb->height = h;

    priv = calloc(1, sizeof(*priv));
    if (priv == NULL)
        return -1;
    priv->compositor = (uint64_t)-1;
    priv->surface = (uint64_t)-1;
    priv->window = (uint64_t)-1;
    priv->events = (uint64_t)-1;

    priv->compositor = connect_compositor();
    if (sys_failed(priv->compositor))
        goto fail;

    priv->surface = inst_syscall(SYS_SURFACE_CREATE,
                                 (uint32_t)w, (uint32_t)h,
                                 SURFACE_FORMAT_BGRA8, 0, 0);
    if (sys_failed(priv->surface))
        goto fail;

    {
        uint64_t mapped = inst_syscall(SYS_SHARED_MAP, priv->surface,
                                       0, 0, 0, 0);
        if (sys_failed(mapped) || mapped == 0)
            goto fail;
        priv->pixels = (uint32_t *)(uintptr_t)mapped;
    }

    priv->window = inst_syscall(SYS_COMPOSITOR_CREATE_WINDOW, priv->compositor,
                                (uint32_t)w, (uint32_t)h, 0, 0);
    if (sys_failed(priv->window))
        goto fail;

    inst_syscall(SYS_WINDOW_SET_TITLE, priv->window,
                 (uint64_t)(uintptr_t)"NetSurf", 0, 0, 0);

    if (sys_failed(inst_syscall(SYS_WINDOW_ATTACH_SURFACE,
                                priv->window, priv->surface, 0, 0, 0)))
        goto fail;

    priv->events = inst_syscall(SYS_WINDOW_EVENT_QUEUE, priv->window,
                                0, 0, 0, 0);
    if (sys_failed(priv->events))
        goto fail;

    /* nsfb draws straight into the compositor surface buffer. linelen MUST be
     * in BYTES (width * 4 for 32bpp); using a pixel stride here is a classic
     * fatal page-fault bug. */
    nsfb->ptr = (uint8_t *)priv->pixels;
    nsfb->linelen = w * 4;
    nsfb->surface_priv = priv;

    select_plotters(nsfb);
    return 0;

fail:
    if (!sys_failed(priv->events))
        inst_syscall(SYS_CLOSE, priv->events, 0, 0, 0, 0);
    if (!sys_failed(priv->window))
        inst_syscall(SYS_CLOSE, priv->window, 0, 0, 0, 0);
    if (!sys_failed(priv->surface))
        inst_syscall(SYS_CLOSE, priv->surface, 0, 0, 0, 0);
    if (!sys_failed(priv->compositor))
        inst_syscall(SYS_CLOSE, priv->compositor, 0, 0, 0, 0);
    free(priv);
    return -1;
}

static int instant_finalise(nsfb_t *nsfb)
{
    struct instant_priv *priv = nsfb->surface_priv;
    if (priv == NULL)
        return 0;

    if (!sys_failed(priv->events))
        inst_syscall(SYS_CLOSE, priv->events, 0, 0, 0, 0);
    if (!sys_failed(priv->window))
        inst_syscall(SYS_CLOSE, priv->window, 0, 0, 0, 0);
    if (!sys_failed(priv->surface))
        inst_syscall(SYS_CLOSE, priv->surface, 0, 0, 0, 0);
    if (!sys_failed(priv->compositor))
        inst_syscall(SYS_CLOSE, priv->compositor, 0, 0, 0, 0);

    free(priv);
    nsfb->surface_priv = NULL;
    nsfb->ptr = NULL;
    return 0;
}

static int instant_set_geometry(nsfb_t *nsfb, int width, int height,
                                enum nsfb_format_e format)
{
    /* The compositor surface size is fixed at creation; we only honour the
     * geometry request before initialise() has run. */
    if (nsfb->surface_priv != NULL)
        return -1;

    if (width > 0)
        nsfb->width = width;
    if (height > 0)
        nsfb->height = height;
    if (format != NSFB_FMT_ANY)
        nsfb->format = format;

    select_plotters(nsfb);
    return 0;
}

static int instant_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct instant_priv *priv = nsfb->surface_priv;
    int x0, y0, x1, y1, w, h;

    if (priv == NULL)
        return -1;

    if (box == NULL) {
        x0 = 0; y0 = 0; x1 = nsfb->width; y1 = nsfb->height;
    } else {
        x0 = box->x0; y0 = box->y0; x1 = box->x1; y1 = box->y1;
    }

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > nsfb->width)  x1 = nsfb->width;
    if (y1 > nsfb->height) y1 = nsfb->height;

    w = x1 - x0;
    h = y1 - y0;
    if (w <= 0 || h <= 0)
        return 0;

    /* SurfaceCommit packs (width << 32) | height into the 4th argument. */
    uint64_t packed = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    inst_syscall(SYS_SURFACE_COMMIT, priv->surface,
                 (uint32_t)x0, (uint32_t)y0, packed, 0);
    return 0;
}

/* Map an InstantOS keycode (mostly ASCII, with the special codes above 255)
 * into an nsfb keycode. For the common ASCII range the values coincide. */
static enum nsfb_key_code_e map_keycode(uint16_t code, const char *text)
{
    if (code != 0 && code < 128)
        return (enum nsfb_key_code_e)code;
    /* Fall back to the produced text byte if the raw code is non-ASCII. */
    if (text != NULL && text[0] != '\0' && (unsigned char)text[0] < 128)
        return (enum nsfb_key_code_e)(unsigned char)text[0];
    return (enum nsfb_key_code_e)code;
}

static bool instant_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct instant_priv *priv = nsfb->surface_priv;
    struct ipc_message msg;
    struct ev e;
    bool wait;

    if (priv == NULL)
        return false;

    if (priv->close_requested) {
        priv->close_requested = false;
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return true;
    }

    /* Wait policy:
     *   timeout < 0  -> block in queue_receive until an event arrives.
     *   timeout == 0 -> poll once, non-blocking.
     *   timeout > 0  -> NetSurf has a scheduled callback (e.g. an active fetch)
     *                   due in `timeout` ms. We must NOT busy-spin: doing a
     *                   non-blocking receive here returns immediately every
     *                   iteration, pegging the CPU at 100% and starving the
     *                   compositor / input pipeline (the browser appears to
     *                   "freeze" and input dies). Instead, sleep in the kernel
     *                   poll() on the event queue for up to `timeout` ms so the
     *                   process yields the CPU; we wake early if an input event
     *                   arrives, otherwise the caller re-runs its scheduler.
     */
    wait = (timeout < 0);

    if (timeout > 0) {
        struct inst_pollfd pfd;
        pfd.fd = (long long)priv->events;
        pfd.events = INST_POLLIN;
        pfd.revents = 0;
        /* Blocks (yielding the CPU) until the event queue is readable or the
         * timeout/a wakeup occurs. Ignore errors and fall through to a
         * non-blocking receive. */
        inst_syscall(SYS_POLL, (uint64_t)(uintptr_t)&pfd, 1,
                     (uint64_t)timeout, 0, 0);
        wait = false;  /* now drain whatever (if anything) is ready */
    }

    memset(&msg, 0, sizeof(msg));
    uint64_t r = inst_syscall(SYS_QUEUE_RECEIVE, priv->events,
                              (uint64_t)(uintptr_t)&msg, wait ? 1 : 0, 0, 0);
    if (sys_failed(r))
        return false;
    if ((msg.flags & IPC_MESSAGE_EVENT) == 0 || msg.size < sizeof(struct ev))
        return false;

    memcpy(&e, msg.data, sizeof(e));

    switch (e.type) {
    case EVT_KEY:
        if (e.u.key.action == KEY_RELEASE) {
            event->type = NSFB_EVENT_KEY_UP;
        } else {
            event->type = NSFB_EVENT_KEY_DOWN; /* press or repeat */
        }
        event->value.keycode = map_keycode(e.u.key.keycode, e.u.key.text);
        return true;

    case EVT_POINTER:
        if (e.u.pointer.action == PTR_SCROLL) {
            /* Encode scroll as a relative move on the z axis. */
            event->type = NSFB_EVENT_MOVE_RELATIVE;
            event->value.vector.x = 0;
            event->value.vector.y = 0;
            event->value.vector.z = e.u.pointer.delta_y;
            return true;
        }
        if (e.u.pointer.action == PTR_BUTTON) {
            /* buttons bitmask: bit0=left, bit1=right, bit2=middle. We can only
             * report one button press/up per event; pick the lowest set bit.
             * The browser primarily cares about button 1 (left). */
            enum nsfb_key_code_e btn = NSFB_KEY_MOUSE_1;
            if (e.u.pointer.buttons & 0x2)
                btn = NSFB_KEY_MOUSE_3; /* right */
            else if (e.u.pointer.buttons & 0x4)
                btn = NSFB_KEY_MOUSE_2; /* middle */
            /* A zero button mask means "all released" -> key up. */
            event->type = (e.u.pointer.buttons == 0)
                              ? NSFB_EVENT_KEY_UP
                              : NSFB_EVENT_KEY_DOWN;
            event->value.keycode = btn;
            return true;
        }
        /* PTR_MOVE: report absolute position. */
        priv->last_x = e.u.pointer.x;
        priv->last_y = e.u.pointer.y;
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = e.u.pointer.x;
        event->value.vector.y = e.u.pointer.y;
        event->value.vector.z = 0;
        return true;

    case EVT_WINDOW:
        if (e.u.window.action == WIN_CLOSE_REQUESTED) {
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_QUIT;
            return true;
        }
        /* Other window events (focus, resize, commit-ack) are not surfaced. */
        return false;

    default:
        return false;
    }
}

const nsfb_surface_rtns_t instant_rtns = {
    .defaults = instant_defaults,
    .initialise = instant_initialise,
    .finalise = instant_finalise,
    .input = instant_input,
    .geometry = instant_set_geometry,
    .update = instant_update,
};

/* Explicit registration entry point. NetSurf's framebuffer frontend calls this
 * once at startup (the kernel does not run init_array, so the usual
 * NSFB_SURFACE_DEF constructor never fires). */
void nsfb_instant_register(void)
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    _nsfb_register_surface(NSFB_SURFACE_INSTANT, &instant_rtns, "instant");
}

/*
 * Local variables:
 *  c-basic-offset: 4
 *  tab-width: 8
 * End:
 */
