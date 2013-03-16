/*
 * Copyright © 2013 Hardening <rdp.effort@gmail.com>
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/update.h>
#include <freerdp/input.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/nsc.h>

#include "compositor.h"
#include "pixman-renderer.h"

struct rdp_compositor_config {
	int width;
	int height;
	char *bind_address;
	int port;
	char *rdp_key;
	char *server_cert;
	char *server_key;
	char *extra_modes;
	int env_socket;
};

struct rdp_output;

struct rdp_compositor {
	struct weston_compositor base;
	struct weston_seat main_seat;

	freerdp_listener *listener;
	struct wl_event_source *listener_events[32];
	struct rdp_output *output;

	char *server_cert;
	char *server_key;
	char *rdp_key;
	int tls_enabled;
};

enum peer_item_flags {
	RDP_PEER_ACTIVATED      = (1 << 0),
	RDP_PEER_OUTPUT_ENABLED = (1 << 1),
};

struct rdp_peers_item {
	int flags;
	freerdp_peer *peer;
	struct weston_seat seat;

	struct wl_list link;
};

struct rdp_output {
	struct weston_output base;
	struct wl_event_source *finish_frame_timer;
	pixman_image_t *shadow_surface;

	struct wl_list peers;
};

struct rdp_peer_context {
	rdpContext _p;
	struct rdp_compositor *rdpCompositor;

	/* file descriptors and associated events */
	int fds[32];
	struct wl_event_source *events[32];

	RFX_CONTEXT *rfx_context;
	STREAM *encode_stream;
	RFX_RECT *rfx_rects;
	NSC_CONTEXT *nsc_context;

	int prev_x;
	int prev_y;
	struct rdp_peers_item item;
};
typedef struct rdp_peer_context RdpPeerContext;

static void
rdp_compositor_config_init(struct rdp_compositor_config *config) {
	config->width = 640;
	config->height = 480;
	config->bind_address = NULL;
	config->port = 3389;
	config->rdp_key = NULL;
	config->server_cert = NULL;
	config->server_key = NULL;
	config->extra_modes = NULL;
	config->env_socket = 0;
}

