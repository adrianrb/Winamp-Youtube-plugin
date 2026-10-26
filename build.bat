@echo off
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo Abra o "x86 Native Tools Command Prompt for VS" antes de executar este script.
    echo O Winamp 5 e 32 bits, portanto a DLL precisa ser compilada como x86.
    exit /b 1
)

if /i "%Platform%"=="x64" (
    echo O ambiente atual e x64. Use o "x86 Native Tools Command Prompt for VS".
    echo O Winamp 5 e 32 bits e nao carrega uma DLL x64.
    exit /b 1
)

if not exist build mkdir build

rc /nologo /c 65001 /fo build\gen_youtube.res src\gen_youtube.rc
if errorlevel 1 exit /b 1

cl /nologo /LD /O2 /MT /std:c++17 /EHsc /utf-8 ^
    /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Ithird_party\winamp /Isrc ^
    src\gen_youtube.cpp build\gen_youtube.res ^
    /Fe:build\gen_youtube.dll ^
    /link user32.lib gdi32.lib shell32.lib comdlg32.lib ws2_32.lib
if errorlevel 1 exit /b 1

echo.
echo DLL gerada em build\gen_youtube.dll
