#ifndef ROMCLOUD_NETSURF_SHM_H
#define ROMCLOUD_NETSURF_SHM_H

#include <stdint.h>
#include <stdbool.h>

#define NETSURF_SHM_NAME "/romcloud_web_shm"
#define NETSURF_SHM_FILE "/tmp/romcloud_web_shm"
#define NETSURF_SHM_MAGIC 0x4E53484D /* 'NSHM' */
#define NETSURF_EVENT_RING_SIZE 128

enum NetSurfShmEventType {
    NETSURF_SHM_EVENT_NONE = 0,
    NETSURF_SHM_EVENT_KEY_DOWN = 1,
    NETSURF_SHM_EVENT_KEY_UP = 2,
    NETSURF_SHM_EVENT_MOVE_ABS = 3,
    NETSURF_SHM_EVENT_CONTROL_QUIT = 4,
};

/* Matches NetSurf nsfb_key_code_e */
enum NetSurfShmKeyCode {
    NS_SHM_KEY_UNKNOWN       = 0,
    NS_SHM_KEY_BACKSPACE     = 8,
    NS_SHM_KEY_TAB           = 9,
    NS_SHM_KEY_RETURN        = 13,
    NS_SHM_KEY_ESCAPE        = 27,
    NS_SHM_KEY_SPACE         = 32,
    NS_SHM_KEY_UP            = 273,
    NS_SHM_KEY_DOWN          = 274,
    NS_SHM_KEY_RIGHT         = 275,
    NS_SHM_KEY_LEFT          = 276,
    NS_SHM_KEY_PAGEUP        = 280,
    NS_SHM_KEY_PAGEDOWN      = 281,
    NS_SHM_KEY_MOUSE_1       = 401, /* Left click */
    NS_SHM_KEY_MOUSE_2       = 402, /* Middle click */
    NS_SHM_KEY_MOUSE_3       = 403, /* Right click */
    NS_SHM_KEY_MOUSE_4       = 404, /* Wheel up */
    NS_SHM_KEY_MOUSE_5       = 405, /* Wheel down */
};

struct NetSurfShmEvent {
    uint32_t type;     /* NetSurfShmEventType */
    int32_t keycode;   /* NetSurfShmKeyCode or ASCII */
    int32_t x;         /* 0..width */
    int32_t y;         /* 0..height */
};

struct NetSurfShmHeader {
    uint32_t magic;           /* NETSURF_SHM_MAGIC */
    uint32_t width;           /* Viewport width (1024) */
    uint32_t height;          /* Viewport height (603) */
    uint32_t stride;          /* width * 4 */
    uint32_t format;          /* 0 = ARGB8888 */
    volatile uint32_t frame_seq; /* Incremented by NetSurf on every redraw */
    
    /* Input ring buffer (RomCloud writes, NetSurf reads) */
    volatile uint32_t event_write_idx;
    volatile uint32_t event_read_idx;
    struct NetSurfShmEvent events[NETSURF_EVENT_RING_SIZE];
    
    /* Process synchronization */
    volatile uint32_t ready;          /* 1 when NetSurf initialized */
    volatile uint32_t quit_requested; /* 1 when RomCloud asks NetSurf to quit */
    
    /* Navigation command slot (RomCloud -> NetSurf) */
    volatile uint32_t nav_req_seq;
    volatile uint32_t nav_ack_seq;
    char target_url[1024];
};

#endif /* ROMCLOUD_NETSURF_SHM_H */
