#!/bin/sh
# Runs the core's D3D11 backend through lrd3d11test in all three modes and
# requires each to end on the same frame. Run from the repo root after building
# ppsspp_libretro.dll. On Linux it is built with mingw-w64 and run under
# Wine (wined3d, so no GPU is needed); in MSYS2 it runs natively.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
core=${1:-$root/libretro/ppsspp_libretro.dll}
frames=${2:-240}
CC=${CC:-x86_64-w64-mingw32-gcc}

"$CC" -O1 -Wall -I "$root/libretro/libretro-common/include" \
	-o "$here/lrd3d11test.exe" "$here/lrd3d11test.c" \
	-ld3d11 -ld3dcompiler -ldxguid -luuid

data=$(mktemp -d)
sys=$(mktemp -d)
mkdir -p "$sys/PPSSPP"
cp -r "$root/assets/." "$sys/PPSSPP/"
# A dump whose shaders Wine's HLSL compiler accepts.
unzip -o -q "$root/frametests/dumps/Depth/21797 Taiko no Tatsujin NPJH50426_0002.zip" -d "$data"
dump=$(ls "$data"/*.ppdmp | head -n 1)

case "$(uname -s)" in
	MINGW*|MSYS*) run() { "$here/lrd3d11test.exe" "$@"; } ;;
	*)
		WINE=${WINE:-$(command -v wine64 || command -v wine)}
		if [ -z "${DISPLAY:-}" ]; then
			Xvfb :81 -screen 0 1024x768x24 >/dev/null 2>&1 &
			xpid=$!
			trap 'kill $xpid 2>/dev/null || true' EXIT
			DISPLAY=:81
			export DISPLAY
			sleep 2
		fi
		export WINEDEBUG=${WINEDEBUG:--all}
		# The mingw runtime the core may link against.
		WINEPATH=$(dirname "$("$CC" -print-file-name=libwinpthread-1.dll)")
		export WINEPATH
		run() { timeout -k 5 600 "$WINE" "$here/lrd3d11test.exe" "$@"; }
		;;
esac

ref=
for mode in v1 v2 v2-threaded; do
	save=$(mktemp -d)
	line=$(LRTEST_SYSTEM=$sys LRTEST_SAVE=$save run "$core" "$dump" "$frames" "$mode" | tr -d '\r' | tee /dev/stderr | grep '^@@') || true
	rm -rf "$save"
	case "$line" in *errors=0*) ;; *) echo "FAIL: $mode"; exit 1;; esac
	case "$line" in *" lit=0 "*) echo "FAIL: $mode drew nothing"; exit 1;; esac
	digest=${line##*last=}
	[ -z "$ref" ] && ref=$digest
	[ "$digest" = "$ref" ] || { echo "FAIL: $mode ends on another frame than v1"; exit 1; }
done
echo "D3D11 v1, v2 and threaded v2 agree."
