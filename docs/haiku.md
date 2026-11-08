# Haiku

Sunshine runs on [Haiku](https://www.haiku-os.org) (x86_64), so any Moonlight client can stream
and control a Haiku desktop.

## What works

| Feature | How it is done on Haiku | Notes |
|---|---|---|
| Screen capture | `BScreen::ReadBitmap()` through app_server | Works with every graphics driver: app_server converts from the screen's depth. One Haiku screen per stream. |
| Hardware encoding | Vulkan Video (`h264_vulkan`, `hevc_vulkan`, `av1_vulkan`) | Used when the GPU's Vulkan driver supports video encoding. FFmpeg loads `libvulkan` at runtime, so the same build runs everywhere. |
| Software encoding | x264, x265, SVT-AV1 from HaikuPorts | Automatic fallback when no hardware encoder is available. |
| System audio | Media Kit pass-through node after the system mixer | Inserted only while a client streams. The host keeps playing audio unless "host audio" is turned off for the stream. |
| Keyboard, mouse, text | libvirtualhid's `virtualhid_input` input_server add-on | Events go through input_server like a real device, so keymaps, mouse settings and input methods apply. |
| Discovery | Built-in mDNS/DNS-SD responder | No daemon needed. Moonlight can always add the computer by IP address too. |
| Gamepads | Not yet | Haiku has no virtual joystick device; see [Roadmap](#roadmap). |
| Touch and pen | Not yet | Not implemented in the Haiku backend yet. |
| HDR | No | Haiku has no HDR output. |

NVENC is not available: it needs NVIDIA's proprietary CUDA driver, which does not exist for Haiku.
NVIDIA GPUs can still encode in hardware through Vulkan Video when their Haiku Vulkan driver
exposes it.

## Building

Requirements: an up-to-date x86_64 Haiku (release or nightly) with access to the HaikuPorts
repositories, about 2 GB of free disk space, and network access during the build.

```sh
git clone --recurse-submodules <this repository>
cd <checkout>
packaging/haiku/build.sh --install-deps
```

`build.sh` options:

| Option | Meaning |
|---|---|
| `--install-deps` | Install build dependencies with `pkgman` (requested by resolvable, e.g. `devel:libx264`). |
| `--prefix DIR` | Install prefix. Default: `/boot/home/config/non-packaged`. |
| `--jobs N` | Parallel build jobs. Default: number of CPUs. |
| `--no-vulkan` | Build without the Vulkan Video encoder. |
| `--clean` | Rebuild FFmpeg and Sunshine from scratch. |

The script builds FFmpeg through `third-party/build-deps` (needed for Sunshine's bitstream
helpers), linked against the encoder libraries from HaikuPorts, then builds and installs Sunshine.

> [!IMPORTANT]
> Use Boost from HaikuPorts (`pkgman install devel:libboost_log ...`). Upstream Boost does not
> build on Haiku without the patches HaikuPorts carries.

## Running

1. Restart input_server once after installing, so it loads
   `add-ons/input_server/devices/virtualhid_input` (rebooting is the simplest way). Until then,
   clients can watch but not control the computer; Sunshine logs a warning explaining this.
2. Start `sunshine` from Terminal.
3. Open `https://localhost:47990`, create the web UI login, and pair Moonlight.

Settings and logs live in `~/config/settings/Sunshine` (or `$XDG_CONFIG_HOME/sunshine` when that
variable is set).

When Haiku shuts down or the application is asked to quit, Sunshine exits cleanly and restores
the audio routing.

## Troubleshooting

**pkgman reports "Operation not supported" for every repository.** The Network Kit of this Haiku
build has no HTTPS support, which happens with self-built systems made without an `@nightly` or
`@release` profile. Add `HAIKU_BUILD_FEATURE_SSL = 1 ;` to `build/jam/UserBuildConfig` and rebuild.
Until then, `build.sh --install-deps` detects the problem and runs pkgman through
`packaging/haiku/tools/pkgman-curl-relay.py`, which fetches the repository files with curl through
a relay on 127.0.0.1. The relay changes each repository's download address only while pkgman runs,
and pkgman still checks every checksum. It also works on its own, e.g.
`python3 packaging/haiku/tools/pkgman-curl-relay.py install -y <package>`.

**Moonlight does not find the computer.** Add it by IP address in Moonlight. Discovery needs UDP
port 5353 to be free for sharing; Sunshine coexists with HaikuPorts' `mdnsd`.

**No sound in the stream.** Check the log for "Couldn't capture system audio". If the system
audio ever stays disconnected (for example after a crash during a stream), open the Media
preferences and choose "Restart media services", or simply restart Sunshine: it reconnects the
mixer to the audio output at startup.

**Mouse and keyboard do nothing.** Make sure `virtualhid_input` is installed in
`~/config/non-packaged/add-ons/input_server/devices/` and input_server was restarted.

**Which encoder is used?** The log lists the encoders that were probed at startup. To force one,
set `encoder = vulkan` or `encoder = software` in the configuration.

## How the port is put together

The port follows Sunshine's platform layer (`src/platform/common.h`) and keeps Haiku specifics in
a few places:

| Location | Purpose |
|---|---|
| `src/platform/haiku/` | Capture, audio tap, mDNS, BApplication lifetime and miscellaneous platform functions. |
| `third-party/libvirtualhid/src/platform/haiku/` | libvirtualhid keyboard/mouse backend and its input_server add-on. Generic, so other libvirtualhid users get Haiku input too. |
| `cmake/*/haiku.cmake` | Build settings selected with CMake's `HAIKU` variable. |
| `src/video.cpp` | Enables the Vulkan encoder on Haiku and skips NVENC. |
| `src/boost_process_compat.h` | Corrects Boost.Process v1's "still running" marker for Haiku's wait-status layout (otherwise launched apps look like they exited immediately). |
| `packaging/haiku/` | Build script, plus patches applied to submodules until they are upstream: the libvirtualhid Haiku backend, and `FFMPEG_EXTRA_CONFIGURE_FLAGS` for build-deps. |

Only public, long-standing Haiku APIs are used (BScreen, BBitmap, BApplication, the Media Kit,
BInputServerDevice, `get_key_map()`), so the port should keep working across nightly, beta and
release builds.

### Input add-on protocol

The libvirtualhid backend talks to the add-on through the kernel port `libvirtualhid:input`. Each
port message is one fixed-size event record (type, two integers, two floats), followed by UTF-8
bytes for text input only; the port message code carries the protocol version (`'LVH1'`). The
add-on checks every message against its size before using it, because it runs inside
input_server. New versions only add event types, and the add-on ignores types it does not know,
so the add-on and the library can be updated independently. See `haiku_input_protocol.hpp`.

The add-on behaves like Haiku's own keyboard and mouse drivers: it follows the user's keymap
(including dead keys such as `´` + `e` = `é`), lock key state and double-click speed, merges
bursts of relative mouse motion and spaces pointer events about 1 ms apart so input_server applies
each one to the current cursor position, and releases every key and button when a client
disconnects. Dead keys are composed without the inline accent preview Haiku's own driver shows.

Like other Haiku IPC, the port is not access-controlled: any local program can send input through
it. This matches Haiku's single-user model, but keep it in mind on shared machines.

### Audio tap

Haiku's system mixer has a single output, owned by the sound card, and there is no loopback
device. While a stream is active, Sunshine registers a small filter node in its own team and
routes `mixer -> tap -> sound card`. The tap copies samples into a ring buffer and forwards each
buffer unchanged, or silenced when host audio is disabled, so the sound card's timing is never
disturbed. The original connection is restored when the stream ends.

The mixer delivers audio in buffers that are usually longer than one stream packet, so the reader
adapts its wait time and latency bound to the buffer size it actually sees. When the mixer goes
quiet, the stream continues with paced silence, and a route that stays silent for several seconds
is reconnected (at most every 30 seconds). Silence inserted while waiting for audio is taken back
out once audio resumes, so it does not accumulate as latency.

## Roadmap

- **Gamepads:** a virtual joystick driver that BJoystick (and therefore SDL) can open, exposed to
  Sunshine through libvirtualhid's gamepad interface.
- **GPU-assisted capture:** an optional capture provider that graphics drivers can implement to
  hand over the scan-out buffer directly, falling back to app_server capture when absent.
- **IPv6 discovery and streaming.**
- **HaikuPorts recipe**, so Sunshine installs as a regular `.hpkg`.
