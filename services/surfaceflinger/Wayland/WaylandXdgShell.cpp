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

static void onPositionerDestroy(struct wl_resource* resource);

void WaylandXdgShell::wmBaseCreatePositioner(struct wl_client* client,
                                              struct wl_resource* resource, uint32_t id) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* positioner =
            wl_resource_create(client, &xdg_positioner_interface, ver, id);
    if (!positioner) {
        wl_resource_post_no_memory(resource);
        return;
    }
    auto* pos = new WaylandXdgPositioner();
    wl_resource_set_implementation(positioner, &kPositionerImpl, pos, onPositionerDestroy);
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

// --- xdg_positioner ---

static void onPositionerDestroy(struct wl_resource* resource) {
    delete static_cast<WaylandXdgPositioner*>(wl_resource_get_user_data(resource));
}

const struct xdg_positioner_interface WaylandXdgShell::kPositionerImpl = {
        .destroy = positionerDestroy,
        .set_size = [](struct wl_client*, struct wl_resource* resource,
                       int32_t width, int32_t height) {
            auto* p = static_cast<WaylandXdgPositioner*>(wl_resource_get_user_data(resource));
            if (p) { p->width = width; p->height = height; }
        },
        .set_anchor_rect = [](struct wl_client*, struct wl_resource* resource,
                              int32_t x, int32_t y, int32_t width, int32_t height) {
            auto* p = static_cast<WaylandXdgPositioner*>(wl_resource_get_user_data(resource));
            if (p) { p->anchorX = x; p->anchorY = y; p->anchorWidth = width; p->anchorHeight = height; }
        },
        .set_anchor = [](struct wl_client*, struct wl_resource* resource, uint32_t anchor) {
            auto* p = static_cast<WaylandXdgPositioner*>(wl_resource_get_user_data(resource));
            if (p) p->anchor = anchor;
        },
        .set_gravity = [](struct wl_client*, struct wl_resource* resource, uint32_t gravity) {
            auto* p = static_cast<WaylandXdgPositioner*>(wl_resource_get_user_data(resource));
            if (p) p->gravity = gravity;
        },
        .set_constraint_adjustment = [](struct wl_client*, struct wl_resource*, uint32_t) {},
        .set_offset = [](struct wl_client*, struct wl_resource* resource,
                         int32_t x, int32_t y) {
            auto* p = static_cast<WaylandXdgPositioner*>(wl_resource_get_user_data(resource));
            if (p) { p->offsetX = x; p->offsetY = y; }
        },
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
    wl_resource_set_implementation(toplevel, &kToplevelImpl, xdgSurface, onToplevelDestroy);

    // Store toplevel reference on the WaylandSurface.
    WaylandSurface* ws = xdgSurface->compositor->findSurface(xdgSurface->wlSurface);
    if (ws) {
        ws->xdgToplevel = toplevel;
    }

    // Send initial configure sequence: toplevel.configure → xdg_surface.configure.
    // Window creation is deferred to first wl_surface.commit so we have
    // title, app_id, parent, and buffer dimensions.
    sendToplevelConfigure(toplevel, resource);

    ALOGI("xdg_toplevel created for xdg_surface %p (window creation deferred to commit)", resource);
}

