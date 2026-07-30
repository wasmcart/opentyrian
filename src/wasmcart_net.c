/*
 * The one translation unit that compiles the SDL_net-over-wc_peer shim.
 *
 * Kept in its own file rather than folded into network.c so that upstream's
 * netcode stays byte-identical -- the whole point of the shim is that the game's
 * network code does not change.
 */
#ifdef WASM_CART
#ifdef WITH_NETWORK

#define WC_SDL_NET_IMPLEMENTATION
#include "wc_sdl_net.h"

#endif
#endif
