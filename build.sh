#!/bin/bash
# Builds KSNetFix (32-bit dinput8.dll) with Visual Studio 2022 from Git Bash - no vcvars needed.
#   ./build.sh          build/dinput8.dll + build/steam_api.dll
#   ./build.sh tests    build/test/kbtest.exe, steamtest.exe, sdrtest.exe
#
# Needs the Steamworks SDK (headers + redistributable_bin): unpack it to ./sdk or set
# STEAMWORKS_SDK to the directory that contains public/steam/steam_api.h.
# Optional overrides: VCTOOLS (MSVC toolset dir), WINSDK / WINSDKV (Windows 10/11 SDK).
#
# Add-ons (the dedicated server) compile extra sources into the same DLL (lists separated by ';'):
#   KSNF_EXTRA_SRC="a.cpp;b.cpp" KSNF_INCLUDE="dir1;dir2" KSNF_DEFINES="-DKSNETFIX_SERVER" KSNF_OUT=<dir> ./build.sh
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
win() { cygpath -m "$1"; }

SDKROOT="${STEAMWORKS_SDK:-$ROOT/sdk}"
if [ ! -f "$SDKROOT/public/steam/steam_api.h" ]; then
  echo "Steamworks SDK not found in '$SDKROOT' - see README (Budowanie)." >&2
  exit 1
fi
if [ -z "$VCTOOLS" ]; then
  for base in "C:/Program Files (x86)/Microsoft Visual Studio/2022" "C:/Program Files/Microsoft Visual Studio/2022"; do
    for ed in BuildTools Community Professional Enterprise; do
      d="$base/$ed/VC/Tools/MSVC"
      if [ -d "$d" ]; then VCTOOLS="$d/$(ls "$d" | sort -V | tail -1)"; break 2; fi
    done
  done
fi
if [ ! -x "$VCTOOLS/bin/Hostx64/x86/cl.exe" ]; then
  echo "MSVC (Visual Studio 2022, C++ x86) not found - set VCTOOLS." >&2
  exit 1
fi
WINSDK="${WINSDK:-C:/Program Files (x86)/Windows Kits/10}"
WINSDKV="${WINSDKV:-$(ls "$WINSDK/Include" | grep '^10\.' | sort -V | tail -1)}"
STEAMW="$(win "$SDKROOT")"
export INCLUDE="$VCTOOLS/include;$WINSDK/Include/$WINSDKV/ucrt;$WINSDK/Include/$WINSDKV/um;$WINSDK/Include/$WINSDKV/shared;$STEAMW/public"
export LIB="$VCTOOLS/lib/x86;$WINSDK/Lib/$WINSDKV/ucrt/x86;$WINSDK/Lib/$WINSDKV/um/x86;$STEAMW/redistributable_bin"
export MSYS2_ARG_CONV_EXCL='*'
CL="$VCTOOLS/bin/Hostx64/x86/cl.exe"
CFLAGS="-nologo -std:c++17 -O2 -MT -W3 -EHsc -GS- -Zi"
LIBS="kernel32.lib user32.lib winmm.lib ole32.lib ws2_32.lib advapi32.lib steam_api.lib delayimp.lib -DELAYLOAD:steam_api.dll"

cd "$ROOT/src"
if [ "$1" = tests ]; then
  OUT="$ROOT/build/test"
  mkdir -p "$OUT"
  O="$(win "$OUT")"
  "$CL" $CFLAGS -Fo"$O/" -Fd"$O/" test/kbtest.cpp -Fe"$O/kbtest.exe" -link user32.lib
  "$CL" $CFLAGS -Fo"$O/" -Fd"$O/" test/steamtest.cpp steampeer.cpp earthnet_core.cpp -Fe"$O/steamtest.exe" -link $LIBS
  "$CL" $CFLAGS -Fo"$O/" -Fd"$O/" test/sdrtest.cpp -Fe"$O/sdrtest.exe" -link steam_api.lib
  "$CL" $CFLAGS -I. -Fo"$O/" -Fd"$O/" test/steamnet_wine_test.cpp earthnet.cpp earthnet_core.cpp \
    -Fe"$O/steamnet_test.exe" -link ws2_32.lib advapi32.lib user32.lib
  cp "$SDKROOT/redistributable_bin/steam_api.dll" "$OUT/"
  echo "built: $OUT/{kbtest,steamtest,sdrtest,steamnet_test}.exe"
  exit 0
fi

OUT="${KSNF_OUT:-$ROOT/build}"
mkdir -p "$OUT"
O="$(win "$OUT")"
EXTRA=()
IFS=';' read -r -a LIST <<< "${KSNF_EXTRA_SRC:-}"
for f in "${LIST[@]}"; do [ -n "$f" ] && EXTRA+=("$(win "$f")"); done
IFS=';' read -r -a LIST <<< "${KSNF_INCLUDE:-}"
for d in "${LIST[@]}"; do [ -n "$d" ] && INCLUDE="$INCLUDE;$(win "$d")"; done
"$CL" $CFLAGS -LD $KSNF_DEFINES -Fo"$O/" -Fd"$O/" ksnetfix.cpp steampeer.cpp earthnet.cpp earthnet_core.cpp "${EXTRA[@]}" \
  -link -DEF:dinput8.def -OUT:"$O/dinput8.dll" -IMPLIB:"$O/dinput8.lib" -PDB:"$O/dinput8.pdb" -PDBALTPATH:%_PDB% -DEBUG -OPT:REF $LIBS
rm -f "$OUT/dinput8.exp" "$OUT/dinput8.lib"
cp "$SDKROOT/redistributable_bin/steam_api.dll" "$OUT/steam_api.dll"
echo "built: $OUT/dinput8.dll (+ steam_api.dll)"
