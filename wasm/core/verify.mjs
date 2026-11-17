import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import path from "node:path";
import { Worker, parentPort, workerData } from "node:worker_threads";
import { fileURLToPath, pathToFileURL } from "node:url";

const coreDir = path.dirname(fileURLToPath(import.meta.url));
const coreMjs = path.join(coreDir, "core.mjs");
const coreWasm = path.join(coreDir, "core.wasm");
const pakHeaderLength = 0x203A4;
const spectrumFloats = 43 * 3 + 240 * 2 + 2;

async function instantiateCore() {
    const moduleUrl = pathToFileURL(coreMjs).href;
    const factory = (await import(moduleUrl)).default;
    assert.equal(typeof factory, "function", "core.mjs must default-export the factory");
    const core = await factory({ wasmBinary: readFileSync(coreWasm) });

    for (const name of [
        "_alloc", "_dealloc", "_parse_pak", "_entry_name", "_entry_offset",
        "_entry_length", "_decode", "_samples", "_clear_pcm",
        "_sm_spectrum", "_sm_spectrum_reset"
    ]) {
        assert.equal(typeof core[name], "function", `${name} is not exported`);
    }
    assert.ok(core.HEAPU8 instanceof Uint8Array, "HEAPU8 is not exposed");
    assert.ok(core.HEAP16 instanceof Int16Array, "HEAP16 is not exposed");
    assert.ok(core.HEAPU8.buffer === core.HEAP16.buffer, "heap views disagree");
    return core;
}

function sha256(bytes) {
    return createHash("sha256").update(bytes).digest("hex");
}

function copyToHeap(core, bytes) {
    const ptr = core._alloc(bytes.byteLength);
    assert.notEqual(ptr, 0, `alloc(${bytes.byteLength}) failed`);
    core.HEAPU8.set(bytes, ptr);
    return ptr;
}

function readName(core, ptr) {
    assert.notEqual(ptr, 0, "entry_name returned null");
    const heap = core.HEAPU8;
    let end = ptr;
    while (end < heap.byteLength && heap[end] !== 0) ++end;
    assert.ok(end < heap.byteLength, "entry_name is not NUL terminated");
    return new TextDecoder().decode(heap.subarray(ptr, end));
}

function decodeAudio(core, bytes) {
    const ptr = copyToHeap(core, bytes);
    const frames = core._decode(ptr, bytes.byteLength);
    core._dealloc(ptr);
    assert.ok(frames > 0, "decode returned no frames");

    const samplePtr = core._samples();
    assert.notEqual(samplePtr, 0, "samples returned null after decode");
    assert.equal(samplePtr % 2, 0, "sample pointer is not i16 aligned");

    const heap16 = core.HEAP16;
    const sampleCount = frames * 2;
    const sampleIndex = samplePtr >>> 1;
    assert.ok(sampleIndex + sampleCount <= heap16.length, "PCM exceeds the heap");

    let energy = 0;
    let nonZero = 0;
    for (let i = 0; i < sampleCount; i += 257) {
        const sample = heap16[sampleIndex + i];
        energy += Math.abs(sample);
        if (sample !== 0) ++nonZero;
    }
    assert.ok(nonZero > 0, "decoded PCM is silent");

    const pcmBytes = new Uint8Array(core.HEAPU8.buffer, samplePtr, frames * 4);
    const result = { frames, sampledEnergy: energy, hash: sha256(pcmBytes) };
    core._clear_pcm();
    assert.equal(core._samples(), 0, "clear_pcm did not release samples");
    return result;
}

async function runWorkerSmoke() {
    const core = await instantiateCore();
    const ptr = core._alloc(16);
    assert.notEqual(ptr, 0, "worker alloc failed");
    core.HEAPU8.fill(0x5a, ptr, ptr + 16);
    assert.equal(core.HEAPU8[ptr], 0x5a);
    core._dealloc(ptr);
    core._sm_spectrum_reset();
    parentPort.postMessage({ ok: true, heapBytes: core.HEAPU8.byteLength });
}

function runWorkerTest() {
    return new Promise((resolve, reject) => {
        const worker = new Worker(new URL(import.meta.url), {
            workerData: { mode: "worker-smoke" }
        });
        let settled = false;
        worker.once("message", (message) => {
            settled = true;
            resolve(message);
        });
        worker.once("error", (error) => {
            settled = true;
            reject(error);
        });
        worker.once("exit", (code) => {
            if (!settled && code !== 0) reject(new Error(`worker exited with ${code}`));
        });
    });
}

function argument(name) {
    const index = process.argv.indexOf(name);
    return index >= 0 ? process.argv[index + 1] : undefined;
}

function positiveInteger(value, name) {
    if (value === undefined) return undefined;
    const parsed = Number(value);
    assert.ok(Number.isSafeInteger(parsed) && parsed > 0, `${name} must be positive`);
    return parsed;
}

