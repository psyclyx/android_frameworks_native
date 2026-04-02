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

#include <linux-dmabuf-unstable-v1-server-protocol.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <cstdint>
#include <vector>

#include "WaylandShm.h" // for WaylandBufferBase

namespace android {

class WaylandCompositor;

// Per-buffer state for a wl_buffer created via linux-dmabuf.
struct WaylandDmabufBuffer : WaylandBufferBase {
    int32_t width = 0;
    int32_t height = 0;
    uint32_t format = 0;
    uint32_t flags = 0;

    struct Plane {
        int fd = -1;
        uint32_t offset = 0;
        uint32_t stride = 0;
        uint64_t modifier = 0;
    };
    std::vector<Plane> planes;

    static void bufferDestroy(struct wl_client* client, struct wl_resource* resource);
    static void onBufferDestroy(struct wl_resource* resource);
    static const struct wl_buffer_interface kBufferImpl;
};

// Manages zwp_linux_dmabuf_v1 global and zwp_linux_buffer_params_v1 objects.
class WaylandDmabuf {
public:
    static struct wl_global* createGlobal(struct wl_display* display,
                                           WaylandCompositor* compositor);

private:
    static void bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id);
    static void dmabufDestroy(struct wl_client* client, struct wl_resource* resource);
    static void dmabufCreateParams(struct wl_client* client, struct wl_resource* resource,
                                    uint32_t paramsId);

    static const struct zwp_linux_dmabuf_v1_interface kDmabufImpl;

    // zwp_linux_buffer_params_v1 per-object state
    struct ParamsData {
        WaylandCompositor* compositor = nullptr;
        struct wl_resource* paramsResource = nullptr;
        std::vector<WaylandDmabufBuffer::Plane> planes;
        bool used = false;
    };

    static void paramsDestroy(struct wl_client* client, struct wl_resource* resource);
    static void paramsAdd(struct wl_client* client, struct wl_resource* resource,
                          int32_t fd, uint32_t planeIdx, uint32_t offset,
                          uint32_t stride, uint32_t modifierHi, uint32_t modifierLo);
    static void paramsCreate(struct wl_client* client, struct wl_resource* resource,
                             int32_t width, int32_t height, uint32_t format, uint32_t flags);
    static void paramsCreateImmed(struct wl_client* client, struct wl_resource* resource,
                                   uint32_t bufferId, int32_t width, int32_t height,
                                   uint32_t format, uint32_t flags);
    static void onParamsDestroy(struct wl_resource* resource);

    static struct wl_resource* createBuffer(struct wl_client* client, uint32_t bufferId,
                                             ParamsData* params, int32_t width, int32_t height,
                                             uint32_t format, uint32_t flags);

    static const struct zwp_linux_buffer_params_v1_interface kParamsImpl;
};

} // namespace android
