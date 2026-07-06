/*
 * rfb_client.c - minimal RFB 3.8 client (Raw encoding, security None).
 * Windows / Winsock. See rfb_client.h for the contract.
 *
 * Protocol reference: RFC 6143.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "rfb_client.h"

#pragma comment(lib, "ws2_32.lib")

static char g_err[256];
static void set_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_err, sizeof(g_err) - 1, fmt, ap);
    va_end(ap);
}
const char *rfb_last_error(void) { return g_err; }

struct rfb_client {
    SOCKET            sock;
    int               fb_w, fb_h;

    /* double buffer: pump thread writes back, game thread reads front */
    uint8_t          *buf[2];        /* RGBA, fb_w * fb_h * 4 each        */
    int               front;         /* index game thread reads           */
    uint32_t          serial;        /* increments per completed update   */
    bool              have_frame;

    CRITICAL_SECTION  fb_lock;       /* guards buf/front/serial           */
    CRITICAL_SECTION  tx_lock;       /* serializes socket writes          */

    HANDLE            thread;
    volatile LONG     running;
};

/* ---------------------------------------------------------------- io -- */

static bool read_exact(SOCKET s, void *dst, int len) {
    char *p = (char *)dst;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

static bool write_exact(SOCKET s, const void *src, int len) {
    const char *p = (const char *)src;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = (v >> 16) & 0xff; p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
}

/* --------------------------------------------------------- handshake -- */

static bool handshake(rfb_client_t *c) {
    char ver[13] = {0};
    if (!read_exact(c->sock, ver, 12)) { set_err("no protocol version"); return false; }
    /* We speak 3.8. QEMU always offers >= 3.8. */
    if (!write_exact(c->sock, "RFB 003.008\n", 12)) return false;

    uint8_t nsec;
    if (!read_exact(c->sock, &nsec, 1)) return false;
    if (nsec == 0) { set_err("server refused connection (security)"); return false; }

    uint8_t types[255];
    if (!read_exact(c->sock, types, nsec)) return false;
    bool has_none = false;
    for (int i = 0; i < nsec; i++) if (types[i] == 1) has_none = true;
    if (!has_none) {
        set_err("server requires VNC auth; launch qemu with plain '-vnc 127.0.0.1:0'");
        return false;
    }
    uint8_t pick = 1; /* None */
    if (!write_exact(c->sock, &pick, 1)) return false;

    uint8_t res[4];
    if (!read_exact(c->sock, res, 4) || rd32(res) != 0) {
        set_err("security handshake failed"); return false;
    }

    uint8_t shared = 1; /* ClientInit */
    if (!write_exact(c->sock, &shared, 1)) return false;

    uint8_t si[24];      /* ServerInit fixed part */
    if (!read_exact(c->sock, si, 24)) return false;
    c->fb_w = rd16(si + 0);
    c->fb_h = rd16(si + 2);
    uint32_t name_len = rd32(si + 20);
    /* drain desktop name */
    while (name_len > 0) {
        char tmp[64];
        uint32_t n = name_len > sizeof(tmp) ? (uint32_t)sizeof(tmp) : name_len;
        if (!read_exact(c->sock, tmp, (int)n)) return false;
        name_len -= n;
    }

    /*
     * SetPixelFormat: 32bpp truecolor, little-endian, red shift 0 /
     * green 8 / blue 16, i.e. bytes land in memory as R,G,B,A which is
     * exactly GL_RGBA + GL_UNSIGNED_BYTE. The server does the conversion
     * from TempleOS's 4-bit palette for us.
     */
    uint8_t spf[20] = {0};
    spf[0] = 0;              /* message type SetPixelFormat */
    spf[4] = 32;             /* bits-per-pixel  */
    spf[5] = 24;             /* depth           */
    spf[6] = 0;              /* big-endian flag */
    spf[7] = 1;              /* true-colour     */
    wr16(spf + 8, 255); wr16(spf + 10, 255); wr16(spf + 12, 255); /* maxima */
    spf[14] = 0;             /* red shift   */
    spf[15] = 8;             /* green shift */
    spf[16] = 16;            /* blue shift  */
    if (!write_exact(c->sock, spf, 20)) return false;

    /* SetEncodings: Raw only. */
    uint8_t se[8] = {0};
    se[0] = 2;               /* message type */
    wr16(se + 2, 1);         /* one encoding */
    wr32(se + 4, 0);         /* Raw = 0      */
    if (!write_exact(c->sock, se, 8)) return false;

    return true;
}

static bool send_fb_update_request(rfb_client_t *c, bool incremental) {
    uint8_t m[10] = {0};
    m[0] = 3; m[1] = incremental ? 1 : 0;
    wr16(m + 2, 0); wr16(m + 4, 0);
    wr16(m + 6, (uint16_t)c->fb_w); wr16(m + 8, (uint16_t)c->fb_h);
    EnterCriticalSection(&c->tx_lock);
    bool ok = write_exact(c->sock, m, 10);
    LeaveCriticalSection(&c->tx_lock);
    return ok;
}

/* --------------------------------------------------------- pump loop -- */

static bool handle_fb_update(rfb_client_t *c) {
    uint8_t hdr[3];
    if (!read_exact(c->sock, hdr, 3)) return false;   /* padding + nrects */
    uint16_t nrects = rd16(hdr + 1);

    /* Work on the back buffer; it always carries the previous full frame
       so incremental rects compose correctly. */
    uint8_t *back = c->buf[c->front ^ 1];

    for (uint16_t r = 0; r < nrects; r++) {
        uint8_t rh[12];
        if (!read_exact(c->sock, rh, 12)) return false;
        uint16_t x = rd16(rh + 0), y = rd16(rh + 2);
        uint16_t w = rd16(rh + 4), h = rd16(rh + 6);
        int32_t enc = (int32_t)rd32(rh + 8);

        if (enc != 0) { set_err("unexpected encoding %d", enc); return false; }
        if (x + w > c->fb_w || y + h > c->fb_h) { set_err("rect out of bounds"); return false; }

        for (uint16_t row = 0; row < h; row++) {
            uint8_t *dst = back + (((size_t)(y + row) * c->fb_w) + x) * 4;
            if (!read_exact(c->sock, dst, (int)w * 4)) return false;
        }
    }

    /* publish: swap front/back, then copy new front into new back so the
       next incremental update starts from a complete image. */
    EnterCriticalSection(&c->fb_lock);
    c->front ^= 1;
    memcpy(c->buf[c->front ^ 1], c->buf[c->front], (size_t)c->fb_w * c->fb_h * 4);
    c->serial++;
    c->have_frame = true;
    LeaveCriticalSection(&c->fb_lock);
    return true;
}

static DWORD WINAPI pump_thread(LPVOID param) {
    rfb_client_t *c = (rfb_client_t *)param;

    if (!send_fb_update_request(c, false)) goto done;  /* full first frame */

    while (InterlockedCompareExchange(&c->running, 1, 1) == 1) {
        uint8_t mtype;
        if (!read_exact(c->sock, &mtype, 1)) break;

        switch (mtype) {
        case 0: /* FramebufferUpdate */
            if (!handle_fb_update(c)) goto done;
            if (!send_fb_update_request(c, true)) goto done;
            break;
        case 1: { /* SetColourMapEntries: drain */
            uint8_t h[5];
            if (!read_exact(c->sock, h, 5)) goto done;
            uint16_t n = rd16(h + 3);
            for (uint16_t i = 0; i < n; i++) {
                uint8_t rgb[6];
                if (!read_exact(c->sock, rgb, 6)) goto done;
            }
            break;
        }
        case 2: /* Bell */
            break;
        case 3: { /* ServerCutText: drain */
            uint8_t h[7];
            if (!read_exact(c->sock, h, 7)) goto done;
            uint32_t len = rd32(h + 3);
            char tmp[128];
            while (len > 0) {
                uint32_t n = len > sizeof(tmp) ? (uint32_t)sizeof(tmp) : len;
                if (!read_exact(c->sock, tmp, (int)n)) goto done;
                len -= n;
            }
            break;
        }
        default:
            set_err("unknown server message %u", mtype);
            goto done;
        }
    }
done:
    InterlockedExchange(&c->running, 0);
    return 0;
}

/* --------------------------------------------------------- public api -- */

rfb_client_t *rfb_connect(const char *host, uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { set_err("WSAStartup failed"); return NULL; }

    rfb_client_t *c = (rfb_client_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->sock == INVALID_SOCKET) { set_err("socket() failed"); free(c); return NULL; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    inet_pton(AF_INET, host, &sa.sin_addr);

    if (connect(c->sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        set_err("connect to %s:%u failed (is qemu running?)", host, port);
        closesocket(c->sock); free(c); return NULL;
    }

    BOOL nd = TRUE;
    setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof(nd));

    if (!handshake(c)) { closesocket(c->sock); free(c); return NULL; }

    size_t sz = (size_t)c->fb_w * c->fb_h * 4;
    c->buf[0] = (uint8_t *)calloc(1, sz);
    c->buf[1] = (uint8_t *)calloc(1, sz);
    if (!c->buf[0] || !c->buf[1]) {
        set_err("out of memory for framebuffer");
        closesocket(c->sock); free(c->buf[0]); free(c->buf[1]); free(c);
        return NULL;
    }

    InitializeCriticalSection(&c->fb_lock);
    InitializeCriticalSection(&c->tx_lock);
    return c;
}

bool rfb_start(rfb_client_t *c) {
    InterlockedExchange(&c->running, 1);
    c->thread = CreateThread(NULL, 0, pump_thread, c, 0, NULL);
    if (!c->thread) { set_err("CreateThread failed"); return false; }
    return true;
}

void rfb_get_size(rfb_client_t *c, int *w, int *h) { *w = c->fb_w; *h = c->fb_h; }

const uint8_t *rfb_acquire_frame(rfb_client_t *c, uint32_t *frame_serial) {
    EnterCriticalSection(&c->fb_lock);
    if (!c->have_frame) { LeaveCriticalSection(&c->fb_lock); return NULL; }
    if (frame_serial) *frame_serial = c->serial;
    return c->buf[c->front];
}

void rfb_release_frame(rfb_client_t *c) { LeaveCriticalSection(&c->fb_lock); }

void rfb_send_key(rfb_client_t *c, uint32_t keysym, bool down) {
    uint8_t m[8] = {0};
    m[0] = 4; m[1] = down ? 1 : 0;
    wr32(m + 4, keysym);
    EnterCriticalSection(&c->tx_lock);
    write_exact(c->sock, m, 8);
    LeaveCriticalSection(&c->tx_lock);
}

void rfb_send_pointer(rfb_client_t *c, uint16_t x, uint16_t y, uint8_t buttons) {
    uint8_t m[6] = {0};
    m[0] = 5; m[1] = buttons;
    wr16(m + 2, x); wr16(m + 4, y);
    EnterCriticalSection(&c->tx_lock);
    write_exact(c->sock, m, 6);
    LeaveCriticalSection(&c->tx_lock);
}

bool rfb_is_connected(rfb_client_t *c) {
    return c && InterlockedCompareExchange(&c->running, 1, 1) == 1;
}

void rfb_disconnect(rfb_client_t *c) {
    if (!c) return;
    InterlockedExchange(&c->running, 0);
    shutdown(c->sock, SD_BOTH);
    closesocket(c->sock);
    if (c->thread) { WaitForSingleObject(c->thread, 2000); CloseHandle(c->thread); }
    DeleteCriticalSection(&c->fb_lock);
    DeleteCriticalSection(&c->tx_lock);
    free(c->buf[0]); free(c->buf[1]);
    free(c);
}
