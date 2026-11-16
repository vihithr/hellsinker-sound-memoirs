@echo off
setlocal
cd /d "%~dp0"
if not defined VSCMD_VER (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "HSBGM_VS=%%i"
)
if defined HSBGM_VS call "%HSBGM_VS%\VC\Auxiliary\Build\vcvars32.bat"
where cl >nul 2>nul || (echo Visual C++ and Windows SDK are required. & exit /b 1)
if not exist obj mkdir obj
if not exist dist mkdir dist
if not defined HSBGM_BUILD_EXE set "HSBGM_BUILD_EXE=dist\hsbgm.exe"
rc /nologo /fo obj\memoirs.res src\memoirs.rc || exit /b 1
cl /nologo /O2 /MT /w /c stb_vorbis.c /Foobj\stb_vorbis.obj || exit /b 1
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Foobj\ src\common.cpp src\pak.cpp src\resolver.cpp src\decoder.cpp src\local_music.cpp src\audio.cpp src\ui.cpp src\main.cpp obj\stb_vorbis.obj obj\memoirs.res /Fe"%HSBGM_BUILD_EXE%" /link winmm.lib gdi32.lib user32.lib comdlg32.lib shell32.lib ole32.lib msimg32.lib mfplat.lib mfreadwrite.lib mfuuid.lib || exit /b 1
echo Built %HSBGM_BUILD_EXE%
