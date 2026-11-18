# Sync the browser build into both static targets.
#
# The GitHub Pages site is served from the repository ROOT (root folder
# deployment), and dist-static/ is the portable archive of the same site.
# wasm/ holds the sources, so every deployable file must be copied to BOTH
# targets - missing one is how the player once shipped an old index.html.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'wasm'
$targets = @($root, (Join-Path $root 'dist-static'))

$files = @('index.html', 'app.js', 'worker.js', 'scene.js', 'screen.css')
foreach ($target in $targets) {
    if (-not (Test-Path $target)) { New-Item -ItemType Directory -Path $target | Out-Null }
    foreach ($f in $files) {
        Copy-Item (Join-Path $src $f) (Join-Path $target $f) -Force
    }
    $coreDir = Join-Path $target 'core'
    if (-not (Test-Path $coreDir)) { New-Item -ItemType Directory -Path $coreDir | Out-Null }
    Copy-Item (Join-Path $src 'core/core.js') $coreDir -Force
    Copy-Item (Join-Path $src 'core/core.wasm') $coreDir -Force

    $assetDir = Join-Path $target 'assets'
    if (-not (Test-Path $assetDir)) { New-Item -ItemType Directory -Path $assetDir | Out-Null }
    Copy-Item (Join-Path $src 'assets/*.png') $assetDir -Force
    Copy-Item (Join-Path $src 'assets/tracks.json') $assetDir -Force
    $fontDir = Join-Path $assetDir 'fonts'
    if (-not (Test-Path $fontDir)) { New-Item -ItemType Directory -Path $fontDir | Out-Null }
    Copy-Item (Join-Path $src 'assets/fonts/*.woff2') $fontDir -Force
    New-Item -ItemType File -Path (Join-Path $target '.nojekyll') -Force | Out-Null
    Write-Host ("synced -> {0}" -f (Split-Path -Leaf $target))
}
Write-Host 'Wrote index.html/app.js/worker.js/scene.js/screen.css + core + assets to the repo root and dist-static/'
