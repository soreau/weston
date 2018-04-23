#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xfixes.h>
#include <X11/Xlib-xcb.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include "config.h"

#ifdef HAVE_XCB_XKB
#include <xcb/xkb.h>
#endif

#include "shared/helpers.h"

#include "xwps-backend.h"

struct xwpsb {
	Display *x_display;
	xcb_connection_t *connection;
	xcb_screen_t *screen;
	int visual_id;
	xcb_colormap_t colormap;
	EGLDisplay egl_display;
	EGLConfig egl_config;
	EGLContext egl_context;

	struct weston_compositor *compositor;
	struct weston_seat core_seat;

	unsigned int has_xkb;
	uint8_t xkb_event_base;

	double prev_x, prev_y;

	PFNEGLQUERYWAYLANDBUFFERWL query_buffer;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;

	struct {
		xcb_atom_t		 string;
		xcb_atom_t		 utf8_string;
		xcb_atom_t		 wm_change_state;
		xcb_atom_t		 _motif_wm_hints;
		xcb_atom_t		 _xkb_rules_names;
		xcb_atom_t		 _net_wm_moveresize;
		xcb_atom_t		 _net_wm_name;
	} atom;

	struct wl_list surfaces;
};

struct xwpsb_window {
	struct xwpsb *xwpsb;

	struct weston_surface *surface;

	xcb_window_t window;

	char *title;

	int x, y, wx, wy, width, height, last_width, last_height, pitch;
	int8_t resized, first_attach, minimized;

	int8_t button_pressed;
	uint32_t edges;
	int16_t button_grab_root_x;
	int16_t button_grab_root_y;
	uint32_t button_grab_button;
	uint16_t button_grab_sequence;
	xcb_button_t button_grab_detail;

	struct weston_buffer_reference buffer_ref;
	int8_t img_ref;
	GLuint texture;
	EGLImageKHR image;
	EGLint y_inverted;
	EGLSurface egl_surface;

	struct wl_list link;
};

struct MotifHints
{
    uint32_t   flags;
    uint32_t   functions;
    uint32_t   decorations;
    int32_t    input_mode;
    uint32_t   status;
};

static const EGLint egl_config_attribs[] = {
	EGL_COLOR_BUFFER_TYPE,     EGL_RGB_BUFFER,
	EGL_BUFFER_SIZE,           24,
	EGL_RED_SIZE,              8,
	EGL_GREEN_SIZE,            8,
	EGL_BLUE_SIZE,             8,
	EGL_ALPHA_SIZE,            8,

	EGL_DEPTH_SIZE,            24,
	EGL_STENCIL_SIZE,          8,

	EGL_SAMPLE_BUFFERS,        0,
	EGL_SAMPLES,               0,

	EGL_SURFACE_TYPE,          EGL_WINDOW_BIT,
	EGL_RENDERABLE_TYPE,       EGL_OPENGL_BIT,
	EGL_CONFORMANT,            EGL_OPENGL_BIT,

	EGL_NONE,
};

static const EGLint egl_context_attribs[] = {
	EGL_NONE,
};

static const EGLint egl_surface_attribs[] = {
	EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
	EGL_NONE,
};

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

#define DEFAULT_AXIS_STEP_DISTANCE 10

static int
get_window_from_id(struct xwpsb *xwpsb, xcb_window_t window, struct xwpsb_window **out_xwpsb_window)
{
	struct xwpsb_window *xwpsb_window;

	wl_list_for_each(xwpsb_window, &xwpsb->surfaces, link) {
		if (window == xwpsb_window->window) {
			*out_xwpsb_window = xwpsb_window;
			return 1;
		}
	}

	return 0;
}

static int
get_window_from_surface(struct xwpsb *xwpsb, struct weston_surface *surface, struct xwpsb_window **out_xwpsb_window)
{
	struct xwpsb_window *xwpsb_window;

	wl_list_for_each(xwpsb_window, &xwpsb->surfaces, link) {
		if (surface == xwpsb_window->surface) {
			*out_xwpsb_window = xwpsb_window;
			return 1;
		}
	}

	return 0;
}

#define F(field) offsetof(struct xwpsb, field)

static void
get_atoms(struct xwpsb *b)
{
	static const struct { const char *name; int offset; } atoms[] = {
		{ "STRING",		F(atom.string) },
		{ "UTF8_STRING",	F(atom.utf8_string) },
		{ "WM_CHANGE_STATE",	F(atom.wm_change_state) },
		{ "_MOTIF_WM_HINTS",	F(atom._motif_wm_hints) },
		{ "_XKB_RULES_NAMES",	F(atom._xkb_rules_names) },
		{ "_NET_WM_MOVERESIZE",	F(atom._net_wm_moveresize) },
		{ "_NET_WM_NAME",	F(atom._net_wm_name) },
	};

	uint32_t i;

	xcb_intern_atom_cookie_t cookies[ARRAY_LENGTH(atoms)];
	xcb_intern_atom_reply_t *reply;

	for (i = 0; i < ARRAY_LENGTH(atoms); i++)
		cookies[i] = xcb_intern_atom (b->connection, 0,
					      strlen(atoms[i].name),
					      atoms[i].name);

	for (i = 0; i < ARRAY_LENGTH(atoms); i++) {
		reply = xcb_intern_atom_reply (b->connection, cookies[i], NULL);
		if (!reply)
			continue;
		*(xcb_atom_t *) ((char *) b + atoms[i].offset) = reply->atom;
		free(reply);
	}
}

static void
destroy_xwpsb_window(struct xwpsb_window *xwpsb_window)
{
	glDeleteTextures(1, &xwpsb_window->texture);
	xcb_destroy_window(xwpsb_window->xwpsb->connection, xwpsb_window->window);
	wl_list_remove(&xwpsb_window->link);
	xcb_flush(xwpsb_window->xwpsb->connection);
	free(xwpsb_window->title);
	free(xwpsb_window);
}

