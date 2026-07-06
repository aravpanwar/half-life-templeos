/*
 * rfb_probe.c - standalone sanity check for the RFB pipeline.
 *
 * Connects to a running QEMU VNC server, grabs one frame, writes it as a
 * PPM you can open anywhere. Proves the VM + rfb_client half works BEFORE
 * you touch Half-Life. Build:
 *
 *   cl /I..\src\rfb rfb_probe.c ..\src\rfb\rfb_client.c ws2_32.lib
 *
 * Run (after launching qemu with -vnc 127.0.0.1:0):
 *   rfb_probe.exe 127.0.0.1 5900 frame.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "rfb_client.h"

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int         port = argc > 2 ? atoi(argv[2]) : 5900;
    const char *out  = argc > 3 ? argv[3] : "frame.ppm";

    rfb_client_t *c = rfb_connect(host, (unsigned short)port);
    if (!c) { printf("connect failed: %s\n", rfb_last_error()); return 1; }
    if (!rfb_start(c)) { printf("start failed: %s\n", rfb_last_error()); return 1; }

    int w, h; rfb_get_size(c, &w, &h);
    printf("connected, framebuffer %dx%d, waiting for first frame...\n", w, h);

    /* rfb_acquire_frame holds the lock only when it returns a frame, so
       only release once we actually got one. */
    const uint8_t *frame = NULL;
    for (int i = 0; i < 200 && !frame; i++) {   /* up to ~10s */
        frame = rfb_acquire_frame(c, NULL);
        if (!frame) Sleep(50);
    }
    if (!frame) { printf("no frame arrived\n"); rfb_disconnect(c); return 1; }

    FILE *f = fopen(out, "wb");
    if (!f) { printf("cannot open %s\n", out); rfb_release_frame(c); rfb_disconnect(c); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) fwrite(frame + i * 4, 1, 3, f); /* RGB, drop A */
    fclose(f);
    rfb_release_frame(c);

    printf("wrote %s. If it shows TempleOS, the pipeline works.\n", out);
    rfb_disconnect(c);
    return 0;
}
