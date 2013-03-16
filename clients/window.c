/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2012-2013 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include "../config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <cairo.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#ifdef HAVE_CAIRO_EGL
#include <wayland-egl.h>

#ifdef USE_CAIRO_GLESV2
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <GL/gl.h>
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cairo-gl.h>
#else /* HAVE_CAIRO_EGL */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#endif /* no HAVE_CAIRO_EGL */

#include <xkbcommon/xkbcommon.h>
#include <wayland-cursor.h>

#include <linux/input.h>
#include <wayland-client.h>
#include "../shared/cairo-util.h"
#include "text-cursor-position-client-protocol.h"
#include "workspaces-client-protocol.h"
#include "../shared/os-compatibility.h"

#include "window.h"

struct shm_pool;

struct global {
	uint32_t name;
	char *interface;
	uint32_t version;
	struct wl_list link;
};

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shell *shell;
	struct wl_shm *shm;
	struct wl_data_device_manager *data_device_manager;
	struct text_cursor_position *text_cursor_position;
	struct workspace_manager *workspace_manager;
	EGLDisplay dpy;
	EGLConfig argb_config;
	EGLContext argb_ctx;
	cairo_device_t *argb_device;
	uint32_t serial;

	int display_fd;
	uint32_t display_fd_events;
	struct task display_task;

	int epoll_fd;
	struct wl_list deferred_list;

	int running;

	struct wl_list global_list;
	struct wl_list window_list;
	struct wl_list input_list;
	struct wl_list output_list;

	struct theme *theme;

	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor **cursors;

	display_output_handler_t output_configure_handler;
	display_global_handler_t global_handler;

	void *user_data;

	struct xkb_context *xkb_context;

	uint32_t workspace;
	uint32_t workspace_count;

	/* A hack to get text extents for tooltips */
	cairo_surface_t *dummy_surface;
	void *dummy_surface_data;
};

enum {
	TYPE_NONE,
	TYPE_TOPLEVEL,
	TYPE_FULLSCREEN,
	TYPE_MAXIMIZED,
	TYPE_TRANSIENT,
	TYPE_MENU,
	TYPE_CUSTOM
};

struct window_output {
	struct output *output;
	struct wl_list link;
};

struct toysurface {
	/*
	 * Prepare the surface for drawing. Makes sure there is a surface
	 * of the right size available for rendering, and returns it.
	 * dx,dy are the x,y of wl_surface.attach.
	 * width,height are the new surface size.
	 * If flags has SURFACE_HINT_RESIZE set, the user is
	 * doing continuous resizing.
	 * Returns the Cairo surface to draw to.
	 */
	cairo_surface_t *(*prepare)(struct toysurface *base, int dx, int dy,
				    int width, int height, uint32_t flags);

	/*
	 * Post the surface to the server, returning the server allocation
	 * rectangle. The Cairo surface from prepare() must be destroyed
	 * after calling this.
	 */
	void (*swap)(struct toysurface *base,
		     struct rectangle *server_allocation);

	/*
	 * Make the toysurface current with the given EGL context.
	 * Returns 0 on success, and negative of failure.
	 */
	int (*acquire)(struct toysurface *base, EGLContext ctx);

	/*
	 * Release the toysurface from the EGL context, returning control
	 * to Cairo.
	 */
	void (*release)(struct toysurface *base);

	/*
	 * Destroy the toysurface, including the Cairo surface, any
	 * backing storage, and the Wayland protocol objects.
	 */
	void (*destroy)(struct toysurface *base);
};

struct surface {
	struct window *window;

	struct wl_surface *surface;
	struct toysurface *toysurface;
	struct widget *widget;

	struct rectangle allocation;
	struct rectangle server_allocation;

	struct wl_region *input_region;
	struct wl_region *opaque_region;

	enum window_buffer_type buffer_type;
	enum wl_output_transform buffer_transform;

	cairo_surface_t *cairo_surface;
};

struct window {
	struct display *display;
	struct window *parent;
	struct wl_list window_output_list;
	char *title;
	struct rectangle saved_allocation;
	struct rectangle min_allocation;
	struct rectangle pending_allocation;
	int x, y;
	int resize_edges;
	int redraw_scheduled;
	int redraw_needed;
	struct task redraw_task;
	int resize_needed;
	int saved_type;
	int type;
	int focus_count;
	int minimized;

	int resizing;
	int fullscreen_method;

	window_key_handler_t key_handler;
	window_keyboard_focus_handler_t keyboard_focus_handler;
	window_data_handler_t data_handler;
	window_drop_handler_t drop_handler;
	window_close_handler_t close_handler;
	window_fullscreen_handler_t fullscreen_handler;
	window_output_handler_t output_handler;

	struct surface *main_surface;
	struct wl_shell_surface *shell_surface;
	struct wl_callback *frame_cb;

	struct frame *frame;

	void *user_data;
	struct wl_list link;
};

struct widget {
	struct window *window;
	struct surface *surface;
	struct tooltip *tooltip;
	struct wl_list child_list;
	struct wl_list link;
	struct rectangle allocation;
	widget_resize_handler_t resize_handler;
	widget_redraw_handler_t redraw_handler;
	widget_enter_handler_t enter_handler;
	widget_leave_handler_t leave_handler;
	widget_motion_handler_t motion_handler;
	widget_button_handler_t button_handler;
	widget_axis_handler_t axis_handler;
	void *user_data;
	int opaque;
	int tooltip_count;
	int default_cursor;
};

struct input {
	struct display *display;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct window *pointer_focus;
	struct window *keyboard_focus;
	int current_cursor;
	uint32_t cursor_anim_start;
	struct wl_callback *cursor_frame_cb;
	struct wl_surface *pointer_surface;
	uint32_t modifiers;
	uint32_t pointer_enter_serial;
	uint32_t cursor_serial;
	float sx, sy;
	struct wl_list link;

	struct widget *focus_widget;
	struct widget *grab;
	uint32_t grab_button;

	struct wl_data_device *data_device;
	struct data_offer *drag_offer;
	struct data_offer *selection_offer;

	struct {
		struct xkb_keymap *keymap;
		struct xkb_state *state;
		xkb_mod_mask_t control_mask;
		xkb_mod_mask_t alt_mask;
		xkb_mod_mask_t shift_mask;
	} xkb;

	struct task repeat_task;
	int repeat_timer_fd;
	uint32_t repeat_sym;
	uint32_t repeat_key;
	uint32_t repeat_time;
};

struct output {
	struct display *display;
	struct wl_output *output;
	struct rectangle allocation;
	struct wl_list link;
	int transform;

	display_output_handler_t destroy_handler;
	void *user_data;
};

enum frame_button_action {
	FRAME_BUTTON_NULL = 0,
	FRAME_BUTTON_ICON = 1,
	FRAME_BUTTON_CLOSE = 2,
	FRAME_BUTTON_MINIMIZE = 3,
	FRAME_BUTTON_MAXIMIZE = 4,
};

enum frame_button_pointer {
	FRAME_BUTTON_DEFAULT = 0,
	FRAME_BUTTON_OVER = 1,
	FRAME_BUTTON_ACTIVE = 2,
};

enum frame_button_align {
	FRAME_BUTTON_RIGHT = 0,
	FRAME_BUTTON_LEFT = 1,
};

enum frame_button_decoration {
	FRAME_BUTTON_NONE = 0,
	FRAME_BUTTON_FANCY = 1,
};

struct frame_button {
	struct widget *widget;
	struct frame *frame;
	cairo_surface_t *icon;
	enum frame_button_action type;
	enum frame_button_pointer state;
	struct wl_list link;	/* buttons_list */
	enum frame_button_align align;
	enum frame_button_decoration decoration;
};

struct frame {
	struct widget *widget;
	struct widget *child;
	struct wl_list buttons_list;
};

struct menu {
	struct window *window;
	struct widget *widget;
	struct input *input;
	const char **entries;
	uint32_t time;
	int current;
	int count;
	int release_count;
	menu_func_t func;
};

struct tooltip {
	struct widget *parent;
	struct window *window;
	struct widget *widget;
	char *entry;
	struct task tooltip_task;
	int tooltip_fd;
	float x, y;
};

struct shm_pool {
	struct wl_shm_pool *pool;
	size_t size;
	size_t used;
	void *data;
};

enum {
	CURSOR_DEFAULT = 100,
	CURSOR_UNSET
};

enum window_location {
	WINDOW_INTERIOR = 0,
	WINDOW_RESIZING_TOP = 1,
	WINDOW_RESIZING_BOTTOM = 2,
	WINDOW_RESIZING_LEFT = 4,
	WINDOW_RESIZING_TOP_LEFT = 5,
	WINDOW_RESIZING_BOTTOM_LEFT = 6,
	WINDOW_RESIZING_RIGHT = 8,
	WINDOW_RESIZING_TOP_RIGHT = 9,
	WINDOW_RESIZING_BOTTOM_RIGHT = 10,
	WINDOW_RESIZING_MASK = 15,
	WINDOW_EXTERIOR = 16,
	WINDOW_TITLEBAR = 17,
	WINDOW_CLIENT_AREA = 18,
};

static const cairo_user_data_key_t shm_surface_data_key;

#ifdef HAVE_CAIRO_EGL

struct egl_window_surface {
	struct toysurface base;
	cairo_surface_t *cairo_surface;
	struct display *display;
	struct wl_surface *surface;
	struct wl_egl_window *egl_window;
	EGLSurface egl_surface;
};

static struct egl_window_surface *
to_egl_window_surface(struct toysurface *base)
{
	return container_of(base, struct egl_window_surface, base);
}

static cairo_surface_t *
egl_window_surface_prepare(struct toysurface *base, int dx, int dy,
			   int width, int height, uint32_t flags)
{
	struct egl_window_surface *surface = to_egl_window_surface(base);

	wl_egl_window_resize(surface->egl_window, width, height, dx, dy);
	cairo_gl_surface_set_size(surface->cairo_surface, width, height);

	return cairo_surface_reference(surface->cairo_surface);
}

static void
egl_window_surface_swap(struct toysurface *base,
			struct rectangle *server_allocation)
{
	struct egl_window_surface *surface = to_egl_window_surface(base);

	cairo_gl_surface_swapbuffers(surface->cairo_surface);
	wl_egl_window_get_attached_size(surface->egl_window,
					&server_allocation->width,
					&server_allocation->height);
}

static int
egl_window_surface_acquire(struct toysurface *base, EGLContext ctx)
{
	struct egl_window_surface *surface = to_egl_window_surface(base);
	cairo_device_t *device;

	device = cairo_surface_get_device(surface->cairo_surface);
	if (!device)
		return -1;

	if (!ctx) {
		if (device == surface->display->argb_device)
			ctx = surface->display->argb_ctx;
		else
			assert(0);
	}

	cairo_device_flush(device);
	cairo_device_acquire(device);
	if (!eglMakeCurrent(surface->display->dpy, surface->egl_surface,
			    surface->egl_surface, ctx))
		fprintf(stderr, "failed to make surface current\n");

	return 0;
}

static void
egl_window_surface_release(struct toysurface *base)
{
	struct egl_window_surface *surface = to_egl_window_surface(base);
	cairo_device_t *device;

	device = cairo_surface_get_device(surface->cairo_surface);
	if (!device)
		return;

	if (!eglMakeCurrent(surface->display->dpy, NULL, NULL,
			    surface->display->argb_ctx))
		fprintf(stderr, "failed to make context current\n");

	cairo_device_release(device);
}

static void
egl_window_surface_destroy(struct toysurface *base)
{
	struct egl_window_surface *surface = to_egl_window_surface(base);
	struct display *d = surface->display;

	cairo_surface_destroy(surface->cairo_surface);
	eglDestroySurface(d->dpy, surface->egl_surface);
	wl_egl_window_destroy(surface->egl_window);
	surface->surface = NULL;

	free(surface);
}

static struct toysurface *
egl_window_surface_create(struct display *display,
			  struct wl_surface *wl_surface,
			  uint32_t flags,
			  struct rectangle *rectangle)
{
	struct egl_window_surface *surface;

	if (display->dpy == EGL_NO_DISPLAY)
		return NULL;

	surface = calloc(1, sizeof *surface);
	if (!surface)
		return NULL;

	surface->base.prepare = egl_window_surface_prepare;
	surface->base.swap = egl_window_surface_swap;
	surface->base.acquire = egl_window_surface_acquire;
	surface->base.release = egl_window_surface_release;
	surface->base.destroy = egl_window_surface_destroy;

	surface->display = display;
	surface->surface = wl_surface;

	surface->egl_window = wl_egl_window_create(surface->surface,
						   rectangle->width,
						   rectangle->height);

	surface->egl_surface = eglCreateWindowSurface(display->dpy,
						      display->argb_config,
						      surface->egl_window,
						      NULL);

	surface->cairo_surface =
		cairo_gl_surface_create_for_egl(display->argb_device,
						surface->egl_surface,
						rectangle->width,
						rectangle->height);

	return &surface->base;
}

#else

static struct toysurface *
egl_window_surface_create(struct display *display,
			  struct wl_surface *wl_surface,
			  uint32_t flags,
			  struct rectangle *rectangle)
{
	return NULL;
}

#endif

struct shm_surface_data {
	struct wl_buffer *buffer;
	struct shm_pool *pool;
};

struct wl_buffer *
display_get_buffer_for_surface(struct display *display,
			       cairo_surface_t *surface)
{
	struct shm_surface_data *data;

	data = cairo_surface_get_user_data(surface, &shm_surface_data_key);

	return data->buffer;
}

static void
shm_pool_destroy(struct shm_pool *pool);

static void
shm_surface_data_destroy(void *p)
{
	struct shm_surface_data *data = p;

	wl_buffer_destroy(data->buffer);
	if (data->pool)
		shm_pool_destroy(data->pool);

	free(data);
}

static struct wl_shm_pool *
make_shm_pool(struct display *display, int size, void **data)
{
	struct wl_shm_pool *pool;
	int fd;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n",
			size);
		return NULL;
	}

	*data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return NULL;
	}

	pool = wl_shm_create_pool(display->shm, fd, size);

	close(fd);

	return pool;
}

static struct shm_pool *
shm_pool_create(struct display *display, size_t size)
{
	struct shm_pool *pool = malloc(sizeof *pool);

	if (!pool)
		return NULL;

	pool->pool = make_shm_pool(display, size, &pool->data);
	if (!pool->pool) {
		free(pool);
		return NULL;
	}

	pool->size = size;
	pool->used = 0;

	return pool;
}

