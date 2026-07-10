/*
 * terminal_mode.cpp - the orchestrator.
 *
 * Owns the lifecycle: launch VM -> connect RFB -> capture monitor textures
 * -> per-frame blit -> route input while the player is "at" a terminal.
 *
 * This file references Half-Life SDK symbols (gEngfuncs, HUD_* entry points).
 * The three functions at the bottom marked >>> SDK WIRING <<< are where you
 * splice into cl_dll. Everything above them is engine-agnostic and unit-
 * testable on its own.
 */

extern "C" {
#include "../rfb/rfb_client.h"
#include "../glhook/gl_hook.h"
#include "../vmproc/vm_launcher.h"
}
#include "keymap.h"

#include <windows.h>
#include <stdio.h>

// ---- HL SDK surface we lean on. These three helpers are implemented in the
//      SDK-side glue (toshl_glue.cpp), which owns all direct engine access.
extern "C" void  Con_Printf(const char *fmt, ...);   // wraps gEngfuncs->Con_Printf
extern "C" int   TOSHL_AimSurface(float max_dist, float out_origin[3], float out_normal[3]);
extern "C" void  TOSHL_QuadParams(float *size, float *fwd, float *right, float *up, float *aspect);
extern "C" int   TOSHL_Freewalk(void);
extern "C" void  LockPlayerMovement(int locked); // suppresses CL_CreateMove

// entry points defined below, referenced across functions
extern "C" void TOSHL_ExitTerminal();

// ---------------------------------------------------------------- state --

static const char *VM_HOST      = "127.0.0.1";
static const int   VNC_DISPLAY  = 0;               // -> tcp 5900
static const uint16_t VNC_PORT  = 5900 + VNC_DISPLAY;

static rfb_client_t *g_rfb;
static bool          g_in_terminal;      // player currently driving TempleOS
static bool          g_shift_down;
static int           g_fb_w, g_fb_h;
static char          g_mod_dir[MAX_PATH];
static bool          g_discover_mode;    // TOSHL_DISCOVER=1: highlight screens
static bool          g_autovm;           // TOSHL_AUTOVM=1: start VM, blit, no +use

static bool          g_want_enter;       // +use requested; handled in OnRedraw
static bool          g_freewalk_active;   // placed-but-walking (vs locked typing)
static bool          g_zoom;              // fullscreen zoom panel active
static float         g_scr_origin[3];    // frozen screen centre (trace hit)
static float         g_scr_normal[3];    // frozen screen normal
static float         g_quad_units = 26.0f; // quad width in game units (4:3)

// -------------------------------------------------------------- helpers --

static void resolve_mod_dir() {
    // GetGameDirectory returns e.g. "half-life-templeos"; make absolute.
    char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd);
    // In production, query gEngfuncs->pfnGetGameDirectory().
    _snprintf(g_mod_dir, MAX_PATH, "%s\\half-life-templeos", cwd);
}

static bool ensure_vm_and_rfb() {
    if (!vm_is_running()) {
        if (!vm_launch(g_mod_dir, VNC_DISPLAY)) {
            Con_Printf("[toshl] failed to launch QEMU: check vm/qemu_path.txt\n");
            return false;
        }
    }
    if (!g_rfb) {
        // QEMU's VNC listener may take a beat to bind; retry briefly.
        for (int attempt = 0; attempt < 40 && !g_rfb; ++attempt) {
            g_rfb = rfb_connect(VM_HOST, VNC_PORT);
            if (!g_rfb) Sleep(250);
        }
        if (!g_rfb) { Con_Printf("[toshl] RFB connect failed: %s\n", rfb_last_error()); return false; }
        rfb_get_size(g_rfb, &g_fb_w, &g_fb_h);
        if (!rfb_start(g_rfb)) { Con_Printf("[toshl] RFB pump failed\n"); return false; }
        Con_Printf("[toshl] TempleOS online at %dx%d. Behold.\n", g_fb_w, g_fb_h);
    }
    return true;
}

