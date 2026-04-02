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

#include <android/gui/FrameTimelineInfo.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <gui/LayerState.h>
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

    // Construct a cros_gralloc_handle so that minigbm's mapper can import
    // the dmabuf via DRM PRIME.  This is cros_gralloc-specific but covers
    // Cuttlefish and all minigbm-based devices.
    const int numFds = static_cast<int>(numPlanes); // no reserved region
    const int numInts = static_cast<int>(
            (sizeof(struct cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int)) -
            numFds;

    auto* hnd = reinterpret_cast<struct cros_gralloc_handle*>(
            native_handle_create(numFds, numInts));
    if (!hnd) {
        ALOGE("native_handle_create failed for cros_gralloc_handle");
        return;
    }

    // Zero the metadata region (everything past the native_handle_t header + fds).
    memset(&hnd->fds, 0, sizeof(struct cros_gralloc_handle) - sizeof(native_handle_t));

    // Initialize unused fd slots to -1.
    for (size_t i = 0; i < DRV_MAX_FDS; i++)
        hnd->fds[i] = -1;

    PixelFormat pixFmt = drmToPixelFormat(dmabuf->format);
    uint32_t bpp = bytesPerPixel(pixFmt);

    for (uint32_t i = 0; i < numPlanes; i++) {
        hnd->fds[i] = dmabuf->planes[i].fd;
        hnd->strides[i] = dmabuf->planes[i].stride;
        hnd->offsets[i] = dmabuf->planes[i].offset;
        // Approximate plane size — exact value not critical for import.
        hnd->sizes[i] = dmabuf->planes[i].stride *
                         static_cast<uint32_t>(dmabuf->height);
    }

    static std::atomic<uint32_t> nextBufferId{1};
    hnd->id = nextBufferId++;
    hnd->width = static_cast<uint32_t>(dmabuf->width);
    hnd->height = static_cast<uint32_t>(dmabuf->height);
    hnd->format = dmabuf->format; // DRM fourcc
    hnd->tiling = 0;              // LINEAR
    hnd->format_modifier = dmabuf->planes[0].modifier;
    hnd->use_flags = kBoUseTexture | kBoUseScanout;
    hnd->magic = kCrosGrallocMagic;
    hnd->pixel_stride = (bpp > 0) ? hnd->strides[0] / bpp : hnd->strides[0];
    hnd->droid_format = pixFmt;
    hnd->usage = static_cast<int64_t>(GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER);
    hnd->num_planes = numPlanes;
    hnd->reserved_region_size = 0;
    hnd->total_size = 0;
    for (uint32_t i = 0; i < numPlanes; i++)
        hnd->total_size += hnd->sizes[i];

    uint64_t usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;

    sp<GraphicBuffer> gb = sp<GraphicBuffer>::make(
            reinterpret_cast<const native_handle_t*>(hnd), GraphicBuffer::CLONE_HANDLE,
            hnd->width, hnd->height, pixFmt, static_cast<uint32_t>(1) /* layerCount */,
            usage, hnd->pixel_stride);

    // CLONE_HANDLE dups the fds, so we can free our handle copy.
    // Restore fds to -1 so native_handle_close does not close the originals.
    for (uint32_t i = 0; i < numPlanes; i++)
        hnd->fds[i] = -1;
    native_handle_close(reinterpret_cast<native_handle_t*>(hnd));
    native_handle_delete(reinterpret_cast<native_handle_t*>(hnd));

    status_t err = gb->initCheck();
    if (err != NO_ERROR) {
        ALOGE("GraphicBuffer import failed for layer %u: %d (%s)",
              layerId, err, strerror(-err));
        return;
    }

    sp<Layer> strongLayer = layer.promote();
    if (!strongLayer) {
        ALOGE("wl_surface.commit: layer already destroyed for surface %u", layerId);
        return;
    }

    auto& re = compositor->flinger().getRenderEngine();
    std::shared_ptr<renderengine::ExternalTexture> texture =
            std::make_shared<renderengine::impl::ExternalTexture>(
                    gb, re, renderengine::impl::ExternalTexture::Usage::READABLE);

    BufferData bufferData;
    bufferData.buffer = gb;
    bufferData.frameNumber = ++frameNumber;
    bufferData.flags |= BufferData::BufferDataChange::frameNumberChanged;
    bufferData.acquireFence = Fence::NO_FENCE;
    bufferData.producerId = producerId;

    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    FrameTimelineInfo ftInfo;

    strongLayer->setBuffer(texture, bufferData, now, /*desiredPresentTime=*/0,
                           /*isAutoTimestamp=*/true, ftInfo, GameMode::Unsupported);

    // Signal SF that a transaction is pending so it schedules a composite cycle.
    compositor->scheduleComposite();

    ALOGD("wl_surface.commit: imported %dx%d buffer (fmt=0x%08x) to layer %u, frame %" PRIu64,
          dmabuf->width, dmabuf->height, dmabuf->format, layerId, frameNumber);
}

