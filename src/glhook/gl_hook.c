/*
 * gl_hook.c - see gl_hook.h.
 *
 * Requires MinHook (third_party/minhook). BSD-2 licensed, compatible with
 * the HL SDK license. We hook the *engine's* GL usage from inside our own
 * mod's client.dll; no proxy opengl32.dll needed.
 *
 * !! Never run this on VAC-secured servers. Singleplayer / -insecure only.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_hook.h"
#include "MinHook.h"

#pragma comment(lib, "opengl32.lib")

/* ------------------------------------------------------------ config -- */

#define MAX_FINGERPRINTS 64
#define MAX_CAPTURES     512

typedef struct { int w, h; uint64_t hash; } fingerprint_t;

typedef struct { GLuint tex; int w, h; uint64_t hash; } capture_t;

static fingerprint_t g_fps[MAX_FINGERPRINTS];
static int           g_fp_count;

/* discovery mode: capture every screen-plausible upload and let the player
   cycle a "solo" highlight through them to identify a monitor in-game. */
static bool          g_discover;
static int           g_solo = -1;
static int           g_painted = -1;   /* capture idx currently overwritten */
static uint8_t      *g_orig;           /* saved original pixels of g_painted */
static size_t        g_orig_cap;

static capture_t     g_caps[MAX_CAPTURES];
static volatile LONG g_cap_count;
static CRITICAL_SECTION g_cap_lock;
static bool          g_cap_lock_init;

static bool          g_logging;
static char          g_log_path[MAX_PATH] = "toshl_uploads.log";

static uint32_t      g_last_serial = 0xFFFFFFFF;

/* scratch buffer for scaled blits, grown on demand */
static uint8_t      *g_scaled;
static size_t        g_scaled_cap;

/* ------------------------------------------------------------- hash -- */

static uint64_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static size_t bytes_per_pixel(GLenum format, GLenum type) {
    if (type != GL_UNSIGNED_BYTE) return 0; /* GoldSrc uploads are ubyte */
    switch (format) {
    case GL_RGBA: case 0x80E1 /*GL_BGRA*/: return 4;
    case GL_RGB:  case 0x80E0 /*GL_BGR */: return 3;
    case GL_LUMINANCE: case GL_ALPHA:      return 1;
    default: return 0;
    }
}

/* -------------------------------------------------------------- hook -- */

typedef void (WINAPI *glTexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                      GLint, GLenum, GLenum, const void *);
static glTexImage2D_t o_glTexImage2D;

static void remember_capture(GLuint tex, int w, int h, uint64_t hash) {
    EnterCriticalSection(&g_cap_lock);
    LONG n = g_cap_count;
    for (LONG i = 0; i < n; i++) {
        if (g_caps[i].tex == tex) { LeaveCriticalSection(&g_cap_lock); return; }
    }
    if (n < MAX_CAPTURES) {
        g_caps[n].tex = tex; g_caps[n].w = w; g_caps[n].h = h; g_caps[n].hash = hash;
        InterlockedIncrement(&g_cap_count);
    }
    LeaveCriticalSection(&g_cap_lock);
}

static void WINAPI hk_glTexImage2D(GLenum target, GLint level, GLint internalfmt,
                                   GLsizei w, GLsizei h, GLint border,
                                   GLenum format, GLenum type, const void *pixels)
{
    /* Let the real upload happen first, always. */
    o_glTexImage2D(target, level, internalfmt, w, h, border, format, type, pixels);

    if (target != GL_TEXTURE_2D || level != 0 || !pixels) return;

    size_t bpp = bytes_per_pixel(format, type);
    if (!bpp) return;

    uint64_t hash = fnv1a(pixels, (size_t)w * h * bpp);

    if (g_logging) {
        FILE *f = fopen(g_log_path, "a");
        if (f) {
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            fprintf(f, "%dx%d:%016llx tex=%d fmt=0x%X\n",
                    (int)w, (int)h, (unsigned long long)hash, bound, format);
            fclose(f);
        }
    }

    if (g_discover) {
        /* capture every screen-plausible texture for interactive cycling */
        if (w >= 96 && h >= 96) {
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            if (bound > 0) remember_capture((GLuint)bound, w, h, hash);
        }
        return;
    }

    for (int i = 0; i < g_fp_count; i++) {
        if (g_fps[i].w == w && g_fps[i].h == h && g_fps[i].hash == hash) {
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            if (bound > 0) remember_capture((GLuint)bound, w, h, hash);
            break;
        }
    }
}

