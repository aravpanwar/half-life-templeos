/*
 * gl_hook.h - GoldSrc monitor-texture hijack.
 *
 * Strategy (Path B, real Steam GoldSrc):
 *   1. At HUD_Init, install an inline hook (MinHook) on glTexImage2D.
 *   2. When the engine uploads map textures, fingerprint every upload
 *      (dims + FNV-1a of pixel data). Uploads matching a fingerprint in
 *      monitor_fingerprints.txt get their GL texture name recorded.
 *   3. Every rendered frame, glTexSubImage2D the (scaled) TempleOS
 *      framebuffer into every recorded texture. Every instance of that
 *      monitor texture in the map becomes a live screen.
 *
 * Discovery mode (toshl_log_uploads 1): logs every texture upload's
 * dims+hash to toshl_uploads.log so you can find your monitor textures
 * by staring at one in-game and diffing.
 */

#ifndef TOSHL_GL_HOOK_H
#define TOSHL_GL_HOOK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install hooks. Call from HUD_Init (before any map is loaded). */
bool glhook_install(void);
void glhook_remove(void);

/* Load fingerprints ("WxH:HASHHEX" lines) from a file in the mod dir. */
int  glhook_load_fingerprints(const char *path);

/* Enable/disable upload logging (discovery mode). */
void glhook_set_logging(bool on, const char *log_path);

/* Forget captured texture ids (call on map change / vid_restart). */
void glhook_reset_captures(void);

/*
 * Blit an RGBA8888 source image into every captured monitor texture,
 * nearest-neighbour scaled to each texture's own dimensions.
 * Call once per frame from the render path (HUD_Redraw is fine) with
 * a valid GL context current. Cheap: skips work if serial is unchanged.
 */
void glhook_blit(const uint8_t *rgba, int src_w, int src_h, uint32_t serial);

/* How many live monitor textures were captured this map. */
int  glhook_capture_count(void);

/*
 * Discovery mode. When enabled, every screen-plausible texture upload is
 * captured (not just fingerprint matches), so you can cycle a highlight
 * through them in-game to identify a monitor:
 *   - glhook_discover_paint() paints a vivid checkerboard onto the currently
 *     "solo" captured texture. Call once per frame with a live GL context.
 *   - glhook_cycle(+1/-1) moves the solo selection.
 *   - glhook_solo_fingerprint() writes the current selection's "WxH:hash"
 *     (plus an index) into `out`, ready to paste into monitor_fingerprints.txt.
 */
/*
 * Draw the RGBA frame as a flat quad in the 3D world, centred at `origin`,
 * facing along `normal` (a surface normal from a trace), sized world_w x
 * world_h game units. Uses our OWN GL texture, so it never touches or smears
 * the map's shared textures. Call from HUD_DrawTransparentTriangles (world
 * matrices are current there).
 */
void glhook_draw_quad(const uint8_t *rgba, int w, int h,
                      const float *origin, const float *normal,
                      float world_w, float world_h,
                      float off_fwd, float off_right, float off_up);

void glhook_discover_enable(bool on);
void glhook_discover_paint(void);
/* Like discover_paint but shows the (scaled) live VM frame on the solo
   texture, so discovery previews real TempleOS on each candidate. Pass NULL
   to fall back to the checkerboard. */
void glhook_discover_frame(const uint8_t *rgba, int src_w, int src_h);
void glhook_cycle(int delta);
void glhook_solo_fingerprint(char *out, int out_size);

#ifdef __cplusplus
}
#endif
#endif /* TOSHL_GL_HOOK_H */
