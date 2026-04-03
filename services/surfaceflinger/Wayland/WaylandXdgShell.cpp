/*
 * Copyright (C) 2025 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "WaylandXdgShell"

#include "WaylandXdgShell.h"
#include "WaylandCompositor.h"
#include "WaylandSurface.h"

#include <algorithm>

#include <log/log.h>
#include <xdg-shell-server-protocol.h>

namespace android {

namespace {

constexpr uint32_t kXdgWmBaseVersion = 2;

} // anonymous namespace

// --- xdg_wm_base ---

const struct xdg_wm_base_interface WaylandXdgShell::kWmBaseImpl = {
        .destroy = wmBaseDestroy,
        .create_positioner = wmBaseCreatePositioner,
        .get_xdg_surface = wmBaseGetXdgSurface,
        .pong = wmBasePong,
};

struct wl_global* WaylandXdgShell::createGlobal(struct wl_display* display,
                                                  WaylandCompositor* compositor) {
    return wl_global_create(display, &xdg_wm_base_interface, kXdgWmBaseVersion,
                            compositor, bind);
}

void WaylandXdgShell::bind(struct wl_client* client, void* data,
                            uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kXdgWmBaseVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &xdg_wm_base_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kWmBaseImpl, data, nullptr);
    ALOGI("xdg_wm_base bound (v%u)", ver);
}

void WaylandXdgShell::wmBaseDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandXdgShell::wmBaseCreatePositioner(struct wl_client* client,
                                              struct wl_resource* resource, uint32_t id) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* positioner =
            wl_resource_create(client, &xdg_positioner_interface, ver, id);
    if (!positioner) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(positioner, &kPositionerImpl, nullptr, nullptr);
}

void WaylandXdgShell::wmBaseGetXdgSurface(struct wl_client* client,
                                            struct wl_resource* resource,
                                            uint32_t id, struct wl_resource* surface) {
    auto* compositor = static_cast<WaylandCompositor*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);

    struct wl_resource* xdgSurfaceResource =
            wl_resource_create(client, &xdg_surface_interface, ver, id);
    if (!xdgSurfaceResource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* xdgSurface = new WaylandXdgSurface();
    xdgSurface->compositor = compositor;
    xdgSurface->resource = xdgSurfaceResource;
    xdgSurface->wlSurface = surface;

    wl_resource_set_implementation(xdgSurfaceResource, &kXdgSurfaceImpl,
                                   xdgSurface, onXdgSurfaceDestroy);

    // Store xdg_surface reference on the WaylandSurface for configure events.
    WaylandSurface* ws = compositor->findSurface(surface);
    if (ws) {
        ws->xdgSurface = xdgSurfaceResource;
    }

    ALOGI("xdg_surface created for wl_surface %p", surface);
}

void WaylandXdgShell::wmBasePong(struct wl_client* /*client*/, struct wl_resource* /*resource*/,
                                  uint32_t /*serial*/) {
    // Accept pong silently.
}

// --- xdg_positioner (stub — only needed for popups) ---

const struct xdg_positioner_interface WaylandXdgShell::kPositionerImpl = {
        .destroy = positionerDestroy,
        .set_size = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
        .set_anchor_rect = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t,
                              int32_t) {},
        .set_anchor = [](struct wl_client*, struct wl_resource*, uint32_t) {},
        .set_gravity = [](struct wl_client*, struct wl_resource*, uint32_t) {},
        .set_constraint_adjustment = [](struct wl_client*, struct wl_resource*, uint32_t) {},
        .set_offset = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
};

