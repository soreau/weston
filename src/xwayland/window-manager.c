/*
 * Copyright © 2011 Intel Corporation
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

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <X11/Xcursor/Xcursor.h>

#include "xwayland.h"

#include "../../shared/cairo-util.h"
#include "../compositor.h"
#include "xserver-server-protocol.h"
#include "hash.h"

struct motif_wm_hints {
	uint32_t flags;
	uint32_t functions;
	uint32_t decorations;
	int32_t input_mode;
	uint32_t status;
};

#define MWM_HINTS_FUNCTIONS     (1L << 0)
#define MWM_HINTS_DECORATIONS   (1L << 1)
#define MWM_HINTS_INPUT_MODE    (1L << 2)
#define MWM_HINTS_STATUS        (1L << 3)

#define MWM_FUNC_ALL            (1L << 0)
#define MWM_FUNC_RESIZE         (1L << 1)
#define MWM_FUNC_MOVE           (1L << 2)
#define MWM_FUNC_MINIMIZE       (1L << 3)
#define MWM_FUNC_MAXIMIZE       (1L << 4)
#define MWM_FUNC_CLOSE          (1L << 5)

#define MWM_DECOR_ALL           (1L << 0)
#define MWM_DECOR_BORDER        (1L << 1)
#define MWM_DECOR_RESIZEH       (1L << 2)
#define MWM_DECOR_TITLE         (1L << 3)
#define MWM_DECOR_MENU          (1L << 4)
#define MWM_DECOR_MINIMIZE      (1L << 5)
#define MWM_DECOR_MAXIMIZE      (1L << 6)

#define MWM_INPUT_MODELESS 0
#define MWM_INPUT_PRIMARY_APPLICATION_MODAL 1
#define MWM_INPUT_SYSTEM_MODAL 2
#define MWM_INPUT_FULL_APPLICATION_MODAL 3
#define MWM_INPUT_APPLICATION_MODAL MWM_INPUT_PRIMARY_APPLICATION_MODAL

#define MWM_TEAROFF_WINDOW      (1L<<0)

#define _NET_WM_MOVERESIZE_SIZE_TOPLEFT      0
#define _NET_WM_MOVERESIZE_SIZE_TOP          1
#define _NET_WM_MOVERESIZE_SIZE_TOPRIGHT     2
#define _NET_WM_MOVERESIZE_SIZE_RIGHT        3
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT  4
#define _NET_WM_MOVERESIZE_SIZE_BOTTOM       5
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT   6
#define _NET_WM_MOVERESIZE_SIZE_LEFT         7
#define _NET_WM_MOVERESIZE_MOVE              8   /* movement only */
#define _NET_WM_MOVERESIZE_SIZE_KEYBOARD     9   /* size via keyboard */
#define _NET_WM_MOVERESIZE_MOVE_KEYBOARD    10   /* move via keyboard */
#define _NET_WM_MOVERESIZE_CANCEL           11   /* cancel operation */

struct weston_wm_window {
	struct weston_wm *wm;
	xcb_window_t id;
	xcb_window_t frame_id;
	cairo_surface_t *cairo_surface;
	struct weston_surface *surface;
	struct shell_surface *shsurf;
	struct wl_listener surface_destroy_listener;
	struct wl_event_source *repaint_source;
	struct wl_event_source *configure_source;
	int properties_dirty;
	int pid;
	char *machine;
	char *class;
	char *name;
	struct weston_wm_window *transient_for;
	uint32_t protocols;
	xcb_atom_t type;
	int width, height;
	int x, y;
	int saved_width, saved_height;
	int saved_size_valid;
	int decorate;
	int override_redirect;
	int fullscreen;
	int maximized;
};

static struct weston_wm_window *
get_wm_window(struct weston_surface *surface);

static void
weston_wm_window_schedule_repaint(struct weston_wm_window *window);

static void
weston_wm_window_configure(void *data);

static void
send_maximize(struct weston_surface *surface);

const char *
get_atom_name(xcb_connection_t *c, xcb_atom_t atom)
{
	xcb_get_atom_name_cookie_t cookie;
	xcb_get_atom_name_reply_t *reply;
	xcb_generic_error_t *e;
	static char buffer[64];

	if (atom == XCB_ATOM_NONE)
		return "None";

	cookie = xcb_get_atom_name (c, atom);
	reply = xcb_get_atom_name_reply (c, cookie, &e);
	snprintf(buffer, sizeof buffer, "%.*s",
		 xcb_get_atom_name_name_length (reply),
		 xcb_get_atom_name_name (reply));
	free(reply);

	return buffer;
}

static xcb_cursor_t
xcb_cursor_image_load_cursor(struct weston_wm *wm, const XcursorImage *img)
{
	xcb_connection_t *c = wm->conn;
	xcb_screen_iterator_t s = xcb_setup_roots_iterator(xcb_get_setup(c));
	xcb_screen_t *screen = s.data;
	xcb_gcontext_t gc;
	xcb_pixmap_t pix;
	xcb_render_picture_t pic;
	xcb_cursor_t cursor;
	int stride = img->width * 4;

	pix = xcb_generate_id(c);
	xcb_create_pixmap(c, 32, pix, screen->root, img->width, img->height);

	pic = xcb_generate_id(c);
	xcb_render_create_picture(c, pic, pix, wm->format_rgba.id, 0, 0);

	gc = xcb_generate_id(c);
	xcb_create_gc(c, gc, pix, 0, 0);

	xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc,
		      img->width, img->height, 0, 0, 0, 32,
		      stride * img->height, (uint8_t *) img->pixels);
	xcb_free_gc(c, gc);

	cursor = xcb_generate_id(c);
	xcb_render_create_cursor(c, cursor, pic, img->xhot, img->yhot);

	xcb_render_free_picture(c, pic);
	xcb_free_pixmap(c, pix);

	return cursor;
}

static xcb_cursor_t
xcb_cursor_images_load_cursor(struct weston_wm *wm, const XcursorImages *images)
{
	/* TODO: treat animated cursors as well */
	if (images->nimage != 1)
		return -1;

	return xcb_cursor_image_load_cursor(wm, images->images[0]);
}

static xcb_cursor_t
xcb_cursor_library_load_cursor(struct weston_wm *wm, const char *file)
{
	xcb_cursor_t cursor;
	XcursorImages *images;
	char *v = NULL;
	int size = 0;

	if (!file)
		return 0;

	v = getenv ("XCURSOR_SIZE");
	if (v)
		size = atoi(v);

	if (!size)
		size = 32;

	images = XcursorLibraryLoadImages (file, NULL, size);
	if (!images)
		return -1;

	cursor = xcb_cursor_images_load_cursor (wm, images);
	XcursorImagesDestroy (images);

	return cursor;
}

void
dump_property(struct weston_wm *wm,
	      xcb_atom_t property, xcb_get_property_reply_t *reply)
{
	int32_t *incr_value;
	const char *text_value, *name;
	xcb_atom_t *atom_value;
	int width, len;
	uint32_t i;

	width = weston_log_continue("%s: ", get_atom_name(wm->conn, property));
	if (reply == NULL) {
		weston_log_continue("(no reply)\n");
		return;
	}

	width += weston_log_continue(
			 "%s/%d, length %d (value_len %d): ",
			 get_atom_name(wm->conn, reply->type),
			 reply->format,
			 xcb_get_property_value_length(reply),
			 reply->value_len);

	if (reply->type == wm->atom.incr) {
		incr_value = xcb_get_property_value(reply);
		weston_log_continue("%d\n", *incr_value);
	} else if (reply->type == wm->atom.utf8_string ||
	      reply->type == wm->atom.string) {
		text_value = xcb_get_property_value(reply);
		if (reply->value_len > 40)
			len = 40;
		else
			len = reply->value_len;
		weston_log_continue("\"%.*s\"\n", len, text_value);
	} else if (reply->type == XCB_ATOM_ATOM) {
		atom_value = xcb_get_property_value(reply);
		for (i = 0; i < reply->value_len; i++) {
			name = get_atom_name(wm->conn, atom_value[i]);
			if (width + strlen(name) + 2 > 78) {
				weston_log_continue("\n    ");
				width = 4;
			} else if (i > 0) {
				width +=  weston_log_continue(", ");
			}

			width +=  weston_log_continue("%s", name);
		}
		weston_log_continue("\n");
	} else {
		weston_log_continue("huh?\n");
	}
}

