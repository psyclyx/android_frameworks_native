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

// Uncomment to apply colored stripe overlays on CPU-copied buffers.
// Blue vertical stripes = dmabuf mmap+copy fallback path.
// Each stripe is 8px wide, 50% opacity blend.
#define WAYLAND_DEBUG_CPU_COPY_TINT 1

#include "WaylandSurface.h"
#include "WaylandCompositor.h"
#include "WaylandDmabuf.h"
#include "WaylandLayerShell.h"
#include "WaylandShm.h"
#include "WaylandXdgShell.h"

#include "Layer.h"
#include "SurfaceFlinger.h"

#include <atomic>
#include <cstring>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <android/gui/FrameTimelineInfo.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <gui/LayerState.h>
#include <gui/TransactionState.h>
#include <linux/memfd.h>
#include <log/log.h>
#include <renderengine/ExternalTexture.h>
#include <renderengine/impl/ExternalTexture.h>
#include <sys/syscall.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Timers.h>
#include <cros_gralloc/cros_gralloc_handle.h>
#include <cutils/native_handle.h>

// From external/minigbm (drv.h, cros_gralloc_handle.h).
// Must match values in external/minigbm/cros_gralloc/cros_gralloc_handle.h
// and external/minigbm/drv.h. Duplicated to avoid pulling in drv.h deps.
constexpr uint64_t kBoUseScanout = 1ull << 0;   // BO_USE_SCANOUT in drv.h
constexpr uint64_t kBoUseTexture = 1ull << 5;   // BO_USE_TEXTURE in drv.h
constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA; // cros_gralloc_handle::magic

// QTI gralloc private_handle_t constants (from gr_priv_handle.h).
// Duplicated to avoid a build dependency on the QTI gralloc HAL.
constexpr int kQtiGrallocMagic = 'gmsm';
constexpr int kQtiNumFds = 2;  // fd + fd_metadata

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
                surface->importBuffer(dmabuf, surface->pendingBuffer);
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
            // For dmabuf: SF's releaseBufferListener callback handles the
            // release (with HWC fence import into the dma-buf).
            // For SHM: use the time-based release queue (compositor owns the
            // gralloc copy, so no fence is needed).
            bool isDmabuf = base && base->bufferType == WaylandBufferType::Dmabuf;
            if (!isDmabuf && surface->currentBuffer &&
                surface->currentBuffer != surface->pendingBuffer) {
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

    // Deferred window creation: on first commit with a buffer, if this surface
    // has an xdg_toplevel role, create the Android window now. By this point we
    // know title, app_id, parent, and buffer dimensions.
    if (surface->xdgSurface && surface->xdgToplevel && surface->currentBuffer) {
        auto* xdgSurf = static_cast<WaylandXdgSurface*>(
                wl_resource_get_user_data(surface->xdgSurface));
        if (xdgSurf && !xdgSurf->mapped) {
            xdgSurf->mapped = true;

            // Resolve parent layerId (-1 = no parent = top-level Activity)
            int parentLayerId = -1;
            if (xdgSurf->parentToplevel) {
                auto* parentXdg = static_cast<WaylandXdgSurface*>(
                        wl_resource_get_user_data(xdgSurf->parentToplevel));
                if (parentXdg) {
                    WaylandSurface* parentWs =
                            surface->compositor->findSurface(parentXdg->wlSurface);
                    if (parentWs) {
                        parentLayerId = static_cast<int>(parentWs->layerId);
                    }
                }
            }

            surface->compositor->requestCreateWindow(
                    static_cast<int>(surface->layerId), surface->handle,
                    xdgSurf->title.empty() ? nullptr : xdgSurf->title.c_str(),
                    xdgSurf->appId.empty() ? nullptr : xdgSurf->appId.c_str(),
                    parentLayerId, 0, 0);
        }
    }

    // Layer-shell: send initial configure on first commit (no buffer),
    // and apply layout on subsequent commits with a buffer.
    if (surface->layerSurface) {
        auto* ls = static_cast<WaylandLayerSurface*>(
                wl_resource_get_user_data(surface->layerSurface));
        if (ls) {
            if (!ls->configured) {
                WaylandLayerShell::sendConfigure(ls);
            } else if (surface->currentBuffer) {
                WaylandLayerShell::applyLayout(ls);
            }
        }
    }

    // Queue frame callbacks for VSYNC-aligned delivery after SF composite.
    if (!surface->pendingFrameCallbacks.empty() && surface->compositor) {
        surface->compositor->queueFrameCallbacks(std::move(surface->pendingFrameCallbacks));
    }
    surface->pendingFrameCallbacks.clear();
}