void WaylandXdgShell::positionerDestroy(struct wl_client* /*client*/,
                                         struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

// --- xdg_surface ---

const struct xdg_surface_interface WaylandXdgShell::kXdgSurfaceImpl = {
        .destroy = xdgSurfaceDestroy,
        .get_toplevel = xdgSurfaceGetToplevel,
        .get_popup = xdgSurfaceGetPopup,
        .set_window_geometry = xdgSurfaceSetWindowGeometry,
        .ack_configure = xdgSurfaceAckConfigure,
};

void WaylandXdgShell::xdgSurfaceDestroy(struct wl_client* /*client*/,
                                          struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandXdgShell::xdgSurfaceGetToplevel(struct wl_client* client,
                                              struct wl_resource* resource, uint32_t id) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);

    struct wl_resource* toplevel =
            wl_resource_create(client, &xdg_toplevel_interface, ver, id);
    if (!toplevel) {
        wl_resource_post_no_memory(resource);
        return;
    }

    xdgSurface->toplevel = toplevel;
    wl_resource_set_implementation(toplevel, &kToplevelImpl, xdgSurface, nullptr);

    // Store toplevel reference on the WaylandSurface.
    WaylandSurface* ws2 = xdgSurface->compositor->findSurface(xdgSurface->wlSurface);
    if (ws2) {
        ws2->xdgToplevel = toplevel;
    }

    // Send initial configure sequence: toplevel.configure → xdg_surface.configure
    sendToplevelConfigure(toplevel, resource);

    // Request Android to create a window (Activity) for this toplevel.
    // The window will reparent the Wayland layer under itself once ready.
    WaylandSurface* ws = xdgSurface->compositor->findSurface(xdgSurface->wlSurface);
    if (ws) {
        xdgSurface->compositor->requestCreateWindow(
                static_cast<int>(ws->layerId), ws->handle,
                xdgSurface->title.empty() ? nullptr : xdgSurface->title.c_str(),
                xdgSurface->appId.empty() ? nullptr : xdgSurface->appId.c_str(),
                0, 0); // width/height 0 = client chooses
    }

    ALOGI("xdg_toplevel created for xdg_surface %p", resource);
}

void WaylandXdgShell::xdgSurfaceGetPopup(struct wl_client* /*client*/,
                                           struct wl_resource* resource, uint32_t /*id*/,
                                           struct wl_resource* /*parent*/,
                                           struct wl_resource* /*positioner*/) {
    wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                           "popups not supported");
}

void WaylandXdgShell::xdgSurfaceSetWindowGeometry(struct wl_client* /*client*/,
                                                    struct wl_resource* /*resource*/,
                                                    int32_t /*x*/, int32_t /*y*/,
                                                    int32_t /*width*/, int32_t /*height*/) {
    // Ignored for MVP.
}

void WaylandXdgShell::xdgSurfaceAckConfigure(struct wl_client* /*client*/,
                                               struct wl_resource* resource, uint32_t serial) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    // Accept ack; MVP does not gate commits on configure state.
    (void)serial;
    (void)xdgSurface;
}

void WaylandXdgShell::onXdgSurfaceDestroy(struct wl_resource* resource) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    delete xdgSurface;
}

// --- xdg_toplevel ---

const struct xdg_toplevel_interface WaylandXdgShell::kToplevelImpl = {
        .destroy = toplevelDestroy,
        .set_parent = toplevelSetParent,
        .set_title = toplevelSetTitle,
        .set_app_id = toplevelSetAppId,
        .show_window_menu = toplevelShowWindowMenu,
        .move = toplevelMove,
        .resize = toplevelResize,
        .set_max_size = toplevelSetMaxSize,
        .set_min_size = toplevelSetMinSize,
        .set_maximized = toplevelSetMaximized,
        .unset_maximized = toplevelUnsetMaximized,
        .set_fullscreen = toplevelSetFullscreen,
        .unset_fullscreen = toplevelUnsetFullscreen,
        .set_minimized = toplevelSetMinimized,
};

void WaylandXdgShell::toplevelDestroy(struct wl_client* /*client*/,
                                       struct wl_resource* resource) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (xdgSurface) {
        // Tell Android to close the window
        WaylandSurface* ws = xdgSurface->compositor->findSurface(xdgSurface->wlSurface);
        if (ws) {
            xdgSurface->compositor->requestDestroyWindow(static_cast<int>(ws->layerId));
        }
        xdgSurface->toplevel = nullptr;
    }
    wl_resource_destroy(resource);
}