static void
draw(struct xwpsb_window *xwpsb_window)
{
	struct xwpsb *xwpsb = xwpsb_window->xwpsb;

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, xwpsb_window->texture);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_TEXTURE_2D);

	glViewport(0, 0, xwpsb_window->width, xwpsb_window->height);

	glBegin(GL_QUADS);
	if (xwpsb_window->y_inverted) {
		glTexCoord2i(0, 1); glVertex2f(-1, -1);
		glTexCoord2i(0, 0); glVertex2f(-1, 1);
		glTexCoord2i(1, 0); glVertex2f(1, 1);
		glTexCoord2i(1, 1); glVertex2f(1, -1);
	} else {
		glTexCoord2i(0, 0); glVertex2f(-1, -1);
		glTexCoord2i(0, 1); glVertex2f(-1, 1);
		glTexCoord2i(1, 1); glVertex2f(1, 1);
		glTexCoord2i(1, 0); glVertex2f(1, -1);
	}
	glEnd();

	eglSwapBuffers(xwpsb->egl_display, xwpsb_window->egl_surface);

	glActiveTexture(0);
	glDisable(GL_TEXTURE_2D);

	eglMakeCurrent(xwpsb->egl_display,
			xwpsb->compositor->egl_surface,
			xwpsb->compositor->egl_surface,
			xwpsb->compositor->egl_context);
}

static struct xkb_keymap *
xwpsb_get_keymap(struct xwpsb *xwpsb)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	struct xkb_rule_names names;
	struct xkb_keymap *ret;
	const char *value_all, *value_part;
	int length_all, length_part;

	memset(&names, 0, sizeof(names));

	cookie = xcb_get_property(xwpsb->connection, 0, xwpsb->screen->root,
				  xwpsb->atom._xkb_rules_names, xwpsb->atom.string, 0, 1024);
	reply = xcb_get_property_reply(xwpsb->connection, cookie, NULL);
	if (reply == NULL)
		return NULL;

	value_all = xcb_get_property_value(reply);
	length_all = xcb_get_property_value_length(reply);
	value_part = value_all;

#define copy_prop_value(to) \
	length_part = strlen(value_part); \
	if (value_part + length_part < (value_all + length_all) && \
	    length_part > 0) \
		names.to = value_part; \
	value_part += length_part + 1;

	copy_prop_value(rules);
	copy_prop_value(model);
	copy_prop_value(layout);
	copy_prop_value(variant);
	copy_prop_value(options);
#undef copy_prop_value

	ret = xkb_keymap_new_from_names(xwpsb->compositor->xkb_context, &names, 0);

	free(reply);
	return ret;
}

static uint32_t
get_xkb_mod_mask(struct xwpsb *xwpsb, uint32_t in)
{
	struct weston_keyboard *keyboard =
		weston_seat_get_keyboard(&xwpsb->core_seat);
	struct weston_xkb_info *info = keyboard->xkb_info;
	uint32_t ret = 0;

	if ((in & ShiftMask) && info->shift_mod != XKB_MOD_INVALID)
		ret |= (1 << info->shift_mod);
	if ((in & LockMask) && info->caps_mod != XKB_MOD_INVALID)
		ret |= (1 << info->caps_mod);
	if ((in & ControlMask) && info->ctrl_mod != XKB_MOD_INVALID)
		ret |= (1 << info->ctrl_mod);
	if ((in & Mod1Mask) && info->alt_mod != XKB_MOD_INVALID)
		ret |= (1 << info->alt_mod);
	if ((in & Mod2Mask) && info->mod2_mod != XKB_MOD_INVALID)
		ret |= (1 << info->mod2_mod);
	if ((in & Mod3Mask) && info->mod3_mod != XKB_MOD_INVALID)
		ret |= (1 << info->mod3_mod);
	if ((in & Mod4Mask) && info->super_mod != XKB_MOD_INVALID)
		ret |= (1 << info->super_mod);
	if ((in & Mod5Mask) && info->mod5_mod != XKB_MOD_INVALID)
		ret |= (1 << info->mod5_mod);

	return ret;
}

#ifdef HAVE_XCB_XKB
static void
update_xkb_keymap(struct xwpsb *xwpsb)
{
	struct xkb_keymap *keymap;

	keymap = xwpsb_get_keymap(xwpsb);
	if (!keymap) {
		weston_log("failed to get XKB keymap\n");
		return;
	}
	weston_seat_update_keymap(&xwpsb->core_seat, keymap);
	xkb_keymap_unref(keymap);
}

static void
update_xkb_state(struct xwpsb *xwpsb, xcb_xkb_state_notify_event_t *state)
{
	struct weston_keyboard *keyboard =
		weston_seat_get_keyboard(&xwpsb->core_seat);

	xkb_state_update_mask(keyboard->xkb_state.state,
			      get_xkb_mod_mask(xwpsb, state->baseMods),
			      get_xkb_mod_mask(xwpsb, state->latchedMods),
			      get_xkb_mod_mask(xwpsb, state->lockedMods),
			      0,
			      0,
			      state->group);

	notify_modifiers(&xwpsb->core_seat,
			 wl_display_next_serial(xwpsb->compositor->wl_display));
}
#endif