async function runMain() {
    const pakPath = argument("--pak");
    const oggPath = argument("--ogg");
    const wavPath = argument("--wav");
    assert.ok(pakPath, "usage: node verify.mjs --pak FILE.pak --ogg FILE.ogg [--wav FILE.wav]");
    assert.ok(oggPath, "usage: node verify.mjs --pak FILE.pak --ogg FILE.ogg [--wav FILE.wav]");

    const expectedOggFrames = positiveInteger(argument("--expect-ogg-frames"), "--expect-ogg-frames");
    const expectedWavFrames = positiveInteger(argument("--expect-wav-frames"), "--expect-wav-frames");

    const core = await instantiateCore();
    console.log("factory/exports/heaps: ok");

    const initialHeapBytes = core.HEAPU8.byteLength;
    const growthProbe = core._alloc(initialHeapBytes * 2);
    assert.notEqual(growthProbe, 0, "memory-growth probe allocation failed");
    assert.ok(core.HEAPU8.byteLength > initialHeapBytes, "WASM memory did not grow");
    assert.ok(core.HEAP16.buffer === core.HEAPU8.buffer, "heap views are stale after growth");
    core._dealloc(growthProbe);
    console.log(`memory growth: ${initialHeapBytes} -> ${core.HEAPU8.byteLength} bytes; views ok`);

    const pakBytes = readFileSync(pakPath);
    assert.ok(pakBytes.byteLength >= pakHeaderLength, "PAK header is truncated");
    const header = pakBytes.subarray(0, pakHeaderLength);
    const headerPtr = copyToHeap(core, header);
    const entryCount = core._parse_pak(headerPtr, header.byteLength, pakBytes.byteLength);
    core._dealloc(headerPtr);
    assert.ok(entryCount > 0, "parse_pak rejected the PAK");

    const firstNames = [];
    for (let i = 0; i < entryCount; ++i) {
        const name = readName(core, core._entry_name(i));
        const offset = core._entry_offset(i);
        const length = core._entry_length(i);
        assert.ok(name.length > 0, `entry ${i} has no name`);
        assert.ok(offset >= pakHeaderLength, `entry ${i} points into the header`);
        assert.ok(length > 0 && offset + length <= pakBytes.byteLength, `entry ${i} is out of range`);
        if (i < 3) firstNames.push(name);
    }
    const shortHeaderPtr = copyToHeap(core, new Uint8Array(4));
    assert.equal(core._parse_pak(shortHeaderPtr, 4, pakBytes.byteLength), 0);
    core._dealloc(shortHeaderPtr);
    console.log(`pak: ${entryCount} entries; ${JSON.stringify(firstNames)}; sha256=${sha256(pakBytes)}`);

    const oggBytes = readFileSync(oggPath);
    const ogg = decodeAudio(core, oggBytes);
    if (expectedOggFrames !== undefined) {
        assert.equal(ogg.frames, expectedOggFrames, "OGG decoded frame count changed");
    }
    console.log(`ogg: frames=${ogg.frames}; hash=${ogg.hash}; input=${sha256(oggBytes)}`);

    if (wavPath) {
        const wavBytes = readFileSync(wavPath);
        const wav = decodeAudio(core, wavBytes);
        if (expectedWavFrames !== undefined) {
            assert.equal(wav.frames, expectedWavFrames, "WAV decoded frame count changed");
        }
        console.log(`wav: frames=${wav.frames}; hash=${wav.hash}; input=${sha256(wavBytes)}`);
    }

    const invalidAudioPtr = copyToHeap(core, new TextEncoder().encode("OggSnot-an-ogg"));
    assert.equal(core._decode(invalidAudioPtr, 14), 0, "invalid OGG was accepted");
    assert.equal(core._samples(), 0, "invalid decode left samples behind");
    core._dealloc(invalidAudioPtr);

    const pcmPtr = core._alloc(1024 * 2 * Int16Array.BYTES_PER_ELEMENT);
    assert.notEqual(pcmPtr, 0, "spectrum input allocation failed");
    const input = new Int16Array(core.HEAPU8.buffer, pcmPtr, 1024 * 2);
    for (let frame = 0; frame < 1024; ++frame) {
        const left = Math.round(18000 * Math.sin(2 * Math.PI * 440 * frame / 44100));
        const right = Math.round(14000 * Math.sin(2 * Math.PI * 660 * frame / 44100));
        input[frame * 2] = left;
        input[frame * 2 + 1] = right;
    }

    core._sm_spectrum_reset();
    const spectrumPtr = core._sm_spectrum(pcmPtr);
    assert.notEqual(spectrumPtr, 0, "sm_spectrum returned null");
    const spectrum = new Float32Array(core.HEAPU8.buffer, spectrumPtr, spectrumFloats);
    assert.ok(Array.from(spectrum).every(Number.isFinite), "spectrum contains non-finite values");
    assert.ok(spectrum.some((value) => value !== 0), "spectrum is all zeroes");
    core._sm_spectrum_reset();
    const reset = new Float32Array(core.HEAPU8.buffer, spectrumPtr, spectrumFloats);
    assert.ok(reset.every((value) => value === 0), "sm_spectrum_reset did not clear state");
    core._dealloc(pcmPtr);
    console.log(`spectrum: ${spectrumFloats} floats; finite/nonzero/reset ok`);

    const worker = await runWorkerTest();
    assert.equal(worker.ok, true);
    console.log(`node worker: ok; heap=${worker.heapBytes} bytes`);
}

if (workerData?.mode === "worker-smoke") {
    await runWorkerSmoke();
} else {
    await runMain();
}
