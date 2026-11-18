# Browser core

This directory contains the C ABI bridge and reproducible Emscripten build for
the existing PAK, WAV/Vorbis, and spectrum core. It does not contain browser UI
glue.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File wasm/core/build.ps1
```

The script pins Emscripten `3.1.74`, downloads the SDK into `player/obj/emsdk`,
and puts compiler caches and objects under `player/obj`. It does not install a
global toolchain. Outputs are `core.js` (an ES module; named `.js` so any static
server serves a JavaScript MIME type) and `core.wasm` in this directory.

The ES module default-exports an async factory:

```js
import createMemoirsCore from "./core.js";

const core = await createMemoirsCore();
```

Each factory call creates an independent instance. The same import can be used
by the main thread or an ES module Web Worker. A web server must serve `.mjs`
as `text/javascript` and `.wasm` as `application/wasm`.

## C ABI

The generated instance exposes these WASM functions as `_alloc`, `_dealloc`,
and so on. Integer arguments and results are JavaScript numbers.

| Export | Contract |
| --- | --- |
| `alloc(size)` | Allocate `size` bytes from WASM memory; returns a byte pointer or `0`. |
| `dealloc(ptr)` | Release a pointer returned by `alloc`. |
| `parse_pak(headerPtr, headerLength, fileSize)` | Parse at least the first `0x203a4` PAK bytes; returns entry count or `0`. |
| `entry_name(i)` | UTF-8, NUL-terminated name pointer or `0`. Valid until the next successful `parse_pak`. |
| `entry_offset(i)` | Absolute file offset of entry `i`, or `0`. |
| `entry_length(i)` | Entry byte length, or `0`. |
| `decode(ptr, len)` | Detect WAV/OGG by magic, return 44.1 kHz stereo frame count or `0`. |
| `samples()` | Interleaved little-endian i16 pointer, or `0`. Valid until the next decode or `clear_pcm`. |
| `clear_pcm()` | Release the current decoded PCM buffer. |
| `sm_spectrum(ptr)` | Analyze exactly 1024 interleaved i16 stereo frames and return a float pointer. |
| `sm_spectrum_reset()` | Reset the spectrum instance state. |

`sm_spectrum` returns 611 floats in this order: `bars[43]`,
`barsPrev[43]`, `barsLevel[43]`, `waveL[240]`, `waveR[240]`, `envL`,
`envR`. The build intentionally does not export `HEAPF32`; read it with
`new Float32Array(core.HEAPU8.buffer, ptr, 611)`.

`HEAPU8` and `HEAP16` are exposed by the factory. The ABI uses 32-bit lengths
and offsets, so a single PAK must be smaller than 4 GiB. The JS side must keep
`core.wasm` beside `core.js`.

## Derived scene painter exports

Revision 1 adds a derived rendering layer under `wasm/render/`. It compiles the
desktop painting constants and algorithms read-only; `player/src` is not modified.

| Export | Contract |
| --- | --- |
| `scene_state_size()` | Byte size of the `SceneState` POD (2688). |
| `scene_begin()` | Reset the command buffer. Call once per frame before rendering. |
| `scene_set_state(ptr)` | Copy a JS-filled `SceneState` into WASM. Running wing envelopes are preserved. |
| `scene_advance(id, seconds, playing, paused)` | Advance wing envelopes from the spectrum in the current state. |
| `scene_render()` | Emit POD drawing commands via the derived painter. |
| `scene_commands()` / `scene_points()` / `scene_count()` | Read the command and point pools for Canvas 2D replay. |

`scene.js` replays the command stream on a 640x480 canvas. `verify_scene.mjs`
validates 13 deterministic frames headlessly; `dump_scene.mjs` plus
`tools/verify_scene_pixels.py` diffs a settled frame against the desktop
`hsbgm.exe --shot` baseline. The current baseline differs in 2 of 307200 pixels.

## Verify

```powershell
node wasm/core/verify.mjs --pak FILE.pak --ogg FILE.ogg --wav FILE.wav
```

The verifier checks the factory and memory views, parses every PAK entry,
decodes OGG and WAV through the exported ABI, validates non-silent PCM, checks
the spectrum layout/reset, and creates a separate Worker instance. Use
`--expect-ogg-frames N` or `--expect-wav-frames N` to pin a known fixture's
sample count.

## Verified build

Revision 1 (with the derived scene painter) was built with Emscripten `3.1.74`:

- `core.js`: 11102 bytes
- `core.wasm`: 119246 bytes
- `core.wasm` SHA-256: `67FBCB28E3A299EE5FF58FF46E025343C33F3E7512CE4A49758385C6928675F6`

The original algorithm-only build from 2026-11-17 produced these hashes:

- `core.mjs`: `ed25eec0311f43572a7f1123f7b18afe4689f4f30e378409116fa9a1081363ee`
- `core.wasm`: `15a50477a8e27097f6c09a5b762b77e64529098633fb235f0ac446b38367d99a`

Validation covered a real 12,865,016-byte PAK with 30 entries, a real Vorbis
file that decoded to 3,029,478 frames, a WAV that decoded to 264,600 frames,
heap growth from 32 MiB to about 65 MiB, the 611-float spectrum snapshot and
reset behavior, plus main-thread and module-Worker execution in headless
Chrome.
