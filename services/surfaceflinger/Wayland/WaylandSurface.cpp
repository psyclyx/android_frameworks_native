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
#define LOG_TAG "WaylandSurface"

#include "WaylandSurface.h"
#include "WaylandCompositor.h"
#include "WaylandDmabuf.h"
#include "WaylandShm.h"

#include "Layer.h"
#include "SurfaceFlinger.h"

#include <atomic>
#include <cstring>
#include <sys/mman.h>

#include <android/gui/FrameTimelineInfo.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <gui/LayerState.h>
#include <gui/TransactionState.h>
#include <log/log.h>
#include <renderengine/ExternalTexture.h>
#include <renderengine/impl/ExternalTexture.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <utils/Timers.h>
#include <cros_gralloc/cros_gralloc_handle.h>
#include <cutils/native_handle.h>

// From external/minigbm (drv.h, cros_gralloc_handle.h).
// Must match values in external/minigbm/cros_gralloc/cros_gralloc_handle.h
// and external/minigbm/drv.h. Duplicated to avoid pulling in drv.h deps.
constexpr uint64_t kBoUseScanout = 1ull << 0;   // BO_USE_SCANOUT in drv.h
constexpr uint64_t kBoUseTexture = 1ull << 5;   // BO_USE_TEXTURE in drv.h
constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA; // cros_gralloc_handle::magic

namespace android {

using gui::FrameTimelineInfo;
using gui::GameMode;

namespace {

PixelFormat drmToPixelFormat(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ABGR8888:
            return PIXEL_FORMAT_RGBA_8888;
        case DRM_FORMAT_XBGR8888:
            return PIXEL_FORMAT_RGBX_8888;
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
            return PIXEL_FORMAT_BGRA_8888;
        default:
            ALOGW("Unsupported DRM format 0x%08x, falling back to RGBA_8888", drmFormat);
            return PIXEL_FORMAT_RGBA_8888;
    }
}

PixelFormat shmToPixelFormat(uint32_t shmFormat) {
    // Wayland ARGB8888 is BGRA in memory (little-endian), same as Android's
    // PIXEL_FORMAT_BGRA_8888. However, SwiftShader's Skia backend does not
    // support BGRA_8888 as a texture source. We allocate RGBA_8888 instead
    // and swizzle R<->B during the pixel copy in importShmBuffer().
    switch (shmFormat) {
        case WL_SHM_FORMAT_ARGB8888:
        case WL_SHM_FORMAT_XRGB8888:
            return PIXEL_FORMAT_RGBA_8888;
        default:
            ALOGW("Unsupported SHM format 0x%08x, falling back to RGBA_8888", shmFormat);
            return PIXEL_FORMAT_RGBA_8888;
    }
}

} // anonymous namespace

// wl_surface_interface — must match request order in wayland.xml
const struct wl_surface_interface WaylandSurface::kImpl = {
        .destroy = WaylandSurface::destroy,
        .attach = WaylandSurface::attach,
        .damage = WaylandSurface::damage,
        .frame = WaylandSurface::frame,
        .set_opaque_region = WaylandSurface::setOpaqueRegion,
        .set_input_region = WaylandSurface::setInputRegion,
        .commit = WaylandSurface::commit,
        .set_buffer_transform = WaylandSurface::setBufferTransform,
        .set_buffer_scale = WaylandSurface::setBufferScale,
        .damage_buffer = WaylandSurface::damageBuffer,
        .offset = WaylandSurface::offset,
};

void WaylandSurface::destroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandSurface::attach(struct wl_client* /*client*/, struct wl_resource* resource,
                            struct wl_resource* buffer, int32_t x, int32_t y) {
    auto* surface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));
    (void)x;
    (void)y;
    surface->pendingBuffer = buffer;
    surface->bufferAttached = true;
}

void WaylandSurface::damage(struct wl_client* /*client*/, struct wl_resource* /*resource*/,
                            int32_t /*x*/, int32_t /*y*/, int32_t /*width*/, int32_t /*height*/) {
    // Ignored for MVP — we always composite the full surface.
}

