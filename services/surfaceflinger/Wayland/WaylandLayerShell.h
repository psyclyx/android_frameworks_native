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
#include "wlr-layer-shell-unstable-v1-server-protocol.h"

#include <cstdint>
#include <string>

namespace android {

class SurfaceFlinger;
class WaylandCompositor;

// Per-layer_surface state, allocated when get_layer_surface is called.
struct WaylandLayerSurface {
    WaylandCompositor* compositor = nullptr;
    struct wl_resource* resource = nullptr;      // zwlr_layer_surface_v1
    struct wl_resource* wlSurface = nullptr;     // underlying wl_surface
    std::string ns;                               // namespace

    uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    uint32_t anchor = 0;
    int32_t exclusiveZone = 0;
    int32_t marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;
    uint32_t desiredWidth = 0, desiredHeight = 0;
    uint32_t keyboardInteractivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;

    uint32_t configureSerial = 0;
    bool configured = false;
};

class WaylandLayerShell {
public:
    static struct wl_global* createGlobal(struct wl_display* display,
                                           WaylandCompositor* compositor);

    // Compute and apply layer surface position/size after a commit.
    static void applyLayout(WaylandLayerSurface* ls);

    // Send a configure event to the layer surface.
    static void sendConfigure(WaylandLayerSurface* ls);

private:
    // zwlr_layer_shell_v1
    static void bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id);
    static void getLayerSurface(struct wl_client* client, struct wl_resource* resource,
                                 uint32_t id, struct wl_resource* surface,
                                 struct wl_resource* output, uint32_t layer,
                                 const char* ns);
    static void shellDestroy(struct wl_client* client, struct wl_resource* resource);

    static const struct zwlr_layer_shell_v1_interface kShellImpl;

    // zwlr_layer_surface_v1
    static void surfSetSize(struct wl_client* client, struct wl_resource* resource,
                            uint32_t width, uint32_t height);
    static void surfSetAnchor(struct wl_client* client, struct wl_resource* resource,
                              uint32_t anchor);
    static void surfSetExclusiveZone(struct wl_client* client, struct wl_resource* resource,
                                     int32_t zone);
    static void surfSetMargin(struct wl_client* client, struct wl_resource* resource,
                              int32_t top, int32_t right, int32_t bottom, int32_t left);
    static void surfSetKeyboardInteractivity(struct wl_client* client,
                                              struct wl_resource* resource,
                                              uint32_t interactivity);
    static void surfGetPopup(struct wl_client* client, struct wl_resource* resource,
                             struct wl_resource* popup);
    static void surfAckConfigure(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t serial);
    static void surfDestroy(struct wl_client* client, struct wl_resource* resource);
    static void surfSetLayer(struct wl_client* client, struct wl_resource* resource,
                             uint32_t layer);
    static void surfSetExclusiveEdge(struct wl_client* client, struct wl_resource* resource,
                                      uint32_t edge);

    static void onLayerSurfaceDestroy(struct wl_resource* resource);

    static void getDisplaySize(SurfaceFlinger& flinger,
                                int32_t& outWidth, int32_t& outHeight);

    static const struct zwlr_layer_surface_v1_interface kSurfaceImpl;
};

} // namespace android
