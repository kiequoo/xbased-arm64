#!/bin/bash
# The host tests of the PC build, with one command (the MSYS2 CLANG64 shell
# on Windows, or Linux; clang, cmake and ninja as for the build itself):
#
#   tools/native/host-tests.sh [the app's build folder]
#
# - tests/hle: the pure modules of the GPU layer and the core (CMake)
# - tests/native: the kernel's crypto, the upcase table, the GLib subset; and
#   under the address sanitizer the disc reader's directory walk, the
#   kernel's dispatch of processor exceptions, its files and devices called
#   with no title, and the savestate's cache-drive section read as a file
#   from anywhere
# - cputest: the translator (native/cpu/cpu_x86.cpp) against the host
#   processor itself, from the app's build folder (default ../build/win;
#   Windows only for now)
#
# tests/native/apu_out and fexrun are not run: they are Android's.
# Prints a line per test program and ends with the number that failed.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
case "$(uname -s)" in
  MINGW*|MSYS*) PLAT=win; EXE=.exe ;;
  *) PLAT=linux; EXE= ;;
esac
APP=${1:-$REPO/../build/$PLAT}
OUT=$REPO/../build/tests
DEPS=${NC_DEPS:-$REPO/../deps}
mkdir -p "$OUT/native" "$OUT/tmp"
if command -v cygpath >/dev/null 2>&1; then
  DEPS=$(cygpath -m "$DEPS")
  # the compiler's temporary files where a Windows program finds them
  TMP=$(cygpath -m "$OUT/tmp")
  export TMP TEMP="$TMP"
else
  export TMP="$OUT/tmp"                 # the tests' own files (a hard disk folder, a state's store)
fi
failed=0
ran=0

check() { # name, command...
  local name=$1
  shift
  ran=$((ran + 1))
  if "$@" >"$OUT/last.log" 2>&1; then
    echo "ok    $name"
    grep -E '^skip' "$OUT/last.log" | sed 's/^/        /'
  else
    echo "FAIL  $name"
    grep -v '^PASS' "$OUT/last.log" | head -20 | sed 's/^/        /'
    failed=$((failed + 1))
  fi
}

# tests/hle
if cmake -S "$REPO/tests/hle" -B "$OUT/hle" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
     "-DNC_DEPS=$DEPS" >"$OUT/hle-configure.log" 2>&1 && cmake --build "$OUT/hle" >"$OUT/hle-build.log" 2>&1; then
  for t in "$OUT"/hle/test_*$EXE; do
    check "hle/$(basename "$t" $EXE)" "$t"
  done
else
  echo "FAIL  tests/hle doesn't build ($OUT/hle-build.log)"
  grep -E 'error' "$OUT/hle-build.log" | head -10 | sed 's/^/        /'
  failed=$((failed + 1))
fi

# tests/native, each a program of its own
cd "$REPO" || exit 1
build() { # name, sources and flags...
  local name=$1
  shift
  if clang -O1 "$@" -o "$OUT/native/$name$EXE" >"$OUT/native/$name.log" 2>&1; then
    check "native/$name" "$OUT/native/$name$EXE"
  else
    echo "FAIL  native/$name doesn't build"
    head -10 "$OUT/native/$name.log" | sed 's/^/        /'
    failed=$((failed + 1))
  fi
}
build xc_test -I. tests/native/xc/xc_test.c native/kernel/xc_crypto.c
build case_test -I. tests/native/rtl/case_test.c
build glib_test -Inative/compat/glib tests/native/compat/glib_test.c native/compat/glib/glib.c -lpthread
# the disc reader's directory walk on damaged and crafted tables, under the address sanitizer
if [ "$PLAT" = win ]; then
  build xdvdfs_test -g -fsanitize=address -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00     -I. -Inative/compat/win/include -include nc_win_posix.h tests/native/disc/xdvdfs_test.c native/core/xdvdfs.c     native/compat/win/win.c -lws2_32 -liphlpapi -lwinmm -lbcrypt -lsynchronization -lpthread
  # a game folder read as a disc, through the reader
  build fold_test -g -fsanitize=address -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00     -I. -Inative/compat/win/include -include nc_win_posix.h tests/native/disc/fold_test.c native/core/fold_image.c     native/core/xdvdfs.c native/compat/win/win.c -lws2_32 -liphlpapi -lwinmm -lbcrypt -lsynchronization -lpthread
