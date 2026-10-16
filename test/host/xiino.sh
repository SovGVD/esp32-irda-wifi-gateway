#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags="-std=c++17 -Wall -Wextra -Wno-unused-function -O1 -g -fsanitize=address,undefined"
for source in "$root"/third_party/libwebp/src/dec/*.c "$root"/third_party/libwebp/src/dsp/*.c "$root"/third_party/libwebp/src/utils/*.c; do
    object=$(basename "$(dirname "$source")")_$(basename "$source" .c)
    cc -std=c99 -O1 -g -fsanitize=address,undefined -DHAVE_CONFIG_H \
        -I"$root/include" -I"$root/third_party/libwebp" -c "$source" -o "$out/$object.o"
done
cc -O1 -g -fsanitize=address,undefined -DPNGLE_NO_GAMMA_CORRECTION -I"$root/include" \
    -c "$root/third_party/pngle/pngle.c" -o "$out/pngle.o"
cc -O1 -g -fsanitize=address,undefined -DMINIZ_USE_UNALIGNED_LOADS_AND_STORES=0 -c "$root/third_party/pngle/miniz.c" -o "$out/miniz.o"
ar rcs "$out/codecs.a" "$out"/*.o
g++ $flags -I"$root/include" -c "$root/src/xiino.cpp" -o "$out/xiino.o"
g++ $flags -I"$root/include" -c "$root/src/xiinoimage.cpp" -o "$out/xiinoimage.o"
g++ $flags -I"$root/include" -c "$root/src/xiinosource.cpp" -o "$out/xiinosource.o"
g++ $flags -I"$root/include" "$here/xiino_test.cpp" "$out/xiino.o" "$out/xiinoimage.o" "$out/xiinosource.o" "$out/codecs.a" -lm -o "$out/xiino_test"
"$out/xiino_test"
g++ $flags -DIRXIINO_TEST_CLOCK -I"$here/esp_http_stubs" -I"$root/include" "$here/xiino_transport_test.cpp" \
    "$root/src/irxiino.cpp" "$out/xiino.o" "$out/xiinoimage.o" "$out/xiinosource.o" "$out/codecs.a" -lm -o "$out/xiino_transport_test"
"$out/xiino_transport_test" "$@"
