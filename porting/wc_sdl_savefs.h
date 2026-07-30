/*
 * wc_sdl_savefs.h — a tiny read/write file layer over the wasmcart save region.
 *
 * Why this exists
 * ---------------
 * A ported game almost always writes its config and progress with stdio:
 * fopen("game.cfg", "wb"), fwrite, fclose. Carts have no filesystem, so those
 * calls fail and the port silently loses saves. Read-only assets are already
 * handled -- wc_load_asset() plus fmemopen() covers loading -- but writes need
 * somewhere to go.
 *
 * wasmcart's answer is the save region: the cart declares a byte blob via
 * wc_info_t.save_ptr / save_size, the host loads it before wc_init() and
 * persists it afterwards, and the cart never learns where it went (a file,
 * browser storage, libretro SRAM). See SPEC.md, "Saving is host-managed".
 *
 * This header turns that blob into a handful of named files so a port can keep
 * using fopen/fread/fwrite. It lives here rather than in each port because every
 * SDL game that saves anything needs the same thing.
 *
 * Layout in the save region
 * -------------------------
 *   magic "WCSF"           4 bytes
 *   count                  1 byte
 *   per entry:  name[24]   NUL-padded
 *               size       4 bytes, little-endian
 *   ...then the file bodies, in entry order.
 *
 * Deliberately simple: a fixed small table, no free list, no fragmentation. A
 * game saving two config files and a progress file does not need a filesystem,
 * and a real one here would be more code than the games it serves.
 *
 * Usage
 * -----
 *   static uint8_t save_blob[WC_SAVEFS_BYTES];   // declare in your cart
 *   info.save_ptr  = (uint32_t)(uintptr_t)save_blob;
 *   info.save_size = sizeof(save_blob);
 *
 *   wc_savefs_init(save_blob, sizeof(save_blob));   // in wc_init()
 *
 *   FILE *f = wc_savefs_fopen("game.cfg", "wb");    // in your fopen shim
 *   ...fwrite...
 *   fclose(f);                                      // commits to the region
 *
 * The host persists the region on its own schedule, so a cart does not need to
 * ask. Closing a write handle only updates the blob.
 *
 * ONE translation unit, or define WC_SAVEFS_IMPLEMENTATION in exactly one
 * -----------------------------------------------------------------------
 * The state below used to be plain `static`, which meant a cart that included
 * this header from two .c files silently got TWO independent save filesystems.
 * The symptom is vicious: whichever file calls wc_savefs_init() initialises its
 * own copy, the other one is left with cap == 0, and its writes go to a
 * zero-capacity fmemopen() stream -- so fopen SUCCEEDS, fwrite short-writes, and
 * the game reports "failed to write" while every save silently vanishes.
 *
 * So the state is now extern, declared here and defined only where
 * WC_SAVEFS_IMPLEMENTATION is set. Put that define in exactly one .c file:
 *
 *   #define WC_SAVEFS_IMPLEMENTATION
 *   #include "wc_sdl_savefs.h"
 *
 * Other files include the header normally and share that one filesystem. A cart
 * that only includes it once needs the define in that one file.
 */

#ifndef WC_SDL_SAVEFS_H
#define WC_SDL_SAVEFS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A port that routes the game's fclose() to wc_savefs_fclose() with a macro --
 * the usual way to hook 30-odd call sites at once -- would otherwise make THIS
 * file's own fclose() calls recurse into wc_savefs_fclose() forever. It is a
 * silent hang, not a crash, and it happens at cart load rather than at save time,
 * so it looks nothing like a save bug.
 *
 * Undefining here means the layer always reaches the real stdio, whatever the
 * including file has done to the name.
 */
#undef fclose
#undef fopen

#ifndef WC_SAVEFS_MAX_FILES
#define WC_SAVEFS_MAX_FILES 8
#endif

#define WC_SAVEFS_NAME_MAX 24
#define WC_SAVEFS_HEADER   (4 + 1 + WC_SAVEFS_MAX_FILES * (WC_SAVEFS_NAME_MAX + 4))