void WaylandSurface::frame(struct wl_client* client, struct wl_resource* resource,
                           uint32_t callback) {
    auto* surface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));
    struct wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, callback);
    if (!cb) {
        wl_resource_post_no_memory(resource);
        return;
    }
    surface->pendingFrameCallbacks.push_back(cb);
}

void WaylandSurface::setOpaqueRegion(struct wl_client* /*client*/,
                                     struct wl_resource* /*resource*/,
                                     struct wl_resource* /*region*/) {
    // Ignored for MVP.
}

void WaylandSurface::setInputRegion(struct wl_client* /*client*/,
                                    struct wl_resource* /*resource*/,
                                    struct wl_resource* /*region*/) {
    // Ignored for MVP.
}

void WaylandSurface::commit(struct wl_client* /*client*/, struct wl_resource* resource) {
    auto* surface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));

    if (surface->bufferAttached && surface->pendingBuffer && surface->layer.promote() != nullptr) {
        auto* base = static_cast<WaylandBufferBase*>(
                wl_resource_get_user_data(surface->pendingBuffer));
        bool imported = false;

        if (!base) {
            ALOGE("wl_surface.commit: attached buffer has no user data");
        } else if (base->bufferType == WaylandBufferType::Dmabuf) {
            auto* dmabuf = static_cast<WaylandDmabufBuffer*>(base);
            if (dmabuf->planes.empty()) {
                ALOGE("wl_surface.commit: dmabuf buffer has no planes");
            } else {
                surface->importBuffer(dmabuf);
                imported = true;
            }
        } else if (base->bufferType == WaylandBufferType::Shm) {
            auto* shm = static_cast<WaylandShmBuffer*>(base);
            surface->importShmBuffer(shm);
            imported = true;
        } else {
            ALOGE("wl_surface.commit: unknown buffer type %d",
                  static_cast<int>(base->bufferType));
        }

        if (imported) {
            // Queue the previous buffer for release after the next composite cycle.
            // Per Wayland protocol, release must wait until the compositor is done reading.
            if (surface->currentBuffer && surface->currentBuffer != surface->pendingBuffer) {
                surface->compositor->queueBufferRelease(surface->currentBuffer);
            }
            surface->currentBuffer = surface->pendingBuffer;
        }
        surface->bufferAttached = false;
        surface->pendingBuffer = nullptr;
    } else if (surface->bufferAttached) {
        // Null buffer attach = unmap. Queue release after composite.
        if (surface->currentBuffer) {
            surface->compositor->queueBufferRelease(surface->currentBuffer);
            surface->currentBuffer = nullptr;
        }
        surface->bufferAttached = false;
        surface->pendingBuffer = nullptr;
    }

    // Queue frame callbacks for VSYNC-aligned delivery after SF composite.
    if (!surface->pendingFrameCallbacks.empty() && surface->compositor) {
        surface->compositor->queueFrameCallbacks(std::move(surface->pendingFrameCallbacks));
    }
    surface->pendingFrameCallbacks.clear();
}

