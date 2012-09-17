/*
 * Copyright © 2012 Scott Moreau
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <wayland-client.h>
#include "weston-control-client-protocol.h"
#include "../shared/config-parser.h"

struct display {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_control *controller;

	struct wl_list surface_list;

	/* Ncurses stuff */
	int color_scheme;
	int no_ncurses;
	int move_surface;
	int resize_surface;
	int alpha_surface;
	int rotate_surface;
	uint32_t surface_count;
};

struct surface {
	uint32_t id;
	uint32_t z;
	char *title;
	char *type;
	int32_t x, y;
	uint32_t width, height;
	uint32_t alpha;
	int rotation;

	int selected;

	struct wl_list link;
};

static int running = 1;
static int resize_needed;

static void
sigwinch_handler(int dummy)
{
	resize_needed = 1;
}

static void
draw_screen(struct display *d, WINDOW *win, int width, int height)
{
	struct surface *surface;
	char control_key[] = "Esc or q to quit";
	char title[] = "WLTOP", space[] = "                   ";
	int x_pos = 0;

	attron(A_BOLD);
	d->color_scheme ? attron(COLOR_PAIR(1)) : attron(COLOR_PAIR(5));

	mvprintw(1, (width / 2) - (strlen(title) / 2), "%s", title);

	attroff(A_BOLD);
	attron(COLOR_PAIR(3));

	mvprintw(height - 1, 4, "%s", control_key);

	attron(A_BOLD);
	d->color_scheme ? attron(COLOR_PAIR(2)) : attron(COLOR_PAIR(6));

	mvprintw(4, x_pos, "%s%s", " Z", space);
	mvprintw(4, x_pos += 4, "%s%s", "Window ID", space);
	d->color_scheme ? attron(COLOR_PAIR(6)) : attron(COLOR_PAIR(2));
	mvprintw(4, x_pos += 13, "%s%s", "Title", space);
	d->color_scheme ? attron(COLOR_PAIR(2)) : attron(COLOR_PAIR(6));
	mvprintw(4, x_pos += 22, "%s%s", "Type", space);
	mvprintw(4, x_pos += 15, "%s%s", "Alpha", space);
	mvprintw(4, x_pos += 15, "%s%s", "X", space);
	mvprintw(4, x_pos += 8, "%s%s", "Y", space);
	mvprintw(4, x_pos += 8, "%s", "Width      ");
	mvprintw(4, x_pos += 8, "%s", "Height     ");

	attrset(0);
	d->color_scheme ? attron(COLOR_PAIR(1)) : attron(COLOR_PAIR(4));

	wl_list_for_each(surface, &d->surface_list, link) {
		x_pos = 0;
		if (surface->selected) {
			attron(A_BOLD);
			attron(COLOR_PAIR(7));
		} else {
			attroff(A_BOLD);
			d->color_scheme ? attron(COLOR_PAIR(1)) : attron(COLOR_PAIR(4));
		}
		mvprintw(surface->z + 5, x_pos, " %d%s", surface->z, space);
		mvprintw(surface->z + 5, x_pos += 4, "%d%s", surface->id, space);
		mvprintw(surface->z + 5, x_pos += 13, "%s%s", surface->title, space);
		mvprintw(surface->z + 5, x_pos += 22, "%s%s", surface->type, space);
		mvprintw(surface->z + 5, x_pos += 15, "%d%s", surface->alpha, space);
		mvprintw(surface->z + 5, x_pos += 15, "%d%s", surface->x, space);
		mvprintw(surface->z + 5, x_pos += 8, "%d%s", surface->y, space);
		mvprintw(surface->z + 5, x_pos += 8, "%d        ", surface->width);
		mvprintw(surface->z + 5, x_pos += 8, "%d        ", surface->height);
	}
	attron(A_BOLD);
	d->color_scheme ? attron(COLOR_PAIR(2)) : attron(COLOR_PAIR(6));
	mvprintw(3, 0, "%s%d ", " Workspace surfaces: ", d->surface_count);
}