/* A reasonable default region size: header plus 60KB of payload. Override before
 * including if your game saves more. */
#ifndef WC_SAVEFS_BYTES
#define WC_SAVEFS_BYTES (WC_SAVEFS_HEADER + 60 * 1024)
#endif

/* Declarations. Always visible; the bodies below compile in the one file that
 * defines WC_SAVEFS_IMPLEMENTATION. */
void  wc_savefs_init(void *region, uint32_t bytes);
FILE *wc_savefs_fopen(const char *name, const char *mode);
int   wc_savefs_fclose(FILE *fp);
int   wc_savefs_exists(const char *name);

#ifdef WC_SAVEFS_IMPLEMENTATION

/* ── internals ─────────────────────────────────────────────────────────── */

typedef struct {
	uint8_t *blob;
	uint32_t cap;
	uint8_t  count;
	char     name[WC_SAVEFS_MAX_FILES][WC_SAVEFS_NAME_MAX];
	uint32_t size[WC_SAVEFS_MAX_FILES];
	uint32_t off[WC_SAVEFS_MAX_FILES];   /* into blob */
} wc_savefs_t;

static wc_savefs_t wc_savefs;

/* An open write handle: fmemopen cannot grow, so writes land in a scratch buffer
 * and are folded back into the region when the handle closes. */
typedef struct {
	char     name[WC_SAVEFS_NAME_MAX];
	uint8_t *buf;
	size_t   cap;
	FILE    *fp;
} wc_savefs_writer_t;

static wc_savefs_writer_t wc_savefs_writers[WC_SAVEFS_MAX_FILES];

static void wc_savefs_reindex(void)
{
	uint32_t off = WC_SAVEFS_HEADER;
	for (uint8_t i = 0; i < wc_savefs.count; i++) {
		wc_savefs.off[i] = off;
		off += wc_savefs.size[i];
	}
}

/* Parse the region. A blob the host has never written is all zeroes, so the
 * magic check doubles as "first run". */
void wc_savefs_init(void *region, uint32_t bytes)
{
	memset(&wc_savefs, 0, sizeof wc_savefs);
	wc_savefs.blob = (uint8_t *)region;
	wc_savefs.cap = bytes;

	if (bytes < WC_SAVEFS_HEADER)
		return;

	if (memcmp(wc_savefs.blob, "WCSF", 4) != 0) {
		memcpy(wc_savefs.blob, "WCSF", 4);
		wc_savefs.blob[4] = 0;
		return;
	}

	uint8_t n = wc_savefs.blob[4];
	if (n > WC_SAVEFS_MAX_FILES)
		n = WC_SAVEFS_MAX_FILES;

	const uint8_t *p = wc_savefs.blob + 5;
	for (uint8_t i = 0; i < n; i++) {
		memcpy(wc_savefs.name[i], p, WC_SAVEFS_NAME_MAX);
		wc_savefs.name[i][WC_SAVEFS_NAME_MAX - 1] = 0;
		p += WC_SAVEFS_NAME_MAX;
		memcpy(&wc_savefs.size[i], p, 4);
		p += 4;
	}
	wc_savefs.count = n;
	wc_savefs_reindex();
}

static void wc_savefs_write_header(void)
{
	memcpy(wc_savefs.blob, "WCSF", 4);
	wc_savefs.blob[4] = wc_savefs.count;
	uint8_t *p = wc_savefs.blob + 5;
	for (uint8_t i = 0; i < wc_savefs.count; i++) {
		memcpy(p, wc_savefs.name[i], WC_SAVEFS_NAME_MAX);
		p += WC_SAVEFS_NAME_MAX;
		memcpy(p, &wc_savefs.size[i], 4);
		p += 4;
	}
}

static int wc_savefs_find(const char *name)
{
	for (uint8_t i = 0; i < wc_savefs.count; i++)
		if (strcmp(wc_savefs.name[i], name) == 0)
			return i;
	return -1;
}

