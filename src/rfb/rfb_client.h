/*
 * rfb_client.h - minimal RFB 3.8 (VNC) client for TempleOS-HL1
 *
 * Deliberately tiny: Raw encoding only, security type None, loopback use.
 * QEMU's built-in VNC server supports all of this out of the box.
 *
 * Threading model:
 *   - rfb_start() spawns a background thread that owns the socket and
 *     continuously requests/receives framebuffer updates into an internal
 *     back buffer.
 *   - The game thread calls rfb_acquire_frame() once per rendered frame.
 *     It returns a pointer to the latest complete frame (RGBA8888) under
 *     an internal lock; call rfb_release_frame() when done copying/uploading.
 *     If it returns NULL, no lock is held and rfb_release_frame() must NOT
 *     be called.
 *   - rfb_send_key()/rfb_send_pointer() are thread-safe (socket writes are
 *     serialized with a critical section).
 */

#ifndef TOSHL_RFB_CLIENT_H
#define TOSHL_RFB_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rfb_client rfb_client_t;

/* Connect + handshake. Returns NULL on failure (check rfb_last_error()). */
rfb_client_t *rfb_connect(const char *host, uint16_t port);

/* Spawn the update-pump thread. Call once after rfb_connect(). */
bool rfb_start(rfb_client_t *c);

/* Framebuffer geometry as reported by ServerInit. */
void rfb_get_size(rfb_client_t *c, int *w, int *h);

/*
 * Acquire the latest complete frame. Returns NULL if no complete frame has
 * arrived yet (e.g. VM still booting); in that case do not call
 * rfb_release_frame(). If `frame_serial` is non-NULL it receives a counter
 * that increments on every new frame; compare against your last-seen value
 * to skip redundant GL uploads.
 * Pixel format: tightly packed RGBA, 4 bytes/px, row-major, no padding.
 */
const uint8_t *rfb_acquire_frame(rfb_client_t *c, uint32_t *frame_serial);
void           rfb_release_frame(rfb_client_t *c);

/* Input injection. keysym = X11 keysym (see keymap.h). */
void rfb_send_key(rfb_client_t *c, uint32_t keysym, bool down);
/* buttons: bit0=left bit1=middle bit2=right. x/y in framebuffer coords. */
void rfb_send_pointer(rfb_client_t *c, uint16_t x, uint16_t y, uint8_t buttons);

/* True while the pump thread is alive and the socket is healthy. */
bool rfb_is_connected(rfb_client_t *c);

void rfb_disconnect(rfb_client_t *c); /* joins thread, closes socket, frees */

const char *rfb_last_error(void);

#ifdef __cplusplus
}
#endif
#endif /* TOSHL_RFB_CLIENT_H */