void WaylandSurface::importBuffer(WaylandDmabufBuffer* dmabuf) {
    const uint32_t numPlanes = static_cast<uint32_t>(dmabuf->planes.size());
    if (numPlanes == 0 || numPlanes > DRV_MAX_PLANES) {
        ALOGE("Invalid plane count %u", numPlanes);
        return;
    }

    PixelFormat pixFmt = drmToPixelFormat(dmabuf->format);
    uint32_t bpp = bytesPerPixel(pixFmt);
    uint32_t w = static_cast<uint32_t>(dmabuf->width);
    uint32_t h = static_cast<uint32_t>(dmabuf->height);
    uint32_t stride = dmabuf->planes[0].stride;
    uint32_t pixelStride = (bpp > 0) ? stride / bpp : stride;
    uint64_t usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;

    sp<GraphicBuffer> gb;

    // --- Strategy 1: cros_gralloc (minigbm) direct import via DRM PRIME ---
    // Works on Cuttlefish and ChromeOS-derived gralloc implementations.
    {
        const int numFds = static_cast<int>(numPlanes);
        const int numInts = static_cast<int>(
                (sizeof(struct cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int)) -
                numFds;

        auto* hnd = reinterpret_cast<struct cros_gralloc_handle*>(
                native_handle_create(numFds, numInts));
        if (hnd) {
            memset(&hnd->fds, 0, sizeof(struct cros_gralloc_handle) - sizeof(native_handle_t));
            for (size_t i = 0; i < DRV_MAX_FDS; i++)
                hnd->fds[i] = -1;

            for (uint32_t i = 0; i < numPlanes; i++) {
                hnd->fds[i] = dmabuf->planes[i].fd;
                hnd->strides[i] = dmabuf->planes[i].stride;
                hnd->offsets[i] = dmabuf->planes[i].offset;
                hnd->sizes[i] = dmabuf->planes[i].stride * h;
            }

            static std::atomic<uint32_t> nextBufferId{1};
            hnd->id = nextBufferId++;
            hnd->width = w;
            hnd->height = h;
            hnd->format = dmabuf->format;
            hnd->tiling = 0;
            hnd->format_modifier = dmabuf->planes[0].modifier;
            hnd->use_flags = kBoUseTexture | kBoUseScanout;
            hnd->magic = kCrosGrallocMagic;
            hnd->pixel_stride = pixelStride;
            hnd->droid_format = pixFmt;
            hnd->usage = static_cast<int64_t>(usage);
            hnd->num_planes = numPlanes;
            hnd->reserved_region_size = 0;
            hnd->total_size = 0;
            for (uint32_t i = 0; i < numPlanes; i++)
                hnd->total_size += hnd->sizes[i];

            sp<GraphicBuffer> candidate = sp<GraphicBuffer>::make(
                    reinterpret_cast<const native_handle_t*>(hnd), GraphicBuffer::CLONE_HANDLE,
                    w, h, pixFmt, 1u, usage, pixelStride);

            for (uint32_t i = 0; i < numPlanes; i++)
                hnd->fds[i] = -1;
            native_handle_close(reinterpret_cast<native_handle_t*>(hnd));
            native_handle_delete(reinterpret_cast<native_handle_t*>(hnd));

            if (candidate->initCheck() == NO_ERROR) {
                gb = std::move(candidate);
                ALOGD("dmabuf import: cros_gralloc path succeeded for %ux%u", w, h);
            }
        }
    }

    // --- Strategy 2: mmap + copy fallback for non-minigbm gralloc (e.g. QTI) ---
    // Allocate a new GraphicBuffer and copy the client's dmabuf content into it.
    // This is a CPU copy but works with any gralloc and any standard Wayland client.
    if (!gb) {
        int fd = dmabuf->planes[0].fd;
        uint32_t offset = dmabuf->planes[0].offset;
        size_t mapSize = static_cast<size_t>(stride) * h + offset;

        void* src = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fd, 0);
        if (src == MAP_FAILED) {
            ALOGE("dmabuf import: mmap failed for fd %d: %s", fd, strerror(errno));
            return;
        }

        gb = sp<GraphicBuffer>::make(w, h, pixFmt, 1u,
                static_cast<uint64_t>(GRALLOC_USAGE_SW_WRITE_OFTEN |
                                      GRALLOC_USAGE_HW_TEXTURE |
                                      GRALLOC_USAGE_HW_COMPOSER),
                "WaylandDmabuf");

        if (gb->initCheck() != NO_ERROR) {
            ALOGE("dmabuf import: GraphicBuffer alloc failed");
            munmap(src, mapSize);
            return;
        }

        void* dst = nullptr;
        status_t lockErr = gb->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &dst);
        if (lockErr != NO_ERROR || !dst) {
            ALOGE("dmabuf import: GraphicBuffer lock failed: %d", lockErr);
            munmap(src, mapSize);
            return;
        }

        const uint8_t* srcBytes = static_cast<const uint8_t*>(src) + offset;
        uint8_t* dstBytes = static_cast<uint8_t*>(dst);
        uint32_t dstStride = gb->getStride() * bpp;
        uint32_t copyWidth = w * bpp;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(dstBytes + y * dstStride, srcBytes + y * stride, copyWidth);
        }

        gb->unlock();
        munmap(src, mapSize);
        ALOGD("dmabuf import: mmap+copy fallback for %ux%u (stride %u→%u)", w, h, stride, dstStride);
    }

    ++frameNumber;

    // Post the SF transaction to a dedicated buffer thread to avoid blocking
    // the Wayland dispatch thread.
    WaylandCompositor::BufferWork bw;
    bw.handle = handle;
    bw.gb = gb;
    bw.frameNumber = frameNumber;
    bw.producerId = producerId;
    bw.layerId = layerId;
    bw.width = dmabuf->width;
    bw.height = dmabuf->height;
    compositor->postBufferWork(std::move(bw));

    ALOGD("wl_surface.commit: imported %dx%d buffer (fmt=0x%08x) to layer %u, frame %" PRIu64,
          dmabuf->width, dmabuf->height, dmabuf->format, layerId, frameNumber);
}