static void *
shm_pool_allocate(struct shm_pool *pool, size_t size, int *offset)
{
	if (pool->used + size > pool->size)
		return NULL;

	*offset = pool->used;
	pool->used += size;

	return (char *) pool->data + *offset;
}

/* destroy the pool. this does not unmap the memory though */
static void
shm_pool_destroy(struct shm_pool *pool)
{
	munmap(pool->data, pool->size);
	wl_shm_pool_destroy(pool->pool);
	free(pool);
}

/* Start allocating from the beginning of the pool again */
static void
shm_pool_reset(struct shm_pool *pool)
{
	pool->used = 0;
}

static int
data_length_for_shm_surface(struct rectangle *rect)
{
	int stride;

	stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32,
						rect->width);
	return stride * rect->height;
}

static cairo_surface_t *
display_create_shm_surface_from_pool(struct display *display,
				     struct rectangle *rectangle,
				     uint32_t flags, struct shm_pool *pool)
{
	struct shm_surface_data *data;
	uint32_t format;
	cairo_surface_t *surface;
	int stride, length, offset;
	void *map;

	data = malloc(sizeof *data);
	if (data == NULL)
		return NULL;

	stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32,
						rectangle->width);
	length = stride * rectangle->height;
	data->pool = NULL;
	map = shm_pool_allocate(pool, length, &offset);

	if (!map) {
		free(data);
		return NULL;
	}

	surface = cairo_image_surface_create_for_data (map,
						       CAIRO_FORMAT_ARGB32,
						       rectangle->width,
						       rectangle->height,
						       stride);

	cairo_surface_set_user_data(surface, &shm_surface_data_key,
				    data, shm_surface_data_destroy);

	if (flags & SURFACE_OPAQUE)
		format = WL_SHM_FORMAT_XRGB8888;
	else
		format = WL_SHM_FORMAT_ARGB8888;

	data->buffer = wl_shm_pool_create_buffer(pool->pool, offset,
						 rectangle->width,
						 rectangle->height,
						 stride, format);

	return surface;
}

static cairo_surface_t *
display_create_shm_surface(struct display *display,
			   struct rectangle *rectangle, uint32_t flags,
			   struct shm_pool *alternate_pool,
			   struct shm_surface_data **data_ret)
{
	struct shm_surface_data *data;
	struct shm_pool *pool;
	cairo_surface_t *surface;

	if (alternate_pool) {
		shm_pool_reset(alternate_pool);
		surface = display_create_shm_surface_from_pool(display,
							       rectangle,
							       flags,
							       alternate_pool);
		if (surface) {
			data = cairo_surface_get_user_data(surface,
							   &shm_surface_data_key);
			goto out;
		}
	}

	pool = shm_pool_create(display,
			       data_length_for_shm_surface(rectangle));
	if (!pool)
		return NULL;

	surface =
		display_create_shm_surface_from_pool(display, rectangle,
						     flags, pool);

	if (!surface) {
		shm_pool_destroy(pool);
		return NULL;
	}

	/* make sure we destroy the pool when the surface is destroyed */
	data = cairo_surface_get_user_data(surface, &shm_surface_data_key);
	data->pool = pool;

out:
	if (data_ret)
		*data_ret = data;

	return surface;
}

static int
check_size(struct rectangle *rect)
{
	if (rect->width && rect->height)
		return 0;

	fprintf(stderr, "tried to create surface of "
		"width: %d, height: %d\n", rect->width, rect->height);
	return -1;
}

cairo_surface_t *
display_create_surface(struct display *display,
		       struct wl_surface *surface,
		       struct rectangle *rectangle,
		       uint32_t flags)
{
	if (check_size(rectangle) < 0)
		return NULL;

	assert(flags & SURFACE_SHM);
	return display_create_shm_surface(display, rectangle, flags,
					  NULL, NULL);
}

struct shm_surface_leaf {
	cairo_surface_t *cairo_surface;
	/* 'data' is automatically destroyed, when 'cairo_surface' is */
	struct shm_surface_data *data;

	struct shm_pool *resize_pool;
	int busy;
};

static void
shm_surface_leaf_release(struct shm_surface_leaf *leaf)
{
	if (leaf->cairo_surface)
		cairo_surface_destroy(leaf->cairo_surface);
	/* leaf->data already destroyed via cairo private */

	if (leaf->resize_pool)
		shm_pool_destroy(leaf->resize_pool);

	memset(leaf, 0, sizeof *leaf);
}

struct shm_surface {
	struct toysurface base;
	struct display *display;
	struct wl_surface *surface;
	uint32_t flags;
	int dx, dy;

	struct shm_surface_leaf leaf[2];
	struct shm_surface_leaf *current;
};

static struct shm_surface *
to_shm_surface(struct toysurface *base)
{
	return container_of(base, struct shm_surface, base);
}

static void
shm_surface_buffer_release(void *data, struct wl_buffer *buffer)
{
	struct shm_surface *surface = data;

	if (surface->leaf[0].data->buffer == buffer)
		surface->leaf[0].busy = 0;
	else if (surface->leaf[1].data->buffer == buffer)
		surface->leaf[1].busy = 0;
	else
		assert(0 && "shm_surface_buffer_release: unknown buffer");

	if (!surface->leaf[0].busy && !surface->leaf[1].busy)
		shm_surface_leaf_release(&surface->leaf[1]);
}

static const struct wl_buffer_listener shm_surface_buffer_listener = {
	shm_surface_buffer_release
};

static cairo_surface_t *
shm_surface_prepare(struct toysurface *base, int dx, int dy,
		    int width, int height, uint32_t flags)
{
	int resize_hint = !!(flags & SURFACE_HINT_RESIZE);
	struct shm_surface *surface = to_shm_surface(base);
	struct rectangle rect = { 0, 0, width, height };
	struct shm_surface_leaf *leaf;

	surface->dx = dx;
	surface->dy = dy;

	/* See shm_surface_buffer_release() */
	if (!surface->leaf[0].busy && !surface->leaf[1].busy &&
	    surface->leaf[1].cairo_surface) {
		fprintf(stderr, "window.c:%s: TODO: release leaf[1]\n",
			__func__);
	}

	/* pick a free buffer from the two */
	if (!surface->leaf[0].busy)
		leaf = &surface->leaf[0];
	else if (!surface->leaf[1].busy)
		leaf = &surface->leaf[1];
	else {
		fprintf(stderr, "%s: both buffers are held by the server.\n",
			__func__);
		return NULL;
	}

	if (!resize_hint && leaf->resize_pool) {
		cairo_surface_destroy(leaf->cairo_surface);
		leaf->cairo_surface = NULL;
		shm_pool_destroy(leaf->resize_pool);
		leaf->resize_pool = NULL;
	}

	if (leaf->cairo_surface &&
	    cairo_image_surface_get_width(leaf->cairo_surface) == width &&
	    cairo_image_surface_get_height(leaf->cairo_surface) == height)
		goto out;

	if (leaf->cairo_surface)
		cairo_surface_destroy(leaf->cairo_surface);

	if (resize_hint && !leaf->resize_pool) {
		/* Create a big pool to allocate from, while continuously
		 * resizing. Mmapping a new pool in the server
		 * is relatively expensive, so reusing a pool performs
		 * better, but may temporarily reserve unneeded memory.
		 */
		/* We should probably base this number on the output size. */
		leaf->resize_pool = shm_pool_create(surface->display,
						    6 * 1024 * 1024);
	}

	leaf->cairo_surface =
		display_create_shm_surface(surface->display, &rect,
					   surface->flags,
					   leaf->resize_pool,
					   &leaf->data);
	wl_buffer_add_listener(leaf->data->buffer,
			       &shm_surface_buffer_listener, surface);

out:
	surface->current = leaf;

	return cairo_surface_reference(leaf->cairo_surface);
}

static void
shm_surface_swap(struct toysurface *base,
		 struct rectangle *server_allocation)
{
	struct shm_surface *surface = to_shm_surface(base);
	struct shm_surface_leaf *leaf = surface->current;

	server_allocation->width =
		cairo_image_surface_get_width(leaf->cairo_surface);
	server_allocation->height =
		cairo_image_surface_get_height(leaf->cairo_surface);

	wl_surface_attach(surface->surface, leaf->data->buffer,
			  surface->dx, surface->dy);
	wl_surface_damage(surface->surface, 0, 0,
			  server_allocation->width, server_allocation->height);
	wl_surface_commit(surface->surface);

	leaf->busy = 1;
	surface->current = NULL;
}

static int
shm_surface_acquire(struct toysurface *base, EGLContext ctx)
{
	return -1;
}

static void
shm_surface_release(struct toysurface *base)
{
}

static void
shm_surface_destroy(struct toysurface *base)
{
	struct shm_surface *surface = to_shm_surface(base);

	shm_surface_leaf_release(&surface->leaf[0]);
	shm_surface_leaf_release(&surface->leaf[1]);

	free(surface);
}

static struct toysurface *
shm_surface_create(struct display *display, struct wl_surface *wl_surface,
		   uint32_t flags, struct rectangle *rectangle)
{
	struct shm_surface *surface;

	surface = calloc(1, sizeof *surface);
	if (!surface)
		return NULL;

	surface->base.prepare = shm_surface_prepare;
	surface->base.swap = shm_surface_swap;
	surface->base.acquire = shm_surface_acquire;
	surface->base.release = shm_surface_release;
	surface->base.destroy = shm_surface_destroy;

	surface->display = display;
	surface->surface = wl_surface;
	surface->flags = flags;

	return &surface->base;
}

/*
 * The following correspondences between file names and cursors was copied
 * from: https://bugs.kde.org/attachment.cgi?id=67313
 */

static const char *bottom_left_corners[] = {
	"bottom_left_corner",
	"sw-resize"
};

static const char *bottom_right_corners[] = {
	"bottom_right_corner",
	"se-resize"
};

static const char *bottom_sides[] = {
	"bottom_side",
	"s-resize"
};

static const char *grabbings[] = {
	"grabbing",
	"closedhand",
	"208530c400c041818281048008011002"
};

static const char *left_ptrs[] = {
	"left_ptr",
	"default",
	"top_left_arrow",
	"left-arrow"
};

static const char *left_sides[] = {
	"left_side",
	"w-resize"
};

static const char *right_sides[] = {
	"right_side",
	"e-resize"
};

static const char *top_left_corners[] = {
	"top_left_corner",
	"nw-resize"
};

static const char *top_right_corners[] = {
	"top_right_corner",
	"ne-resize"
};

static const char *top_sides[] = {
	"top_side",
	"n-resize"
};

static const char *xterms[] = {
	"xterm",
	"ibeam",
	"text"
};

static const char *hand1s[] = {
	"hand1",
	"pointer",
	"pointing_hand",
	"e29285e634086352946a0e7090d73106"
};

static const char *watches[] = {
	"watch",
	"wait",
	"0426c94ea35c87780ff01dc239897213"
};

struct cursor_alternatives {
	const char **names;
	size_t count;
};

static const struct cursor_alternatives cursors[] = {
	{bottom_left_corners, ARRAY_LENGTH(bottom_left_corners)},
	{bottom_right_corners, ARRAY_LENGTH(bottom_right_corners)},
	{bottom_sides, ARRAY_LENGTH(bottom_sides)},
	{grabbings, ARRAY_LENGTH(grabbings)},
	{left_ptrs, ARRAY_LENGTH(left_ptrs)},
	{left_sides, ARRAY_LENGTH(left_sides)},
	{right_sides, ARRAY_LENGTH(right_sides)},
	{top_left_corners, ARRAY_LENGTH(top_left_corners)},
	{top_right_corners, ARRAY_LENGTH(top_right_corners)},
	{top_sides, ARRAY_LENGTH(top_sides)},
	{xterms, ARRAY_LENGTH(xterms)},
	{hand1s, ARRAY_LENGTH(hand1s)},
	{watches, ARRAY_LENGTH(watches)},
};

static void
create_cursors(struct display *display)
{
	char *config_file;
	char *theme = NULL;
	unsigned int i, j;
	struct wl_cursor *cursor;
	struct config_key shell_keys[] = {
		{ "cursor-theme", CONFIG_KEY_STRING, &theme },
	};
	struct config_section cs[] = {
		{ "shell", shell_keys, ARRAY_LENGTH(shell_keys), NULL },
	};

	config_file = config_file_path("weston.ini");
	parse_config_file(config_file, cs, ARRAY_LENGTH(cs), NULL);
	free(config_file);

	display->cursor_theme = wl_cursor_theme_load(theme, 32, display->shm);
	display->cursors =
		malloc(ARRAY_LENGTH(cursors) * sizeof display->cursors[0]);

	for (i = 0; i < ARRAY_LENGTH(cursors); i++) {
		cursor = NULL;
		for (j = 0; !cursor && j < cursors[i].count; ++j)
			cursor = wl_cursor_theme_get_cursor(
			    display->cursor_theme, cursors[i].names[j]);

		if (!cursor)
			fprintf(stderr, "could not load cursor '%s'\n",
				cursors[i].names[0]);

		display->cursors[i] = cursor;
	}
}

static void
destroy_cursors(struct display *display)
{
	wl_cursor_theme_destroy(display->cursor_theme);
	free(display->cursors);
}

struct wl_cursor_image *
display_get_pointer_image(struct display *display, int pointer)
{
	struct wl_cursor *cursor = display->cursors[pointer];

	return cursor ? cursor->images[0] : NULL;
}

static void
surface_flush(struct surface *surface)
{
	if (!surface->cairo_surface)
		return;

	if (surface->opaque_region) {
		wl_surface_set_opaque_region(surface->surface,
					     surface->opaque_region);
		wl_region_destroy(surface->opaque_region);
		surface->opaque_region = NULL;
	}

	if (surface->input_region) {
		wl_surface_set_input_region(surface->surface,
					    surface->input_region);
		wl_region_destroy(surface->input_region);
		surface->input_region = NULL;
	}

	surface->toysurface->swap(surface->toysurface,
				  &surface->server_allocation);

	cairo_surface_destroy(surface->cairo_surface);
	surface->cairo_surface = NULL;
}

int
window_has_focus(struct window *window)
{
	return window->focus_count > 0;
}