WL_EXPORT int
xwpsb_surface_move(void *data, struct weston_surface *surface)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;
	xcb_client_message_event_t message;
	int direction = _NET_WM_MOVERESIZE_MOVE;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window) ||
		!xwpsb_window->button_pressed)
		return 0;

	message.response_type = XCB_CLIENT_MESSAGE;
	message.format = 32;
	message.sequence = xwpsb_window->button_grab_sequence;
	message.window = xwpsb_window->window;
	message.type = xwpsb->atom._net_wm_moveresize;
	message.data.data32[0] = xwpsb_window->button_grab_root_x;
	message.data.data32[1] = xwpsb_window->button_grab_root_y;
	message.data.data32[2] = direction;
	message.data.data32[3] = xwpsb_window->button_grab_detail;
	message.data.data32[4] = 1;

	xcb_ungrab_pointer(xwpsb->connection, XCB_CURRENT_TIME);
	xcb_flush(xwpsb->connection);

	xcb_send_event(xwpsb->connection, 0, xwpsb->screen->root,
			XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
			(char*) &message);
	xcb_flush(xwpsb->connection);

	return 1;
}

WL_EXPORT int
xwpsb_surface_resize(void *data, struct weston_surface *surface,
			uint32_t edges)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window) ||
		!xwpsb_window->button_pressed)
		return 0;

	if (!xwpsb_window->button_pressed)
		return 0;

	xwpsb_window->edges = edges;
	xwpsb_window->last_width = xwpsb_window->width;
	xwpsb_window->last_height = xwpsb_window->height;

	return 0;
}

WL_EXPORT void
xwpsb_surface_minimize(void *data, struct weston_surface *surface)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;
	xcb_client_message_event_t message;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window))
		return;

	memset(&message, 0, sizeof (message));

	message.response_type = XCB_CLIENT_MESSAGE;
	message.format = 32;
	message.sequence = xwpsb_window->button_grab_sequence;
	message.window = xwpsb_window->window;
	message.type = xwpsb->atom.wm_change_state;
	message.data.data32[0] = XCB_ICCCM_WM_STATE_ICONIC;

	xcb_send_event(xwpsb->connection, 0, xwpsb->screen->root,
			XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
			(char*) &message);
	xcb_flush(xwpsb->connection);

	xwpsb_window->minimized = 1;
}

WL_EXPORT void
xwpsb_set_title(void *data, struct weston_surface *surface, const char *s)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;
	char *tmp;

	if (!s || !get_window_from_surface(xwpsb, surface, &xwpsb_window))
		return;

	tmp = strdup(s);

	free(xwpsb_window->title);

	xwpsb_window->title = tmp;

	if (!xwpsb_window->window)
		return;

	xcb_change_property(xwpsb->connection, XCB_PROP_MODE_REPLACE,
				xwpsb_window->window, xwpsb->atom._net_wm_name,
				xwpsb->atom.utf8_string, 8, strlen(tmp), tmp);
	xcb_flush(xwpsb->connection);
}

WL_EXPORT void
xwpsb_position_notify(void *data, struct weston_surface *surface, int x, int y)
{
	struct weston_pointer_motion_event motion_event = { 0 };
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;
	int offset_x, offset_y;
	struct timespec time;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window))
		return;

	if (xwpsb_window->edges)
		return;

	offset_x = x - xwpsb_window->wx;
	offset_y = y - xwpsb_window->wy;

	xwpsb_window->wx = x;
	xwpsb_window->wy = y;

	if (offset_x == 0 && offset_y == 0)
		return;

	motion_event = (struct weston_pointer_motion_event) {
		.mask = WESTON_POINTER_MOTION_REL,
		.dx = offset_x,
		.dy = offset_y
	};

	weston_compositor_get_time(&time);
	notify_motion(&xwpsb->core_seat, &time, &motion_event);
	notify_pointer_frame(&xwpsb->core_seat);
}

static void
xwpsb_deliver_button_event(struct xwpsb *xwpsb,
				xcb_generic_event_t *event)
{
	xcb_button_press_event_t *button_event =
		(xcb_button_press_event_t *) event;
	struct weston_pointer_axis_event weston_event;
	bool is_button_pressed = event->response_type == XCB_BUTTON_PRESS;
	struct xwpsb_window *xwpsb_window;
	struct timespec time = { 0 };
	uint32_t button;

	if (event->response_type != XCB_BUTTON_PRESS &&
		event->response_type != XCB_BUTTON_RELEASE)
		return;

	if (!get_window_from_id(xwpsb, button_event->event, &xwpsb_window))
		return;

	switch (button_event->detail) {
	case 1:
		button = BTN_LEFT;
		break;
	case 2:
		button = BTN_MIDDLE;
		break;
	case 3:
		button = BTN_RIGHT;
		break;
	case 4:
		/* Axis are measured in pixels, but the xcb events are discrete
		 * steps. Therefore move the axis by some pixels every step. */
		if (is_button_pressed) {
			weston_event.value = -DEFAULT_AXIS_STEP_DISTANCE;
			weston_event.discrete = -1;
			weston_event.has_discrete = true;
			weston_event.axis =
				WL_POINTER_AXIS_VERTICAL_SCROLL;
			weston_compositor_get_time(&time);
			notify_axis(&xwpsb->core_seat, &time, &weston_event);
			notify_pointer_frame(&xwpsb->core_seat);
		}
		return;
	case 5:
		if (is_button_pressed) {
			weston_event.value = DEFAULT_AXIS_STEP_DISTANCE;
			weston_event.discrete = 1;
			weston_event.has_discrete = true;
			weston_event.axis =
				WL_POINTER_AXIS_VERTICAL_SCROLL;
			weston_compositor_get_time(&time);
			notify_axis(&xwpsb->core_seat, &time, &weston_event);
			notify_pointer_frame(&xwpsb->core_seat);
		}
		return;
	case 6:
		if (is_button_pressed) {
			weston_event.value = -DEFAULT_AXIS_STEP_DISTANCE;
			weston_event.discrete = -1;
			weston_event.has_discrete = true;
			weston_event.axis =
				WL_POINTER_AXIS_HORIZONTAL_SCROLL;
			weston_compositor_get_time(&time);
			notify_axis(&xwpsb->core_seat, &time, &weston_event);
			notify_pointer_frame(&xwpsb->core_seat);
		}
		return;
	case 7:
		if (is_button_pressed) {
			weston_event.value = DEFAULT_AXIS_STEP_DISTANCE;
			weston_event.discrete = 1;
			weston_event.has_discrete = true;
			weston_event.axis =
				WL_POINTER_AXIS_HORIZONTAL_SCROLL;
			weston_compositor_get_time(&time);
			notify_axis(&xwpsb->core_seat, &time, &weston_event);
			notify_pointer_frame(&xwpsb->core_seat);
		}
		return;
	default:
		button = button_event->detail + BTN_SIDE - 8;
		break;
	}

	if (is_button_pressed) {
		xwpsb_window->button_pressed = 1;
		xwpsb_window->button_grab_sequence = button_event->sequence;
		xwpsb_window->button_grab_root_x = button_event->root_x;
		xwpsb_window->button_grab_root_y = button_event->root_y;
		xwpsb_window->button_grab_detail = button_event->detail;
		xwpsb_window->button_grab_button = button;
	} else {
		xwpsb_window->button_pressed = 0;
		xwpsb_window->edges = 0;
	}

	weston_compositor_get_time(&time);

	notify_button(&xwpsb->core_seat, &time, button,
		      is_button_pressed ? WL_POINTER_BUTTON_STATE_PRESSED :
					  WL_POINTER_BUTTON_STATE_RELEASED);
	notify_pointer_frame(&xwpsb->core_seat);
}

