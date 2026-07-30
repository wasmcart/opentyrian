#!/bin/bash
# Pack opentyrian.wasc from the compiled .wasm plus the Tyrian 2.1 data.
#
# The data files were released as FREEWARE by the publisher, so unlike most
# classic ports this cart can ship complete and playable -- no "bring your own
# game files" step. They are fetched rather than committed: this is a source
# repo, and the upstream URL is the canonical copy.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build"
ASSETS="$OUT/assets"
WASM="$OUT/opentyrian.wasm"

DATA_URL="https://camanis.net/tyrian/tyrian21.zip"
DATA_ZIP="$OUT/tyrian21.zip"

if [ ! -f "$WASM" ]; then
    echo "ERROR: $WASM not found. Run build.sh first." >&2
    exit 1
fi

if [ ! -f "$ASSETS/tyrian1.lvl" ]; then
    echo "Fetching Tyrian 2.1 freeware data..."
    mkdir -p "$ASSETS"
    [ -f "$DATA_ZIP" ] || curl -sL -o "$DATA_ZIP" "$DATA_URL"
    unzip -oq "$DATA_ZIP" -d "$ASSETS"
    # The zip nests everything under tyrian21/, but data_dir() looks for the
    # files at the root of the asset archive, so flatten it.
    if [ -d "$ASSETS/tyrian21" ]; then
        mv "$ASSETS/tyrian21"/* "$ASSETS/"
        rmdir "$ASSETS/tyrian21"
    fi
fi

if [ ! -f "$ASSETS/tyrian1.lvl" ]; then
    echo "ERROR: game data missing after fetch -- expected $ASSETS/tyrian1.lvl" >&2
    exit 1
fi

echo "Packing cart..."
npx --yes --package=wasmcart wasmcart-pack \
    --wasm "$WASM" \
    --assets "$ASSETS" \
    --name "OpenTyrian" \
    -o "$OUT/opentyrian.wasc"

ls -la "$OUT/opentyrian.wasc"
