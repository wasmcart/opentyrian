/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "file.h"

#include "opentyr.h"
#include "varz.h"

#include "SDL.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *custom_data_dir = NULL;

// finds the Tyrian data directory
const char *data_dir(void)
{
	const char *const dirs[] =
	{
		custom_data_dir,
		TYRIAN_DIR,
		"data",
		".",
	};

	static const char *dir = NULL;

	if (dir != NULL)
		return dir;

	for (uint i = 0; i < COUNTOF(dirs); ++i)
	{
		if (dirs[i] == NULL)
			continue;

		FILE *f = dir_fopen(dirs[i], "tyrian1.lvl", "rb");
		if (f)
		{
			fclose(f);

			dir = dirs[i];
			break;
		}
	}

	if (dir == NULL) // data not found
		dir = "";

	return dir;
}

// prepend directory and fopen
#ifdef WASM_CART
/*
 * Cart builds have no filesystem: the game data lives in the .wasc and is read
 * through wc_load_asset(). Shimming here rather than at every call site works
 * because all three dir_fopen_* variants and dir_file_exists() funnel through
 * this one function -- so the ~300 stdio calls in the rest of the game are
 * untouched and keep working on a normal FILE*.
 *
 * The asset is loaded whole into memory and wrapped with fmemopen(), which
 * emscripten does support. Tyrian's largest asset is a few hundred KB, so
 * whole-file loading is simpler than a streaming shim and costs little.
 *
 * WRITES go to the cart's save region rather than failing. get_user_directory()
 * returns a sentinel in cart builds (see config.c), so this function can tell a
 * save file -- tyrian.cfg, tyrian.sav, opentyrian.cfg -- from a shipped data file
 * and route it to wc_savefs (wasmcart-sdl2's named-file layer over save_ptr).
 * Shipped assets stay read-only: a write to one returns NULL and the game warns
 * and carries on, which is what it already does on a read-only install.
 */
#include "wasmcart.h"

/* This file compiles the save filesystem; wasmcart_cart.c includes the header
 * normally and shares it. Exactly one TU may define this -- see the header. */
#define WC_SAVEFS_IMPLEMENTATION
#include "wc_sdl_savefs.h"

/* Save-region backing store. Declared in wc_get_info() as save_ptr/save_size, so
 * the host loads it before wc_init() and persists it afterwards. */
uint8_t wasmcart_save_blob[WC_SAVEFS_BYTES];

/* The fclose() the rest of the game calls (see wasmcart_config.h). The header
 * above already #undef'd the macro, so this reaches the real wc_savefs_fclose. */
int wasmcart_fclose(FILE *fp) { return wc_savefs_fclose(fp); }

FILE *dir_fopen(const char *dir, const char *file, const char *mode)
{
	/* Two different worlds behind one function. get_user_directory() returns a
	 * sentinel in cart builds, so config and progress go to the save region
	 * (read AND write), while everything else is a shipped asset (read only). */
	if (dir != NULL && strcmp(dir, WASMCART_SAVE_DIR) == 0)
		return wc_savefs_fopen(file, mode);

	if (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL ||
	    strchr(mode, '+') != NULL)
		return NULL;   /* assets are read-only; caller warns and continues */

	const int32_t size = wc_asset_size(file, strlen(file));
	if (size <= 0)
		return NULL;

	/* fmemopen takes ownership of nothing, so this buffer must outlive the
	 * FILE*. Leaked on purpose: the game opens each data file a handful of
	 * times over a session and there is no fclose hook to free it from. */
	uint8_t *buf = malloc((size_t)size);
	if (buf == NULL)
		return NULL;

	if (wc_load_asset(file, strlen(file), buf, (uint32_t)size) != size)
	{
		free(buf);
		return NULL;
	}

	FILE *f = fmemopen(buf, (size_t)size, "rb");
	if (f == NULL)
		free(buf);
	return f;
}
#else
FILE *dir_fopen(const char *dir, const char *file, const char *mode)
{
	char *path = malloc(strlen(dir) + 1 + strlen(file) + 1);
	sprintf(path, "%s/%s", dir, file);

	FILE *f = fopen(path, mode);

	free(path);

	return f;
}
#endif

// warn when dir_fopen fails
FILE *dir_fopen_warn(const char *dir, const char *file, const char *mode)
{
	FILE *f = dir_fopen(dir, file, mode);

	if (f == NULL)
		fprintf(stderr, "warning: failed to open '%s': %s\n", file, strerror(errno));

	return f;
}

// die when dir_fopen fails
FILE *dir_fopen_die(const char *dir, const char *file, const char *mode)
{
	FILE *f = dir_fopen(dir, file, mode);

	if (f == NULL)
	{
		fprintf(stderr, "error: failed to open '%s': %s\n", file, strerror(errno));
		fprintf(stderr, "error: One or more of the required Tyrian " TYRIAN_VERSION " data files could not be found.\n"
		                "       Please read the README file.\n");
		JE_tyrianHalt(1);
	}

	return f;
}

// check if file can be opened for reading
bool dir_file_exists(const char *dir, const char *file)
{
	FILE *f = dir_fopen(dir, file, "rb");
	if (f != NULL)
		fclose(f);
	return (f != NULL);
}

// returns end-of-file position
long ftell_eof(FILE *f)
{
	long pos = ftell(f);

	fseek(f, 0, SEEK_END);
	long size = ftell(f);

	fseek(f, pos, SEEK_SET);

	return size;
}

void fread_die(void *buffer, size_t size, size_t count, FILE *stream)
{
	size_t result = fread(buffer, size, count, stream);
	if (result != count)
	{
		fprintf(stderr, "error: An unexpected problem occurred while reading from a file.\n");
		SDL_Quit();
		exit(EXIT_FAILURE);
	}
}

void fwrite_die(const void *buffer, size_t size, size_t count, FILE *stream)
{
	size_t result = fwrite(buffer, size, count, stream);
	if (result != count)
	{
		fprintf(stderr, "error: An unexpected problem occurred while writing to a file.\n");
		SDL_Quit();
		exit(EXIT_FAILURE);
	}
}
