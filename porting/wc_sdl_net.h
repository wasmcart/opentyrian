/*
 * wc_sdl_net.h — SDL_net's UDP API over wasmcart's peer transport.
 *
 * Why this exists
 * ---------------
 * A lot of the classic SDL games worth porting have networked multiplayer, and
 * almost all of them use SDL_net's UDP calls: SDLNet_UDP_Open, SDLNet_UDP_Send,
 * SDLNet_UDP_Recv, with SDLNet_AllocPacket for buffers. There is no BSD socket
 * layer inside a cart, so those games either lose multiplayer or need their
 * netcode rewritten.
 *
 * They do not need rewriting. wasmcart has wc_peer_* -- a transport-agnostic
 * message interface (see SPEC.md, "Peer Connection") -- and a UDP datagram maps
 * onto a peer message essentially unchanged: both are discrete, bounded,
 * unordered and unreliable. Games built on SDL_net UDP already assume packet
 * loss and reordering and do their own sequencing and acknowledgement, so they
 * tolerate a peer transport for exactly the reasons they tolerate UDP.
 *
 * This header implements the SDL_net surface those games actually call, on top of
 * wc_peer_*. Include it instead of <SDL_net.h> in a cart build and the game's
 * netcode compiles and runs untouched.
 *
 * What maps cleanly, and what does not
 * ------------------------------------
 * Send, receive, packet alloc/free and the byte-order helpers are direct.
 *
 * Addressing is NOT. SDL_net addresses a host and port; wc_peer_* addresses an
 * opaque peer id the host hands out, and the host decides what a peer even is
 * (a WebSocket relay, a data channel, a LAN socket). So SDLNet_ResolveHost
 * stores the name for wc_peer_open() to interpret, and the numeric host/port a
 * game computes are not meaningful here.
 *
 * Peer DISCOVERY does not map at all. SDL_net games find each other with
 * broadcast or multicast, and a browser cannot send either -- no API exists, at
 * any privilege level. Nothing this header does can work around that; it is a
 * property of the platform, not a gap in the shim. A host that wants LAN-style
 * discovery has to virtualise it (a rendezvous server, a relay room, a signalling
 * channel) and expose the results as peers. wc_peer_broadcast() sends to peers
 * already connected -- it is not a discovery mechanism.
 *
 * Consequence for a port: connect by whatever addressing the host defines and
 * let it hand you peers. "Search the LAN for a game" is out of reach.
 *
 * Usage
 * -----
 *   #define WC_SDL_NET_IMPLEMENTATION      // in exactly one .c file
 *   #include "wc_sdl_net.h"
 *
 * Then call wc_net_pump() once per frame, before the game polls for packets, so
 * arriving messages are queued where SDLNet_UDP_Recv can see them. Delivery is
 * driven by the host's callbacks, which only fire while the cart is running.
 */

#ifndef WC_SDL_NET_H
#define WC_SDL_NET_H

/* The callbacks below are wasm exports. Guarding the attribute lets this header
 * compile natively so the shim's queueing and packet handling can be unit tested
 * against a stub peer layer, rather than only through a full cart. */
#ifdef __wasm__
#define WC_NET_EXPORT(name) __attribute__((export_name(name)))
#else
#define WC_NET_EXPORT(name)
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* wasmcart.h keeps the peer declarations behind this gate so a cart that does not
 * network declares no net imports. A cart including this header is networking by
 * definition, so turn it on before the include rather than making every port
 * remember to. */
#ifndef WC_NET_NO_WASMCART_H
#ifndef WC_USE_NET_PEER
#define WC_USE_NET_PEER
#endif
#include "wasmcart.h"
#endif

/* Matching the SDL_net names on purpose: the point is that a game's existing
 * network code compiles unchanged. */
typedef struct {
	uint32_t host;      /* not meaningful under a peer transport; see above */
	uint16_t port;
} IPaddress;

typedef struct {
	int       channel;
	uint8_t  *data;
	int       len;
	int       maxlen;
	int       status;
	IPaddress address;
} UDPpacket;

typedef struct WC_UDPsocket *UDPsocket;

/* How many inbound datagrams to hold between pumps. A game polling once per
 * frame at 60Hz will not fall behind by more than a handful; beyond that the
 * oldest are dropped, which is what a UDP socket's receive buffer does too. */
#ifndef WC_NET_QUEUE_DEPTH
#define WC_NET_QUEUE_DEPTH 64
#endif

/* SDL_net's own cap. Peer messages can exceed it, but a game written to SDL_net
 * never sends more. */
#ifndef WC_NET_PACKET_MAX
#define WC_NET_PACKET_MAX 1500
#endif