void WaylandXdgShell::xdgSurfaceGetPopup(struct wl_client* client,
                                           struct wl_resource* resource, uint32_t id,
                                           struct wl_resource* parent,
                                           struct wl_resource* positioner) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);

    struct wl_resource* popupResource =
            wl_resource_create(client, &xdg_popup_interface, ver, id);
    if (!popupResource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* popup = new WaylandXdgPopup();
    popup->compositor = xdgSurface->compositor;
    popup->resource = popupResource;

    // Resolve parent: the parent arg is an xdg_surface, get the underlying wl_surface
    if (parent) {
        auto* parentXdg = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(parent));
        if (parentXdg) {
            popup->parentSurface = parentXdg->wlSurface;
        }
    }

    // Copy positioner state
    if (positioner) {
        auto* pos = static_cast<WaylandXdgPositioner*>(wl_resource_get_user_data(positioner));
        if (pos) {
            popup->positioner = *pos;
        }
    }

    // Compute position
    computePopupPosition(popup->positioner, &popup->x, &popup->y);

    xdgSurface->popup = popupResource;
    wl_resource_set_implementation(popupResource, &kPopupImpl, popup, onPopupDestroy);

    // Store popup reference on the WaylandSurface.
    // Don't reparent yet — the commit handler will create a sub-window
    // and the compositor will reparent under that.
    WaylandSurface* ws = xdgSurface->compositor->findSurface(xdgSurface->wlSurface);
    if (ws) {
        ws->xdgPopup = popupResource;
        popup->ownerSurface = ws;
    }

    // Send initial configure for the popup
    xdg_popup_send_configure(popupResource, popup->x, popup->y,
                             popup->positioner.width, popup->positioner.height);
    xdgSurface->pendingConfigureSerial++;
    xdg_surface_send_configure(resource, xdgSurface->pendingConfigureSerial);

    ALOGI("xdg_popup created at %d,%d size %dx%d",
          popup->x, popup->y, popup->positioner.width, popup->positioner.height);
}

void WaylandXdgShell::xdgSurfaceSetWindowGeometry(struct wl_client* /*client*/,
                                                    struct wl_resource* resource,
                                                    int32_t x, int32_t y,
                                                    int32_t width, int32_t height) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (xdgSurface) {
        xdgSurface->geomX = x;
        xdgSurface->geomY = y;
        xdgSurface->geomWidth = width;
        xdgSurface->geomHeight = height;
        WL_LOGV("set_window_geometry: %d,%d %dx%d", x, y, width, height);
    }
}

void WaylandXdgShell::xdgSurfaceAckConfigure(struct wl_client* /*client*/,
                                               struct wl_resource* resource, uint32_t serial) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (xdgSurface) {
        xdgSurface->ackedConfigureSerial = serial;
    }
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
        xdgSurface->toplevel = nullptr;
    }
    // wl_resource_destroy triggers onToplevelDestroy which does the actual cleanup.
    wl_resource_destroy(resource);
}

void WaylandXdgShell::onToplevelDestroy(struct wl_resource* resource) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (!xdgSurface) return;

    WaylandSurface* ws = xdgSurface->compositor->findSurface(xdgSurface->wlSurface);
    if (ws) {
        xdgSurface->compositor->requestDestroyWindow(static_cast<int>(ws->layerId));
        ws->xdgToplevel = nullptr;
    }
    xdgSurface->toplevel = nullptr;
}

void WaylandXdgShell::toplevelSetParent(struct wl_client* /*client*/,
                                         struct wl_resource* resource,
                                         struct wl_resource* parent) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (xdgSurface) {
        xdgSurface->parentToplevel = parent;
        ALOGD("xdg_toplevel.set_parent: %p -> parent %p", resource, parent);
    }
}

