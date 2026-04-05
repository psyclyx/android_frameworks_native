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
#define LOG_TAG "WaylandDecoration"

#include "WaylandDecoration.h"
#include "WaylandXdgShell.h"

#include <algorithm>
#include <log/log.h>
#include <xdg-decoration-unstable-v1-server-protocol.h>
#include <xdg-shell-server-protocol.h>

namespace android {

namespace {

constexpr uint32_t kManagerVersion = 1;

// --- zxdg_toplevel_decoration_v1 ---

void decorationDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void decorationSetMode(struct wl_client* /*client*/, struct wl_resource* resource,
                       uint32_t /*mode*/) {
    // Always enforce server-side decorations regardless of what the client requests.
    zxdg_toplevel_decoration_v1_send_configure(resource,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void decorationUnsetMode(struct wl_client* /*client*/, struct wl_resource* resource) {
    // Default to server-side.
    zxdg_toplevel_decoration_v1_send_configure(resource,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

const struct zxdg_toplevel_decoration_v1_interface kDecorationImpl = {
    .destroy = decorationDestroy,
    .set_mode = decorationSetMode,
    .unset_mode = decorationUnsetMode,
};

// --- zxdg_decoration_manager_v1 ---

void managerDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void managerGetToplevelDecoration(struct wl_client* client, struct wl_resource* resource,
                                   uint32_t id, struct wl_resource* toplevel) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* decoration =
            wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, ver, id);
    if (!decoration) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(decoration, &kDecorationImpl, nullptr, nullptr);

    // Send SERVER_SIDE mode followed by a new xdg_surface.configure so the
    // client sees the decoration mode as part of a configure cycle.
    zxdg_toplevel_decoration_v1_send_configure(decoration,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    // Find the xdg_surface for this toplevel and send a new configure sequence.
    auto* xdgSurface = static_cast<WaylandXdgSurface*>(wl_resource_get_user_data(toplevel));
    if (xdgSurface && xdgSurface->resource && xdgSurface->toplevel) {
        struct wl_array states;
        wl_array_init(&states);
        uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *s = XDG_TOPLEVEL_STATE_ACTIVATED;
        xdg_toplevel_send_configure(xdgSurface->toplevel, 0, 0, &states);
        wl_array_release(&states);

        xdgSurface->pendingConfigureSerial++;
        xdg_surface_send_configure(xdgSurface->resource, xdgSurface->pendingConfigureSerial);
    }

    ALOGD("Created toplevel decoration, sent SERVER_SIDE + configure");
}

const struct zxdg_decoration_manager_v1_interface kManagerImpl = {
    .destroy = managerDestroy,
    .get_toplevel_decoration = managerGetToplevelDecoration,
};

void bindManager(struct wl_client* client, void* data,
                 uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kManagerVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &zxdg_decoration_manager_v1_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kManagerImpl, data, nullptr);
    ALOGI("zxdg_decoration_manager_v1 bound (v%u)", ver);
}

} // anonymous namespace

struct wl_global* WaylandDecoration::createGlobal(struct wl_display* display,
                                                    WaylandCompositor* compositor) {
    return wl_global_create(display, &zxdg_decoration_manager_v1_interface,
                            kManagerVersion, compositor, bindManager);
}

} // namespace android
