#include <assert.h>
#include <stdlib.h>
#include "types/wlr_xdg_shell_v6.h"
#include "util/signal.h"

#define SHELL_VERSION 1

static const struct zxdg_shell_v6_interface xdg_shell_impl;

static struct wlr_xdg_client_v6 *xdg_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_shell_v6_interface,
		&xdg_shell_impl));
	return wl_resource_get_user_data(resource);
}

static void xdg_shell_handle_create_positioner(struct wl_client *wl_client,
		struct wl_resource *resource, uint32_t id) {
	struct wlr_xdg_client_v6 *client =
		xdg_client_from_resource(resource);
	create_xdg_positioner_v6(client, id);
}

static void xdg_shell_handle_get_xdg_surface(struct wl_client *wl_client,
		struct wl_resource *client_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_xdg_client_v6 *client =
		xdg_client_from_resource(client_resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	create_xdg_surface_v6(client, surface, id);
}

static void xdg_shell_handle_pong(struct wl_client *wl_client,
		struct wl_resource *resource, uint32_t serial) {
	struct wlr_xdg_client_v6 *client = xdg_client_from_resource(resource);

	if (client->ping_serial != serial) {
		return;
	}

	wl_event_source_timer_update(client->ping_timer, 0);
	client->ping_serial = 0;
}

static void xdg_shell_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *resource) {
	struct wlr_xdg_client_v6 *client = xdg_client_from_resource(resource);

	if (!wl_list_empty(&client->surfaces)) {
		wl_resource_post_error(client->resource,
			ZXDG_SHELL_V6_ERROR_DEFUNCT_SURFACES,
			"xdg_wm_base was destroyed before children");
		return;
	}

	wl_resource_destroy(resource);
}

static const struct zxdg_shell_v6_interface xdg_shell_impl = {
	.destroy = xdg_shell_handle_destroy,
	.create_positioner = xdg_shell_handle_create_positioner,
	.get_xdg_surface = xdg_shell_handle_get_xdg_surface,
	.pong = xdg_shell_handle_pong,
};

static void xdg_client_v6_handle_resource_destroy(
		struct wl_resource *resource) {
	struct wlr_xdg_client_v6 *client = xdg_client_from_resource(resource);

	struct wlr_xdg_surface_v6 *surface, *tmp = NULL;
	wl_list_for_each_safe(surface, tmp, &client->surfaces, link) {
		wl_resource_destroy(surface->resource);
	}

	if (client->ping_timer != NULL) {
		wl_event_source_remove(client->ping_timer);
	}

	wl_list_remove(&client->link);
	free(client);
}

static int xdg_client_v6_ping_timeout(void *user_data) {
	struct wlr_xdg_client_v6 *client = user_data;

	struct wlr_xdg_surface_v6 *surface;
	wl_list_for_each(surface, &client->surfaces, link) {
		wlr_signal_emit_safe(&surface->events.ping_timeout, surface);
	}

	client->ping_serial = 0;
	return 1;
}

static void xdg_shell_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_xdg_shell_v6 *xdg_shell = data;
	assert(wl_client && xdg_shell);

	struct wlr_xdg_client_v6 *client =
		calloc(1, sizeof(struct wlr_xdg_client_v6));
	if (client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_list_init(&client->surfaces);

	client->resource =
		wl_resource_create(wl_client, &zxdg_shell_v6_interface, version, id);
	if (client->resource == NULL) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}
	client->client = wl_client;
	client->shell = xdg_shell;

	wl_resource_set_implementation(client->resource, &xdg_shell_impl, client,
		xdg_client_v6_handle_resource_destroy);
	wl_list_insert(&xdg_shell->clients, &client->link);

	struct wl_display *display = wl_client_get_display(client->client);
	struct wl_event_loop *loop = wl_display_get_event_loop(display);
	client->ping_timer = wl_event_loop_add_timer(loop,
		xdg_client_v6_ping_timeout, client);
	if (client->ping_timer == NULL) {
		wl_client_post_no_memory(client->client);
	}
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_shell_v6 *xdg_shell =
		wl_container_of(listener, xdg_shell, display_destroy);
	wlr_xdg_shell_v6_destroy(xdg_shell);
}

struct wlr_xdg_shell_v6 *wlr_xdg_shell_v6_create(struct wl_display *display) {
	struct wlr_xdg_shell_v6 *xdg_shell =
		calloc(1, sizeof(struct wlr_xdg_shell_v6));
	if (!xdg_shell) {
		return NULL;
	}

	xdg_shell->ping_timeout = 10000;

	wl_list_init(&xdg_shell->clients);
	wl_list_init(&xdg_shell->popup_grabs);

	struct wl_global *global = wl_global_create(display,
		&zxdg_shell_v6_interface, SHELL_VERSION, xdg_shell, xdg_shell_bind);
	if (!global) {
		free(xdg_shell);
		return NULL;
	}
	xdg_shell->global = global;

	wl_signal_init(&xdg_shell->events.new_surface);

	xdg_shell->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &xdg_shell->display_destroy);

	return xdg_shell;
}

void wlr_xdg_shell_v6_destroy(struct wlr_xdg_shell_v6 *xdg_shell) {
	if (!xdg_shell) {
		return;
	}
	wl_list_remove(&xdg_shell->display_destroy.link);
	wl_global_destroy(xdg_shell->global);
	free(xdg_shell);
}
