/*
 * Copyright © 2012 Intel Corporation
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

#include <wayland-server.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <cairo/cairo-xcb.h>

#include "../compositor.h"

struct weston_xserver {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	struct wl_event_source *sigchld_source;
	int abstract_fd;
	struct wl_event_source *abstract_source;
	int unix_fd;
	struct wl_event_source *unix_source;
	int display;
	struct weston_process process;
	struct wl_resource *resource;
	struct wl_client *client;
	struct weston_compositor *compositor;
	struct weston_wm *wm;
	struct wl_listener destroy_listener;
};

struct weston_wm {
	xcb_connection_t *conn;
	const xcb_query_extension_reply_t *xfixes;
	struct wl_event_source *source;
	xcb_screen_t *screen;
	struct hash_table *window_hash;
	struct weston_xserver *server;
	xcb_window_t wm_window;
	struct weston_wm_window *focus_window;
	struct weston_wm_window *focus_latest;
	struct theme *theme;
	xcb_cursor_t *cursors;
	int last_cursor;
	xcb_render_pictforminfo_t format_rgb, format_rgba;
	struct wl_listener activate_listener;
	struct wl_listener kill_listener;

	xcb_window_t selection_window;
	xcb_window_t selection_owner;
	int incr;
	int data_source_fd;
	struct wl_event_source *property_source;
	xcb_get_property_reply_t *property_reply;
	int property_start;
	struct wl_array source_data;
	xcb_selection_request_event_t selection_request;
	xcb_atom_t selection_target;
	xcb_timestamp_t selection_timestamp;
	int selection_property_set;
	int flush_property_on_delete;
	struct wl_listener selection_listener;

	struct {
		xcb_atom_t		 wm_protocols;
		xcb_atom_t		 wm_take_focus;
		xcb_atom_t		 wm_delete_window;
		xcb_atom_t		 wm_state;
		xcb_atom_t		 wm_change_state;
		xcb_atom_t		 wm_s0;
		xcb_atom_t		 wm_client_machine;
		xcb_atom_t		 net_wm_name;
		xcb_atom_t		 net_wm_pid;
		xcb_atom_t		 net_wm_icon;
		xcb_atom_t		 net_wm_state;
		xcb_atom_t		 net_wm_state_fullscreen;
		xcb_atom_t		 net_wm_state_maximized_vert;
		xcb_atom_t		 net_wm_state_maximized_horz;
		xcb_atom_t		 net_wm_user_time;
		xcb_atom_t		 net_wm_icon_name;
		xcb_atom_t		 net_wm_window_type;
		xcb_atom_t		 net_wm_window_type_desktop;
		xcb_atom_t		 net_wm_window_type_dock;
		xcb_atom_t		 net_wm_window_type_toolbar;
		xcb_atom_t		 net_wm_window_type_menu;
		xcb_atom_t		 net_wm_window_type_utility;
		xcb_atom_t		 net_wm_window_type_splash;
		xcb_atom_t		 net_wm_window_type_dialog;
		xcb_atom_t		 net_wm_window_type_dropdown;
		xcb_atom_t		 net_wm_window_type_popup;
		xcb_atom_t		 net_wm_window_type_tooltip;
		xcb_atom_t		 net_wm_window_type_notification;
		xcb_atom_t		 net_wm_window_type_combo;
		xcb_atom_t		 net_wm_window_type_dnd;
		xcb_atom_t		 net_wm_window_type_normal;
		xcb_atom_t		 net_wm_moveresize;
		xcb_atom_t		 net_supporting_wm_check;
		xcb_atom_t		 net_supported;
		xcb_atom_t		 motif_wm_hints;
		xcb_atom_t		 clipboard;
		xcb_atom_t		 clipboard_manager;
		xcb_atom_t		 targets;
		xcb_atom_t		 utf8_string;
		xcb_atom_t		 wl_selection;
		xcb_atom_t		 incr;
		xcb_atom_t		 timestamp;
		xcb_atom_t		 multiple;
		xcb_atom_t		 compound_text;
		xcb_atom_t		 text;
		xcb_atom_t		 string;
		xcb_atom_t		 text_plain_utf8;
		xcb_atom_t		 text_plain;
	} atom;
};

void
dump_property(struct weston_wm *wm, xcb_atom_t property,
	      xcb_get_property_reply_t *reply);

const char *
get_atom_name(xcb_connection_t *c, xcb_atom_t atom);

void
weston_wm_selection_init(struct weston_wm *wm);
int
weston_wm_handle_selection_event(struct weston_wm *wm,
				 xcb_generic_event_t *event);

extern const struct xserver_interface xserver_implementation;

struct weston_wm *
weston_wm_create(struct weston_xserver *wxs);
void
weston_wm_destroy(struct weston_wm *wm);

struct weston_seat *
weston_wm_pick_seat(struct weston_wm *wm);
