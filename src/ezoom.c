/*
 * Copyright © 2014 Scott Moreau
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

#include <stdlib.h>
#include <linux/input.h>

#include "compositor.h"


struct weston_fixed_point {
	wl_fixed_t x, y;
};

struct ezoom_output {
	struct weston_output *output;
	int active;
	float increment;
	float level;
	float max_level;
	float trans_x, trans_y;
	struct weston_animation animation_z;
	struct weston_spring spring_z;
	struct weston_animation animation_xy;
	struct weston_spring spring_xy;
	struct weston_fixed_point from;
	struct weston_fixed_point to;
	struct weston_fixed_point current;
	struct wl_listener motion_listener;
	struct wl_list link;
};

struct {
	struct weston_compositor *compositor;
	struct wl_list output_list;
} ezoom;

static struct weston_seat *
weston_zoom_pick_seat(struct weston_compositor *compositor)
{
	return container_of(compositor->seat_list.next,
			    struct weston_seat, link);
}

static int
output_contains_point(struct weston_output *output)
{
	struct weston_seat *seat;

	seat = weston_zoom_pick_seat(output->compositor);

	if (pixman_region32_contains_point(&output->region,
					   wl_fixed_to_int(seat->pointer->x),
					   wl_fixed_to_int(seat->pointer->y),
					   NULL))
		return 1;
	return 0;
}

static struct ezoom_output *
get_output(struct weston_output *output)
{
	struct ezoom_output *ezoom_output;

	wl_list_for_each(ezoom_output, &ezoom.output_list, link) {
		if (ezoom_output->output == output)
			return ezoom_output;
	}

	return NULL;
}

static void
weston_zoom_frame_z(struct weston_animation *animation,
		struct weston_output *output, uint32_t msecs)
{
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)))
		return;

	if (animation->frame_counter <= 1)
		ezoom_output->spring_z.timestamp = msecs;

	weston_spring_update(&ezoom_output->spring_z, msecs);

	if (ezoom_output->spring_z.current > ezoom_output->max_level)
		ezoom_output->spring_z.current = ezoom_output->max_level;
	else if (ezoom_output->spring_z.current < 0.0)
		ezoom_output->spring_z.current = 0.0;

	if (weston_spring_done(&ezoom_output->spring_z)) {
		if (ezoom_output->active && ezoom_output->level <= 0.0) {
			ezoom_output->active = output->compositor->filter_linear = 0;
			output->disable_planes--;
			wl_list_remove(&ezoom_output->motion_listener.link);
		}
		ezoom_output->spring_z.current = ezoom_output->level;
		wl_list_remove(&animation->link);
		wl_list_init(&animation->link);
	}

	output->dirty = 1;
	weston_output_damage(output);
}

static void
weston_zoom_frame_xy(struct weston_animation *animation,
		struct weston_output *output, uint32_t msecs)
{
	struct weston_seat *seat = weston_zoom_pick_seat(output->compositor);
	wl_fixed_t x, y;
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)))
		return;

	if (animation->frame_counter <= 1)
		ezoom_output->spring_xy.timestamp = msecs;

	weston_spring_update(&ezoom_output->spring_xy, msecs);

	x = ezoom_output->from.x - ((ezoom_output->from.x - ezoom_output->to.x) *
						ezoom_output->spring_xy.current);
	y = ezoom_output->from.y - ((ezoom_output->from.y - ezoom_output->to.y) *
						ezoom_output->spring_xy.current);

	ezoom_output->current.x = x;
	ezoom_output->current.y = y;

	if (weston_spring_done(&ezoom_output->spring_xy)) {
		ezoom_output->spring_xy.current = ezoom_output->spring_xy.target;
		ezoom_output->current.x = seat->pointer->x;
		ezoom_output->current.y = seat->pointer->y;
		wl_list_remove(&animation->link);
		wl_list_init(&animation->link);
	}

	output->dirty = 1;
	weston_output_damage(output);
}

static void
zoom_area_center_from_pointer(struct weston_output *output,
				wl_fixed_t *x, wl_fixed_t *y)
{
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)))
		return;

	float level = ezoom_output->spring_z.current;
	wl_fixed_t offset_x = wl_fixed_from_int(output->x);
	wl_fixed_t offset_y = wl_fixed_from_int(output->y);
	wl_fixed_t w = wl_fixed_from_int(output->width);
	wl_fixed_t h = wl_fixed_from_int(output->height);

	*x -= ((((*x - offset_x) / (float) w) - 0.5) * (w * (1.0 - level)));
	*y -= ((((*y - offset_y) / (float) h) - 0.5) * (h * (1.0 - level)));
}

static void
weston_output_update_zoom_transform(struct weston_output *output)
{
	struct ezoom_output *ezoom_output;
	float trans_min, trans_max;
	float global_x, global_y;
	float ratio, level;
	wl_fixed_t x, y;

	if (!(ezoom_output = get_output(output)))
		return;

	x = ezoom_output->current.x;
	y = ezoom_output->current.y;

	level = ezoom_output->spring_z.current;
	ratio = 1 / level;

	if (!ezoom_output->active || level > ezoom_output->max_level ||
	    level == 0.0f)
		return;

	if (wl_list_empty(&ezoom_output->animation_xy.link))
		zoom_area_center_from_pointer(output, &x, &y);

	global_x = wl_fixed_to_double(x);
	global_y = wl_fixed_to_double(y);

	ezoom_output->trans_x =
		((((global_x - output->x) / output->width) *
		(level * 2)) - level) * ratio;
	ezoom_output->trans_y =
		((((global_y - output->y) / output->height) *
		(level * 2)) - level) * ratio;

	trans_max = level * 2 - level;
	trans_min = -trans_max;

	/* Clip zoom area to output */
	if (ezoom_output->trans_x > trans_max)
		ezoom_output->trans_x = trans_max;
	else if (ezoom_output->trans_x < trans_min)
		ezoom_output->trans_x = trans_min;
	if (ezoom_output->trans_y > trans_max)
		ezoom_output->trans_y = trans_max;
	else if (ezoom_output->trans_y < trans_min)
		ezoom_output->trans_y = trans_min;
}