/* ------------------------------------------------------------ public -- */

bool glhook_install(void) {
    if (!g_cap_lock_init) { InitializeCriticalSection(&g_cap_lock); g_cap_lock_init = true; }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) gl = LoadLibraryA("opengl32.dll");
    if (!gl) return false;

    void *target = (void *)GetProcAddress(gl, "glTexImage2D");
    if (!target) return false;

    if (MH_CreateHook(target, (void *)hk_glTexImage2D, (void **)&o_glTexImage2D) != MH_OK)
        return false;
    return MH_EnableHook(target) == MH_OK;
}

void glhook_remove(void) {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

int glhook_load_fingerprints(const char *path) {
    g_fp_count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    while (g_fp_count < MAX_FINGERPRINTS && fgets(line, sizeof(line), f)) {
        int w, h; unsigned long long hash;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%dx%d:%llx", &w, &h, &hash) == 3) {
            g_fps[g_fp_count].w = w;
            g_fps[g_fp_count].h = h;
            g_fps[g_fp_count].hash = (uint64_t)hash;
            g_fp_count++;
        }
    }
    fclose(f);
    return g_fp_count;
}

void glhook_set_logging(bool on, const char *log_path) {
    g_logging = on;
    if (log_path) { strncpy(g_log_path, log_path, MAX_PATH - 1); g_log_path[MAX_PATH-1] = 0; }
}

void glhook_reset_captures(void) {
    EnterCriticalSection(&g_cap_lock);
    InterlockedExchange(&g_cap_count, 0);
    g_last_serial = 0xFFFFFFFF;
    g_solo = -1;
    g_painted = -1;
    LeaveCriticalSection(&g_cap_lock);
}

int glhook_capture_count(void) { return (int)g_cap_count; }

/* ---------------------------------------------------------- discovery -- */

void glhook_discover_enable(bool on) { g_discover = on; }

void glhook_cycle(int delta) {
    EnterCriticalSection(&g_cap_lock);
    LONG n = g_cap_count;
    if (n <= 0) {
        g_solo = -1;
    } else if (g_solo < 0) {
        g_solo = 0;
    } else {
        g_solo = (int)((((LONG)g_solo + delta) % n + n) % n);
    }
    LeaveCriticalSection(&g_cap_lock);
}

void glhook_solo_fingerprint(char *out, int out_size) {
    if (!out || out_size <= 0) return;
    EnterCriticalSection(&g_cap_lock);
    if (g_solo < 0 || g_solo >= g_cap_count) {
        _snprintf(out, out_size - 1, "none (%d captured)", (int)g_cap_count);
    } else {
        capture_t *c = &g_caps[g_solo];
        _snprintf(out, out_size - 1, "%dx%d:%016llx  [%d/%d]",
                  c->w, c->h, (unsigned long long)c->hash,
                  g_solo + 1, (int)g_cap_count);
    }
    out[out_size - 1] = 0;
    LeaveCriticalSection(&g_cap_lock);
}

/* Restore the previously highlighted texture's original level-0 pixels. */
static void discover_unpaint(void) {
    if (g_painted < 0 || g_painted >= g_cap_count || !g_orig) { g_painted = -1; return; }
    capture_t *c = &g_caps[g_painted];
    GLint pb = 0, pa = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pb);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &pa);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, c->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c->w, c->h,
                    GL_RGBA, GL_UNSIGNED_BYTE, g_orig);
    glBindTexture(GL_TEXTURE_2D, (GLuint)pb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, pa);
    g_painted = -1;
}

