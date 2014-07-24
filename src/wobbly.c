/*
 * Copyright © 2005 Novell, Inc.
 * Copyright © 2014 Scott Moreau
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of
 * Novell, Inc. not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior permission.
 * Novell, Inc. makes no representations about the suitability of this
 * software for any purpose. It is provided "as is" without express or
 * implied warranty.
 *
 * NOVELL, INC. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL NOVELL, INC. BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author: David Reveman <davidr@novell.com>
 *         Scott Moreau  <oreaus@gmail.com>
 */

/*
 * Spring model implemented by Kristian Hogsberg.
 */

#include <values.h>

#include "compositor.h"
#include "gl-renderer.h"

#define WOBBLY_FRICTION 3
#define WOBBLY_SPRING_K 8

#define GRID_WIDTH  4
#define GRID_HEIGHT 4

#define MODEL_MAX_SPRINGS (GRID_WIDTH * GRID_HEIGHT * 2)

#define MASS 50.0f

typedef struct _xy_pair {
	float	x, y;
} Point, Vector;

typedef struct _Edge {
	float	next, prev;

	float	start;
	float	end;

	float	attract;
	float	velocity;
} Edge;

typedef struct _Object {
	Vector	force;
	Point	position;
	Vector	velocity;
	float	theta;
	int	immobile;
	Edge	vertEdge;
	Edge	horzEdge;
} Object;

typedef struct _Spring {
	Object	*a;
	Object	*b;
	Vector	offset;
} Spring;

typedef struct _Model {
	Object	*objects;
	int	numObjects;
	Spring	springs[MODEL_MAX_SPRINGS];
	int	numSprings;
	Object	*anchorObject;
	float	steps;
	Point	topLeft;
	Point	bottomRight;
} Model;

typedef struct _WobblyWindow {
	Model		*model;
	int		wobbly;
	int		grabbed;
	int		velocity;
	unsigned int	state;
} WobblyWindow;

struct surface {
	WobblyWindow *ww;
	struct weston_surface *surface;
	struct weston_transform transform;
	float	x, y;
	int	width, height;
	int	last_x, last_y;
	int	x_cells, y_cells;
	int	synced;

	struct wl_list link;
};

struct {
	struct wl_list surface_list;
} wobbly;

static struct surface *
get_surface(struct weston_surface *surface)
{
	struct surface *ws;

	wl_list_for_each(ws, &wobbly.surface_list, link)
		if (ws->surface == surface)
			return ws;

	return NULL;
}

static struct surface *
get_wobbly_surface(struct weston_surface *surface)
{
	struct surface *ws, *next;

	wl_list_for_each_safe(ws, next, &wobbly.surface_list, link)
		if ((ws = get_surface(surface)) &&
			(ws->ww->grabbed || !ws->synced))
			return ws;

	return NULL;
}

#define WobblyInitial	(1L << 0)
#define WobblyForce	(1L << 1)
#define WobblyVelocity	(1L << 2)

static void
objectInit(Object *object,
		float  positionX,
		float  positionY,
		float  velocityX,
		float  velocityY)
{
	object->force.x = 0;
	object->force.y = 0;

	object->position.x = positionX;
	object->position.y = positionY;

	object->velocity.x = velocityX;
	object->velocity.y = velocityY;

	object->theta	= 0;
	object->immobile = 0;

	object->vertEdge.next = 0.0f;
	object->horzEdge.next = 0.0f;
}

static void
springInit(Spring *spring,
		Object *a,
		Object *b,
		float  offsetX,
		float  offsetY)
{
	spring->a		 = a;
	spring->b		 = b;
	spring->offset.x = offsetX;
	spring->offset.y = offsetY;
}

static void
modelCalcBounds(Model *model)
{
	int i;

	model->topLeft.x	 = MAXSHORT;
	model->topLeft.y	 = MAXSHORT;
	model->bottomRight.x = MINSHORT;
	model->bottomRight.y = MINSHORT;

	for (i = 0; i < model->numObjects; i++)
	{
		if (model->objects[i].position.x < model->topLeft.x)
			model->topLeft.x = model->objects[i].position.x;
		else if (model->objects[i].position.x > model->bottomRight.x)
			model->bottomRight.x = model->objects[i].position.x;

		if (model->objects[i].position.y < model->topLeft.y)
			model->topLeft.y = model->objects[i].position.y;
		else if (model->objects[i].position.y > model->bottomRight.y)
			model->bottomRight.y = model->objects[i].position.y;
	}
}