static void
rdp_peer_refresh_rfx(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height, nrects, i;
	pixman_box32_t *region, *rects;
	uint32_t *ptr;
	RFX_RECT *rfxRect;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND* cmd = &update->surface_bits_command;
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	stream_clear(context->encode_stream);
	stream_set_pos(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

	cmd->destLeft = damage->extents.x1;
	cmd->destTop = damage->extents.y1;
	cmd->destRight = damage->extents.x2;
	cmd->destBottom = damage->extents.y2;
	cmd->bpp = 32;
	cmd->codecID = peer->settings->RemoteFxCodecId;
	cmd->width = width;
	cmd->height = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * pixman_image_get_width(image);

	rects = pixman_region32_rectangles(damage, &nrects);
	if(context->rfx_rects)
		context->rfx_rects = malloc(nrects * sizeof *rfxRect);
	else
		context->rfx_rects = realloc(context->rfx_rects, nrects * sizeof *rfxRect);

	for (i = 0; i < nrects; i++) {
		region = &rects[i];
		rfxRect = &context->rfx_rects[i];

		rfxRect->x = (region->x1 - damage->extents.x1);
		rfxRect->y = (region->y1 - damage->extents.y1);
		rfxRect->width = (region->x2 - region->x1);
		rfxRect->height = (region->y2 - region->y1);
	}

	rfx_compose_message(context->rfx_context, context->encode_stream, context->rfx_rects, nrects,
			(BYTE *)ptr, width, height,
			pixman_image_get_stride(image)
	);

	cmd->bitmapDataLength = stream_get_length(context->encode_stream);
	cmd->bitmapData = stream_get_head(context->encode_stream);

	update->SurfaceBits(update->context, cmd);
}


static void
rdp_peer_refresh_nsc(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height;
	uint32_t *ptr;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND* cmd = &update->surface_bits_command;
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	stream_clear(context->encode_stream);
	stream_set_pos(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

	cmd->destLeft = damage->extents.x1;
	cmd->destTop = damage->extents.y1;
	cmd->destRight = damage->extents.x2;
	cmd->destBottom = damage->extents.y2;
	cmd->bpp = 32;
	cmd->codecID = peer->settings->NSCodecId;
	cmd->width = width;
	cmd->height = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * pixman_image_get_width(image);

	nsc_compose_message(context->nsc_context, context->encode_stream, (BYTE *)ptr,
			cmd->width,	cmd->height,
			pixman_image_get_stride(image));
	cmd->bitmapDataLength = stream_get_length(context->encode_stream);
	cmd->bitmapData = stream_get_head(context->encode_stream);
	update->SurfaceBits(update->context, cmd);
}

static void
rdp_peer_refresh_raw(pixman_region32_t *region, pixman_image_t *image, freerdp_peer *peer)
{
	pixman_image_t *tile;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND* cmd = &update->surface_bits_command;
	pixman_box32_t *extends = pixman_region32_extents(region);

	cmd->bpp = 32;
	cmd->codecID = 0;
	cmd->width = (extends->x2 - extends->x1);
	cmd->height = (extends->y2 - extends->y1);;
	cmd->bitmapDataLength = cmd->width * cmd->height * 4;
	tile = pixman_image_create_bits(PIXMAN_x8r8g8b8, cmd->width, cmd->height, 0, cmd->width * 4);
	pixman_image_composite32(PIXMAN_OP_SRC,	image, NULL, /* op, src, mask */
		tile, extends->x1, extends->y1, /* dest, src_x, src_y */
		0, 0, /* mask_x, mask_y */
		0, 0, /* dest_x, dest_y */
		cmd->width, cmd->height /* width, height */
	);
	freerdp_image_flip((BYTE *)pixman_image_get_data(tile),
			(BYTE *)pixman_image_get_data(tile),
			cmd->width, cmd->height, cmd->bpp
	);
	cmd->bitmapData = (BYTE *)pixman_image_get_data(tile);
	cmd->destLeft = extends->x1;
	cmd->destTop = extends->y1;
	cmd->destRight = extends->x2;
	cmd->destBottom = extends->y2;
	update->SurfaceBits(peer->context, cmd);
	pixman_image_unref(tile);
}

static void
rdp_peer_refresh_region(pixman_region32_t *region, freerdp_peer *peer)
{
	RdpPeerContext *context = (RdpPeerContext *)peer->context;
	struct rdp_output *output = context->rdpCompositor->output;
	rdpSettings *settings = peer->settings;
	pixman_box32_t *extents = pixman_region32_extents(region);

	int regionSz = (extents->x2 - extents->x1) * (extents->y2 - extents->y1);

	if(regionSz > 64 * 64) {
		if(settings->RemoteFxCodec)
			rdp_peer_refresh_rfx(region, output->shadow_surface, peer);
		else if(settings->NSCodec)
			rdp_peer_refresh_nsc(region, output->shadow_surface, peer);
		else
			rdp_peer_refresh_raw(region, output->shadow_surface, peer);
	} else {
		rdp_peer_refresh_raw(region, output->shadow_surface, peer);
	}
}


static void
rdp_output_repaint(struct weston_output *output_base, pixman_region32_t *damage)
{
	struct rdp_output *output = container_of(output_base, struct rdp_output, base);
	struct weston_compositor *ec = output->base.compositor;
	struct rdp_peers_item *outputPeer;

	pixman_renderer_output_set_buffer(output_base, output->shadow_surface);
	ec->renderer->repaint_output(&output->base, damage);

	wl_list_for_each(outputPeer, &output->peers, link) {
		if ((outputPeer->flags & RDP_PEER_ACTIVATED) &&
				(outputPeer->flags & RDP_PEER_OUTPUT_ENABLED))
		{
			rdp_peer_refresh_region(damage, outputPeer->peer);
		}
	}

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	wl_event_source_timer_update(output->finish_frame_timer, 16);
}

static void
rdp_output_destroy(struct weston_output *output_base)
{
	struct rdp_output *output = (struct rdp_output *)output_base;

	wl_event_source_remove(output->finish_frame_timer);
	free(output);
}

static int
finish_frame_handler(void *data)
{
	struct weston_output *output = data;
	uint32_t msec;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	weston_output_finish_frame(output, msec);

	return 1;
}


static struct weston_mode *
find_matching_mode(struct weston_output *output, struct weston_mode *target) {
	struct weston_mode *local;

	wl_list_for_each(local, &output->mode_list, link) {
		if((local->width == target->width) && (local->height == target->height))
			return local;
	}
	return 0;
}

static int
rdp_switch_mode(struct weston_output *output, struct weston_mode *target_mode) {
	struct rdp_output *rdpOutput = container_of(output, struct rdp_output, base);
	struct rdp_peers_item *rdpPeer;
	rdpSettings *settings;
	pixman_image_t *new_shadow_buffer;
	struct weston_mode *local_mode;

	local_mode = find_matching_mode(output, target_mode);
	if(!local_mode) {
		weston_log("mode %dx%d not available\n", target_mode->width, target_mode->height);
		return -ENOENT;
	}

	if(local_mode == output->current)
		return 0;

	output->current->flags = 0;
	output->current = local_mode;
	output->current->flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

	pixman_renderer_output_destroy(output);
	pixman_renderer_output_create(output);

	new_shadow_buffer = pixman_image_create_bits(PIXMAN_x8r8g8b8, target_mode->width,
			target_mode->height, 0, target_mode->width * 4);
	pixman_image_composite32(PIXMAN_OP_SRC, rdpOutput->shadow_surface, 0, new_shadow_buffer,
			0, 0, 0, 0, 0, 0, target_mode->width, target_mode->height);
	pixman_image_unref(rdpOutput->shadow_surface);
	rdpOutput->shadow_surface = new_shadow_buffer;

	wl_list_for_each(rdpPeer, &rdpOutput->peers, link) {
		settings = rdpPeer->peer->settings;
		if(!settings->DesktopResize) {
			/* too bad this peer does not support desktop resize */
			rdpPeer->peer->Close(rdpPeer->peer);
		} else {
			settings->DesktopWidth = target_mode->width;
			settings->DesktopHeight = target_mode->height;
			rdpPeer->peer->update->DesktopResize(rdpPeer->peer->context);
		}
	}
	return 0;
}

static int
parse_extra_modes(const char *modes_str, struct rdp_output *output) {
	const char *startAt = modes_str;
	const char *nextPos;
	int w, h;
	struct weston_mode *mode;

	while(startAt && *startAt) {
		nextPos = strchr(startAt, 'x');
		if(!nextPos)
			return -1;

		w = strtoul(startAt, NULL, 0);
		startAt = nextPos + 1;
		if(!*startAt)
			return -1;

		h = strtoul(startAt, NULL, 0);

		if(!w || (w > 3000) || !h || (h > 3000))
			return -1;
		mode = malloc(sizeof *mode);
		if(!mode)
			return -1;

		mode->width = w;
		mode->height = h;
		mode->refresh = 5;
		mode->flags = 0;
		wl_list_insert(&output->base.mode_list, &mode->link);

		startAt = strchr(startAt, ',');
		if(startAt && *startAt == ',')
			startAt++;
	}
	return 0;
}
static int
rdp_compositor_create_output(struct rdp_compositor *c, int width, int height,
		const char *extraModes)
{
	struct rdp_output *output;
	struct wl_event_loop *loop;
	struct weston_mode *currentMode, *next;

	output = malloc(sizeof *output);
	if (output == NULL)
		return -1;
	memset(output, 0, sizeof *output);

	wl_list_init(&output->peers);
	wl_list_init(&output->base.mode_list);

	currentMode = malloc(sizeof *currentMode);
	if(!currentMode)
		goto out_free_output;
	currentMode->flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	currentMode->width = width;
	currentMode->height = height;
	currentMode->refresh = 5;
	wl_list_insert(&output->base.mode_list, &currentMode->link);

	if(parse_extra_modes(extraModes, output) < 0) {
		weston_log("invalid extra modes\n");
		goto out_free_output_and_modes;
	}

	output->base.current = currentMode;
	weston_output_init(&output->base, &c->base, 0, 0, width, height,
			WL_OUTPUT_TRANSFORM_NORMAL);

	output->base.make = "weston";
	output->base.model = "rdp";
	output->shadow_surface = pixman_image_create_bits(PIXMAN_x8r8g8b8,
			width, height,
		    NULL,
		    width * 4);
	if (output->shadow_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		goto out_output;
	}

	if (pixman_renderer_output_create(&output->base) < 0)
		goto out_shadow_surface;

	weston_output_move(&output->base, 0, 0);

	loop = wl_display_get_event_loop(c->base.wl_display);
	output->finish_frame_timer = wl_event_loop_add_timer(loop, finish_frame_handler, output);

	output->base.origin = output->base.current;
	output->base.repaint = rdp_output_repaint;
	output->base.destroy = rdp_output_destroy;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = rdp_switch_mode;
	c->output = output;

	wl_list_insert(c->base.output_list.prev, &output->base.link);
	return 0;

out_shadow_surface:
	pixman_image_unref(output->shadow_surface);
out_output:
	weston_output_destroy(&output->base);
out_free_output_and_modes:
	wl_list_for_each_safe(currentMode, next, &output->base.mode_list, link)
		free(currentMode);
out_free_output:
	free(output);
	return -1;
}

static void
rdp_restore(struct weston_compositor *ec)
{
}

static void
rdp_destroy(struct weston_compositor *ec)
{
	struct rdp_compositor *c = (struct rdp_compositor *) ec;

	weston_seat_release(&c->main_seat);

	ec->renderer->destroy(ec);
	weston_compositor_shutdown(ec);

	free(ec);
}

static
int rdp_listener_activity(int fd, uint32_t mask, void *data) {
	freerdp_listener* instance = (freerdp_listener *)data;

	if (!(mask & WL_EVENT_READABLE))
		return 0;
	if (!instance->CheckFileDescriptor(instance))
	{
		weston_log("failed to check FreeRDP file descriptor\n");
		return -1;
	}
	return 0;
}

static
int rdp_implant_listener(struct rdp_compositor *c, freerdp_listener* instance) {
	int i, fd;
	int rcount = 0;
	void* rfds[32];
	struct wl_event_loop *loop;

	if (!instance->GetFileDescriptor(instance, rfds, &rcount)) {
		weston_log("Failed to get FreeRDP file descriptor\n");
		return -1;
	}

	loop = wl_display_get_event_loop(c->base.wl_display);
	for (i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);
		c->listener_events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				rdp_listener_activity, instance);
	}

	for( ; i < 32; i++)
		c->listener_events[i] = 0;
	return 0;
}