// -------------------------------------------------- per-frame rendering --
// Call from HUD_Redraw (context current). Uploads the latest VM frame into
// every captured monitor texture.
// One non-blocking connect attempt per call (loopback refuses instantly while
// QEMU is still booting, so this never stalls the frame). Launches the VM on
// first call. Returns true once the RFB pump is live.
static bool try_connect_vm_once() {
    if (g_rfb) return true;
    if (!vm_is_running()) vm_launch(g_mod_dir, VNC_DISPLAY);
    rfb_client_t *c = rfb_connect(VM_HOST, VNC_PORT);
    if (!c) return false;
    g_rfb = c;
    rfb_get_size(g_rfb, &g_fb_w, &g_fb_h);
    rfb_start(g_rfb);
    Con_Printf("[toshl] VM online at %dx%d.\n", g_fb_w, g_fb_h);
    return true;
}

// HUD pass. Handles VM housekeeping and the deferred +use enter. The actual
// TempleOS drawing happens in TOSHL_DrawWorld (world pass). We do NOT blit onto
// map textures here anymore: that smeared, because GoldSrc shares/tiles
// textures. The quad in TOSHL_DrawWorld uses our own texture instead.
extern "C" void TOSHL_OnRedraw() {
    if (g_discover_mode) {
        try_connect_vm_once();
        if (g_rfb) {
            uint32_t serial;
            const uint8_t *frame = rfb_acquire_frame(g_rfb, &serial);
            if (frame) { glhook_discover_frame(frame, g_fb_w, g_fb_h); rfb_release_frame(g_rfb); }
            else glhook_discover_paint();
        } else {
            glhook_discover_paint();
        }
        return;
    }

    // Keep the VM warm so pressing use is instant.
    try_connect_vm_once();

    // Resolve a pending +use here (a clean context, unlike CL_CreateMove) so
    // the trace's PM-state push/pop is balanced. Allowed to re-place while
    // already shown (freewalk lets +use re-aim the panel).
    if (g_want_enter) {
        g_want_enter = false;
        float o[3], nrm[3];
        if (TOSHL_AimSurface(96.0f, o, nrm)) {
            memcpy(g_scr_origin, o, sizeof(o));
            memcpy(g_scr_normal, nrm, sizeof(nrm));
            g_in_terminal = true;
            g_freewalk_active = (TOSHL_Freewalk() != 0);
            if (g_freewalk_active) {
                LockPlayerMovement(0); // stay free to walk and judge
                Con_Printf("[toshl] placed. Walk around to judge it. F10 clears it,\n");
                Con_Printf("[toshl] 'toshl_freewalk 0' then re-place to type into it.\n");
            } else {
                LockPlayerMovement(1);
                Con_Printf("[toshl] terminal engaged. F10 to exit.\n");
            }
        } else {
            Con_Printf("[toshl] no surface in front of you.\n");
        }
    }
}

// World pass (HUD_DrawTransparentTriangles): draw the live TempleOS frame as a
// quad pinned to the surface the player engaged. Our own texture, so it never
// smears onto the map's shared textures.
extern "C" void TOSHL_DrawWorld() {
    if (!g_in_terminal || !g_rfb) return;
    uint32_t serial;
    const uint8_t *frame = rfb_acquire_frame(g_rfb, &serial);
    if (frame) {
        float size = g_quad_units, fwd = 1.0f, sr = 0.0f, su = 0.0f, aspect = 0.75f;
        TOSHL_QuadParams(&size, &fwd, &sr, &su, &aspect);
        if (size <= 0.0f) size = g_quad_units;
        if (aspect <= 0.0f) aspect = 0.75f;
        glhook_draw_quad(frame, g_fb_w, g_fb_h, g_scr_origin, g_scr_normal,
                         size, size * aspect, fwd, sr, su);
        rfb_release_frame(g_rfb);
    }
}