static void
modelAddSpring(Model  *model,
		Object *a,
		Object *b,
		float  offsetX,
		float  offsetY)
{
	Spring *spring;

	spring = &model->springs[model->numSprings];
	model->numSprings++;

	springInit(spring, a, b, offsetX, offsetY);
}

static void
modelSetMiddleAnchor(Model *model,
			  int   x,
			  int   y,
			  int   width,
			  int   height)
{
	float gx, gy;

	gx = ((GRID_WIDTH  - 1) / 2 * width)  / (float) (GRID_WIDTH  - 1);
	gy = ((GRID_HEIGHT - 1) / 2 * height) / (float) (GRID_HEIGHT - 1);

	if (model->anchorObject)
		model->anchorObject->immobile = 0;

	model->anchorObject = &model->objects[GRID_WIDTH *
					  ((GRID_HEIGHT - 1) / 2) +
					  (GRID_WIDTH - 1) / 2];
	model->anchorObject->position.x = x + gx;
	model->anchorObject->position.y = y + gy;

	model->anchorObject->immobile = 1;
}

static void
modelInitObjects(Model *model,
		  int	x,
		  int   y,
		  int	width,
		  int	height)
{
	int gridX, gridY, i = 0;
	float gw, gh;

	gw = GRID_WIDTH  - 1;
	gh = GRID_HEIGHT - 1;

	for (gridY = 0; gridY < GRID_HEIGHT; gridY++)
	{
		for (gridX = 0; gridX < GRID_WIDTH; gridX++)
		{
			objectInit(&model->objects[i],
				x + (gridX * width) / gw,
				y + (gridY * height) / gh,
				0, 0);
			i++;
		}
	}

	modelSetMiddleAnchor(model, x, y, width, height);
}

static void
modelInitSprings(Model *model,
		  int   x,
		  int   y,
		  int   width,
		  int   height)
{
	int   gridX, gridY, i = 0;
	float hpad, vpad;

	model->numSprings = 0;

	hpad = ((float) width) / (GRID_WIDTH  - 1);
	vpad = ((float) height) / (GRID_HEIGHT - 1);

	for (gridY = 0; gridY < GRID_HEIGHT; gridY++)
	{
		for (gridX = 0; gridX < GRID_WIDTH; gridX++)
		{
			if (gridX > 0)
				modelAddSpring(model,
						&model->objects[i - 1],
						&model->objects[i],
						hpad, 0);

			if (gridY > 0)
				modelAddSpring(model,
						&model->objects[i - GRID_WIDTH],
						&model->objects[i],
						0, vpad);

			i++;
		}
	}
}

static Model *
createModel(int	  x,
		 int	  y,
		 int	  width,
		 int	  height)
{
	Model *model;

	model = zalloc(sizeof (Model));
	if (!model)
		return 0;

	model->numObjects = GRID_WIDTH * GRID_HEIGHT;
	model->objects = zalloc(sizeof (Object) * model->numObjects);
	if (!model->objects)
	{
		free(model);
		return 0;
	}

	model->anchorObject = 0;
	model->numSprings = 0;

	model->steps = 0;

	modelInitObjects(model, x, y, width, height);
	modelInitSprings(model, x, y, width, height);

	modelCalcBounds(model);

	return model;
}

static void
objectApplyForce(Object *object,
		  float  fx,
		  float  fy)
{
	object->force.x += fx;
	object->force.y += fy;
}

static void
springExertForces(Spring *spring,
		   float  k)
{
	Vector da, db;
	Vector a, b;

	a = spring->a->position;
	b = spring->b->position;

	da.x = 0.5f * (b.x - a.x - spring->offset.x);
	da.y = 0.5f * (b.y - a.y - spring->offset.y);

	db.x = 0.5f * (a.x - b.x + spring->offset.x);
	db.y = 0.5f * (a.y - b.y + spring->offset.y);

	objectApplyForce(spring->a, k * da.x, k * da.y);
	objectApplyForce(spring->b, k * db.x, k * db.y);
}

