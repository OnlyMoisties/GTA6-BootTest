@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist ..\bin\stubs mkdir ..\bin\stubs
if not exist ..\bin\obj mkdir ..\bin\obj
for %%f in (xmem d3d12_xs XCRDAPI xg_xs) do (
  cl /nologo /O1 /MT /W3 /LD %%f.c /Fo..\bin\obj\%%f.obj /Fe..\bin\stubs\%%f.dll /link /NOIMPLIB /NOEXP user32.lib gdi32.lib >..\bin\obj\%%f.log 2>&1 || (type ..\bin\obj\%%f.log & exit /b 1)
)
for %%f in (chad xapu AcpHal) do (
  cl /nologo /O1 /MT /W3 /LD /DWHICH_%%f audio_stubs.c /Fo..\bin\obj\%%f.obj /Fe..\bin\stubs\%%f.dll /link /NOIMPLIB /NOEXP user32.lib gdi32.lib >..\bin\obj\%%f.log 2>&1 || (type ..\bin\obj\%%f.log & exit /b 1)
)
cl /nologo /O1 /MT /W3 harness.c /Fo..\bin\obj\harness.obj /Fe..\bin\harness.exe >..\bin\obj\harness.log 2>&1 || (type ..\bin\obj\harness.log & exit /b 1)
echo BUILD OK
