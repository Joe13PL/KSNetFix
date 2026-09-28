#!/bin/bash
# Builds KSNetFix and packs the release: dist/KSNetFix-<version>.zip
# (dinput8.dll, steam_api.dll, ksnetfix.ini, INSTRUKCJA.txt, ZAINSTALUJ.bat, ODINSTALUJ.bat).
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
VER="$(sed -n 's/^#define KSNETFIX_VERSION "\(.*\)".*/\1/p' "$ROOT/src/ksnetfix.cpp" | tr -d '\r')"
"$ROOT/build.sh"
NAME="KSNetFix-$VER"
rm -rf "$ROOT/dist/$NAME" "$ROOT/dist/$NAME.zip"
mkdir -p "$ROOT/dist/$NAME"
cp "$ROOT/build/dinput8.dll" "$ROOT/build/steam_api.dll" "$ROOT/ksnetfix.ini" "$ROOT/package/"* "$ROOT/dist/$NAME/"
cd "$ROOT/dist"
/c/Windows/System32/tar.exe -a -c -f "$NAME.zip" "$NAME"
echo "packed: dist/$NAME.zip"