static void
window_flush(struct window *window)
{
	if (window->type == TYPE_NONE) {
		window->type = TYPE_TOPLEVEL;
		if (window->shell_surface)
			wl_shell_surface_set_toplevel(window->shell_surface);
	}

	surface_flush(window->main_surface);
}

struct display *
window_get_display(struct window *window)
{
	return window->display;
}

static void
surface_create_surface(struct surface *surface, int dx, int dy, uint32_t flags)
{
	struct display *display = surface->window->display;
	struct rectangle allocation = surface->allocation;

	switch (surface->buffer_transform) {
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		allocation.width = surface->allocation.height;
		allocation.height = surface->allocation.width;
		break;
	default:
		break;
	}

	if (!surface->toysurface && display->dpy &&
	    surface->buffer_type == WINDOW_BUFFER_TYPE_EGL_WINDOW) {
		surface->toysurface =
			egl_window_surface_create(display,
						  surface->surface,
						  flags,
						  &allocation);
	}

	if (!surface->toysurface)
		surface->toysurface = shm_surface_create(display,
							 surface->surface,
							 flags, &allocation);

	surface->cairo_surface = surface->toysurface->prepare(
		surface->toysurface, dx, dy,
		allocation.width, allocation.height, flags);
}

static void
window_create_main_surface(struct window *window)
{
	struct surface *surface = window->main_surface;
	uint32_t flags = 0;
	int dx = 0;
	int dy = 0;

	if (window->resizing)
		flags |= SURFACE_HINT_RESIZE;

	if (window->resize_edges & WINDOW_RESIZING_LEFT)
		dx = surface->server_allocation.width -
			surface->allocation.width;

	if (window->resize_edges & WINDOW_RESIZING_TOP)
		dy = surface->server_allocation.height -
			surface->allocation.height;

	window->resize_edges = 0;

	surface_create_surface(surface, dx, dy, flags);
}

int
window_get_buffer_transform(struct window *window)
{
	return window->main_surface->buffer_transform;
}

void
window_set_buffer_transform(struct window *window,
			    enum wl_output_transform transform)
{
	window->main_surface->buffer_transform = transform;
	wl_surface_set_buffer_transform(window->main_surface->surface,
					transform);
}

static void frame_destroy(struct frame *frame);

static void
surface_destroy(struct surface *surface)
{
	if (surface->input_region)
		wl_region_destroy(surface->input_region);

	if (surface->opaque_region)
		wl_region_destroy(surface->opaque_region);

	wl_surface_destroy(surface->surface);

	if (surface->toysurface)
		surface->toysurface->destroy(surface->toysurface);

	free(surface);
}

void
window_destroy(struct window *window)
{
	struct display *display = window->display;
	struct input *input;
	struct window_output *window_output;
	struct window_output *window_output_tmp;

	if (window->redraw_scheduled)
		wl_list_remove(&window->redraw_task.link);

	wl_list_for_each(input, &display->input_list, link) {
		if (input->pointer_focus == window)
			input->pointer_focus = NULL;
		if (input->keyboard_focus == window)
			input->keyboard_focus = NULL;
		if (input->focus_widget &&
		    input->focus_widget->window == window)
			input->focus_widget = NULL;
	}

	wl_list_for_each_safe(window_output, window_output_tmp,
			      &window->window_output_list, link) {
		free (window_output);
	}

	if (window->frame)
		frame_destroy(window->frame);

	if (window->shell_surface)
		wl_shell_surface_destroy(window->shell_surface);

	surface_destroy(window->main_surface);

	wl_list_remove(&window->link);

	if (window->frame_cb)
		wl_callback_destroy(window->frame_cb);
	free(window->title);
	free(window);
}

static struct widget *
widget_find_widget(struct widget *widget, int32_t x, int32_t y)
{
	struct widget *child, *target;

	wl_list_for_each(child, &widget->child_list, link) {
		target = widget_find_widget(child, x, y);
		if (target)
			return target;
	}

	if (widget->allocation.x <= x &&
	    x < widget->allocation.x + widget->allocation.width &&
	    widget->allocation.y <= y &&
	    y < widget->allocation.y + widget->allocation.height) {
		return widget;
	}

	return NULL;
}

static struct widget *
window_find_widget(struct window *window, int32_t x, int32_t y)
{
	return widget_find_widget(window->main_surface->widget, x, y);
}

static struct widget *
widget_create(struct window *window, struct surface *surface, void *data)
{
	struct widget *widget;

	widget = malloc(sizeof *widget);
	memset(widget, 0, sizeof *widget);
	widget->window = window;
	widget->surface = surface;
	widget->user_data = data;
	widget->allocation = surface->allocation;
	wl_list_init(&widget->child_list);
	widget->opaque = 0;
	widget->tooltip = NULL;
	widget->tooltip_count = 0;
	widget->default_cursor = CURSOR_LEFT_PTR;

	return widget;
}

struct widget *
window_add_widget(struct window *window, void *data)
{
	struct widget *widget;

	widget = widget_create(window, window->main_surface, data);
	wl_list_init(&widget->link);
	window->main_surface->widget = widget;

	return widget;
}

struct widget *
widget_add_widget(struct widget *parent, void *data)
{
	struct widget *widget;

	widget = widget_create(parent->window, parent->surface, data);
	wl_list_insert(parent->child_list.prev, &widget->link);

	return widget;
}

void
widget_destroy(struct widget *widget)
{
	struct display *display = widget->window->display;
	struct input *input;

	if (widget->tooltip) {
		free(widget->tooltip);
		widget->tooltip = NULL;
	}

	wl_list_for_each(input, &display->input_list, link) {
		if (input->focus_widget == widget)
			input->focus_widget = NULL;
	}

	wl_list_remove(&widget->link);
	free(widget);
}

void
widget_set_default_cursor(struct widget *widget, int cursor)
{
	widget->default_cursor = cursor;
}

void
widget_get_allocation(struct widget *widget, struct rectangle *allocation)
{
	*allocation = widget->allocation;
}

void
widget_set_size(struct widget *widget, int32_t width, int32_t height)
{
	widget->allocation.width = width;
	widget->allocation.height = height;
}

void
widget_set_allocation(struct widget *widget,
		      int32_t x, int32_t y, int32_t width, int32_t height)
{
	widget->allocation.x = x;
	widget->allocation.y = y;
	widget_set_size(widget, width, height);
}

void
widget_set_transparent(struct widget *widget, int transparent)
{
	widget->opaque = !transparent;
}

void *
widget_get_user_data(struct widget *widget)
{
	return widget->user_data;
}

static cairo_surface_t *
widget_get_cairo_surface(struct widget *widget)
{
	struct surface *surface = widget->surface;
	struct window *window = widget->window;

	if (!surface->cairo_surface) {
		if (surface == window->main_surface)
			window_create_main_surface(window);
		else
			surface_create_surface(surface, 0, 0, 0);
	}

	return surface->cairo_surface;
}

cairo_t *
widget_cairo_create(struct widget *widget)
{
	cairo_surface_t *cairo_surface;
	cairo_t *cr;

	cairo_surface = widget_get_cairo_surface(widget);
	cr = cairo_create(cairo_surface);

	return cr;
}

void
widget_set_resize_handler(struct widget *widget,
			  widget_resize_handler_t handler)
{
	widget->resize_handler = handler;
}

void
widget_set_redraw_handler(struct widget *widget,
			  widget_redraw_handler_t handler)
{
	widget->redraw_handler = handler;
}

void
widget_set_enter_handler(struct widget *widget, widget_enter_handler_t handler)
{
	widget->enter_handler = handler;
}

void
widget_set_leave_handler(struct widget *widget, widget_leave_handler_t handler)
{
	widget->leave_handler = handler;
}

void
widget_set_motion_handler(struct widget *widget,
			  widget_motion_handler_t handler)
{
	widget->motion_handler = handler;
}

void
widget_set_button_handler(struct widget *widget,
			  widget_button_handler_t handler)
{
	widget->button_handler = handler;
}

void
widget_set_axis_handler(struct widget *widget,
			widget_axis_handler_t handler)
{
	widget->axis_handler = handler;
}

void
widget_schedule_redraw(struct widget *widget)
{
	window_schedule_redraw(widget->window);
}

cairo_surface_t *
window_get_surface(struct window *window)
{
	cairo_surface_t *cairo_surface;

	cairo_surface = widget_get_cairo_surface(window->main_surface->widget);

	return cairo_surface_reference(cairo_surface);
}

struct wl_surface *
window_get_wl_surface(struct window *window)
{
	return window->main_surface->surface;
}

struct wl_shell_surface *
window_get_wl_shell_surface(struct window *window)
{
	return window->shell_surface;
}

static void
tooltip_redraw_handler(struct widget *widget, void *data)
{
	cairo_t *cr;
	const int32_t r = 3;
	struct tooltip *tooltip = data;
	int32_t width, height;

	cr = widget_cairo_create(widget);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	width = widget->allocation.width;
	height = widget->allocation.height;
	rounded_rect(cr, 0, 0, width, height, r);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.4, 0.8);
	cairo_fill(cr);

	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	cairo_move_to(cr, 10, 16);
	cairo_show_text(cr, tooltip->entry);
	cairo_destroy(cr);
}

static cairo_text_extents_t
get_text_extents(struct tooltip *tooltip)
{
	cairo_t *cr;
	cairo_text_extents_t extents;

	/* Use the dummy_surface because tooltip's surface was not
	 * created yet, and parent does not have a valid surface
	 * outside repaint, either.
	 */
	cr = cairo_create(tooltip->window->display->dummy_surface);
	cairo_text_extents(cr, tooltip->entry, &extents);
	cairo_destroy(cr);

	return extents;
}

static int
window_create_tooltip(struct tooltip *tooltip)
{
	struct widget *parent = tooltip->parent;
	struct display *display = parent->window->display;
	struct window *window;
	const int offset_y = 27;
	const int margin = 3;
	cairo_text_extents_t extents;

	if (tooltip->widget)
		return 0;

	window = window_create_transient(display, parent->window, tooltip->x,
					 tooltip->y + offset_y,
					 WL_SHELL_SURFACE_TRANSIENT_INACTIVE);
	if (!window)
		return -1;

	tooltip->window = window;
	tooltip->widget = window_add_widget(tooltip->window, tooltip);

	extents = get_text_extents(tooltip);
	widget_set_redraw_handler(tooltip->widget, tooltip_redraw_handler);
	window_schedule_resize(window, extents.width + 20, 20 + margin * 2);

	return 0;
}

void
widget_destroy_tooltip(struct widget *parent)
{
	struct tooltip *tooltip = parent->tooltip;

	parent->tooltip_count = 0;
	if (!tooltip)
		return;

	if (tooltip->widget) {
		widget_destroy(tooltip->widget);
		window_destroy(tooltip->window);
		tooltip->widget = NULL;
		tooltip->window = NULL;
	}

	close(tooltip->tooltip_fd);
	free(tooltip->entry);
	free(tooltip);
	parent->tooltip = NULL;
}

static void
tooltip_func(struct task *task, uint32_t events)
{
	struct tooltip *tooltip =
		container_of(task, struct tooltip, tooltip_task);
	uint64_t exp;

	if (read(tooltip->tooltip_fd, &exp, sizeof (uint64_t)) != sizeof (uint64_t))
		abort();
	window_create_tooltip(tooltip);
}

#define TOOLTIP_TIMEOUT 500
static int
tooltip_timer_reset(struct tooltip *tooltip)
{
	struct itimerspec its;

	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = TOOLTIP_TIMEOUT / 1000;
	its.it_value.tv_nsec = (TOOLTIP_TIMEOUT % 1000) * 1000 * 1000;
	if (timerfd_settime(tooltip->tooltip_fd, 0, &its, NULL) < 0) {
		fprintf(stderr, "could not set timerfd\n: %m");
		return -1;
	}

	return 0;
}

int
widget_set_tooltip(struct widget *parent, char *entry, float x, float y)
{
	struct tooltip *tooltip = parent->tooltip;

	parent->tooltip_count++;
	if (tooltip) {
		tooltip->x = x;
		tooltip->y = y;
		tooltip_timer_reset(tooltip);
		return 0;
	}

	/* the handler might be triggered too fast via input device motion, so
	 * we need this check here to make sure tooltip is fully initialized */
	if (parent->tooltip_count > 1)
		return 0;

        tooltip = malloc(sizeof *tooltip);
        if (!tooltip)
                return -1;

	parent->tooltip = tooltip;
	tooltip->parent = parent;
	tooltip->widget = NULL;
	tooltip->window = NULL;
	tooltip->x = x;
	tooltip->y = y;
	tooltip->entry = strdup(entry);
	tooltip->tooltip_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (tooltip->tooltip_fd < 0) {
		fprintf(stderr, "could not create timerfd\n: %m");
		return -1;
	}

	tooltip->tooltip_task.run = tooltip_func;
	display_watch_fd(parent->window->display, tooltip->tooltip_fd,
			 EPOLLIN, &tooltip->tooltip_task);
	tooltip_timer_reset(tooltip);

	return 0;
}

static void
workspace_manager_state(void *data,
			struct workspace_manager *workspace_manager,
			uint32_t current,
			uint32_t count)
{
	struct display *display = data;

	display->workspace = current;
	display->workspace_count = count;
}

static const struct workspace_manager_listener workspace_manager_listener = {
	workspace_manager_state
};