void WaylandSurface::importShmBuffer(WaylandShmBuffer* shm) {
    const void* pixels = shm->pixelData();
    if (!pixels) {
        ALOGE("wl_surface.commit: SHM buffer has null pixel data");
        return;
    }

    PixelFormat pixFmt = shmToPixelFormat(shm->format);

    sp<GraphicBuffer> gb = sp<GraphicBuffer>::make(
            static_cast<uint32_t>(shm->width), static_cast<uint32_t>(shm->height), pixFmt,
            1u /* layerCount */,
            static_cast<uint64_t>(GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_TEXTURE |
                                  GRALLOC_USAGE_HW_COMPOSER),
            "WaylandShm");

    status_t err = gb->initCheck();
    if (err != NO_ERROR) {
        ALOGE("GraphicBuffer alloc failed for SHM buffer: %d (%s)", err, strerror(-err));
        return;
    }

    void* dst = nullptr;
    err = gb->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &dst);
    if (err != NO_ERROR || !dst) {
        ALOGE("GraphicBuffer lock failed: %d (%s)", err, strerror(-err));
        return;
    }

    // Copy with BGRA→RGBA swizzle (Wayland ARGB8888 is BGRA in memory;
    // we allocated RGBA_8888 for Skia/SwiftShader compatibility).
    const uint32_t bpp = bytesPerPixel(pixFmt);
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    uint8_t* dstBytes = static_cast<uint8_t*>(dst);
    const uint32_t dstStride = gb->getStride() * bpp;
    const uint32_t srcStride = static_cast<uint32_t>(shm->stride);
    const uint32_t w = static_cast<uint32_t>(shm->width);
    const uint32_t h = static_cast<uint32_t>(shm->height);

    for (uint32_t y = 0; y < h; y++) {
        const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(src + y * srcStride);
        uint32_t* dstRow = reinterpret_cast<uint32_t*>(dstBytes + y * dstStride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = srcRow[x]; // BGRA byte order (Wayland ARGB8888)
            // Swap R and B: BGRA → RGBA
            uint32_t b = (px >> 0) & 0xFF;
            uint32_t g = (px >> 8) & 0xFF;
            uint32_t r = (px >> 16) & 0xFF;
            uint32_t a = (px >> 24) & 0xFF;
            dstRow[x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }

    gb->unlock();

    sp<Layer> strongLayer = layer.promote();
    if (!strongLayer) {
        ALOGE("wl_surface.commit: layer already destroyed for SHM surface %u", layerId);
        return;
    }

    auto& re = compositor->flinger().getRenderEngine();
    std::shared_ptr<renderengine::ExternalTexture> texture =
            std::make_shared<renderengine::impl::ExternalTexture>(
                    gb, re, renderengine::impl::ExternalTexture::Usage::READABLE);

    BufferData bufferData;
    bufferData.buffer = gb;
    bufferData.frameNumber = ++frameNumber;
    bufferData.flags |= BufferData::BufferDataChange::frameNumberChanged;
    bufferData.acquireFence = Fence::NO_FENCE;
    bufferData.producerId = producerId;

    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    FrameTimelineInfo ftInfo;

    strongLayer->setBuffer(texture, bufferData, now, /*desiredPresentTime=*/0,
                           /*isAutoTimestamp=*/true, ftInfo, GameMode::Unsupported);

    // Signal SF that a transaction is pending so it schedules a composite cycle.
    compositor->scheduleComposite();

    ALOGD("wl_surface.commit: imported SHM %dx%d buffer (fmt=0x%08x) to layer %u, frame %" PRIu64,
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