static void
xwpsb_window_shape(struct xwpsb_window *xwpsb_window)
{
	struct weston_surface *surface = xwpsb_window->surface;
	xcb_rectangle_t rect;

	rect.x = surface->input.extents.x1;
	rect.y = surface->input.extents.y1;
	rect.width = surface->input.extents.x2 - rect.x;
	rect.height = surface->input.extents.y2 - rect.y;

	xcb_shape_rectangles(xwpsb_window->xwpsb->connection,
			XCB_SHAPE_SO_SET,
			XCB_SHAPE_SK_INPUT,
			0, xwpsb_window->window,
			0, 0, 1, &rect);

	xcb_flush(xwpsb_window->xwpsb->connection);
}

static int
xwpsb_next_event(xcb_connection_t *connection,
		       xcb_generic_event_t **event, uint32_t mask)
{
	if (mask & WL_EVENT_READABLE)
		*event = xcb_poll_for_event(connection);
	else
		*event = xcb_poll_for_queued_event(connection);

	return *event != NULL;
}

static int
xwpsb_handle_event(int fd, uint32_t mask, void *data)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;
	struct weston_view *view;
	xcb_generic_event_t *event;
	xcb_key_press_event_t *key;
	struct timespec time;
	int count = 0;

	while (xwpsb_next_event(xwpsb->connection, &event, mask)) {
		switch(event->response_type & ~0x80) {
			case XCB_EXPOSE: {
				xcb_expose_event_t *expose = (xcb_expose_event_t *) event;
				if (!get_window_from_id(xwpsb, expose->window, &xwpsb_window))
					break;
				if (xwpsb_window->first_attach || xwpsb_window->minimized) {
					eglMakeCurrent(xwpsb->egl_display, xwpsb_window->egl_surface, xwpsb_window->egl_surface, xwpsb->egl_context);
					draw(xwpsb_window);
					xcb_flush(xwpsb->connection);
					xwpsb_window->first_attach = 0;
					xwpsb_window->minimized = 0;
				}
				break;
			}
			case XCB_KEY_PRESS: {
				key = (xcb_key_press_event_t *) event;
				if (!get_window_from_id(xwpsb, key->event, &xwpsb_window))
					break;

				/* Quit on Esc */
				if (key->detail == 9) {
					destroy_xwpsb_window(xwpsb_window);
					break;
				}
				weston_compositor_get_time(&time);
				notify_key(&xwpsb->core_seat,
					   &time,
					   key->detail - 8,
					   WL_KEYBOARD_KEY_STATE_PRESSED,
					   xwpsb->has_xkb ? STATE_UPDATE_NONE :
						STATE_UPDATE_AUTOMATIC);
				break;
			}
			case XCB_KEY_RELEASE: {
				key = (xcb_key_press_event_t *) event;
				weston_compositor_get_time(&time);
				notify_key(&xwpsb->core_seat,
					   &time,
					   key->detail - 8,
					   WL_KEYBOARD_KEY_STATE_RELEASED,
					   STATE_UPDATE_NONE);
				break;
			}
			case XCB_BUTTON_PRESS:
			case XCB_BUTTON_RELEASE: {
				xwpsb_deliver_button_event(xwpsb, event);
				break;
			}
			case XCB_MOTION_NOTIFY: {
				double x, y;
				struct weston_pointer_motion_event motion_event = { 0 };
				xcb_motion_notify_event_t *motion_notify =
						(xcb_motion_notify_event_t *) event;

				if (!get_window_from_id(xwpsb, motion_notify->event, &xwpsb_window))
					break;

				view = container_of(xwpsb_window->surface->views.next,
							struct weston_view, surface_link);

				weston_output_transform_coordinate(&xwpsb_window->surface->output[0],
								   motion_notify->root_x,
								   motion_notify->root_y,
								   &x, &y);

				motion_event = (struct weston_pointer_motion_event) {
					.mask = WESTON_POINTER_MOTION_REL,
					.dx = x - xwpsb->prev_x,
					.dy = y - xwpsb->prev_y
				};

				weston_compositor_get_time(&time);
				notify_motion(&xwpsb->core_seat, &time, &motion_event);
				notify_pointer_frame(&xwpsb->core_seat);

				xwpsb->prev_x = x;
				xwpsb->prev_y = y;
				break;
			}
			case XCB_ENTER_NOTIFY: {
				double x, y;
				xcb_enter_notify_event_t *enter_notify =
						(xcb_enter_notify_event_t *) event;

				if (enter_notify->state >= Button1Mask)
					break;

				if (!get_window_from_id(xwpsb, enter_notify->event, &xwpsb_window))
					break;

				view = container_of(xwpsb_window->surface->views.next,
									struct weston_view, surface_link);

				weston_output_transform_coordinate(&xwpsb_window->surface->output[0],
								   enter_notify->event_x + view->geometry.x,
								   enter_notify->event_y + view->geometry.y,
								   &x, &y);

				notify_pointer_focus(&xwpsb->core_seat, &xwpsb_window->surface->output[0], x, y);

				xwpsb->prev_x = enter_notify->root_x;
				xwpsb->prev_y = enter_notify->root_y;

				if (xwpsb_window->button_pressed) {
					weston_compositor_get_time(&time);
					notify_button(&xwpsb->core_seat, &time, xwpsb_window->button_grab_button,
						      WL_POINTER_BUTTON_STATE_RELEASED);
					notify_pointer_frame(&xwpsb->core_seat);
					xwpsb_window->button_pressed = 0;
				}
				break;
			}
			case XCB_CONFIGURE_NOTIFY: {
				xcb_configure_notify_event_t *configure_notify =
					(xcb_configure_notify_event_t *) event;

				if (!get_window_from_id(xwpsb, configure_notify->window, &xwpsb_window))
					break;

				xwpsb_window->x = configure_notify->x;
				xwpsb_window->y = configure_notify->y;

				xwpsb_window_shape(xwpsb_window);

				if (xwpsb_window->resized &&
					xwpsb_window->width == configure_notify->width &&
					xwpsb_window->height == configure_notify->height) {
					eglMakeCurrent(xwpsb->egl_display, xwpsb_window->egl_surface, xwpsb_window->egl_surface, xwpsb->egl_context);
					draw(xwpsb_window);
					xcb_flush(xwpsb->connection);
				}
				break;
			}
			default: {
				break;
			}
		}
#ifdef HAVE_XCB_XKB
		if (xwpsb->has_xkb) {
			if ((event->response_type & ~0x80) == xwpsb->xkb_event_base) {
				xcb_xkb_state_notify_event_t *state =
					(xcb_xkb_state_notify_event_t *) event;
				if (state->xkbType == XCB_XKB_STATE_NOTIFY)
					update_xkb_state(xwpsb, state);
			} else if ((event->response_type & ~0x80) == XCB_PROPERTY_NOTIFY) {
				xcb_property_notify_event_t *prop_notify =
					(xcb_property_notify_event_t *) event;
				if (prop_notify->window == xwpsb->screen->root &&
				    prop_notify->atom == xwpsb->atom._xkb_rules_names &&
				    prop_notify->state == XCB_PROPERTY_NEW_VALUE)
					update_xkb_keymap(xwpsb);
			}
		}
#endif

		count++;
		free(event);
	}

	return count;
}