static void
weston_zoom_transition(struct weston_output *output)
{
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)))
		return;

	if (ezoom_output->level != ezoom_output->spring_z.current) {
		ezoom_output->spring_z.target = ezoom_output->level;
		if (wl_list_empty(&ezoom_output->animation_z.link)) {
			ezoom_output->animation_z.frame_counter = 0;
			wl_list_insert(output->animation_list.prev,
				&ezoom_output->animation_z.link);
		}
	}

	output->dirty = 1;
	weston_output_damage(output);
}

static void
weston_output_update_zoom(struct weston_output *output)
{
	struct weston_seat *seat = weston_zoom_pick_seat(output->compositor);
	wl_fixed_t x = seat->pointer->x;
	wl_fixed_t y = seat->pointer->y;
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)) ||
	     !output_contains_point(output))
		return;

	zoom_area_center_from_pointer(output, &x, &y);

	if (wl_list_empty(&ezoom_output->animation_xy.link)) {
		ezoom_output->current.x = seat->pointer->x;
		ezoom_output->current.y = seat->pointer->y;
	} else {
		ezoom_output->to.x = x;
		ezoom_output->to.y = y;
	}

	weston_zoom_transition(output);
	weston_output_update_zoom_transform(output);
}

static void
output_set_transform_coords(struct weston_output *output, wl_fixed_t *tx, wl_fixed_t *ty)
{
	float zoom_scale, zx, zy;
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)))
		return;

	if (ezoom_output->active) {
		zoom_scale = ezoom_output->spring_z.current;
		zx = (wl_fixed_to_double(*tx) * (1.0f - zoom_scale) +
		      output->width / 2.0f *
		      (zoom_scale + ezoom_output->trans_x));
		zy = (wl_fixed_to_double(*ty) * (1.0f - zoom_scale) +
		      output->height / 2.0f *
		      (zoom_scale + ezoom_output->trans_y));
		*tx = wl_fixed_from_double(zx);
		*ty = wl_fixed_from_double(zy);
	}

	weston_output_update_zoom(output);
}