// Toggle the fullscreen zoom panel (bound to the toshl_zoom console command).
extern "C" void TOSHL_ToggleZoom() {
    g_zoom = !g_zoom;
    Con_Printf("[toshl] zoom %s.\n", g_zoom ? "ON" : "off");
}

// 2D pass (HUD_Redraw): when zoomed, draw TempleOS as a big centred panel so
// it is legible from any distance, even if you can't walk up to the monitor.
extern "C" void TOSHL_DrawOverlay() {
    if (!g_zoom || !g_rfb) return;
    uint32_t serial;
    const uint8_t *frame = rfb_acquire_frame(g_rfb, &serial);
    if (frame) {
        glhook_draw_screen(frame, g_fb_w, g_fb_h, 0.92f);
        rfb_release_frame(g_rfb);
    }
}

// ------------------------------------------------------- input handling --
// Called from the client's window-proc hook (see SDK WIRING #3). Returns
// true if the event was consumed (i.e. player is in terminal mode).
extern "C" bool TOSHL_HandleKey(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_in_terminal || !g_rfb) return false;

    bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);

    // F10 clears/exits in any mode.
    if (down && wp == VK_F10) { TOSHL_ExitTerminal(); return true; }

    // Freewalk: the panel is only placed for judging, so let the engine keep
    // the keyboard (WASD, ~ console, etc.). Only F10 above is intercepted.
    if (g_freewalk_active) return false;

    bool up = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
    if (!down && !up) return true; // swallow char msgs while typing

    if (wp == VK_SHIFT || wp == VK_LSHIFT || wp == VK_RSHIFT) g_shift_down = down;

    uint32_t ks = vk_to_keysym(wp, g_shift_down);
    if (ks) rfb_send_key(g_rfb, ks, down);
    return true;
}

// mouse motion while in terminal: map trace UV -> framebuffer coords
extern "C" void TOSHL_HandleMouse(float uv_x, float uv_y, int buttons) {
    if (!g_in_terminal || !g_rfb) return;
    uint16_t x = (uint16_t)(uv_x * g_fb_w);
    uint16_t y = (uint16_t)(uv_y * g_fb_h);
    rfb_send_pointer(g_rfb, x, y, (uint8_t)buttons);
}

// ------------------------------------------------------- enter / leave --

// Called on the +use rising edge from CL_CreateMove. We only set a flag here;
// the trace + enter happen in TOSHL_OnRedraw, off the movement path. Setting it
// unconditionally lets +use re-aim the panel while in freewalk mode.
extern "C" void TOSHL_TryEnterTerminal() {
    g_want_enter = true;
}

extern "C" void TOSHL_ExitTerminal() {
    if (!g_in_terminal) return;
    g_in_terminal = false;
    g_freewalk_active = false;
    g_shift_down = false;
    LockPlayerMovement(0);
}

// ------------------------------------------------------------ lifecycle --

extern "C" void TOSHL_Init() {
    // HUD_Init fires on every server connect / map load; only set up once.
    static bool inited = false;
    if (inited) return;
    inited = true;

    resolve_mod_dir();
    if (!glhook_install()) { Con_Printf("[toshl] GL hook install FAILED\n"); return; }
    char fp[MAX_PATH]; _snprintf(fp, MAX_PATH, "%s\\monitor_fingerprints.txt", g_mod_dir);
    int n = glhook_load_fingerprints(fp);
    Con_Printf("[toshl] loaded %d monitor fingerprint(s)\n", n);
    // upload logging toggle via env var for convenience
    if (GetEnvironmentVariableA("TOSHL_LOG", NULL, 0))
        glhook_set_logging(true, "toshl_uploads.log");

    // discovery mode: capture-all + cycle a highlight to identify screens.
    if (GetEnvironmentVariableA("TOSHL_DISCOVER", NULL, 0)) {
        g_discover_mode = true;
        glhook_discover_enable(true);
        Con_Printf("[toshl] DISCOVER mode: press USE (E) to cycle the highlight\n");
        Con_Printf("[toshl] across screen textures. Stop when your screen lights up.\n");
    }

    // auto-VM test mode: bring the VM up and blit onto matched fingerprints
    // without needing the +use trace. For proving the pipeline in-game.
    if (GetEnvironmentVariableA("TOSHL_AUTOVM", NULL, 0)) {
        g_autovm = true;
        Con_Printf("[toshl] AUTOVM: will launch the VM and stream to matched textures.\n");
    }
}