static int
input_handler(struct display *d)
{
	struct surface *surface, *t_surface;
	int s_increment = 20;
	int ch = getch();

	if (ch == 'q' || ch == 27)
		return -1;

	switch (ch) {
	case 'c':
		d->color_scheme = !d->color_scheme;
		break;
	case 'h':
		wl_list_for_each(surface, &d->surface_list, link) {
			if (surface->selected) {
				wl_control_surface_toggle_hide(d->controller,
								surface->id);
				return 0;
			}
		}
		break;
	case 'f':
	case '\n':
	case KEY_ENTER:
		wl_list_for_each(surface, &d->surface_list, link) {
			if (surface->selected) {
				wl_control_focus_surface(d->controller, surface->id);
				wl_control_raise_surface(d->controller, surface->id);
				return 0;
			}
		}
		break;
	case 'm':
		if (!wl_list_empty(&d->surface_list))
			d->move_surface = !d->move_surface;
		break;
	case 'r':
		if (d->rotate_surface) {
			wl_list_for_each(surface, &d->surface_list, link) {
				if (surface->selected) {
					wl_control_set_surface_rotation(d->controller,
								surface->id, 0);
					surface->rotation = d->rotate_surface = 0;
					return 0;
				}
			}
		}
		if (!wl_list_empty(&d->surface_list))
			d->resize_surface = !d->resize_surface;
		break;
	case 'a':
		if (!wl_list_empty(&d->surface_list))
			d->alpha_surface = !d->alpha_surface;
		break;
	case 't':
		if (!wl_list_empty(&d->surface_list))
			d->rotate_surface = !d->rotate_surface;
		break;
	case 'k':
		wl_list_for_each(surface, &d->surface_list, link)
			if (surface->selected) {
				wl_list_for_each(t_surface, &d->surface_list, link) {
					if (t_surface->z == 1) {
						t_surface->selected = 1;
						surface->selected = 0;
						break;
					}
				}
				wl_control_kill_surface(d->controller, surface->id);
				return 0;
			}
		break;
	case KEY_UP:
		wl_list_for_each(surface, &d->surface_list, link) {
			if (surface->selected) {
				if (d->move_surface) {
					wl_control_move_surface(d->controller,
								surface->id,
								surface->x,
								surface->y -
								s_increment);
					return 0;
				}
				if (d->resize_surface) {
					wl_control_resize_surface(d->controller,
								surface->id,
								surface->width,
								surface->height -
								s_increment);
					return 0;
				}
				if (d->alpha_surface) {
					wl_control_set_surface_alpha(d->controller,
								surface->id,
								surface->alpha +
								s_increment);
					return 0;
				}
				if (d->rotate_surface) {
					surface->rotation += (s_increment / 2);
					if (surface->rotation > 360)
						surface->rotation = surface->rotation - 360;
					wl_control_set_surface_rotation(d->controller,
								surface->id,
								surface->rotation);
					return 0;
				}
				if (surface->z == 1) {
					/* The selected surface is the first
					 * in the list, so there is nothing
					 * above it */
					return 0;
				}
				/* Find the previous surface in the stack */
				wl_list_for_each(t_surface, &d->surface_list, link) {
					if (t_surface->z == surface->z - 1) {
						t_surface->selected = 1;
						surface->selected = 0;
						return 0;
					}
				}
			}
		}
		break;
	case KEY_DOWN:
		wl_list_for_each(surface, &d->surface_list, link) {
			if (surface->selected) {
				if (d->move_surface) {
					wl_control_move_surface(d->controller,
								surface->id,
								surface->x,
								surface->y +
								s_increment);
					return 0;
				}
				if (d->resize_surface) {
					wl_control_resize_surface(d->controller,
								surface->id,
								surface->width,
								surface->height +
								s_increment);
					return 0;
				}
				if (d->alpha_surface) {
					wl_control_set_surface_alpha(d->controller,
								surface->id,
								surface->alpha -
								s_increment);
					return 0;
				}
				if (d->rotate_surface) {
					surface->rotation -= (s_increment / 2);
					if (surface->rotation < 0)
						surface->rotation = surface->rotation + 360;
					wl_control_set_surface_rotation(d->controller,
								surface->id,
								surface->rotation);
					return 0;
				}
				if (surface->z == d->surface_count) {
					/* The selected surface is the last
					 * in the list, so there is nothing
					 * below it */
					return 0;
				}
				/* Find the previous surface in the stack */
				wl_list_for_each(t_surface, &d->surface_list, link) {
					if (t_surface->z == surface->z + 1) {
						t_surface->selected = 1;
						surface->selected = 0;
						return 0;
					}
				}
			}
		}
		break;
	case KEY_LEFT:
		wl_list_for_each(surface, &d->surface_list, link) {
			if (surface->selected) {
				if (d->move_surface) {
					wl_control_move_surface(d->controller,
								surface->id,
								surface->x -
								s_increment,
								surface->y);
					return 0;
				}
				if (d->resize_surface) {
					wl_control_resize_surface(d->controller,
								surface->id,
								surface->width -
								s_increment,
								surface->height);
					return 0;
				}
				if (d->alpha_surface) {
					wl_control_set_surface_alpha(d->controller,
								surface->id,
								surface->alpha -
								s_increment * 2);
					return 0;
				}
				if (d->rotate_surface) {
					surface->rotation -= s_increment;
					if (surface->rotation < 0)
						surface->rotation = surface->rotation + 360;
					wl_control_set_surface_rotation(d->controller,
								surface->id,
								surface->rotation);
					return 0;
				}
				if (surface->z == 1) {
					/* The selected surface is the first
					 * in the list, so there is nothing
					 * above it */
					return 0;
				}
				/* Find the previous surface in the stack */
				wl_list_for_each(t_surface, &d->surface_list, link) {
					if (t_surface->z == surface->z - 1) {
						t_surface->selected = 1;
						surface->selected = 0;
						return 0;
					}
				}
			}
		}
		break;
	case KEY_RIGHT:
		wl_list_for_each(surface, &d->surface_list, link) {
			if (surface->selected) {
				if (d->move_surface) {
					wl_control_move_surface(d->controller,
								surface->id,
								surface->x +
								s_increment,
								surface->y);
					return 0;
				}
				if (d->resize_surface) {
					wl_control_resize_surface(d->controller,
								surface->id,
								surface->width +
								s_increment,
								surface->height);
					return 0;
				}
				if (d->alpha_surface) {
					wl_control_set_surface_alpha(d->controller,
								surface->id,
								surface->alpha +
								s_increment * 2);
					return 0;
				}
				if (d->rotate_surface) {
					surface->rotation += s_increment;
					if (surface->rotation > 360)
						surface->rotation = surface->rotation - 360;
					wl_control_set_surface_rotation(d->controller,
								surface->id,
								surface->rotation);
					return 0;
				}
				if (surface->z == d->surface_count) {
					/* The selected surface is the last
					 * in the list, so there is nothing
					 * below it */
					return 0;
				}
				/* Find the previous surface in the stack */
				wl_list_for_each(t_surface, &d->surface_list, link) {
					if (t_surface->z == surface->z + 1) {
						t_surface->selected = 1;
						surface->selected = 0;
						return 0;
					}
				}
			}
		}
		break;
	}

	return 0;
}

