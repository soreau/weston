#include "compositor.h"

void
xwpsb_added_notify(void *data, struct weston_surface *surface);

void
xwpsb_removed_notify(void *data, struct weston_surface *surface);

void
xwpsb_committed_notify(void *data, struct weston_surface *surface);

void
xwpsb_flush_damage_notify(struct weston_surface *surface);

void
xwpsb_attach_notify(struct weston_surface *surface,
			struct weston_buffer *buffer);

void
xwpsb_position_notify(void *data, struct weston_surface *surface,
			int x, int y);

int
xwpsb_surface_move(void *data, struct weston_surface *surface);

int
xwpsb_surface_resize(void *data, struct weston_surface *surface,
			uint32_t edges);

void
xwpsb_surface_minimize(void *data, struct weston_surface *surface);

void
xwpsb_set_title(void *data, struct weston_surface *surface, const char *s);

void *
xwpsb_init(struct weston_compositor *compositor);