static void
output_update_matrix(struct weston_output *output)
{
	float magnification;
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)))
		return;

	if (ezoom_output->active) {
		magnification = 1 / (1 - ezoom_output->spring_z.current);
		weston_output_update_zoom(output);
		weston_matrix_translate(&output->matrix, -ezoom_output->trans_x,
					ezoom_output->trans_y, 0);
		weston_matrix_scale(&output->matrix, magnification,
				    magnification, 1.0);
	}
}

static void
weston_output_activate_zoom(struct weston_output *output)
{
	struct weston_seat *seat = weston_zoom_pick_seat(output->compositor);
	struct ezoom_output *ezoom_output;

	if (!(ezoom_output = get_output(output)))
		return;

	if (ezoom_output->active)
		return;

	ezoom_output->active = output->compositor->filter_linear = 1;
	output->disable_planes++;
	wl_signal_add(&seat->pointer->motion_signal,
		      &ezoom_output->motion_listener);
}

static void
motion(struct wl_listener *listener, void *data)
{
	struct weston_compositor *compositor = ezoom.compositor;
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link) {
		weston_output_update_zoom(output);
	}
}

static void
input_action(struct weston_seat *seat, uint32_t time, uint32_t key, uint32_t axis, wl_fixed_t value)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *compositor = ws->compositor;
	struct weston_output *output;
	struct ezoom_output *ezoom_output;
	float increment;

	wl_list_for_each(output, &compositor->output_list, link) {
		if (!(ezoom_output = get_output(output)))
			continue;
		if (output_contains_point(output)) {
			if (key == KEY_PAGEUP)
				increment = ezoom_output->increment;
			else if (key == KEY_PAGEDOWN)
				increment = -ezoom_output->increment;
			else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
				/* For every pixel zoom 20th of a step */
				increment = ezoom_output->increment *
					    -wl_fixed_to_double(value) / 20.0;
			else
				increment = 0;

			ezoom_output->level += increment;

			if (ezoom_output->level < 0.0)
				ezoom_output->level = 0.0;
			else if (ezoom_output->level > ezoom_output->max_level)
				ezoom_output->level = ezoom_output->max_level;
			else if (!ezoom_output->active) {
				weston_output_activate_zoom(output);
			}

			ezoom_output->spring_z.target = ezoom_output->level;

			weston_output_update_zoom(output);
		}
	}
}

static void
fini(struct weston_compositor *compositor)
{
	struct ezoom_output *ezoom_output, *next;

	wl_list_for_each_safe(ezoom_output, next, &ezoom.output_list, link)
		free(ezoom_output);
}

static int
init(struct weston_compositor *compositor)
{
	struct ezoom_output *ezoom_output;
	struct weston_output *output;

	ezoom.compositor = compositor;
	wl_list_init(&ezoom.output_list);

	wl_list_for_each(output, &compositor->output_list, link) {
		ezoom_output = zalloc(sizeof *ezoom_output);

		ezoom_output->output = output;
		ezoom_output->active = 0;
		ezoom_output->increment = 0.07;
		ezoom_output->max_level = 0.95;
		ezoom_output->level = 0.0;
		ezoom_output->trans_x = 0.0;
		ezoom_output->trans_y = 0.0;
		weston_spring_init(&ezoom_output->spring_z, 250.0, 0.0, 0.0);
		ezoom_output->spring_z.friction = 1000;
		ezoom_output->animation_z.frame = weston_zoom_frame_z;
		wl_list_init(&ezoom_output->animation_z.link);
		weston_spring_init(&ezoom_output->spring_xy, 250.0, 0.0, 0.0);
		ezoom_output->spring_xy.friction = 1000;
		ezoom_output->animation_xy.frame = weston_zoom_frame_xy;
		wl_list_init(&ezoom_output->animation_xy.link);
		ezoom_output->motion_listener.notify = motion;

		wl_list_insert(&ezoom.output_list, &ezoom_output->link);
	}

	return 0;
}

WL_EXPORT struct weston_plugin_interface plugin_interface = {
	.init = init,
	.fini = fini,
	.input_action = input_action,
	.output_set_transform_coords = output_set_transform_coords,
	.output_update_matrix = output_update_matrix,
};
