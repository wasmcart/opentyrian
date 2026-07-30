/*
 * wasmcart_config.h — build-time settings for the cart target.
 *
 * TYRIAN_DIR is where data_dir() looks for the game files. In a cart the data is
 * packed into the .wasc and mounted at the root, so "." is correct.
 *
 * Defined here rather than with -D on the command line: passing a quoted string
 * macro through the shell into emcc mangles the quotes in several entertaining
 * ways, and a header is unambiguous.
 */
#ifndef WASMCART_CONFIG_H
#define WASMCART_CONFIG_H

#ifdef WASM_CART
#  undef TYRIAN_DIR
#  define TYRIAN_DIR "."
#endif

#endif
