/*
 * Copyright © 2011 Kristian Høgsberg
 * Copyright © 2011 Collabora, Ltd.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <cairo.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <sys/epoll.h> 
#include <linux/input.h>
#include <libgen.h>
#include <ctype.h>
#include <time.h>

#include <wayland-client.h>
#include "window.h"
#include "../shared/cairo-util.h"
#include "../shared/config-parser.h"

#include "desktop-shell-client-protocol.h"
#include "dock-client-protocol.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

extern char **environ; /* defined by libc */

struct desktop {
	struct display *display;
	struct desktop_shell *shell;
	struct dock *dock;
	struct wl_list surfaces;
	struct wl_list outputs;
	uint32_t output_count;

	struct window *grab_window;
	struct widget *grab_widget;

	enum cursor_type grab_cursor;

	struct surface_data_manager *surface_data_manager;
};

struct surface {
	struct surface_data *surface_data;
	struct desktop *desktop;
	uint32_t output_mask;
	char *title;
	int maximized, minimized, focused;

	/* One window list item per dock of the surface's output_mask */
	struct wl_list item_list;

	struct wl_list link;
};

struct resize {
	void (*configure)(void *data,
			  struct dock *dock,
			  uint32_t edges, struct window *window,
			  int32_t width, int32_t height);
};

struct rgba {
	float r, g, b, a;
};

struct dock {
	struct resize base;
	struct window *window;
	struct widget *widget;
	struct wl_list launcher_list;
	struct wl_list window_list;
	struct rectangle window_list_rect;
	uint32_t surface_count;
	struct rgba focused_item;
	struct dock_clock *clock;
	int vertical;
};

struct background {
	struct resize base;
	struct window *window;
	struct widget *widget;
};

struct output {
	struct wl_output *output;
	struct wl_list link;
	uint32_t id;

	struct dock *dock;
	struct background *background;
};

struct list_item {
	struct surface *surface;
	struct widget *widget;
	struct dock *dock;
	cairo_surface_t *icon;
	int focused, highlight;
	int x, y;
	struct wl_list link;
	struct wl_list surface_link;
	struct wl_list reorder_link;
};

struct dock_launcher {
	struct widget *widget;
	struct dock *dock;
	cairo_surface_t *icon;
	int focused, pressed;
	int main_menu_button;
	char *path;
	struct wl_list link;
	struct wl_array envp;
	struct wl_array argv;
};

struct dock_clock {
	struct widget *widget;
	struct dock *dock;
	struct task clock_task;
	int clock_fd;
};

struct unlock_dialog {
	struct window *window;
	struct widget *widget;
	struct widget *button;
	int button_focused;
	int closing;
	struct desktop *desktop;
};

static char *key_background_image = DATADIR "/weston/pattern.png";
static char *key_background_type = "tile";
static uint32_t key_dock_color = 0xaa000000;
static uint32_t key_background_color = 0xff002244;
static char *key_launcher_icon;
static char *key_launcher_path;
static void launcher_section_done(void *data);
static int key_locking = 1;

static const struct config_key shell_config_keys[] = {
	{ "background-image", CONFIG_KEY_STRING, &key_background_image },
	{ "background-type", CONFIG_KEY_STRING, &key_background_type },
	{ "dock-color", CONFIG_KEY_UNSIGNED_INTEGER, &key_dock_color },
	{ "background-color", CONFIG_KEY_UNSIGNED_INTEGER, &key_background_color },
	{ "locking", CONFIG_KEY_BOOLEAN, &key_locking },
};

static const struct config_key launcher_config_keys[] = {
	{ "icon", CONFIG_KEY_STRING, &key_launcher_icon },
	{ "path", CONFIG_KEY_STRING, &key_launcher_path },
};

static const struct config_section config_sections[] = {
	{ "shell",
	  shell_config_keys, ARRAY_LENGTH(shell_config_keys) },
	{ "launcher",
	  launcher_config_keys, ARRAY_LENGTH(launcher_config_keys),
	  launcher_section_done }
};

static void
sigchild_handler(int s)
{
	int status;
	pid_t pid;

	while (pid = waitpid(-1, &status, WNOHANG), pid > 0)
		fprintf(stderr, "child %d exited\n", pid);
}

static void
dock_menu_func(struct window *window, int index, void *data)
{
	printf("Selected index %d from a dock menu.\n", index);
}

static void
dock_show_menu(struct dock *dock, struct input *input, uint32_t time)
{
	int32_t x, y;
	static const char *entries[] = {
		"Roy", "Pris", "Leon", "Zhora"
	};

	input_get_position(input, &x, &y);
	window_show_menu(window_get_display(dock->window),
			 input, time, dock->window,
			 x - 10, y - 10, dock_menu_func, entries, 4);
}

static void
dock_launcher_activate(struct dock_launcher *widget)
{
	char **argv;
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork failed: %m\n");
		return;
	}

	if (pid)
		return;

	argv = widget->argv.data;
	if (execve(argv[0], argv, widget->envp.data) < 0) {
		fprintf(stderr, "execl '%s' failed: %m\n", argv[0]);
		exit(1);
	}
}