static void
read_and_dump_property(struct weston_wm *wm,
		       xcb_window_t window, xcb_atom_t property)
{
	xcb_get_property_reply_t *reply;
	xcb_get_property_cookie_t cookie;

	cookie = xcb_get_property(wm->conn, 0, window,
				  property, XCB_ATOM_ANY, 0, 2048);
	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	dump_property(wm, property, reply);

	free(reply);
}

/* We reuse some predefined, but otherwise useles atoms */
#define TYPE_WM_PROTOCOLS	XCB_ATOM_CUT_BUFFER0
#define TYPE_MOTIF_WM_HINTS	XCB_ATOM_CUT_BUFFER1
#define TYPE_NET_WM_STATE	XCB_ATOM_CUT_BUFFER2

static void
weston_wm_window_read_properties(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;

#define F(field) offsetof(struct weston_wm_window, field)
	const struct {
		xcb_atom_t atom;
		xcb_atom_t type;
		int offset;
	} props[] = {
		{ XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, F(class) },
		{ XCB_ATOM_WM_NAME, XCB_ATOM_STRING, F(name) },
		{ XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, F(transient_for) },
		{ wm->atom.wm_protocols, TYPE_WM_PROTOCOLS, F(protocols) },
		{ wm->atom.net_wm_state, TYPE_NET_WM_STATE },
		{ wm->atom.net_wm_window_type, XCB_ATOM_ATOM, F(type) },
		{ wm->atom.net_wm_name, XCB_ATOM_STRING, F(name) },
		{ wm->atom.net_wm_pid, XCB_ATOM_CARDINAL, F(pid) },
		{ wm->atom.motif_wm_hints, TYPE_MOTIF_WM_HINTS, 0 },
		{ wm->atom.wm_client_machine, XCB_ATOM_WM_CLIENT_MACHINE, F(machine) },
	};
#undef F

	xcb_get_property_cookie_t cookie[ARRAY_LENGTH(props)];
	xcb_get_property_reply_t *reply;
	void *p;
	uint32_t *xid;
	xcb_atom_t *atom;
	uint32_t i;
	struct motif_wm_hints *hints;
	int prev_decorate; /* used to tell if the window->decorate property has changed since last*/

	if (!window->properties_dirty)
		return;
	window->properties_dirty = 0;

	for (i = 0; i < ARRAY_LENGTH(props); i++)
		cookie[i] = xcb_get_property(wm->conn,
					     0, /* delete */
					     window->id,
					     props[i].atom,
					     XCB_ATOM_ANY, 0, 2048);

	prev_decorate = window->decorate;
	window->decorate = !window->override_redirect;
	for (i = 0; i < ARRAY_LENGTH(props); i++)  {
		reply = xcb_get_property_reply(wm->conn, cookie[i], NULL);
		if (!reply)
			/* Bad window, typically */
			continue;
		if (reply->type == XCB_ATOM_NONE) {
			/* No such property */
			free(reply);
			continue;
		}

		p = ((char *) window + props[i].offset);

		switch (props[i].type) {
		case XCB_ATOM_WM_CLIENT_MACHINE:
		case XCB_ATOM_STRING:
			/* FIXME: We're using this for both string and
			   utf8_string */
			if (*(char **) p)
				free(*(char **) p);

			*(char **) p =
				strndup(xcb_get_property_value(reply),
					xcb_get_property_value_length(reply));
			break;
		case XCB_ATOM_WINDOW:
			xid = xcb_get_property_value(reply);
			*(struct weston_wm_window **) p =
				hash_table_lookup(wm->window_hash, *xid);
			break;
		case XCB_ATOM_CARDINAL:
		case XCB_ATOM_ATOM:
			atom = xcb_get_property_value(reply);
			*(xcb_atom_t *) p = *atom;
			break;
		case TYPE_WM_PROTOCOLS:
			break;
		case TYPE_NET_WM_STATE:
			window->fullscreen = 0;
			atom = xcb_get_property_value(reply);
			for (i = 0; i < reply->value_len; i++) {
				if (atom[i] == wm->atom.net_wm_state_fullscreen)
					window->fullscreen = 1;
				else if (atom[i] == wm->atom.net_wm_state_maximized_vert ||
					atom[i] == wm->atom.net_wm_state_maximized_horz)
					window->maximized = 1;
			}
			break;
		case TYPE_MOTIF_WM_HINTS:
			hints = xcb_get_property_value(reply);
			if (hints->flags & MWM_HINTS_DECORATIONS)
				window->decorate = hints->decorations > 0;
			if (prev_decorate != window->decorate) {
				/* if whether or not we draw decorations has changed, send a configure
					request to the window to make it redraw itself with everything in order */
				weston_wm_window_configure(window);
				if (window->maximized) {
					/* if the window was already maximized, we need to do it again since the
						actual size of the window changes when borders/title bar change */
					window->maximized = 0;
					send_maximize(window->surface);
				}
			}
			break;
		default:
			break;
		}
		free(reply);
	}
}

static void
weston_wm_window_get_frame_size(struct weston_wm_window *window,
				int *width, int *height)
{
	struct theme *t = window->wm->theme;

	if (window->fullscreen) {
		*width = window->width;
		*height = window->height;
	} else if (window->decorate && !window->maximized) {
		*width = window->width + (t->margin + t->width) * 2;
		*height = window->height +
			t->margin * 2 + t->width + t->titlebar_height;
	} else if (window->decorate && window->maximized) {
		*width = window->width + t->width * 2;
		*height = window->height + t->margin;
	} else if (!window->decorate && window->maximized) {
		*width = window->width;
		*height = window->height;
	} else {
		*width = window->width + t->margin * 2;
		*height = window->height + t->margin * 2;
	}
}

static void
weston_wm_window_get_child_position(struct weston_wm_window *window,
				    int *x, int *y)
{
	struct theme *t = window->wm->theme;

	if (window->fullscreen) {
		*x = 0;
		*y = 0;
	} else if (window->decorate && !window->maximized) {
		*x = t->margin + t->width;
		*y = t->margin + t->titlebar_height;
	} else if (window->decorate && window->maximized) {
		*x = t->width;
		*y = t->titlebar_height;
	} else if (!window->decorate && window->maximized) {
		*x = 0;
		*y = 0;
	} else {
		*x = t->margin;
		*y = t->margin;
	}
}

static void
weston_wm_window_send_configure_notify(struct weston_wm_window *window)
{
	xcb_configure_notify_event_t configure_notify;
	struct weston_wm *wm = window->wm;
	int x, y;

	weston_wm_window_get_child_position(window, &x, &y);
	configure_notify.response_type = XCB_CONFIGURE_NOTIFY;
	configure_notify.pad0 = 0;
	configure_notify.event = window->id;
	configure_notify.window = window->id;
	configure_notify.above_sibling = XCB_WINDOW_NONE;
	configure_notify.x = x;
	configure_notify.y = y;
	configure_notify.width = window->width;
	configure_notify.height = window->height;
	configure_notify.border_width = 0;
	configure_notify.override_redirect = 0;
	configure_notify.pad1 = 0;

	xcb_send_event(wm->conn, 0, window->id, 
		       XCB_EVENT_MASK_STRUCTURE_NOTIFY,
		       (char *) &configure_notify);
}

