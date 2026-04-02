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
#define LOG_TAG "WaylandCompositor"

#include "WaylandCompositor.h"

#include <algorithm>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/gui/ISurfaceComposerClient.h>
#include <gui/LayerMetadata.h>
#include <log/log.h>
#include <wayland-server-protocol.h>

#include <gui/LayerState.h>
#include <gui/TransactionState.h>

#include "FrontEnd/LayerCreationArgs.h"
#include "FrontEnd/LayerHandle.h"
#include "Layer.h"
#include "SurfaceFlinger.h"

namespace android {

using gui::ISurfaceComposerClient;

namespace {

constexpr const char* kSocketDir = "/data/wayland";
constexpr const char* kSocketName = "wayland-0";
constexpr uint32_t kCompositorVersion = 4; // wl_surface up to damage_buffer (v4)

bool ensureSocketDir() {
    struct stat st;
    if (stat(kSocketDir, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(kSocketDir, 0755) != 0) {
        ALOGE("Failed to create %s: %s", kSocketDir, strerror(errno));
        return false;
    }
    return true;
}

// wl_region stub — we accept create_region but the resource does nothing.
void regionDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
void regionAdd(struct wl_client* /*client*/, struct wl_resource* /*resource*/, int32_t /*x*/,
               int32_t /*y*/, int32_t /*width*/, int32_t /*height*/) {}
void regionSubtract(struct wl_client* /*client*/, struct wl_resource* /*resource*/, int32_t /*x*/,
                    int32_t /*y*/, int32_t /*width*/, int32_t /*height*/) {}

const struct wl_region_interface kRegionImpl = {
        .destroy = regionDestroy,
        .add = regionAdd,
        .subtract = regionSubtract,
};

} // anonymous namespace

// wl_compositor_interface
const struct wl_compositor_interface WaylandCompositor::kCompositorImpl = {
        .create_surface = WaylandCompositor::compositorCreateSurface,
        .create_region = WaylandCompositor::compositorCreateRegion,
};

WaylandCompositor::WaylandCompositor(SurfaceFlinger& flinger)
      : mFlinger(flinger) {}

WaylandCompositor::~WaylandCompositor() {
    mSurfaces.clear();
    if (mDisplay) {
        wl_display_destroy(mDisplay);
    }
}

std::unique_ptr<WaylandCompositor> WaylandCompositor::create(SurfaceFlinger& flinger,
                                                              const sp<Looper>& looper) {
    auto compositor = std::unique_ptr<WaylandCompositor>(new WaylandCompositor(flinger));
    if (!compositor->init(looper)) {
        return nullptr;
    }
    return compositor;
}

bool WaylandCompositor::init(const sp<Looper>& looper) {
    if (!ensureSocketDir()) {
        return false;
    }

    mDisplay = wl_display_create();
    if (!mDisplay) {
        ALOGE("wl_display_create failed");
        return false;
    }

    // Remove any stale socket before binding.
    std::string socketPath = std::string(kSocketDir) + "/" + kSocketName;
    unlink(socketPath.c_str());

    if (wl_display_add_socket(mDisplay, socketPath.c_str()) != 0) {
        ALOGE("Failed to add Wayland socket at %s: %s", socketPath.c_str(), strerror(errno));
        return false;
    }

    // Allow other users (e.g. shell for testing) to connect to the socket.
    if (chmod(socketPath.c_str(), 0666) != 0) {
        ALOGW("Failed to chmod Wayland socket: %s", strerror(errno));
    }

    // Register wl_compositor global.
    if (!wl_global_create(mDisplay, &wl_compositor_interface, kCompositorVersion,
                          this, bindCompositor)) {
        ALOGE("Failed to create wl_compositor global");
        return false;
    }

    // Register zwp_linux_dmabuf_v1 global.
    if (!WaylandDmabuf::createGlobal(mDisplay, this)) {
        ALOGE("Failed to create zwp_linux_dmabuf_v1 global");
        return false;
    }

    // Register xdg_wm_base global.
    if (!WaylandXdgShell::createGlobal(mDisplay, this)) {
        ALOGE("Failed to create xdg_wm_base global");
        return false;
    }

    // Register wl_output global.
    if (!WaylandOutput::createGlobal(mDisplay, &mFlinger)) {
        ALOGE("Failed to create wl_output global");
        return false;
    }

    // Register wl_shm global.
    if (!WaylandShm::createGlobal(mDisplay, this)) {
        ALOGE("Failed to create wl_shm global");
        return false;
    }

    // Register wl_seat global (stub — no input events).
    if (!WaylandSeat::createGlobal(mDisplay)) {
        ALOGE("Failed to create wl_seat global");
        return false;
    }

    mEventLoop = wl_display_get_event_loop(mDisplay);
    mEventLoopFd = wl_event_loop_get_fd(mEventLoop);

    int ret = looper->addFd(mEventLoopFd, Looper::POLL_CALLBACK, Looper::EVENT_INPUT,
                            onWaylandEvent, this);
    if (ret != 1) {
        ALOGE("Failed to add Wayland FD to Looper");
        return false;
    }

    ALOGI("Wayland compositor listening on %s (wl_compositor v%u)", socketPath.c_str(),
          kCompositorVersion);
    return true;
}

int WaylandCompositor::onWaylandEvent(int /*fd*/, int /*events*/, void* data) {
    auto* self = static_cast<WaylandCompositor*>(data);
    wl_event_loop_dispatch(self->mEventLoop, 0);
    wl_display_flush_clients(self->mDisplay);
    return 1; // Keep listening.
}

void WaylandCompositor::bindCompositor(struct wl_client* client, void* data,
                                       uint32_t version, uint32_t id) {
    auto* self = static_cast<WaylandCompositor*>(data);
    int ver = static_cast<int>(std::min(version, kCompositorVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &wl_compositor_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kCompositorImpl, self, nullptr);
    ALOGI("wl_compositor bound (v%u)", ver);
}

void WaylandCompositor::compositorCreateSurface(struct wl_client* client,
                                                 struct wl_resource* resource,
                                                 uint32_t id) {
    auto* self = static_cast<WaylandCompositor*>(wl_resource_get_user_data(resource));
    uint32_t surfaceNum = self->mNextSurfaceNum++;

    // Build a unique name for this Wayland surface.
    pid_t clientPid;
    wl_client_get_credentials(client, &clientPid, nullptr, nullptr);
    std::string layerName = "wayland:" + std::to_string(clientPid) + ":" +
                            std::to_string(surfaceNum);

    // Create SF Layer via createLayer().
    LayerCreationArgs args(&self->mFlinger, nullptr /*client*/, layerName,
                           ISurfaceComposerClient::eFXSurfaceBufferState |
                           ISurfaceComposerClient::eNoColorFill,
                           gui::LayerMetadata());
    args.addToRoot = true;

    gui::CreateSurfaceResult result;
    status_t err = self->mFlinger.createLayer(args, result);
    if (err != NO_ERROR) {
        ALOGE("Failed to create SF layer for Wayland surface '%s': %d", layerName.c_str(), err);
        wl_resource_post_no_memory(resource);
        return;
    }

    // Place the Wayland layer above normal app layers so it's visible.
    {
        TransactionState txn;
        ComposerState cs;
        cs.state.what = layer_state_t::eLayerChanged;
        cs.state.surface = result.handle;
        cs.state.z = 0x7FFFFFFE; // just below max, above all apps
        txn.mComposerStates.push_back(cs);
        txn.mId = static_cast<uint64_t>(surfaceNum) | (1ULL << 48);
        self->mFlinger.setTransactionState(std::move(txn), /*applyToken=*/nullptr);
    }

    // Create WaylandSurface state.
    auto surface = std::make_unique<WaylandSurface>();
    surface->compositor = self;
    surface->handle = result.handle;
    surface->layerId = static_cast<uint32_t>(result.layerId);
    surface->layer = LayerHandle::getLayer(result.handle);
    surface->producerId = surfaceNum + 1; // non-zero unique ID per surface

    // Create the wl_surface resource.
    int ver = wl_resource_get_version(resource);
    struct wl_resource* surfaceResource =
            wl_resource_create(client, &wl_surface_interface, ver, id);
    if (!surfaceResource) {
        // unique_ptr destruction drops sp<IBinder> handle, triggering
        // ~LayerHandle() → onHandleDestroyed() to clean up the SF layer.
        wl_resource_post_no_memory(resource);
        return;
    }

    surface->resource = surfaceResource;
    WaylandSurface* surfacePtr = surface.get();
    self->mSurfaces[surfaceResource] = std::move(surface);

    wl_resource_set_implementation(surfaceResource, &WaylandSurface::kImpl,
                                   surfacePtr, WaylandSurface::onDestroy);

    ALOGI("Created Wayland surface '%s' (layer %d)", layerName.c_str(), result.layerId);
}

void WaylandCompositor::compositorCreateRegion(struct wl_client* client,
                                                struct wl_resource* resource,
                                                uint32_t id) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* regionResource =
            wl_resource_create(client, &wl_region_interface, ver, id);
    if (!regionResource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(regionResource, &kRegionImpl, nullptr, nullptr);
}

void WaylandCompositor::removeSurface(struct wl_resource* resource) {
    auto it = mSurfaces.find(resource);
    if (it != mSurfaces.end()) {
        uint32_t layerId = it->second->layerId;
        mSurfaces.erase(it);
        // Erasing destroys WaylandSurface, which drops sp<IBinder> handle.
        // ~LayerHandle() calls onHandleDestroyed() to remove the Layer from SF.
        ALOGI("Removed Wayland surface and SF layer %u", layerId);
    }
}

void WaylandCompositor::scheduleComposite() {
    mFlinger.scheduleComposite(SurfaceFlinger::FrameHint::kActive);
}

void WaylandCompositor::queueFrameCallbacks(std::vector<struct wl_resource*>&& callbacks) {
    std::lock_guard<std::mutex> lock(mCallbacksMutex);
    mPendingFrameCallbacks.insert(mPendingFrameCallbacks.end(),
                                  std::make_move_iterator(callbacks.begin()),
                                  std::make_move_iterator(callbacks.end()));
}

void WaylandCompositor::fireFrameCallbacks(uint32_t vsyncTimeMs) {
    std::vector<struct wl_resource*> callbacks;
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        if (mPendingFrameCallbacks.empty()) return;
        callbacks = std::move(mPendingFrameCallbacks);
        mPendingFrameCallbacks.clear();
    }

    for (auto* cb : callbacks) {
        wl_callback_send_done(cb, vsyncTimeMs);
        wl_resource_destroy(cb);
    }

    wl_display_flush_clients(mDisplay);
}

void WaylandCompositor::queueBufferRelease(struct wl_resource* buffer) {
    std::lock_guard<std::mutex> lock(mCallbacksMutex);
    mPendingBufferReleases.push_back(buffer);
}

void WaylandCompositor::fireBufferReleases() {
    std::vector<struct wl_resource*> buffers;
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        if (mPendingBufferReleases.empty()) return;
        buffers = std::move(mPendingBufferReleases);
        mPendingBufferReleases.clear();
    }

    for (auto* buf : buffers) {
        wl_buffer_send_release(buf);
    }

    wl_display_flush_clients(mDisplay);
}

void WaylandCompositor::notifyBufferDestroyed(struct wl_resource* buffer) {
    // Clear dangling currentBuffer/pendingBuffer pointers on all surfaces.
    for (auto& [res, surface] : mSurfaces) {
        if (surface->currentBuffer == buffer) {
            surface->currentBuffer = nullptr;
        }
        if (surface->pendingBuffer == buffer) {
            surface->pendingBuffer = nullptr;
        }
    }

    // Remove from pending release queue (under lock since fireBufferReleases
    // may be called from the SF composite thread).
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        auto it = std::remove(mPendingBufferReleases.begin(), mPendingBufferReleases.end(), buffer);
        mPendingBufferReleases.erase(it, mPendingBufferReleases.end());
    }
}

} // namespace android