static void
dock_launcher_redraw_handler(struct widget *widget, void *data)
{
	struct dock_launcher *launcher = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;

	surface = window_get_surface(launcher->dock->window);
	cr = cairo_create(surface);

	widget_get_allocation(widget, &allocation);
	if (launcher->pressed) {
		allocation.x++;
		allocation.y++;
	}

	cairo_set_source_surface(cr, launcher->icon,
				 allocation.x, allocation.y);

	cairo_paint(cr);

	if (!launcher->focused) {
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_set_source_rgba (cr, 0.4, 0.3, 0.2, 0.9);
		cairo_mask_surface (cr, launcher->icon, allocation.x, allocation.y);
	}

	cairo_destroy(cr);
}

static int
dock_launcher_motion_handler(struct widget *widget, struct input *input,
			      uint32_t time, float x, float y, void *data)
{/*
	struct dock_launcher *launcher = data;

	widget_set_tooltip(widget, basename((char *)launcher->path), x, y);*/

	return CURSOR_LEFT_PTR;
}

static void
set_hex_color(cairo_t *cr, uint32_t color)
{
	cairo_set_source_rgba(cr, 
			      ((color >> 16) & 0xff) / 255.0,
			      ((color >>  8) & 0xff) / 255.0,
			      ((color >>  0) & 0xff) / 255.0,
			      ((color >> 24) & 0xff) / 255.0);
}

static void
get_hex_color_rgba(uint32_t color, float *r, float *g, float *b, float *a)
{
	*r = ((color >> 16) & 0xff) / 255.0;
	*g = ((color >>  8) & 0xff) / 255.0;
	*b = ((color >>  0) & 0xff) / 255.0;
	*a = ((color >> 24) & 0xff) / 255.0;
}

static void
dock_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	struct dock *dock = data;

	surface = window_get_surface(dock->window);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_hex_color(cr, key_dock_color);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static int
dock_launcher_enter_handler(struct widget *widget, struct input *input,
			     float x, float y, void *data)
{
	struct dock_launcher *launcher = data;

	launcher->focused = 1;
	widget_schedule_redraw(widget);

	return CURSOR_LEFT_PTR;
}

static void
dock_launcher_leave_handler(struct widget *widget,
			     struct input *input, void *data)
{
	struct dock_launcher *launcher = data;

	launcher->focused = 0;
	//widget_destroy_tooltip(widget);
	widget_schedule_redraw(widget);
}

static void
dock_launcher_button_handler(struct widget *widget,
			      struct input *input, uint32_t time,
			      uint32_t button,
			      enum wl_pointer_button_state state, void *data)
{
	struct dock_launcher *launcher;

	launcher = widget_get_user_data(widget);
	widget_schedule_redraw(widget);
	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (launcher->main_menu_button)
			printf("main menu clicked\n");
		else
			dock_launcher_activate(launcher);
	}
}

static void
clock_func(struct task *task, uint32_t events)
{
	struct dock_clock *clock =
		container_of(task, struct dock_clock, clock_task);
	uint64_t exp;

	if (read(clock->clock_fd, &exp, sizeof exp) != sizeof exp)
		abort();
	widget_schedule_redraw(clock->widget);
}

static void
dock_clock_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	struct dock_clock *clock = data;
	cairo_t *cr;
	struct rectangle allocation;
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;
	time_t rawtime;
	struct tm * timeinfo;
	char string[128];

	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(string, sizeof string, "%a %b %d, %I:%M %p", timeinfo);

	widget_get_allocation(widget, &allocation);
	if (allocation.width == 0)
		return;

	surface = window_get_surface(clock->dock->window);
	cr = cairo_create(surface);
	cairo_select_font_face(cr, "sans",
			       CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	cairo_text_extents(cr, string, &extents);
	cairo_font_extents (cr, &font_extents);
	cairo_move_to(cr, allocation.x + 5,
		      allocation.y + 3 * (allocation.height >> 2) + 1);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_show_text(cr, string);
	cairo_move_to(cr, allocation.x + 4,
		      allocation.y + 3 * (allocation.height >> 2));
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_show_text(cr, string);
	cairo_destroy(cr);
}

static int
clock_timer_reset(struct dock_clock *clock)
{
	struct itimerspec its;

	its.it_interval.tv_sec = 60;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = 60;
	its.it_value.tv_nsec = 0;
	if (timerfd_settime(clock->clock_fd, 0, &its, NULL) < 0) {
		fprintf(stderr, "could not set timerfd\n: %m");
		return -1;
	}

	return 0;
}

static void
dock_destroy_clock(struct dock_clock *clock)
{
	widget_destroy(clock->widget);

	close(clock->clock_fd);

	free(clock);
}