static int
setup_x(struct xwpsb *xwpsb)
{
	Display *x_display = XOpenDisplay(":0");
	if (!x_display) {
		fprintf(stderr, "Failed to open X display\n");
		return -1;
	}

	xcb_connection_t *connection = XGetXCBConnection(x_display);
	if (!connection) {
		fprintf(stderr, "Failed to get XCB connection\n");
		return -1;
	}
	if (xcb_connection_has_error(connection)) {
		fprintf(stderr, "XCB connection error occured\n");
		return -1;
	}

	const xcb_setup_t *setup = xcb_get_setup(connection);
	xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
	if (screen == 0) {
		fprintf(stderr, "Error getting XCB screen\n");
		return -1;
	}

	/* Find 32 bit True Color visual */
	xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);
	xcb_depth_t *depth = NULL;

	while (depth_iter.rem) {
		if (depth_iter.data->depth == 32 && depth_iter.data->visuals_len) {
			depth = depth_iter.data;
			break;
		}
		xcb_depth_next(&depth_iter);
	}
	if (!depth) {
		fprintf(stderr, "ERROR: screen does not support 32 bit color depth\n");
		xcb_disconnect(connection);
		return -1;
	}

	xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth);
	xcb_visualtype_t *visual = NULL;

	while (visual_iter.rem) {
		if (visual_iter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
			visual = visual_iter.data;
			break;
		}
		xcb_visualtype_next(&visual_iter);
	}
	if (!visual) {
		fprintf(stderr, "ERROR: screen does not support True Color\n");
		xcb_disconnect(connection);
		return -1;
	}

	weston_log("Found True Color visual id: %d\n", visual->visual_id);

	xcb_colormap_t colormap = xcb_generate_id(connection);
	if (colormap <= 0) {
		fprintf(stderr, "Failed to generate colormap id\n");
		return -1;
	}

	xcb_create_colormap(
		connection,
		XCB_COLORMAP_ALLOC_NONE,
		colormap,
		screen->root,
		visual->visual_id
	);

	xwpsb->x_display = x_display;
	xwpsb->connection = connection;
	xwpsb->screen = screen;
	xwpsb->visual_id = visual->visual_id;
	xwpsb->colormap = colormap;

	get_atoms(xwpsb);

	return 0;
}