static void
weston_wm_handle_configure_request(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_configure_request_event_t *configure_request = 
		(xcb_configure_request_event_t *) event;
	struct weston_wm_window *window;
	uint32_t mask, values[16];
	int x, y, width, height, i = 0;

	weston_log("XCB_CONFIGURE_REQUEST (window %d) %d,%d @ %dx%d\n",
		configure_request->window,
		configure_request->x, configure_request->y,
		configure_request->width, configure_request->height);

	window = hash_table_lookup(wm->window_hash, configure_request->window);

	if (window->fullscreen || window->maximized) {
		weston_wm_window_send_configure_notify(window);
		return;
	}

	if (configure_request->value_mask & XCB_CONFIG_WINDOW_WIDTH)
		window->width = configure_request->width;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
		window->height = configure_request->height;

	weston_wm_window_get_child_position(window, &x, &y);
	values[i++] = x;
	values[i++] = y;
	values[i++] = window->width;
	values[i++] = window->height;
	values[i++] = 0;
	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		XCB_CONFIG_WINDOW_BORDER_WIDTH;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
		values[i++] = configure_request->sibling;
		mask |= XCB_CONFIG_WINDOW_SIBLING;
	}
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
		values[i++] = configure_request->stack_mode;
		mask |= XCB_CONFIG_WINDOW_STACK_MODE;
	}

	xcb_configure_window(wm->conn, window->id, mask, values);

	weston_wm_window_get_frame_size(window, &width, &height);
	values[0] = width;
	values[1] = height;
	mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	xcb_configure_window(wm->conn, window->frame_id, mask, values);

	weston_wm_window_schedule_repaint(window);
}

static void
weston_wm_handle_configure_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_configure_notify_event_t *configure_notify = 
		(xcb_configure_notify_event_t *) event;
	struct weston_wm_window *window;
	int x, y;

	window = hash_table_lookup(wm->window_hash, configure_notify->window);

	weston_log("XCB_CONFIGURE_NOTIFY (%s window %d) %d,%d @ %dx%d\n",
		configure_notify->window == window->id ? "client" : "frame",
		configure_notify->window,
		configure_notify->x, configure_notify->y,
		configure_notify->width, configure_notify->height);

	/* resize falls here */
	if (configure_notify->window != window->id)
		return;

	weston_wm_window_get_child_position(window, &x, &y);
	window->x = configure_notify->x - x;
	window->y = configure_notify->y - y;
}

static void
weston_wm_kill_client(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct weston_wm_window *window = get_wm_window(surface);
	char name[1024];

	if (!window)
		return;

	gethostname(name, 1024);

	/* this is only one heuristic to guess the PID of a client is valid,
	 * assuming it's compliant with icccm and ewmh. Non-compliants and
	 * remote applications of course fail. */
	if (!strcmp(window->machine, name) && window->pid != 0)
		kill(window->pid, SIGKILL);
}

static void
weston_wm_window_activate(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct weston_wm_window *window = get_wm_window(surface);
	struct weston_wm *wm =
		container_of(listener, struct weston_wm, activate_listener);
	xcb_client_message_event_t client_message;

	if (window) {
		client_message.response_type = XCB_CLIENT_MESSAGE;
		client_message.format = 32;
		client_message.window = window->id;
		client_message.type = wm->atom.wm_protocols;
		client_message.data.data32[0] = wm->atom.wm_take_focus;
		client_message.data.data32[1] = XCB_TIME_CURRENT_TIME;

		xcb_send_event(wm->conn, 0, window->id, 
			       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
			       (char *) &client_message);

		xcb_set_input_focus (wm->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
				     window->id, XCB_TIME_CURRENT_TIME);
	} else {
		xcb_set_input_focus (wm->conn,
				     XCB_INPUT_FOCUS_POINTER_ROOT,
				     XCB_NONE,
				     XCB_TIME_CURRENT_TIME);
	}

	if (wm->focus_window)
		weston_wm_window_schedule_repaint(wm->focus_window);
	wm->focus_window = window;
	if (window)
		wm->focus_latest = window;
	if (wm->focus_window)
		weston_wm_window_schedule_repaint(wm->focus_window);
}

static int
our_resource(struct weston_wm *wm, uint32_t id)
{
	const xcb_setup_t *setup;

	setup = xcb_get_setup(wm->conn);

	return (id & ~setup->resource_id_mask) == setup->resource_id_base;
}

#define ICCCM_WITHDRAWN_STATE	0
#define ICCCM_NORMAL_STATE	1
#define ICCCM_ICONIC_STATE	3

static void
weston_wm_window_set_wm_state(struct weston_wm_window *window, int32_t state)
{
	struct weston_wm *wm = window->wm;
	uint32_t property[2];

	property[0] = state;
	property[1] = XCB_WINDOW_NONE;

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    window->id,
			    wm->atom.wm_state,
			    wm->atom.wm_state,
			    32, /* format */
			    2, property);
}

static void
weston_wm_window_set_net_wm_state(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;
	uint32_t property[3];
	int i;

	i = 0;
	if (window->fullscreen)
		property[i++] = wm->atom.net_wm_state_fullscreen;
	else if (window->maximized) {
		property[i++] = wm->atom.net_wm_state_maximized_vert;
		property[i++] = wm->atom.net_wm_state_maximized_horz;
	}

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    window->id,
			    wm->atom.net_wm_state,
			    XCB_ATOM_ATOM,
			    32, /* format */
			    i, property);
}

static void
weston_wm_handle_map_request(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_map_request_event_t *map_request =
		(xcb_map_request_event_t *) event;
	struct weston_wm_window *window;
	uint32_t values[1];
	int x, y, width, height;

	if (our_resource(wm, map_request->window)) {
		weston_log("XCB_MAP_REQUEST (window %d, ours)\n",
			map_request->window);
		return;
	}

	window = hash_table_lookup(wm->window_hash, map_request->window);

	if (window->frame_id)
		return;

	weston_wm_window_read_properties(window);

	weston_wm_window_get_frame_size(window, &width, &height);
	weston_wm_window_get_child_position(window, &x, &y);

	values[0] =
		XCB_EVENT_MASK_KEY_PRESS |
		XCB_EVENT_MASK_KEY_RELEASE |
		XCB_EVENT_MASK_BUTTON_PRESS |
		XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_POINTER_MOTION |
		XCB_EVENT_MASK_ENTER_WINDOW |
		XCB_EVENT_MASK_LEAVE_WINDOW |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;

	window->frame_id = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  XCB_COPY_FROM_PARENT,
			  window->frame_id,
			  wm->screen->root,
			  0, 0,
			  width, height,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  wm->screen->root_visual,
			  XCB_CW_EVENT_MASK, values);
	xcb_reparent_window(wm->conn, window->id, window->frame_id, x, y);

	values[0] = 0;
	xcb_configure_window(wm->conn, window->id,
			     XCB_CONFIG_WINDOW_BORDER_WIDTH, values);

	weston_log("XCB_MAP_REQUEST (window %d, %p, frame %d)\n",
		window->id, window, window->frame_id);

	weston_wm_window_set_wm_state(window, ICCCM_NORMAL_STATE);
	weston_wm_window_set_net_wm_state(window);

	xcb_map_window(wm->conn, map_request->window);
	xcb_map_window(wm->conn, window->frame_id);

	window->cairo_surface =
		cairo_xcb_surface_create_with_xrender_format(wm->conn,
							     wm->screen,
							     window->frame_id,
							     &wm->format_rgb,
							     width, height);

	hash_table_insert(wm->window_hash, window->frame_id, window);
}

static void
weston_wm_handle_map_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_map_notify_event_t *map_notify = (xcb_map_notify_event_t *) event;

	if (our_resource(wm, map_notify->window)) {
			weston_log("XCB_MAP_NOTIFY (window %d, ours)\n",
				map_notify->window);
			return;
	}

	weston_log("XCB_MAP_NOTIFY (window %d)\n", map_notify->window);
}

static void
weston_wm_handle_unmap_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_unmap_notify_event_t *unmap_notify =
		(xcb_unmap_notify_event_t *) event;
	struct weston_wm_window *window;

	weston_log("XCB_UNMAP_NOTIFY (window %d, event %d%s)\n",
		unmap_notify->window,
		unmap_notify->event,
		our_resource(wm, unmap_notify->window) ? ", ours" : "");

	if (our_resource(wm, unmap_notify->window))
		return;

	if (unmap_notify->response_type & 0x80)
		/* We just ignore the ICCCM 4.1.4 synthetic unmap notify
		 * as it may come in after we've destroyed the window. */
		return;

	window = hash_table_lookup(wm->window_hash, unmap_notify->window);
	if (window->repaint_source)
		wl_event_source_remove(window->repaint_source);
	if (window->cairo_surface)
		cairo_surface_destroy(window->cairo_surface);

	if (window->frame_id) {
		xcb_reparent_window(wm->conn, window->id, wm->wm_window, 0, 0);
		xcb_destroy_window(wm->conn, window->frame_id);
		weston_wm_window_set_wm_state(window, ICCCM_WITHDRAWN_STATE);
		hash_table_remove(wm->window_hash, window->frame_id);
		window->frame_id = XCB_WINDOW_NONE;
	}

	if (wm->focus_window == window)
		wm->focus_window = NULL;
	if (window->surface)
		wl_list_remove(&window->surface_destroy_listener.link);
	window->surface = NULL;
}

