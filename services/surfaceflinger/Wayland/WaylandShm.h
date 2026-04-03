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
#include <wayland-server-protocol.h>

#include <cstdint>

namespace android {

class WaylandCompositor;

// Shared tag for identifying buffer type in wl_surface.commit().
enum class WaylandBufferType : uint8_t { Dmabuf, Shm };
struct WaylandBufferBase {
    WaylandBufferType bufferType;
    WaylandCompositor* compositor = nullptr;
};

// Per-pool state for wl_shm_pool (mmap'd shared memory region).
// Reference-counted: the mmap and fd stay alive as long as any buffer
// created from this pool exists, per the Wayland wl_shm_pool spec.
struct WaylandShmPool {
    WaylandCompositor* compositor = nullptr;
    void* data = nullptr;   // mmap'd region
    int32_t size = 0;
    int fd = -1;
    int refCount = 1;       // starts at 1 for the pool resource itself

    void ref() { ++refCount; }
    void unref();

    static void poolDestroy(struct wl_client* client, struct wl_resource* resource);
    static void poolCreateBuffer(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t id, int32_t offset, int32_t width, int32_t height,
                                  int32_t stride, uint32_t format);
    static void poolResize(struct wl_client* client, struct wl_resource* resource, int32_t size);
    static void onPoolDestroy(struct wl_resource* resource);

    static const struct wl_shm_pool_interface kPoolImpl;
};

// Per-buffer state for a wl_buffer created via wl_shm.
struct WaylandShmBuffer : WaylandBufferBase {
    WaylandShmPool* pool = nullptr;
    int32_t offset = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    uint32_t format = 0;   // wl_shm_format

    const void* pixelData() const;

    static void bufferDestroy(struct wl_client* client, struct wl_resource* resource);
    static void onBufferDestroy(struct wl_resource* resource);
    static const struct wl_buffer_interface kBufferImpl;
};

// Manages wl_shm global.
class WaylandShm {
public:
    static struct wl_global* createGlobal(struct wl_display* display,
                                           WaylandCompositor* compositor);

private:
    static void bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id);
    static void shmCreatePool(struct wl_client* client, struct wl_resource* resource,
                               uint32_t id, int32_t fd, int32_t size);

    static const struct wl_shm_interface kShmImpl;
};

} // namespace android