static float
modelStepObject(Model		*model,
		 Object		*object,
		 float		friction,
		 float		*force)
{
	object->theta += 0.05f;

	if (object->immobile)
	{
		object->velocity.x = 0.0f;
		object->velocity.y = 0.0f;

		object->force.x = 0.0f;
		object->force.y = 0.0f;

		*force = 0.0f;

		return 0.0f;
	} else {
		object->force.x -= friction * object->velocity.x;
		object->force.y -= friction * object->velocity.y;

		object->velocity.x += object->force.x / MASS;
		object->velocity.y += object->force.y / MASS;

		object->position.x += object->velocity.x;
		object->position.y += object->velocity.y;

		*force = fabs(object->force.x) + fabs(object->force.y);

		object->force.x = 0.0f;
		object->force.y = 0.0f;

		return fabs(object->velocity.x) + fabs(object->velocity.y);
	}
}

static int
modelStep(Model	  *model,
	   float	  friction,
	   float	  k,
	   float	  time)
{
	int   i, j, steps, wobbly = 0;
	float velocitySum = 0.0f;
	float force, forceSum = 0.0f;

	model->steps += time / 15.0f;
	steps = floor(model->steps);
	model->steps -= steps;

	if (!steps)
		return 1;

	for (j = 0; j < steps; j++)
	{
		for (i = 0; i < model->numSprings; i++)
			springExertForces(&model->springs[i], k);

		for (i = 0; i < model->numObjects; i++)
		{
			velocitySum += modelStepObject(model,
							&model->objects[i],
							friction,
							&force);
			forceSum += force;
		}
	}

	modelCalcBounds(model);

	if (velocitySum > 0.3f)
		wobbly |= WobblyVelocity;

	if (forceSum > 15.0f)
		wobbly |= WobblyForce;

	return wobbly;
}

static void
bezierPatchEvaluate(Model *model,
			 float u,
			 float v,
			 float *patchX,
			 float *patchY)
{
	float coeffsU[4], coeffsV[4];
	float x, y;
	int   i, j;

	coeffsU[0] = (1 - u) * (1 - u) * (1 - u);
	coeffsU[1] = 3 * u * (1 - u) * (1 - u);
	coeffsU[2] = 3 * u * u * (1 - u);
	coeffsU[3] = u * u * u;

	coeffsV[0] = (1 - v) * (1 - v) * (1 - v);
	coeffsV[1] = 3 * v * (1 - v) * (1 - v);
	coeffsV[2] = 3 * v * v * (1 - v);
	coeffsV[3] = v * v * v;

	x = y = 0.0f;

	for (i = 0; i < 4; i++)
	{
		for (j = 0; j < 4; j++)
		{
			x += coeffsU[i] * coeffsV[j] *
			model->objects[j * GRID_WIDTH + i].position.x;
			y += coeffsU[i] * coeffsV[j] *
			model->objects[j * GRID_HEIGHT + i].position.y;
		}
	}

	*patchX = x;
	*patchY = y;
}

static int
wobblyEnsureModel(struct surface *surface)
{
	WobblyWindow *ww = surface->ww;

	if (!ww->model)
	{
		ww->model = createModel(surface->x, surface->y, surface->width, surface->height);
		if (!ww->model)
			return 0;
	}

	return 1;
}

static float
objectDistance(Object *object,
		float  x,
		float  y)
{
	float dx, dy;

	dx = object->position.x - x;
	dy = object->position.y - y;

	return sqrt(dx * dx + dy * dy);
}

static Object *
modelFindNearestObject(Model *model,
			float x,
			float y)
{
	Object *object = &model->objects[0];
	float  distance, minDistance = 0.0;
	int	i;

	for (i = 0; i < model->numObjects; i++)
	{
		distance = objectDistance(&model->objects[i], x, y);
		if (i == 0 || distance < minDistance)
		{
			minDistance = distance;
			object = &model->objects[i];
		}
	}

	return object;
}

static void
wobbly_prepare_paint(struct weston_view *view, int msSinceLastPaint, int *needs_paint)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;
	WobblyWindow *ww;
	float  friction, springK;

	*needs_paint = 1;

	if (!(ws = get_wobbly_surface(surface)))
		return;

	ww = ws->ww;

	friction = WOBBLY_FRICTION;
	springK  = WOBBLY_SPRING_K;

	if (ww->wobbly)
	{
		if (ww->wobbly & (WobblyInitial | WobblyVelocity | WobblyForce))
		{
			ww->wobbly = modelStep(ww->model, friction, springK,
						(ww->wobbly & WobblyVelocity) ?
						msSinceLastPaint : 16);

			if (ww->wobbly) {
				modelCalcBounds(ww->model);
				*needs_paint = 0;
			} else {
				ws->x = ww->model->topLeft.x;
				ws->y = ww->model->topLeft.y;
				weston_view_set_position(view, ws->x, ws->y);
				wl_list_remove(&ws->transform.link);
				wl_list_init(&ws->transform.link);
				ws->synced = 1;

				weston_compositor_damage_all(view->surface->compositor);
				weston_view_geometry_dirty(view);
				weston_view_update_transform(view);
				weston_view_schedule_repaint(view);
			}
		}
	}
}