static void
weston_wm_window_draw_decoration(void *data)
{
	struct weston_wm_window *window = data;
	struct weston_wm *wm = window->wm;
	struct theme *t = wm->theme;
	cairo_t *cr;
	int x, y, width, height;
	const char *title;
	uint32_t flags = 0;

	weston_wm_window_read_properties(window);

	window->repaint_source = NULL;

	weston_wm_window_get_frame_size(window, &width, &height);
	weston_wm_window_get_child_position(window, &x, &y);

	cairo_xcb_surface_set_size(window->cairo_surface, width, height);
	cr = cairo_create(window->cairo_surface);

	if (window->fullscreen) {
		/* nothing */
	} else if (!window->decorate && window->maximized) {
		/* nothing */
	} else if (window->decorate && !window->maximized) {
		if (wm->focus_window == window)
			flags |= THEME_FRAME_ACTIVE;

		if (window->name)
			title = window->name;
		else
			title = "untitled";

		theme_render_frame(t, cr, width, height, title, flags);
	} else if (window->decorate && window->maximized) {
		flags = THEME_FRAME_MAXIMIZED;
		if (wm->focus_window == window)
			flags |= THEME_FRAME_ACTIVE;

		if (window->name)
			title = window->name;
		else
			title = "untitled";

		theme_render_frame(t, cr, width, height, title, flags);
	} else {
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_rgba(cr, 0, 0, 0, 0);
		cairo_paint(cr);

		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
		tile_mask(cr, t->shadow, 2, 2, width + 8, height + 8, 64, 64);
	}

	cairo_destroy(cr);

	if (window->surface) {
		pixman_region32_fini(&window->surface->pending.opaque);
		/* We leave an extra pixel around the X window area to
		 * make sure we don't sample from the undefined alpha
		 * channel when filtering. */
		pixman_region32_init_rect(&window->surface->pending.opaque, 
					  x - 1, y - 1,
					  window->width + 2,
					  window->height + 2);
		window->surface->geometry.dirty = 1;
	}

	if (window->surface && !window->fullscreen && !window->maximized) {
		pixman_region32_fini(&window->surface->pending.input);
		pixman_region32_init_rect(&window->surface->pending.input,
					  t->margin, t->margin,
					  width - 2 * t->margin,
					  height - 2 * t->margin);
	} else if (window->maximized) {
		pixman_region32_fini(&window->surface->pending.input);
		pixman_region32_init_rect(&window->surface->pending.input,
					  0, 0, width, height);
	}
}

static void
weston_wm_window_schedule_repaint(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;
	int width, height;

	if (window->frame_id == XCB_WINDOW_NONE) {
		if (window->surface != NULL) {
			weston_wm_window_get_frame_size(window, &width, &height);
			pixman_region32_fini(&window->surface->pending.opaque);
			pixman_region32_init_rect(&window->surface->pending.opaque, 0, 0,
						  width, height);
			window->surface->geometry.dirty = 1;
		}
		return;
	}

	if (window->repaint_source)
		return;

	window->repaint_source =
		wl_event_loop_add_idle(wm->server->loop,
				       weston_wm_window_draw_decoration,
				       window);
}

static void
weston_wm_handle_property_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	struct weston_shell_interface *shell_interface =
		&wm->server->compositor->shell_interface;
	xcb_property_notify_event_t *property_notify =
		(xcb_property_notify_event_t *) event;
	struct weston_wm_window *window;

	window = hash_table_lookup(wm->window_hash, property_notify->window);
	if (!window)
		return;

	window->properties_dirty = 1;

	weston_log("XCB_PROPERTY_NOTIFY: window %d, ",
		property_notify->window);
	if (property_notify->state == XCB_PROPERTY_DELETE)
		weston_log("deleted\n");
	else {
		read_and_dump_property(wm, property_notify->window,
				       property_notify->atom);
		weston_wm_window_read_properties(window);
	}

	if (property_notify->atom == wm->atom.net_wm_name ||
	    property_notify->atom == XCB_ATOM_WM_NAME) {
		weston_wm_window_schedule_repaint(window);
		if (window->shsurf && window->name)
			shell_interface->set_title(window->shsurf, window->name);
	}
}

static void
weston_wm_window_create(struct weston_wm *wm,
			xcb_window_t id, int width, int height, int override)
{
	struct weston_wm_window *window;
	uint32_t values[1];

	window = malloc(sizeof *window);
	if (window == NULL) {
		weston_log("failed to allocate window\n");
		return;
	}

	values[0] = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(wm->conn, id, XCB_CW_EVENT_MASK, values);

	memset(window, 0, sizeof *window);
	window->wm = wm;
	window->id = id;
	window->properties_dirty = 1;
	window->override_redirect = override;
	window->width = width;
	window->height = height;

	hash_table_insert(wm->window_hash, id, window);
}

static void
weston_wm_window_destroy(struct weston_wm_window *window)
{
	hash_table_remove(window->wm->window_hash, window->id);
	free(window);
}

static void
weston_wm_handle_create_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_create_notify_event_t *create_notify =
		(xcb_create_notify_event_t *) event;

	weston_log("XCB_CREATE_NOTIFY (window %d, width %d, height %d%s%s)\n",
		create_notify->window,
		create_notify->width, create_notify->height,
		create_notify->override_redirect ? ", override" : "",
		our_resource(wm, create_notify->window) ? ", ours" : "");

	if (our_resource(wm, create_notify->window))
		return;

	weston_wm_window_create(wm, create_notify->window,
				create_notify->width, create_notify->height,
				create_notify->override_redirect);
}

static void
weston_wm_handle_destroy_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_destroy_notify_event_t *destroy_notify =
		(xcb_destroy_notify_event_t *) event;
	struct weston_wm_window *window;

	weston_log("XCB_DESTROY_NOTIFY, win %d, event %d%s\n",
		destroy_notify->window,
		destroy_notify->event,
		our_resource(wm, destroy_notify->window) ? ", ours" : "");

	if (our_resource(wm, destroy_notify->window))
		return;

	window = hash_table_lookup(wm->window_hash, destroy_notify->window);
	weston_wm_window_destroy(window);
}

static void
weston_wm_handle_reparent_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_reparent_notify_event_t *reparent_notify =
		(xcb_reparent_notify_event_t *) event;
	struct weston_wm_window *window;

	weston_log("XCB_REPARENT_NOTIFY (window %d, parent %d, event %d)\n",
		reparent_notify->window,
		reparent_notify->parent,
		reparent_notify->event);

	if (reparent_notify->parent == wm->screen->root) {
		weston_wm_window_create(wm, reparent_notify->window, 10, 10,
					reparent_notify->override_redirect);
	} else if (!our_resource(wm, reparent_notify->parent)) {
		window = hash_table_lookup(wm->window_hash,
					   reparent_notify->window);
		weston_wm_window_destroy(window);
	}
}

struct weston_seat *
weston_wm_pick_seat(struct weston_wm *wm)
{
	return container_of(wm->server->compositor->seat_list.next,
			    struct weston_seat, link);
}

