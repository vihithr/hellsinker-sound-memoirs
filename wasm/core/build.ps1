[CmdletBinding()]
param(
    [string]$EmsdkVersion = "3.1.74"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$CoreSource = Join-Path $RepoRoot "src\core"
$SrcRoot = Join-Path $RepoRoot "src"
$BuildRoot = Join-Path $RepoRoot "obj\wasm-core"
$TempRoot = Join-Path $RepoRoot "obj\wasm-temp"
$EmsdkRoot = Join-Path $RepoRoot "obj\emsdk"
$EmsdkBat = Join-Path $EmsdkRoot "emsdk.bat"
$Emcc = Join-Path $EmsdkRoot "upstream\emscripten\emcc.bat"
$Emxx = Join-Path $EmsdkRoot "upstream\emscripten\em++.bat"
# Emit the ES module as core.js so any static server (including
# `python -m http.server`) serves it with a JavaScript MIME type. `.mjs` is
# served as text/plain by Python's default handler and breaks ES imports.
$OutputMjs = Join-Path $PSScriptRoot "core.js"
$OutputWasm = Join-Path $PSScriptRoot "core.wasm"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command $($Arguments -join ' ')"
    }
}

# The scene painter is shared with the desktop build: src/scene.cpp drives
# src/stage.cpp and src/layout.cpp through the gfx primitives implemented by
# the WASM backend (wasm/core/gfx_wasm.cpp). The former derived copies under
# wasm/render are no longer compiled.
$RequiredSources = @(
    (Join-Path $CoreSource "pak_index.cpp"),
    (Join-Path $CoreSource "decoder.cpp"),
    (Join-Path $CoreSource "spectrum.cpp"),
    (Join-Path $RepoRoot "stb_vorbis.c"),
    (Join-Path $SrcRoot "scene.cpp"),
    (Join-Path $SrcRoot "stage.cpp"),
    (Join-Path $SrcRoot "layout.cpp"),
    (Join-Path $PSScriptRoot "gfx_wasm.cpp"),
    (Join-Path $PSScriptRoot "api.cpp")
)
foreach ($Source in $RequiredSources) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required source is missing: $Source"
    }
}

New-Item -ItemType Directory -Force -Path $BuildRoot, $TempRoot | Out-Null

if (-not (Test-Path -LiteralPath $Emcc -PathType Leaf)) {
    if (Test-Path -LiteralPath $EmsdkRoot) {
        throw "Found an incomplete SDK at $EmsdkRoot. Remove that generated directory and rerun."
    }
    Invoke-Checked "git" @(
        "clone", "--depth", "1", "--branch", $EmsdkVersion,
        "https://github.com/emscripten-core/emsdk.git", $EmsdkRoot
    )
}

$env:EMSDK = $EmsdkRoot
$env:EM_CONFIG = Join-Path $EmsdkRoot ".emscripten"
$env:EM_CACHE = Join-Path $EmsdkRoot "upstream\emscripten\cache"
$env:TEMP = $TempRoot
$env:TMP = $TempRoot

if (-not (Test-Path -LiteralPath $env:EM_CONFIG -PathType Leaf)) {
    Invoke-Checked $EmsdkBat @("install", $EmsdkVersion)
    Invoke-Checked $EmsdkBat @("activate", $EmsdkVersion)
}

$VersionFile = Join-Path $EmsdkRoot "upstream\emscripten\emscripten-version.txt"
$VersionOutput = Get-Content -LiteralPath $VersionFile -Raw
if ($VersionOutput -notmatch [regex]::Escape($EmsdkVersion)) {
    throw "Expected Emscripten $EmsdkVersion, but the local SDK reported:`n$VersionOutput"
}

$CommonFlags = @(
    "-O3",
    "-DNDEBUG",
    "-DSTB_VORBIS_NO_STDIO",
    "-I$CoreSource",
    "-I$SrcRoot"
)

$CppSources = @(
    (Join-Path $CoreSource "pak_index.cpp"),
    (Join-Path $CoreSource "decoder.cpp"),
    (Join-Path $CoreSource "spectrum.cpp"),
    (Join-Path $SrcRoot "scene.cpp"),
    (Join-Path $SrcRoot "stage.cpp"),
    (Join-Path $SrcRoot "layout.cpp"),
    (Join-Path $PSScriptRoot "gfx_wasm.cpp"),
    (Join-Path $PSScriptRoot "api.cpp")
)
$CppObjects = @()
foreach ($Source in $CppSources) {
    $Object = Join-Path $BuildRoot (([IO.Path]::GetFileNameWithoutExtension($Source)) + ".o")
    Invoke-Checked $Emxx ($CommonFlags + @(
        "-std=c++17", "-fno-exceptions", "-fno-rtti", "-c", $Source, "-o", $Object
    ))
    $CppObjects += $Object
}

$StbSource = Join-Path $RepoRoot "stb_vorbis.c"
$StbObject = Join-Path $BuildRoot "stb_vorbis.o"
Invoke-Checked $Emcc ($CommonFlags + @("-c", $StbSource, "-o", $StbObject))

$Exports = @(
    "_alloc",
    "_dealloc",
    "_parse_pak",
    "_entry_name",
    "_entry_offset",
    "_entry_length",
    "_decode",
    "_samples",
    "_clear_pcm",
    "_sm_spectrum",
    "_sm_spectrum_reset",
    "_scene_state_size",
    "_scene_begin",
    "_scene_set_state",
    "_scene_advance",
    "_scene_render",
    "_scene_commands",
    "_scene_points",
    "_scene_count"
) -join ","

$LinkFlags = @(
    "--no-entry",
    "-O3",
    "-fno-exceptions",
    "-fno-rtti",
    "-sFILESYSTEM=0",
    "-sMODULARIZE=1",
    "-sEXPORT_ES6=1",
    "-sEXPORT_NAME=createMemoirsCore",
    "-sENVIRONMENT=web,worker,node",
    "-sASSERTIONS=0",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sINITIAL_MEMORY=33554432",
    "-sMAXIMUM_MEMORY=2147483648",
    "-sSTACK_SIZE=1048576",
    "-sINVOKE_RUN=0",
    "-sNO_EXIT_RUNTIME=1",
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP16",
    "-sEXPORTED_FUNCTIONS=$Exports"
)

Remove-Item -LiteralPath $OutputMjs, $OutputWasm -Force -ErrorAction SilentlyContinue
Invoke-Checked $Emxx ($LinkFlags + $CppObjects + @($StbObject, "-o", $OutputMjs))

if (-not (Test-Path -LiteralPath $OutputMjs -PathType Leaf) -or
    -not (Test-Path -LiteralPath $OutputWasm -PathType Leaf)) {
    throw "Build completed without producing core.js and core.wasm."
}

$Mjs = Get-Item -LiteralPath $OutputMjs
$Wasm = Get-Item -LiteralPath $OutputWasm
Write-Host "Emscripten $EmsdkVersion"
Write-Host "core.js   $($Mjs.Length) bytes"
Write-Host "core.wasm $($Wasm.Length) bytes"