static int
setup_egl(struct xwpsb *xwpsb, EGLDisplay egl_display)
{
	/* Setup EGL */
	EGLConfig egl_config;
	EGLContext egl_context;
	EGLint ignore;
	EGLBoolean ok;

	ok = eglBindAPI(EGL_OPENGL_API);
	if (!ok) {
		fprintf(stderr, "eglBindAPI(EGL_OPENGL_API) failed\n");
		return -1;
	}

	ok = eglInitialize(egl_display, &ignore, &ignore);
	if (!ok) {
		fprintf(stderr, "eglInitialize() failed\n");
		return -1;
	}

	/* Choose config */
	EGLint configs_size = 256;
	EGLConfig *configs = calloc(configs_size, sizeof (EGLConfig));
	EGLint num_configs;

	ok = eglChooseConfig(egl_display,
			egl_config_attribs,
			configs,
			configs_size, // num requested configs
			&num_configs); // num returned configs
	if (!ok) {
		fprintf(stderr, "eglChooseConfig() failed\n");
		return -1;
	}
	if (num_configs == 0) {
		fprintf(stderr, "failed to find suitable EGLConfig\n");
		return -1;
	}

	egl_config = configs[0];

	free(configs);

	egl_context = eglCreateContext(egl_display,
					egl_config,
					EGL_NO_CONTEXT,
					egl_context_attribs);
	if (!egl_context) {
		fprintf(stderr, "eglCreateContext() failed\n");
		return -1;
	}

	xwpsb->query_buffer = (void *) eglGetProcAddress("eglQueryWaylandBufferWL");
	xwpsb->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	xwpsb->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");
	xwpsb->image_target_texture_2d = (void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");

	xwpsb->egl_display = egl_display;
	xwpsb->egl_config = egl_config;
	xwpsb->egl_context = egl_context;

	return 0;
}

static void
xwpsb_create_x11_window(struct xwpsb_window *xwpsb_window, int width, int height)
{
	struct xwpsb *xwpsb = xwpsb_window->xwpsb;
	struct weston_view *view;
	EGLSurface egl_surface;

	xcb_window_t window = xcb_generate_id(xwpsb->connection);
	if (window <= 0) {
		fprintf(stderr, "Failed to generate X window id\n");
		return;
	}

	uint32_t xcb_window_attrib_mask = XCB_CW_BORDER_PIXEL |
					XCB_CW_EVENT_MASK |
					XCB_CW_COLORMAP;
	uint32_t xcb_window_attrib_list[] = {
		xwpsb->screen->white_pixel,
		XCB_EVENT_MASK_EXPOSURE |
		XCB_EVENT_MASK_BUTTON_PRESS |
		XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_POINTER_MOTION |
		XCB_EVENT_MASK_ENTER_WINDOW |
		XCB_EVENT_MASK_LEAVE_WINDOW |
		XCB_EVENT_MASK_KEY_PRESS |
		XCB_EVENT_MASK_KEY_RELEASE |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY,
		xwpsb->colormap,
		0 };

	xcb_void_cookie_t create_cookie = xcb_create_window_checked(
					xwpsb->connection,
					32,
					window,
					xwpsb->screen->root,
					0,
					0,
					width,
					height,
					0,
					XCB_WINDOW_CLASS_INPUT_OUTPUT,
					xwpsb->visual_id,
					xcb_window_attrib_mask,
					xcb_window_attrib_list);

	struct MotifHints hints;
	memset(&hints, 0, sizeof (hints));
	hints.flags = 2;

	xcb_change_property_checked (xwpsb->connection,
					XCB_PROP_MODE_REPLACE,
					window,
					xwpsb->atom._motif_wm_hints,
					xwpsb->atom._motif_wm_hints,
					32,
					5, &hints );

	xcb_void_cookie_t map_cookie = xcb_map_window_checked(xwpsb->connection, window);

	xcb_flush(xwpsb->connection);

	xcb_generic_error_t *error = xcb_request_check(xwpsb->connection, create_cookie);
	if (error) {
		fprintf(stderr, "Failed to create X window: %d\n", error->error_code);
		return;
	}

	error = xcb_request_check(xwpsb->connection, map_cookie);
	if (error) {
		fprintf(stderr, "Failed to map X window: %d\n", error->error_code);
		return;
	}

	egl_surface = eglCreateWindowSurface(xwpsb->egl_display,
						xwpsb->egl_config,
						window,
						egl_surface_attribs);
	if (!egl_surface) {
		fprintf(stderr, "eglCreateWindowSurface() failed\n");
		return;
	}

	glGenTextures(1, &xwpsb_window->texture);

	view = container_of(xwpsb_window->surface->views.next,
				struct weston_view, surface_link);

	xwpsb_window->egl_surface = egl_surface;
	xwpsb_window->window = window;
	xwpsb_window->wx = view->geometry.x;
	xwpsb_window->wy = view->geometry.y;
	xwpsb_window->width = width;
	xwpsb_window->height = height;
	xwpsb_window->first_attach = 1;
	xwpsb_window->y_inverted = EGL_TRUE;

	xwpsb_set_title(xwpsb, xwpsb_window->surface, xwpsb_window->title);
}

static struct xwpsb_window *
xwpsb_create_window(struct xwpsb *xwpsb)
{
	struct xwpsb_window *xwpsb_window;

	xwpsb_window = zalloc(sizeof (struct xwpsb_window));
	if (!xwpsb_window)
		return NULL;

	return xwpsb_window;
}

static void
xwpsb_resize_window(struct xwpsb_window *xwpsb_window, uint32_t width, uint32_t height)
{
	struct xwpsb *xwpsb = xwpsb_window->xwpsb;

	uint32_t values[4];
	uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;

	values[0] = xwpsb_window->x;
	values[1] = xwpsb_window->y;

	xwpsb_window->width = values[2] = width;
	xwpsb_window->height = values[3] = height;

	if (xwpsb_window->edges) {
		if (xwpsb_window->edges & WL_SHELL_SURFACE_RESIZE_LEFT)
			values[0] = xwpsb_window->x +
				(xwpsb_window->last_width - xwpsb_window->width);
		if (xwpsb_window->edges & WL_SHELL_SURFACE_RESIZE_TOP)
			values[1] = xwpsb_window->y +
				(xwpsb_window->last_height - xwpsb_window->height);
	}

	xwpsb_window->last_width = xwpsb_window->width;
	xwpsb_window->last_height = xwpsb_window->height;

	xcb_configure_window(xwpsb->connection, xwpsb_window->window,
				mask | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
				values);
	xcb_flush(xwpsb->connection);
	xwpsb_window->resized = 1;
}

WL_EXPORT void
xwpsb_added_notify(void *data, struct weston_surface *surface)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;

	xwpsb_window = xwpsb_create_window(xwpsb);

	xwpsb_window->xwpsb = xwpsb;
	xwpsb_window->surface = surface;

	wl_list_insert(&xwpsb->surfaces, &xwpsb_window->link);
}