void glhook_discover_paint(void) {
    if (!g_discover) return;

    int idx = g_solo;
    LONG n = g_cap_count;

    /* Selection changed: restore the old texture, snapshot the new one so it
       can be restored later. This makes only ONE texture highlighted at a
       time instead of permanently overwriting everything we pass through. */
    if (g_painted != idx) {
        discover_unpaint();
        if (idx >= 0 && idx < n) {
            capture_t *c = &g_caps[idx];
            size_t need = (size_t)c->w * c->h * 4;
            if (need > g_orig_cap) {
                uint8_t *nb = (uint8_t *)realloc(g_orig, need);
                if (!nb) return;
                g_orig = nb; g_orig_cap = need;
            }
            GLint pb = 0, pa = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &pb);
            glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glBindTexture(GL_TEXTURE_2D, c->tex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_orig);
            glBindTexture(GL_TEXTURE_2D, (GLuint)pb);
            glPixelStorei(GL_PACK_ALIGNMENT, pa);
            g_painted = idx;
        }
    }

    if (idx < 0 || idx >= n) return;
    capture_t cap = g_caps[idx];

    size_t need = (size_t)cap.w * cap.h * 4;
    if (need > g_scaled_cap) {
        uint8_t *nb = (uint8_t *)realloc(g_scaled, need);
        if (!nb) return;
        g_scaled = nb; g_scaled_cap = need;
    }

    /* vivid magenta/green checkerboard, chunky (32px) so it reads as squares */
    for (int y = 0; y < cap.h; y++) {
        for (int x = 0; x < cap.w; x++) {
            uint8_t *p = g_scaled + ((size_t)y * cap.w + x) * 4;
            int c = ((x >> 5) ^ (y >> 5)) & 1;
            p[0] = c ? 255 : 0;   /* R */
            p[1] = c ? 0 : 255;   /* G */
            p[2] = 255;           /* B */
            p[3] = 255;           /* A */
        }
    }

    GLint prev_bound = 0, prev_align = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, cap.tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cap.w, cap.h,
                    GL_RGBA, GL_UNSIGNED_BYTE, g_scaled);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}

/* nearest-neighbour RGBA scale */
static void scale_rgba(const uint8_t *src, int sw, int sh,
                       uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        const uint32_t *srow = (const uint32_t *)(src + (size_t)sy * sw * 4);
        uint32_t *drow = (uint32_t *)(dst + (size_t)y * dw * 4);
        for (int x = 0; x < dw; x++)
            drow[x] = srow[(int)((int64_t)x * sw / dw)];
    }
}

void glhook_blit(const uint8_t *rgba, int src_w, int src_h, uint32_t serial) {
    if (!rgba || g_cap_count == 0) return;
    if (serial == g_last_serial) return;   /* no new VM frame */
    g_last_serial = serial;

    GLint prev_bound = 0, prev_align = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    EnterCriticalSection(&g_cap_lock);
    LONG n = g_cap_count;
    for (LONG i = 0; i < n; i++) {
        capture_t *cap = &g_caps[i];
        const uint8_t *pixels = rgba;

        if (cap->w != src_w || cap->h != src_h) {
            size_t need = (size_t)cap->w * cap->h * 4;
            if (need > g_scaled_cap) {
                uint8_t *nb = (uint8_t *)realloc(g_scaled, need);
                if (!nb) continue;
                g_scaled = nb; g_scaled_cap = need;
            }
            scale_rgba(rgba, src_w, src_h, g_scaled, cap->w, cap->h);
            pixels = g_scaled;
        }

        glBindTexture(GL_TEXTURE_2D, cap->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cap->w, cap->h,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    LeaveCriticalSection(&g_cap_lock);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}
