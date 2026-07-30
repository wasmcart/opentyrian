/*
 * wasmcart_cart.c — wasmcart entry points for OpenTyrian.
 *
 * Why the port looks like this:
 *
 * OpenTyrian draws a 320x200 8-bit paletted surface (VGAScreen) and pushes it to
 * the screen through exactly one function, JE_showVGA(). All 79 draw sites funnel
 * through there, so that is the natural frame boundary and the port is one
 * interception rather than a rewrite.
 *
 * The game owns its loop: main() calls titleScreen() and JE_main(), which spin
 * internally, and there are 86 delayUntilElapsed() sites scattered through the
 * code. Restructuring that into a per-frame step function would be rewriting the
 * game. So the cart uses wasmcart's loop inversion instead -- the game runs until
 * it flips a frame, wc_frame_yield() unwinds the whole stack out to the host, and
 * the next wc_render() rewinds back to exactly that point. From the game's side
 * nothing changed.
 *
 * Input: the game reads keysactive[], a plain scancode array. Driving that from
 * the pad is far less code than standing up a fake SDL joystick, and it means
 * menus, gameplay and the ship editor all work without touching any per-screen
 * input handling. Tyrian is arrow-keys-and-fire, which is already a pad-shaped
 * control scheme -- that is why this game was picked.
 *
 * Audio: the game opens a normal SDL audio device with a pull callback. The
 * wasmcart SDL2 backend already bridges that to the host's ring buffer, so this
 * file only has to own the ring and pump it once per frame.
 */

/* wc_peer_count() below decides single-player vs networked, so this TU needs the
 * peer declarations. wc_sdl_net.h sets the same gate for its own use. */
#ifndef WC_USE_NET_PEER
#define WC_USE_NET_PEER
#endif
#include "wasmcart.h"
#include "wc_cart.h"

#include "SDL.h"
#include "SDL_wasmcart_audio.h"

#include "keyboard.h"
#include "video.h"
#include "wc_sdl_savefs.h"

/* Defined in file.c, next to the shim that writes it. Sized here so sizeof()
 * works -- an extern array with no extent is an incomplete type. */
extern uint8_t wasmcart_save_blob[WC_SAVEFS_BYTES];

#include <stdbool.h>
#include <stdint.h>

/* The game's native resolution. Declaring it rather than scaling keeps the
 * pixels exact and lets the host letterbox however it wants. */
#define CART_W 320
#define CART_H 200

/* Mono 11025*OUTPUT_QUALITY from loudness.c, expanded to stereo by the pump.
 * S16 to match the callback's AUDIO_S16SYS, so no conversion happens. */
#define AUDIO_RATE 22050
#define AUDIO_CAP  4096

static uint32_t framebuffer[CART_W * CART_H];
static int16_t  audio_ring[AUDIO_CAP * 2];
static uint32_t audio_write_cursor;
static wc_pad_t pads[4];

/* Set once the game's entry point has been called. wc_on_suspend() checks it so a
 * suspend before startup cannot write uninitialised config over a restored save. */
static bool game_started;
static wc_time_t cart_time;
static wc_info_t info;
static wc_host_info_t host_info;

/* Asyncify unwind stack. The call depth when the game flips a frame is deep --
 * menus call loops that call the renderer -- so this is deliberately generous.
 * Too small corrupts the unwind rather than failing cleanly. */
static uint8_t  yield_stack[256 * 1024];
static uint32_t yield_desc[2];

__attribute__((export_name("wc_yield_buffer")))
uint32_t wc_yield_buffer(void)
{
	yield_desc[0] = (uint32_t)(uintptr_t)yield_stack;
	yield_desc[1] = (uint32_t)(uintptr_t)yield_stack + sizeof(yield_stack);
	return (uint32_t)(uintptr_t)yield_desc;
}

/* ─── Pad → keyboard ─────────────────────────────────────────────────────
 *
 *   d-pad / left stick   ship movement
 *   A                    fire            (space)
 *   B                    rear weapon     (enter)
 *   X / Y                left/right sidekick (ctrl / alt)
 *   START                confirm         (enter)
 *   SELECT               pause / back    (escape)
 */