static void
dock_add_clock(struct dock *dock)
{
	struct dock_clock *clock;
	int timerfd;

	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (timerfd < 0) {
		fprintf(stderr, "could not create timerfd\n: %m");
		return;
	}

	clock = malloc(sizeof *clock);
	memset(clock, 0, sizeof *clock);
	clock->dock = dock;
	dock->clock = clock;
	clock->clock_fd = timerfd;

	clock->clock_task.run = clock_func;
	display_watch_fd(window_get_display(dock->window), clock->clock_fd,
			 EPOLLIN, &clock->clock_task);
	clock_timer_reset(clock);

	clock->widget = widget_add_widget(dock->widget, clock);
	widget_set_redraw_handler(clock->widget, dock_clock_redraw_handler);
}

static void
dock_button_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     uint32_t button,
		     enum wl_pointer_button_state state, void *data)
{
	struct dock *dock = data;

	if (button == BTN_RIGHT && state == WL_POINTER_BUTTON_STATE_PRESSED)
		dock_show_menu(dock, input, time);
}

static void
dock_window_list_schedule_redraw(struct dock *dock)
{
	struct list_item *item;
	int item_width, padding, x, w;

	/* If there are no window list items, redraw the dock to clear it */
	if (wl_list_empty(&dock->window_list)) {
		widget_schedule_redraw(dock->widget);
		return;
	}

	item_width = (dock->window_list_rect.width / dock->surface_count);
	padding = 2;

	x = dock->window_list_rect.x + padding;
	w = MIN(item_width - padding, 200);

	wl_list_for_each(item, &dock->window_list, link) {
		widget_set_allocation(item->widget, x, 4, w, 24);

		x += w + padding;
		widget_schedule_redraw(item->widget);
	}
}

static void
dock_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct dock_launcher *launcher;
	struct rectangle launcher_rect;
//	struct rectangle clock_rect;
	struct dock *dock = data;
	int x, y, w, h;

	x = 16;
	y = 16;

	launcher_rect.x = x;
	launcher_rect.y = y;

	wl_list_for_each(launcher, &dock->launcher_list, link) {
		w = cairo_image_surface_get_width(launcher->icon);
		h = cairo_image_surface_get_height(launcher->icon);

		if (dock->vertical) {
			widget_set_allocation(launcher->widget,
					      x - 4 , y + 4, w + 1, h + 1);
			y += h + 9;
		} else {
			widget_set_allocation(launcher->widget,
					      x + 4, y - 4, w + 1, h + 1);
			x += w + 9;
		}
	}

	launcher_rect.width = x - launcher_rect.x;
	launcher_rect.height = y - launcher_rect.y;
/*
	w=170;
	h=20;

	if (dock->clock)
		widget_set_allocation(dock->clock->widget,
				      width - w - 8, 4, w, 24);

	widget_get_allocation(dock->clock->widget, &clock_rect);

	dock->window_list_rect.x = launcher_rect.x + launcher_rect.width;
	dock->window_list_rect.y = 2;
	dock->window_list_rect.width = width -
					dock->window_list_rect.x -
					(clock_rect.width + 20);
	dock->window_list_rect.height = 28;
	dock_window_list_schedule_redraw(dock);*/
}

static void
dock_configure(void *data,
		struct dock *dock,
		uint32_t edges, struct window *window,
		int32_t width, int32_t height)
{
	struct resize *resize = window_get_user_data(window);
	struct dock *dock_mock = container_of(resize, struct dock, base);

	if (dock_mock->vertical)
		window_schedule_resize(dock_mock->window, 96, height);
	else
		window_schedule_resize(dock_mock->window, width, 96);
}

static void
dock_destroy_launcher(struct dock_launcher *launcher)
{
	wl_array_release(&launcher->argv);
	wl_array_release(&launcher->envp);

	free(launcher->path);

	cairo_surface_destroy(launcher->icon);

	widget_destroy(launcher->widget);
	wl_list_remove(&launcher->link);

	free(launcher);
}

static void
dock_destroy_instance(struct dock *dock)
{
	struct dock_launcher *tmp;
	struct dock_launcher *launcher;

	dock_destroy_clock(dock->clock);

	wl_list_for_each_safe(launcher, tmp, &dock->launcher_list, link)
		dock_destroy_launcher(launcher);

	widget_destroy(dock->widget);
	window_destroy(dock->window);

	free(dock);
}

static void
dock_set_list_item_focus_color(struct dock *dock)
{
	float r, g, b, a;

	/* Consider dock color when choosing item highlight color */
	get_hex_color_rgba(key_dock_color, &r, &b, &g, &a);
	r += 0.2;
	g += 0.2;
	b += 0.2;
	dock->focused_item.r = r > 1.0 ? 0.6 : r;
	dock->focused_item.g = g > 1.0 ? 0.6 : g;
	dock->focused_item.b = b > 1.0 ? 0.6 : b;
	dock->focused_item.a = 0.75;
}