int          SDLNet_Init(void);
void         SDLNet_Quit(void);
const char  *SDLNet_GetError(void);
int          SDLNet_ResolveHost(IPaddress *address, const char *host, uint16_t port);
UDPsocket    SDLNet_UDP_Open(uint16_t port);
void         SDLNet_UDP_Close(UDPsocket sock);
int          SDLNet_UDP_Bind(UDPsocket sock, int channel, const IPaddress *address);
void         SDLNet_UDP_Unbind(UDPsocket sock, int channel);
int          SDLNet_UDP_Send(UDPsocket sock, int channel, UDPpacket *packet);
int          SDLNet_UDP_Recv(UDPsocket sock, UDPpacket *packet);
UDPpacket   *SDLNet_AllocPacket(int size);
void         SDLNet_FreePacket(UDPpacket *packet);

/* Pump host-delivered messages into the receive queue. Call once per frame. */
void         wc_net_pump(void);

/* SDL_net's byte-order helpers are big-endian on the wire regardless of host, so
 * they are the same code everywhere and can stay inline. */
static inline void SDLNet_Write16(uint16_t value, void *area)
{
	uint8_t *p = (uint8_t *)area;
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)(value & 0xff);
}

static inline void SDLNet_Write32(uint32_t value, void *area)
{
	uint8_t *p = (uint8_t *)area;
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)(value & 0xff);
}