static void
rdp_peer_context_new(freerdp_peer* client, RdpPeerContext* context)
{
	context->item.peer = client;
	context->item.flags = 0;

	context->rfx_context = rfx_context_new();
	context->rfx_context->mode = RLGR3;
	context->rfx_context->width = client->settings->DesktopWidth;
	context->rfx_context->height = client->settings->DesktopHeight;
	rfx_context_set_pixel_format(context->rfx_context, RDP_PIXEL_FORMAT_B8G8R8A8);

	context->nsc_context = nsc_context_new();
	rfx_context_set_pixel_format(context->rfx_context, RDP_PIXEL_FORMAT_B8G8R8A8);

	context->encode_stream = stream_new(65536);
}

static void
rdp_peer_context_free(freerdp_peer* client, RdpPeerContext* context)
{
	int i;
	if(!context)
		return;

	wl_list_remove(&context->item.link);
	for(i = 0; i < 32; i++) {
		if(context->fds[i] != -1)
			wl_event_source_remove(context->events[i]);
	}

	if(context->item.flags & RDP_PEER_ACTIVATED)
		weston_seat_release(&context->item.seat);
	stream_free(context->encode_stream);
	nsc_context_free(context->nsc_context);
	rfx_context_free(context->rfx_context);
	free(context->rfx_rects);
}


