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
#include <utils/Looper.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ui/GraphicBuffer.h>

#include "WaylandDmabuf.h"
#include "WaylandDrm.h"
#include "WaylandOutput.h"
#include "WaylandSeat.h"
#include "WaylandShm.h"
#include "WaylandSurface.h"
#include "WaylandXdgShell.h"

namespace android {

class SurfaceFlinger;

class WaylandCompositor {
public:
    static std::unique_ptr<WaylandCompositor> create(SurfaceFlinger& flinger,
                                                      const sp<Looper>& looper);
    ~WaylandCompositor();

    WaylandCompositor(const WaylandCompositor&) = delete;
    WaylandCompositor& operator=(const WaylandCompositor&) = delete;

    SurfaceFlinger& flinger() { return mFlinger; }
    WaylandSeat* seat() { return mSeat.get(); }
    struct wl_display* display() { return mDisplay; }
    void removeSurface(struct wl_resource* resource);

    // Look up a WaylandSurface by its wl_surface resource.
    WaylandSurface* findSurface(struct wl_resource* wlSurface);

    // Look up a WaylandSurface by its SF layer ID.
    WaylandSurface* findSurfaceByLayerId(int layerId);

    // Reparent a child surface layer under a parent surface layer.
    void reparentSurfaceUnder(WaylandSurface* child, WaylandSurface* parent);

    // Set position of a surface layer.
    void setSurfacePosition(WaylandSurface* surface, int32_t x, int32_t y);

    // Send xdg_toplevel configure with a new size.
    void sendToplevelConfigure(int layerId, int32_t width, int32_t height);

    // Request the Android WaylandWindowService to create/destroy a window.
    void requestCreateWindow(int layerId, const sp<IBinder>& layerHandle,
                             const char* title, const char* appId,
                             int width, int height);
    void requestDestroyWindow(int layerId);
    void reparentLayerUnderWindow(int layerId, const sp<IBinder>& windowHandle);

    // Queue frame callbacks to be fired after the next composite cycle.
    void queueFrameCallbacks(std::vector<struct wl_resource*>&& callbacks);
    // Dispatch pending Wayland events. Called from SF composite cycle.
    void dispatchEvents();

    // Fire all queued frame callbacks. Called from SF::composite() post-composition.
    void fireFrameCallbacks(uint32_t vsyncTimeMs);

    // Signal SF that a new buffer was committed and composition is needed.
    void scheduleComposite();

    // Queue a buffer for deferred release after the next composite cycle.
    void queueBufferRelease(struct wl_resource* buffer);
    // Release all queued buffers. Called from SF::composite() post-composition.
    void fireBufferReleases();

    // Called when a wl_buffer resource is destroyed. Clears dangling references
    // in surfaces and pending release queue to prevent use-after-free.
    void notifyBufferDestroyed(struct wl_resource* buffer);

    // Input dispatch — called from binder thread.
    // layerId identifies which Wayland surface to target.
    void dispatchPointerMotion(int layerId, uint32_t timeMs, double x, double y);
    void dispatchPointerButton(int layerId, uint32_t timeMs, uint32_t button, bool pressed);
    void dispatchKey(int layerId, uint32_t timeMs, uint32_t evdevKey, bool pressed);

private:
    explicit WaylandCompositor(SurfaceFlinger& flinger);

    bool init(const sp<Looper>& looper);
    static int onWaylandEvent(int fd, int events, void* data);

    // wl_compositor global
    static void bindCompositor(struct wl_client* client, void* data,
                               uint32_t version, uint32_t id);
    static void compositorCreateSurface(struct wl_client* client,
                                        struct wl_resource* resource,
                                        uint32_t id);
    static void compositorCreateRegion(struct wl_client* client,
                                       struct wl_resource* resource,
                                       uint32_t id);

    static const struct wl_compositor_interface kCompositorImpl;

    SurfaceFlinger& mFlinger;
    struct wl_listener mClientCreatedListener = {};
    struct wl_display* mDisplay = nullptr;
    struct wl_event_loop* mEventLoop = nullptr;
    int mEventLoopFd = -1;
    uint32_t mNextSurfaceNum = 0;

    std::unique_ptr<WaylandSeat> mSeat;

    // Keyed by wl_resource* of the wl_surface
    std::unordered_map<struct wl_resource*, std::unique_ptr<WaylandSurface>> mSurfaces;

    // Frame callbacks queued by wl_surface.commit, fired after composite.
    std::mutex mCallbacksMutex;
    std::vector<struct wl_resource*> mPendingFrameCallbacks GUARDED_BY(mCallbacksMutex);

    // Two-stage buffer release: buffers are first added to mPendingBufferReleases,
    // then on the next composite cycle moved to mReadyBufferReleases, then on the
    // NEXT composite cycle actually released. This 2-frame delay ensures HWC has
    // fully finished scanning out the buffer before the client reuses it.
    std::vector<struct wl_resource*> mPendingBufferReleases GUARDED_BY(mCallbacksMutex);
    std::vector<struct wl_resource*> mReadyBufferReleases GUARDED_BY(mCallbacksMutex);
    uint32_t mPendingVsyncTimeMs GUARDED_BY(mCallbacksMutex) = 0;

    // Pending configure events (queued from binder thread, dispatched on Wayland thread).
    // Pending events queued from other threads, dispatched on Wayland thread.
    struct PendingEvent {
        enum Type { Configure, PointerMotion, PointerButton, Key };
        Type type;
        int layerId;
        int32_t i1, i2, i3; // generic int args
        float f1, f2;       // generic float args
        bool b1;
    };
    std::vector<PendingEvent> mPendingEvents GUARDED_BY(mCallbacksMutex);

    // Background dispatch thread for Wayland protocol events.
    std::thread mDispatchThread;
    std::atomic<bool> mDispatchStop{false};
    int mWakeEventFd = -1;

    // Actually fire frame callbacks/releases (called on Wayland thread).
    void doFireFrameCallbacksAndReleases();

    // --- Buffer submission thread ---
    // Buffer imports (gralloc alloc + pixel copy + setTransactionState) are posted
    // to a dedicated thread to avoid blocking the Wayland dispatch thread, which
    // would cause deadlocks with Vulkan WSI clients that do nested event dispatch.
public:
    struct BufferWork {
        sp<IBinder> handle;
        sp<GraphicBuffer> gb; // pre-built GraphicBuffer (dmabuf path)
        std::vector<uint8_t> pixels; // raw pixel data (SHM path, empty for dmabuf)
        PixelFormat pixFmt;
        uint32_t srcStride;
        uint64_t frameNumber;
        uint32_t producerId;
        uint32_t layerId;
        int32_t width;
        int32_t height;
        int dmabufFd = -1; // dup'd dmabuf fd for sync (buffer thread will close)
    };
    void postBufferWork(BufferWork&& work);

private:
    std::thread mBufferThread;
    std::mutex mBufferMutex;
    std::condition_variable mBufferCv;
    std::vector<BufferWork> mBufferQueue; // protected by mBufferMutex
    void bufferThreadLoop();
};

} // namespace android