void WaylandXdgShell::toplevelSetTitle(struct wl_client* /*client*/,
                                        struct wl_resource* resource, const char* title) {
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(resource));
    if (xdgSurface && title) {
        xdgSurface->title = title;
        WaylandSurface* ws = xdgSurface->compositor->findSurface(xdgSurface->wlSurface);
        if (ws) {
            xdgSurface->compositor->requestSetTitle(static_cast<int>(ws->layerId), title);
        }
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

// --- xdg_popup ---

const struct xdg_popup_interface WaylandXdgShell::kPopupImpl = {
        .destroy = popupDestroy,
        .grab = popupGrab,
        .reposition = popupReposition,
};

void WaylandXdgShell::popupDestroy(struct wl_client* /*client*/,
                                    struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandXdgShell::popupGrab(struct wl_client* /*client*/, struct wl_resource* /*resource*/,
                                 struct wl_resource* /*seat*/, uint32_t /*serial*/) {
    // Accept grab silently — input routing already works via parent Activity.
}

void WaylandXdgShell::popupReposition(struct wl_client* /*client*/,
                                       struct wl_resource* /*resource*/,
                                       struct wl_resource* /*positioner*/, uint32_t /*token*/) {
    // Repositioning not yet supported.
}

void WaylandXdgShell::onPopupDestroy(struct wl_resource* resource) {
    auto* popup = static_cast<WaylandXdgPopup*>(wl_resource_get_user_data(resource));
    if (popup) {
        if (popup->ownerSurface) {
            popup->ownerSurface->xdgPopup = nullptr;
            popup->compositor->requestDestroyWindow(
                    static_cast<int>(popup->ownerSurface->layerId));
        }
        delete popup;
    }
}

void WaylandXdgShell::computePopupPosition(const WaylandXdgPositioner& pos,
                                             int32_t* outX, int32_t* outY) {
    // Compute the anchor point on the anchor rect based on anchor edge/corner.
    // xdg_positioner anchor values (from xdg-shell protocol):
    //   NONE=0, TOP=1, BOTTOM=2, LEFT=3, RIGHT=4,
    //   TOP_LEFT=5, BOTTOM_LEFT=6, TOP_RIGHT=7, BOTTOM_RIGHT=8
    int32_t ax = pos.anchorX;
    int32_t ay = pos.anchorY;

    // Default: center of anchor rect
    ax += pos.anchorWidth / 2;
    ay += pos.anchorHeight / 2;

    switch (pos.anchor) {
        case XDG_POSITIONER_ANCHOR_TOP:
            ax = pos.anchorX + pos.anchorWidth / 2;
            ay = pos.anchorY;
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM:
            ax = pos.anchorX + pos.anchorWidth / 2;
            ay = pos.anchorY + pos.anchorHeight;
            break;
        case XDG_POSITIONER_ANCHOR_LEFT:
            ax = pos.anchorX;
            ay = pos.anchorY + pos.anchorHeight / 2;
            break;
        case XDG_POSITIONER_ANCHOR_RIGHT:
            ax = pos.anchorX + pos.anchorWidth;
            ay = pos.anchorY + pos.anchorHeight / 2;
            break;
        case XDG_POSITIONER_ANCHOR_TOP_LEFT:
            ax = pos.anchorX;
            ay = pos.anchorY;
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
            ax = pos.anchorX;
            ay = pos.anchorY + pos.anchorHeight;
            break;
        case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
            ax = pos.anchorX + pos.anchorWidth;
            ay = pos.anchorY;
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
            ax = pos.anchorX + pos.anchorWidth;
            ay = pos.anchorY + pos.anchorHeight;
            break;
        default: // NONE — center
            break;
    }

    // Apply gravity to offset the popup from the anchor point.
    // Gravity values match anchor values but determine which direction
    // the popup "falls" from the anchor point.
    int32_t px = ax, py = ay;
    switch (pos.gravity) {
        case XDG_POSITIONER_GRAVITY_TOP:
            py = ay - pos.height;
            px = ax - pos.width / 2;
            break;
        case XDG_POSITIONER_GRAVITY_BOTTOM:
            px = ax - pos.width / 2;
            break;
        case XDG_POSITIONER_GRAVITY_LEFT:
            px = ax - pos.width;
            py = ay - pos.height / 2;
            break;
        case XDG_POSITIONER_GRAVITY_RIGHT:
            py = ay - pos.height / 2;
            break;
        case XDG_POSITIONER_GRAVITY_TOP_LEFT:
            px = ax - pos.width;
            py = ay - pos.height;
            break;
        case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
            px = ax - pos.width;
            break;
        case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
            py = ay - pos.height;
            break;
        case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
            // popup falls to bottom-right — default (px=ax, py=ay)
            break;
        default: // NONE — center on anchor
            px = ax - pos.width / 2;
            py = ay - pos.height / 2;
            break;
    }

    // Apply manual offset
    *outX = px + pos.offsetX;
    *outY = py + pos.offsetY;
}

} // namespace android
