# Architecture

## Why out-of-process

Two hard facts force the design:

- **`hl.exe` is 32-bit; TempleOS is x86-64.** No in-process VM is possible.
- **QEMU is GPL; the Half-Life SDK license is GPL-incompatible.** Linking a
  VM into the mod DLL would be a license violation *and* a linkage nightmare.

Running TempleOS in a **separate QEMU process** and talking to it over a
loopback socket solves both at once. The only thing crossing the boundary is
pixels (VM→game) and input events (game→VM), over VNC/RFB.

## Data flow, per frame

```
QEMU renders TempleOS ─► VNC server (in QEMU) ─► loopback TCP
      ▲                                              │
      │ KeyEvent / PointerEvent                      ▼
      │                                    rfb pump thread (ours)
      │                                    decodes Raw rects into
      │                                    double-buffered RGBA
      │                                              │
 terminal_mode routes                                ▼
 keyboard+mouse ◄──────────  game render thread: rfb_acquire_frame()
                                     │
                                     ▼
                             glhook_blit(): glTexSubImage2D into every
                             captured monitor texture
```

The RFB client runs its own thread so VM latency never stalls the game loop.
The game thread only ever does a locked pointer grab + a texture upload.

## Component contracts

### `src/rfb`: the bridge
- RFB 3.8, security type **None**, **Raw** encoding only. That's all QEMU
  needs and all TempleOS's tiny 640x480x4bpp frames warrant (~1.2 MB raw,
  trivial over loopback).
- We `SetPixelFormat` to 32bpp with R/G/B shifts 0/8/16 so the bytes arrive
  as `GL_RGBA + GL_UNSIGNED_BYTE`: **zero conversion** before upload.
- Double-buffered: pump thread composes into the back buffer (incremental
  rects compose correctly because we copy front→back after each publish),
  game thread reads the front under a short critical section.
- A `serial` counter lets the renderer skip GL uploads when no new frame has
  arrived.

### `src/glhook`: Path B injection
- MinHook installs an inline hook on `glTexImage2D` at `HUD_Init` time,
  before any map texture uploads, since GoldSrc uploads world/model textures
  lazily on map load.
- Every upload is FNV-1a hashed with its dimensions. Matches against
  `monitor_fingerprints.txt` record the engine's GL texture *name* (queried
  via `GL_TEXTURE_BINDING_2D`).
- `glhook_blit` scales (nearest-neighbour) the VM framebuffer to each
  captured texture's dimensions and `glTexSubImage2D`s it in, restoring
  previous bind + unpack alignment so the engine never notices.
- Discovery mode logs every upload's `WxH:hash` so you can identify screen
  textures empirically.

Why hook from `client.dll` rather than ship an `opengl32.dll` proxy: we own
`client.dll`, so we avoid re-exporting ~360 GL symbols and avoid loader
ordering fights. The technique (fingerprint at upload, sub-image each frame)
is identical either way; this is just the cleaner delivery vehicle. If you
later want engine-wide injection (menus, other DLLs), the same
`gl_hook.c` compiles into a proxy with only an export table added.

### `src/vmproc`: VM lifecycle
- `CreateProcess` QEMU suspended, assign to a **Job Object** with
  `KILL_ON_JOB_CLOSE`, then resume. The VM therefore cannot outlive
  `hl.exe`, even on a hard crash. No orphaned emulator eating 512 MB.
- `-snapshot` so every session is a pristine boot. Swap to a qcow2 overlay
  (and serialize it into HL saves) if you want persistence.

### `src/client`: orchestration
- `terminal_mode.cpp` is engine-agnostic except three clearly marked splice
  points. It owns: lazy VM launch on first use, RFB connect-with-retry (the
  VNC listener needs a beat to bind), per-frame blit, enter/exit terminal,
  and input routing.
- `keymap.h` maps Win32 VKs → X11 keysyms (what RFB `KeyEvent` expects),
  handling shift for correct ASCII.

## The three SDK splices

All in the Half-Life SDK `cl_dll`:

1. **Lifecycle**: call `TOSHL_Init` in `HUD_Init`, `TOSHL_OnRedraw` at the
   end of `HUD_Redraw`, `TOSHL_OnMapChange` in `HUD_VidInit`,
   `TOSHL_Shutdown` on unload.
2. **+use**: in `CL_CreateMove`, rising edge of `IN_USE` while not already
   in a terminal → `TOSHL_TryEnterTerminal`. While in a terminal, zero the
   movement fields so the player stands still.
3. **Keyboard**: subclass the engine HWND; in the wndproc, give
   `TOSHL_HandleKey` first refusal. If it consumes the event, don't chain to
   the engine (keeps WASD out of gameplay while typing HolyC).

## Failure modes & handling

| Failure | Behaviour |
|---|---|
| QEMU not found | launcher reads `vm/qemu_path.txt`; logs a clear error |
| VNC not bound yet | RFB connect retries ~10s before giving up |
| No fingerprints match | monitors stay vanilla; log tells you to run discovery |
| VM frame not ready | `rfb_acquire_frame` returns NULL; renderer no-ops |
| Game crash | Job Object kills QEMU automatically |

## Performance

- Frame is ~1.2 MB; loopback copy + one `glTexSubImage2D` per monitor per
  frame. On any modern machine this is noise next to GoldSrc's own draw.
- The `serial` check means idle TempleOS screens cost ~nothing: no upload
  when the VM hasn't redrawn.