void WaylandSurface::importBuffer(WaylandDmabufBuffer* dmabuf,
                                   struct wl_resource* wlBuffer) {
    const uint32_t numPlanes = static_cast<uint32_t>(dmabuf->planes.size());
    if (numPlanes == 0) {
        ALOGE("Invalid plane count %u", numPlanes);
        return;
    }

    PixelFormat pixFmt = drmToPixelFormat(dmabuf->format);
    uint32_t w = static_cast<uint32_t>(dmabuf->width);
    uint32_t h = static_cast<uint32_t>(dmabuf->height);
    uint32_t stride = dmabuf->planes[0].stride;

    uint64_t usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;
    uint32_t bpp = bytesPerPixel(pixFmt);
    uint32_t pixelStride = (bpp > 0) ? stride / bpp : stride;

    // Look up the dmabuf by its inode to reuse cached GraphicBuffer imports.
    // Vulkan swapchains cycle through a fixed set of dmabuf fds.
    struct stat st;
    uint64_t inodeKey = 0;
    if (fstat(dmabuf->planes[0].fd, &st) == 0) {
        inodeKey = st.st_ino;
    }

    // Look up cached gralloc handle, or import if first time seeing this dmabuf.
    ImportedHandle* cached = nullptr;
    if (inodeKey != 0) {
        auto it = importedBuffers.find(inodeKey);
        if (it != importedBuffers.end()) {
            cached = &it->second;
        }
    }

    if (!cached) {
        // --- QTI gralloc zero-copy import via private_handle_t ---
        int metaFd = static_cast<int>(
                syscall(__NR_memfd_create, "wayland_dmabuf_meta", MFD_CLOEXEC));
        if (metaFd >= 0) {
            const size_t metaSize = 65536;
            ftruncate(metaFd, metaSize);
            void* metaMap = mmap(nullptr, metaSize, PROT_READ | PROT_WRITE,
                                MAP_SHARED, metaFd, 0);
            if (metaMap != MAP_FAILED) {
                memset(metaMap, 0, metaSize);
                munmap(metaMap, metaSize);
            }

            constexpr int kQtiNumInts = 23;
            auto* hnd = reinterpret_cast<native_handle_t*>(
                    native_handle_create(kQtiNumFds, kQtiNumInts));
            if (hnd) {
                int* data = &hnd->data[0];
                memset(data, 0, (kQtiNumFds + kQtiNumInts) * sizeof(int));

                int dupFd = dup(dmabuf->planes[0].fd);
                int dupMetaFd = dup(metaFd);

                data[0] = dupFd;
                data[1] = dupMetaFd;
                int idx = 2;
                data[idx++] = kQtiGrallocMagic;
                data[idx++] = 0x00100000 | 0x00080000;
                data[idx++] = static_cast<int>(pixelStride);
                data[idx++] = static_cast<int>(h);
                data[idx++] = static_cast<int>(w);
                data[idx++] = static_cast<int>(h);
                data[idx++] = pixFmt;
                data[idx++] = 0;
                *reinterpret_cast<unsigned int*>(&data[idx++]) = 1;
                static std::atomic<uint64_t> nextQtiId{1};
                uint64_t bufId = nextQtiId++;
                memcpy(&data[idx], &bufId, sizeof(uint64_t));
                idx += 2;
                memcpy(&data[idx], &usage, sizeof(uint64_t));
                idx += 2;
                *reinterpret_cast<unsigned int*>(&data[idx++]) = stride * h;

                buffer_handle_t importedHandle = nullptr;
                status_t importErr = GraphicBufferMapper::get().importBufferNoValidate(
                        hnd, &importedHandle);

                data[0] = -1;
                data[1] = -1;
                close(dupFd);
                close(dupMetaFd);
                native_handle_delete(hnd);

                if (importErr == NO_ERROR && importedHandle) {
                    ImportedHandle ih;
                    ih.handle = importedHandle;
                    ih.width = w;
                    ih.height = h;
                    ih.format = pixFmt;
                    ih.pixelStride = pixelStride;
                    ih.usage = usage;
                    if (inodeKey != 0) {
                        importedBuffers[inodeKey] = ih;
                        cached = &importedBuffers[inodeKey];
                    } else {
                        // No inode key — unlikely, but import anyway as one-shot.
                        importedBuffers[reinterpret_cast<uint64_t>(importedHandle)] = ih;
                        cached = &importedBuffers[reinterpret_cast<uint64_t>(importedHandle)];
                    }
                    ALOGD("dmabuf import: QTI gralloc imported %ux%u (inode %" PRIu64 ")",
                          w, h, inodeKey);
                }
            }
            close(metaFd);
        }
    }

    if (!cached || !cached->handle) {
        ALOGE("dmabuf import: all strategies failed for %ux%u", w, h);
        return;
    }

    // Create a fresh GraphicBuffer wrapper each frame so HWC sees a unique
    // buffer ID and doesn't skip the update via its buffer cache.
    sp<GraphicBuffer> gb = sp<GraphicBuffer>::make(
            cached->handle, GraphicBuffer::WRAP_HANDLE,
            cached->width, cached->height, cached->format,
            1u, cached->usage, cached->pixelStride);

    ++frameNumber;

    WaylandCompositor::BufferWork bw;
    bw.handle = handle;
    bw.gb = gb;
    bw.frameNumber = frameNumber;
    bw.producerId = producerId;
    bw.layerId = layerId;
    bw.width = dmabuf->width;
    bw.height = dmabuf->height;
    bw.dmabufFd = dup(dmabuf->planes[0].fd);
    bw.wlBuffer = wlBuffer; // track for fence-based release

    // Extract GPU fence NOW on the dispatch thread — by the time the buffer
    // thread processes this, the GPU will likely have finished and the fence
    // will be stale/signaled.  Extracting here catches it while still active.
    {
        struct dma_buf_export_sync_file exportSync = {};
        exportSync.flags = DMA_BUF_SYNC_READ;
        exportSync.fd = -1;
        if (ioctl(bw.dmabufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exportSync) == 0 &&
            exportSync.fd >= 0) {
            bw.acquireFence = sp<Fence>::make(exportSync.fd);
        }
    }

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