/* Fold a finished writer back into the region. */
static void wc_savefs_commit(wc_savefs_writer_t *w, size_t written)
{
	int idx = wc_savefs_find(w->name);

	if (idx < 0) {
		if (wc_savefs.count >= WC_SAVEFS_MAX_FILES)
			return;                       /* table full: drop it */
		idx = wc_savefs.count++;
		snprintf(wc_savefs.name[idx], WC_SAVEFS_NAME_MAX, "%s", w->name);
		wc_savefs.size[idx] = 0;
	}

	/* Rewriting a file whose size changed shifts everything after it, so move
	 * the tail rather than leaving a hole. Files here are a few KB and saved
	 * rarely, so a memmove costs nothing worth optimising. */
	wc_savefs_reindex();
	const uint32_t old_size = wc_savefs.size[idx];
	const uint32_t start = wc_savefs.off[idx];
	uint32_t tail_off = start + old_size;
	uint32_t tail_len = 0;
	for (uint8_t i = (uint8_t)idx + 1; i < wc_savefs.count; i++)
		tail_len += wc_savefs.size[i];

	if (start + written + tail_len > wc_savefs.cap)
		return;                           /* would not fit: drop it */

	if (tail_len && written != old_size)
		memmove(wc_savefs.blob + start + written,
		        wc_savefs.blob + tail_off, tail_len);

	memcpy(wc_savefs.blob + start, w->buf, written);
	wc_savefs.size[idx] = (uint32_t)written;
	wc_savefs_reindex();
	wc_savefs_write_header();
}

/* ── the API a port calls ──────────────────────────────────────────────── */

/*
 * Open a save-region file. Returns NULL if the name is unknown (reads) or the
 * table is full (writes). Mirrors fopen's mode strings loosely: anything with
 * 'w' or 'a' is a write.
 *
 * A write handle must be closed with wc_savefs_fclose(), not plain fclose(),
 * so the data reaches the region.
 */
FILE *wc_savefs_fopen(const char *name, const char *mode)
{
	const int writing = (strchr(mode, 'w') != NULL) ||
	                    (strchr(mode, 'a') != NULL) ||
	                    (strchr(mode, '+') != NULL);

	if (!writing) {
		const int idx = wc_savefs_find(name);
		if (idx < 0)
			return NULL;
		return fmemopen(wc_savefs.blob + wc_savefs.off[idx],
		                wc_savefs.size[idx], "rb");
	}

	for (int i = 0; i < WC_SAVEFS_MAX_FILES; i++) {
		wc_savefs_writer_t *w = &wc_savefs_writers[i];
		if (w->fp != NULL)
			continue;
		w->cap = wc_savefs.cap;           /* generous: it is bounded on commit */
		w->buf = (uint8_t *)malloc(w->cap);
		if (w->buf == NULL)
			return NULL;
		snprintf(w->name, sizeof w->name, "%s", name);
		w->fp = fmemopen(w->buf, w->cap, "wb");
		if (w->fp == NULL) {
			free(w->buf);
			w->buf = NULL;
			return NULL;
		}
		return w->fp;
	}
	return NULL;                          /* too many open writers */
}

/*
 * Close a handle from wc_savefs_fopen(). Safe to call on a read handle, and on a
 * handle this layer did not open -- it falls through to fclose(), so a port can
 * route every fclose() here without tracking which is which.
 */
int wc_savefs_fclose(FILE *fp)
{
	if (fp == NULL)
		return EOF;

	for (int i = 0; i < WC_SAVEFS_MAX_FILES; i++) {
		wc_savefs_writer_t *w = &wc_savefs_writers[i];
		if (w->fp != fp)
			continue;

		fflush(fp);
		const long n = ftell(fp);
		fclose(fp);
		if (n > 0)
			wc_savefs_commit(w, (size_t)n);
		free(w->buf);
		w->buf = NULL;
		w->fp = NULL;
		w->name[0] = 0;
		return 0;
	}
	return fclose(fp);
}

/* Does a save file exist? For ports that probe before reading. */
int wc_savefs_exists(const char *name)
{
	return wc_savefs_find(name) >= 0;
}

#endif /* WC_SAVEFS_IMPLEMENTATION */

#endif /* WC_SDL_SAVEFS_H */