static void
run_ncurses_wl_interface(struct display *d)
{
	struct surface *surface;
	struct winsize size;
	WINDOW *win;
	int width, height;
	int item_selected;

	win = initscr();

	cbreak();
	noecho();
	curs_set(0);
	nodelay(win, 1);
	keypad(stdscr, TRUE);
	getmaxyx(win, height, width);

	signal(SIGWINCH, sigwinch_handler);

	start_color();
	use_default_colors();
	init_pair(1, COLOR_CYAN, -1);
	init_pair(2, COLOR_WHITE, COLOR_BLUE);
	init_pair(3, COLOR_RED, -1);
	init_pair(4, COLOR_GREEN, -1);

	init_pair(5, COLOR_YELLOW, -1);
	init_pair(6, COLOR_WHITE, COLOR_GREEN);
	init_pair(7, COLOR_WHITE, COLOR_CYAN);

	/* Quit on 'q' or Esc */
	while (running) {

		wl_display_roundtrip(d->display);

		if (input_handler(d))
			break;

		if (resize_needed) {
			resize_needed = 0;

			if (ioctl(fileno(stdout), TIOCGWINSZ, &size) == 0)
			{
				width = size.ws_col;
				height = size.ws_row;
				resizeterm(size.ws_row, size.ws_col);
			}
			clear();
		}
		draw_screen(d, win, width, height);
		refresh();

		/* Make sure at least one item in the list is selected */
		item_selected = 0;
		if (!wl_list_empty(&d->surface_list)) {
			wl_list_for_each(surface, &d->surface_list, link) {
				if (surface->selected) {
					item_selected = 1;
					break;
				}
			}
			if (!item_selected)
				container_of(d->surface_list.next,
					struct surface, link)->selected = 1;
		}
		usleep(100000);
	}

	clear();
	endwin();
}

static struct surface*
create_surface(void)
{
	struct surface *surface;

	surface = calloc(1, sizeof *surface);

	if (!surface) {
		fprintf(stderr, "ERROR: Failed to allocate memory!\n");
		exit(-1);
	}

	return surface;
}

static void
destroy_surface(struct surface *surface)
{
	wl_list_remove(&surface->link);
	free(surface->title);
	free(surface->type);
	free(surface);
}

static struct display*
create_display(void)
{
	struct display *d;

	d = calloc(1, sizeof *d);

	if (!d) {
		fprintf(stderr, "ERROR: Failed to allocate memory!\n");
		exit(-1);
	}

	wl_list_init(&d->surface_list);

	return d;
}