static void set_key(int sc, bool down)
{
	if (sc >= 0 && sc < SDL_NUM_SCANCODES)
		keysactive[sc] = down;
}

static void apply_pad_to_keys(void)
{
	const wc_pad_t *p = &pads[0];
	const uint16_t b = p->buttons;

	/* Stick doubles as the d-pad past a deadzone, so either input works. */
	const int16_t dz = 8000;
	set_key(SDL_SCANCODE_LEFT,  (b & WC_BTN_LEFT)  || p->left_x < -dz);
	set_key(SDL_SCANCODE_RIGHT, (b & WC_BTN_RIGHT) || p->left_x >  dz);
	set_key(SDL_SCANCODE_UP,    (b & WC_BTN_UP)    || p->left_y < -dz);
	set_key(SDL_SCANCODE_DOWN,  (b & WC_BTN_DOWN)  || p->left_y >  dz);

	set_key(SDL_SCANCODE_SPACE,  (b & WC_BTN_A) != 0);
	set_key(SDL_SCANCODE_RETURN, (b & (WC_BTN_B | WC_BTN_START)) != 0);
	set_key(SDL_SCANCODE_LCTRL,  (b & WC_BTN_X) != 0);
	set_key(SDL_SCANCODE_LALT,   (b & WC_BTN_Y) != 0);
	set_key(SDL_SCANCODE_ESCAPE, (b & WC_BTN_SELECT) != 0);
}

/* ─── Frame handoff ──────────────────────────────────────────────────────
 *
 * Called from JE_showVGA() (see video.c). Converts the paletted surface into the
 * cart framebuffer, pumps audio, and yields. Execution resumes on the line after
 * the yield when the host calls wc_render() again.
 */
void wasmcart_present(const uint8_t *pixels, int pitch, const SDL_Color *palette)
{
	for (int y = 0; y < CART_H; y++)
	{
		const uint8_t *row = pixels + (size_t)y * (size_t)pitch;
		uint32_t *dst = &framebuffer[(size_t)y * CART_W];
		for (int x = 0; x < CART_W; x++)
		{
			const SDL_Color c = palette[row[x]];
			dst[x] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
		}
	}

	/* Keep the host's audio buffer fed. Driven off delta_ms so it stays correct
	 * if the host's frame rate is not 60. */
	uint32_t delta = (uint32_t)cart_time.delta_ms;
	if (delta == 0 || delta > 100)
		delta = 16;   /* first frame, or a stall the clamp already caught */
	wasmcart_audio_pump(audio_ring, AUDIO_CAP, &audio_write_cursor,
	                    AUDIO_RATE, delta);

	wc_frame_yield();      /* → host. Resumes here next wc_render(). */
	apply_pad_to_keys();   /* pad state the host wrote while we were suspended */
}

/*
 * The game's SDL_Delay(), redirected here by wasmcart_config.h.
 *
 * The five wait loops in keyboard.c poll for input and SDL_Delay() between tries
 * without ever drawing, so they never reach the frame boundary above. Without a
 * yield here the cart never returns to the host, the host never gets to write pad
 * state, and the loop waits forever for input that cannot arrive.
 *
 * Same handoff as a frame, minus the framebuffer conversion: there is no new
 * picture to convert, and the host keeps showing the last one.
 */
void wasmcart_delay_yield(unsigned int ms)
{
	uint32_t delta = (uint32_t)cart_time.delta_ms;
	if (delta == 0 || delta > 100)
		delta = 16;
	wasmcart_audio_pump(audio_ring, AUDIO_CAP, &audio_write_cursor,
	                    AUDIO_RATE, delta);

	(void)ms;   /* the host sets the pace; a cart cannot sleep past a frame */

	wc_frame_yield();
	apply_pad_to_keys();
}

/* ─── Cart ABI ───────────────────────────────────────────────────────────
 *
 * Defined in opentyr.c; renamed from main() for the cart build so the game's
 * entry point can be called on demand rather than by the runtime.
 */
extern int opentyrian_main(int argc, char *argv[]);