static int
rdp_client_activity(int fd, uint32_t mask, void *data) {
	freerdp_peer* client = (freerdp_peer *)data;

	if (mask & WL_EVENT_HANGUP) {
		weston_log("connection closed with %p\n", client);
		goto out_clean;
	}

	if (!client->CheckFileDescriptor(client)) {
		weston_log("unable to checkDescriptor for %p\n", client);
		goto out_clean;
	}
	return 0;

out_clean:
	freerdp_peer_context_free(client);
	freerdp_peer_free(client);
	return 0;
}

static BOOL
xf_peer_capabilities(freerdp_peer* client)
{
	return TRUE;
}

struct rdp_to_xkb_keyboard_layout {
	UINT32	rdpLayoutCode;
	char 	*xkbLayout;
};

/* picked from http://technet.microsoft.com/en-us/library/cc766503(WS.10).aspx */
static struct rdp_to_xkb_keyboard_layout rdp_keyboards[] = {
	{0x00000406, "dk"},
	{0x00000407, "de"},
	{0x00000409, "us"},
	{0x0000040c, "fr"},
	{0x00000410, "it"},
	{0x00000813, "be"},
	{0x00000000, 0},
};

/* taken from 2.2.7.1.6 Input Capability Set (TS_INPUT_CAPABILITYSET) */
static char *rdp_keyboard_types[] = {
	"",		/* 0: unused */
	"", 	/* 1: IBM PC/XT or compatible (83-key) keyboard */
	"", 	/* 2: Olivetti "ICO" (102-key) keyboard */
	"", 	/* 3: IBM PC/AT (84-key) or similar keyboard */
	"pc102",/* 4: IBM enhanced (101- or 102-key) keyboard */
	"", 	/* 5: Nokia 1050 and similar keyboards */
	"",		/* 6: Nokia 9140 and similar keyboards */
	""		/* 7: Japanese keyboard */
};

