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
#define WAYLAND_DEBUG_CPU_COPY_TINT 0

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
#include <poll.h>
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

    if (buffer) {
        auto* base = static_cast<WaylandBufferBase*>(wl_resource_get_user_data(buffer));
        WL_LOGV("[BUF] attach: layer=%u type=%s buffer=%p",
              surface->layerId,
              base ? (base->bufferType == WaylandBufferType::Dmabuf ? "dmabuf" : "shm") : "null",
              buffer);
    } else {
        WL_LOGV("[BUF] attach: layer=%u NULL buffer (unmap)", surface->layerId);
    }
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

    WL_LOGV("[BUF] commit: layer=%u attached=%d pending=%p currentBuf=%p hasLayer=%d",
          surface->layerId, surface->bufferAttached, surface->pendingBuffer,
          surface->currentBuffer, surface->layer.promote() != nullptr);

    if (surface->bufferAttached && surface->pendingBuffer && surface->layer.promote() != nullptr) {
        auto* base = static_cast<WaylandBufferBase*>(
                wl_resource_get_user_data(surface->pendingBuffer));
        bool imported = false;

        if (!base) {
            ALOGE("[BUF] commit: attached buffer has no user data");
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
            // With the CPU-copy debug path, all buffers use time-based release
            // (compositor owns the gralloc copy, client buffer can be reused
            // immediately after the copy).
            if (surface->currentBuffer &&
                surface->currentBuffer != surface->pendingBuffer) {
                WL_LOGV("[BUF] commit: releasing prev buffer=%p for layer=%u",
                      surface->currentBuffer, surface->layerId);
                surface->compositor->queueBufferRelease(surface->currentBuffer);
            }
            surface->currentBuffer = surface->pendingBuffer;
            WL_LOGV("[BUF] commit: buffer imported OK, currentBuffer=%p layer=%u frame=%" PRIu64,
                  surface->currentBuffer, surface->layerId, surface->frameNumber);
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
    // has an xdg role (toplevel or popup), create the Android window now.
    if (surface->xdgSurface && surface->currentBuffer) {
        auto* xdgSurf = static_cast<WaylandXdgSurface*>(
                wl_resource_get_user_data(surface->xdgSurface));
        if (xdgSurf && !xdgSurf->mapped) {
            if (xdgSurf->toplevel) {
                xdgSurf->mapped = true;

                // Dismiss any open popups from this client (e.g. menu that
                // triggered this dialog).
                surface->compositor->dismissPopupsForClient(
                        wl_resource_get_client(resource));

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

                // Get buffer dimensions for dialog sizing.
                // Use full buffer size (includes CSD shadow) so the semi-transparent
                // shadow blends naturally with the background.
                int winW = 0, winH = 0;
                if (surface->currentBuffer) {
                    auto* bufBase = static_cast<WaylandBufferBase*>(
                            wl_resource_get_user_data(surface->currentBuffer));
                    if (bufBase && bufBase->bufferType == WaylandBufferType::Dmabuf) {
                        auto* dmabuf = static_cast<WaylandDmabufBuffer*>(bufBase);
                        winW = dmabuf->width;
                        winH = dmabuf->height;
                    } else if (bufBase && bufBase->bufferType == WaylandBufferType::Shm) {
                        auto* shm = static_cast<WaylandShmBuffer*>(bufBase);
                        winW = shm->width;
                        winH = shm->height;
                    }
                }

                surface->compositor->requestCreateWindow(
                        static_cast<int>(surface->layerId), surface->handle,
                        xdgSurf->title.empty() ? nullptr : xdgSurf->title.c_str(),
                        xdgSurf->appId.empty() ? nullptr : xdgSurf->appId.c_str(),
                        parentLayerId, winW, winH);
            } else if (xdgSurf->popup) {
                xdgSurf->mapped = true;

                auto* popup = static_cast<WaylandXdgPopup*>(
                        wl_resource_get_user_data(xdgSurf->popup));
                if (popup) {
                    // Find the parent surface's layerId
                    int parentLayerId = -1;
                    if (popup->parentSurface) {
                        WaylandSurface* parentWs =
                                surface->compositor->findSurface(popup->parentSurface);
                        if (parentWs) {
                            parentLayerId = static_cast<int>(parentWs->layerId);
                        }
                    }

                    int32_t scale = surface->compositor->outputScale();

                    // Use full buffer size (includes CSD shadow) so the
                    // semi-transparent shadow blends with the background.
                    int popW = popup->positioner.width * scale;
                    int popH = popup->positioner.height * scale;
                    if (surface->currentBuffer) {
                        auto* bufBase = static_cast<WaylandBufferBase*>(
                                wl_resource_get_user_data(surface->currentBuffer));
                        if (bufBase && bufBase->bufferType == WaylandBufferType::Dmabuf) {
                            popW = static_cast<WaylandDmabufBuffer*>(bufBase)->width;
                            popH = static_cast<WaylandDmabufBuffer*>(bufBase)->height;
                        } else if (bufBase && bufBase->bufferType == WaylandBufferType::Shm) {
                            popW = static_cast<WaylandShmBuffer*>(bufBase)->width;
                            popH = static_cast<WaylandShmBuffer*>(bufBase)->height;
                        }
                    }

                    // Popup position is in logical coords relative to parent.
                    // The popup window is sized to the full buffer (including
                    // shadow), so no geometry adjustment needed here.
                    surface->compositor->requestCreateWindow(
                            static_cast<int>(surface->layerId), surface->handle,
                            nullptr, nullptr,
                            parentLayerId, popW, popH,
                            popup->x * scale, popup->y * scale);
                }
            }
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
        ALOGE("[BUF] importBuffer: invalid plane count 0");
        return;
    }

    PixelFormat pixFmt = drmToPixelFormat(dmabuf->format);
    uint32_t w = static_cast<uint32_t>(dmabuf->width);
    uint32_t h = static_cast<uint32_t>(dmabuf->height);
    uint32_t stride = dmabuf->planes[0].stride;

    WL_LOGV("[BUF] importBuffer: layer=%u %ux%u stride=%u fmt=0x%08x planes=%u fd=%d mod=0x%" PRIx64,
          layerId, w, h, stride, dmabuf->format, numPlanes,
          dmabuf->planes[0].fd, dmabuf->planes[0].modifier);

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
            WL_LOGV("[BUF] importBuffer: cache HIT inode=%" PRIu64 " handle=%p layer=%u",
                  inodeKey, cached->handle, layerId);
        }
    }

    if (!cached) {
        WL_LOGV("[BUF] importBuffer: cache MISS inode=%" PRIu64 " → QTI gralloc import layer=%u",
              inodeKey, layerId);
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
                data[idx++] = static_cast<int>(pixelStride); // width (aligned)
                data[idx++] = static_cast<int>(h);          // height
                data[idx++] = static_cast<int>(pixelStride); // unaligned_width = pixelStride too
                data[idx++] = static_cast<int>(h);           // unaligned_height
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

                WL_LOGV("[BUF] importBuffer: importBufferNoValidate result=%d handle=%p layer=%u",
                      importErr, importedHandle, layerId);
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
                        importedBuffers[reinterpret_cast<uint64_t>(importedHandle)] = ih;
                        cached = &importedBuffers[reinterpret_cast<uint64_t>(importedHandle)];
                    }
                    WL_LOGV("dmabuf import: QTI gralloc imported %ux%u stride=%u pixStride=%u (inode %" PRIu64 ")",
                          w, h, stride, pixelStride, inodeKey);
                }
            }
            close(metaFd);
        }
    }

    if (!cached || !cached->handle) {
        ALOGE("[BUF] importBuffer: all strategies failed for %ux%u layer=%u", w, h, layerId);
        return;
    }

    WL_LOGV("[BUF] importBuffer: using cached handle=%p %ux%u pixStride=%u fmt=%d layer=%u",
          cached->handle, cached->width, cached->height, cached->pixelStride,
          cached->format, layerId);

    // Stamp a unique gralloc buffer ID into the cached handle before each
    // WRAP_HANDLE.  QTI's SDM display engine caches DRM framebuffer objects
    // by handle_id (the gralloc private_handle_t::id field).  If the id
    // stays the same across frames, SDE may serve stale tile data from its
    // internal cache.  Mutating the id forces SDE to create a fresh fb_id.
    {
        // private_handle_t::id is at int offset 11 (uint64_t spanning [11..12])
        // after: fd[0], fd_metadata[1], magic[2], flags[3], width[4], height[5],
        //        unaligned_width[6], unaligned_height[7], format[8],
        //        buffer_type[9], layer_count[10], id[11..12]
        constexpr int kIdIntOffset = 2 + 9; // 2 fds + 9 int fields = offset 11
        static std::atomic<uint64_t> sFrameId{1};
        uint64_t uniqueId = sFrameId++;
        auto* mutableHandle = const_cast<native_handle_t*>(cached->handle);
        memcpy(&mutableHandle->data[kIdIntOffset], &uniqueId, sizeof(uint64_t));
    }

    sp<GraphicBuffer> gb = sp<GraphicBuffer>::make(
            cached->handle, GraphicBuffer::WRAP_HANDLE,
            cached->width, cached->height, cached->format,
            1u, cached->usage, cached->pixelStride);

    WL_LOGV("[BUF] importBuffer: WRAP_HANDLE → gb=%p id=%" PRIu64 " initCheck=%d layer=%u",
          gb.get(), gb ? gb->getId() : 0, gb ? gb->initCheck() : -1, layerId);

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
            // Non-blocking check: is the fence already signaled?
            struct pollfd pfd = { .fd = exportSync.fd, .events = POLLIN };
            int pollRes = poll(&pfd, 1, 0);
            WL_LOGV("dmabuf fence: fd=%d status=%s (poll=%d revents=0x%x)",
                  exportSync.fd,
                  pollRes > 0 ? "ALREADY_SIGNALED" : "ACTIVE",
                  pollRes, pfd.revents);
            bw.acquireFence = sp<Fence>::make(exportSync.fd);
        } else {
            ALOGW("dmabuf fence: EXPORT_SYNC_FILE failed (errno=%d: %s)",
                  errno, strerror(errno));
        }
    }

    // DEBUG: force CPU-copy path for dmabuf to isolate gralloc handle issues.
    // mmap the dmabuf, wait for GPU fence, copy pixels into the BufferWork
    // pixel vector (like SHM), and let the buffer thread allocate a fresh
    // gralloc buffer. If display corruption vanishes, the zero-copy handle
    // import is the problem.
    {
        WL_LOGV("[BUF] importBuffer: CPU-copy fallback: waiting for fence layer=%u frame=%" PRIu64,
              layerId, frameNumber);
        if (bw.acquireFence && bw.acquireFence->isValid()) {
            bw.acquireFence->waitForever("dmabuf_cpu_copy_wait");
            WL_LOGV("[BUF] importBuffer: fence wait done layer=%u", layerId);
        }
        size_t mapSize = static_cast<size_t>(stride) * h;
        void* mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED,
                            dmabuf->planes[0].fd, 0);
        WL_LOGV("[BUF] importBuffer: mmap fd=%d size=%zu result=%p layer=%u",
              dmabuf->planes[0].fd, mapSize, mapped, layerId);
        if (mapped != MAP_FAILED) {
            bw.pixels.resize(mapSize);
            memcpy(bw.pixels.data(), mapped, mapSize);
            munmap(mapped, mapSize);
            // Switch to SHM-style path: clear the GraphicBuffer so buffer
            // thread allocates a fresh one and does the pixel copy.
            bw.gb = nullptr;
            // Force RGBA_8888 for the gralloc alloc (SwiftShader can't
            // texture BGRA_8888).  The BGRA→RGBA swizzle in the buffer
            // thread converts the dmabuf's [B,G,R,A] memory layout to
            // the [R,G,B,A] layout that RGBA_8888 expects.
            bw.pixFmt = PIXEL_FORMAT_RGBA_8888;
            bw.srcStride = stride;
            bw.skipSwizzle = false;
            bw.acquireFence = nullptr; // already waited
            // Close the dmabuf fd — we don't need zero-copy tracking.
            if (bw.dmabufFd >= 0) {
                close(bw.dmabufFd);
                bw.dmabufFd = -1;
            }
            bw.wlBuffer = nullptr; // use time-based release instead
            WL_LOGV("dmabuf CPU-copy: %ux%u stride=%u → SHM-style path", w, h, stride);
        } else {
            ALOGE("dmabuf CPU-copy: mmap failed: %s", strerror(errno));
        }
    }

    WL_LOGV("[BUF] importBuffer: posting BufferWork layer=%u frame=%" PRIu64 " hasPix=%d hasGb=%d w=%d h=%d",
          layerId, frameNumber, !bw.pixels.empty(), bw.gb != nullptr, bw.width, bw.height);
    compositor->postBufferWork(std::move(bw));
}

void WaylandSurface::importShmBuffer(WaylandShmBuffer* shm) {
    WL_LOGV("[BUF] importShmBuffer: layer=%u %dx%d stride=%d fmt=0x%08x offset=%d",
          layerId, shm->width, shm->height, shm->stride, shm->format, shm->offset);

    if (!shm->pool || !shm->pool->data) {
        ALOGE("[BUF] importShmBuffer: no pool or pool data layer=%u", layerId);
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
    WL_LOGV("[BUF] importShmBuffer: posting BufferWork layer=%u frame=%" PRIu64 " dataSize=%zu",
          layerId, frameNumber, work.pixels.size());
    compositor->postBufferWork(std::move(work));
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
        WL_LOGV("[BUF] onDestroy: layer=%u currentBuffer=%p importedBuffers=%zu",
              surface->layerId, surface->currentBuffer, surface->importedBuffers.size());
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
