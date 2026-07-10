# TempleOS-HL1

Run a full, live [TempleOS](https://templeos.org) instance on the in-game
computer monitors of **Half-Life 1** (GoldSrc). Walk up to a terminal, press
use, and you're dropped into Terry Davis's divine 640x480 16-color operating
system, running for real. Not a video, not a mockup. HolyC at your
fingertips, inside Black Mesa.

> **A tribute.** TempleOS is the life's work of Terry A. Davis. It's public
> domain by his wish. This project runs it unmodified and unmocked.

---

## Read this first: VAC / online play

This mod inline-hooks OpenGL calls inside `hl.exe`. **Doing that while
connected to a VAC-secured server can get your account banned.** This is a
**singleplayer / `-insecure` / local-listen-server toy only.** Do not use it
on secure multiplayer servers. You've been warned in bold.

---

## How it actually works

TempleOS is 64-bit; `hl.exe` is a 32-bit process. You can't embed the OS in
the game. So instead:

```
 QEMU (64-bit child process)              Half-Life client.dll (this mod)
 ┌───────────────────────────┐   VNC/RFB  ┌────────────────────────────────┐
 │ TempleOS, headless        │  loopback  │ tiny RFB client (Raw encoding) │
 │ -vnc 127.0.0.1:0          │◄──────────►│ MinHook on glTexImage2D        │
 │ public-domain ISO         │            │ blits framebuffer → monitor tex│
 └───────────────────────────┘            │ +use = terminal mode, kb/mouse │
                                          └────────────────────────────────┘
```

1. **VM**: QEMU boots TempleOS headless with a loopback VNC server.
2. **Bridge**: the mod speaks just enough RFB (RFC 6143, Raw encoding, no
   auth) to stream the framebuffer as RGBA every frame.
3. **Injection (Path B)**: MinHook intercepts the engine's `glTexImage2D`,
   fingerprints monitor screen textures as they upload, then
   `glTexSubImage2D`s the live TempleOS image into them each frame. Every
   monitor in the map showing that texture becomes a real screen.
4. **Interaction**: `+use` while looking at a monitor enters *terminal
   mode*: movement locks, keyboard/mouse route to the VM over RFB. **F10**
   exits.

## Repo layout

```
src/rfb/         minimal RFB/VNC client (Raw encoding), no external deps
src/glhook/      MinHook-based glTexImage2D hijack + fingerprint + blit
src/vmproc/      QEMU launcher (Job Object: VM dies with the game)
src/client/      terminal-mode orchestrator + keymap + SDK wiring notes
tools/           rfb_probe: prove the VM pipeline works before touching HL
vm/              setup script; TempleOS.iso + qemu_path.txt live here
monitor_fingerprints.txt   which textures become screens (see discovery)
```

## Build

Prerequisites: Half-Life SDK, a 32-bit MSVC toolchain, QEMU, CMake, and the
MinHook submodule.

```sh
git clone --recursive https://github.com/aravpanwar/half-life-templeos
cd half-life-templeos

# if you cloned without --recursive:
#   git submodule update --init

# 1. VM + ISO
cd vm && powershell -ExecutionPolicy Bypass -File setup.ps1 && cd ..

# 2. sanity-check the pipeline with NO game involved
cmake -B build -A Win32
cmake --build build --config Release --target rfb_probe
#   (launch qemu per setup.ps1's printed command, then:)
build\Release\rfb_probe.exe 127.0.0.1 5900 frame.ppm   # open frame.ppm

# 3. build toshl_core and splice into the HL SDK cl_dll
#    (see the SDK WIRING block in src/client/terminal_mode.cpp)
```

## Finding your monitor textures (discovery mode)

The screens are hijacked by texture fingerprint, so you tell the mod which
textures to take over:

1. Set env var `TOSHL_LOG=1` before launching.
2. Load a map, look at the monitor you want.
3. Open `toshl_uploads.log`, find the screen texture (usually 64x64 or
   128x128), copy its `WxH:hash` line into `monitor_fingerprints.txt`.
4. Restart the map. That monitor is now TempleOS.

## Roadmap ideas

- CRT/scanline shader pass on the monitor quad
- Persist VM state into HL save games (serialize a QEMU savestate alongside)
- Per-map independent VMs (RAM permitting)
- PC-speaker → DirectSound so you hear the hymns
- Multiple distinct fingerprints → different screens run different programs

## Licensing

- **This mod's code:** see [LICENSE](LICENSE).
- **TempleOS:** public domain (Terry A. Davis). The ISO may be redistributed.
- **QEMU:** GPL, *not bundled*. Kept as a separate process the mod talks to
  over a socket, deliberately, so nothing GPL is linked into the HL SDK-
  derived DLLs. You point the mod at your own QEMU install.
- **MinHook:** BSD-2, SDK-compatible.
- **Half-Life:** you must own it; assets are not distributed here.

## Credits

In memory of Terry A. Davis (1969-2018). *"An idiot admires complexity, a
genius admires simplicity."*
