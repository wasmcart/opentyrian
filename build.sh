#!/bin/bash
# Build OpenTyrian as a wasmcart cart (.wasm).
#
# Requires the Emscripten SDK and a built libSDL2_wc.a from wasmcart-sdl2:
#   EMSDK=/path/to/emsdk ../wasmcart-sdl2/sdl2_wc/build_sdl2_wc.sh
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
OUT="$HERE/build"
OBJ="$OUT/obj"
mkdir -p "$OBJ"

WASMCART_SDL2="${WASMCART_SDL2:-$HERE/../wasmcart-sdl2}"
SDL2_LIB="$WASMCART_SDL2/sdl2_wc/lib/libSDL2_wc.a"

if [ ! -f "$SDL2_LIB" ]; then
    echo "ERROR: $SDL2_LIB not found." >&2
    echo "Build it first:  EMSDK=/path/to/emsdk $WASMCART_SDL2/sdl2_wc/build_sdl2_wc.sh" >&2
    exit 1
fi

# Compile and link are SEPARATE steps on purpose. -sUSE_SDL=0 is LINK-ONLY:
# passing it while compiling swaps in Emscripten's `fakesdl` headers and every
# SDL type goes undeclared. Compiling needs -sUSE_SDL=2 for the real headers,
# linking needs =0 so it resolves against libSDL2_wc.a instead of Emscripten's
# own SDL. See wasmcart-sdl2's PORTING_GUIDE, Common Problems.
# TYRIAN_DIR is where the game looks for its data files. In a cart the data is
# packed into the .wasc and mounted at the root, so "." is correct.
CFLAGS="-O2 -DWASM_CART -DTARGET_UNIX -DWITH_NETWORK -include $HERE/porting/wasmcart_config.h -sUSE_SDL=2 \
  -I $SRC -I $HERE/porting -I $WASMCART_SDL2/sdl2_wc \
  -Wno-macro-redefined -Wno-unused-command-line-argument \
  -s ASYNCIFY=1 -s ASYNCIFY_IMPORTS=wc_frame_yield"

echo "Building OpenTyrian wasmcart cart..."

OBJS=""
for c in "$SRC"/*.c; do
    o="$OBJ/$(basename "${c%.c}").o"
    emcc $CFLAGS -c "$c" -o "$o"
    OBJS="$OBJS $o"
done

# sdl2_gl_blit.c is deliberately NOT linked: it imports 25 GL functions, and a
# cart that imports anything from the `gl` module is treated as a GL cart by the
# host, which then reads back an empty GL context and shows blank frames. This
# game is pure software rendering, so src/gl4es_stub.c satisfies the linker
# instead.

# Asyncify is what makes this port possible: the game owns its loop and yields
# from JE_showVGA(). ASYNCIFY_IMPORTS stays narrow so only the call paths that
# can actually reach a yield get instrumented.
emcc -O2 \
  $OBJS "$SDL2_LIB" \
  -sUSE_SDL=0 \
  -s ASYNCIFY=1 -s ASYNCIFY_IMPORTS=wc_frame_yield \
  -s STANDALONE_WASM=1 --no-entry \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
  -s INITIAL_MEMORY=64MB -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS='["_wc_get_info","_wc_init","_wc_render","_wc_yield_buffer","_wc_on_suspend","_wc_peer_on_message","_wc_peer_on_open","_wc_peer_on_close","_wc_peer_on_error","_malloc","_free"]' \
  -o "$OUT/opentyrian.wasm"

echo "Built: $OUT/opentyrian.wasm"
ls -la "$OUT/opentyrian.wasm"
