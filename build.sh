#!/usr/bin/env sh
set -eu

CXX="${CXX:-i686-w64-mingw32-g++}"
RC="${RC:-i686-w64-mingw32-windres}"

command -v "$CXX" >/dev/null 2>&1 || {
    echo "Compilador nao encontrado: $CXX" >&2
    echo "Instale mingw-w64 i686 ou defina CXX e RC." >&2
    exit 1
}
command -v "$RC" >/dev/null 2>&1 || {
    echo "Compilador de recursos nao encontrado: $RC" >&2
    exit 1
}

mkdir -p build

"$RC" --codepage=65001 -I src -I third_party/winamp -i src/gen_youtube.rc -o build/gen_youtube.res.o

"$CXX" -std=c++17 -O2 -shared -Wall -Wextra \
    -static -static-libgcc -static-libstdc++ \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -Ithird_party/winamp -Isrc \
    src/gen_youtube.cpp build/gen_youtube.res.o \
    -o build/gen_youtube.dll \
    -luser32 -lgdi32 -lshell32 -lcomdlg32 -lws2_32

echo "DLL gerada em build/gen_youtube.dll"
