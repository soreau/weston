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

struct {
	struct weston_compositor *compositor;
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
} ezoom;

static struct weston_seat *
weston_zoom_pick_seat(struct weston_compositor *compositor)
{
	return container_of(compositor->seat_list.next,
			    struct weston_seat, link);
}

static void
weston_zoom_frame_z(struct weston_animation *animation,
		struct weston_output *output, uint32_t msecs)
{
	if (animation->frame_counter <= 1)
		ezoom.spring_z.timestamp = msecs;

	weston_spring_update(&ezoom.spring_z, msecs);

	if (ezoom.spring_z.current > ezoom.max_level)
		ezoom.spring_z.current = ezoom.max_level;
	else if (ezoom.spring_z.current < 0.0)
		ezoom.spring_z.current = 0.0;

	if (weston_spring_done(&ezoom.spring_z)) {
		if (ezoom.active && ezoom.level <= 0.0) {
			ezoom.active = output->compositor->filter_linear = 0;
			output->disable_planes--;
			wl_list_remove(&ezoom.motion_listener.link);
		}
		ezoom.spring_z.current = ezoom.level;
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

	if (animation->frame_counter <= 1)
		ezoom.spring_xy.timestamp = msecs;

	weston_spring_update(&ezoom.spring_xy, msecs);

	x = ezoom.from.x - ((ezoom.from.x - ezoom.to.x) *
						ezoom.spring_xy.current);
	y = ezoom.from.y - ((ezoom.from.y - ezoom.to.y) *
						ezoom.spring_xy.current);

	ezoom.current.x = x;
	ezoom.current.y = y;

	if (weston_spring_done(&ezoom.spring_xy)) {
		ezoom.spring_xy.current = ezoom.spring_xy.target;
		ezoom.current.x = seat->pointer->x;
		ezoom.current.y = seat->pointer->y;
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
	float level = ezoom.spring_z.current;
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
	float global_x, global_y;
	wl_fixed_t x = ezoom.current.x;
	wl_fixed_t y = ezoom.current.y;
	float trans_min, trans_max;
	float ratio, level;

	level = ezoom.spring_z.current;
	ratio = 1 / level;

	if (!ezoom.active || level > ezoom.max_level ||
	    level == 0.0f)
		return;

	if (wl_list_empty(&ezoom.animation_xy.link))
		zoom_area_center_from_pointer(output, &x, &y);

	global_x = wl_fixed_to_double(x);
	global_y = wl_fixed_to_double(y);

	ezoom.trans_x =
		((((global_x - output->x) / output->width) *
		(level * 2)) - level) * ratio;
	ezoom.trans_y =
		((((global_y - output->y) / output->height) *
		(level * 2)) - level) * ratio;

	trans_max = level * 2 - level;
	trans_min = -trans_max;

	/* Clip zoom area to output */
	if (ezoom.trans_x > trans_max)
		ezoom.trans_x = trans_max;
	else if (ezoom.trans_x < trans_min)
		ezoom.trans_x = trans_min;
	if (ezoom.trans_y > trans_max)
		ezoom.trans_y = trans_max;
	else if (ezoom.trans_y < trans_min)
		ezoom.trans_y = trans_min;
}

static void
weston_zoom_transition(struct weston_output *output, wl_fixed_t x, wl_fixed_t y)
{
	if (ezoom.level != ezoom.spring_z.current) {
		ezoom.spring_z.target = ezoom.level;
		if (wl_list_empty(&ezoom.animation_z.link)) {
			ezoom.animation_z.frame_counter = 0;
			wl_list_insert(output->animation_list.prev,
				&ezoom.animation_z.link);
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

	zoom_area_center_from_pointer(output, &x, &y);

	if (wl_list_empty(&ezoom.animation_xy.link)) {
		ezoom.current.x = seat->pointer->x;
		ezoom.current.y = seat->pointer->y;
	} else {
		ezoom.to.x = x;
		ezoom.to.y = y;
	}

	weston_zoom_transition(output, x, y);
	weston_output_update_zoom_transform(output);
}

static void
output_set_transform_coords(struct weston_output *output, wl_fixed_t *tx, wl_fixed_t *ty)
{
	float zoom_scale, zx, zy;

	if (ezoom.active) {
		zoom_scale = ezoom.spring_z.current;
		zx = (wl_fixed_to_double(*tx) * (1.0f - zoom_scale) +
		      output->width / 2.0f *
		      (zoom_scale + ezoom.trans_x));
		zy = (wl_fixed_to_double(*ty) * (1.0f - zoom_scale) +
		      output->height / 2.0f *
		      (zoom_scale + ezoom.trans_y));
		*tx = wl_fixed_from_double(zx);
		*ty = wl_fixed_from_double(zy);
	}

	weston_output_update_zoom(output);
}

static void
output_update_matrix(struct weston_output *output)
{
	float magnification;

	if (ezoom.active) {
		magnification = 1 / (1 - ezoom.spring_z.current);
		weston_output_update_zoom(output);
		weston_matrix_translate(&output->matrix, -ezoom.trans_x,
					ezoom.trans_y, 0);
		weston_matrix_scale(&output->matrix, magnification,
				    magnification, 1.0);
	}
}

static void
weston_output_activate_zoom(struct weston_output *output)
{
	struct weston_seat *seat = weston_zoom_pick_seat(output->compositor);

	if (ezoom.active)
		return;

	ezoom.active = output->compositor->filter_linear = 1;
	output->disable_planes++;
	wl_signal_add(&seat->pointer->motion_signal,
		      &ezoom.motion_listener);
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
	float increment;

	wl_list_for_each(output, &compositor->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						   wl_fixed_to_double(seat->pointer->x),
						   wl_fixed_to_double(seat->pointer->y),
						   NULL)) {
			if (key == KEY_PAGEUP)
				increment = ezoom.increment;
			else if (key == KEY_PAGEDOWN)
				increment = -ezoom.increment;
			else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
				/* For every pixel zoom 20th of a step */
				increment = ezoom.increment *
					    -wl_fixed_to_double(value) / 20.0;
			else
				increment = 0;

			ezoom.level += increment;

			if (ezoom.level < 0.0)
				ezoom.level = 0.0;
			else if (ezoom.level > ezoom.max_level)
				ezoom.level = ezoom.max_level;
			else if (!ezoom.active) {
				weston_output_activate_zoom(output);
			}

			ezoom.spring_z.target = ezoom.level;

			weston_output_update_zoom(output);
		}
	}
}

static void
fini(struct weston_plugin *plugin)
{
}

static int
init(struct weston_compositor *compositor)
{
	ezoom.compositor = compositor;
	ezoom.active = 0;
	ezoom.increment = 0.07;
	ezoom.max_level = 0.95;
	ezoom.level = 0.0;
	ezoom.trans_x = 0.0;
	ezoom.trans_y = 0.0;
	weston_spring_init(&ezoom.spring_z, 250.0, 0.0, 0.0);
	ezoom.spring_z.friction = 1000;
	ezoom.animation_z.frame = weston_zoom_frame_z;
	wl_list_init(&ezoom.animation_z.link);
	weston_spring_init(&ezoom.spring_xy, 250.0, 0.0, 0.0);
	ezoom.spring_xy.friction = 1000;
	ezoom.animation_xy.frame = weston_zoom_frame_xy;
	wl_list_init(&ezoom.animation_xy.link);
	ezoom.motion_listener.notify = motion;

	return 0;
}

WL_EXPORT struct weston_plugin_interface plugin_interface = {
	.init = init,
	.fini = fini,
	.input_action = input_action,
	.output_set_transform_coords = output_set_transform_coords,
	.output_update_matrix = output_update_matrix,
};
