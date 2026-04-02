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
#define LOG_TAG "WaylandShm"

#include "WaylandShm.h"
#include "WaylandCompositor.h"

#include <algorithm>
#include <cstring>

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include <log/log.h>

namespace android {

namespace {
constexpr uint32_t kShmVersion = 1;
} // anonymous namespace

// --- wl_shm_pool_interface ---

const struct wl_shm_pool_interface WaylandShmPool::kPoolImpl = {
        .create_buffer = WaylandShmPool::poolCreateBuffer,
        .destroy = WaylandShmPool::poolDestroy,
        .resize = WaylandShmPool::poolResize,
};

void WaylandShmPool::poolDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandShmPool::poolCreateBuffer(struct wl_client* client, struct wl_resource* resource,
                                       uint32_t id, int32_t offset, int32_t width, int32_t height,
                                       int32_t stride, uint32_t format) {
    auto* pool = static_cast<WaylandShmPool*>(wl_resource_get_user_data(resource));

    if (offset < 0 || width <= 0 || height <= 0 || stride <= 0) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE,
                               "invalid buffer params: offset=%d w=%d h=%d stride=%d",
                               offset, width, height, stride);
        return;
    }

    int64_t needed = static_cast<int64_t>(offset) + static_cast<int64_t>(stride) * height;
    if (needed > pool->size) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE,
                               "buffer exceeds pool: need %" PRId64 " have %d", needed, pool->size);
        return;
    }

    struct wl_resource* bufferResource =
            wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!bufferResource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* buf = new WaylandShmBuffer();
    buf->bufferType = WaylandBufferType::Shm;
    buf->compositor = pool->compositor;
    buf->pool = pool;
    buf->offset = offset;
    buf->width = width;
    buf->height = height;
    buf->stride = stride;
    buf->format = format;

    wl_resource_set_implementation(bufferResource, &WaylandShmBuffer::kBufferImpl,
                                    buf, WaylandShmBuffer::onBufferDestroy);

    ALOGD("Created SHM wl_buffer: %dx%d fmt=0x%08x stride=%d offset=%d",
          width, height, format, stride, offset);
}

void WaylandShmPool::poolResize(struct wl_client* /*client*/, struct wl_resource* resource,
                                 int32_t size) {
    auto* pool = static_cast<WaylandShmPool*>(wl_resource_get_user_data(resource));

    if (size <= 0 || size < pool->size) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
                               "invalid pool resize: %d (current %d)", size, pool->size);
        return;
    }

    void* newData = mremap(pool->data, static_cast<size_t>(pool->size),
                           static_cast<size_t>(size), MREMAP_MAYMOVE);
    if (newData == MAP_FAILED) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
                               "mremap failed: %s", strerror(errno));
        return;
    }

    pool->data = newData;
    pool->size = size;
}

void WaylandShmPool::onPoolDestroy(struct wl_resource* resource) {
    auto* pool = static_cast<WaylandShmPool*>(wl_resource_get_user_data(resource));
    if (pool) {
        if (pool->data && pool->data != MAP_FAILED) {
            munmap(pool->data, static_cast<size_t>(pool->size));
        }
        if (pool->fd >= 0) {
            close(pool->fd);
        }
        delete pool;
    }
}

// --- wl_buffer_interface for SHM buffers ---

const struct wl_buffer_interface WaylandShmBuffer::kBufferImpl = {
        .destroy = WaylandShmBuffer::bufferDestroy,
};

void WaylandShmBuffer::bufferDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandShmBuffer::onBufferDestroy(struct wl_resource* resource) {
    auto* buf = static_cast<WaylandShmBuffer*>(wl_resource_get_user_data(resource));
    if (buf) {
        if (buf->compositor) {
            buf->compositor->notifyBufferDestroyed(resource);
        }
        delete buf;
    }
}

const void* WaylandShmBuffer::pixelData() const {
    if (!pool || !pool->data) return nullptr;
    return static_cast<const uint8_t*>(pool->data) + offset;
}

// --- wl_shm_interface ---

const struct wl_shm_interface WaylandShm::kShmImpl = {
        .create_pool = WaylandShm::shmCreatePool,
};

struct wl_global* WaylandShm::createGlobal(struct wl_display* display,
                                             WaylandCompositor* compositor) {
    return wl_global_create(display, &wl_shm_interface, kShmVersion, compositor, bind);
}

void WaylandShm::bind(struct wl_client* client, void* data,
                       uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kShmVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &wl_shm_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kShmImpl, data, nullptr);

    // Advertise supported formats.
    wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);

    ALOGI("wl_shm bound (v%u)", ver);
}

void WaylandShm::shmCreatePool(struct wl_client* client, struct wl_resource* resource,
                                 uint32_t id, int32_t fd, int32_t size) {
    if (size <= 0) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
                               "invalid pool size %d", size);
        close(fd);
        return;
    }

    void* data = mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
                               "mmap failed: %s", strerror(errno));
        close(fd);
        return;
    }

    struct wl_resource* poolResource =
            wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    if (!poolResource) {
        munmap(data, static_cast<size_t>(size));
        close(fd);
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* pool = new WaylandShmPool();
    pool->compositor = static_cast<WaylandCompositor*>(wl_resource_get_user_data(resource));
    pool->data = data;
    pool->size = size;
    pool->fd = fd;

    wl_resource_set_implementation(poolResource, &WaylandShmPool::kPoolImpl,
                                    pool, WaylandShmPool::onPoolDestroy);

    ALOGD("Created SHM pool: %d bytes", size);
}

} // namespace android
