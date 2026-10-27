@echo off
setlocal EnableExtensions

set "NO_BUMP=0"
if /i "%~1"=="--no-bump" set "NO_BUMP=1"

if not exist version.txt (
    echo Arquivo version.txt nao encontrado.
    exit /b 1
)

set "VERSION="
for /f "usebackq delims=" %%v in ("version.txt") do if not defined VERSION set "VERSION=%%v"
echo(%VERSION%| findstr /r /c:"^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$" >nul
if errorlevel 1 (
    echo Versao invalida em version.txt: "%VERSION%" ^(esperado X.Y.Z^).
    exit /b 1
)

for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do (
    set "MAJOR=%%a"
    set "MINOR=%%b"
    set "PATCH=%%c"
)
call :CheckComponent "%MAJOR%"
if errorlevel 1 exit /b 1
call :CheckComponent "%MINOR%"
if errorlevel 1 exit /b 1
call :CheckComponent "%PATCH%"
if errorlevel 1 exit /b 1

echo Versao: %VERSION%

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

echo DLL gerada em build\gen_youtube.dll ^(versao %VERSION%^)

if "%NO_BUMP%"=="1" (
    echo Versao mantida: %VERSION% ^(--no-bump^).
    goto :done
)

set /a NEXT_PATCH=PATCH+1
>version.txt echo %MAJOR%.%MINOR%.%NEXT_PATCH%
echo Proxima versao: %MAJOR%.%MINOR%.%NEXT_PATCH%

:done
exit /b 0

:CheckComponent
set "COMPONENT=%~1"
if "%COMPONENT%"=="0" exit /b 0
if "%COMPONENT:~0,1%"=="0" (
    echo Versao invalida: componente com zero a esquerda.
    exit /b 1
)
exit /b 0