static void
frame_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct frame *frame = data;
	struct widget *child = frame->child;
	struct rectangle allocation;
	struct display *display = widget->window->display;
	struct surface *surface = widget->surface;
	struct frame_button * button;
	struct theme *t = display->theme;
	int x_l, x_r, y, w, h;
	int decoration_width, decoration_height;
	int opaque_margin, shadow_margin;

	switch (widget->window->type) {
	case TYPE_FULLSCREEN:
		decoration_width = 0;
		decoration_height = 0;

		allocation.x = 0;
		allocation.y = 0;
		allocation.width = width;
		allocation.height = height;
		opaque_margin = 0;

		wl_list_for_each(button, &frame->buttons_list, link)
			button->widget->opaque = 1;
		break;
	case TYPE_MAXIMIZED:
		decoration_width = t->width * 2;
		decoration_height = t->width + t->titlebar_height;

		allocation.x = t->width;
		allocation.y = t->titlebar_height;
		allocation.width = width - decoration_width;
		allocation.height = height - decoration_height;

		opaque_margin = 0;

		wl_list_for_each(button, &frame->buttons_list, link)
			button->widget->opaque = 0;
		break;
	default:
		decoration_width = (t->width + t->margin) * 2;
		decoration_height = t->width +
			t->titlebar_height + t->margin * 2;

		allocation.x = t->width + t->margin;
		allocation.y = t->titlebar_height + t->margin;
		allocation.width = width - decoration_width;
		allocation.height = height - decoration_height;

		opaque_margin = t->margin + t->frame_radius;

		wl_list_for_each(button, &frame->buttons_list, link)
			button->widget->opaque = 0;
		break;
	}

	widget_set_allocation(child, allocation.x, allocation.y,
			      allocation.width, allocation.height);

	if (child->resize_handler)
		child->resize_handler(child,
				      allocation.width,
				      allocation.height,
				      child->user_data);

	width = child->allocation.width + decoration_width;
	height = child->allocation.height + decoration_height;

	shadow_margin = widget->window->type == TYPE_MAXIMIZED ? 0 : t->margin;

	surface->input_region =
		wl_compositor_create_region(display->compositor);
	if (widget->window->type != TYPE_FULLSCREEN) {
		wl_region_add(surface->input_region,
			      shadow_margin, shadow_margin,
			      width - 2 * shadow_margin,
			      height - 2 * shadow_margin);
	} else {
		wl_region_add(surface->input_region, 0, 0, width, height);
	}

	widget_set_allocation(widget, 0, 0, width, height);

	if (child->opaque)
		wl_region_add(surface->opaque_region,
			      opaque_margin, opaque_margin,
			      widget->allocation.width - 2 * opaque_margin,
			      widget->allocation.height - 2 * opaque_margin);

	/* frame internal buttons */
	x_r = frame->widget->allocation.width - t->width - shadow_margin;
	x_l = t->width + shadow_margin;
	y = t->width + shadow_margin;
	wl_list_for_each(button, &frame->buttons_list, link) {
		const int button_padding = 4;
		w = cairo_image_surface_get_width(button->icon);
		h = cairo_image_surface_get_height(button->icon);

		if (button->decoration == FRAME_BUTTON_FANCY)
			w += 10;

		if (button->align == FRAME_BUTTON_LEFT) {
			widget_set_allocation(button->widget,
					      x_l, y , w + 1, h + 1);
			x_l += w;
			x_l += button_padding;
		} else {
			x_r -= w;
			widget_set_allocation(button->widget,
					      x_r, y , w + 1, h + 1);
			x_r -= button_padding;
		}
	}
}

static int
frame_button_enter_handler(struct widget *widget,
			   struct input *input, float x, float y, void *data)
{
	struct frame_button *frame_button = data;

	widget_schedule_redraw(frame_button->widget);
	frame_button->state = FRAME_BUTTON_OVER;

	return CURSOR_LEFT_PTR;
}

static void
frame_button_leave_handler(struct widget *widget, struct input *input, void *data)
{
	struct frame_button *frame_button = data;

	widget_schedule_redraw(frame_button->widget);
	frame_button->state = FRAME_BUTTON_DEFAULT;
}

static void
frame_button_button_handler(struct widget *widget,
			    struct input *input, uint32_t time,
			    uint32_t button,
			    enum wl_pointer_button_state state, void *data)
{
	struct frame_button *frame_button = data;
	struct window *window = widget->window;
	int was_pressed = (frame_button->state == FRAME_BUTTON_ACTIVE);

	if (button != BTN_LEFT)
		return;

	switch (state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		frame_button->state = FRAME_BUTTON_ACTIVE;
		widget_schedule_redraw(frame_button->widget);

		if (frame_button->type == FRAME_BUTTON_ICON)
			window_show_frame_menu(window, input, time);
		return;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		frame_button->state = FRAME_BUTTON_DEFAULT;
		widget_schedule_redraw(frame_button->widget);
		break;
	}

	if (!was_pressed)
		return;

	switch (frame_button->type) {
	case FRAME_BUTTON_CLOSE:
		if (window->close_handler)
			window->close_handler(window->parent,
					      window->user_data);
		else
			display_exit(window->display);
		break;
	case FRAME_BUTTON_MINIMIZE:
		window_set_minimized(window, !window->minimized);
		break;
	case FRAME_BUTTON_MAXIMIZE:
		window_set_maximized(window, window->type != TYPE_MAXIMIZED);
		break;
	default:
		/* Unknown operation */
		break;
	}
}

static int
frame_button_motion_handler(struct widget *widget,
                            struct input *input, uint32_t time,
                            float x, float y, void *data)
{
	struct frame_button *frame_button = data;
	enum frame_button_pointer previous_button_state = frame_button->state;

	/* only track state for a pressed button */
	if (input->grab != widget)
		return CURSOR_LEFT_PTR;

	if (x > widget->allocation.x &&
	    x < (widget->allocation.x + widget->allocation.width) &&
	    y > widget->allocation.y &&
	    y < (widget->allocation.y + widget->allocation.height)) {
		frame_button->state = FRAME_BUTTON_ACTIVE;
	} else {
		frame_button->state = FRAME_BUTTON_DEFAULT;
	}

	if (frame_button->state != previous_button_state)
		widget_schedule_redraw(frame_button->widget);

	return CURSOR_LEFT_PTR;
}

static void
frame_button_redraw_handler(struct widget *widget, void *data)
{
	struct frame_button *frame_button = data;
	cairo_t *cr;
	int width, height, x, y;

	x = widget->allocation.x;
	y = widget->allocation.y;
	width = widget->allocation.width;
	height = widget->allocation.height;

	if (!width)
		return;
	if (!height)
		return;
	if (widget->opaque)
		return;

	cr = widget_cairo_create(widget);

	if (frame_button->decoration == FRAME_BUTTON_FANCY) {
		cairo_set_line_width(cr, 1);

		cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
		cairo_rectangle (cr, x, y, 25, 16);

		cairo_stroke_preserve(cr);

		switch (frame_button->state) {
		case FRAME_BUTTON_DEFAULT:
			cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
			break;
		case FRAME_BUTTON_OVER:
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
			break;
		case FRAME_BUTTON_ACTIVE:
			cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
			break;
		}

		cairo_fill (cr);

		x += 4;
	}

	cairo_set_source_surface(cr, frame_button->icon, x, y);
	cairo_paint(cr);

	cairo_destroy(cr);
}

static struct widget *
frame_button_create(struct frame *frame, void *data, enum frame_button_action type,
	enum frame_button_align align, enum frame_button_decoration style)
{
	struct frame_button *frame_button;
	const char *icon = data;

	frame_button = malloc (sizeof *frame_button);
	memset(frame_button, 0, sizeof *frame_button);

	frame_button->icon = cairo_image_surface_create_from_png(icon);
	frame_button->widget = widget_add_widget(frame->widget, frame_button);
	frame_button->frame = frame;
	frame_button->type = type;
	frame_button->align = align;
	frame_button->decoration = style;

	wl_list_insert(frame->buttons_list.prev, &frame_button->link);

	widget_set_redraw_handler(frame_button->widget, frame_button_redraw_handler);
	widget_set_enter_handler(frame_button->widget, frame_button_enter_handler);
	widget_set_leave_handler(frame_button->widget, frame_button_leave_handler);
	widget_set_button_handler(frame_button->widget, frame_button_button_handler);
	widget_set_motion_handler(frame_button->widget, frame_button_motion_handler);
	return frame_button->widget;
}

static void
frame_button_destroy(struct frame_button *frame_button)
{
	widget_destroy(frame_button->widget);
	wl_list_remove(&frame_button->link);
	cairo_surface_destroy(frame_button->icon);
	free(frame_button);

	return;
}

static void
frame_redraw_handler(struct widget *widget, void *data)
{
	cairo_t *cr;
	struct window *window = widget->window;
	struct theme *t = window->display->theme;
	uint32_t flags = 0;

	if (window->type == TYPE_FULLSCREEN)
		return;

	cr = widget_cairo_create(widget);

	if (window->focus_count)
		flags |= THEME_FRAME_ACTIVE;
	if (window->type == TYPE_MAXIMIZED)
		flags |= THEME_FRAME_MAXIMIZED;
	theme_render_frame(t, cr, widget->allocation.width,
			   widget->allocation.height, window->title, flags);

	cairo_destroy(cr);
}

static int
frame_get_pointer_image_for_location(struct frame *frame, struct input *input)
{
	struct theme *t = frame->widget->window->display->theme;
	struct window *window = frame->widget->window;
	int location;

	if (window->type != TYPE_TOPLEVEL)
		return CURSOR_LEFT_PTR;

	location = theme_get_location(t, input->sx, input->sy,
				      frame->widget->allocation.width,
				      frame->widget->allocation.height,
				      window->type == TYPE_MAXIMIZED ?
				      THEME_FRAME_MAXIMIZED : 0);

	switch (location) {
	case THEME_LOCATION_RESIZING_TOP:
		return CURSOR_TOP;
	case THEME_LOCATION_RESIZING_BOTTOM:
		return CURSOR_BOTTOM;
	case THEME_LOCATION_RESIZING_LEFT:
		return CURSOR_LEFT;
	case THEME_LOCATION_RESIZING_RIGHT:
		return CURSOR_RIGHT;
	case THEME_LOCATION_RESIZING_TOP_LEFT:
		return CURSOR_TOP_LEFT;
	case THEME_LOCATION_RESIZING_TOP_RIGHT:
		return CURSOR_TOP_RIGHT;
	case THEME_LOCATION_RESIZING_BOTTOM_LEFT:
		return CURSOR_BOTTOM_LEFT;
	case THEME_LOCATION_RESIZING_BOTTOM_RIGHT:
		return CURSOR_BOTTOM_RIGHT;
	case THEME_LOCATION_EXTERIOR:
	case THEME_LOCATION_TITLEBAR:
	default:
		return CURSOR_LEFT_PTR;
	}
}

static void
frame_menu_func(struct window *window, int index, void *data)
{
	struct display *display;

	switch (index) {
	case 0: /* close */
		if (window->close_handler)
			window->close_handler(window->parent,
					      window->user_data);
		else
			display_exit(window->display);
		break;
	case 1: /* move to workspace above */
		display = window->display;
		if (display->workspace > 0)
			workspace_manager_move_surface(
				display->workspace_manager,
				window->main_surface->surface,
				display->workspace - 1);
		break;
	case 2: /* move to workspace below */
		display = window->display;
		if (display->workspace < display->workspace_count - 1)
			workspace_manager_move_surface(
				display->workspace_manager,
				window->main_surface->surface,
				display->workspace + 1);
		break;
	case 3: /* fullscreen */
		/* we don't have a way to get out of fullscreen for now */
		if (window->fullscreen_handler)
			window->fullscreen_handler(window, window->user_data);
		break;
	}
}

void
window_show_frame_menu(struct window *window,
		       struct input *input, uint32_t time)
{
	int32_t x, y;
	int count;

	static const char *entries[] = {
		"Close",
		"Move to workspace above", "Move to workspace below",
		"Fullscreen"
	};

	if (window->fullscreen_handler)
		count = ARRAY_LENGTH(entries);
	else
		count = ARRAY_LENGTH(entries) - 1;

	input_get_position(input, &x, &y);
	window_show_menu(window->display, input, time, window,
			 x - 10, y - 10, frame_menu_func, entries, count);
}

static int
frame_enter_handler(struct widget *widget,
		    struct input *input, float x, float y, void *data)
{
	return frame_get_pointer_image_for_location(data, input);
}

static int
frame_motion_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     float x, float y, void *data)
{
	return frame_get_pointer_image_for_location(data, input);
}

static void
frame_button_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     uint32_t button, enum wl_pointer_button_state state,
		     void *data)

{
	struct frame *frame = data;
	struct window *window = widget->window;
	struct display *display = window->display;
	int location;

	if (window->type != TYPE_TOPLEVEL)
		return;

	location = theme_get_location(display->theme, input->sx, input->sy,
				      frame->widget->allocation.width,
				      frame->widget->allocation.height,
				      window->type == TYPE_MAXIMIZED ?
				      THEME_FRAME_MAXIMIZED : 0);

	if (window->display->shell && button == BTN_LEFT &&
	    state == WL_POINTER_BUTTON_STATE_PRESSED) {
		switch (location) {
		case THEME_LOCATION_TITLEBAR:
			if (!window->shell_surface)
				break;
			input_ungrab(input);
			wl_shell_surface_move(window->shell_surface,
					      input_get_seat(input),
					      display->serial);
			break;
		case THEME_LOCATION_RESIZING_TOP:
		case THEME_LOCATION_RESIZING_BOTTOM:
		case THEME_LOCATION_RESIZING_LEFT:
		case THEME_LOCATION_RESIZING_RIGHT:
		case THEME_LOCATION_RESIZING_TOP_LEFT:
		case THEME_LOCATION_RESIZING_TOP_RIGHT:
		case THEME_LOCATION_RESIZING_BOTTOM_LEFT:
		case THEME_LOCATION_RESIZING_BOTTOM_RIGHT:
			if (!window->shell_surface)
				break;
			input_ungrab(input);

			window->resizing = 1;
			wl_shell_surface_resize(window->shell_surface,
						input_get_seat(input),
						display->serial, location);
			break;
		}
	} else if (button == BTN_RIGHT &&
		   state == WL_POINTER_BUTTON_STATE_PRESSED) {
		window_show_frame_menu(window, input, time);
	}
}

struct widget *
frame_create(struct window *window, void *data)
{
	struct frame *frame;

	frame = malloc(sizeof *frame);
	memset(frame, 0, sizeof *frame);

	frame->widget = window_add_widget(window, frame);
	frame->child = widget_add_widget(frame->widget, data);

	widget_set_redraw_handler(frame->widget, frame_redraw_handler);
	widget_set_resize_handler(frame->widget, frame_resize_handler);
	widget_set_enter_handler(frame->widget, frame_enter_handler);
	widget_set_motion_handler(frame->widget, frame_motion_handler);
	widget_set_button_handler(frame->widget, frame_button_handler);

	/* Create empty list for frame buttons */
	wl_list_init(&frame->buttons_list);

	frame_button_create(frame, DATADIR "/weston/icon_window.png",
		FRAME_BUTTON_ICON, FRAME_BUTTON_LEFT, FRAME_BUTTON_NONE);

	frame_button_create(frame, DATADIR "/weston/sign_close.png",
		FRAME_BUTTON_CLOSE, FRAME_BUTTON_RIGHT, FRAME_BUTTON_FANCY);

	frame_button_create(frame, DATADIR "/weston/sign_maximize.png",
		FRAME_BUTTON_MAXIMIZE, FRAME_BUTTON_RIGHT, FRAME_BUTTON_FANCY);

	frame_button_create(frame, DATADIR "/weston/sign_minimize.png",
		FRAME_BUTTON_MINIMIZE, FRAME_BUTTON_RIGHT, FRAME_BUTTON_FANCY);

	window->frame = frame;

	return frame->child;
}