static BOOL
xf_peer_post_connect(freerdp_peer* client)
{
	RdpPeerContext *peerCtx;
	struct rdp_compositor *c;
	struct rdp_output *output;
	rdpSettings *settings;
	struct xkb_context *xkbContext;
	struct xkb_rule_names xkbRuleNames;
	struct xkb_keymap *keymap;
	int i;


	peerCtx = (RdpPeerContext *)client->context;
	c = peerCtx->rdpCompositor;
	output = c->output;
	settings = client->settings;

	if(!settings->SurfaceCommandsEnabled) {
		weston_log("client doesn't support required SurfaceCommands\n");
		return FALSE;
	}

	if(output->base.width != (int)settings->DesktopWidth ||
			output->base.height != (int)settings->DesktopHeight)
	{
		if(!settings->DesktopResize) {
			weston_log("client don't support desktopResize()\n");
			return FALSE;
		}

		/* force the client size */
		settings->DesktopWidth = output->base.width;
		settings->DesktopHeight = output->base.height;
		client->update->DesktopResize(client->context);
	}

	weston_log("kbd_layout:%x kbd_type:%x kbd_subType:%x kbd_functionKeys:%x\n",
			settings->KeyboardLayout, settings->KeyboardType, settings->KeyboardSubType,
			settings->KeyboardFunctionKey);

	memset(&xkbRuleNames, 0, sizeof(xkbRuleNames));
	if(settings->KeyboardType <= 7)
		xkbRuleNames.model = rdp_keyboard_types[settings->KeyboardType];
	for(i = 0; rdp_keyboards[i].xkbLayout; i++) {
		if(rdp_keyboards[i].rdpLayoutCode == settings->KeyboardLayout) {
			xkbRuleNames.layout = rdp_keyboards[i].xkbLayout;
			break;
		}
	}

	keymap = NULL;
	if(xkbRuleNames.layout) {
		xkbContext = xkb_context_new(0);
		if(!xkbContext) {
			weston_log("unable to create a xkb_context\n");
			return FALSE;
		}

		keymap = xkb_keymap_new_from_names(xkbContext, &xkbRuleNames, 0);
	}
	weston_seat_init_keyboard(&peerCtx->item.seat, keymap);
	weston_seat_init_pointer(&peerCtx->item.seat);

	/* initialize with pointer initial position usually (100,100) */
	peerCtx->prev_x = wl_fixed_to_int(peerCtx->item.seat.pointer.x);
	peerCtx->prev_y = wl_fixed_to_int(peerCtx->item.seat.pointer.y);
	peerCtx->item.flags |= RDP_PEER_ACTIVATED;
	return TRUE;
}

