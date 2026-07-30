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

# WASMCART_WS grants the cart WebSocket access to a relay domain, which is what
# makes wc_peer_open() succeed for network play. It is NOT set for the shipped
# cart on purpose: which relay to trust is the host's or player's call, not
# something a game should bake in. Pass it to build a cart for a relay you run:
#
#   WASMCART_WS=127.0.0.1:8787 bash pack.sh
#
# Without it the cart still declares WC_FLAG_NET_PEER, so a host that supplies
# peers itself (addPeer) can still do multiplayer -- host-supplied peers need no
# grant. See docs/networking.md in the wasmcart repo.
WS_ARGS=()
if [ -n "${WASMCART_WS:-}" ]; then
    WS_ARGS=(--ws "$WASMCART_WS")
    echo "Granting WebSocket access to $WASMCART_WS"
fi

echo "Packing cart..."
npx --yes --package=wasmcart wasmcart-pack \
    --wasm "$WASM" \
    --assets "$ASSETS" \
    --name "OpenTyrian" \
    "${WS_ARGS[@]}" \
    -o "$OUT/opentyrian.wasc"

ls -la "$OUT/opentyrian.wasc"
