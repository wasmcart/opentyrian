#include <stdio.h>
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

/* Sentinel returned by get_user_directory() in a cart build. dir_fopen() checks
 * for it to route config and progress to the wasmcart save region instead of the
 * read-only asset archive. Not a real path -- nothing ever opens it. */
#  define WASMCART_SAVE_DIR "\x01wc-save"

/* Route every fclose() through the save layer.
 *
 * A write handle from wc_savefs_fopen() has to be closed by wc_savefs_fclose(),
 * which is what folds the buffered bytes back into the save region -- a plain
 * fclose() would drop them silently. The game has 30 fclose() sites and no
 * central place to hook, so redirect the call instead of editing each one.
 * wc_savefs_fclose() falls through to the real fclose() for handles it does not
 * own, so asset reads are unaffected.
 *
 * Declared here rather than including the header, because this file is
 * force-included before everything and wc_sdl_savefs.h needs <stdio.h> first.
 *
 * wc_sdl_savefs.h #undef's this macro at its top, so the layer's own fclose()
 * calls reach real stdio instead of recursing back into themselves. That
 * recursion is a silent hang at cart load, and it cost an hour to find. */
int wasmcart_fclose(FILE *fp);
#  define fclose(fp) wasmcart_fclose(fp)

/* The save paths bracket every write with mkdir() and fsync(fileno(f)), which is
 * the right thing to do against a real filesystem and meaningless here.
 *
 * fsync() is the one that actually bites: a save handle is an fmemopen() stream,
 * which has no underlying descriptor, so fileno() returns -1 and fsync(-1) does
 * not come back. Durability is the host's job anyway -- it persists the whole
 * save region after the frame -- so both calls become no-ops.
 *
 * The success value matters for mkdir: the game only warns on failure, but
 * returning 0 keeps the log clean.
 *
 * The real headers come first on purpose. Defining these macros before
 * <sys/stat.h> and <unistd.h> are parsed rewrites their own declarations of the
 * functions into nonsense and the build dies inside libc. */
#  include <sys/stat.h>
#  include <unistd.h>
#  define mkdir(...) (0)
#  define fsync(fd)  (0)

/* Yield on SDL_Delay() as well as on a frame flip.
 *
 * JE_showVGA() is the frame boundary, but it is not the only place the game gives
 * up control. keyboard.c has five `while (true) { poll; if (input) return;
 * SDL_Delay(); }` loops, and the intro logos and every menu prompt sit in one.
 * None of them draw, so none of them reach JE_showVGA() -- the cart would spin
 * inside the wait forever while the host waited for it to return, unable to
 * deliver the very button press the loop is waiting for. A real deadlock, and it
 * presents as "the cart hangs a few seconds in", which looks like a crash.
 *
 * SDL_Delay is exactly the right hook: its whole meaning is "I have nothing to do
 * right now". Yielding there hands the frame back, the host writes fresh pad
 * state, and the loop's next poll sees it.
 *
 * The framebuffer is not repainted for these yields -- the host simply shows the
 * last frame again, which is correct, because a game sitting in a wait loop has
 * nothing new to draw. */
void wasmcart_delay_yield(unsigned int ms);
#  define SDL_Delay(ms) wasmcart_delay_yield(ms)
#endif

#endif