static void
weston_wm_window_handle_moveresize(struct weston_wm_window *window,
				   xcb_client_message_event_t *client_message)
{
	static const int map[] = {
		THEME_LOCATION_RESIZING_TOP_LEFT,
		THEME_LOCATION_RESIZING_TOP,
		THEME_LOCATION_RESIZING_TOP_RIGHT,
		THEME_LOCATION_RESIZING_RIGHT,
		THEME_LOCATION_RESIZING_BOTTOM_RIGHT,
		THEME_LOCATION_RESIZING_BOTTOM,
		THEME_LOCATION_RESIZING_BOTTOM_LEFT,
		THEME_LOCATION_RESIZING_LEFT
	};

	struct weston_wm *wm = window->wm;
	struct weston_seat *seat = weston_wm_pick_seat(wm);
	int detail;
	struct weston_shell_interface *shell_interface =
		&wm->server->compositor->shell_interface;

	if (seat->seat.pointer->button_count != 1 ||
	    seat->seat.pointer->focus != &window->surface->surface)
		return;

	detail = client_message->data.data32[2];
	switch (detail) {
	case _NET_WM_MOVERESIZE_MOVE:
		if (!window->maximized)
			shell_interface->move(window->shsurf, seat);
		break;
	case _NET_WM_MOVERESIZE_SIZE_TOPLEFT:
	case _NET_WM_MOVERESIZE_SIZE_TOP:
	case _NET_WM_MOVERESIZE_SIZE_TOPRIGHT:
	case _NET_WM_MOVERESIZE_SIZE_RIGHT:
	case _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT:
	case _NET_WM_MOVERESIZE_SIZE_BOTTOM:
	case _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT:
	case _NET_WM_MOVERESIZE_SIZE_LEFT:
		shell_interface->resize(window->shsurf, seat, map[detail]);
		break;
	case _NET_WM_MOVERESIZE_CANCEL:
		break;
	}
}

#define _NET_WM_STATE_REMOVE	0
#define _NET_WM_STATE_ADD	1
#define _NET_WM_STATE_TOGGLE	2

static int
update_state(int action, int *state)
{
	int new_state, changed;

	switch (action) {
	case _NET_WM_STATE_REMOVE:
		new_state = 0;
		break;
	case _NET_WM_STATE_ADD:
		new_state = 1;
		break;
	case _NET_WM_STATE_TOGGLE:
		new_state = !*state;
		break;
	default:
		return 0;
	}

	changed = (*state != new_state);
	*state = new_state;

	return changed;
}

static void
weston_wm_window_handle_state(struct weston_wm_window *window,
			      xcb_client_message_event_t *client_message)
{
	struct weston_wm *wm = window->wm;
	struct weston_shell_interface *shell_interface =
		&wm->server->compositor->shell_interface;
	uint32_t action, property;

	action = client_message->data.data32[0];
	property = client_message->data.data32[1];

	if (property == wm->atom.net_wm_state_fullscreen &&
	    update_state(action, &window->fullscreen)) {
		weston_wm_window_set_net_wm_state(window);
		if (window->fullscreen) {
			if (!window->saved_size_valid) {
				window->saved_width = window->width;
				window->saved_height = window->height;
				window->saved_size_valid = 1;
			}

			if (window->shsurf)
				shell_interface->set_fullscreen(window->shsurf,
								WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
								0, NULL);
		} else {
			if (window->maximized)
				shell_interface->set_maximized(window->shsurf, NULL);
			else {
				shell_interface->set_toplevel(window->shsurf);
				window->width = window->saved_width;
				window->height = window->saved_height;
				window->saved_size_valid = 0;
				weston_wm_window_configure(window);
			}
		}
	} else if ((property == wm->atom.net_wm_state_maximized_vert ||
	     property == wm->atom.net_wm_state_maximized_horz) &&
	     update_state(action, &window->maximized)) {
		weston_wm_window_set_net_wm_state(window);
		if (window->maximized) {
			if (!window->saved_size_valid) {
				window->saved_width = window->width;
				window->saved_height = window->height;
				window->saved_size_valid = 1;
			}

			if (window->shsurf)
				shell_interface->set_maximized(window->shsurf, NULL);
		} else {
			shell_interface->set_toplevel(window->shsurf);
			window->width = window->saved_width;
			window->height = window->saved_height;
			window->saved_size_valid = 0;
			weston_wm_window_configure(window);
		}
	}
}

static void
weston_wm_handle_client_message(struct weston_wm *wm,
				xcb_generic_event_t *event)
{
	xcb_client_message_event_t *client_message =
		(xcb_client_message_event_t *) event;
	struct weston_wm_window *window;

	window = hash_table_lookup(wm->window_hash, client_message->window);

	weston_log("XCB_CLIENT_MESSAGE (%s %d %d %d %d %d win %d)\n",
		   get_atom_name(wm->conn, client_message->type),
		   client_message->data.data32[0],
		   client_message->data.data32[1],
		   client_message->data.data32[2],
		   client_message->data.data32[3],
		   client_message->data.data32[4],
		   client_message->window);

	if (client_message->type == wm->atom.net_wm_moveresize)
		weston_wm_window_handle_moveresize(window, client_message);
	else if (client_message->type == wm->atom.net_wm_state)
		weston_wm_window_handle_state(window, client_message);
}

enum cursor_type {
	XWM_CURSOR_TOP,
	XWM_CURSOR_BOTTOM,
	XWM_CURSOR_LEFT,
	XWM_CURSOR_RIGHT,
	XWM_CURSOR_TOP_LEFT,
	XWM_CURSOR_TOP_RIGHT,
	XWM_CURSOR_BOTTOM_LEFT,
	XWM_CURSOR_BOTTOM_RIGHT,
	XWM_CURSOR_LEFT_PTR,
};

static const char *cursors[] = {
	"top_side",
	"bottom_side",
	"left_side",
	"right_side",
	"top_left_corner",
	"top_right_corner",
	"bottom_left_corner",
	"bottom_right_corner",
	"left_ptr"
};

static void
weston_wm_create_cursors(struct weston_wm *wm)
{
	int i, count = ARRAY_LENGTH(cursors);

	wm->cursors = malloc(count * sizeof(xcb_cursor_t));
	for (i = 0; i < count; i++) {
		wm->cursors[i] =
			xcb_cursor_library_load_cursor(wm, cursors[i]);
	}

	wm->last_cursor = -1;
}

static void
weston_wm_destroy_cursors(struct weston_wm *wm)
{
	uint8_t i;

	for (i = 0; i < ARRAY_LENGTH(cursors); i++)
		xcb_free_cursor(wm->conn, wm->cursors[i]);

	free(wm->cursors);
}

static int
get_cursor_for_location(struct theme *t,
			int width, int height, int x, int y, int maximized)
{
	int location = theme_get_location(t, x, y, width, height,
					maximized ? THEME_FRAME_MAXIMIZED : 0);

	switch (location) {
		case THEME_LOCATION_RESIZING_TOP:
			return XWM_CURSOR_TOP;
		case THEME_LOCATION_RESIZING_BOTTOM:
			return XWM_CURSOR_BOTTOM;
		case THEME_LOCATION_RESIZING_LEFT:
			return XWM_CURSOR_LEFT;
		case THEME_LOCATION_RESIZING_RIGHT:
			return XWM_CURSOR_RIGHT;
		case THEME_LOCATION_RESIZING_TOP_LEFT:
			return XWM_CURSOR_TOP_LEFT;
		case THEME_LOCATION_RESIZING_TOP_RIGHT:
			return XWM_CURSOR_TOP_RIGHT;
		case THEME_LOCATION_RESIZING_BOTTOM_LEFT:
			return XWM_CURSOR_BOTTOM_LEFT;
		case THEME_LOCATION_RESIZING_BOTTOM_RIGHT:
			return XWM_CURSOR_BOTTOM_RIGHT;
		case THEME_LOCATION_EXTERIOR:
		case THEME_LOCATION_TITLEBAR:
		default:
			return XWM_CURSOR_LEFT_PTR;
	}
}

static void
weston_wm_window_set_cursor(struct weston_wm *wm, xcb_window_t window_id,
			    int cursor)
{
	uint32_t cursor_value_list;

	if (wm->last_cursor == cursor)
		return;

	wm->last_cursor = cursor;

	cursor_value_list = wm->cursors[cursor];
	xcb_change_window_attributes (wm->conn, window_id,
				      XCB_CW_CURSOR, &cursor_value_list);
	xcb_flush(wm->conn);
}