else
  build xdvdfs_test -g -fsanitize=address -D_GNU_SOURCE -I. -Inative/compat/linux/include     tests/native/disc/xdvdfs_test.c native/core/xdvdfs.c
  build fold_test -g -fsanitize=address -D_GNU_SOURCE -I. -Inative/compat/linux/include     tests/native/disc/fold_test.c native/core/fold_image.c native/core/xdvdfs.c -lpthread
fi
# the USB stack: a host driver's enumeration of the hubs and a gamepad, a report and rumble
if [ "$PLAT" = win ]; then
  build usb_test -g -fsanitize=address -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00     -I. -Inative/compat/win/include -include nc_win_posix.h tests/native/usb/usb_test.c native/dev/usb.c     native/dev/usb_hub.c native/dev/xid.c native/state/state.c native/compat/win/win.c -lz -lws2_32 -liphlpapi     -lwinmm -lbcrypt -lsynchronization -lpthread
else
  build usb_test -g -fsanitize=address -D_GNU_SOURCE -I. -Inative/compat/linux/include tests/native/usb/usb_test.c     native/dev/usb.c native/dev/usb_hub.c native/dev/xid.c native/state/state.c -lz -lpthread
fi
# the kernel's dispatch of processor exceptions to a title's handlers
if [ "$PLAT" = win ]; then
  build seh_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -I. -Inative/compat/win/include -include nc_win_posix.h tests/native/kernel/seh_test.c native/kernel/seh.c -lpthread
else
  build seh_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -I. -Inative/compat/linux/include tests/native/kernel/seh_test.c native/kernel/seh.c -lpthread
fi
# the kernel's files and devices with no title (paths, partitions, handles, a state's open files),
# the savestate's cache-drive section read as a file from anywhere, and the mods' manifest and
# their section of a state over a disc and a memory of the test's own
if [ "$PLAT" = win ]; then
  build io_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -I. -Inative/compat/win/include -include nc_win_posix.h tests/native/kernel/io_test.c native/kernel/io.c native/kernel/ob.c native/kernel/pool.c native/core/xdvdfs.c native/state/state.c native/compat/win/win.c -lz -lws2_32 -liphlpapi -lwinmm -lbcrypt -lsynchronization -lpthread
  build cache_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -I. "-I$DEPS/xxhash" -Inative/compat/win/include -include nc_win_posix.h tests/native/state/cache_test.c native/state/cache.c native/state/state.c native/compat/win/win.c -lz -lws2_32 -liphlpapi -lwinmm -lbcrypt -lsynchronization -lpthread
  build mods_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -I. -Inative/compat/glib -Inative/compat/win/include -include nc_win_posix.h tests/native/core/mods_test.c native/core/mods.c native/core/cheats.c hw/xbox/hle/core/xbe.c native/state/state.c native/compat/win/win.c -lz -lws2_32 -liphlpapi -lwinmm -lbcrypt -lsynchronization -lpthread
else
  build io_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -I. -Inative/compat/linux/include tests/native/kernel/io_test.c native/kernel/io.c native/kernel/ob.c native/kernel/pool.c native/core/xdvdfs.c native/state/state.c native/compat/linux/linux.c -lz -lpthread
  build cache_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -I. "-I$DEPS/xxhash" -Inative/compat/linux/include tests/native/state/cache_test.c native/state/cache.c native/state/state.c -lz -lpthread
  build mods_test -g -fsanitize=address -DNC_DESKTOP=1 -D_GNU_SOURCE -I. -Inative/compat/glib -Inative/compat/linux/include tests/native/core/mods_test.c native/core/mods.c native/core/cheats.c hw/xbox/hle/core/xbe.c native/state/state.c native/compat/linux/linux.c -lz -lpthread
fi

# the translator against the processor
if [ "$PLAT" = win ] && [ -f "$APP/build.ninja" ]; then
  if cmake --build "$APP" --target cputest >"$OUT/cputest-build.log" 2>&1; then
    check "cputest" "$APP/cputest$EXE"
    grep -E '^GAP' "$OUT/last.log" | sed 's/^/        /'
    tail -1 "$OUT/last.log" | sed 's/^/        /'
  else
    echo "FAIL  cputest doesn't build ($OUT/cputest-build.log)"
    failed=$((failed + 1))
  fi
else
  echo "skip  cputest (the app's build folder: $APP)"
fi

echo "$ran test programs run, $failed failed"
exit $failed