static void
wobbly_add_geometry(struct weston_view *view)
{
	struct weston_surface *surface = view->surface;
	struct gl_renderer *gr = get_renderer(surface->compositor);
	struct surface *ws;
	WobblyWindow *ww;
	float	width, height;
	float	deformedX, deformedY;
	int	x, y, iw, ih;
	float	cell_w, cell_h;
	GLfloat *v;

	if (!(ws = get_wobbly_surface(surface)))
		return;

	ww = ws->ww;

	if (ww->wobbly)
	{
		width  = ws->width;
		height = ws->height;

		cell_w = width / ws->x_cells;
		cell_h = height / ws->y_cells;

		iw = ws->x_cells + 1;
		ih = ws->y_cells + 1;

		v = wl_array_add(&gr->vertices, sizeof (GLfloat) * 4 * iw * ih);

		for (y = 0; y < ih; y++)
		{
			for (x = 0; x < iw; x++)
			{
				bezierPatchEvaluate(ww->model,
							(x * cell_w) / width,
							(y * cell_h) / height,
							&deformedX,
							&deformedY);

				*v++ = deformedX;
				*v++ = deformedY;

				*v++ = (x * cell_w) / width;
				*v++ = (y * cell_h) / height;
			}
		}
	}
}

static void
wobbly_paint_view(struct weston_view *view)
{
	struct gl_renderer *gr = get_renderer(view->surface->compositor);
	GLfloat *v;
	GLushort *indices;
	int i, x, y, x_pts;
	struct surface *ws;

	if (!(ws = get_wobbly_surface(view->surface)))
		return;

	x_pts = ws->x_cells + 1;

	v = gr->vertices.data;

	if (!v)
		return;

	indices = malloc(sizeof (GLushort) * ws->x_cells * ws->y_cells * 6);
	if (!indices)
		return;

	for (y = 0, i = 0; y < ws->y_cells; y++)
		for (x = 0; x < ws->x_cells; x++) {
			*(indices + i++) = y * x_pts + x;
			*(indices + i++) = y * x_pts + x + 1;
			*(indices + i++) = (y + 1) * x_pts + x;

			*(indices + i++) = y * x_pts + x + 1;
			*(indices + i++) = (y + 1) * x_pts + x + 1;
			*(indices + i++) = (y + 1) * x_pts + x;
		}

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, v);
	glEnableVertexAttribArray(0);

	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, v + 2);
	glEnableVertexAttribArray(1);

	glDrawElements(GL_TRIANGLES, 128 * 3, GL_UNSIGNED_SHORT, indices);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	gr->vertices.size = 0;

	free(indices);
}

static void
wobbly_done_paint(struct weston_view *view)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;
	WobblyWindow *ww;

	if (!(ws = get_wobbly_surface(surface)))
		return;

	ww = ws->ww;

	if (ww->wobbly)
	{
		ws->x = ww->model->topLeft.x;
		ws->y = ww->model->topLeft.y;
		weston_view_set_position(view, ws->x, ws->y);
	}

	weston_compositor_damage_all(view->surface->compositor);
	weston_view_geometry_dirty(view);
	weston_view_schedule_repaint(view);
}

static void
wobbly_resize_notify(struct weston_view *view)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;
	WobblyWindow *ww;
	int x, y, w, h;

	if (!(ws = get_surface(surface)))
		return;

	ww = ws->ww;

	x = ws->x = view->geometry.x;
	y = ws->y = view->geometry.y;
	w = ws->width = surface->width;
	h = ws->height = surface->height;

	if (ww->model)
	{
		if (!ww->wobbly)
			modelInitObjects(ww->model, x, y, w, h);

		modelInitSprings(ww->model, x, y, w, h);
	}
}

static void
wobbly_move_notify(struct weston_view *view, int x, int y)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;
	WobblyWindow *ww;
	int dx, dy;

	if (!(ws = get_wobbly_surface(surface)))
		return;

	ww = ws->ww;

	dx = x - ws->last_x;
	dy = y - ws->last_y;

	if (ww->grabbed) {
		ww->model->anchorObject->position.x += dx;
		ww->model->anchorObject->position.y += dy;

		ww->wobbly |= WobblyInitial;

		if (wl_list_empty(&ws->transform.link))
			wl_list_insert(&view->geometry.transformation_list, &ws->transform.link);
	}

	ws->last_x = x;
	ws->last_y = y;
}