static void
weston_wm_handle_button(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_button_press_event_t *button = (xcb_button_press_event_t *) event;
	struct weston_shell_interface *shell_interface =
		&wm->server->compositor->shell_interface;
	struct weston_seat *seat = weston_wm_pick_seat(wm);
	struct weston_wm_window *window;
	enum theme_location location;
	struct theme *t = wm->theme;
	int width, height;

	weston_log("XCB_BUTTON_%s (detail %d)\n",
		button->response_type == XCB_BUTTON_PRESS ?
		"PRESS" : "RELEASE", button->detail);

	window = hash_table_lookup(wm->window_hash, button->event);
	weston_wm_window_get_frame_size(window, &width, &height);

	if (button->response_type == XCB_BUTTON_PRESS &&
	    button->detail == 1) {
		location = theme_get_location(t,
					      button->event_x,
					      button->event_y,
					      width, height, window->maximized ? THEME_FRAME_MAXIMIZED : 0);

		switch (location) {
		case THEME_LOCATION_TITLEBAR:
			shell_interface->move(window->shsurf, seat);
			break;
		case THEME_LOCATION_RESIZING_TOP:
		case THEME_LOCATION_RESIZING_BOTTOM:
		case THEME_LOCATION_RESIZING_LEFT:
		case THEME_LOCATION_RESIZING_RIGHT:
		case THEME_LOCATION_RESIZING_TOP_LEFT:
		case THEME_LOCATION_RESIZING_TOP_RIGHT:
		case THEME_LOCATION_RESIZING_BOTTOM_LEFT:
		case THEME_LOCATION_RESIZING_BOTTOM_RIGHT:
			shell_interface->resize(window->shsurf,
						seat, location);
			break;
		default:
			break;
		}
	}
}

static void
weston_wm_handle_motion(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *) event;
	struct weston_wm_window *window;
	int cursor, width, height;

	window = hash_table_lookup(wm->window_hash, motion->event);
	if (!window || !window->decorate)
		return;

	weston_wm_window_get_frame_size(window, &width, &height);
	cursor = get_cursor_for_location(wm->theme, width, height,
					 motion->event_x, motion->event_y,
					 window->maximized);

	weston_wm_window_set_cursor(wm, window->frame_id, cursor);
}

static void
weston_wm_handle_enter(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_enter_notify_event_t *enter = (xcb_enter_notify_event_t *) event;
	struct weston_wm_window *window;
	int cursor, width, height;

	window = hash_table_lookup(wm->window_hash, enter->event);
	if (!window || !window->decorate)
		return;

	weston_wm_window_get_frame_size(window, &width, &height);
	cursor = get_cursor_for_location(wm->theme, width, height,
					 enter->event_x, enter->event_y,
					 window->maximized);

	weston_wm_window_set_cursor(wm, window->frame_id, cursor);
}

static void
weston_wm_handle_leave(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_leave_notify_event_t *leave = (xcb_leave_notify_event_t *) event;
	struct weston_wm_window *window;

	window = hash_table_lookup(wm->window_hash, leave->event);
	if (!window || !window->decorate)
		return;

	weston_wm_window_set_cursor(wm, window->frame_id, XWM_CURSOR_LEFT_PTR);
}

static int
weston_wm_handle_event(int fd, uint32_t mask, void *data)
{
	struct weston_wm *wm = data;
	xcb_generic_event_t *event;
	int count = 0;

	while (event = xcb_poll_for_event(wm->conn), event != NULL) {
		if (weston_wm_handle_selection_event(wm, event)) {
			free(event);
			count++;
			continue;
		}

		switch (event->response_type & ~0x80) {
		case XCB_BUTTON_PRESS:
		case XCB_BUTTON_RELEASE:
			weston_wm_handle_button(wm, event);
			break;
		case XCB_ENTER_NOTIFY:
			weston_wm_handle_enter(wm, event);
			break;
		case XCB_LEAVE_NOTIFY:
			weston_wm_handle_leave(wm, event);
			break;
		case XCB_MOTION_NOTIFY:
			weston_wm_handle_motion(wm, event);
			break;
		case XCB_CREATE_NOTIFY:
			weston_wm_handle_create_notify(wm, event);
			break;
		case XCB_MAP_REQUEST:
			weston_wm_handle_map_request(wm, event);
			break;
		case XCB_MAP_NOTIFY:
			weston_wm_handle_map_notify(wm, event);
			break;
		case XCB_UNMAP_NOTIFY:
			weston_wm_handle_unmap_notify(wm, event);
			break;
		case XCB_REPARENT_NOTIFY:
			weston_wm_handle_reparent_notify(wm, event);
			break;
		case XCB_CONFIGURE_REQUEST:
			weston_wm_handle_configure_request(wm, event);
			break;
		case XCB_CONFIGURE_NOTIFY:
			weston_wm_handle_configure_notify(wm, event);
			break;
		case XCB_DESTROY_NOTIFY:
			weston_wm_handle_destroy_notify(wm, event);
			break;
		case XCB_MAPPING_NOTIFY:
			weston_log("XCB_MAPPING_NOTIFY\n");
			break;
		case XCB_PROPERTY_NOTIFY:
			weston_wm_handle_property_notify(wm, event);
			break;
		case XCB_CLIENT_MESSAGE:
			weston_wm_handle_client_message(wm, event);
			break;
		}

		free(event);
		count++;
	}

	xcb_flush(wm->conn);

	return count;
}

