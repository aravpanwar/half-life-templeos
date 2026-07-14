/*
 * toshl_glue.cpp - SDK-facing glue for TempleOS-HL1.
 *
 * Implements the three helpers terminal_mode.cpp expects (Con_Printf,
 * LockPlayerMovement, TraceMonitorInFront) plus the +use edge adapter and the
 * movement-lock query, using only Half-Life SDK engine functions.
 *
 * No windows.h in this translation unit: the Win32 keyboard glue lives in
 * toshl_input_win32.cpp, because the SDK's PlatformHeaders.h disables the
 * VK_/WM_ definitions that keyboard code needs. Keeping the two worlds in
 * separate .cpp files avoids the header conflict entirely.
 *
 * Part of the mod. Compiled into the Half-Life SDK client library via the
 * project edits in integration/halflife-updated.patch.
 */
#include "hud.h"
#include "cl_util.h"
#include "cl_entity.h"
#include "pm_defs.h"
#include "event_api.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "toshl_hooks.h"

extern "C" {
void TOSHL_TryEnterTerminal(void); // defined in terminal_mode.cpp
}

// ------------------------------------------------- discovery commands --
static void toshl_cmd_next(void) { TOSHL_DiscoverCycle(+1); }
static void toshl_cmd_prev(void) { TOSHL_DiscoverCycle(-1); }
static void toshl_cmd_lock(void) { TOSHL_DiscoverPrint(); }
static void toshl_cmd_zoom(void) { TOSHL_ToggleZoom(); }

// Live-tunable placement of the TempleOS quad (set in the console, e.g.
// "toshl_size 18"). Read every frame by TOSHL_QuadParams.
extern "C" void TOSHL_QuadParams(float* size, float* fwd, float* right, float* up, float* aspect)
{
	*size   = gEngfuncs.pfnGetCvarFloat("toshl_size");
	*fwd    = gEngfuncs.pfnGetCvarFloat("toshl_fwd");
	*right  = gEngfuncs.pfnGetCvarFloat("toshl_shiftr");
	*up     = gEngfuncs.pfnGetCvarFloat("toshl_shiftu");
	*aspect = gEngfuncs.pfnGetCvarFloat("toshl_aspect");
}

extern "C" int TOSHL_Fixed(void)
{
	return gEngfuncs.pfnGetCvarFloat("toshl_fixed") != 0.0f ? 1 : 0;
}

// CRT post-effect controls, read every frame by the render path. toshl_crt
// toggles the whole effect; the rest tune curvature, scanline depth and the
// RGB shadow mask.
extern "C" void TOSHL_CrtParams(int* on, float* curve, float* scan, float* mask)
{
	*on    = gEngfuncs.pfnGetCvarFloat("toshl_crt") != 0.0f ? 1 : 0;
	*curve = gEngfuncs.pfnGetCvarFloat("toshl_crt_curve");
	*scan  = gEngfuncs.pfnGetCvarFloat("toshl_crt_scan");
	*mask  = gEngfuncs.pfnGetCvarFloat("toshl_crt_mask");
}

extern "C" void TOSHL_DrawHud(void)
{
	// Hide the aim dot only while actually driving a terminal (movement locked).
	if (0 != TOSHL_WantsMovementLock())
		return;
	int w = ScreenWidth, h = ScreenHeight;
	if (w <= 0 || h <= 0)
		return;
	int cx = w / 2, cy = h / 2;
	const int s = 2;
	gEngfuncs.pfnFillRGBA(cx - s - 1, cy - s - 1, 2 * s + 2, 2 * s + 2, 0, 0, 0, 220);
	gEngfuncs.pfnFillRGBA(cx - s, cy - s, 2 * s, 2 * s, 0, 255, 0, 255);
}