static BOOL
xf_peer_activate(freerdp_peer* client)
{
	//weston_log("peer activated\n");
	return TRUE;
}

static void
xf_mouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y) {
	wl_fixed_t wl_x, wl_y;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct rdp_output *output;
	uint32_t button = 0;

	if (flags & PTR_FLAGS_MOVE) {
		output = peerContext->rdpCompositor->output;
		if(x < output->base.width && y < output->base.height) {
			wl_x = wl_fixed_from_int((int)x - peerContext->prev_x);
			wl_y = wl_fixed_from_int((int)y - peerContext->prev_y);
			peerContext->prev_x = x;
			peerContext->prev_y = y;
			notify_motion(&peerContext->item.seat, weston_compositor_get_time(), wl_x, wl_y);
		}
	}

	if (flags & PTR_FLAGS_BUTTON1)
		button = BTN_LEFT;
	else if (flags & PTR_FLAGS_BUTTON2)
		button = BTN_RIGHT;
	else if (flags & PTR_FLAGS_BUTTON3)
		button = BTN_MIDDLE;

	if(button) {
		notify_button(&peerContext->item.seat, weston_compositor_get_time(), button,
			(flags & PTR_FLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
		);
	}
}

static void
xf_extendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y) {
	wl_fixed_t wl_x, wl_y;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct rdp_output *output;

	output = peerContext->rdpCompositor->output;
	if(x < output->base.width && y < output->base.height) {
		wl_x = wl_fixed_from_int((int)x - peerContext->prev_x);
		wl_y = wl_fixed_from_int((int)y - peerContext->prev_y);
		peerContext->prev_x = x;
		peerContext->prev_y = y;
		notify_motion(&peerContext->item.seat, weston_compositor_get_time(), wl_x, wl_y);
	}
}


static void
xf_input_synchronize_event(rdpInput* input, UINT32 flags)
{
	freerdp_peer* client = input->context->peer;
	rdpPointerUpdate *pointer = client->update->pointer;
	RdpPeerContext *peerCtx = (RdpPeerContext *)input->context;
	struct rdp_output *output = peerCtx->rdpCompositor->output;
	pixman_box32_t box;
	pixman_region32_t damage;

	/* disable pointer on the client side */
	pointer->pointer_system.type = SYSPTR_NULL;
	pointer->PointerSystem(client->context, &pointer->pointer_system);

	/* sends a full refresh */
	box.x1 = 0;
	box.y1 = 0;
	box.x2 = output->base.width;
	box.y2 = output->base.height;
	pixman_region32_init_with_extents(&damage, &box);

	rdp_peer_refresh_region(&damage, client);

	pixman_region32_fini(&damage);
}

