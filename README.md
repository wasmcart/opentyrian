# OpenTyrian — wasmcart port

[![build](https://img.shields.io/github/actions/workflow/status/wasmcart/opentyrian/build.yml?label=build)](https://github.com/wasmcart/opentyrian/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/wasmcart/opentyrian)](https://github.com/wasmcart/opentyrian/releases/latest)
[![license](https://img.shields.io/badge/license-GPL--2.0-blue)](COPYING)
[![wasmcart](https://img.shields.io/badge/wasmcart-.wasc%20cart-8a2be2)](https://wasmcart.org)

[OpenTyrian](https://github.com/opentyrian/opentyrian) — the open-source port of
the 1995 DOS shooter **Tyrian** — built as a [wasmcart](https://wasmcart.org)
cart: one `.wasc` file that runs on any conforming host, whether that is a
browser, Node, the native player, or a libretro core in RetroArch.

**[wasmcart.org](https://wasmcart.org)** · [the spec and reference
host](https://github.com/wasmcart/wasmcart) ·
[download the cart](https://github.com/wasmcart/opentyrian/releases/latest)

![Tyrian in play: the player ship firing over cave terrain, with an enemy craft above and the weapon HUD at right](docs/screenshot-gameplay.png)

![Tyrian over an ice field, showing enemy fire and the shield and armor gauges](docs/screenshot-terrain.png)

Captured from the cart itself, running through a wasmcart host.

## It ships complete

Tyrian's data files were **released as freeware** by the publisher, so this cart
is playable the moment you download it. Most classic-game ports cannot say that —
they need you to supply original game files. `pack.sh` fetches the data from
[camanis.net](https://camanis.net/tyrian/tyrian21.zip) at build time rather than
committing it.

## Play

Grab the `.wasc` from
[Releases](https://github.com/wasmcart/opentyrian/releases/latest) and run it with
any wasmcart host:

```sh
npx wasmcart opentyrian.wasc
```

### Controls

Tyrian is an arrow-keys-and-fire game, so it maps onto a gamepad cleanly — that
is why it was chosen for porting over a cursor-driven game.

| Pad | Action |
|---|---|
| d-pad / left stick | move the ship |
| **A** | fire |
| **B** | rear weapon mode |
| **X** / **Y** | left / right sidekick |
| **START** | confirm |
| **SELECT** | pause / back |

## Build

Needs the [Emscripten SDK](https://emscripten.org) and the
[wasmcart-sdl2](https://github.com/wasmcart/wasmcart-sdl2) backend checked out
alongside this repo.

```sh
# once: build SDL2 with the wasmcart backends
EMSDK=/path/to/emsdk ../wasmcart-sdl2/sdl2_wc/build_sdl2_wc.sh

bash build.sh    # -> build/opentyrian.wasm
bash pack.sh     # -> build/opentyrian.wasc   (fetches the game data)
```

`libSDL2_wc.a` is not committed anywhere: it is toolchain-specific, built against
a particular emsdk's SDL port, so a mismatched one produces silent ABI problems.

## How the port works

Four small edits to the game plus a handful of new files. Not a rewrite — the
game logic, assets and feel are the original's.

**One frame boundary.** The game draws a 320×200 8-bit paletted surface and
pushes it to the screen through exactly one function, `JE_showVGA()`. All 79 draw
sites funnel through there, so `src/video.c` intercepts that single function:
convert the paletted surface into the cart framebuffer, then yield.

**Loop inversion instead of restructuring.** The game owns its loop — `main()`
calls `titleScreen()` and `JE_main()`, which spin internally, and there are 86
`delayUntilElapsed()` sites scattered through the code. Turning that into a
per-frame step function would mean rewriting the game. Instead the cart uses
wasmcart's asyncify loop inversion: the game runs until it flips a frame,
`wc_frame_yield()` unwinds the whole stack out to the host, and the next
`wc_render()` rewinds back to exactly that point. Nothing in the game notices.

**Files come from the cart, not a filesystem.** `src/file.c` shims `dir_fopen()`
onto `wc_load_asset()` + `fmemopen()`. All three `dir_fopen_*` variants and
`dir_file_exists()` funnel through it, so the roughly 300 stdio calls in the rest
of the game are untouched and keep working on a normal `FILE*`.

**Input drives the game's own key array.** `keysactive[]` is a plain scancode
array, so the pad writes into it directly. That is less code than standing up a
fake SDL joystick, and it means menus, gameplay and the ship editor all work
without touching any per-screen input handling.

**Audio needed no work.** The game opens a normal SDL audio device with a pull
callback, and the wasmcart SDL2 backend already bridges that to the host's ring
buffer.

### Two traps, recorded so the next port skips them

- **`sdl2_gl_blit.c` is deliberately not linked.** It imports 25 GL functions,
  and a cart that imports anything from the `gl` module is treated as a GL cart
  by the host — which then reads frames back from a GL context this cart never
  draws into, and every frame arrives blank. This game is pure software
  rendering, so `src/gl4es_stub.c` satisfies the linker instead and the cart
  imports zero GL symbols.
- **`-sUSE_SDL=0` is link-only.** Passing it while compiling swaps in
  Emscripten's `fakesdl` headers and every SDL type goes undeclared, so compile
  and link have to be separate steps.

## Known gaps

- **Saves are not persisted.** The game's writes to `tyrian.cfg` and `tyrian.sav`
  return `NULL`; it warns and carries on with defaults. Wiring them to the cart
  save region (`wc_info_t.save_ptr`) is follow-up work.
- **Network multiplayer is not built.** Upstream uses `SDL2_net`, which the
  wasmcart SDL2 backend does not provide. wasmcart has its own transport-agnostic
  peer ABI (`wc_peer_*`) that would suit it better.

## Building the original (non-wasmcart)

Upstream's build is untouched and still works — see `README` and the
[OpenTyrian repo](https://github.com/opentyrian/opentyrian).

## Licensing

OpenTyrian is GPL-2.0 (`COPYING`), unchanged. The Tyrian 2.1 data files are
freeware, redistributed by permission of the original publisher; they are not
committed here and are fetched at build time.
