/*
 * toshl_hooks.h - entry points the TempleOS-HL1 mod exposes to the HL SDK
 * client. Included by cdll_int.cpp and input.cpp at the three splice points.
 *
 * This header intentionally uses only plain C types so it can be included in
 * the SDK core files without dragging in windows.h. All Win32-specific glue
 * (window subclass, message routing) lives in toshl_glue.cpp.
 *
 * This header is part of the mod. It bridges to Half-Life SDK symbols, so the
 * SDK's core client files (cdll_int.cpp, input.cpp, tri.cpp) include it at the
 * splice points added by integration/halflife-updated.patch.
 */
#ifndef TOSHL_HOOKS_H
#define TOSHL_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle (defined in terminal_mode.cpp). */
void TOSHL_Init(void);        /* one-time setup; safe to call every HUD_Init */
void TOSHL_Shutdown(void);    /* tear down VM, RFB, GL hook                  */
void TOSHL_OnRedraw(void);    /* HUD pass: VM housekeeping + enter handling  */
void TOSHL_OnMapChange(void); /* reset captured textures; call in HUD_VidInit*/
void TOSHL_DrawWorld(void);   /* world pass: draw the TempleOS quad          */
void TOSHL_DrawHud(void);     /* 2D pass: aim dot; call in HUD_Redraw        */
void TOSHL_DrawOverlay(void); /* 2D pass: fullscreen zoom; call in HUD_Redraw*/
void TOSHL_ToggleZoom(void);  /* toggle the fullscreen zoom panel            */
int  TOSHL_InTerminal(void);  /* nonzero while driving a terminal            */

/* Trace helper (defined in toshl_glue.cpp): forward ray from the eye; on a
   hit fills origin+normal and returns 1, else 0. */
int  TOSHL_AimSurface(float max_dist, float out_origin[3], float out_normal[3]);

/* Live quad placement from cvars (toshl_size / toshl_fwd / toshl_shiftr /
   toshl_shiftu). Defined in toshl_glue.cpp. */
void TOSHL_QuadParams(float* size, float* fwd, float* right, float* up, float* aspect);

/* CRT post-effect controls from cvars (toshl_crt / toshl_crt_curve /
   toshl_crt_scan / toshl_crt_mask / toshl_crt_bezel). Defined in
   toshl_glue.cpp. */
void TOSHL_CrtParams(int* on, float* curve, float* scan, float* mask, float* bezel);

/* 1 = auto-lock the panel to the baked control-room monitor (c1a0). */
int  TOSHL_Fixed(void);

/* 1 if the local player is within `radius` units of (x,y,z). */
int  TOSHL_PlayerNear(float x, float y, float z, float radius);

/* Input adapters (defined in toshl_glue.cpp). */
void TOSHL_InstallInput(void);   /* subclass the engine window for keyboard  */
void TOSHL_RemoveInput(void);    /* restore the original window proc         */
void TOSHL_OnUseState(int down); /* feed +use key state each CL_CreateMove    */
int  TOSHL_WantsMovementLock(void); /* nonzero while driving the terminal    */

/* 1 if opt-in PC-speaker audio is enabled (toshl_sound). Defined in
   toshl_glue.cpp. */
int  TOSHL_SoundEnabled(void);

/* Discovery console commands: registered by TOSHL_RegisterCommands (glue),
   backed by TOSHL_DiscoverCycle / TOSHL_DiscoverPrint (terminal_mode.cpp). */
void TOSHL_RegisterCommands(void);
void TOSHL_DiscoverCycle(int delta);
void TOSHL_DiscoverPrint(void);
int  TOSHL_IsDiscover(void);

#ifdef __cplusplus
}
#endif
#endif /* TOSHL_HOOKS_H */
