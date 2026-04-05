/* Generated from kde-server-decoration protocol */

#ifndef SERVER_DECORATION_SERVER_PROTOCOL_H
#define SERVER_DECORATION_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

struct org_kde_kwin_server_decoration_manager;
struct org_kde_kwin_server_decoration;

#ifndef ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_ENUM
#define ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_ENUM
enum org_kde_kwin_server_decoration_manager_mode {
    ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_NONE = 0,
    ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT = 1,
    ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER = 2,
};
#endif

extern const struct wl_interface org_kde_kwin_server_decoration_manager_interface;
extern const struct wl_interface org_kde_kwin_server_decoration_interface;

struct org_kde_kwin_server_decoration_manager_interface {
    void (*create)(struct wl_client *client,
                   struct wl_resource *resource,
                   uint32_t id,
                   struct wl_resource *surface);
};

#define ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_DEFAULT_MODE 0

static inline void
org_kde_kwin_server_decoration_manager_send_default_mode(
        struct wl_resource *resource_, uint32_t mode)
{
    wl_resource_post_event(resource_,
            ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_DEFAULT_MODE, mode);
}

struct org_kde_kwin_server_decoration_interface {
    void (*release)(struct wl_client *client,
                    struct wl_resource *resource);
    void (*request_mode)(struct wl_client *client,
                         struct wl_resource *resource,
                         uint32_t mode);
};

#define ORG_KDE_KWIN_SERVER_DECORATION_MODE 0

static inline void
org_kde_kwin_server_decoration_send_mode(struct wl_resource *resource_,
                                          uint32_t mode)
{
    wl_resource_post_event(resource_,
            ORG_KDE_KWIN_SERVER_DECORATION_MODE, mode);
}

#ifdef  __cplusplus
}
#endif

#endif
