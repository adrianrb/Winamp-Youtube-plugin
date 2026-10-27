#!/usr/bin/env sh
set -eu

NO_BUMP=0
for argument in "$@"; do
    case "$argument" in
        --no-bump)
            NO_BUMP=1
            ;;
        *)
            echo "Opcao desconhecida: $argument" >&2
            echo "Uso: $0 [--no-bump]" >&2
            exit 1
            ;;
    esac
done

if [ ! -f version.txt ]; then
    echo "Arquivo version.txt nao encontrado." >&2
    exit 1
fi

VERSION=$(tr -d '[:space:]' < version.txt)
OLD_IFS=$IFS
IFS=.
set -- $VERSION
IFS=$OLD_IFS
if [ "$#" -ne 3 ]; then
    echo "Versao invalida em version.txt: '$VERSION' (esperado X.Y.Z)." >&2
    exit 1
fi
MAJOR=$1
MINOR=$2
PATCH=$3
for part in "$MAJOR" "$MINOR" "$PATCH"; do
    case "$part" in
        ''|*[!0-9]*)
            echo "Versao invalida em version.txt: '$VERSION' (esperado X.Y.Z)." >&2
            exit 1
            ;;
    esac
    if [ "$part" != "0" ] && [ "${part#0}" != "$part" ]; then
        echo "Versao invalida em version.txt: '$VERSION' (sem zeros a esquerda)." >&2
        exit 1
    fi
done

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

echo "Versao: $VERSION"

"$RC" --codepage=65001 -I src -I third_party/winamp -i src/gen_youtube.rc -o build/gen_youtube.res.o

"$CXX" -std=c++17 -O2 -shared -Wall -Wextra \
    -static -static-libgcc -static-libstdc++ \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -Ithird_party/winamp -Isrc \
    src/gen_youtube.cpp build/gen_youtube.res.o \
    -o build/gen_youtube.dll \
    -luser32 -lgdi32 -lshell32 -lcomdlg32 -lws2_32

echo "DLL gerada em build/gen_youtube.dll (versao $VERSION)"

if [ "$NO_BUMP" -eq 1 ]; then
    echo "Versao mantida: $VERSION (--no-bump)."
    exit 0
fi

NEXT_PATCH=$((PATCH + 1))
printf '%s.%s.%s\n' "$MAJOR" "$MINOR" "$NEXT_PATCH" > version.txt
echo "Proxima versao: $MAJOR.$MINOR.$NEXT_PATCH"