extern "C" void TOSHL_RegisterCommands(void)
{
	gEngfuncs.pfnAddCommand("toshl_next", toshl_cmd_next);
	gEngfuncs.pfnAddCommand("toshl_prev", toshl_cmd_prev);
	gEngfuncs.pfnAddCommand("toshl_lock", toshl_cmd_lock);
	gEngfuncs.pfnAddCommand("toshl_zoom", toshl_cmd_zoom); // toggle fullscreen zoom

	gEngfuncs.pfnRegisterVariable("toshl_size", "32", 0);    // quad width (units)
	gEngfuncs.pfnRegisterVariable("toshl_fwd", "0.1", 0);    // offset toward viewer
	gEngfuncs.pfnRegisterVariable("toshl_shiftr", "0", 0);   // shift right in plane
	gEngfuncs.pfnRegisterVariable("toshl_shiftu", "0.6", 0); // shift up in plane
	gEngfuncs.pfnRegisterVariable("toshl_aspect", "0.85", 0); // panel height/width
	gEngfuncs.pfnRegisterVariable("toshl_fixed", "1", 0);    // 1=auto-lock to baked c1a0 monitor
	gEngfuncs.pfnRegisterVariable("toshl_crt", "1", 0);         // 1=CRT shader on
	gEngfuncs.pfnRegisterVariable("toshl_crt_curve", "0.12", 0); // barrel curvature
	gEngfuncs.pfnRegisterVariable("toshl_crt_scan", "0.35", 0);  // scanline depth
	gEngfuncs.pfnRegisterVariable("toshl_crt_mask", "0.30", 0);  // shadow-mask depth

	// In discovery mode, bind the cycle keys ourselves. This runs at HUD_Init,
	// AFTER the engine has exec'd config.cfg, so these binds win (config.cfg
	// otherwise rebinds [ ] and the mouse wheel to weapon switching).
	if (TOSHL_IsDiscover())
	{
		gEngfuncs.pfnClientCmd("bind \"]\" \"toshl_next\"\n");
		gEngfuncs.pfnClientCmd("bind \"[\" \"toshl_prev\"\n");
		gEngfuncs.pfnClientCmd("bind \"p\" \"toshl_lock\"\n");
		gEngfuncs.Con_Printf("[toshl] discovery keys bound: ] next, [ prev, p lock\n");
	}
}

// -------------------------------------------------------------- console --
extern "C" void Con_Printf(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	gEngfuncs.Con_Printf("%s", buf);
}

// -------------------------------------------------------- movement lock --
static int g_toshl_lock = 0;
extern "C" void LockPlayerMovement(int locked) { g_toshl_lock = locked ? 1 : 0; }
extern "C" int TOSHL_WantsMovementLock(void) { return g_toshl_lock; }

// ---------------------------------------------------- +use rising edge --
extern "C" void TOSHL_OnUseState(int down)
{
	static int prev = 0;
	if (0 != down && 0 == prev)
	{
		if (TOSHL_IsDiscover())
			TOSHL_DiscoverCycle(+1); // in discovery, USE cycles the highlight
		else if (0 == TOSHL_Fixed() && 0 == g_toshl_lock)
			TOSHL_TryEnterTerminal(); // aim-and-place mode only; fixed mode uses X
	}
	prev = down;
}

// ------------------------------------------------------- surface trace --
// Trace forward from the eye and return the hit point + surface normal. The
// mod draws its own TempleOS quad at that plane, so we do NOT need to know the
// surface's texture name; any surface the player faces becomes a terminal.
// Called from HUD_Redraw (a clean context), not CL_CreateMove, so the
// PM-state push/pop is balanced and does not warn.
extern "C" int TOSHL_AimSurface(float max_dist, float out_origin[3], float out_normal[3])
{
	cl_entity_t* pl = gEngfuncs.GetLocalPlayer();
	if (!pl)
		return 0;

	Vector angles, forward, right, up;
	gEngfuncs.GetViewAngles(angles);
	gEngfuncs.pfnAngleVectors(angles, forward, right, up);

	Vector eye = pl->origin;
	eye.z += 28.0f; // approx standing view height
	Vector end = eye + forward * max_dist;

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(0, 1);
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers(pl->index - 1);
	gEngfuncs.pEventAPI->EV_SetTraceHull(2);

	pmtrace_t tr;
	gEngfuncs.pEventAPI->EV_PlayerTrace(eye, end, PM_STUDIO_IGNORE, -1, &tr);

	gEngfuncs.pEventAPI->EV_PopPMStates();

	if (tr.fraction >= 1.0f)
		return 0;

	out_origin[0] = tr.endpos.x;
	out_origin[1] = tr.endpos.y;
	out_origin[2] = tr.endpos.z;
	out_normal[0] = tr.plane.normal.x;
	out_normal[1] = tr.plane.normal.y;
	out_normal[2] = tr.plane.normal.z;
	return 1;
}

// 1 if the local player is within `radius` game units of (x,y,z). Used to gate
// terminal focus so you can only start typing when standing at the console.
extern "C" int TOSHL_PlayerNear(float x, float y, float z, float radius)
{
	cl_entity_t* pl = gEngfuncs.GetLocalPlayer();
	if (!pl)
		return 0;
	float dx = pl->origin.x - x, dy = pl->origin.y - y, dz = pl->origin.z - z;
	return (dx * dx + dy * dy + dz * dz) <= radius * radius ? 1 : 0;
}
