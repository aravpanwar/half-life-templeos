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
#include <math.h>

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

static void scale_rgba(const uint8_t *src, int sw, int sh,
                       uint8_t *dst, int dw, int dh); /* defined below */

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

/*
 * Paint the current "solo" captured texture so exactly one surface is
 * highlighted at a time. If `rgba` is non-NULL, the (scaled) live VM frame is
 * shown on it, so discovery previews real TempleOS on each candidate; if NULL,
 * a magenta/green checkerboard is shown instead (VM not up yet).
 */
void glhook_discover_frame(const uint8_t *rgba, int src_w, int src_h) {
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

    if (rgba && src_w > 0 && src_h > 0) {
        scale_rgba(rgba, src_w, src_h, g_scaled, cap.w, cap.h);
    } else {
        /* magenta/green checkerboard, chunky (32px) so it reads as squares */
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

void glhook_discover_paint(void) { glhook_discover_frame(NULL, 0, 0); }

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

/* ----------------------------------------------------------- CRT shader -- */
/*
 * Optional CRT post-effect: barrel curvature, scanlines locked to TempleOS's
 * emulated rows, an RGB shadow mask and a soft vignette, via a GLSL 120
 * fragment shader. The shader entry points are not in the classic opengl32
 * import library, so they are loaded with wglGetProcAddress at first use. If
 * GL 2.0 or shader compilation is unavailable, crt_begin() returns 0 and the
 * caller draws the plain textured quad instead; we never retry. So the effect
 * is pure upside: it either turns on or silently does nothing.
 */

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#endif

typedef GLuint (WINAPI *PFN_glCreateShader)(GLenum);
typedef void   (WINAPI *PFN_glShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void   (WINAPI *PFN_glCompileShader)(GLuint);
typedef void   (WINAPI *PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef GLuint (WINAPI *PFN_glCreateProgram)(void);
typedef void   (WINAPI *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (WINAPI *PFN_glLinkProgram)(GLuint);
typedef void   (WINAPI *PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (WINAPI *PFN_glUseProgram)(GLuint);
typedef GLint  (WINAPI *PFN_glGetUniformLocation)(GLuint, const char*);
typedef void   (WINAPI *PFN_glUniform1i)(GLint, GLint);
typedef void   (WINAPI *PFN_glUniform1f)(GLint, GLfloat);
typedef void   (WINAPI *PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void   (WINAPI *PFN_glDeleteShader)(GLuint);

static PFN_glCreateShader      p_glCreateShader;
static PFN_glShaderSource      p_glShaderSource;
static PFN_glCompileShader     p_glCompileShader;
static PFN_glGetShaderiv       p_glGetShaderiv;
static PFN_glCreateProgram     p_glCreateProgram;
static PFN_glAttachShader      p_glAttachShader;
static PFN_glLinkProgram       p_glLinkProgram;
static PFN_glGetProgramiv      p_glGetProgramiv;
static PFN_glUseProgram        p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform1i         p_glUniform1i;
static PFN_glUniform1f         p_glUniform1f;
static PFN_glUniform2f         p_glUniform2f;
static PFN_glDeleteShader      p_glDeleteShader;

static const char *CRT_VS =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

static const char *CRT_FS =
    "#version 120\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2  uSrc;\n"   /* emulated resolution, e.g. 640x480 */
    "uniform float uCurve;\n" /* barrel amount                     */
    "uniform float uScan;\n"  /* scanline depth 0..1               */
    "uniform float uMask;\n"  /* shadow-mask depth 0..1            */
    "void main() {\n"
    "    vec2 uv0 = gl_TexCoord[0].xy;\n"
    "    vec2 cc = uv0 * 2.0 - 1.0;\n"
    "    cc *= 1.0 + uCurve * dot(cc, cc);\n"
    "    vec2 uv = cc * 0.5 + 0.5;\n"
    "    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
    "        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return;\n"
    "    }\n"
    "    vec3 col = texture2D(uTex, uv).rgb;\n"
    /* scanlines locked to the emulated rows, faded out when the panel is too
       small to resolve them (avoids moire on the distant in-world monitor) */
    "    float sl = 0.5 + 0.5 * cos(uv.y * uSrc.y * 6.2831853);\n"
    "    float fw = fwidth(uv.y) * uSrc.y;\n"
    "    float scanAmt = uScan * clamp(1.0 - fw, 0.0, 1.0);\n"
    "    col *= mix(1.0 - scanAmt, 1.0, sl);\n"
    /* RGB shadow mask in screen space (crisp at any panel size) */
    "    float mx = mod(gl_FragCoord.x, 3.0);\n"
    "    vec3 mask = vec3(1.0 - uMask);\n"
    "    if (mx < 1.0) mask.r = 1.0; else if (mx < 2.0) mask.g = 1.0; else mask.b = 1.0;\n"
    "    col *= mask;\n"
    "    col *= 1.0 + 0.7 * uMask + 0.3 * scanAmt;\n" /* recover mask/scan losses */
    "    float d = length(uv0 * 2.0 - 1.0);\n"
    "    col *= mix(1.0, smoothstep(1.35, 0.2, d), 0.25);\n" /* soft vignette */
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

static int    g_crt_on = 1;
static float  g_crt_curve = 0.12f, g_crt_scan = 0.35f, g_crt_mask = 0.30f;
static int    g_crt_gl2 = 0;    /* 0 untried, 1 loaded, -1 unavailable */
static int    g_crt_state = 0;  /* 0 untried, 1 ready,  -1 failed      */
static GLuint g_crt_prog;
static GLint  u_tex, u_src, u_curve, u_scan, u_mask;

static int crt_load_gl2(void) {
    if (g_crt_gl2) return g_crt_gl2 > 0;
    #define TOSHL_LOAD(n) do { p_##n = (PFN_##n)wglGetProcAddress(#n); \
        if (!p_##n) { g_crt_gl2 = -1; return 0; } } while (0)
    TOSHL_LOAD(glCreateShader);       TOSHL_LOAD(glShaderSource);
    TOSHL_LOAD(glCompileShader);      TOSHL_LOAD(glGetShaderiv);
    TOSHL_LOAD(glCreateProgram);      TOSHL_LOAD(glAttachShader);
    TOSHL_LOAD(glLinkProgram);        TOSHL_LOAD(glGetProgramiv);
    TOSHL_LOAD(glUseProgram);         TOSHL_LOAD(glGetUniformLocation);
    TOSHL_LOAD(glUniform1i);          TOSHL_LOAD(glUniform1f);
    TOSHL_LOAD(glUniform2f);          TOSHL_LOAD(glDeleteShader);
    #undef TOSHL_LOAD
    g_crt_gl2 = 1;
    return 1;
}

static GLuint crt_compile(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    if (!s) return 0;
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { p_glDeleteShader(s); return 0; }
    return s;
}

static int crt_build(void) {
    if (g_crt_state) return g_crt_state > 0;
    if (!crt_load_gl2()) { g_crt_state = -1; return 0; }

    GLuint vs = crt_compile(GL_VERTEX_SHADER, CRT_VS);
    GLuint fs = crt_compile(GL_FRAGMENT_SHADER, CRT_FS);
    if (!vs || !fs) {
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        g_crt_state = -1;
        return 0;
    }

    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glLinkProgram(p);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) { g_crt_state = -1; return 0; }

    g_crt_prog = p;
    u_tex   = p_glGetUniformLocation(p, "uTex");
    u_src   = p_glGetUniformLocation(p, "uSrc");
    u_curve = p_glGetUniformLocation(p, "uCurve");
    u_scan  = p_glGetUniformLocation(p, "uScan");
    u_mask  = p_glGetUniformLocation(p, "uMask");
    g_crt_state = 1;
    return 1;
}

/* Bind the CRT program and push the current parameters. Returns 1 if the
   effect is on and drawing should proceed under the shader, 0 to draw plain. */
static int crt_begin(int src_w, int src_h) {
    if (!g_crt_on) return 0;
    if (!crt_build()) return 0;
    p_glUseProgram(g_crt_prog);
    if (u_tex   >= 0) p_glUniform1i(u_tex, 0);
    if (u_src   >= 0) p_glUniform2f(u_src, (GLfloat)src_w, (GLfloat)src_h);
    if (u_curve >= 0) p_glUniform1f(u_curve, g_crt_curve);
    if (u_scan  >= 0) p_glUniform1f(u_scan, g_crt_scan);
    if (u_mask  >= 0) p_glUniform1f(u_mask, g_crt_mask);
    return 1;
}

static void crt_end(void) { p_glUseProgram(0); }

/* Live CRT tuning from terminal_mode (which reads the toshl_crt* cvars).
   Negative curve/scan/mask keep the current value. */
void glhook_set_crt(int on, float curve, float scan, float mask) {
    g_crt_on = on ? 1 : 0;
    if (curve >= 0.0f) g_crt_curve = curve;
    if (scan  >= 0.0f) g_crt_scan  = scan;
    if (mask  >= 0.0f) g_crt_mask  = mask;
}

/* ----------------------------------------------------- world-space quad -- */

static GLuint g_quad_tex;      /* our own texture, never a map texture */
static int    g_quad_w, g_quad_h;

static void normalize3(float *v) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; }
}
static void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}

void glhook_draw_quad(const uint8_t *rgba, int w, int h,
                      const float *origin, const float *normal,
                      float world_w, float world_h,
                      float off_fwd, float off_right, float off_up)
{
    if (!rgba || w <= 0 || h <= 0) return;

    /* upload the frame into our own texture (allocate on first/size change).
       NEAREST filtering keeps TempleOS's 640x480 pixels crisp instead of a
       blurry upscale. */
    if (!g_quad_tex) {
        glGenTextures(1, &g_quad_tex);
        glBindTexture(GL_TEXTURE_2D, g_quad_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        g_quad_w = g_quad_h = 0;
    }

    GLint prev_bound = 0, prev_align = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, g_quad_tex);
    if (w != g_quad_w || h != g_quad_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        g_quad_w = w; g_quad_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    /* build an in-plane basis from the surface normal */
    float n[3] = { normal[0], normal[1], normal[2] };
    normalize3(n);
    float wup[3] = { 0.0f, 0.0f, 1.0f };
    float r[3];
    cross3(n, wup, r);
    if (r[0]*r[0] + r[1]*r[1] + r[2]*r[2] < 1e-6f) { r[0]=1; r[1]=0; r[2]=0; }
    normalize3(r);
    float u[3];
    cross3(r, n, u);
    normalize3(u);

    /* centre = hit point, offset toward the viewer (off_fwd, avoids z-fighting)
       and shifted within the screen plane (off_right, off_up) for alignment.
       `r` points to the viewer's left (same reason the U texcoords are
       flipped), so negate off_right to make positive toshl_shiftr go right. */
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = origin[i] + n[i]*off_fwd - r[i]*off_right + u[i]*off_up;
    float hw = world_w * 0.5f, hh = world_h * 0.5f;
    float tl[3], tr[3], bl[3], br[3];
    for (int i = 0; i < 3; i++) {
        tl[i] = c[i] - r[i]*hw + u[i]*hh;
        tr[i] = c[i] + r[i]*hw + u[i]*hh;
        bl[i] = c[i] - r[i]*hw - u[i]*hh;
        br[i] = c[i] + r[i]*hw - u[i]*hh;
    }

    GLboolean was_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean was_blend = glIsEnabled(GL_BLEND);
    GLboolean was_tex = glIsEnabled(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    /* U is flipped (1 on the left corners) because the in-plane "right" basis
       derived from the surface normal points to the viewer's left; without
       this, TempleOS renders mirrored. */
    int crt = crt_begin(w, h);
    glBegin(GL_QUADS);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(tl[0], tl[1], tl[2]);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(tr[0], tr[1], tr[2]);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(br[0], br[1], br[2]);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(bl[0], bl[1], bl[2]);
    glEnd();
    if (crt) crt_end();

    if (was_cull) glEnable(GL_CULL_FACE);
    if (was_blend) glEnable(GL_BLEND);
    if (!was_tex) glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}

void glhook_draw_screen(const uint8_t *rgba, int w, int h, float coverage) {
    if (!rgba || w <= 0 || h <= 0) return;
    if (coverage <= 0.0f) coverage = 0.92f;

    if (!g_quad_tex) {
        glGenTextures(1, &g_quad_tex);
        glBindTexture(GL_TEXTURE_2D, g_quad_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        g_quad_w = g_quad_h = 0;
    }

    GLint prev_bound = 0, prev_align = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, g_quad_tex);
    if (w != g_quad_w || h != g_quad_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        g_quad_w = w; g_quad_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float sw = (float)vp[2], sh = (float)vp[3];
    if (sw <= 0 || sh <= 0) { glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound); return; }

    /* fit the frame inside `coverage` of the screen, preserving w:h */
    float tw = sw * coverage;
    float th = tw * (float)h / (float)w;
    if (th > sh * coverage) { th = sh * coverage; tw = th * (float)w / (float)h; }
    float x0 = (sw - tw) * 0.5f, y0 = (sh - th) * 0.5f;
    float x1 = x0 + tw, y1 = y0 + th;

    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0.0, sw, sh, 0.0, -1.0, 1.0);          /* y-down screen space */
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();

    GLboolean was_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean was_cull  = glIsEnabled(GL_CULL_FACE);
    GLboolean was_blend = glIsEnabled(GL_BLEND);
    GLboolean was_tex   = glIsEnabled(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    int crt = crt_begin(w, h);
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x1, y0);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x1, y1);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x0, y1);
    glEnd();
    if (crt) crt_end();

    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    if (was_depth) glEnable(GL_DEPTH_TEST);
    if (was_cull)  glEnable(GL_CULL_FACE);
    if (was_blend) glEnable(GL_BLEND);
    if (!was_tex)  glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}