static void
destroy_display(struct display *d)
{
	struct surface *surface, *next;

	wl_list_for_each_safe(surface, next, &d->surface_list, link)
		destroy_surface(surface);

	wl_compositor_destroy(d->compositor);
	wl_display_flush(d->display);
	wl_display_disconnect(d->display);

	free(d);
}

/* Wayland stuff */
static void
surface_info_handler(void *data, struct wl_control *wl_control,
						uint32_t id,
						uint32_t z,
						const char *title,
						const char *type,
						int32_t x, int32_t y,
						uint32_t width, uint32_t height,
						uint32_t alpha)
{
	struct display *d = data;
	struct surface *surface;

	wl_list_for_each(surface, &d->surface_list, link) {
		if (id == surface->id) {
			if (!d->no_ncurses) {
				/* If this surface is already in
				 * our list, just update its info */
				surface->id = id;
				surface->z = z;
				surface->title = strdup(title);
				surface->type = strdup(type);
				surface->x = x;
				surface->y = y;
				surface->width = width;
				surface->height = height;
				surface->alpha = alpha;
			}

			return;
		}
	}

	surface = create_surface();

	surface->id = id;
	surface->z = z;
	surface->title = strdup(title);
	surface->type = strdup(type);
	surface->x = x;
	surface->y = y;
	surface->width = width;
	surface->height = height;
	surface->alpha = alpha;
	surface->rotation = 0;

	if (d->no_ncurses) {
		printf("id = %d\n", id);
		printf("z = %d\n", z);
		printf("title = %s\n", title);
		printf("type = %s\n", type);
		printf("position = %d, %d\n", x, y);
		printf("size = %d, %d\n", width, height);
		printf("alpha = %d\n\n", alpha);

		/* Keep track of surfaces to avoid printing duplicates */
		wl_list_init(&surface->link);
		wl_list_insert(&d->surface_list, &surface->link);

		return;
	}

	if (wl_list_empty(&d->surface_list))
		surface->selected = 1;

	wl_list_init(&surface->link);
	wl_list_insert(&d->surface_list, &surface->link);
	d->surface_count++;
}

static void
surface_destroy_handler(void *data, struct wl_control *wl_control,
						uint32_t id)
{
	struct display *d = data;
	struct surface *surface, *next;

	wl_list_for_each_safe(surface, next, &d->surface_list, link) {
		if (id == surface->id) {
			destroy_surface(surface);
			d->surface_count--;
			clear();
			return;
		}
	}
}

static const struct wl_control_listener surface_info_listener = {
	surface_info_handler,
	surface_destroy_handler
};

static void
handle_global(struct wl_display *display, uint32_t id,
	      const char *interface, uint32_t version, void *data)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0)
		d->compositor =
			wl_display_bind(display, id, &wl_compositor_interface);
	else if (strcmp(interface, "wl_control") == 0)
		d->controller = wl_display_bind(display, id, &wl_control_interface);
}

static void
signal_int(int signum)
{
	running = 0;
}