void
frame_set_child_size(struct widget *widget, int child_width, int child_height)
{
	struct display *display = widget->window->display;
	struct theme *t = display->theme;
	int decoration_width, decoration_height;
	int width, height;
	int margin = widget->window->type == TYPE_MAXIMIZED ? 0 : t->margin;

	if (widget->window->type != TYPE_FULLSCREEN) {
		decoration_width = (t->width + margin) * 2;
		decoration_height = t->width +
			t->titlebar_height + margin * 2;

		width = child_width + decoration_width;
		height = child_height + decoration_height;
	} else {
		width = child_width;
		height = child_height;
	}

	window_schedule_resize(widget->window, width, height);
}

static void
frame_destroy(struct frame *frame)
{
	struct frame_button *button, *tmp;

	wl_list_for_each_safe(button, tmp, &frame->buttons_list, link)
		frame_button_destroy(button);

	/* frame->child must be destroyed by the application */
	widget_destroy(frame->widget);
	free(frame);
}

static void
input_set_focus_widget(struct input *input, struct widget *focus,
		       float x, float y)
{
	struct widget *old, *widget;
	int cursor;

	if (focus == input->focus_widget)
		return;

	old = input->focus_widget;
	if (old) {
		widget = old;
		if (input->grab)
			widget = input->grab;
		if (widget->leave_handler)
			widget->leave_handler(old, input, widget->user_data);
		input->focus_widget = NULL;
	}

	if (focus) {
		widget = focus;
		if (input->grab)
			widget = input->grab;
		input->focus_widget = focus;
		if (widget->enter_handler)
			cursor = widget->enter_handler(focus, input, x, y,
						       widget->user_data);
		else
			cursor = widget->default_cursor;

		input_set_pointer_image(input, cursor);
	}
}

void
input_grab(struct input *input, struct widget *widget, uint32_t button)
{
	input->grab = widget;
	input->grab_button = button;
}

void
input_ungrab(struct input *input)
{
	struct widget *widget;

	input->grab = NULL;
	if (input->pointer_focus) {
		widget = window_find_widget(input->pointer_focus,
					    input->sx, input->sy);
		input_set_focus_widget(input, widget, input->sx, input->sy);
	}
}

static void
input_remove_pointer_focus(struct input *input)
{
	struct window *window = input->pointer_focus;

	if (!window)
		return;

	input_set_focus_widget(input, NULL, 0, 0);

	input->pointer_focus = NULL;
	input->current_cursor = CURSOR_UNSET;
}

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
		     uint32_t serial, struct wl_surface *surface,
		     wl_fixed_t sx_w, wl_fixed_t sy_w)
{
	struct input *input = data;
	struct window *window;
	struct widget *widget;
	float sx = wl_fixed_to_double(sx_w);
	float sy = wl_fixed_to_double(sy_w);

	if (!surface) {
		/* enter event for a window we've just destroyed */
		return;
	}

	input->display->serial = serial;
	input->pointer_enter_serial = serial;
	input->pointer_focus = wl_surface_get_user_data(surface);
	window = input->pointer_focus;

	if (window->resizing) {
		window->resizing = 0;
		/* Schedule a redraw to free the pool */
		window_schedule_redraw(window);
	}

	input->sx = sx;
	input->sy = sy;

	widget = window_find_widget(window, sx, sy);
	input_set_focus_widget(input, widget, sx, sy);
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
		     uint32_t serial, struct wl_surface *surface)
{
	struct input *input = data;

	input->display->serial = serial;
	input_remove_pointer_focus(input);
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
		      uint32_t time, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
	struct input *input = data;
	struct window *window = input->pointer_focus;
	struct widget *widget;
	int cursor;
	float sx = wl_fixed_to_double(sx_w);
	float sy = wl_fixed_to_double(sy_w);

	input->sx = sx;
	input->sy = sy;

	if (!window)
		return;

	if (!(input->grab && input->grab_button)) {
		widget = window_find_widget(window, sx, sy);
		input_set_focus_widget(input, widget, sx, sy);
	}

	if (input->grab)
		widget = input->grab;
	else
		widget = input->focus_widget;
	if (widget && widget->motion_handler)
		cursor = widget->motion_handler(input->focus_widget,
						input, time, sx, sy,
						widget->user_data);
	else
		cursor = input->focus_widget->default_cursor;

	input_set_pointer_image(input, cursor);
}

static void
pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial,
		      uint32_t time, uint32_t button, uint32_t state_w)
{
	struct input *input = data;
	struct widget *widget;
	enum wl_pointer_button_state state = state_w;

	input->display->serial = serial;
	if (input->focus_widget && input->grab == NULL &&
	    state == WL_POINTER_BUTTON_STATE_PRESSED)
		input_grab(input, input->focus_widget, button);

	widget = input->grab;
	if (widget && widget->button_handler)
		(*widget->button_handler)(widget,
					  input, time,
					  button, state,
					  input->grab->user_data);

	if (input->grab && input->grab_button == button &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED)
		input_ungrab(input);
}

static void
pointer_handle_axis(void *data, struct wl_pointer *pointer,
		    uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct input *input = data;
	struct widget *widget;

	widget = input->focus_widget;
	if (input->grab)
		widget = input->grab;
	if (widget && widget->axis_handler)
		(*widget->axis_handler)(widget,
					input, time,
					axis, value,
					widget->user_data);
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
};

static void
input_remove_keyboard_focus(struct input *input)
{
	struct window *window = input->keyboard_focus;
	struct itimerspec its;

	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = 0;
	its.it_value.tv_nsec = 0;
	timerfd_settime(input->repeat_timer_fd, 0, &its, NULL);

	if (!window)
		return;

	window->focus_count--;
	if (window->keyboard_focus_handler)
		(*window->keyboard_focus_handler)(window, NULL,
						  window->user_data);

	input->keyboard_focus = NULL;
}

static void
keyboard_repeat_func(struct task *task, uint32_t events)
{
	struct input *input =
		container_of(task, struct input, repeat_task);
	struct window *window = input->keyboard_focus;
	uint64_t exp;

	if (read(input->repeat_timer_fd, &exp, sizeof exp) != sizeof exp)
		/* If we change the timer between the fd becoming
		 * readable and getting here, there'll be nothing to
		 * read and we get EAGAIN. */
		return;

	if (window && window->key_handler) {
		(*window->key_handler)(window, input, input->repeat_time,
				       input->repeat_key, input->repeat_sym,
				       WL_KEYBOARD_KEY_STATE_PRESSED,
				       window->user_data);
	}
}

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
		       uint32_t format, int fd, uint32_t size)
{
	struct input *input = data;
	char *map_str;

	if (!data) {
		close(fd);
		return;
	}

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_str == MAP_FAILED) {
		close(fd);
		return;
	}

	input->xkb.keymap = xkb_map_new_from_string(input->display->xkb_context,
						    map_str,
						    XKB_KEYMAP_FORMAT_TEXT_V1,
						    0);
	munmap(map_str, size);
	close(fd);

	if (!input->xkb.keymap) {
		fprintf(stderr, "failed to compile keymap\n");
		return;
	}

	input->xkb.state = xkb_state_new(input->xkb.keymap);
	if (!input->xkb.state) {
		fprintf(stderr, "failed to create XKB state\n");
		xkb_map_unref(input->xkb.keymap);
		input->xkb.keymap = NULL;
		return;
	}

	input->xkb.control_mask =
		1 << xkb_map_mod_get_index(input->xkb.keymap, "Control");
	input->xkb.alt_mask =
		1 << xkb_map_mod_get_index(input->xkb.keymap, "Mod1");
	input->xkb.shift_mask =
		1 << xkb_map_mod_get_index(input->xkb.keymap, "Shift");
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface,
		      struct wl_array *keys)
{
	struct input *input = data;
	struct window *window;

	input->display->serial = serial;
	input->keyboard_focus = wl_surface_get_user_data(surface);

	window = input->keyboard_focus;
	window->focus_count++;
	if (window->keyboard_focus_handler)
		(*window->keyboard_focus_handler)(window,
						  input, window->user_data);
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface)
{
	struct input *input = data;

	input->display->serial = serial;
	input_remove_keyboard_focus(input);
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
		    uint32_t serial, uint32_t time, uint32_t key,
		    uint32_t state_w)
{
	struct input *input = data;
	struct window *window = input->keyboard_focus;
	uint32_t code, num_syms;
	enum wl_keyboard_key_state state = state_w;
	const xkb_keysym_t *syms;
	xkb_keysym_t sym;
	struct itimerspec its;

	input->display->serial = serial;
	code = key + 8;
	if (!window || !input->xkb.state)
		return;

	num_syms = xkb_key_get_syms(input->xkb.state, code, &syms);

	sym = XKB_KEY_NoSymbol;
	if (num_syms == 1)
		sym = syms[0];

	if (sym == XKB_KEY_F5 && input->modifiers == MOD_ALT_MASK) {
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
			window_set_maximized(window,
					     window->type != TYPE_MAXIMIZED);
	} else if (sym == XKB_KEY_F11 &&
		   window->fullscreen_handler &&
		   state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		window->fullscreen_handler(window, window->user_data);
	} else if (sym == XKB_KEY_F4 &&
		   input->modifiers == MOD_ALT_MASK &&
		   state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (window->close_handler)
			window->close_handler(window->parent,
					      window->user_data);
		else
			display_exit(window->display);
	} else if (window->key_handler) {
		(*window->key_handler)(window, input, time, key,
				       sym, state, window->user_data);
	}

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED &&
	    key == input->repeat_key) {
		its.it_interval.tv_sec = 0;
		its.it_interval.tv_nsec = 0;
		its.it_value.tv_sec = 0;
		its.it_value.tv_nsec = 0;
		timerfd_settime(input->repeat_timer_fd, 0, &its, NULL);
	} else if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		input->repeat_sym = sym;
		input->repeat_key = key;
		input->repeat_time = time;
		its.it_interval.tv_sec = 0;
		its.it_interval.tv_nsec = 25 * 1000 * 1000;
		its.it_value.tv_sec = 0;
		its.it_value.tv_nsec = 400 * 1000 * 1000;
		timerfd_settime(input->repeat_timer_fd, 0, &its, NULL);
	}
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
			  uint32_t serial, uint32_t mods_depressed,
			  uint32_t mods_latched, uint32_t mods_locked,
			  uint32_t group)
{
	struct input *input = data;
	xkb_mod_mask_t mask;

	xkb_state_update_mask(input->xkb.state, mods_depressed, mods_latched,
			      mods_locked, 0, 0, group);
	mask = xkb_state_serialize_mods(input->xkb.state,
					XKB_STATE_DEPRESSED |
					XKB_STATE_LATCHED);
	input->modifiers = 0;
	if (mask & input->xkb.control_mask)
		input->modifiers |= MOD_CONTROL_MASK;
	if (mask & input->xkb.alt_mask)
		input->modifiers |= MOD_ALT_MASK;
	if (mask & input->xkb.shift_mask)
		input->modifiers |= MOD_SHIFT_MASK;
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	struct input *input = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer) {
		input->pointer = wl_seat_get_pointer(seat);
		wl_pointer_set_user_data(input->pointer, input);
		wl_pointer_add_listener(input->pointer, &pointer_listener,
					input);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer) {
		wl_pointer_destroy(input->pointer);
		input->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard) {
		input->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_set_user_data(input->keyboard, input);
		wl_keyboard_add_listener(input->keyboard, &keyboard_listener,
					 input);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->keyboard) {
		wl_keyboard_destroy(input->keyboard);
		input->keyboard = NULL;
	}
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
};

void
input_get_position(struct input *input, int32_t *x, int32_t *y)
{
	*x = input->sx;
	*y = input->sy;
}

struct display *
input_get_display(struct input *input)
{
	return input->display;
}

struct wl_seat *
input_get_seat(struct input *input)
{
	return input->seat;
}

uint32_t
input_get_modifiers(struct input *input)
{
	return input->modifiers;
}

struct widget *
input_get_focus_widget(struct input *input)
{
	return input->focus_widget;
}

struct data_offer {
	struct wl_data_offer *offer;
	struct input *input;
	struct wl_array types;
	int refcount;

	struct task io_task;
	int fd;
	data_func_t func;
	int32_t x, y;
	void *user_data;
};

static void
data_offer_offer(void *data, struct wl_data_offer *wl_data_offer, const char *type)
{
	struct data_offer *offer = data;
	char **p;

	p = wl_array_add(&offer->types, sizeof *p);
	*p = strdup(type);
}

static const struct wl_data_offer_listener data_offer_listener = {
	data_offer_offer,
};

static void
data_offer_destroy(struct data_offer *offer)
{
	char **p;

	offer->refcount--;
	if (offer->refcount == 0) {
		wl_data_offer_destroy(offer->offer);
		for (p = offer->types.data; *p; p++)
			free(*p);
		wl_array_release(&offer->types);
		free(offer);
	}
}

static void
data_device_data_offer(void *data,
		       struct wl_data_device *data_device,
		       struct wl_data_offer *_offer)
{
	struct data_offer *offer;

	offer = malloc(sizeof *offer);

	wl_array_init(&offer->types);
	offer->refcount = 1;
	offer->input = data;
	offer->offer = _offer;
	wl_data_offer_add_listener(offer->offer,
				   &data_offer_listener, offer);
}

static void
data_device_enter(void *data, struct wl_data_device *data_device,
		  uint32_t serial, struct wl_surface *surface,
		  wl_fixed_t x_w, wl_fixed_t y_w,
		  struct wl_data_offer *offer)
{
	struct input *input = data;
	struct window *window;
	void *types_data;
	float x = wl_fixed_to_double(x_w);
	float y = wl_fixed_to_double(y_w);
	char **p;

