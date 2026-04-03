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
#include <cstdarg>
#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/gui/ISurfaceComposerClient.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <gui/LayerMetadata.h>
#include <log/log.h>
#include <wayland-server-protocol.h>
#include <xdg-shell-server-protocol.h>

#include <gui/LayerState.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/TransactionState.h>

#include "WaylandDataDevice.h"
#include "WaylandSubcompositor.h"

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

// Forward libwayland server-side log messages to Android logcat.
// This captures protocol errors that libwayland generates before disconnecting clients.
void waylandLogHandler(const char* fmt, va_list args) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    ALOGE("libwayland: %s", buf);
}

// Listener attached to each client to log when they disconnect.
struct ClientDestroyData {
    struct wl_listener listener;
    pid_t pid;
};

void onClientDestroy(struct wl_listener* listener, void* /*data*/) {
    ClientDestroyData* d;
    d = wl_container_of(listener, d, listener);
    ALOGI("Wayland client pid=%d disconnected", d->pid);
    wl_list_remove(&d->listener.link);
    delete d;
}

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
      : mFlinger(flinger) {
    mClientCreatedListener.notify = [](struct wl_listener* /*listener*/, void* data) {
        auto* client = static_cast<struct wl_client*>(data);
        pid_t pid;
        wl_client_get_credentials(client, &pid, nullptr, nullptr);
        ALOGI("Wayland client connected: pid=%d", pid);

        auto* d = new ClientDestroyData();
        d->pid = pid;
        d->listener.notify = onClientDestroy;
        wl_client_add_destroy_listener(client, &d->listener);
    };
}

