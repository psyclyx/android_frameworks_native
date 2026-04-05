/* Generated from kde-server-decoration protocol */

#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface org_kde_kwin_server_decoration_interface;

static const struct wl_interface *decoration_types[] = {
    &org_kde_kwin_server_decoration_interface,
    NULL, /* wl_surface — left NULL, libwayland resolves by name */
};

/* org_kde_kwin_server_decoration_manager::create(id<decoration>, surface<wl_surface>) */
static const struct wl_message manager_requests[] = {
    { "create", "no", decoration_types },
};

/* org_kde_kwin_server_decoration_manager::default_mode(mode) */
static const struct wl_message manager_events[] = {
    { "default_mode", "u", NULL },
};

const struct wl_interface org_kde_kwin_server_decoration_manager_interface = {
    "org_kde_kwin_server_decoration_manager", 1,
    1, manager_requests,
    1, manager_events,
};

/* org_kde_kwin_server_decoration::release() */
/* org_kde_kwin_server_decoration::request_mode(mode) */
static const struct wl_message decoration_requests[] = {
    { "release", "", NULL },
    { "request_mode", "u", NULL },
};

/* org_kde_kwin_server_decoration::mode(mode) */
static const struct wl_message decoration_events[] = {
    { "mode", "u", NULL },
};

const struct wl_interface org_kde_kwin_server_decoration_interface = {
    "org_kde_kwin_server_decoration", 1,
    2, decoration_requests,
    1, decoration_events,
};