static inline uint16_t SDLNet_Read16(const void *area)
{
	const uint8_t *p = (const uint8_t *)area;
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t SDLNet_Read32(const void *area)
{
	const uint8_t *p = (const uint8_t *)area;
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

#ifdef WC_SDL_NET_IMPLEMENTATION

struct WC_UDPsocket {
	int open;
};

static struct WC_UDPsocket wc_net_socket;
static const char *wc_net_error = "";

/* The peer the game is talking to. SDL_net binds channels to addresses; with a
 * peer transport there is one logical link, so channel 0 is the peer and the
 * bind is bookkeeping. */
static int  wc_net_peer = -1;
static char wc_net_addr[256];

/* Inbound ring. Fixed slots so nothing allocates on the delivery path. */
typedef struct {
	uint8_t data[WC_NET_PACKET_MAX];
	int     len;
} wc_net_slot_t;

static wc_net_slot_t wc_net_queue[WC_NET_QUEUE_DEPTH];
static int wc_net_head, wc_net_tail, wc_net_dropped;

/*
 * Host callback: a peer message arrived.
 *
 * Copied into the ring rather than handed straight to the game, because the game
 * polls with SDLNet_UDP_Recv on its own schedule and the buffer the host gives us
 * is only valid for the duration of this call.
 */
WC_NET_EXPORT("wc_peer_on_message")
void wc_peer_on_message(int peer_id, const void *data, unsigned int len)
{
	(void)peer_id;

	if (len > WC_NET_PACKET_MAX)
		len = WC_NET_PACKET_MAX;   /* SDL_net games never send more */

	const int next = (wc_net_tail + 1) % WC_NET_QUEUE_DEPTH;
	if (next == wc_net_head) {
		/* Full: drop the newest. A UDP receive buffer overflowing drops too,
		 * and the game's own sequencing already handles loss. */
		wc_net_dropped++;
		return;
	}

	memcpy(wc_net_queue[wc_net_tail].data, data, len);
	wc_net_queue[wc_net_tail].len = (int)len;
	wc_net_tail = next;
}

WC_NET_EXPORT("wc_peer_on_open")
void wc_peer_on_open(int peer_id)
{
	/* First peer to connect becomes the link, whether we dialled it or the host
	 * handed it to us (a relay room, a lobby). */
	if (wc_net_peer < 0)
		wc_net_peer = peer_id;
}

WC_NET_EXPORT("wc_peer_on_close")
void wc_peer_on_close(int peer_id)
{
	if (wc_net_peer == peer_id)
		wc_net_peer = -1;
}

WC_NET_EXPORT("wc_peer_on_error")
void wc_peer_on_error(int peer_id, const void *msg, unsigned int len)
{
	(void)peer_id; (void)msg; (void)len;
	wc_net_error = "peer transport error";
}

/* Messages are delivered by callback, so there is nothing to poll for. This
 * exists so a port can call it once per frame without caring how delivery works,
 * and so the drop counter surfaces somewhere. */
void wc_net_pump(void)
{
	if (wc_net_dropped > 0) {
		/* Loud, but once: a game losing packets to a full queue is a real
		 * problem worth seeing, and silence here would look like a netcode bug. */
		static int warned = 0;
		if (!warned) {
			warned = 1;
			wc_net_error = "receive queue overflowed; packets dropped";
		}
	}

	/* Adopt a host-supplied peer that connected before we asked for one. */
	if (wc_net_peer < 0 && wc_peer_count() > 0)
		wc_net_peer = wc_peer_id(0);
}

int SDLNet_Init(void)
{
	wc_net_head = wc_net_tail = wc_net_dropped = 0;
	wc_net_error = "";
	return 0;
}

void SDLNet_Quit(void)
{
	if (wc_net_peer >= 0) {
		wc_peer_close(wc_net_peer);
		wc_net_peer = -1;
	}
	wc_net_socket.open = 0;
}

const char *SDLNet_GetError(void)
{
	return wc_net_error;
}

/*
 * Stash the address for wc_peer_open() rather than resolving it.
 *
 * A cart cannot resolve a name and the numeric host/port SDL_net would return
 * mean nothing to a peer transport -- the host decides what an address is. So the
 * string is kept and handed over verbatim on bind.
 */
int SDLNet_ResolveHost(IPaddress *address, const char *host, uint16_t port)
{
	if (address == NULL)
		return -1;

	address->host = 0;
	address->port = port;

	if (host == NULL) {
		/* SDL_net's "listen" form. Nothing to dial: wait for the host to hand
		 * us a peer via wc_peer_on_open. */
		wc_net_addr[0] = 0;
		return 0;
	}

	snprintf(wc_net_addr, sizeof wc_net_addr, "%s", host);
	return 0;
}

UDPsocket SDLNet_UDP_Open(uint16_t port)
{
	(void)port;   /* the host owns addressing; a cart does not bind ports */
	wc_net_socket.open = 1;
	return &wc_net_socket;
}

void SDLNet_UDP_Close(UDPsocket sock)
{
	if (sock != NULL)
		sock->open = 0;
}

/*
 * Dial the address stashed by SDLNet_ResolveHost.
 *
 * SDL_net binds a channel to an address so later sends can name the channel;
 * with one logical peer link, this is where the connection is actually made.
 */
int SDLNet_UDP_Bind(UDPsocket sock, int channel, const IPaddress *address)
{
	(void)address;

	if (sock == NULL || !sock->open)
		return -1;

	/* Empty address means listen-only: the peer arrives on its own. */
	if (wc_net_addr[0] == 0)
		return channel;

	if (wc_net_peer < 0) {
		const int id = wc_peer_open(wc_net_addr, (unsigned int)strlen(wc_net_addr));
		if (id < 0) {
			wc_net_error = "wc_peer_open failed (host declined or no net grant)";
			return -1;
		}
		wc_net_peer = id;
	}
	return channel;
}

void SDLNet_UDP_Unbind(UDPsocket sock, int channel)
{
	(void)sock; (void)channel;
	if (wc_net_peer >= 0) {
		wc_peer_close(wc_net_peer);
		wc_net_peer = -1;
	}
}

/* Returns 1 on success, 0 on failure -- SDL_net's convention, which callers
 * check. */
int SDLNet_UDP_Send(UDPsocket sock, int channel, UDPpacket *packet)
{
	(void)channel;

	if (sock == NULL || !sock->open || packet == NULL || packet->len <= 0)
		return 0;

	if (wc_net_peer < 0) {
		wc_net_error = "no peer connected";
		return 0;
	}

	if (wc_peer_send(wc_net_peer, packet->data, (unsigned int)packet->len) < 0) {
		wc_net_error = "wc_peer_send failed";
		return 0;
	}
	return 1;
}

/* Returns 1 if a packet was received, 0 if none waiting, -1 on error -- again
 * SDL_net's convention. */
int SDLNet_UDP_Recv(UDPsocket sock, UDPpacket *packet)
{
	if (sock == NULL || !sock->open || packet == NULL)
		return -1;

	if (wc_net_head == wc_net_tail)
		return 0;

	const wc_net_slot_t *slot = &wc_net_queue[wc_net_head];
	int len = slot->len;
	if (len > packet->maxlen)
		len = packet->maxlen;

	memcpy(packet->data, slot->data, (size_t)len);
	packet->len = len;
	packet->channel = 0;
	packet->status = len;
	packet->address.host = 0;
	packet->address.port = 0;

	wc_net_head = (wc_net_head + 1) % WC_NET_QUEUE_DEPTH;
	return 1;
}

UDPpacket *SDLNet_AllocPacket(int size)
{
	if (size < 0)
		return NULL;

	UDPpacket *p = (UDPpacket *)calloc(1, sizeof(UDPpacket));
	if (p == NULL)
		return NULL;

	p->data = (uint8_t *)calloc(1, (size_t)size > 0 ? (size_t)size : 1);
	if (p->data == NULL) {
		free(p);
		return NULL;
	}
	p->maxlen = size;
	return p;
}

void SDLNet_FreePacket(UDPpacket *packet)
{
	if (packet == NULL)
		return;
	free(packet->data);
	free(packet);
}

#endif /* WC_SDL_NET_IMPLEMENTATION */

#endif /* WC_SDL_NET_H */