	input->pointer_enter_serial = serial;
	window = wl_surface_get_user_data(surface);
	input->pointer_focus = window;

	if (offer) {
		input->drag_offer = wl_data_offer_get_user_data(offer);

		p = wl_array_add(&input->drag_offer->types, sizeof *p);
		*p = NULL;

		types_data = input->drag_offer->types.data;
	} else {
		input->drag_offer = NULL;
		types_data = NULL;
	}

	window = input->pointer_focus;
	if (window->data_handler)
		window->data_handler(window, input, x, y, types_data,
				     window->user_data);
}

static void
data_device_leave(void *data, struct wl_data_device *data_device)
{
	struct input *input = data;

	if (input->drag_offer) {
		data_offer_destroy(input->drag_offer);
		input->drag_offer = NULL;
	}
}

static void
data_device_motion(void *data, struct wl_data_device *data_device,
		   uint32_t time, wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct input *input = data;
	struct window *window = input->pointer_focus;
	float x = wl_fixed_to_double(x_w);
	float y = wl_fixed_to_double(y_w);
	void *types_data;

	input->sx = x;
	input->sy = y;

	if (input->drag_offer)
		types_data = input->drag_offer->types.data;
	else
		types_data = NULL;

	if (window->data_handler)
		window->data_handler(window, input, x, y, types_data,
				     window->user_data);
}

static void
data_device_drop(void *data, struct wl_data_device *data_device)
{
	struct input *input = data;
	struct window *window = input->pointer_focus;

	if (window->drop_handler)
		window->drop_handler(window, input,
				     input->sx, input->sy, window->user_data);
}

static void
data_device_selection(void *data,
		      struct wl_data_device *wl_data_device,
		      struct wl_data_offer *offer)
{
	struct input *input = data;
	char **p;

	if (input->selection_offer)
		data_offer_destroy(input->selection_offer);

	if (offer) {
		input->selection_offer = wl_data_offer_get_user_data(offer);
		p = wl_array_add(&input->selection_offer->types, sizeof *p);
		*p = NULL;
	} else {
		input->selection_offer = NULL;
	}
}

static const struct wl_data_device_listener data_device_listener = {
	data_device_data_offer,
	data_device_enter,
	data_device_leave,
	data_device_motion,
	data_device_drop,
	data_device_selection
};

static void
input_set_pointer_image_index(struct input *input, int index)
{
	struct wl_buffer *buffer;
	struct wl_cursor *cursor;
	struct wl_cursor_image *image;

	if (!input->pointer)
		return;

	cursor = input->display->cursors[input->current_cursor];
	if (!cursor)
		return;

	if (index >= (int) cursor->image_count) {
		fprintf(stderr, "cursor index out of range\n");
		return;
	}

	image = cursor->images[index];
	buffer = wl_cursor_image_get_buffer(image);
	if (!buffer)
		return;

	wl_pointer_set_cursor(input->pointer, input->pointer_enter_serial,
			      input->pointer_surface,
			      image->hotspot_x, image->hotspot_y);
	wl_surface_attach(input->pointer_surface, buffer, 0, 0);
	wl_surface_damage(input->pointer_surface, 0, 0,
			  image->width, image->height);
	wl_surface_commit(input->pointer_surface);
}

static const struct wl_callback_listener pointer_surface_listener;

static void
pointer_surface_frame_callback(void *data, struct wl_callback *callback,
			       uint32_t time)
{
	struct input *input = data;
	struct wl_cursor *cursor;
	int i;

	if (callback) {
		assert(callback == input->cursor_frame_cb);
		wl_callback_destroy(callback);
		input->cursor_frame_cb = NULL;
	}

	if (!input->pointer)
		return;

	if (input->current_cursor == CURSOR_BLANK) {
		wl_pointer_set_cursor(input->pointer,
				      input->pointer_enter_serial,
				      NULL, 0, 0);
		return;
	}

	if (input->current_cursor == CURSOR_UNSET)
		return;
	cursor = input->display->cursors[input->current_cursor];
	if (!cursor)
		return;

	/* FIXME We don't have the current time on the first call so we set
	 * the animation start to the time of the first frame callback. */
	if (time == 0)
		input->cursor_anim_start = 0;
	else if (input->cursor_anim_start == 0)
		input->cursor_anim_start = time;

	if (time == 0 || input->cursor_anim_start == 0)
		i = 0;
	else
		i = wl_cursor_frame(cursor, time - input->cursor_anim_start);

	if (cursor->image_count > 1) {
		input->cursor_frame_cb =
			wl_surface_frame(input->pointer_surface);
		wl_callback_add_listener(input->cursor_frame_cb,
					 &pointer_surface_listener, input);
	}

	input_set_pointer_image_index(input, i);
}

static const struct wl_callback_listener pointer_surface_listener = {
	pointer_surface_frame_callback
};

void
input_set_pointer_image(struct input *input, int pointer)
{
	int force = 0;

	if (!input->pointer)
		return;

	if (input->pointer_enter_serial > input->cursor_serial)
		force = 1;

	if (!force && pointer == input->current_cursor)
		return;

	input->current_cursor = pointer;
	input->cursor_serial = input->pointer_enter_serial;
	if (!input->cursor_frame_cb)
		pointer_surface_frame_callback(input, NULL, 0);
	else if (force) {
		/* The current frame callback may be stuck if, for instance,
		 * the set cursor request was processed by the server after
		 * this client lost the focus. In this case the cursor surface
		 * might not be mapped and the frame callback wouldn't ever
		 * complete. Send a set_cursor and attach to try to map the
		 * cursor surface again so that the callback will finish */
		input_set_pointer_image_index(input, 0);
	}
}

struct wl_data_device *
input_get_data_device(struct input *input)
{
	return input->data_device;
}

void
input_set_selection(struct input *input,
		    struct wl_data_source *source, uint32_t time)
{
	wl_data_device_set_selection(input->data_device, source, time);
}

void
input_accept(struct input *input, const char *type)
{
	wl_data_offer_accept(input->drag_offer->offer,
			     input->pointer_enter_serial, type);
}

static void
offer_io_func(struct task *task, uint32_t events)
{
	struct data_offer *offer =
		container_of(task, struct data_offer, io_task);
	unsigned int len;
	char buffer[4096];

	len = read(offer->fd, buffer, sizeof buffer);
	offer->func(buffer, len,
		    offer->x, offer->y, offer->user_data);

	if (len == 0) {
		close(offer->fd);
		data_offer_destroy(offer);
	}
}

static void
data_offer_receive_data(struct data_offer *offer, const char *mime_type,
			data_func_t func, void *user_data)
{
	int p[2];

	if (pipe2(p, O_CLOEXEC) == -1)
		return;

	wl_data_offer_receive(offer->offer, mime_type, p[1]);
	close(p[1]);

	offer->io_task.run = offer_io_func;
	offer->fd = p[0];
	offer->func = func;
	offer->refcount++;
	offer->user_data = user_data;

	display_watch_fd(offer->input->display,
			 offer->fd, EPOLLIN, &offer->io_task);
}

void
input_receive_drag_data(struct input *input, const char *mime_type,
			data_func_t func, void *data)
{
	data_offer_receive_data(input->drag_offer, mime_type, func, data);
	input->drag_offer->x = input->sx;
	input->drag_offer->y = input->sy;
}

int
input_receive_selection_data(struct input *input, const char *mime_type,
			     data_func_t func, void *data)
{
	char **p;

	if (input->selection_offer == NULL)
		return -1;

	for (p = input->selection_offer->types.data; *p; p++)
		if (strcmp(mime_type, *p) == 0)
			break;

	if (*p == NULL)
		return -1;

	data_offer_receive_data(input->selection_offer,
				mime_type, func, data);
	return 0;
}

int
input_receive_selection_data_to_fd(struct input *input,
				   const char *mime_type, int fd)
{
	if (input->selection_offer)
		wl_data_offer_receive(input->selection_offer->offer,
				      mime_type, fd);

	return 0;
}

void
window_move(struct window *window, struct input *input, uint32_t serial)
{
	if (!window->shell_surface)
		return;

	wl_shell_surface_move(window->shell_surface, input->seat, serial);
}

static void
surface_resize(struct surface *surface)
{
	struct widget *widget = surface->widget;
	struct wl_compositor *compositor = widget->window->display->compositor;

	if (surface->input_region) {
		wl_region_destroy(surface->input_region);
		surface->input_region = NULL;
	}

	if (surface->opaque_region)
		wl_region_destroy(surface->opaque_region);

	surface->opaque_region = wl_compositor_create_region(compositor);

	if (widget->resize_handler)
		widget->resize_handler(widget,
				       widget->allocation.width,
				       widget->allocation.height,
				       widget->user_data);

	if (surface->allocation.width != widget->allocation.width ||
	    surface->allocation.height != widget->allocation.height) {
		surface->allocation = widget->allocation;
		window_schedule_redraw(widget->window);
	}

	if (widget->opaque)
		wl_region_add(surface->opaque_region, 0, 0,
			      widget->allocation.width,
			      widget->allocation.height);
}

static void
idle_resize(struct window *window)
{
	window->resize_needed = 0;

	widget_set_allocation(window->main_surface->widget,
			      window->pending_allocation.x,
			      window->pending_allocation.y,
			      window->pending_allocation.width,
			      window->pending_allocation.height);

	surface_resize(window->main_surface);
}

void
window_schedule_resize(struct window *window, int width, int height)
{
	window->pending_allocation.x = 0;
	window->pending_allocation.y = 0;
	window->pending_allocation.width = width;
	window->pending_allocation.height = height;

	if (window->min_allocation.width == 0)
		window->min_allocation = window->pending_allocation;
	if (window->pending_allocation.width < window->min_allocation.width)
		window->pending_allocation.width = window->min_allocation.width;
	if (window->pending_allocation.height < window->min_allocation.height)
		window->pending_allocation.height = window->min_allocation.height;

	window->resize_needed = 1;
	window_schedule_redraw(window);
}

void
widget_schedule_resize(struct widget *widget, int32_t width, int32_t height)
{
	window_schedule_resize(widget->window, width, height);
}

static void
handle_ping(void *data, struct wl_shell_surface *shell_surface,
							uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
handle_configure(void *data, struct wl_shell_surface *shell_surface,
		 uint32_t edges, int32_t width, int32_t height)
{
	struct window *window = data;

	window->resize_edges = edges;
	window_schedule_resize(window, width, height);
}

static void
menu_destroy(struct menu *menu)
{
	widget_destroy(menu->widget);
	window_destroy(menu->window);
	free(menu);
}

static void
handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
	struct window *window = data;
	struct menu *menu = window->main_surface->widget->user_data;

	/* FIXME: Need more context in this event, at least the input
	 * device.  Or just use wl_callback.  And this really needs to
	 * be a window vfunc that the menu can set.  And we need the
	 * time. */

	menu->func(window->parent, menu->current, window->parent->user_data);
	input_ungrab(menu->input);
	menu_destroy(menu);
}

static void
handle_maximize(void *data, struct wl_shell_surface *shell_surface)
{
	struct window *window = data;

	window_set_maximized(window, 1);
}

static void
handle_unmaximize(void *data, struct wl_shell_surface *shell_surface)
{
	struct window *window = data;

	window_set_maximized(window, 0);
}

static void
handle_minimize(void *data, struct wl_shell_surface *shell_surface)
{
	struct window *window = data;

	window->minimized = 1;
}

static void
handle_unminimize(void *data, struct wl_shell_surface *shell_surface)
{
	struct window *window = data;

	window->minimized = 0;
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	handle_ping,
	handle_configure,
	handle_popup_done,
	handle_maximize,
	handle_unmaximize,
	handle_minimize,
	handle_unminimize
};

void
window_get_allocation(struct window *window,
		      struct rectangle *allocation)
{
	*allocation = window->main_surface->allocation;
}

static void
widget_redraw(struct widget *widget)
{
	struct widget *child;

	if (widget->redraw_handler)
		widget->redraw_handler(widget, widget->user_data);
	wl_list_for_each(child, &widget->child_list, link)
		widget_redraw(child);
}

static void
frame_callback(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;

	assert(callback == window->frame_cb);
	wl_callback_destroy(callback);
	window->frame_cb = 0;
	window->redraw_scheduled = 0;
	if (window->redraw_needed)
		window_schedule_redraw(window);
}

static const struct wl_callback_listener listener = {
	frame_callback
};

static void
idle_redraw(struct task *task, uint32_t events)
{
	struct window *window = container_of(task, struct window, redraw_task);

	if (window->resize_needed)
		idle_resize(window);

	widget_redraw(window->main_surface->widget);
	window->redraw_needed = 0;
	wl_list_init(&window->redraw_task.link);

	window->frame_cb = wl_surface_frame(window->main_surface->surface);
	wl_callback_add_listener(window->frame_cb, &listener, window);
	window_flush(window);
}

void
window_schedule_redraw(struct window *window)
{
	window->redraw_needed = 1;
	if (!window->redraw_scheduled) {
		window->redraw_task.run = idle_redraw;
		display_defer(window->display, &window->redraw_task);
		window->redraw_scheduled = 1;
	}
}

int
window_is_fullscreen(struct window *window)
{
	return window->type == TYPE_FULLSCREEN;
}

void
window_set_fullscreen(struct window *window, int fullscreen)
{
	if (!window->display->shell)
		return;

	if ((window->type == TYPE_FULLSCREEN) == fullscreen)
		return;

	if (fullscreen) {
		window->saved_type = window->type;
		if (window->type == TYPE_TOPLEVEL) {
			window->saved_allocation = window->main_surface->allocation;
		}
		window->type = TYPE_FULLSCREEN;
		wl_shell_surface_set_fullscreen(window->shell_surface,
						window->fullscreen_method,
						0, NULL);
	} else {
		if (window->saved_type == TYPE_MAXIMIZED) {
			window_set_maximized(window, 1);
		} else {
			window->type = TYPE_TOPLEVEL;
			wl_shell_surface_set_toplevel(window->shell_surface);
			window_schedule_resize(window,
					   window->saved_allocation.width,
					   window->saved_allocation.height);
		}
	}
}