WaylandCompositor::~WaylandCompositor() {
    mDispatchStop = true;
    if (mDisplay) {
        wl_display_terminate(mDisplay);
    }
    if (mDispatchThread.joinable()) {
        mDispatchThread.join();
    }
    // Wake and join the buffer thread.
    mBufferCv.notify_all();
    if (mBufferThread.joinable()) {
        mBufferThread.join();
    }
    mSurfaces.clear();
    if (mWakeEventFd >= 0) {
        close(mWakeEventFd);
    }
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

bool WaylandCompositor::init(const sp<Looper>& /*looper*/) {
    if (!ensureSocketDir()) {
        return false;
    }

    // Capture libwayland protocol errors in logcat.
    wl_log_set_handler_server(waylandLogHandler);

    mDisplay = wl_display_create();
    if (!mDisplay) {
        ALOGE("wl_display_create failed");
        return false;
    }

    // Log when new clients connect and attach a destroy listener.
    wl_display_add_client_created_listener(mDisplay, &mClientCreatedListener);

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

    // Register wl_drm global (Mesa EGL uses this to discover the DRM render node).
    if (!WaylandDrm::createGlobal(mDisplay, this)) {
        ALOGE("Failed to create wl_drm global");
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

    // Register wl_seat global (with real input dispatch).
    mSeat = std::make_unique<WaylandSeat>(this);
    if (!mSeat->createGlobal(mDisplay)) {
        ALOGE("Failed to create wl_seat global");
        return false;
    }

    // Register wl_data_device_manager global (stub for clipboard/DnD).
    if (!WaylandDataDevice::createGlobal(mDisplay)) {
        ALOGE("Failed to create wl_data_device_manager global");
        return false;
    }

    // Register wl_subcompositor global.
    if (!WaylandSubcompositor::createGlobal(mDisplay, this)) {
        ALOGE("Failed to create wl_subcompositor global");
        return false;
    }

    mEventLoop = wl_display_get_event_loop(mDisplay);
    mEventLoopFd = wl_event_loop_get_fd(mEventLoop);

    // Run Wayland event dispatch on a dedicated thread using wl_display_run(),
    // which is the canonical libwayland event loop. This ensures all protocol
    // events are dispatched promptly regardless of SurfaceFlinger's vsync state.
    // Register the Wayland event loop fd with SF's Looper for event-driven dispatch.
    // Also register a periodic timer to ensure we don't miss events.
    mWakeEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (mWakeEventFd < 0) {
        ALOGE("eventfd creation failed: %s", strerror(errno));
        return false;
    }
    // Register the eventfd with the Wayland event loop so the dispatch thread
    // wakes up when other threads queue work (frame callbacks, configures, etc.).
    wl_event_loop_add_fd(mEventLoop, mWakeEventFd, WL_EVENT_READABLE,
            [](int fd, uint32_t /*mask*/, void* data) -> int {
                auto* self = static_cast<WaylandCompositor*>(data);
                uint64_t val;
                read(fd, &val, sizeof(val));
                self->doFireFrameCallbacksAndReleases();
                return 0;
            }, this);

    // Run a dedicated dispatch thread that uses the standard wl_display_run loop.
    // This is the ONLY thread that touches libwayland APIs.
    mDispatchThread = std::thread([this]() {
        while (!mDispatchStop) {
            wl_display_flush_clients(mDisplay);
            // Short timeout to allow periodic flushing and checking mDispatchStop.
            wl_event_loop_dispatch(mEventLoop, 16);
        }
    });

    // Buffer submission thread — handles gralloc alloc + setTransactionState
    // off the Wayland dispatch thread to avoid deadlocking with SF or Vulkan WSI.
    mBufferThread = std::thread([this]() { bufferThreadLoop(); });

    ALOGI("Wayland compositor listening on %s (wl_compositor v%u)", socketPath.c_str(),
          kCompositorVersion);
    return true;
}

int WaylandCompositor::onWaylandEvent(int /*fd*/, int /*events*/, void* data) {
    auto* self = static_cast<WaylandCompositor*>(data);
    wl_event_loop_dispatch(self->mEventLoop, 0);
    wl_display_flush_clients(self->mDisplay);
    self->scheduleComposite();
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

WaylandSurface* WaylandCompositor::findSurface(struct wl_resource* wlSurface) {
    auto it = mSurfaces.find(wlSurface);
    return (it != mSurfaces.end()) ? it->second.get() : nullptr;
}

WaylandSurface* WaylandCompositor::findSurfaceByLayerId(int layerId) {
    for (auto& [res, surface] : mSurfaces) {
        if (static_cast<int>(surface->layerId) == layerId) {
            return surface.get();
        }
    }
    return nullptr;
}

// AIDL transaction codes for IWaylandWindowManager (must match the generated AIDL stub).
// These correspond to the methods in order: createWindow=1, destroyWindow=2, setTitle=3.
enum {
    TRANSACTION_createWindow = ::android::IBinder::FIRST_CALL_TRANSACTION + 0,
    TRANSACTION_destroyWindow = ::android::IBinder::FIRST_CALL_TRANSACTION + 1,
    TRANSACTION_setTitle = ::android::IBinder::FIRST_CALL_TRANSACTION + 2,
};

enum {
    CB_TRANSACTION_onWindowReady = ::android::IBinder::FIRST_CALL_TRANSACTION + 0,
    CB_TRANSACTION_onWindowClosed = ::android::IBinder::FIRST_CALL_TRANSACTION + 1,
    CB_TRANSACTION_onWindowResized = ::android::IBinder::FIRST_CALL_TRANSACTION + 2,
    CB_TRANSACTION_onPointerMotion = ::android::IBinder::FIRST_CALL_TRANSACTION + 3,
    CB_TRANSACTION_onPointerButton = ::android::IBinder::FIRST_CALL_TRANSACTION + 4,
    CB_TRANSACTION_onKey = ::android::IBinder::FIRST_CALL_TRANSACTION + 5,
};

// Native binder callback that receives the Activity's window handle
// and reparents the Wayland layer under it.
class WaylandWindowCallback : public BBinder {
public:
    WaylandWindowCallback(WaylandCompositor* compositor) : mCompositor(compositor) {}

    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                        uint32_t flags) override {
        switch (code) {
            case CB_TRANSACTION_onWindowReady: {
                // Match Java AIDL: enforceInterface, readInt(layerId), readStrongBinder(handle)
                data.enforceInterface(
                        String16("org.lineageos.wayland.IWaylandWindowCallback"));
                int32_t layerId = data.readInt32();
                sp<IBinder> windowHandle = data.readStrongBinder();

                ALOGI("onWindowReady: layerId=%d windowHandle=%p",
                      layerId, windowHandle.get());

                if (windowHandle && mCompositor) {
                    mCompositor->reparentLayerUnderWindow(layerId, windowHandle);
                }

                if (reply) reply->writeNoException();
                return NO_ERROR;
            }
            case CB_TRANSACTION_onWindowClosed: {
                data.enforceInterface(
                        String16("org.lineageos.wayland.IWaylandWindowCallback"));
                int32_t layerId = data.readInt32();
                ALOGI("onWindowClosed: layerId=%d", layerId);
                if (reply) reply->writeNoException();
                return NO_ERROR;
            }
            case CB_TRANSACTION_onWindowResized: {
                data.enforceInterface(
                        String16("org.lineageos.wayland.IWaylandWindowCallback"));
                int32_t layerId = data.readInt32();
                int32_t width = data.readInt32();
                int32_t height = data.readInt32();
                ALOGI("onWindowResized: layerId=%d size=%dx%d", layerId, width, height);
                if (mCompositor) {
                    mCompositor->sendToplevelConfigure(layerId, width, height);
                }
                if (reply) reply->writeNoException();
                return NO_ERROR;
            }
            case CB_TRANSACTION_onPointerMotion: {
                data.enforceInterface(
                        String16("org.lineageos.wayland.IWaylandWindowCallback"));
                int32_t layerId = data.readInt32();
                int64_t timeMs = data.readInt64();
                float x = data.readFloat();
                float y = data.readFloat();
                if (mCompositor) {
                    mCompositor->dispatchPointerMotion(layerId,
                            static_cast<uint32_t>(timeMs), x, y);
                }
                if (reply) reply->writeNoException();
                return NO_ERROR;
            }
            case CB_TRANSACTION_onPointerButton: {
                data.enforceInterface(
                        String16("org.lineageos.wayland.IWaylandWindowCallback"));
                int32_t layerId = data.readInt32();
                int64_t timeMs = data.readInt64();
                int32_t button = data.readInt32();
                bool pressed = data.readBool();
                if (mCompositor) {
                    mCompositor->dispatchPointerButton(layerId,
                            static_cast<uint32_t>(timeMs),
                            static_cast<uint32_t>(button), pressed);
                }
                if (reply) reply->writeNoException();
                return NO_ERROR;
            }
            case CB_TRANSACTION_onKey: {
                data.enforceInterface(
                        String16("org.lineageos.wayland.IWaylandWindowCallback"));
                int32_t layerId = data.readInt32();
                int64_t timeMs = data.readInt64();
                int32_t evdevKey = data.readInt32();
                bool pressed = data.readBool();
                if (mCompositor) {
                    mCompositor->dispatchKey(layerId,
                            static_cast<uint32_t>(timeMs),
                            static_cast<uint32_t>(evdevKey), pressed);
                }
                if (reply) reply->writeNoException();
                return NO_ERROR;
            }
            default:
                return BBinder::onTransact(code, data, reply, flags);
        }
    }

private:
    WaylandCompositor* mCompositor;
};

void WaylandCompositor::requestCreateWindow(int layerId, const sp<IBinder>& /*layerHandle*/,
                                             const char* title, const char* appId,
                                             int width, int height) {
    sp<IServiceManager> sm = defaultServiceManager();
    if (!sm) {
        ALOGE("requestCreateWindow: no service manager");
        return;
    }
    sp<IBinder> service = sm->checkService(String16("wayland_window_manager"));
    if (!service) {
        ALOGW("wayland_window_manager service not found, window will not be managed");
        return;
    }
    ALOGI("requestCreateWindow: found service, calling createWindow for layer %d", layerId);

    Parcel data, reply;
    data.writeInterfaceToken(String16("org.lineageos.wayland.IWaylandWindowManager"));
    data.writeInt32(layerId);
    data.writeString16(title ? String16(title) : String16());
    data.writeString16(appId ? String16(appId) : String16());
    data.writeInt32(width);
    data.writeInt32(height);
    sp<WaylandWindowCallback> callback = sp<WaylandWindowCallback>::make(this);
    data.writeStrongBinder(callback);

    status_t err = service->transact(TRANSACTION_createWindow, data, &reply);
    if (err != NO_ERROR) {
        ALOGE("Failed to call createWindow on wayland_window_manager: %d", err);
    } else {
        int32_t exceptionCode = reply.readExceptionCode();
        if (exceptionCode != 0) {
            ALOGE("createWindow threw exception: %d", exceptionCode);
        }
    }
}

void WaylandCompositor::requestDestroyWindow(int layerId) {
    sp<IBinder> service = defaultServiceManager()->checkService(
            String16("wayland_window_manager"));
    if (!service) return;

    Parcel data, reply;
    data.writeInterfaceToken(String16("org.lineageos.wayland.IWaylandWindowManager"));
    data.writeInt32(layerId);
    service->transact(TRANSACTION_destroyWindow, data, &reply);
}

void WaylandCompositor::reparentLayerUnderWindow(int layerId, const sp<IBinder>& windowHandle) {
    // Find the Wayland surface with this layerId
    sp<IBinder> waylandHandle;
    for (auto& [res, surface] : mSurfaces) {
        if (static_cast<int>(surface->layerId) == layerId) {
            waylandHandle = surface->handle;
            break;
        }
    }
    if (!waylandHandle) {
        ALOGW("reparentLayerUnderWindow: no surface with layerId %d", layerId);
        return;
    }

    // Reparent the Wayland layer under the Activity's window layer
    // and reset its z-order to 0 (the Activity manages positioning).
    sp<SurfaceControl> parentSc = sp<SurfaceControl>::make(
            SurfaceComposerClient::getDefault(), windowHandle,
            /*layerId=*/0, "wayland-window-parent");

    TransactionState txn;
    ComposerState cs;
    cs.state.surface = waylandHandle;
    cs.state.updateParentLayer(parentSc);
    cs.state.what |= layer_state_t::eLayerChanged;
    cs.state.z = 0;
    txn.mComposerStates.push_back(std::move(cs));
    txn.mId = static_cast<uint64_t>(layerId) | (2ULL << 48);
    mFlinger.setTransactionState(std::move(txn), /*applyToken=*/nullptr);

    ALOGI("Reparented Wayland layer %d under window %p", layerId, windowHandle.get());
}

void WaylandCompositor::reparentSurfaceUnder(WaylandSurface* child, WaylandSurface* parent) {
    if (!child || !parent || !child->handle || !parent->handle) return;

    sp<SurfaceControl> parentSc = sp<SurfaceControl>::make(
            SurfaceComposerClient::getDefault(), parent->handle,
            0, "wayland-subsurface-parent");

    TransactionState txn;
    ComposerState cs;
    cs.state.surface = child->handle;
    cs.state.updateParentLayer(parentSc);
    cs.state.what |= layer_state_t::eLayerChanged;
    cs.state.z = 1;
    txn.mComposerStates.push_back(std::move(cs));
    txn.mId = (static_cast<uint64_t>(child->layerId) << 32) | 0x40000000;
    mFlinger.setTransactionState(std::move(txn), nullptr);

    ALOGI("Subsurface layer %u reparented under layer %u",
          child->layerId, parent->layerId);
}

void WaylandCompositor::setSurfacePosition(WaylandSurface* surface, int32_t x, int32_t y) {
    if (!surface || !surface->handle) return;

    TransactionState txn;
    ComposerState cs;
    cs.state.what = layer_state_t::ePositionChanged;
    cs.state.surface = surface->handle;
    cs.state.x = static_cast<float>(x);
    cs.state.y = static_cast<float>(y);
    txn.mComposerStates.push_back(std::move(cs));
    txn.mId = (static_cast<uint64_t>(surface->layerId) << 32) | 0x50000000;
    mFlinger.setTransactionState(std::move(txn), nullptr);
}

void WaylandCompositor::sendToplevelConfigure(int layerId, int32_t width, int32_t height) {
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        mPendingEvents.push_back({PendingEvent::Configure, layerId, width, height, 0, 0, 0, false});
    }
    if (mWakeEventFd >= 0) { uint64_t v=1; write(mWakeEventFd, &v, sizeof(v)); }
}

void WaylandCompositor::dispatchPointerMotion(int layerId, uint32_t timeMs, double x, double y) {
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        mPendingEvents.push_back({PendingEvent::PointerMotion, layerId,
                static_cast<int32_t>(timeMs), 0, 0,
                static_cast<float>(x), static_cast<float>(y), false});
    }
    if (mWakeEventFd >= 0) { uint64_t v=1; write(mWakeEventFd, &v, sizeof(v)); }
}