static void
xf_input_keyboard_event(rdpInput* input, UINT16 flags, UINT16 code)
{
	enum wl_keyboard_key_state keyState;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	int notify = 0;

	if (flags & KBD_FLAGS_DOWN) {
		keyState = WL_KEYBOARD_KEY_STATE_PRESSED;
		notify = 1;
	} else if (flags & KBD_FLAGS_RELEASE) {
		keyState = WL_KEYBOARD_KEY_STATE_RELEASED;
		notify = 1;
	}

	if(notify)
		notify_key(&peerContext->item.seat, weston_compositor_get_time(),
					code, keyState,	STATE_UPDATE_AUTOMATIC);
}

static void
xf_input_unicode_keyboard_event(rdpInput* input, UINT16 flags, UINT16 code)
{
	weston_log("Client sent a unicode keyboard event (flags:0x%X code:0x%X)\n", flags, code);
}

void update_register_server_callbacks(rdpUpdate* update);

static void
xf_suppress_output(rdpContext* context, BYTE allow, RECTANGLE_16* area) {
	RdpPeerContext *peerContext = (RdpPeerContext *)context;
	if(allow)
		peerContext->item.flags |= RDP_PEER_OUTPUT_ENABLED;
	else
		peerContext->item.flags &= (~RDP_PEER_OUTPUT_ENABLED);
}

static int
rdp_peer_init(freerdp_peer* client, struct rdp_compositor *c)
{
	int rcount = 0;
	void *rfds[32];
	int i, fd;
	struct wl_event_loop *loop;
	rdpSettings	*settings;
	rdpInput *input;
	RdpPeerContext	*peerCtx;

	client->context_size = sizeof(RdpPeerContext);
	client->ContextNew = (psPeerContextNew)rdp_peer_context_new;
	client->ContextFree = (psPeerContextFree)rdp_peer_context_free;
	freerdp_peer_context_new(client);

	peerCtx = (RdpPeerContext *) client->context;
	peerCtx->rdpCompositor = c;

	settings = client->settings;
	settings->RdpKeyFile = c->rdp_key;
	if(c->tls_enabled) {
		settings->CertificateFile = c->server_cert;
		settings->PrivateKeyFile = c->server_key;
	} else {
		settings->TlsSecurity = FALSE;
	}

	settings->NlaSecurity = FALSE;

	client->Capabilities = xf_peer_capabilities;
	client->PostConnect = xf_peer_post_connect;
	client->Activate = xf_peer_activate;

	client->update->SuppressOutput = xf_suppress_output;
	update_register_server_callbacks(client->update);

	input = client->input;
	input->SynchronizeEvent = xf_input_synchronize_event;
	input->MouseEvent = xf_mouseEvent;
	input->ExtendedMouseEvent = xf_extendedMouseEvent;
	input->KeyboardEvent = xf_input_keyboard_event;
	input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;
	weston_seat_init(&peerCtx->item.seat, &c->base);

	client->Initialize(client);

	if (!client->GetFileDescriptor(client, rfds, &rcount)) {
		weston_log("unable to retrieve client fds\n");
		return -1;
	}

	loop = wl_display_get_event_loop(c->base.wl_display);
	for(i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);

		peerCtx->fds[i] = fd;
		peerCtx->events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP,
				rdp_client_activity, client);
	}
	for( ; i < 32; i++) {
		peerCtx->fds[i] = -1;
		peerCtx->events[i] = 0;
	}

	wl_list_insert(&c->output->peers, &peerCtx->item.link);
	return 0;
}


static void
rdp_incoming_peer(freerdp_listener* instance, freerdp_peer* client)
{
	struct rdp_compositor *c = (struct rdp_compositor *)instance->param4;
	if (rdp_peer_init(client, c) < 0)
		return;
}