static void
wobbly_grab_notify(struct weston_view *view, int x, int y)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;
	WobblyWindow *ww;

	if (!(ws = get_surface(surface)))
		return;

	ww = ws->ww;

	ws->last_x = x;
	ws->last_y = y;

	if (wobblyEnsureModel(ws))
	{
		Spring *s;
		int	   i;

		if (ww->model->anchorObject)
			ww->model->anchorObject->immobile = 0;

		ww->model->anchorObject = modelFindNearestObject(ww->model, x, y);
		ww->model->anchorObject->immobile = 1;

		ww->grabbed = 1;
		ws->synced = 0;

		for (i = 0; i < ww->model->numSprings; i++)
		{
			s = &ww->model->springs[i];

			if (s->a == ww->model->anchorObject)
			{
				s->b->velocity.x -= s->offset.x * 0.05f;
				s->b->velocity.y -= s->offset.y * 0.05f;
			}
			else if (s->b == ww->model->anchorObject)
			{
				s->a->velocity.x += s->offset.x * 0.05f;
				s->a->velocity.y += s->offset.y * 0.05f;
			}
		}

		ww->wobbly |= WobblyInitial;

		if (wl_list_empty(&ws->transform.link))
			wl_list_insert(&view->geometry.transformation_list, &ws->transform.link);
	}
}

static void
wobbly_ungrab_notify(struct weston_view *view)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;
	WobblyWindow *ww;

	if (!(ws = get_wobbly_surface(surface)))
		return;

	ww = ws->ww;

	if (ww->grabbed)
	{
		if (ww->model)
		{
			if (ww->model->anchorObject)
				ww->model->anchorObject->immobile = 0;

			ww->model->anchorObject = NULL;

			ww->wobbly |= WobblyInitial;
		}

		ww->grabbed = 0;
	}
}

static void
wobbly_init(struct weston_view *view)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;
	WobblyWindow *ww;

	if (get_wobbly_surface(surface))
		return;

	ww = zalloc(sizeof (WobblyWindow));
	if (!ww)
		return;

	ws = zalloc(sizeof *ws);
	if (!ws)
		return;

	ww->model   = 0;
	ww->wobbly  = 0;
	ww->grabbed = 0;
	ww->state   = 0;

	ws->ww = ww;
	ws->surface = surface;

	ws->width = surface->width;
	ws->height = surface->height;
	ws->x = view->geometry.x;
	ws->y = view->geometry.y;
	ws->synced = 1;
	ws->x_cells = 8;
	ws->y_cells = 8;
	wl_list_init(&ws->transform.link);
	weston_matrix_init(&ws->transform.matrix);

	if(!wobblyEnsureModel(ws)) {
		 free(ww);
		 free(ws);
		 return;
	}

	wl_list_insert(&wobbly.surface_list, &ws->link);
}

static void
wobbly_fini(struct weston_view *view)
{
	struct weston_surface *surface = view->surface;
	struct surface *ws;

	if (!(ws = get_wobbly_surface(surface)))
		return;

	WobblyWindow *ww = ws->ww;

	if (ww->model)
	{
		free(ww->model->objects);
		free(ww->model);
	}

	wl_list_remove(&ws->link);

	free(ww);
	free(ws);
}

static int
init(struct weston_compositor *compositor)
{
	wl_list_init(&wobbly.surface_list);

	return 0;
}

static void
fini(struct weston_compositor *compositor)
{
	struct surface *ws, *next;

	wl_list_for_each_safe(ws, next, &wobbly.surface_list, link)
		free(ws);
}

WL_EXPORT struct weston_plugin_interface plugin_interface = {
	.init = init,
	.fini = fini,
	.view_init = wobbly_init,
	.view_fini = wobbly_fini,
	.prepare_paint = wobbly_prepare_paint,
	.add_geometry = wobbly_add_geometry,
	.paint_view = wobbly_paint_view,
	.done_paint = wobbly_done_paint,
	.resize_notify = wobbly_resize_notify,
	.move_notify = wobbly_move_notify,
	.grab_notify = wobbly_grab_notify,
	.ungrab_notify = wobbly_ungrab_notify,
};