WL_EXPORT void
xwpsb_removed_notify(void *data, struct weston_surface *surface)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window, *tmp;

	wl_list_for_each_safe(xwpsb_window, tmp, &xwpsb->surfaces, link) {
		if (surface == xwpsb_window->surface) {
			destroy_xwpsb_window(xwpsb_window);
			break;
		}
	}
}

WL_EXPORT void
xwpsb_committed_notify(void *data, struct weston_surface *surface)
{
	struct xwpsb *xwpsb = (struct xwpsb *) data;
	struct xwpsb_window *xwpsb_window;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window))
		return;

	if (xwpsb_window->width != surface->width ||
			xwpsb_window->height != surface->height)
		xwpsb_resize_window(xwpsb_window, surface->width, surface->height);
}

static void
xwpsb_attach_egl(struct xwpsb_window *xwpsb_window,
			struct weston_buffer *buffer,
			uint32_t format)
{
	struct xwpsb *xwpsb = xwpsb_window->xwpsb;
	EGLint attribs[3];

	/* We're relying on gl renderer to set this */
	// buffer->legacy_buffer = (struct wl_buffer *)buffer->resource;

	if (xwpsb_window->img_ref) {
		xwpsb->destroy_image(xwpsb->egl_display, xwpsb_window->image);
		xwpsb_window->img_ref = 0;
	}

	if (format != EGL_TEXTURE_RGBA)
		return;

	if (!xwpsb->query_buffer(xwpsb->egl_display, (void *)buffer->resource,
					EGL_WAYLAND_Y_INVERTED_WL,
					&xwpsb_window->y_inverted))
		xwpsb_window->y_inverted = EGL_TRUE;

	attribs[0] = EGL_WAYLAND_PLANE_WL;
	attribs[1] = 0;
	attribs[2] = EGL_NONE;

	xwpsb_window->image = xwpsb->create_image(xwpsb->egl_display,
					EGL_NO_CONTEXT,
					EGL_WAYLAND_BUFFER_WL,
					(EGLClientBuffer) buffer->legacy_buffer,
					attribs);
	if (xwpsb_window->image == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "Failed to create egl image\n");
		return;
	} else {
		xwpsb_window->img_ref = 1;
	}

	eglMakeCurrent(xwpsb->egl_display, xwpsb_window->egl_surface, xwpsb_window->egl_surface, xwpsb->egl_context);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, xwpsb_window->texture);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	xwpsb->image_target_texture_2d(GL_TEXTURE_2D, xwpsb_window->image);

	draw(xwpsb_window);

	weston_buffer_reference(&xwpsb_window->buffer_ref, NULL);
}

void
xwpsb_flush_damage_notify(struct weston_surface *surface)
{
	struct xwpsb *xwpsb = (struct xwpsb *) surface->compositor->xwpsb;
	struct xwpsb_window *xwpsb_window;
	struct weston_buffer *buffer;
	uint8_t *data;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window))
		return;

	buffer = xwpsb_window->buffer_ref.buffer;

	if (!buffer)
		return;

	data = wl_shm_buffer_get_data(buffer->shm_buffer);

	eglMakeCurrent(xwpsb->egl_display, xwpsb_window->egl_surface, xwpsb_window->egl_surface, xwpsb->egl_context);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, xwpsb_window->texture);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,
			GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	wl_shm_buffer_begin_access(buffer->shm_buffer);

	glBindTexture(GL_TEXTURE_2D, xwpsb_window->texture);
	glTexImage2D(GL_TEXTURE_2D, 0,
		     GL_RGBA,
		     xwpsb_window->pitch,
		     buffer->height,
		     0,
		     GL_RGBA,
		     GL_UNSIGNED_BYTE,
		     data);

	wl_shm_buffer_end_access(buffer->shm_buffer);

	draw(xwpsb_window);

	weston_buffer_reference(&xwpsb_window->buffer_ref, NULL);
}

static void
xwpsb_attach_shm(struct weston_surface *surface, struct weston_buffer *buffer,
		       struct wl_shm_buffer *shm_buffer)
{
	struct xwpsb *xwpsb = (struct xwpsb *) surface->compositor->xwpsb;
	struct xwpsb_window *xwpsb_window;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window))
		return;

	buffer->shm_buffer = shm_buffer;
	buffer->width = wl_shm_buffer_get_width(shm_buffer);
	buffer->height = wl_shm_buffer_get_height(shm_buffer);

	xwpsb_window->pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
}

void
xwpsb_attach_notify(struct weston_surface *surface,
			struct weston_buffer *buffer)
{
	struct xwpsb *xwpsb = (struct xwpsb *) surface->compositor->xwpsb;
	struct xwpsb_window *xwpsb_window;
	struct wl_shm_buffer *shm_buffer;
	EGLint format;

	if (!get_window_from_surface(xwpsb, surface, &xwpsb_window) ||
		surface->width == 0 || surface->height == 0)
		return;

	if (xwpsb_window->width == 0 && xwpsb_window->height == 0 &&
		xwpsb_window->window == 0) {
		weston_log("Creating new xwpsb_window for surface: %p\n", surface);
		xwpsb_create_x11_window(xwpsb_window, surface->width, surface->height);
	}