static void
weston_wm_get_resources(struct weston_wm *wm)
{

#define F(field) offsetof(struct weston_wm, field)

	static const struct { const char *name; int offset; } atoms[] = {
		{ "WM_PROTOCOLS",	F(atom.wm_protocols) },
		{ "WM_TAKE_FOCUS",	F(atom.wm_take_focus) },
		{ "WM_DELETE_WINDOW",	F(atom.wm_delete_window) },
		{ "WM_STATE",		F(atom.wm_state) },
		{ "WM_S0",		F(atom.wm_s0) },
		{ "WM_CLIENT_MACHINE",	F(atom.wm_client_machine) },
		{ "_NET_WM_NAME",	F(atom.net_wm_name) },
		{ "_NET_WM_PID",	F(atom.net_wm_pid) },
		{ "_NET_WM_ICON",	F(atom.net_wm_icon) },
		{ "_NET_WM_STATE",	F(atom.net_wm_state) },
		{ "_NET_WM_STATE_FULLSCREEN", F(atom.net_wm_state_fullscreen) },
		{ "_NET_WM_STATE_MAXIMIZED_VERT", F(atom.net_wm_state_maximized_vert) },
		{ "_NET_WM_STATE_MAXIMIZED_HORZ", F(atom.net_wm_state_maximized_horz) },
		{ "_NET_WM_USER_TIME", F(atom.net_wm_user_time) },
		{ "_NET_WM_ICON_NAME", F(atom.net_wm_icon_name) },
		{ "_NET_WM_WINDOW_TYPE", F(atom.net_wm_window_type) },

		{ "_NET_WM_WINDOW_TYPE_DESKTOP", F(atom.net_wm_window_type_desktop) },
		{ "_NET_WM_WINDOW_TYPE_DOCK", F(atom.net_wm_window_type_dock) },
		{ "_NET_WM_WINDOW_TYPE_TOOLBAR", F(atom.net_wm_window_type_toolbar) },
		{ "_NET_WM_WINDOW_TYPE_MENU", F(atom.net_wm_window_type_menu) },
		{ "_NET_WM_WINDOW_TYPE_UTILITY", F(atom.net_wm_window_type_utility) },
		{ "_NET_WM_WINDOW_TYPE_SPLASH", F(atom.net_wm_window_type_splash) },
		{ "_NET_WM_WINDOW_TYPE_DIALOG", F(atom.net_wm_window_type_dialog) },
		{ "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", F(atom.net_wm_window_type_dropdown) },
		{ "_NET_WM_WINDOW_TYPE_POPUP_MENU", F(atom.net_wm_window_type_popup) },
		{ "_NET_WM_WINDOW_TYPE_TOOLTIP", F(atom.net_wm_window_type_tooltip) },
		{ "_NET_WM_WINDOW_TYPE_NOTIFICATION", F(atom.net_wm_window_type_notification) },
		{ "_NET_WM_WINDOW_TYPE_COMBO", F(atom.net_wm_window_type_combo) },
		{ "_NET_WM_WINDOW_TYPE_DND", F(atom.net_wm_window_type_dnd) },
		{ "_NET_WM_WINDOW_TYPE_NORMAL",	F(atom.net_wm_window_type_normal) },

		{ "_NET_WM_MOVERESIZE", F(atom.net_wm_moveresize) },
		{ "_NET_SUPPORTING_WM_CHECK",
					F(atom.net_supporting_wm_check) },
		{ "_NET_SUPPORTED",     F(atom.net_supported) },
		{ "_MOTIF_WM_HINTS",	F(atom.motif_wm_hints) },
		{ "CLIPBOARD",		F(atom.clipboard) },
		{ "CLIPBOARD_MANAGER",	F(atom.clipboard_manager) },
		{ "TARGETS",		F(atom.targets) },
		{ "UTF8_STRING",	F(atom.utf8_string) },
		{ "_WL_SELECTION",	F(atom.wl_selection) },
		{ "INCR",		F(atom.incr) },
		{ "TIMESTAMP",		F(atom.timestamp) },
		{ "MULTIPLE",		F(atom.multiple) },
		{ "UTF8_STRING"	,	F(atom.utf8_string) },
		{ "COMPOUND_TEXT",	F(atom.compound_text) },
		{ "TEXT",		F(atom.text) },
		{ "STRING",		F(atom.string) },
		{ "text/plain;charset=utf-8",	F(atom.text_plain_utf8) },
		{ "text/plain",		F(atom.text_plain) },
	};
#undef F

	xcb_xfixes_query_version_cookie_t xfixes_cookie;
	xcb_xfixes_query_version_reply_t *xfixes_reply;
	xcb_intern_atom_cookie_t cookies[ARRAY_LENGTH(atoms)];
	xcb_intern_atom_reply_t *reply;
	xcb_render_query_pict_formats_reply_t *formats_reply;
	xcb_render_query_pict_formats_cookie_t formats_cookie;
	xcb_render_pictforminfo_t *formats;
	uint32_t i;

	xcb_prefetch_extension_data (wm->conn, &xcb_xfixes_id);

	formats_cookie = xcb_render_query_pict_formats(wm->conn);

	for (i = 0; i < ARRAY_LENGTH(atoms); i++)
		cookies[i] = xcb_intern_atom (wm->conn, 0,
					      strlen(atoms[i].name),
					      atoms[i].name);

	for (i = 0; i < ARRAY_LENGTH(atoms); i++) {
		reply = xcb_intern_atom_reply (wm->conn, cookies[i], NULL);
		*(xcb_atom_t *) ((char *) wm + atoms[i].offset) = reply->atom;
		free(reply);
	}

	wm->xfixes = xcb_get_extension_data(wm->conn, &xcb_xfixes_id);
	if (!wm->xfixes || !wm->xfixes->present)
		weston_log("xfixes not available\n");

	xfixes_cookie = xcb_xfixes_query_version(wm->conn,
						 XCB_XFIXES_MAJOR_VERSION,
						 XCB_XFIXES_MINOR_VERSION);
	xfixes_reply = xcb_xfixes_query_version_reply(wm->conn,
						      xfixes_cookie, NULL);

	weston_log("xfixes version: %d.%d\n",
	       xfixes_reply->major_version, xfixes_reply->minor_version);

	free(xfixes_reply);

	formats_reply = xcb_render_query_pict_formats_reply(wm->conn,
							    formats_cookie, 0);
	if (formats_reply == NULL)
		return;

	formats = xcb_render_query_pict_formats_formats(formats_reply);
	for (i = 0; i < formats_reply->num_formats; i++) {
		if (formats[i].direct.red_mask != 0xff &&
		    formats[i].direct.red_shift != 16)
			continue;
		if (formats[i].type == XCB_RENDER_PICT_TYPE_DIRECT &&
		    formats[i].depth == 24)
			wm->format_rgb = formats[i];
		if (formats[i].type == XCB_RENDER_PICT_TYPE_DIRECT &&
		    formats[i].depth == 32 &&
		    formats[i].direct.alpha_mask == 0xff &&
		    formats[i].direct.alpha_shift == 24)
			wm->format_rgba = formats[i];
	}

	free(formats_reply);
}

static void
weston_wm_create_wm_window(struct weston_wm *wm)
{
	static const char name[] = "Weston WM";

	wm->wm_window = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  XCB_COPY_FROM_PARENT,
			  wm->wm_window,
			  wm->screen->root,
			  0, 0,
			  10, 10,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  wm->screen->root_visual,
			  0, NULL);

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->wm_window,
			    wm->atom.net_supporting_wm_check,
			    XCB_ATOM_WINDOW,
			    32, /* format */
			    1, &wm->wm_window);

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->wm_window,
			    wm->atom.net_wm_name,
			    wm->atom.utf8_string,
			    8, /* format */
			    strlen(name), name);

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->screen->root,
			    wm->atom.net_supporting_wm_check,
			    XCB_ATOM_WINDOW,
			    32, /* format */
			    1, &wm->wm_window);

	/* Claim the WM_S0 selection even though we don't suport
	 * the --replace functionality. */
	xcb_set_selection_owner(wm->conn,
				wm->wm_window,
				wm->atom.wm_s0,
				XCB_TIME_CURRENT_TIME);
}

struct weston_wm *
weston_wm_create(struct weston_xserver *wxs)
{
	struct weston_wm *wm;
	struct wl_event_loop *loop;
	xcb_screen_iterator_t s;
	uint32_t values[1];
	int sv[2];
	xcb_atom_t supported[5];

	wm = malloc(sizeof *wm);
	if (wm == NULL)
		return NULL;

	memset(wm, 0, sizeof *wm);
	wm->server = wxs;
	wm->window_hash = hash_table_create();
	if (wm->window_hash == NULL) {
		free(wm);
		return NULL;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		weston_log("socketpair failed\n");
		hash_table_destroy(wm->window_hash);
		free(wm);
		return NULL;
	}

	xserver_send_client(wxs->resource, sv[1]);
	wl_client_flush(wxs->resource->client);
	close(sv[1]);
	
	/* xcb_connect_to_fd takes ownership of the fd. */
	wm->conn = xcb_connect_to_fd(sv[0], NULL);
	if (xcb_connection_has_error(wm->conn)) {
		weston_log("xcb_connect_to_fd failed\n");
		close(sv[0]);
		hash_table_destroy(wm->window_hash);
		free(wm);
		return NULL;
	}

	s = xcb_setup_roots_iterator(xcb_get_setup(wm->conn));
	wm->screen = s.data;

	loop = wl_display_get_event_loop(wxs->wl_display);
	wm->source =
		wl_event_loop_add_fd(loop, sv[0],
				     WL_EVENT_READABLE,
				     weston_wm_handle_event, wm);
	wl_event_source_check(wm->source);

	weston_wm_get_resources(wm);

	values[0] =
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(wm->conn, wm->screen->root,
				     XCB_CW_EVENT_MASK, values);
	wm->theme = theme_create(0);

	weston_wm_create_wm_window(wm);

	supported[0] = wm->atom.net_wm_moveresize;
	supported[1] = wm->atom.net_wm_state;
	supported[2] = wm->atom.net_wm_state_fullscreen;
	supported[3] = wm->atom.net_wm_state_maximized_vert;
	supported[4] = wm->atom.net_wm_state_maximized_horz;
	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->screen->root,
			    wm->atom.net_supported,
			    XCB_ATOM_ATOM,
			    32, /* format */
			    ARRAY_LENGTH(supported), supported);

	weston_wm_selection_init(wm);

	xcb_flush(wm->conn);

	wm->activate_listener.notify = weston_wm_window_activate;
	wl_signal_add(&wxs->compositor->activate_signal,
		      &wm->activate_listener);
	wm->kill_listener.notify = weston_wm_kill_client;
	wl_signal_add(&wxs->compositor->kill_signal,
		      &wm->kill_listener);

	weston_wm_create_cursors(wm);
	weston_wm_window_set_cursor(wm, wm->screen->root, XWM_CURSOR_LEFT_PTR);

	weston_log("created wm\n");

	return wm;
}