void WaylandXdgShell::toplevelSetParent(struct wl_client* /*client*/,
                                         struct wl_resource* /*resource*/,
                                         struct wl_resource* /*parent*/) {}

void WaylandXdgShell::toplevelSetTitle(struct wl_client* /*client*/,
                                        struct wl_resource* resource, const char* title) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (xdgSurface && title) {
        xdgSurface->title = title;
    }
    ALOGD("xdg_toplevel.set_title: %s", title ? title : "(null)");
}

void WaylandXdgShell::toplevelSetAppId(struct wl_client* /*client*/,
                                        struct wl_resource* resource, const char* appId) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (xdgSurface && appId) {
        xdgSurface->appId = appId;
    }
    ALOGD("xdg_toplevel.set_app_id: %s", appId ? appId : "(null)");
}

void WaylandXdgShell::toplevelShowWindowMenu(struct wl_client* /*client*/,
                                              struct wl_resource* /*resource*/,
                                              struct wl_resource* /*seat*/, uint32_t /*serial*/,
                                              int32_t /*x*/, int32_t /*y*/) {}

void WaylandXdgShell::toplevelMove(struct wl_client* /*client*/, struct wl_resource* /*resource*/,
                                    struct wl_resource* /*seat*/, uint32_t /*serial*/) {}

void WaylandXdgShell::toplevelResize(struct wl_client* /*client*/,
                                      struct wl_resource* /*resource*/,
                                      struct wl_resource* /*seat*/, uint32_t /*serial*/,
                                      uint32_t /*edges*/) {}

void WaylandXdgShell::toplevelSetMaxSize(struct wl_client* /*client*/,
                                          struct wl_resource* /*resource*/,
                                          int32_t /*width*/, int32_t /*height*/) {}

void WaylandXdgShell::toplevelSetMinSize(struct wl_client* /*client*/,
                                          struct wl_resource* /*resource*/,
                                          int32_t /*width*/, int32_t /*height*/) {}

void WaylandXdgShell::toplevelSetMaximized(struct wl_client* /*client*/,
                                            struct wl_resource* /*resource*/) {}

void WaylandXdgShell::toplevelUnsetMaximized(struct wl_client* /*client*/,
                                              struct wl_resource* /*resource*/) {}

void WaylandXdgShell::toplevelSetFullscreen(struct wl_client* /*client*/,
                                             struct wl_resource* /*resource*/,
                                             struct wl_resource* /*output*/) {}

void WaylandXdgShell::toplevelUnsetFullscreen(struct wl_client* /*client*/,
                                               struct wl_resource* /*resource*/) {}

void WaylandXdgShell::toplevelSetMinimized(struct wl_client* /*client*/,
                                            struct wl_resource* /*resource*/) {}

void WaylandXdgShell::sendToplevelConfigure(struct wl_resource* toplevel,
                                             struct wl_resource* xdgSurface) {
    auto* surface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(xdgSurface));

    // Send xdg_toplevel.configure with the display size so the client
    // fills the screen. Use ACTIVATED state so client knows it has focus.
    struct wl_array states;
    wl_array_init(&states);
    uint32_t* activated = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *activated = XDG_TOPLEVEL_STATE_ACTIVATED;
    // Use display dimensions as default. The actual window size will be
    // sent via a follow-up configure when the Activity is ready.
    xdg_toplevel_send_configure(toplevel, 0, 0, &states);
    wl_array_release(&states);

    // Send xdg_surface.configure with a serial the client must ack.
    surface->pendingConfigureSerial++;
    xdg_surface_send_configure(xdgSurface, surface->pendingConfigureSerial);

    ALOGD("Sent initial configure (serial %u) for xdg_surface %p",
          surface->pendingConfigureSerial, xdgSurface);
}

} // namespace android
