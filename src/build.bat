@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist ..\bin\stubs mkdir ..\bin\stubs
if not exist ..\bin\obj mkdir ..\bin\obj
set LIBS=user32.lib gdi32.lib winmm.lib
for %%f in (xmem XCRDAPI xg_xs) do (
  cl /nologo /O1 /MT /W3 /LD %%f.c /Fo..\bin\obj\%%f.obj /Fe..\bin\stubs\%%f.dll /link /NOIMPLIB /NOEXP %LIBS% >..\bin\obj\%%f.log 2>&1 || (type ..\bin\obj\%%f.log & exit /b 1)
)
rem d3d12_xs = stand-in + real PC D3D12 backend (separate file: the D3D12 headers clash with the stand-in exports)
cl /nologo /O1 /MT /W3 /LD d3d12_xs.c realgpu.c /Fo..\bin\obj\ /Fe..\bin\stubs\d3d12_xs.dll /link /NOIMPLIB /NOEXP %LIBS% d3d12.lib dxgi.lib dxguid.lib >..\bin\obj\d3d12_xs.log 2>&1 || (type ..\bin\obj\d3d12_xs.log & exit /b 1)
for %%f in (chad xapu AcpHal) do (
  cl /nologo /O1 /MT /W3 /LD /DWHICH_%%f audio_stubs.c /Fo..\bin\obj\%%f.obj /Fe..\bin\stubs\%%f.dll /link /NOIMPLIB /NOEXP %LIBS% >..\bin\obj\%%f.log 2>&1 || (type ..\bin\obj\%%f.log & exit /b 1)
)
cl /nologo /O1 /MT /W3 harness.c /Fo..\bin\obj\harness.obj /Fe..\bin\harness.exe >..\bin\obj\harness.log 2>&1 || (type ..\bin\obj\harness.log & exit /b 1)
echo BUILD OK
