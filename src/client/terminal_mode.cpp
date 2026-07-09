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
extern "C" int   TraceMonitorInFront(float max_dist, float hit_uv[2]); // see .md
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
extern "C" void TOSHL_OnRedraw() {
    if (!g_rfb) return;
    uint32_t serial;
    const uint8_t *frame = rfb_acquire_frame(g_rfb, &serial);
    if (frame) {
        glhook_blit(frame, g_fb_w, g_fb_h, serial);
        rfb_release_frame(g_rfb);
    }
}

// ------------------------------------------------------- input handling --
// Called from the client's window-proc hook (see SDK WIRING #3). Returns
// true if the event was consumed (i.e. player is in terminal mode).
extern "C" bool TOSHL_HandleKey(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_in_terminal || !g_rfb) return false;

    bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
    bool up   = (msg == WM_KEYUP   || msg == WM_SYSKEYUP);
    if (!down && !up) return true; // swallow char msgs while in terminal

    if (wp == VK_SHIFT || wp == VK_LSHIFT || wp == VK_RSHIFT) g_shift_down = down;

    // Dedicated escape key to LEAVE the terminal (F10 by convention; ESC is
    // sent through to TempleOS since it's meaningful there).
    if (wp == VK_F10 && down) { TOSHL_ExitTerminal(); return true; }

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

extern "C" void TOSHL_TryEnterTerminal() {
    float uv[2];
    if (!TraceMonitorInFront(96.0f, uv)) return; // not looking at a monitor
    if (!ensure_vm_and_rfb()) return;
    g_in_terminal = true;
    LockPlayerMovement(1);
    Con_Printf("[toshl] entering terminal. F10 to exit.\n");
}

extern "C" void TOSHL_ExitTerminal() {
    if (!g_in_terminal) return;
    g_in_terminal = false;
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
    // discovery mode toggle via env var for convenience
    if (GetEnvironmentVariableA("TOSHL_LOG", NULL, 0))
        glhook_set_logging(true, "toshl_uploads.log");
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
