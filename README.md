# TempleOS-HL1

Run a full, live [TempleOS](https://templeos.org) instance on a working
computer monitor inside **Half-Life 1** (GoldSrc). Walk up to the terminal,
press X, and you're driving Terry Davis's 640x480 16-color operating system,
running for real inside Black Mesa. Not a video, not a mockup. HolyC at your
fingertips.

![Three TempleOS programs running live on a Black Mesa control-room monitor in Half-Life: the Varoom racing game, a rotating 3D tank, and spinning polygons, all streamed in real time](docs/demo.gif)

## How it works

TempleOS is 64-bit and `hl.exe` is a 32-bit process, so you can't embed the OS
in the game. Instead it runs beside the game and is streamed in:

```
 QEMU (64-bit child process)               Half-Life client.dll (this mod)
 ┌───────────────────────────┐   VNC/RFB   ┌────────────────────────────────┐
 │ TempleOS live CD, headless│  loopback   │ tiny RFB client (Raw encoding) │
 │ -vnc 127.0.0.1:0          │◄───────────►│ streams 640x480 RGBA per frame │
 │ public-domain ISO         │             │ draws it as a quad on a monitor│
 └───────────────────────────┘             │ press X to type; keys to the VM│
                                           └────────────────────────────────┘
```

1. **VM.** QEMU boots the TempleOS live-CD ISO headless with a loopback VNC
   server. The mod auto-answers the boot prompts so it lands on the desktop.
2. **Bridge.** A background thread speaks just enough RFB (RFC 6143, Raw
   encoding, no auth) to pull TempleOS's framebuffer as RGBA every frame.
3. **Render.** From the client's world-draw pass the mod uploads that frame to
   its own GL texture and draws it as a flat quad pinned to a monitor surface.
   It uses the mod's own texture and geometry, so it never touches the map's
   shared, tiled textures. The screen stays crisp with zero bleed onto walls. A
   GLSL fragment shader then gives it curved glass, scanlines and an RGB shadow
   mask, so it reads as a real CRT instead of a flat sprite. If the driver has
   no GL 2.0, the shader is skipped and the plain quad is drawn.
4. **Interaction.** Walk up to the console and press **X** to sit down and
   type, which only works when you are close; your keyboard then routes straight
   to TempleOS over RFB. Press **Z** to blow the panel up fullscreen so it is
   readable, from anywhere in the room, and **F10** to stand back up. TempleOS is
   keyboard-driven by design, so the keyboard is all you need.
5. **Sound (opt-in).** Set `toshl_sound 1` and reload the map to route
   TempleOS's PC-speaker output (the beeps and the hymns) to the host over SDL
   audio. It is off by default, and if the audio backend can't start headless
   the VM simply launches silent, so sound can never hold TempleOS back.

![The panel pinned to the monitor in the c1a0 control room, curved CRT glass and all](docs/screenshot.jpg)

By default the panel auto-locks onto a specific Black Mesa control-room monitor
in `c1a0`, so TempleOS is just there when you arrive. Set `toshl_fixed 0` to aim
at any surface and drop it there instead.

## Controls

No key binding needed; the mod claims these keys directly.

| Key / command         | What it does                                     |
|-----------------------|--------------------------------------------------|
| X                     | sit down and type (only when near the terminal)  |
| Z                     | toggle fullscreen zoom (works from anywhere)     |
| F10                   | stop typing                                      |
| `toshl_size N`        | panel width in game units                        |
| `toshl_aspect N`      | panel height / width (0.75 = 4:3)                |
| `toshl_shiftr N`      | slide the panel right (negative = left)          |
| `toshl_shiftu N`      | slide the panel up (negative = down)             |
| `toshl_fwd N`         | push the panel off the wall toward you           |
| `toshl_fixed 0`       | drop the baked spot; aim and +use to place it    |
| `toshl_crt 0`         | turn the CRT shader off (curvature/scanlines/mask)|
| `toshl_crt_curve N`   | barrel curvature amount (0 = flat glass)         |
| `toshl_crt_scan N`    | scanline depth (0 = none)                        |
| `toshl_crt_mask N`    | RGB shadow-mask depth (0 = none)                 |
| `toshl_crt_bezel N`   | curvature-border opacity (1 = black, 0 = clear)  |
| `toshl_sound 1`       | opt-in PC-speaker audio (reload the map to apply) |

Console commands (type them in the `~` console):

| Command | What it does |
|---------|--------------|
| `toshl_zoom` | toggle the fullscreen zoom (same as pressing Z) |
| `toshl_next` / `toshl_prev` | discovery tool: cycle the highlighted surface |
| `toshl_lock` | discovery tool: print the highlighted surface's fingerprint |

The discovery commands only do anything when the mod is started with the
`TOSHL_DISCOVER` environment variable set; they exist to locate a monitor
surface when pinning the panel on a new map.

## Using TempleOS

TempleOS is keyboard-driven and case-sensitive; its shell is a live HolyC JIT,
so statements end with `;`. Once you have pressed **X** to type and **Z** to
zoom, a few commands get you moving:

| Command | What it does |
|---------|--------------|
| `Dir;` | list the current folder |
| `Cd("::/Demo");` | change folder (`::/` is the drive root; `Cd("..");` goes up) |
| `#include "::/Demo/Games/Castle/Castle"` | compile and run a program (no extension needed) |
| `"Hello from Black Mesa\n";` | a bare string prints itself |
| `Print("%d\n", 2 + 2);` | formatted print |

Worth trying: the games and demos under `::/Demo/Games` and `::/Demo/Graphics`
(run `Dir;` there to see what your ISO ships), and TempleOS's built-in oracle,
"God's Word", from the menu that Terry believed let God speak through the RNG.
Most demos exit with **Esc**.

![The HolyC command line and the God menu driven live over the in-game monitor](docs/holyc.gif)

For sound, set `toshl_sound 1` and reload the map (`map c1a0`). TempleOS makes
all its noise on the PC speaker, which the mod routes to your host, so once it
is on, try something musical like God Song from the menu or a demo under
`::/Demo`. If the audio backend can't start on your machine the VM just runs
silent, it never blocks TempleOS from booting.

## Download and run

Prefer not to build it? Grab the prebuilt client from the
[latest release](https://github.com/aravpanwar/half-life-templeos/releases/latest)
and drop it next to Half-Life. No compiler needed.

1. Own Half-Life on Steam and switch it to the `steam_legacy` branch (right-click
   Half-Life, Properties, Betas). Install
   [QEMU for Windows](https://www.qemu.org/download/#windows).
2. Unzip the release into your Half-Life folder so the `half-life-templeos` folder
   sits next to `hl.exe` and the `valve` folder.
3. Run `half-life-templeos\vm\setup.ps1` to fetch the TempleOS ISO and locate QEMU.
4. With Steam running:
   `hl.exe -game half-life-templeos -console -insecure +map c1a0`

Walk up to the control-room monitor, press **X** to type and **Z** to zoom. The
mod is client-only and reuses Half-Life's stock server DLL automatically, so no
Valve files need copying.

## Repo layout

```
src/rfb/         minimal RFB/VNC client (Raw encoding), no external deps
src/glhook/      GL rendering: world-space quad + fullscreen zoom (+ discovery)
src/vmproc/      QEMU launcher (Job Object: the VM dies with the game)
src/client/      orchestrator: RFB to render, placement, input, keymap
src/sdk_glue/    Half-Life SDK glue: entry points, +use edge, window subclass
integration/     patch + script that wire the mod into the HL SDK client build
tools/           rfb_probe: prove the VM pipeline works before touching HL
vm/              setup script; TempleOS.iso + qemu_path.txt live here
```

## Build it from source

Want to compile it yourself instead of using the [prebuilt
release](#download-and-run)? The mod's sources compile into the Half-Life SDK's
client library, so you build that library and assemble a small mod folder next
to Half-Life. It is several steps, but every one is a concrete command.

### Prerequisites

- **Half-Life on Steam, switched to the `steam_legacy` branch.** In Steam,
  right-click Half-Life, choose Properties, Betas, and select `steam_legacy`.
  The 25th-Anniversary update replaced GoldSrc's classic OpenGL path, and the GL
  rendering this mod relies on only works on the pre-anniversary engine.
- **Visual Studio 2019** with the "Desktop development with C++" workload (the
  v142 toolset and 32-bit tools).
- **QEMU for Windows** (https://www.qemu.org/download/#windows).
- **Steam must be running whenever you launch.** GoldSrc loads through Steam; if
  Steam is closed the game sits at the menu and the VM never starts.

### 1. Get the sources, side by side

Clone both into the same parent folder and keep the `half-life-templeos` folder
name (the project references the mod by that relative path).

```powershell
git clone https://github.com/SamVanheer/halflife-updated
git -C halflife-updated checkout steam_legacy   # developed against commit edbae22
git clone --recursive https://github.com/aravpanwar/half-life-templeos
#   git submodule update --init   # if you forgot --recursive
```

The mod pulls in MinHook as a submodule. If that clone fails with `Filename too
long`, your path is deep enough to hit Windows' 260-character limit; run `git
config --global core.longpaths true` (or clone into a short path like `C:\dev`)
and retry `git submodule update --init`.

### 2. Fetch the ISO and point at QEMU

```powershell
powershell -ExecutionPolicy Bypass -File half-life-templeos\vm\setup.ps1
```

Downloads the public-domain TempleOS ISO into `vm\` and writes `vm\qemu_path.txt`
with the path to `qemu-system-x86_64.exe` (edit it if QEMU is somewhere unusual).

### 3. Wire the mod into the SDK

```powershell
powershell -ExecutionPolicy Bypass -File half-life-templeos\integration\apply-integration.ps1 -SdkPath halflife-updated
```

This applies one git patch (`integration/halflife-updated.patch`): it adds the
mod sources to the client project and splices three entry-point calls into
`cdll_int.cpp`, `input.cpp` and `tri.cpp`. Run it again with `-Revert` to undo it
cleanly.

### 4. Build the client library

Open `halflife-updated\projects\vs2019\projects.sln` in Visual Studio 2019,
choose **Release / Win32**, and build the **hl_cdll** project. The post-build
step copies `client.dll` into the mod folder named in
`halflife-updated\filecopy.bat` (edit that path if your Steam library lives
elsewhere).

### 5. Assemble the mod folder

The build drops in `client.dll`, but a GoldSrc mod folder needs a few more
files. Run this once, from the parent folder, adjusting the Half-Life path if
yours differs:

```powershell
$hl   = "C:\Program Files (x86)\Steam\steamapps\common\Half-Life"
$mod  = "$hl\half-life-templeos"
$repo = "half-life-templeos"   # this repo
New-Item -ItemType Directory -Force "$mod\vm" | Out-Null
Copy-Item "$repo\liblist.gam", "$repo\monitor_fingerprints.txt" $mod
Copy-Item "$repo\vm\TempleOS.iso", "$repo\vm\qemu_path.txt" "$mod\vm\"
```

The mod is client-only, so it never ships a server DLL. `liblist.gam` sets
`fallback_dir "valve"`, so the engine loads Half-Life's stock `hl.dll`, maps and
assets straight from your `valve` folder.

### 6. Run it

Make sure Steam is running, then launch on the steam_legacy branch and load a
map with a monitor:

```powershell
& "$hl\hl.exe" -game half-life-templeos -console -insecure +map c1a0
```

TempleOS boots on the control-room monitor in `c1a0`. Walk up, press **X** to
type and **Z** to zoom; then use the commands under [Using TempleOS](#using-templeos).

### Optional: prove the VM pipeline first

Before involving the game you can confirm QEMU and the RFB client work on their
own. Start the VM by hand (leave it running), then grab a frame in another shell:

```powershell
cd half-life-templeos
cmake -B build -A Win32
cmake --build build --config Release --target rfb_probe
& (Get-Content vm\qemu_path.txt) -m 512 -drive file=vm\TempleOS.iso,media=cdrom -boot d -snapshot -vnc 127.0.0.1:0
build\Release\rfb_probe.exe 127.0.0.1 5900 frame.ppm   # then open frame.ppm
```

## Roadmap

- Persist a writable TempleOS data drive so files survive between sessions
- Terminals placed in more maps
- Mouse support ([#1](https://github.com/aravpanwar/half-life-templeos/issues/1)).
  TempleOS is PS/2-only, and QEMU confines a PS/2 mouse driven over VNC to a
  small region; making the cursor track fully would need an absolute pointing
  device TempleOS does not have a driver for.

## Licensing

- **This mod's code:** see [LICENSE](LICENSE).
- **TempleOS:** public domain (Terry A. Davis). The ISO may be redistributed.
- **QEMU:** GPL, *not bundled*. Kept as a separate process the mod talks to
  over a socket, so nothing GPL is linked into the HL SDK-derived DLLs. You
  point the mod at your own QEMU install.
- **MinHook:** BSD-2, SDK-compatible.
- **Half-Life:** you must own it; assets are not distributed here.

## Credits

In memory of Terry A. Davis (1969-2018). *"An idiot admires complexity, a
genius admires simplicity."*