void
window_set_fullscreen_method(struct window *window,
			     enum wl_shell_surface_fullscreen_method method)
{
	window->fullscreen_method = method;
}

int
window_is_maximized(struct window *window)
{
	return window->type == TYPE_MAXIMIZED;
}

void
window_set_maximized(struct window *window, int maximized)
{
	if (!window->display->shell)
		return;

	if ((window->type == TYPE_MAXIMIZED) == maximized)
		return;

	if (window->type == TYPE_TOPLEVEL) {
		window->saved_allocation = window->main_surface->allocation;
		wl_shell_surface_set_maximized(window->shell_surface, NULL);
		window->type = TYPE_MAXIMIZED;
	} else if (window->type == TYPE_FULLSCREEN) {
		wl_shell_surface_set_maximized(window->shell_surface, NULL);
		window->type = TYPE_MAXIMIZED;
	} else {
		wl_shell_surface_set_toplevel(window->shell_surface);
		window->type = TYPE_TOPLEVEL;
		window_schedule_resize(window,
				       window->saved_allocation.width,
				       window->saved_allocation.height);
	}
}

void
window_set_minimized(struct window *window, int minimized)
{
	if (!window->display->shell)
		return;

	if (window->minimized == minimized)
		return;

	if (minimized) {
		wl_shell_surface_set_minimized(window->shell_surface);
		window->minimized = 1;
	} else
		window->minimized = 0;
}

void
window_set_user_data(struct window *window, void *data)
{
	window->user_data = data;
}

void *
window_get_user_data(struct window *window)
{
	return window->user_data;
}

void
window_set_key_handler(struct window *window,
		       window_key_handler_t handler)
{
	window->key_handler = handler;
}

void
window_set_keyboard_focus_handler(struct window *window,
				  window_keyboard_focus_handler_t handler)
{
	window->keyboard_focus_handler = handler;
}

void
window_set_data_handler(struct window *window, window_data_handler_t handler)
{
	window->data_handler = handler;
}

void
window_set_drop_handler(struct window *window, window_drop_handler_t handler)
{
	window->drop_handler = handler;
}

void
window_set_close_handler(struct window *window,
			 window_close_handler_t handler)
{
	window->close_handler = handler;
}

void
window_set_fullscreen_handler(struct window *window,
			      window_fullscreen_handler_t handler)
{
	window->fullscreen_handler = handler;
}

void
window_set_output_handler(struct window *window,
			  window_output_handler_t handler)
{
	window->output_handler = handler;
}

void
window_set_title(struct window *window, const char *title)
{
	free(window->title);
	window->title = strdup(title);
	if (window->shell_surface)
		wl_shell_surface_set_title(window->shell_surface, title);
}

const char *
window_get_title(struct window *window)
{
	return window->title;
}

void
window_set_text_cursor_position(struct window *window, int32_t x, int32_t y)
{
	struct text_cursor_position *text_cursor_position =
					window->display->text_cursor_position;

	if (!text_cursor_position)
		return;

	text_cursor_position_notify(text_cursor_position,
				    window->main_surface->surface,
				    wl_fixed_from_int(x),
				    wl_fixed_from_int(y));
}

void
window_damage(struct window *window, int32_t x, int32_t y,
	      int32_t width, int32_t height)
{
	wl_surface_damage(window->main_surface->surface, x, y, width, height);
}

static void
surface_enter(void *data,
	      struct wl_surface *wl_surface, struct wl_output *wl_output)
{
	struct window *window = data;
	struct output *output;
	struct output *output_found = NULL;
	struct window_output *window_output;

	wl_list_for_each(output, &window->display->output_list, link) {
		if (output->output == wl_output) {
			output_found = output;
			break;
		}
	}

	if (!output_found)
		return;

	window_output = malloc (sizeof *window_output);
	window_output->output = output_found;

	wl_list_insert (&window->window_output_list, &window_output->link);

	if (window->output_handler)
		window->output_handler(window, output_found, 1,
				       window->user_data);
}

static void
surface_leave(void *data,
	      struct wl_surface *wl_surface, struct wl_output *output)
{
	struct window *window = data;
	struct window_output *window_output;
	struct window_output *window_output_found = NULL;

	wl_list_for_each(window_output, &window->window_output_list, link) {
		if (window_output->output->output == output) {
			window_output_found = window_output;
			break;
		}
	}

	if (window_output_found) {
		wl_list_remove(&window_output_found->link);

		if (window->output_handler)
			window->output_handler(window, window_output->output,
					       0, window->user_data);

		free(window_output_found);
	}
}

static const struct wl_surface_listener surface_listener = {
	surface_enter,
	surface_leave
};

static struct surface *
surface_create(struct window *window)
{
	struct display *display = window->display;
	struct surface *surface;

	surface = calloc(1, sizeof *surface);
	if (!surface)
		return NULL;

	surface->window = window;
	surface->surface = wl_compositor_create_surface(display->compositor);
	wl_surface_add_listener(surface->surface, &surface_listener, window);

	return surface;
}

static struct window *
window_create_internal(struct display *display,
		       struct window *parent, int type)
{
	struct window *window;
	struct surface *surface;

	window = malloc(sizeof *window);
	if (window == NULL)
		return NULL;

	memset(window, 0, sizeof *window);
	window->display = display;
	window->parent = parent;

	surface = surface_create(window);
	window->main_surface = surface;

	if (type != TYPE_CUSTOM && display->shell) {
		window->shell_surface =
			wl_shell_get_shell_surface(display->shell,
						   surface->surface);
	}

	window->type = type;
	window->fullscreen_method = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;

	if (display->argb_device)
#ifdef HAVE_CAIRO_EGL
		surface->buffer_type = WINDOW_BUFFER_TYPE_EGL_WINDOW;
#else
		surface->buffer_type = WINDOW_BUFFER_TYPE_SHM;
#endif
	else
		surface->buffer_type = WINDOW_BUFFER_TYPE_SHM;

	wl_surface_set_user_data(surface->surface, window);
	wl_list_insert(display->window_list.prev, &window->link);
	wl_list_init(&window->redraw_task.link);

	if (window->shell_surface) {
		wl_shell_surface_set_user_data(window->shell_surface, window);
		wl_shell_surface_add_listener(window->shell_surface,
					      &shell_surface_listener, window);
	}

	wl_list_init (&window->window_output_list);

	return window;
}

struct window *
window_create(struct display *display)
{
	struct window *window;

	window = window_create_internal(display, NULL, TYPE_NONE);
	if (!window)
		return NULL;

	return window;
}

struct window *
window_create_custom(struct display *display)
{
	struct window *window;

	window = window_create_internal(display, NULL, TYPE_CUSTOM);
	if (!window)
		return NULL;

	return window;
}

struct window *
window_create_transient(struct display *display, struct window *parent,
			int32_t x, int32_t y, uint32_t flags)
{
	struct window *window;

	window = window_create_internal(parent->display,
					parent, TYPE_TRANSIENT);
	if (!window)
		return NULL;

	window->x = x;
	window->y = y;

	if (display->shell)
		wl_shell_surface_set_transient(
			window->shell_surface,
			window->parent->main_surface->surface,
			window->x, window->y, flags);

	return window;
}

static void
menu_set_item(struct menu *menu, int sy)
{
	int next;

	next = (sy - 8) / 20;
	if (menu->current != next) {
		menu->current = next;
		widget_schedule_redraw(menu->widget);
	}
}

static int
menu_motion_handler(struct widget *widget,
		    struct input *input, uint32_t time,
		    float x, float y, void *data)
{
	struct menu *menu = data;

	if (widget == menu->widget)
		menu_set_item(data, y);

	return CURSOR_LEFT_PTR;
}

static int
menu_enter_handler(struct widget *widget,
		   struct input *input, float x, float y, void *data)
{
	struct menu *menu = data;

	if (widget == menu->widget)
		menu_set_item(data, y);

	return CURSOR_LEFT_PTR;
}

static void
menu_leave_handler(struct widget *widget, struct input *input, void *data)
{
	struct menu *menu = data;

	if (widget == menu->widget)
		menu_set_item(data, -200);
}

static void
menu_button_handler(struct widget *widget,
		    struct input *input, uint32_t time,
		    uint32_t button, enum wl_pointer_button_state state,
		    void *data)

{
	struct menu *menu = data;

	if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
	    (menu->release_count > 0 || time - menu->time > 500)) {
		/* Either relase after press-drag-release or
		 * click-motion-click. */
		menu->func(menu->window->parent, 
			   menu->current, menu->window->parent->user_data);
		input_ungrab(input);
		menu_destroy(menu);
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		menu->release_count++;
	}
}

static void
menu_redraw_handler(struct widget *widget, void *data)
{
	cairo_t *cr;
	const int32_t r = 3, margin = 3;
	struct menu *menu = data;
	int32_t width, height, i;

	cr = widget_cairo_create(widget);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	width = widget->allocation.width;
	height = widget->allocation.height;
	rounded_rect(cr, 0, 0, width, height, r);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.4, 0.8);
	cairo_fill(cr);

	for (i = 0; i < menu->count; i++) {
		if (i == menu->current) {
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
			cairo_rectangle(cr, margin, i * 20 + margin,
					width - 2 * margin, 20);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
			cairo_move_to(cr, 10, i * 20 + 16);
			cairo_show_text(cr, menu->entries[i]);
		} else {
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
			cairo_move_to(cr, 10, i * 20 + 16);
			cairo_show_text(cr, menu->entries[i]);
		}
	}

	cairo_destroy(cr);
}

void
window_show_menu(struct display *display,
		 struct input *input, uint32_t time, struct window *parent,
		 int32_t x, int32_t y,
		 menu_func_t func, const char **entries, int count)
{
	struct window *window;
	struct menu *menu;
	const int32_t margin = 3;

	menu = malloc(sizeof *menu);
	if (!menu)
		return;

	window = window_create_internal(parent->display, parent, TYPE_MENU);
	if (!window) {
		free(menu);
		return;
	}

	menu->window = window;
	menu->widget = window_add_widget(menu->window, menu);
	menu->entries = entries;
	menu->count = count;
	menu->release_count = 0;
	menu->current = -1;
	menu->time = time;
	menu->func = func;
	menu->input = input;
	window->type = TYPE_MENU;
	window->x = x;
	window->y = y;

	input_ungrab(input);
	wl_shell_surface_set_popup(window->shell_surface, input->seat,
				   display_get_serial(window->display),
				   window->parent->main_surface->surface,
				   window->x, window->y, 0);

	widget_set_redraw_handler(menu->widget, menu_redraw_handler);
	widget_set_enter_handler(menu->widget, menu_enter_handler);
	widget_set_leave_handler(menu->widget, menu_leave_handler);
	widget_set_motion_handler(menu->widget, menu_motion_handler);
	widget_set_button_handler(menu->widget, menu_button_handler);

	input_grab(input, menu->widget, 0);
	window_schedule_resize(window, 200, count * 20 + margin * 2);
}

void
window_set_buffer_type(struct window *window, enum window_buffer_type type)
{
	window->main_surface->buffer_type = type;
}


static void
display_handle_geometry(void *data,
			struct wl_output *wl_output,
			int x, int y,
			int physical_width,
			int physical_height,
			int subpixel,
			const char *make,
			const char *model,
			int transform)
{
	struct output *output = data;

	output->allocation.x = x;
	output->allocation.y = y;
	output->transform = transform;
}

static void
display_handle_mode(void *data,
		    struct wl_output *wl_output,
		    uint32_t flags,
		    int width,
		    int height,
		    int refresh)
{
	struct output *output = data;
	struct display *display = output->display;

	if (flags & WL_OUTPUT_MODE_CURRENT) {
		output->allocation.width = width;
		output->allocation.height = height;
		if (display->output_configure_handler)
			(*display->output_configure_handler)(
						output, display->user_data);
	}
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode
};

static void
display_add_output(struct display *d, uint32_t id)
{
	struct output *output;

	output = malloc(sizeof *output);
	if (output == NULL)
		return;

	memset(output, 0, sizeof *output);
	output->display = d;
	output->output =
		wl_registry_bind(d->registry, id, &wl_output_interface, 1);
	wl_list_insert(d->output_list.prev, &output->link);

	wl_output_add_listener(output->output, &output_listener, output);
}

static void
output_destroy(struct output *output)
{
	if (output->destroy_handler)
		(*output->destroy_handler)(output, output->user_data);

	wl_output_destroy(output->output);
	wl_list_remove(&output->link);
	free(output);
}

void
display_set_global_handler(struct display *display,
			   display_global_handler_t handler)
{
	struct global *global;

	display->global_handler = handler;
	if (!handler)
		return;

	wl_list_for_each(global, &display->global_list, link)
		display->global_handler(display,
					global->name, global->interface,
					global->version, display->user_data);
}

void
display_set_output_configure_handler(struct display *display,
				     display_output_handler_t handler)
{
	struct output *output;

	display->output_configure_handler = handler;
	if (!handler)
		return;

	wl_list_for_each(output, &display->output_list, link) {
		if (output->allocation.width == 0 &&
		    output->allocation.height == 0)
			continue;

		(*display->output_configure_handler)(output,
						     display->user_data);
	}
}

void
output_set_user_data(struct output *output, void *data)
{
	output->user_data = data;
}

void *
output_get_user_data(struct output *output)
{
	return output->user_data;
}

void
output_set_destroy_handler(struct output *output,
			   display_output_handler_t handler)
{
	output->destroy_handler = handler;
	/* FIXME: implement this, once we have way to remove outputs */
}

void
output_get_allocation(struct output *output, struct rectangle *base)
{
	struct rectangle allocation = output->allocation;

	switch (output->transform) {
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
	        /* Swap width and height */
	        allocation.width = output->allocation.height;
	        allocation.height = output->allocation.width;
	        break;
	}

	*base = allocation;
}

struct wl_output *
output_get_wl_output(struct output *output)
{
	return output->output;
}

enum wl_output_transform
output_get_transform(struct output *output)
{
	return output->transform;
}

static void
fini_xkb(struct input *input)
{
	xkb_state_unref(input->xkb.state);
	xkb_map_unref(input->xkb.keymap);
}