static struct dock *
dock_create_instance(struct display *display, int vertical)
{
	struct dock *dock;

	dock = malloc(sizeof *dock);
	memset(dock, 0, sizeof *dock);

	dock->base.configure = dock_configure;
	dock->window = window_create_custom(display);
	dock->widget = window_add_widget(dock->window, dock);
	wl_list_init(&dock->launcher_list);
	wl_list_init(&dock->window_list);

	window_set_title(dock->window, "dock");
	window_set_user_data(dock->window, dock);

	widget_set_redraw_handler(dock->widget, dock_redraw_handler);
	widget_set_resize_handler(dock->widget, dock_resize_handler);
	widget_set_button_handler(dock->widget, dock_button_handler);

	dock->surface_count = 0;
	dock_set_list_item_focus_color(dock);
	dock_add_clock(dock);

	dock->vertical = vertical;

	return dock;
}

static cairo_surface_t *
load_icon_or_fallback(const char *icon, int launcher)
{
	cairo_surface_t *isurface = load_cairo_surface(icon);
	cairo_surface_t *surface;
	cairo_t *cr;
	int w = 40;
	int sw, sh;

	if (isurface && !launcher)
		return isurface;

	sw = cairo_image_surface_get_width(isurface);
	sh = cairo_image_surface_get_height(isurface);

	if (sw != 64 || sh != 64)
		isurface = cairo_resize_surface(isurface, 0, 64, 64);

	/* draw fallback icon */
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * 2 + 6, w * 2 + 2);
	cr = cairo_create(surface);

	cairo_move_to (cr, w/2, 0);
	cairo_rel_line_to (cr, w, 0);
	cairo_rotate (cr, 60 * (M_PI/180.0));
	cairo_rel_line_to (cr, w, 0);
	cairo_rotate (cr, 60 * (M_PI/180.0));
	cairo_rel_line_to (cr, w, 0);
	cairo_rotate (cr, 60 * (M_PI/180.0));
	cairo_rel_line_to (cr, w, 0);
	cairo_rotate (cr, 60 * (M_PI/180.0));
	cairo_rel_line_to (cr, w, 0);
	cairo_rotate (cr, 60 * (M_PI/180.0));
	cairo_rel_line_to (cr, w, 0);
	cairo_rotate (cr, 60 * (M_PI/180.0));
	cairo_close_path (cr);
	cairo_set_source_rgba(cr, 0.4, 0.2, 0.4, 0.7);
	cairo_set_line_width(cr, 1.5);
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
	cairo_stroke_preserve (cr);
	cairo_set_source_rgba(cr, 0.5, 0.5, 0.6, 0.5);
	cairo_fill (cr);


	if (cairo_surface_status(isurface) == CAIRO_STATUS_SUCCESS) {
		cairo_set_source_surface (cr, isurface, 8, 2);
		//cairo_translate(cr, -28, -34);
		cairo_rectangle (cr, 0, 0, 128, 128);
		cairo_fill (cr);
	} else {
		cairo_surface_destroy(isurface);
	}

	cairo_destroy(cr);

	return surface;
}

static void
dock_add_launcher(struct dock *dock, const char *icon, const char *path)
{
	struct dock_launcher *launcher;
	char *start, *p, *eq, **ps;
	int i, j, k;

	launcher = malloc(sizeof *launcher);
	memset(launcher, 0, sizeof *launcher);
	launcher->icon = load_icon_or_fallback(icon, 1);
	if (path) {
		launcher->path = strdup(path);
		launcher->main_menu_button = 0;

		wl_array_init(&launcher->envp);
		wl_array_init(&launcher->argv);
		for (i = 0; environ[i]; i++) {
			ps = wl_array_add(&launcher->envp, sizeof *ps);
			*ps = environ[i];
		}
		j = 0;

		start = launcher->path;
		while (*start) {
			for (p = start, eq = NULL; *p && !isspace(*p); p++)
				if (*p == '=')
					eq = p;

			if (eq && j == 0) {
				ps = launcher->envp.data;
				for (k = 0; k < i; k++)
					if (strncmp(ps[k], start, eq - start) == 0) {
						ps[k] = start;
						break;
					}
				if (k == i) {
					ps = wl_array_add(&launcher->envp, sizeof *ps);
					*ps = start;
					i++;
				}
			} else {
				ps = wl_array_add(&launcher->argv, sizeof *ps);
				*ps = start;
				j++;
			}

			while (*p && isspace(*p))
				*p++ = '\0';

			start = p;
		}
	} else {
		launcher->main_menu_button = 1;
	}

	ps = wl_array_add(&launcher->envp, sizeof *ps);
	*ps = NULL;
	ps = wl_array_add(&launcher->argv, sizeof *ps);
	*ps = NULL;

	launcher->dock = dock;
	wl_list_insert(dock->launcher_list.prev, &launcher->link);

	launcher->widget = widget_add_widget(dock->widget, launcher);
	widget_set_enter_handler(launcher->widget,
				 dock_launcher_enter_handler);
	widget_set_leave_handler(launcher->widget,
				   dock_launcher_leave_handler);
	widget_set_button_handler(launcher->widget,
				    dock_launcher_button_handler);
	widget_set_redraw_handler(launcher->widget,
				  dock_launcher_redraw_handler);
	widget_set_motion_handler(launcher->widget,
				  dock_launcher_motion_handler);
}

