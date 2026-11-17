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
global toolchain. Outputs are `core.mjs` and `core.wasm` in this directory.

The ES module default-exports an async factory:

```js
import createMemoirsCore from "./core.mjs";

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
`core.wasm` beside `core.mjs`.

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

The delivered files were built with Emscripten `3.1.74` on 2026-11-17. Two
consecutive builds produced identical output hashes:

- `core.mjs`: `ed25eec0311f43572a7f1123f7b18afe4689f4f30e378409116fa9a1081363ee`
- `core.wasm`: `15a50477a8e27097f6c09a5b762b77e64529098633fb235f0ac446b38367d99a`

Validation covered a real 12,865,016-byte PAK with 30 entries, a real Vorbis
file that decoded to 3,029,478 frames, a WAV that decoded to 264,600 frames,
heap growth from 32 MiB to about 65 MiB, the 611-float spectrum snapshot and
reset behavior, plus main-thread and module-Worker execution in headless
Chrome.