// Discovery console-command backends (invoked from the SDK glue). Cycling
// moves the highlight; lock prints the current screen's fingerprint to paste
// into monitor_fingerprints.txt.
extern "C" void TOSHL_DiscoverCycle(int delta) {
    glhook_cycle(delta);
    char b[96]; glhook_solo_fingerprint(b, sizeof(b));
    Con_Printf("[toshl] highlighting %s\n", b);
    // Mirror the current selection to a file so it can be read without the
    // console (the operator picks it up when the right screen is highlighted).
    char path[MAX_PATH]; _snprintf(path, MAX_PATH, "%s\\toshl_current.txt", g_mod_dir);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", b); fclose(f); }
}

extern "C" int TOSHL_IsDiscover() { return g_discover_mode ? 1 : 0; }

extern "C" int TOSHL_InTerminal() { return g_in_terminal ? 1 : 0; }

extern "C" void TOSHL_DiscoverPrint() {
    char b[96]; glhook_solo_fingerprint(b, sizeof(b));
    Con_Printf("[toshl] LOCK -> add this line to monitor_fingerprints.txt: %s\n", b);
    // Also append to <mod>/toshl_lock.txt so the fingerprint can be recovered
    // without reading the console.
    char path[MAX_PATH]; _snprintf(path, MAX_PATH, "%s\\toshl_lock.txt", g_mod_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", b); fclose(f); }
}

extern "C" void TOSHL_OnMapChange() { glhook_reset_captures(); }

extern "C" void TOSHL_Shutdown() {
    TOSHL_ExitTerminal();
    if (g_rfb) { rfb_disconnect(g_rfb); g_rfb = NULL; }
    vm_kill();
    glhook_remove();
}

/* ===================================================================== *
 *  >>> SDK WIRING <<<  (do these three splices in the HL SDK cl_dll)     *
 * ===================================================================== *
 *
 * 1. cdll_int.cpp : HUD_Init()      -> call TOSHL_Init();
 *                    HUD_Redraw()    -> call TOSHL_OnRedraw();  (end of fn)
 *                    HUD_VidInit()   -> call TOSHL_OnMapChange();
 *                    (shutdown path) -> call TOSHL_Shutdown();
 *
 * 2. +use interaction: in input.cpp CL_CreateMove(), when IN_USE goes from
 *    up->down and !g_in_terminal, call TOSHL_TryEnterTerminal(). While
 *    g_in_terminal, zero out cmd->forwardmove/sidemove/upmove so the body
 *    stands still. (LockPlayerMovement() flips the flag this reads.)
 *
 * 3. keyboard: subclass the engine window in HUD_Init (GetActiveWindow +
 *    SetWindowLongPtr(GWLP_WNDPROC)) and, in your wndproc, call
 *    TOSHL_HandleKey(msg, wParam, lParam) first. If it returns true,
 *    return 0 without chaining, so WASD etc. don't leak into gameplay.
 *
 * TraceMonitorInFront(): gEngfuncs->pEventAPI->EV_SetTraceHull + EV_PlayerTrace
 * a ray from view origin along forward; if it hits a brush/entity whose
 * texture is one of your monitor fingerprints, compute the hit's texture UV
 * and return it. A pragmatic first version: just check distance + view angle
 * to a known "monitor" entity classname placed in the map.
 */