	weston_buffer_reference(&xwpsb_window->buffer_ref, buffer);

	shm_buffer = wl_shm_buffer_get(buffer->resource);

	if (shm_buffer) {
		xwpsb_attach_shm(surface, buffer, shm_buffer);
	} else if (xwpsb->query_buffer(xwpsb->egl_display, (void *)buffer->resource,
				  EGL_TEXTURE_FORMAT, &format)) {
		xwpsb_attach_egl(xwpsb_window, buffer, format);
	} else {
		fprintf(stderr, "unhandled buffer type\n");
		weston_buffer_reference(&xwpsb_window->buffer_ref, NULL);
	}
}

static void
xwpsb_setup_xkb(struct xwpsb *xwpsb)
{
#ifndef HAVE_XCB_XKB
	weston_log("XCB-XKB not available during build\n");
	xwpsb->has_xkb = 0;
	xwpsb->xkb_event_base = 0;
	return;
#else
	struct weston_keyboard *keyboard;
	const xcb_query_extension_reply_t *ext;
	xcb_generic_error_t *error;
	xcb_void_cookie_t select;
	xcb_xkb_use_extension_cookie_t use_ext;
	xcb_xkb_use_extension_reply_t *use_ext_reply;
	xcb_xkb_per_client_flags_cookie_t pcf;
	xcb_xkb_per_client_flags_reply_t *pcf_reply;
	xcb_xkb_get_state_cookie_t state;
	xcb_xkb_get_state_reply_t *state_reply;
	uint32_t values[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE };

	xwpsb->has_xkb = 0;
	xwpsb->xkb_event_base = 0;

	ext = xcb_get_extension_data(xwpsb->connection, &xcb_xkb_id);
	if (!ext) {
		weston_log("XKB extension not available on host X11 server\n");
		return;
	}
	xwpsb->xkb_event_base = ext->first_event;

	select = xcb_xkb_select_events_checked(xwpsb->connection,
					       XCB_XKB_ID_USE_CORE_KBD,
					       XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
					       0,
					       XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
					       0,
					       0,
					       NULL);
	error = xcb_request_check(xwpsb->connection, select);
	if (error) {
		weston_log("error: failed to select for XKB state events\n");
		free(error);
		return;
	}

	use_ext = xcb_xkb_use_extension(xwpsb->connection,
					XCB_XKB_MAJOR_VERSION,
					XCB_XKB_MINOR_VERSION);
	use_ext_reply = xcb_xkb_use_extension_reply(xwpsb->connection, use_ext, NULL);
	if (!use_ext_reply) {
		weston_log("couldn't start using XKB extension\n");
		return;
	}

	if (!use_ext_reply->supported) {
		weston_log("XKB extension version on the server is too old "
			   "(want %d.%d, has %d.%d)\n",
			   XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION,
			   use_ext_reply->serverMajor, use_ext_reply->serverMinor);
		free(use_ext_reply);
		return;
	}
	free(use_ext_reply);

	pcf = xcb_xkb_per_client_flags(xwpsb->connection,
				       XCB_XKB_ID_USE_CORE_KBD,
				       XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
				       XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
				       0,
				       0,
				       0);
	pcf_reply = xcb_xkb_per_client_flags_reply(xwpsb->connection, pcf, NULL);
	if (!pcf_reply ||
	    !(pcf_reply->value & XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT)) {
		weston_log("failed to set XKB per-client flags, not using "
			   "detectable repeat\n");
		free(pcf_reply);
		return;
	}
	free(pcf_reply);

	state = xcb_xkb_get_state(xwpsb->connection, XCB_XKB_ID_USE_CORE_KBD);
	state_reply = xcb_xkb_get_state_reply(xwpsb->connection, state, NULL);
	if (!state_reply) {
		weston_log("failed to get initial XKB state\n");
		return;
	}

	keyboard = weston_seat_get_keyboard(&xwpsb->core_seat);
	xkb_state_update_mask(keyboard->xkb_state.state,
			      get_xkb_mod_mask(xwpsb, state_reply->baseMods),
			      get_xkb_mod_mask(xwpsb, state_reply->latchedMods),
			      get_xkb_mod_mask(xwpsb, state_reply->lockedMods),
			      0,
			      0,
			      state_reply->group);

	free(state_reply);

	xcb_change_window_attributes(xwpsb->connection, xwpsb->screen->root,
				     XCB_CW_EVENT_MASK, values);

	xwpsb->has_xkb = 1;
#endif
}

WL_EXPORT void *
xwpsb_init(struct weston_compositor *compositor)
{
	struct wl_event_loop *loop;
	struct wl_event_source *source;
	struct xkb_keymap *keymap;
	struct xwpsb *xwpsb;

	xwpsb = zalloc(sizeof (struct xwpsb));
	if (!xwpsb)
		return NULL;

	setup_x(xwpsb);
	setup_egl(xwpsb, compositor->egl_display);

	wl_list_init(&xwpsb->surfaces);

	xwpsb->compositor = compositor;

	weston_seat_init(&xwpsb->core_seat, compositor, "default");

	weston_seat_init_pointer(&xwpsb->core_seat);

	keymap = xwpsb_get_keymap(xwpsb);
	if (weston_seat_init_keyboard(&xwpsb->core_seat, keymap) < 0)
		fprintf(stderr, "Failed to init keyboard with keymap\n");
	xkb_keymap_unref(keymap);

	xwpsb_setup_xkb(xwpsb);

	loop = wl_display_get_event_loop(compositor->wl_display);
	source = wl_event_loop_add_fd(loop,
					xcb_get_file_descriptor(xwpsb->connection),
					WL_EVENT_READABLE,
					xwpsb_handle_event, xwpsb);
	wl_event_source_check(source);

	return (void *) xwpsb;
}