void WaylandSurface::importShmBuffer(WaylandShmBuffer* shm) {
    if (!shm->pool || !shm->pool->data) {
        ALOGE("wl_surface.commit: SHM buffer has no pool or pool data");
        return;
    }

    // Validate buffer bounds against the pool.
    int64_t needed = static_cast<int64_t>(shm->offset) +
                     static_cast<int64_t>(shm->stride) * shm->height;
    if (needed > shm->pool->size) {
        ALOGE("wl_surface.commit: SHM buffer exceeds pool: need %" PRId64 " have %d",
              needed, shm->pool->size);
        return;
    }

    const void* pixels = shm->pixelData();
    if (!pixels) {
        ALOGE("wl_surface.commit: SHM buffer has null pixel data");
        return;
    }

    PixelFormat pixFmt = shmToPixelFormat(shm->format);
    const uint32_t srcStride = static_cast<uint32_t>(shm->stride);
    const uint32_t h = static_cast<uint32_t>(shm->height);


    // Snapshot the pixel data from the SHM pool. This is a fast memcpy that
    // doesn't touch gralloc, so it's safe on the Wayland dispatch thread.
    // The heavy work (gralloc alloc + swizzle + setTransactionState) happens
    // on the buffer thread to avoid deadlocking with Vulkan WSI clients.
    const size_t dataSize = static_cast<size_t>(srcStride) * h;
    std::vector<uint8_t> pixelCopy(dataSize);
    memcpy(pixelCopy.data(), pixels, dataSize);

    ++frameNumber;

    WaylandCompositor::BufferWork work;
    work.handle = handle;
    work.pixels = std::move(pixelCopy);
    work.pixFmt = pixFmt;
    work.srcStride = srcStride;
    work.frameNumber = frameNumber;
    work.producerId = producerId;
    work.layerId = layerId;
    work.width = shm->width;
    work.height = shm->height;
    compositor->postBufferWork(std::move(work));

    ALOGD("wl_surface.commit: queued SHM %dx%d buffer (fmt=0x%08x) to layer %u, frame %" PRIu64,
          shm->width, shm->height, shm->format, layerId, frameNumber);
}

void WaylandSurface::setBufferTransform(struct wl_client* /*client*/,
                                        struct wl_resource* /*resource*/, int32_t /*transform*/) {
    // Ignored for MVP.
}

void WaylandSurface::setBufferScale(struct wl_client* /*client*/,
                                    struct wl_resource* /*resource*/, int32_t /*scale*/) {
    // Ignored for MVP.
}

void WaylandSurface::damageBuffer(struct wl_client* /*client*/, struct wl_resource* /*resource*/,
                                  int32_t /*x*/, int32_t /*y*/, int32_t /*width*/,
                                  int32_t /*height*/) {
    // Ignored for MVP.
}

void WaylandSurface::offset(struct wl_client* /*client*/, struct wl_resource* /*resource*/,
                            int32_t /*x*/, int32_t /*y*/) {
    // Ignored for MVP.
}

void WaylandSurface::onDestroy(struct wl_resource* resource) {
    auto* surface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));
    if (surface) {
        if (surface->currentBuffer) {
            wl_buffer_send_release(surface->currentBuffer);
            surface->currentBuffer = nullptr;
        }
        if (surface->compositor) {
            surface->compositor->removeSurface(resource);
        }
    }
}

} // namespace android