int main(int argc, char *argv[])
{
	struct display *d;
	struct sigaction sigint;
	uint32_t window_id = 0, i;
	int32_t x, y;
	uint32_t width, height;
	char *position = NULL;
	char *size = NULL;
	char *alpha = NULL;
	char *rotation = NULL;
	char *key_event = NULL;
	char *color = NULL;
	char *crop = NULL;
	struct wl_region *crop_region;
	uint32_t a = 0, r = 0;
	uint32_t hide = 0, focus = 0, raise = 0, kill = 0;
	uint32_t key = 0, state = 0;
	uint32_t red = 0, green = 0, blue = 0;
	uint32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;

	d = create_display();

	d->display = wl_display_connect(NULL);

	if (d->display == NULL) {
		fprintf(stderr, "failed to create d->display: %m\n");
		return -1;
	}

	wl_display_add_global_listener(d->display, handle_global, d);
	wl_display_roundtrip(d->display);

	if (d->controller == NULL) {
		fprintf(stderr, "d->display doesn't support d->controller interface\n");
		return -1;
	}

	const struct weston_option wl_control_options[] = {
		{ WESTON_OPTION_BOOLEAN, "info", 'n', &d->no_ncurses },
		{ WESTON_OPTION_INTEGER, "id", 'i', &window_id },
		{ WESTON_OPTION_STRING, "position", 'p', &position },
		{ WESTON_OPTION_STRING, "size", 's', &size },
		{ WESTON_OPTION_STRING, "alpha", 'a', &alpha },
		{ WESTON_OPTION_BOOLEAN, "hide-toggle", 'h', &hide },
		{ WESTON_OPTION_STRING, "rotation", 0, &rotation },
		{ WESTON_OPTION_STRING, "key", 'k', &key_event },
		{ WESTON_OPTION_BOOLEAN, "focus", 'f', &focus },
		{ WESTON_OPTION_BOOLEAN, "raise", 'r', &raise },
		{ WESTON_OPTION_STRING, "theme", 't', &color },
		{ WESTON_OPTION_STRING, "crop", 'c', &crop },
		{ WESTON_OPTION_BOOLEAN, "kill", 0, &kill },
	};

	argc = parse_options(wl_control_options,
			     ARRAY_LENGTH(wl_control_options), argc, argv);
	for (i = 1; argv[i]; i++)
		printf("warning: unhandled option: %s\n", argv[i]);

	if (position) {
		if (sscanf(position, "%d,%d", &x, &y) != 2) {
			printf("Invalid position\n");
			return -1;
		}
		wl_control_move_surface(d->controller, window_id, x, y);
	}

	if (size) {
		if (sscanf(size, "%d,%d", &width, &height) != 2) {
			printf("Invalid size\n");
			return -1;
		}
		wl_control_resize_surface(d->controller, window_id, width, height);
	}

	if (alpha) {
		if (sscanf(alpha, "%d", &a) != 1) {
			printf("Invalid alpha value\n");
			return -1;
		}

		if (a > 255)
			a = 255;

		wl_control_set_surface_alpha(d->controller, window_id, a);
	}

	if (hide)
		wl_control_surface_toggle_hide(d->controller, window_id);

	if (rotation) {
		if (sscanf(rotation, "%d", &r) != 1) {
			printf("Invalid rotation value\n");
			return -1;
		}

		if (r > 360)
			r = 360;

		wl_control_set_surface_rotation(d->controller, window_id, r);
	}

	if (key_event) {
		/* Key codes can be found in kernel header include/linux/input.h */
		if (sscanf(key_event, "%d,%d", &key, &state) == 2) {
			if (state != 0 && state != 1) {
				printf("Invalid key state value\n");
				return -1;
			}
			if (key < 1 || key > 226) {
				printf("Invalid key value\n");
				return -1;
			}
			wl_control_send_key_event(d->controller, key, state);
		} else if (sscanf(key_event, "%d", &key) == 1) {
			if (key < 1 || key > 226) {
				printf("Invalid key value\n");
				return -1;
			}
			wl_control_send_key_event(d->controller, key, 1);
			wl_display_roundtrip(d->display);
			wl_control_send_key_event(d->controller, key, 0);
		} else {
			printf("Invalid key event\n");
			return -1;
		}
	}

	if (focus)
		wl_control_focus_surface(d->controller, window_id);

	if (raise)
		wl_control_raise_surface(d->controller, window_id);

	if (color) {
		if (sscanf(color, "%d,%d,%d", &red, &green, &blue) != 3) {
			printf("Invalid theme color\n");
			return -1;
		}

		if (red > 255)
			red = 255;
		if (green > 255)
			green = 255;
		if (blue > 255)
			blue = 255;

		wl_control_set_theme_color(d->controller, red, green, blue);
	}

	if (crop) {
		if (!strcmp(crop, "reset") || !strcmp(crop, "r"))
			wl_control_reset_crop_region(d->controller, window_id);
		else {
			if (sscanf(crop, "%d,%d,%d,%d", &x1, &y1, &x2, &y2) != 4) {
				printf("Invalid crop region\n");
				return -1;
			}
			crop_region = wl_compositor_create_region(d->compositor);
			wl_region_add(crop_region, x1, y1, x2, y2);

			wl_control_set_crop_region(d->controller, window_id, crop_region);
		}
	}

	if (kill)
		wl_control_kill_surface(d->controller, window_id);

	if (position || size || alpha || hide || rotation || key_event ||
					focus || raise || color || crop || kill) {
		wl_display_roundtrip(d->display);
		/* Don't print info after sending an action command */
		return 0;
	}

	if (!d->controller) {
		printf("Failed to add d->controller listener\n");
		return -1;
	}

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	if (d->no_ncurses) {
		wl_control_add_listener(d->controller, &surface_info_listener, d);
		wl_control_get_surface_info(d->controller);
		wl_display_roundtrip(d->display);
	}
	else {
		sleep(1);
		wl_control_add_listener(d->controller, &surface_info_listener, d);
		run_ncurses_wl_interface(d);
	}

	destroy_display(d);

	return 0;
}