void WaylandCompositor::dispatchPointerButton(int layerId, uint32_t timeMs, uint32_t button,
                                               bool pressed) {
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        mPendingEvents.push_back({PendingEvent::PointerButton, layerId,
                static_cast<int32_t>(timeMs), static_cast<int32_t>(button), 0,
                0, 0, pressed});
    }
    if (mWakeEventFd >= 0) { uint64_t v=1; write(mWakeEventFd, &v, sizeof(v)); }
}

void WaylandCompositor::dispatchKey(int layerId, uint32_t timeMs, uint32_t evdevKey,
                                     bool pressed) {
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        mPendingEvents.push_back({PendingEvent::Key, layerId,
                static_cast<int32_t>(timeMs), static_cast<int32_t>(evdevKey), 0,
                0, 0, pressed});
    }
    if (mWakeEventFd >= 0) { uint64_t v=1; write(mWakeEventFd, &v, sizeof(v)); }
}

void WaylandCompositor::dispatchEvents() {
    wl_event_loop_dispatch(mEventLoop, 0);
    wl_display_flush_clients(mDisplay);
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
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        if (mPendingFrameCallbacks.empty() && mPendingBufferReleases.empty()) return;
        mPendingVsyncTimeMs = vsyncTimeMs;
    }
    // Wake the Wayland dispatch thread to fire callbacks there.
    if (mWakeEventFd >= 0) {
        uint64_t val = 1;
        write(mWakeEventFd, &val, sizeof(val));
    }
}