static struct weston_compositor *
rdp_compositor_create(struct wl_display *display,
		struct rdp_compositor_config *config,
		int *argc, char *argv[], const char *config_file)
{
	struct rdp_compositor *c;
	char *fd_str;
	int fd;

	c = malloc(sizeof *c);
	if (c == NULL)
		return NULL;

	memset(c, 0, sizeof *c);

	if (weston_compositor_init(&c->base, display, argc, argv,
				   config_file) < 0)
		goto err_free;

	weston_seat_init(&c->main_seat, &c->base);
	c->base.destroy = rdp_destroy;
	c->base.restore = rdp_restore;
	c->rdp_key = config->rdp_key ? strdup(config->rdp_key) : NULL;

	/* activate TLS only if certificate/key are available */
	if(config->server_cert && config->server_key) {
		weston_log("TLS support activated\n");
		c->server_cert = strdup(config->server_cert);
		c->server_key = strdup(config->server_key);
		if(!c->server_cert || !c->server_key)
			goto err_free_strings;
		c->tls_enabled = 1;
	}

	if (pixman_renderer_init(&c->base) < 0)
		goto err_compositor;

	if (rdp_compositor_create_output(c, config->width, config->height, config->extra_modes) < 0)
		goto err_compositor;

	if(!config->env_socket) {
		c->listener = freerdp_listener_new();
		c->listener->PeerAccepted = rdp_incoming_peer;
		c->listener->param4 = c;
		if(!c->listener->Open(c->listener, config->bind_address, config->port)) {
			weston_log("unable to bind rdp socket\n");
			goto err_listener;
		}

		if (rdp_implant_listener(c, c->listener) < 0)
			goto err_compositor;
	} else {
		/* get the socket from RDP_FD var */
		fd_str = getenv("RDP_FD");
		if(!fd_str) {
			weston_log("RDP_FD env variable not set");
			goto err_output;
		}

		fd = strtoul(fd_str, NULL, 10);
		if(rdp_peer_init(freerdp_peer_new(fd), c))
			goto err_output;
	}

	return &c->base;

err_listener:
	freerdp_listener_free(c->listener);
err_output:
	weston_output_destroy(&c->output->base);
err_compositor:
	weston_compositor_shutdown(&c->base);
err_free_strings:
	if(c->rdp_key)
		free(c->rdp_key);
	if(c->server_cert)
		free(c->server_cert);
	if(c->server_key)
		free(c->server_key);
	weston_seat_release(&c->main_seat);
err_free:
	free(c);
	return NULL;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int *argc, char *argv[],
	     const char *config_file)
{
	struct rdp_compositor_config config;
	rdp_compositor_config_init(&config);
	int major, minor, revision;

	freerdp_get_version(&major, &minor, &revision);
	weston_log("using FreeRDP version %d.%d.%d\n", major, minor, revision);

	const struct weston_option rdp_options[] = {
		{ WESTON_OPTION_BOOLEAN, "env-socket", 0, &config.env_socket },
		{ WESTON_OPTION_INTEGER, "width", 0, &config.width },
		{ WESTON_OPTION_INTEGER, "height", 0, &config.height },
		{ WESTON_OPTION_STRING,  "extra-modes", 0, &config.extra_modes },
		{ WESTON_OPTION_STRING,  "address", 0, &config.bind_address },
		{ WESTON_OPTION_INTEGER, "port", 0, &config.port },
		{ WESTON_OPTION_STRING,  "rdp-key", 0, &config.rdp_key },
		{ WESTON_OPTION_STRING,  "server-cert", 0, &config.server_cert },
		{ WESTON_OPTION_STRING,  "server-key", 0, &config.server_key }
	};

	parse_options(rdp_options, ARRAY_LENGTH(rdp_options), argc, argv);
	return rdp_compositor_create(display, &config, argc, argv, config_file);
}
