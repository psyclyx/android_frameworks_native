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

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "WaylandDmabuf.h"
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
    void removeSurface(struct wl_resource* resource);

    // Queue frame callbacks to be fired after the next composite cycle.
    void queueFrameCallbacks(std::vector<struct wl_resource*>&& callbacks);
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
    struct wl_display* mDisplay = nullptr;
    struct wl_event_loop* mEventLoop = nullptr;
    int mEventLoopFd = -1;
    uint32_t mNextSurfaceNum = 0;

    // Keyed by wl_resource* of the wl_surface
    std::unordered_map<struct wl_resource*, std::unique_ptr<WaylandSurface>> mSurfaces;

    // Frame callbacks queued by wl_surface.commit, fired after composite.
    std::mutex mCallbacksMutex;
    std::vector<struct wl_resource*> mPendingFrameCallbacks GUARDED_BY(mCallbacksMutex);

    // Buffers queued for release after composite (previous buffers replaced during commit).
    std::vector<struct wl_resource*> mPendingBufferReleases GUARDED_BY(mCallbacksMutex);
};

} // namespace android