void WaylandCompositor::queueBufferRelease(struct wl_resource* buffer) {
    std::lock_guard<std::mutex> lock(mCallbacksMutex);
    mPendingBufferReleases.push_back(buffer);
}

void WaylandCompositor::fireBufferReleases() {
    // Buffer releases are fired together with frame callbacks via the wake eventfd.
    // Just wake the dispatch thread if there are pending releases.
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        if (mPendingBufferReleases.empty()) return;
    }
    if (mWakeEventFd >= 0) {
        uint64_t val = 1;
        write(mWakeEventFd, &val, sizeof(val));
    }
}

void WaylandCompositor::doFireFrameCallbacksAndReleases() {
    std::vector<struct wl_resource*> callbacks;
    std::vector<struct wl_resource*> buffers;
    std::vector<PendingEvent> events;
    uint32_t vsyncMs;
    {
        std::lock_guard<std::mutex> lock(mCallbacksMutex);
        callbacks = std::move(mPendingFrameCallbacks);
        mPendingFrameCallbacks.clear();
        buffers = std::move(mPendingBufferReleases);
        mPendingBufferReleases.clear();
        events = std::move(mPendingEvents);
        mPendingEvents.clear();
        vsyncMs = mPendingVsyncTimeMs;
    }

    for (auto* buf : buffers) {
        wl_buffer_send_release(buf);
    }
    for (auto* cb : callbacks) {
        wl_callback_send_done(cb, vsyncMs);
        wl_resource_destroy(cb);
    }

    // Process queued events on the Wayland thread.
    for (auto& ev : events) {
        switch (ev.type) {
            case PendingEvent::Configure: {
                WaylandSurface* ws = findSurfaceByLayerId(ev.layerId);
                if (!ws || !ws->xdgToplevel || !ws->xdgSurface) break;
                struct wl_array states;
                wl_array_init(&states);
                uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
                *s = 4; // XDG_TOPLEVEL_STATE_ACTIVATED
                xdg_toplevel_send_configure(ws->xdgToplevel, ev.i1, ev.i2, &states);
                wl_array_release(&states);
                // Use the xdg_surface's serial counter (same one used for initial configure)
                // to avoid duplicate serials which violate the xdg_surface protocol.
                auto* xdgSurf = static_cast<WaylandXdgSurface*>(
                        wl_resource_get_user_data(ws->xdgSurface));
                if (xdgSurf) {
                    xdgSurf->pendingConfigureSerial++;
                    xdg_surface_send_configure(ws->xdgSurface,
                                               xdgSurf->pendingConfigureSerial);
                }
                ALOGI("Sent toplevel configure %dx%d for layer %d", ev.i1, ev.i2, ev.layerId);
                break;
            }
            case PendingEvent::PointerMotion: {
                WaylandSurface* ws = findSurfaceByLayerId(ev.layerId);
                if (ws && ws->resource && mSeat) {
                    mSeat->setFocus(ws->resource);
                    mSeat->sendPointerMotion(static_cast<uint32_t>(ev.i1), ev.f1, ev.f2);
                }
                break;
            }
            case PendingEvent::PointerButton: {
                WaylandSurface* ws = findSurfaceByLayerId(ev.layerId);
                if (ws && ws->resource && mSeat) {
                    mSeat->setFocus(ws->resource);
                    mSeat->sendPointerButton(static_cast<uint32_t>(ev.i1),
                            static_cast<uint32_t>(ev.i2), ev.b1);
                }
                break;
            }
            case PendingEvent::Key: {
                WaylandSurface* ws = findSurfaceByLayerId(ev.layerId);
                if (ws && ws->resource && mSeat) {
                    mSeat->setFocus(ws->resource);
                    mSeat->sendKey(static_cast<uint32_t>(ev.i1),
                            static_cast<uint32_t>(ev.i2), ev.b1);
                }
                break;
            }
        }
    }
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

void WaylandCompositor::postBufferWork(BufferWork&& work) {
    {
        std::lock_guard<std::mutex> lock(mBufferMutex);
        mBufferQueue.push_back(std::move(work));
    }
    mBufferCv.notify_one();
}

void WaylandCompositor::bufferThreadLoop() {
    while (!mDispatchStop) {
        std::vector<BufferWork> work;
        {
            std::unique_lock<std::mutex> lock(mBufferMutex);
            mBufferCv.wait(lock, [this]() REQUIRES(mBufferMutex) {
                return mDispatchStop.load() || !mBufferQueue.empty();
            });
            if (mDispatchStop.load() && mBufferQueue.empty()) break;
            work.swap(mBufferQueue);
        }

        for (auto& item : work) {
            sp<GraphicBuffer> gb = item.gb;

            // If we have raw pixel data, do the gralloc alloc + copy here
            // (off the Wayland dispatch thread).
            if (!item.pixels.empty() && !gb) {
                gb = sp<GraphicBuffer>::make(
                        static_cast<uint32_t>(item.width),
                        static_cast<uint32_t>(item.height),
                        item.pixFmt, 1u,
                        static_cast<uint64_t>(GRALLOC_USAGE_SW_WRITE_OFTEN |
                                              GRALLOC_USAGE_HW_TEXTURE |
                                              GRALLOC_USAGE_HW_COMPOSER),
                        "WaylandShm");
                if (gb->initCheck() != NO_ERROR) {
                    ALOGE("BufferThread: GraphicBuffer alloc failed");
                    continue;
                }
                void* dst = nullptr;
                if (gb->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &dst) != NO_ERROR || !dst) {
                    ALOGE("BufferThread: GraphicBuffer lock failed");
                    continue;
                }
                const uint32_t bpp = bytesPerPixel(item.pixFmt);
                const uint8_t* src = item.pixels.data();
                uint8_t* dstBytes = static_cast<uint8_t*>(dst);
                const uint32_t dstStride = gb->getStride() * bpp;
                const uint32_t w = static_cast<uint32_t>(item.width);
                const uint32_t h = static_cast<uint32_t>(item.height);
                for (uint32_t y = 0; y < h; y++) {
                    const uint32_t* srcRow =
                            reinterpret_cast<const uint32_t*>(src + y * item.srcStride);
                    uint32_t* dstRow =
                            reinterpret_cast<uint32_t*>(dstBytes + y * dstStride);
                    for (uint32_t x = 0; x < w; x++) {
                        uint32_t px = srcRow[x];
                        uint32_t b = (px >> 0) & 0xFF;
                        uint32_t g = (px >> 8) & 0xFF;
                        uint32_t r = (px >> 16) & 0xFF;
                        uint32_t a = (px >> 24) & 0xFF;
                        dstRow[x] = (a << 24) | (b << 16) | (g << 8) | r;
                    }
                }
                gb->unlock();
            }

            if (!gb) continue;

            TransactionState txn;
            ComposerState cs;
            cs.state.what = layer_state_t::eBufferChanged | layer_state_t::eCropChanged;
            cs.state.surface = item.handle;
            cs.state.bufferData = std::make_shared<BufferData>();
            cs.state.bufferData->buffer = gb;
            cs.state.bufferData->frameNumber = item.frameNumber;
            cs.state.bufferData->flags |= BufferData::BufferDataChange::frameNumberChanged;
            cs.state.bufferData->acquireFence = Fence::NO_FENCE;
            cs.state.bufferData->producerId = item.producerId;
            cs.state.crop = FloatRect(0, 0, item.width, item.height);
            txn.mComposerStates.push_back(std::move(cs));
            txn.mId = (static_cast<uint64_t>(item.layerId) << 32) | item.frameNumber;
            txn.mIsAutoTimestamp = true;
            mFlinger.setTransactionState(std::move(txn), /*applyToken=*/nullptr);
        }
    }
}

} // namespace android