static void
display_add_input(struct display *d, uint32_t id)
{
	struct input *input;

	input = malloc(sizeof *input);
	if (input == NULL)
		return;

	memset(input, 0, sizeof *input);
	input->display = d;
	input->seat = wl_registry_bind(d->registry, id, &wl_seat_interface, 1);
	input->pointer_focus = NULL;
	input->keyboard_focus = NULL;
	wl_list_insert(d->input_list.prev, &input->link);

	wl_seat_add_listener(input->seat, &seat_listener, input);
	wl_seat_set_user_data(input->seat, input);

	input->data_device =
		wl_data_device_manager_get_data_device(d->data_device_manager,
						       input->seat);
	wl_data_device_add_listener(input->data_device, &data_device_listener,
				    input);

	input->pointer_surface = wl_compositor_create_surface(d->compositor);

	input->repeat_timer_fd = timerfd_create(CLOCK_MONOTONIC,
						TFD_CLOEXEC | TFD_NONBLOCK);
	input->repeat_task.run = keyboard_repeat_func;
	display_watch_fd(d, input->repeat_timer_fd,
			 EPOLLIN, &input->repeat_task);
}

static void
input_destroy(struct input *input)
{
	input_remove_keyboard_focus(input);
	input_remove_pointer_focus(input);

	if (input->drag_offer)
		data_offer_destroy(input->drag_offer);

	if (input->selection_offer)
		data_offer_destroy(input->selection_offer);

	wl_data_device_destroy(input->data_device);
	fini_xkb(input);

	wl_surface_destroy(input->pointer_surface);

	wl_list_remove(&input->link);
	wl_seat_destroy(input->seat);
	close(input->repeat_timer_fd);
	free(input);
}

static void
init_workspace_manager(struct display *d, uint32_t id)
{
	d->workspace_manager =
		wl_registry_bind(d->registry, id,
				 &workspace_manager_interface, 1);
	if (d->workspace_manager != NULL)
		workspace_manager_add_listener(d->workspace_manager,
					       &workspace_manager_listener,
					       d);
}

static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t id,
		       const char *interface, uint32_t version)
{
	struct display *d = data;
	struct global *global;

	global = malloc(sizeof *global);
	global->name = id;
	global->interface = strdup(interface);
	global->version = version;
	wl_list_insert(d->global_list.prev, &global->link);

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor = wl_registry_bind(registry, id,
						 &wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_output") == 0) {
		display_add_output(d, id);
	} else if (strcmp(interface, "wl_seat") == 0) {
		display_add_input(d, id);
	} else if (strcmp(interface, "wl_shell") == 0) {
		d->shell = wl_registry_bind(registry,
					    id, &wl_shell_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	} else if (strcmp(interface, "wl_data_device_manager") == 0) {
		d->data_device_manager =
			wl_registry_bind(registry, id,
					 &wl_data_device_manager_interface, 1);
	} else if (strcmp(interface, "text_cursor_position") == 0) {
		d->text_cursor_position =
			wl_registry_bind(registry, id,
					 &text_cursor_position_interface, 1);
	} else if (strcmp(interface, "workspace_manager") == 0) {
		init_workspace_manager(d, id);
	}

	if (d->global_handler)
		d->global_handler(d, id, interface, version, d->user_data);
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
	struct display *d = data;
	struct global *global;
	struct global *tmp;

	wl_list_for_each_safe(global, tmp, &d->global_list, link) {
		if (global->name != name)
			continue;

		/* XXX: Should destroy bound globals, and call
		 * the counterpart of display::global_handler
		 */
		wl_list_remove(&global->link);
		free(global->interface);
		free(global);
	}
}

void *
display_bind(struct display *display, uint32_t name,
	     const struct wl_interface *interface, uint32_t version)
{
	return wl_registry_bind(display->registry, name, interface, version);
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

#ifdef HAVE_CAIRO_EGL
static int
init_egl(struct display *d)
{
	EGLint major, minor;
	EGLint n;

#ifdef USE_CAIRO_GLESV2
#  define GL_BIT EGL_OPENGL_ES2_BIT
#else
#  define GL_BIT EGL_OPENGL_BIT
#endif

	static const EGLint argb_cfg_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_DEPTH_SIZE, 1,
		EGL_RENDERABLE_TYPE, GL_BIT,
		EGL_NONE
	};

#ifdef USE_CAIRO_GLESV2
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLint api = EGL_OPENGL_ES_API;
#else
	EGLint *context_attribs = NULL;
	EGLint api = EGL_OPENGL_API;
#endif

	d->dpy = eglGetDisplay(d->display);
	if (!eglInitialize(d->dpy, &major, &minor)) {
		fprintf(stderr, "failed to initialize EGL\n");
		return -1;
	}

	if (!eglBindAPI(api)) {
		fprintf(stderr, "failed to bind EGL client API\n");
		return -1;
	}

	if (!eglChooseConfig(d->dpy, argb_cfg_attribs,
			     &d->argb_config, 1, &n) || n != 1) {
		fprintf(stderr, "failed to choose argb EGL config\n");
		return -1;
	}

	d->argb_ctx = eglCreateContext(d->dpy, d->argb_config,
				       EGL_NO_CONTEXT, context_attribs);
	if (d->argb_ctx == NULL) {
		fprintf(stderr, "failed to create EGL context\n");
		return -1;
	}

	if (!eglMakeCurrent(d->dpy, NULL, NULL, d->argb_ctx)) {
		fprintf(stderr, "failed to make EGL context current\n");
		return -1;
	}

	d->argb_device = cairo_egl_device_create(d->dpy, d->argb_ctx);
	if (cairo_device_status(d->argb_device) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "failed to get cairo EGL argb device\n");
		return -1;
	}

	return 0;
}

static void
fini_egl(struct display *display)
{
	cairo_device_destroy(display->argb_device);

	eglMakeCurrent(display->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	eglTerminate(display->dpy);
	eglReleaseThread();
}
#endif

static void
init_dummy_surface(struct display *display)
{
	int len;
	void *data;

	len = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, 1);
	data = malloc(len);
	display->dummy_surface =
		cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32,
						    1, 1, len);
	display->dummy_surface_data = data;
}

static void
handle_display_data(struct task *task, uint32_t events)
{
	struct display *display =
		container_of(task, struct display, display_task);
	struct epoll_event ep;
	int ret;

	display->display_fd_events = events;

	if (events & EPOLLERR || events & EPOLLHUP) {
		display_exit(display);
		return;
	}

	if (events & EPOLLIN) {
		ret = wl_display_dispatch(display->display);
		if (ret == -1) {
			display_exit(display);
			return;
		}
	}

	if (events & EPOLLOUT) {
		ret = wl_display_flush(display->display);
		if (ret == 0) {
			ep.events = EPOLLIN | EPOLLERR | EPOLLHUP;
			ep.data.ptr = &display->display_task;
			epoll_ctl(display->epoll_fd, EPOLL_CTL_MOD,
				  display->display_fd, &ep);
		} else if (ret == -1 && errno != EAGAIN) {
			display_exit(display);
			return;
		}
	}
}

struct display *
display_create(int *argc, char *argv[])
{
	struct display *d;

	d = malloc(sizeof *d);
	if (d == NULL)
		return NULL;

        memset(d, 0, sizeof *d);

	d->display = wl_display_connect(NULL);
	if (d->display == NULL) {
		fprintf(stderr, "failed to connect to Wayland display: %m\n");
		free(d);
		return NULL;
	}

	d->epoll_fd = os_epoll_create_cloexec();
	d->display_fd = wl_display_get_fd(d->display);
	d->display_task.run = handle_display_data;
	display_watch_fd(d, d->display_fd, EPOLLIN | EPOLLERR | EPOLLHUP,
			 &d->display_task);

	wl_list_init(&d->deferred_list);
	wl_list_init(&d->input_list);
	wl_list_init(&d->output_list);
	wl_list_init(&d->global_list);

	d->xkb_context = xkb_context_new(0);
	if (d->xkb_context == NULL) {
		fprintf(stderr, "Failed to create XKB context\n");
		return NULL;
	}

	d->workspace = 0;
	d->workspace_count = 1;

	d->registry = wl_display_get_registry(d->display);
	wl_registry_add_listener(d->registry, &registry_listener, d);

	if (wl_display_dispatch(d->display) < 0) {
		fprintf(stderr, "Failed to process Wayland connection: %m\n");
		return NULL;
	}

#ifdef HAVE_CAIRO_EGL
	if (init_egl(d) < 0)
		fprintf(stderr, "EGL does not seem to work, "
			"falling back to software rendering and wl_shm.\n");
#endif

	create_cursors(d);

	d->theme = theme_create();

	wl_list_init(&d->window_list);

	init_dummy_surface(d);

	return d;
}

static void
display_destroy_outputs(struct display *display)
{
	struct output *tmp;
	struct output *output;

	wl_list_for_each_safe(output, tmp, &display->output_list, link)
		output_destroy(output);
}

static void
display_destroy_inputs(struct display *display)
{
	struct input *tmp;
	struct input *input;

	wl_list_for_each_safe(input, tmp, &display->input_list, link)
		input_destroy(input);
}

void
display_destroy(struct display *display)
{
	if (!wl_list_empty(&display->window_list))
		fprintf(stderr, "toytoolkit warning: %d windows exist.\n",
			wl_list_length(&display->window_list));

	if (!wl_list_empty(&display->deferred_list))
		fprintf(stderr, "toytoolkit warning: deferred tasks exist.\n");

	cairo_surface_destroy(display->dummy_surface);
	free(display->dummy_surface_data);

	display_destroy_outputs(display);
	display_destroy_inputs(display);

	xkb_context_unref(display->xkb_context);

	theme_destroy(display->theme);
	destroy_cursors(display);

#ifdef HAVE_CAIRO_EGL
	if (display->argb_device)
		fini_egl(display);
#endif

	if (display->shell)
		wl_shell_destroy(display->shell);

	if (display->shm)
		wl_shm_destroy(display->shm);

	if (display->data_device_manager)
		wl_data_device_manager_destroy(display->data_device_manager);

	wl_compositor_destroy(display->compositor);
	wl_registry_destroy(display->registry);

	close(display->epoll_fd);

	if (!(display->display_fd_events & EPOLLERR) &&
	    !(display->display_fd_events & EPOLLHUP))
		wl_display_flush(display->display);

	wl_display_disconnect(display->display);
	free(display);
}

void
display_set_user_data(struct display *display, void *data)
{
	display->user_data = data;
}

void *
display_get_user_data(struct display *display)
{
	return display->user_data;
}

struct wl_display *
display_get_display(struct display *display)
{
	return display->display;
}

struct output *
display_get_output(struct display *display)
{
	return container_of(display->output_list.next, struct output, link);
}

struct wl_compositor *
display_get_compositor(struct display *display)
{
	return display->compositor;
}

uint32_t
display_get_serial(struct display *display)
{
	return display->serial;
}

EGLDisplay
display_get_egl_display(struct display *d)
{
	return d->dpy;
}

struct wl_data_source *
display_create_data_source(struct display *display)
{
	return wl_data_device_manager_create_data_source(display->data_device_manager);
}

EGLConfig
display_get_argb_egl_config(struct display *d)
{
	return d->argb_config;
}

struct wl_shell *
display_get_shell(struct display *display)
{
	return display->shell;
}

int
display_acquire_window_surface(struct display *display,
			       struct window *window,
			       EGLContext ctx)
{
	struct surface *surface = window->main_surface;

	if (surface->buffer_type != WINDOW_BUFFER_TYPE_EGL_WINDOW)
		return -1;

	widget_get_cairo_surface(window->main_surface->widget);
	return surface->toysurface->acquire(surface->toysurface, ctx);
}

void
display_release_window_surface(struct display *display,
			       struct window *window)
{
	struct surface *surface = window->main_surface;

	if (surface->buffer_type != WINDOW_BUFFER_TYPE_EGL_WINDOW)
		return;

	surface->toysurface->release(surface->toysurface);
}

void
display_defer(struct display *display, struct task *task)
{
	wl_list_insert(&display->deferred_list, &task->link);
}

void
display_watch_fd(struct display *display,
		 int fd, uint32_t events, struct task *task)
{
	struct epoll_event ep;

	ep.events = events;
	ep.data.ptr = task;
	epoll_ctl(display->epoll_fd, EPOLL_CTL_ADD, fd, &ep);
}

void
display_unwatch_fd(struct display *display, int fd)
{
	epoll_ctl(display->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

void
display_run(struct display *display)
{
	struct task *task;
	struct epoll_event ep[16];
	int i, count, ret;

	display->running = 1;
	while (1) {
		while (!wl_list_empty(&display->deferred_list)) {
			task = container_of(display->deferred_list.prev,
					    struct task, link);
			wl_list_remove(&task->link);
			task->run(task, 0);
		}

		wl_display_dispatch_pending(display->display);

		if (!display->running)
			break;

		ret = wl_display_flush(display->display);
		if (ret < 0 && errno == EAGAIN) {
			ep[0].events =
				EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
			ep[0].data.ptr = &display->display_task;

			epoll_ctl(display->epoll_fd, EPOLL_CTL_MOD,
				  display->display_fd, &ep[0]);
		} else if (ret < 0) {
			break;
		}

		count = epoll_wait(display->epoll_fd,
				   ep, ARRAY_LENGTH(ep), -1);
		for (i = 0; i < count; i++) {
			task = ep[i].data.ptr;
			task->run(task, ep[i].events);
		}
	}
}

void
display_exit(struct display *display)
{
	display->running = 0;
}

void
keysym_modifiers_add(struct wl_array *modifiers_map,
		     const char *name)
{
	size_t len = strlen(name) + 1;
	char *p;

	p = wl_array_add(modifiers_map, len);

	if (p == NULL)
		return;

	strncpy(p, name, len);
}

static xkb_mod_index_t
keysym_modifiers_get_index(struct wl_array *modifiers_map,
			   const char *name)
{
	xkb_mod_index_t index = 0;
	char *p = modifiers_map->data;

	while ((const char *)p < (const char *)(modifiers_map->data + modifiers_map->size)) {
		if (strcmp(p, name) == 0)
			return index;

		index++;
		p += strlen(p) + 1;
	}

	return XKB_MOD_INVALID;
}

xkb_mod_mask_t
keysym_modifiers_get_mask(struct wl_array *modifiers_map,
			  const char *name)
{
	xkb_mod_index_t index = keysym_modifiers_get_index(modifiers_map, name);

	if (index == XKB_MOD_INVALID)
		return XKB_MOD_INVALID;

	return 1 << index;
}
