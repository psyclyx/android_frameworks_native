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
#define LOG_TAG "WaylandSubcompositor"

#include "WaylandSubcompositor.h"
#include "WaylandCompositor.h"
#include "WaylandSurface.h"

#include <algorithm>
#include <log/log.h>
#include <wayland-server-protocol.h>

namespace android {

namespace {

constexpr uint32_t kSubcompositorVersion = 1;

struct WaylandSubsurface {
    WaylandCompositor* compositor = nullptr;
    struct wl_resource* surface = nullptr;
    struct wl_resource* parent = nullptr;
};

void subsurfaceDestroy(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void subsurfaceSetPosition(struct wl_client*, struct wl_resource* resource,
                           int32_t x, int32_t y) {
    auto* sub = static_cast<WaylandSubsurface*>(wl_resource_get_user_data(resource));
    if (!sub || !sub->compositor) return;
    WaylandSurface* ws = sub->compositor->findSurface(sub->surface);
    if (ws) sub->compositor->setSurfacePosition(ws, x, y);
}

void subsurfacePlaceAbove(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void subsurfacePlaceBelow(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void subsurfaceSetSync(struct wl_client*, struct wl_resource*) {}
void subsurfaceSetDesync(struct wl_client*, struct wl_resource*) {}

void onSubsurfaceDestroy(struct wl_resource* resource) {
    delete static_cast<WaylandSubsurface*>(wl_resource_get_user_data(resource));
}

const struct wl_subsurface_interface kSubsurfaceImpl = {
    .destroy = subsurfaceDestroy,
    .set_position = subsurfaceSetPosition,
    .place_above = subsurfacePlaceAbove,
    .place_below = subsurfacePlaceBelow,
    .set_sync = subsurfaceSetSync,
    .set_desync = subsurfaceSetDesync,
};

void subcompositorDestroy(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void subcompositorGetSubsurface(struct wl_client* client, struct wl_resource* resource,
                                uint32_t id, struct wl_resource* surface,
                                struct wl_resource* parent) {
    auto* compositor = static_cast<WaylandCompositor*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);

    struct wl_resource* subsurfaceRes =
            wl_resource_create(client, &wl_subsurface_interface, ver, id);
    if (!subsurfaceRes) {
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* sub = new WaylandSubsurface();
    sub->compositor = compositor;
    sub->surface = surface;
    sub->parent = parent;

    wl_resource_set_implementation(subsurfaceRes, &kSubsurfaceImpl, sub, onSubsurfaceDestroy);

    // Reparent the subsurface's SF layer under the parent's SF layer.
    WaylandSurface* childWs = compositor->findSurface(surface);
    WaylandSurface* parentWs = compositor->findSurface(parent);
    if (childWs && parentWs) {
        compositor->reparentSurfaceUnder(childWs, parentWs);
    }
}

const struct wl_subcompositor_interface kSubcompositorImpl = {
    .destroy = subcompositorDestroy,
    .get_subsurface = subcompositorGetSubsurface,
};

void bindSubcompositor(struct wl_client* client, void* data,
                       uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kSubcompositorVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &wl_subcompositor_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kSubcompositorImpl, data, nullptr);
}

} // anonymous namespace

struct wl_global* WaylandSubcompositor::createGlobal(struct wl_display* display,
                                                      WaylandCompositor* compositor) {
    return wl_global_create(display, &wl_subcompositor_interface,
                            kSubcompositorVersion, compositor, bindSubcompositor);
}

} // namespace android