static void
do_we_need_this_configure(void *data,
			struct dock *dock,
			uint32_t edges,
			struct wl_surface *surface,
			int32_t width, int32_t height)
{
	struct window *window = wl_surface_get_user_data(surface);
	struct resize *r = window_get_user_data(window);

	r->configure(data, dock, edges, window, width, height);
}

static const struct dock_listener dock_listener = {
	do_we_need_this_configure
};

static void
background_destroy(struct background *background)
{
	widget_destroy(background->widget);
	window_destroy(background->window);

	free(background);
}

static void
dock_list_item_redraw_handler(struct widget *widget, void *data)
{
	cairo_t *cr;
	cairo_surface_t *surface;
	struct list_item *item = data;
	struct rectangle rect;
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;
	int icon_width, icon_height;
	unsigned int dots;
	double padding;
	char title[128];

	widget_get_allocation(widget, &rect);
	if (rect.width == 0)
		return;

	surface = window_get_surface(item->dock->window);
	cr = cairo_create(surface);

	if (item->focused || item->surface->focused) {
		cairo_set_source_rgba(cr, item->dock->focused_item.r,
					  item->dock->focused_item.g,
					  item->dock->focused_item.b,
					  item->dock->focused_item.a);
		cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
		cairo_fill(cr);
	}

	icon_width = cairo_image_surface_get_width(item->icon);
	icon_height = cairo_image_surface_get_height(item->icon);
	padding = (rect.height / 2.f) - (icon_height / 2.f);
	if (rect.width > icon_width * 2) {
		cairo_set_source_surface(cr, item->icon,
					 rect.x + padding,
					 rect.y + padding);
		cairo_paint(cr);
	} else {
		icon_width = 0;
		icon_height = 0;
		padding = 1;
	}

	strcpy(title, item->surface->title);
	cairo_select_font_face(cr, "sans",
			       CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	cairo_text_extents(cr, title, &extents);

	/* If the string is too long, clip text to button width */
	while (extents.width > (rect.width - (icon_width + padding * 3))) {
		title[strlen(title) - 1] = '\0';
		cairo_text_extents(cr, title, &extents);
		if (extents.width <= 0) {
			title[0] = '\0';
			break;
		}
	}

	/* If the text is clipped, add an ellipsis */
	dots = 3;
	if (strlen(title) < dots)
		dots = strlen(title) + 1;
	if (strlen(title) != strlen(item->surface->title))
		while (dots-- > 0)
			title[strlen(title) - dots] = '.';

	cairo_font_extents (cr, &font_extents);
	cairo_move_to(cr, rect.x + icon_width + padding * 3 + 1,
		      rect.y + 3 * (rect.height >> 2) + 1);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_show_text(cr, title);
	cairo_move_to(cr, rect.x + icon_width + padding * 3,
		      rect.y + 3 * (rect.height >> 2));
	if (item->highlight)
		cairo_set_source_rgb(cr, 1, 1, 1);
	else
		cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
	cairo_show_text(cr, title);
	cairo_destroy(cr);
}

static int
dock_list_item_motion_handler(struct widget *widget, struct input *input,
			      uint32_t time, float x, float y, void *data)
{
	struct list_item *item = data;

	item->x = x;
	item->y = y;

	widget_set_tooltip(widget, basename((char *)item->surface->title), x, y);

	return CURSOR_LEFT_PTR;
}

static int
dock_list_item_enter_handler(struct widget *widget, struct input *input,
			     float x, float y, void *data)
{
	struct list_item *item = data, *t_item;

	item->x = x;
	item->y = y;
	item->highlight = 1;
	item->focused = 1;
	widget_schedule_redraw(widget);

	wl_list_for_each(t_item, &item->dock->window_list, link) {
		if(item == t_item)
			continue;
		t_item->highlight = 0;
		t_item->focused = 0;
	}

	return CURSOR_LEFT_PTR;
}

static void
dock_list_item_leave_handler(struct widget *widget,
			     struct input *input, void *data)
{
	struct list_item *item = data;

	item->highlight = 0;
	item->focused = 0;
	widget_destroy_tooltip(widget);
	widget_schedule_redraw(widget);
}

static void
desktop_update_list_items(struct desktop *desktop, struct surface *surface);

static void
list_item_menu_handle_button(struct list_item *item, int index)
{
	struct surface *surface = item->surface;

	switch (index) {
	case 0: /* (Un)Minimize */
		if (surface->minimized) {
			surface_data_unminimize(surface->surface_data);
			surface->minimized = 0;
		}
		else {
			surface_data_minimize(surface->surface_data);
			surface->minimized = 1;
		}
		break;
	case 1: /* (Un)Maximize */
		if (surface->maximized) {
			surface_data_unmaximize(surface->surface_data);
			surface->maximized = 0;
		}
		else {
			surface_data_maximize(surface->surface_data);
			surface->maximized = 1;
		}
		break;
	case 2: /* Close */
		surface_data_close(surface->surface_data);
		break;
	default:
		item->highlight = 0;
		break;
	}

	item->focused = 0;

	desktop_update_list_items(surface->desktop, surface);
	widget_destroy_tooltip(item->widget);
	widget_schedule_redraw(item->widget);
}

static void
list_item_menu_func(struct window *window, int index, void *data)
{
	struct list_item *item;
	struct dock *dock;

	dock = data;

	wl_list_for_each(item, &dock->window_list, link)
		if (item->focused) {
			list_item_menu_handle_button(item, index);
			return;
		}
}

#define MENU_ENTRIES 3

static void
list_item_show_menu(struct list_item *item, struct input *input, uint32_t time)
{
	struct dock *dock;
	int32_t x, y;
	static const char *entries[MENU_ENTRIES];

	entries[0] = item->surface->minimized ? "Unminimize" : "Minimize";
	entries[1] = item->surface->maximized ? "Unmaximize" : "Maximize";
	entries[2] = "Close";

	dock = item->dock;
	input_get_position(input, &x, &y);
	window_show_menu(window_get_display(dock->window), input,
				time, dock->window, x - 10, y - 10,
				list_item_menu_func, entries, MENU_ENTRIES);
}

static int
rect_contains_point(struct rectangle rect, int x, int y)
{
	int x1, y1, x2, y2;

	x1 = rect.x;
	y1 = rect.y;
	x2 = rect.x + rect.width;
	y2 = rect.y + rect.height;

	if (x > x1 && x < x2 && y > y1 && y < y2)
		return 1;

	return 0;
}

static int
item_contains_point(struct list_item *item, int x, int y)
{
	struct rectangle item_rect;

	widget_get_allocation(item->widget, &item_rect);

	return rect_contains_point(item_rect, x, y);
}

static int
list_contains_point(struct list_item *item, int x, int y)
{
	struct rectangle list_rect;

	list_rect = item->dock->window_list_rect;

	return rect_contains_point(list_rect, x, y);
}

static void
dock_item_list_reorder(struct dock *dock,
			struct list_item *current, struct list_item *item)
{
	struct rectangle current_rect, item_rect;

	if (current == item)
		return;

	widget_get_allocation(current->widget, &current_rect);
	widget_get_allocation(item->widget, &item_rect);

	wl_list_remove(&current->link);

	if (item_rect.x < current_rect.x)
		wl_list_insert(item->link.prev, &current->link);
	else
		wl_list_insert(&item->link, &current->link);

	dock_window_list_schedule_redraw(item->dock);
}

static void
list_item_move(struct list_item *current, int x, int y)
{
	struct list_item *item;

	wl_list_for_each(item, &current->dock->window_list, link) {
		if (item == current)
			continue;
		if (item_contains_point(item, x, y)) {
			dock_item_list_reorder(item->dock, current, item);
			return;
		}
	}
}

static void
dock_list_item_button_handler(struct widget *widget,
			      struct input *input, uint32_t time,
			      uint32_t button,
			      enum wl_pointer_button_state state, void *data)
{
	struct list_item *item;
	struct surface *surface;

	item = data;

	widget_schedule_redraw(widget);

	if (button == BTN_RIGHT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
		widget_destroy_tooltip(item->widget);
		widget_schedule_redraw(item->widget);
		list_item_show_menu(item, input, time);
		return;
	}

	if ((button != BTN_LEFT) || (state != WL_POINTER_BUTTON_STATE_RELEASED))
		return;

	surface = item->surface;
	if (!item_contains_point(item, item->x, item->y)) {
		if (list_contains_point(item, item->x, item->y))
			list_item_move(item, item->x, item->y);
		return;
	}
	if (!surface->focused && !surface->minimized) {
		surface_data_focus(surface->surface_data);
		surface->focused = 1;
		return;
	}
	if (surface->minimized) {
		surface_data_unminimize(surface->surface_data);
		surface->minimized = 0;
	}
	else {
		surface_data_minimize(surface->surface_data);
		surface->minimized = 1;
	}
}

static struct list_item *
dock_list_item_add(struct dock *dock, const char *icon, const char *text)
{
	struct list_item *item;
	item = malloc(sizeof *item);
	memset(item, 0, sizeof *item);

	item->icon = load_icon_or_fallback(icon, 0);

	item->dock = dock;
	wl_list_insert(dock->window_list.prev, &item->link);
	dock->surface_count++;

	item->widget = widget_add_widget(dock->widget, item);
	widget_set_enter_handler(item->widget, dock_list_item_enter_handler);
	widget_set_leave_handler(item->widget, dock_list_item_leave_handler);
	widget_set_button_handler(item->widget, dock_list_item_button_handler);
	widget_set_redraw_handler(item->widget, dock_list_item_redraw_handler);
	widget_set_motion_handler(item->widget, dock_list_item_motion_handler);

	return item;
}

static void
dock_list_item_remove(struct list_item *item)
{
	item->dock->surface_count--;
	wl_list_remove(&item->link);
	wl_list_remove(&item->surface_link);
	widget_destroy(item->widget);
	dock_window_list_schedule_redraw(item->dock);
	cairo_surface_destroy(item->icon);
	free(item);
}

static int
dock_list_item_exists(struct dock *dock, struct surface *surface)
{
	struct list_item *p_item, *s_item;

	wl_list_for_each(p_item, &dock->window_list, link) {
		wl_list_for_each(s_item, &surface->item_list, surface_link) {
			if (p_item == s_item)
				return 1;
		}
	}

	return 0;
}

static void
output_update_window_list(struct output *output, struct surface *surface)
{
	struct list_item *item, *next;
	struct dock *dock;

	dock = output->dock;

	/* Make a list item for each dock of the surfaces output mask */
	if ((1 << output->id) & surface->output_mask) {
		if (!dock_list_item_exists(dock, surface)) {
			item = dock_list_item_add(dock,
					DATADIR "/weston/list_item_icon.png",
					surface->title);
			wl_list_insert(surface->item_list.prev,
							&item->surface_link);
			item->surface = surface;
		}
	} else {
		/* Remove item from dock if surface
		 * is no longer on the output */
		wl_list_for_each_safe(item, next, &surface->item_list,
								surface_link) {
			if (item->dock == dock)
				dock_list_item_remove(item);
		}
	}

	dock_window_list_schedule_redraw(dock);
}

static void
desktop_destroy_surface(struct surface *surface)
{
	struct list_item *item, *next;

	wl_list_for_each_safe(item, next, &surface->item_list, surface_link)
		dock_list_item_remove(item);

	wl_list_remove(&surface->link);
	free(surface->title);
	free(surface);
}

static void
desktop_update_list_items(struct desktop *desktop, struct surface *surface)
{
	struct output *output;

	wl_list_for_each(output, &desktop->outputs, link)
		output_update_window_list(output, surface);
}

static void
surface_data_set_output_mask(void *data,
				struct surface_data *surface_data,
				uint32_t output_mask)
{
	struct desktop *desktop;
	struct surface *surface = data;

	desktop = surface->desktop;

	surface->output_mask = output_mask;

	desktop_update_list_items(desktop, surface);
}

static void
surface_data_set_title(void *data,
				struct surface_data *surface_data,
				const char *title)
{
	struct desktop *desktop;
	struct surface *surface = data;

	desktop = surface->desktop;

	if (surface->title)
		free(surface->title);
	surface->title = strdup(title);

	desktop_update_list_items(desktop, surface);
}

static void
surface_data_set_maximized_state(void *data,
				struct surface_data *surface_data,
				int maximized)
{
	struct desktop *desktop;
	struct surface *surface = data;

	desktop = surface->desktop;

	surface->maximized = maximized;

	desktop_update_list_items(desktop, surface);
}

static void
surface_data_set_minimized_state(void *data,
				struct surface_data *surface_data,
				int minimized)
{
	struct desktop *desktop;
	struct surface *surface = data;

	desktop = surface->desktop;

	surface->minimized = minimized;

	desktop_update_list_items(desktop, surface);
}

static void
surface_data_set_focused_state(void *data,
				struct surface_data *surface_data,
				int focused)
{
	struct desktop *desktop;
	struct surface *surface = data, *es;
	struct list_item *item;

	desktop = surface->desktop;

	wl_list_for_each(es, &desktop->surfaces, link)
		if (es->surface_data != surface_data && focused) {
			es->focused = 0;
			wl_list_for_each(item, &es->item_list, surface_link)
				if (!item->focused)
					item->highlight = 0;
		}

	surface->focused = focused;

	desktop_update_list_items(desktop, surface);
}

static void
surface_data_destroy_handler(void *data, struct surface_data *surface_data)
{
	struct list_item *item, *next;
	struct desktop *desktop;
	struct surface *surface = data;
	struct output *output;
	struct dock *dock;

	desktop = surface->desktop;

	surface_data_destroy(surface_data);

	wl_list_for_each(output, &desktop->outputs, link) {
		dock = output->dock;
		wl_list_for_each_safe(item, next, &dock->window_list, link) {
			if (surface_data == item->surface->surface_data) {
				desktop_destroy_surface(item->surface);
				return;
			}
		}
	}
}

static const struct surface_data_listener surface_data_listener = {
	surface_data_set_output_mask,
	surface_data_set_title,
	surface_data_set_maximized_state,
	surface_data_set_minimized_state,
	surface_data_set_focused_state,
	surface_data_destroy_handler
};

static void
surface_data_receive_surface_object(void *data,
				struct surface_data_manager *manager,
				struct surface_data *surface_data)
{
	struct desktop *desktop = data;
	struct surface *surface;

	surface = calloc(1, sizeof *surface);

	if (!surface) {
		fprintf(stderr, "ERROR: Failed to allocate memory!\n");
		exit(EXIT_FAILURE);
	}

	surface->desktop = desktop;
	surface->surface_data = surface_data;
	surface->desktop = desktop;
	surface->title = strdup("unknown");
	surface->output_mask = 1;
	wl_list_init(&surface->item_list);
	wl_list_insert(&desktop->surfaces, &surface->link);
	surface_data_add_listener(surface_data,
				   &surface_data_listener, surface);
}

static const struct surface_data_manager_listener surface_data_manager_listener = {
	surface_data_receive_surface_object
};

static void
output_destroy(struct output *output)
{
	background_destroy(output->background);
	dock_destroy_instance(output->dock);
	wl_output_destroy(output->output);
	wl_list_remove(&output->link);

	free(output);
}

static void
desktop_destroy_outputs(struct desktop *desktop)
{
	struct output *tmp;
	struct output *output;

	wl_list_for_each_safe(output, tmp, &desktop->outputs, link)
		output_destroy(output);
}

static void
desktop_destroy_surfaces(struct desktop *desktop)
{
	struct surface *surface, *next;

	wl_list_for_each_safe(surface, next, &desktop->surfaces, link)
		desktop_destroy_surface(surface);
}

static void
create_output(struct desktop *desktop, uint32_t id)
{
	struct output *output;

	output = calloc(1, sizeof *output);
	if (!output)
		return;

	output->output =
		display_bind(desktop->display, id, &wl_output_interface, 1);

	output->id = desktop->output_count++;

	wl_list_insert(&desktop->outputs, &output->link);
}

static void
global_handler(struct display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct desktop *desktop = data;

	if (!strcmp(interface, "wl_output")) {
		create_output(desktop, id);
	} else if (!strcmp(interface, "surface_data_manager")) {
		desktop->surface_data_manager =
				display_bind(display, id,
					&surface_data_manager_interface, 1);
		surface_data_manager_add_listener(desktop->surface_data_manager,
					&surface_data_manager_listener, desktop);
	} else if (!strcmp(interface, "dock")) {
		desktop->dock = display_bind(desktop->display,
					      id, &dock_interface, 1);
		dock_add_listener(desktop->dock, &dock_listener, desktop);
	}
}

static void
launcher_section_done(void *data)
{
	struct desktop *desktop = data;
	struct output *output;

	if (key_launcher_icon == NULL || key_launcher_path == NULL) {
		fprintf(stderr, "invalid launcher section\n");
		return;
	}

	wl_list_for_each(output, &desktop->outputs, link) {
		dock_add_launcher(output->dock,
				   key_launcher_icon, key_launcher_path);
	}

	free(key_launcher_icon);
	key_launcher_icon = NULL;
	free(key_launcher_path);
	key_launcher_path = NULL;
}

static void
add_default_launcher(struct desktop *desktop)
{
	struct output *output;

	wl_list_for_each(output, &desktop->outputs, link)
		dock_add_launcher(output->dock,
				   DATADIR "/weston/terminal.png",
				   BINDIR "/weston-terminal");
}

int main(int argc, char *argv[])
{
	struct desktop desktop = { 0 };
	char *config_file;
	struct output *output;
	int ret, vertical = 0;

	wl_list_init(&desktop.outputs);

	desktop.display = display_create(&argc, argv);
	if (desktop.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	if ((argc > 1) && (!strcmp(argv[1], "--vertical") || !strcmp(argv[1], "-v"))) { printf("setting vertical\n");
		vertical = 1;}

	wl_list_init(&desktop.surfaces);
	desktop.output_count = 0;

	display_set_user_data(desktop.display, &desktop);
	display_set_global_handler(desktop.display, global_handler);

	wl_list_for_each(output, &desktop.outputs, link) {
		struct wl_surface *surface;

		output->dock = dock_create_instance(desktop.display, vertical);
		surface = window_get_wl_surface(output->dock->window);
		dock_set_dock(desktop.dock, output->output, surface, vertical);
		/* Main menu */
		dock_add_launcher(output->dock,
				   DATADIR "/weston/wayland.png", NULL);
	}

	config_file = config_file_path("weston.ini");
	ret = parse_config_file(config_file,
				config_sections, ARRAY_LENGTH(config_sections),
				&desktop);
	free(config_file);
	if (ret < 0)
		add_default_launcher(&desktop);

	signal(SIGCHLD, sigchild_handler);
printf("Running main loop\n");
	display_run(desktop.display);
printf("Well that was fun\n");
printf("Time to cleanup\n");
	/* Cleanup */
	desktop_destroy_surfaces(&desktop);
	desktop_destroy_outputs(&desktop);
	desktop_shell_destroy(desktop.shell);
	display_destroy(desktop.display);

	return 0;
}