WC_EXPORT wc_info_t *wc_get_info(void)
{
	info.version = WC_ABI_VERSION;
	info.width = CART_W;
	info.height = CART_H;
	info.fb_ptr = (uint32_t)(uintptr_t)framebuffer;
	info.audio_ptr = (uint32_t)(uintptr_t)audio_ring;
	info.audio_cap = AUDIO_CAP;
	info.audio_write_ptr = (uint32_t)(uintptr_t)&audio_write_cursor;
	info.input_ptr = (uint32_t)(uintptr_t)pads;
	info.time_ptr = (uint32_t)(uintptr_t)&cart_time;
	info.host_info_ptr = (uint32_t)(uintptr_t)&host_info;
	/* Save region. The host loads it before wc_init() and persists it after, so
	 * config and progress survive a relaunch. file.c routes the game's writes
	 * here via wc_savefs (see its dir_fopen shim). */
	info.save_ptr = (uint32_t)(uintptr_t)wasmcart_save_blob;
	info.save_size = (uint32_t)sizeof(wasmcart_save_blob);
	/* WC_FLAG_NET_PEER declares that this cart can use the peer transport. It is
	 * only half the gate: the host also requires a net.domains grant in the
	 * manifest, so declaring it does not grant anything by itself. Multiplayer
	 * simply stays unavailable on a host that offers no peers, and the game runs
	 * single-player as normal.
	 *
	 * S16 audio: WC_FLAG_AUDIO_F32 deliberately NOT set. */
	info.flags = WC_FLAG_NET_PEER;
	return &info;
}

WC_EXPORT_INIT void wc_init(void)
{
	/* Parse whatever the host restored into the save region. Must happen before
	 * the game reads its config, which it does on the first wc_render(). */
	wc_savefs_init(wasmcart_save_blob, sizeof(wasmcart_save_blob));

	/* Deliberately empty. The game's own entry point does every bit of
	 * initialisation, and it has to run inside wc_render() so that anything it
	 * draws along the way (the loading screens, the Christmas prompt) can yield
	 * like a normal frame. */
}

/*
 * Save config when the host is about to stop calling us.
 *
 * The game writes its config in JE_tyrianHalt(), on the way out of main() -- and
 * a cart never gets there. The host just stops calling wc_render(); there is no
 * quit path through the game's own shutdown. So without this, options and
 * high scores set during a session would never reach the save region.
 *
 * Progress is already covered: JE_saveGame() calls saveSaves() itself, so
 * saving a game in-game commits immediately. This is for the rest.
 */
__attribute__((export_name("wc_on_suspend")))
void wc_on_suspend(void)
{
	extern void saveConfiguration(void);
	extern void saveSaves(void);

	/* Guard on started: a suspend before the game has initialised would write
	 * uninitialised config over whatever the host restored. */
	if (game_started)
	{
		saveConfiguration();
		saveSaves();
	}
}

WC_EXPORT_RENDER void wc_render(void)
{
	if (!game_started)
	{
		game_started = true;
		apply_pad_to_keys();

		/*
		 * Enter network mode when the host has actually given us a peer.
		 *
		 * Upstream selects multiplayer with `--net <host:port>` on argv, which a
		 * cart has none of. Deciding from wc_peer_count() instead is the right
		 * signal: peers only exist if the host arranged them (a lobby, a relay
		 * room, a matchmaker), so a cart launched normally plays single-player
		 * and one launched into a session plays networked, with no UI either way.
		 *
		 * The address passed to --net is not used for dialling here -- the peer is
		 * already open, and wc_sdl_net.h routes sends to it -- but the game parses
		 * argv strictly, so it has to be well-formed.
		 */
		char *argv_single[] = { (char *)"opentyrian", NULL };
		char *argv_net[] = { (char *)"opentyrian", (char *)"--net", (char *)"peer:1", NULL };

		const int have_peer = wc_peer_count() > 0;

		/* Never returns. The game yields from wasmcart_present() once per flip,
		 * and the host's rewind resumes inside the game rather than back here.
		 * A cart that reaches the line below has exited the game. */
		if (have_peer)
			opentyrian_main(3, argv_net);
		else
			opentyrian_main(1, argv_single);
	}
}