void
weston_wm_destroy(struct weston_wm *wm)
{
	/* FIXME: Free windows in hash. */
	hash_table_destroy(wm->window_hash);
	weston_wm_destroy_cursors(wm);
	xcb_disconnect(wm->conn);
	wl_event_source_remove(wm->source);
	wl_list_remove(&wm->selection_listener.link);
	wl_list_remove(&wm->activate_listener.link);
	wl_list_remove(&wm->kill_listener.link);

	free(wm);
}

static void
surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_wm_window *window =
		container_of(listener,
			     struct weston_wm_window, surface_destroy_listener);

	weston_log("surface for xid %d destroyed\n", window->id);
}

static struct weston_wm_window *
get_wm_window(struct weston_surface *surface)
{
	struct wl_resource *resource = &surface->surface.resource;
	struct wl_listener *listener;

	listener = wl_signal_get(&resource->destroy_signal, surface_destroy);
	if (listener)
		return container_of(listener, struct weston_wm_window,
				    surface_destroy_listener);

	return NULL;
}

static void
weston_wm_window_configure(void *data)
{
	struct weston_wm_window *window = data;
	struct weston_wm *wm = window->wm;
	uint32_t values[4];
	int x, y, width, height;

	weston_wm_window_get_child_position(window, &x, &y);
	values[0] = x;
	values[1] = y;
	values[2] = window->width;
	values[3] = window->height;
	xcb_configure_window(wm->conn,
			     window->id,
			     XCB_CONFIG_WINDOW_X |
			     XCB_CONFIG_WINDOW_Y |
			     XCB_CONFIG_WINDOW_WIDTH |
			     XCB_CONFIG_WINDOW_HEIGHT,
			     values);

	weston_wm_window_get_frame_size(window, &width, &height);
	values[0] = width;
	values[1] = height;
	xcb_configure_window(wm->conn,
			     window->frame_id,
			     XCB_CONFIG_WINDOW_WIDTH |
			     XCB_CONFIG_WINDOW_HEIGHT,
			     values);

	window->configure_source = NULL;

	weston_wm_window_schedule_repaint(window);
}

static void
send_configure(struct weston_surface *surface,
	       uint32_t edges, int32_t width, int32_t height)
{
	struct weston_wm_window *window = get_wm_window(surface);
	struct weston_wm *wm = window->wm;
	struct theme *t = window->wm->theme;

	if (width < 200)
		width = 200;
	if (height < 200)
		height = 200;

	if (window->fullscreen) {
		window->width = width;
		window->height = height;
	} else if (window->decorate && !window->maximized) {
		window->width = width - 2 * (t->margin + t->width);
		window->height = height - 2 * t->margin -
			t->titlebar_height - t->width;
	} else if (window->decorate && window->maximized) {
		window->width = width - t->width * 2;
		window->height = height - t->margin * 2;
	} else if (!window->decorate && window->maximized) {
		window->width = width;
		window->height = height;
	} else {
		window->width = width - 2 * t->margin;
		window->height = height - 2 * t->margin;
	}

	if (window->configure_source)
		return;

	window->configure_source =
		wl_event_loop_add_idle(wm->server->loop,
				       weston_wm_window_configure, window);
}

static void
send_maximize(struct weston_surface *surface)
{
	struct weston_wm_window *window = get_wm_window(surface);
	struct weston_wm *wm = window->wm;
	struct weston_shell_interface *shell_interface =
		&wm->server->compositor->shell_interface;

	if (window->maximized)
		return;

	if (!window->saved_size_valid) {
		window->saved_width = window->width;
		window->saved_height = window->height;
		window->saved_size_valid = 1;
	}
	update_state(_NET_WM_STATE_TOGGLE, &window->maximized);
	weston_wm_window_set_net_wm_state(window);
	shell_interface->set_maximized(window->shsurf, NULL);
}

static void
send_unmaximize(struct weston_surface *surface)
{
	struct weston_wm_window *window = get_wm_window(surface);
	struct weston_wm *wm = window->wm;
	struct weston_shell_interface *shell_interface =
		&wm->server->compositor->shell_interface;

	if (!window->maximized)
		return;

	window->width = window->saved_width;
	window->height = window->saved_height;
	window->saved_size_valid = 0;
	update_state(_NET_WM_STATE_TOGGLE, &window->maximized);
	weston_wm_window_set_net_wm_state(window);
	shell_interface->set_toplevel(window->shsurf);
	weston_wm_window_configure(window);
}

static void
send_minimize(struct weston_surface *surface)
{
	/* TODO: Track minimize state */
}

static void
send_unminimize(struct weston_surface *surface)
{
	/* TODO: Track minimize state */
}

static void
send_close(struct weston_surface *surface)
{
	weston_log("TODO: xwayland client close\n");
}

static const struct weston_shell_client shell_client = {
	send_configure,
	send_maximize,
	send_unmaximize,
	send_minimize,
	send_unminimize,
	send_close
};

static void
xserver_map_shell_surface(struct weston_wm *wm,
			  struct weston_wm_window *window)
{
	struct weston_shell_interface *shell_interface =
		&wm->server->compositor->shell_interface;
	struct weston_wm_window *parent;
	struct theme *t = window->wm->theme;
	int parent_id, x = 0, y = 0;

	if (!shell_interface->create_shell_surface)
		return;

	window->shsurf = 
		shell_interface->create_shell_surface(shell_interface->shell,
						      window->surface,
						      &shell_client);

	if (window->name)
		shell_interface->set_title(window->shsurf, window->name);

	if (window->fullscreen) {
		if (!window->saved_size_valid) {
			window->saved_width = window->width;
			window->saved_height = window->height;
			window->saved_size_valid = 1;
		}
		shell_interface->set_fullscreen(window->shsurf,
						WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
						0, NULL);
		return;
	} else if (window->maximized) {
		if (!window->saved_size_valid) {
			window->saved_width = window->width;
			window->saved_height = window->height;
			window->saved_size_valid = 1;
		}
		shell_interface->set_maximized(window->shsurf, NULL);
		return;
	} else if (!window->override_redirect) {
		/* ICCCM 4.1.1 */
		shell_interface->set_toplevel(window->shsurf);
		return;
	}

	/* not all non-toplevel has transient_for set. So we need this
	 * workaround to guess a parent that will determine the relative
	 * position of the transient surface */
	if (!window->transient_for)
		parent_id = wm->focus_latest->id;
	else
		parent_id = window->transient_for->id;

	parent = hash_table_lookup(wm->window_hash, parent_id);

	/* non-decorated and non-toplevel windows, e.g. sub-menus */
	if (!parent->decorate && parent->override_redirect) {
		x = parent->x + t->margin;
		y = parent->y + t->margin;
	}

	shell_interface->set_transient(window->shsurf, parent->surface,
				       window->x + t->margin - x,
				       window->y + t->margin - y,
				       WL_SHELL_SURFACE_TRANSIENT_INACTIVE);
}

static void
xserver_set_window_id(struct wl_client *client, struct wl_resource *resource,
		      struct wl_resource *surface_resource, uint32_t id)
{
	struct weston_xserver *wxs = resource->data;
	struct weston_wm *wm = wxs->wm;
	struct wl_surface *surface = surface_resource->data;
	struct weston_wm_window *window;

	if (client != wxs->client)
		return;

	window = hash_table_lookup(wm->window_hash, id);
	if (window == NULL) {
		weston_log("set_window_id for unknown window %d\n", id);
		return;
	}

	weston_log("set_window_id %d for surface %p\n", id, surface);

	weston_wm_window_read_properties(window);

	window->surface = (struct weston_surface *) surface;
	window->surface_destroy_listener.notify = surface_destroy;
	wl_signal_add(&surface->resource.destroy_signal,
		      &window->surface_destroy_listener);

	weston_wm_window_schedule_repaint(window);
	xserver_map_shell_surface(wm, window);
}

const struct xserver_interface xserver_implementation = {
	xserver_set_window_id
};
