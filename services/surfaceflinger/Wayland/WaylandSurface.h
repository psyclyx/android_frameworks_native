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

#include <binder/IBinder.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <ui/GraphicBuffer.h>

namespace android {

class Layer;
class SurfaceFlinger;
class WaylandCompositor;
struct WaylandDmabufBuffer;
struct WaylandShmBuffer;

struct WaylandSurface {
    WaylandCompositor* compositor = nullptr;
    struct wl_resource* resource = nullptr;
    wp<Layer> layer;
    sp<IBinder> handle;
    uint32_t layerId = 0;
    uint64_t frameNumber = 0;
    uint32_t producerId = 0; // unique per-surface, set at creation

    // Current committed buffer (released when replaced or surface destroyed)
    struct wl_resource* currentBuffer = nullptr;

    // Cache of imported GraphicBuffers keyed by dmabuf fd inode.
    // Vulkan swapchains reuse the same 4 dmabuf fds, so we import once and reuse.
    std::unordered_map<uint64_t, sp<GraphicBuffer>> importedBuffers;

    // XDG role resources (set when xdg_surface/toplevel are created)
    struct wl_resource* xdgSurface = nullptr;
    struct wl_resource* xdgToplevel = nullptr;
    uint32_t configureSerial = 0;

    // Pending state (applied on commit)
    struct wl_resource* pendingBuffer = nullptr;
    std::vector<struct wl_resource*> pendingFrameCallbacks;
    bool bufferAttached = false;

    static const struct wl_surface_interface kImpl;

    static void destroy(struct wl_client* client, struct wl_resource* resource);
    static void attach(struct wl_client* client, struct wl_resource* resource,
                       struct wl_resource* buffer, int32_t x, int32_t y);
    static void damage(struct wl_client* client, struct wl_resource* resource,
                       int32_t x, int32_t y, int32_t width, int32_t height);
    static void frame(struct wl_client* client, struct wl_resource* resource,
                      uint32_t callback);
    static void setOpaqueRegion(struct wl_client* client, struct wl_resource* resource,
                                struct wl_resource* region);
    static void setInputRegion(struct wl_client* client, struct wl_resource* resource,
                               struct wl_resource* region);
    static void commit(struct wl_client* client, struct wl_resource* resource);
    static void setBufferTransform(struct wl_client* client, struct wl_resource* resource,
                                   int32_t transform);
    static void setBufferScale(struct wl_client* client, struct wl_resource* resource,
                               int32_t scale);
    static void damageBuffer(struct wl_client* client, struct wl_resource* resource,
                             int32_t x, int32_t y, int32_t width, int32_t height);
    static void offset(struct wl_client* client, struct wl_resource* resource,
                       int32_t x, int32_t y);

    static void onDestroy(struct wl_resource* resource);

    void importBuffer(struct WaylandDmabufBuffer* dmabuf);
    void importShmBuffer(struct WaylandShmBuffer* shm);
};

} // namespace android
