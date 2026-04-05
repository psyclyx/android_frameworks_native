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

#pragma once

#include <wayland-server-core.h>
#include <xdg-shell-server-protocol.h>

#include <cstdint>
#include <string>

namespace android {

class WaylandCompositor;
struct WaylandSurface;

// Per-positioner state for popup placement.
struct WaylandXdgPositioner {
    int32_t width = 0, height = 0;           // set_size
    int32_t anchorX = 0, anchorY = 0;        // set_anchor_rect
    int32_t anchorWidth = 0, anchorHeight = 0;
    uint32_t anchor = 0;                      // set_anchor (edge/corner)
    uint32_t gravity = 0;                     // set_gravity
    int32_t offsetX = 0, offsetY = 0;        // set_offset
};

// Per-xdg_popup state.
struct WaylandXdgPopup {
    WaylandCompositor* compositor = nullptr;
    struct wl_resource* resource = nullptr;   // xdg_popup resource
    struct wl_resource* parentSurface = nullptr; // parent wl_surface
    WaylandSurface* ownerSurface = nullptr;   // back-pointer for cleanup
    WaylandXdgPositioner positioner;
    int32_t x = 0, y = 0; // computed position relative to parent
};

// Per-xdg_surface state: tracks the wl_surface resource and pending configure serial.
struct WaylandXdgSurface {
    WaylandCompositor* compositor = nullptr;
    struct wl_resource* resource = nullptr;     // xdg_surface resource
    struct wl_resource* wlSurface = nullptr;    // underlying wl_surface
    struct wl_resource* toplevel = nullptr;      // xdg_toplevel (if role assigned)
    struct wl_resource* popup = nullptr;         // xdg_popup (if role assigned)
    struct wl_resource* parentToplevel = nullptr; // set_parent target (for dialogs)
    uint32_t pendingConfigureSerial = 0;
    std::string title;
    std::string appId;
    bool mapped = false; // true after first commit triggers window creation
    // Window geometry (surface-local coords, excludes shadows/CSD borders)
    int32_t geomX = 0, geomY = 0, geomWidth = 0, geomHeight = 0;
};

// Manages xdg_wm_base global, xdg_surface, and xdg_toplevel.
class WaylandXdgShell {
public:
    static struct wl_global* createGlobal(struct wl_display* display,
                                           WaylandCompositor* compositor);

private:
    // xdg_wm_base
    static void bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id);
    static void wmBaseDestroy(struct wl_client* client, struct wl_resource* resource);
    static void wmBaseCreatePositioner(struct wl_client* client, struct wl_resource* resource,
                                        uint32_t id);
    static void wmBaseGetXdgSurface(struct wl_client* client, struct wl_resource* resource,
                                     uint32_t id, struct wl_resource* surface);
    static void wmBasePong(struct wl_client* client, struct wl_resource* resource,
                            uint32_t serial);

    static const struct xdg_wm_base_interface kWmBaseImpl;

    // xdg_positioner (stub)
    static void positionerDestroy(struct wl_client* client, struct wl_resource* resource);
    static const struct xdg_positioner_interface kPositionerImpl;

    // xdg_surface
    static void xdgSurfaceDestroy(struct wl_client* client, struct wl_resource* resource);
    static void xdgSurfaceGetToplevel(struct wl_client* client, struct wl_resource* resource,
                                       uint32_t id);
    static void xdgSurfaceGetPopup(struct wl_client* client, struct wl_resource* resource,
                                    uint32_t id, struct wl_resource* parent,
                                    struct wl_resource* positioner);
    static void xdgSurfaceSetWindowGeometry(struct wl_client* client,
                                             struct wl_resource* resource,
                                             int32_t x, int32_t y, int32_t width, int32_t height);
    static void xdgSurfaceAckConfigure(struct wl_client* client, struct wl_resource* resource,
                                        uint32_t serial);
    static void onXdgSurfaceDestroy(struct wl_resource* resource);
    static void onToplevelDestroy(struct wl_resource* resource);

    static const struct xdg_surface_interface kXdgSurfaceImpl;

    // xdg_toplevel
    static void toplevelDestroy(struct wl_client* client, struct wl_resource* resource);
    static void toplevelSetParent(struct wl_client* client, struct wl_resource* resource,
                                   struct wl_resource* parent);
    static void toplevelSetTitle(struct wl_client* client, struct wl_resource* resource,
                                  const char* title);
    static void toplevelSetAppId(struct wl_client* client, struct wl_resource* resource,
                                  const char* appId);
    static void toplevelShowWindowMenu(struct wl_client* client, struct wl_resource* resource,
                                        struct wl_resource* seat, uint32_t serial,
                                        int32_t x, int32_t y);
    static void toplevelMove(struct wl_client* client, struct wl_resource* resource,
                              struct wl_resource* seat, uint32_t serial);
    static void toplevelResize(struct wl_client* client, struct wl_resource* resource,
                                struct wl_resource* seat, uint32_t serial, uint32_t edges);
    static void toplevelSetMaxSize(struct wl_client* client, struct wl_resource* resource,
                                    int32_t width, int32_t height);
    static void toplevelSetMinSize(struct wl_client* client, struct wl_resource* resource,
                                    int32_t width, int32_t height);
    static void toplevelSetMaximized(struct wl_client* client, struct wl_resource* resource);
    static void toplevelUnsetMaximized(struct wl_client* client, struct wl_resource* resource);
    static void toplevelSetFullscreen(struct wl_client* client, struct wl_resource* resource,
                                       struct wl_resource* output);
    static void toplevelUnsetFullscreen(struct wl_client* client, struct wl_resource* resource);
    static void toplevelSetMinimized(struct wl_client* client, struct wl_resource* resource);

    static const struct xdg_toplevel_interface kToplevelImpl;

    static void sendToplevelConfigure(struct wl_resource* toplevel, struct wl_resource* xdgSurface);

    // xdg_popup
    static void popupDestroy(struct wl_client* client, struct wl_resource* resource);
    static void popupGrab(struct wl_client* client, struct wl_resource* resource,
                           struct wl_resource* seat, uint32_t serial);
    static void popupReposition(struct wl_client* client, struct wl_resource* resource,
                                 struct wl_resource* positioner, uint32_t token);
    static void onPopupDestroy(struct wl_resource* resource);

    static const struct xdg_popup_interface kPopupImpl;

    // Compute popup position from positioner + anchor geometry.
    static void computePopupPosition(const WaylandXdgPositioner& pos, int32_t* outX, int32_t* outY);
};

} // namespace android
