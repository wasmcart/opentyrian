/*
 * gl4es_stub.c — link-time stubs for a 2D-only cart.
 *
 * The wasmcart SDL2 video backend references three GL symbols even when nothing
 * calls them. OpenTyrian draws a paletted software surface and never touches GL,
 * so these exist purely to satisfy the linker. Documented in wasmcart-sdl2's
 * PORTING_GUIDE under Common Problems.
 */
void  gl4es_bridge_set_size(int w, int h) { (void)w; (void)h; }
void *wc_gl4es_GetProcAddress(const char *p) { (void)p; return 0; }

/* Also stubbed rather than linking sdl2_gl_blit.c.
 *
 * That file imports 25 GL functions, and a cart importing anything from the `gl`
 * module is treated as a GL cart by the host -- it then reads frames back from a
 * GL context this cart never draws into, and every frame comes out blank. The
 * game renders a paletted software surface and hands it over through
 * wasmcart_present(), so the GL path is never taken and this only has to exist
 * for the linker. */
void wc_sdl_gl_blit(const void *pixels, int w, int h)
{
	(void)pixels; (void)w; (void)h;
}
