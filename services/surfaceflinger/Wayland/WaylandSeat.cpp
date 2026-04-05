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
#define LOG_TAG "WaylandSeat"

#include "WaylandSeat.h"
#include "WaylandCompositor.h"
#include "WaylandTextInput.h"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <log/log.h>
#include <wayland-server-protocol.h>

namespace android {

namespace {
constexpr uint32_t kSeatVersion = 5;

// Read the xkb keymap from a file path.
// Returns the content as a string (empty on failure).
std::string readKeymapFile(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return {};
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0) { close(fd); return {}; }
    lseek(fd, 0, SEEK_SET);
    std::string data(static_cast<size_t>(size), '\0');
    ssize_t n = read(fd, data.data(), data.size());
    close(fd);
    if (n != size) return {};
    return data;
}

} // anonymous namespace

// --- wl_seat_interface ---

const struct ::wl_seat_interface WaylandSeat::kSeatImpl = {
        .get_pointer = WaylandSeat::seatGetPointer,
        .get_keyboard = WaylandSeat::seatGetKeyboard,
        .get_touch = WaylandSeat::seatGetTouch,
        .release = WaylandSeat::seatRelease,
};

const struct ::wl_pointer_interface WaylandSeat::kPointerImpl = {
        .set_cursor = WaylandSeat::pointerSetCursor,
        .release = WaylandSeat::pointerRelease,
};

const struct ::wl_keyboard_interface WaylandSeat::kKeyboardImpl = {
        .release = WaylandSeat::keyboardRelease,
};

WaylandSeat::WaylandSeat(WaylandCompositor* compositor) : mCompositor(compositor) {
    // Load the xkb keymap and create a sealed memfd for sharing with clients.
    // Try system path first, then fall back to the path next to surfaceflinger.
    std::string keymap;
    const char* paths[] = {
        "/system/etc/wayland/us-keymap.xkb",
        "/system_ext/etc/wayland/us-keymap.xkb",
    };
    for (const char* p : paths) {
        keymap = readKeymapFile(p);
        if (!keymap.empty()) {
            ALOGI("Loaded xkb keymap from %s (%zu bytes)", p, keymap.size());
            break;
        }
    }

    if (keymap.empty()) {
        ALOGW("No xkb keymap found; keyboard input will not work");
        return;
    }

    // Create memfd and write keymap content (with null terminator for xkbcommon).
    mKeymapSize = static_cast<uint32_t>(keymap.size() + 1);
    mKeymapFd = memfd_create("wayland-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mKeymapFd < 0) {
        ALOGE("memfd_create failed: %s", strerror(errno));
        return;
    }
    if (ftruncate(mKeymapFd, mKeymapSize) != 0) {
        ALOGE("ftruncate keymap memfd failed: %s", strerror(errno));
        close(mKeymapFd);
        mKeymapFd = -1;
        return;
    }
    void* map = mmap(nullptr, mKeymapSize, PROT_WRITE, MAP_SHARED, mKeymapFd, 0);
    if (map == MAP_FAILED) {
        ALOGE("mmap keymap memfd failed: %s", strerror(errno));
        close(mKeymapFd);
        mKeymapFd = -1;
        return;
    }
    memcpy(map, keymap.c_str(), mKeymapSize); // includes null terminator
    munmap(map, mKeymapSize);

    // Seal the memfd so clients can mmap it read-only.
    fcntl(mKeymapFd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
    ALOGI("xkb keymap ready: fd=%d size=%u", mKeymapFd, mKeymapSize);
}

WaylandSeat::~WaylandSeat() {
    if (mKeymapFd >= 0) close(mKeymapFd);
}

struct wl_global* WaylandSeat::createGlobal(struct wl_display* display) {
    return wl_global_create(display, &wl_seat_interface, kSeatVersion,
                            this, bind);
}

uint32_t WaylandSeat::nextSerial() {
    return ++mSerial;
}

void WaylandSeat::bind(struct wl_client* client, void* data,
                        uint32_t version, uint32_t id) {
    auto* self = static_cast<WaylandSeat*>(data);
    int ver = static_cast<int>(std::min(version, kSeatVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &wl_seat_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kSeatImpl, self, nullptr);

    wl_seat_send_capabilities(resource,
                              WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    if (ver >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, "default");
    }

    // Flush immediately so the client receives capabilities during its
    // current wl_display_dispatch call and can send get_keyboard/get_pointer
    // right away, rather than waiting for the next flush cycle.
    wl_client_flush(client);

    ALOGI("wl_seat bound (v%u)", ver);
}

void WaylandSeat::seatGetPointer(struct wl_client* client,
                                  struct wl_resource* resource,
                                  uint32_t id) {
    auto* self = static_cast<WaylandSeat*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);
    struct wl_resource* pointer =
            wl_resource_create(client, &wl_pointer_interface, ver, id);
    if (!pointer) {
        wl_resource_post_no_memory(resource);
        return;
    }

    wl_resource_set_implementation(pointer, &kPointerImpl, self,
            [](struct wl_resource* res) {
                auto* seat = static_cast<WaylandSeat*>(wl_resource_get_user_data(res));
                if (seat) {
                    std::lock_guard<std::mutex> lock(seat->mResourcesMutex);
                    auto& v = seat->mPointers;
                    v.erase(std::remove(v.begin(), v.end(), res), v.end());
                }
            });

    {
        std::lock_guard<std::mutex> lock(self->mResourcesMutex);
        self->mPointers.push_back(pointer);
    }
}

void WaylandSeat::seatGetKeyboard(struct wl_client* client,
                                   struct wl_resource* resource,
                                   uint32_t id) {
    auto* self = static_cast<WaylandSeat*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);
    struct wl_resource* keyboard =
            wl_resource_create(client, &wl_keyboard_interface, ver, id);
    if (!keyboard) {
        wl_resource_post_no_memory(resource);
        return;
    }

    wl_resource_set_implementation(keyboard, &kKeyboardImpl, self,
            [](struct wl_resource* res) {
                auto* seat = static_cast<WaylandSeat*>(wl_resource_get_user_data(res));
                if (seat) {
                    std::lock_guard<std::mutex> lock(seat->mResourcesMutex);
                    auto& v = seat->mKeyboards;
                    v.erase(std::remove(v.begin(), v.end(), res), v.end());
                }
            });

    {
        std::lock_guard<std::mutex> lock(self->mResourcesMutex);
        self->mKeyboards.push_back(keyboard);
    }

    self->sendKeymapToKeyboard(keyboard);
}

void WaylandSeat::seatGetTouch(struct wl_client* /*client*/,
                                struct wl_resource* /*resource*/,
                                uint32_t /*id*/) {
    // Touch not advertised; ignore.
}

void WaylandSeat::seatRelease(struct wl_client* /*client*/,
                               struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

// --- wl_pointer ---

void WaylandSeat::pointerSetCursor(struct wl_client* /*client*/,
                                    struct wl_resource* /*resource*/,
                                    uint32_t /*serial*/,
                                    struct wl_resource* /*surface*/,
                                    int32_t /*hotspot_x*/,
                                    int32_t /*hotspot_y*/) {
    // No cursor rendering for now.
}

void WaylandSeat::pointerRelease(struct wl_client* /*client*/,
                                  struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

// --- wl_keyboard ---

void WaylandSeat::keyboardRelease(struct wl_client* /*client*/,
                                   struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandSeat::sendKeymapToKeyboard(struct wl_resource* keyboard) {
    if (mKeymapFd < 0) {
        // No keymap available — send no-keymap with /dev/null.
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0);
            close(fd);
        }
        return;
    }

    // Dup the fd so each client gets its own.
    int fd = fcntl(mKeymapFd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        ALOGE("Failed to dup keymap fd: %s", strerror(errno));
        return;
    }
    ALOGI("sendKeymapToKeyboard: sending XKB_V1 keymap fd=%d size=%u", fd, mKeymapSize);
    wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, mKeymapSize);
    // Do NOT close fd — libwayland takes ownership via sendmsg(SCM_RIGHTS).
    // close(fd);
}

// --- Focus management ---

void WaylandSeat::setFocus(struct wl_resource* surface) {
    if (mFocusedSurface == surface) return;

    struct wl_client* oldClient = mFocusedSurface ? wl_resource_get_client(mFocusedSurface) : nullptr;
    struct wl_client* newClient = surface ? wl_resource_get_client(surface) : nullptr;

    uint32_t serial = nextSerial();

    std::lock_guard<std::mutex> lock(mResourcesMutex);

    // Send pointer leave + keyboard leave to old surface
    if (mFocusedSurface) {
        for (auto* ptr : mPointers) {
            if (wl_resource_get_client(ptr) == oldClient) {
                wl_pointer_send_leave(ptr, serial, mFocusedSurface);
            }
        }
        for (auto* kb : mKeyboards) {
            if (wl_resource_get_client(kb) == oldClient) {
                wl_keyboard_send_leave(kb, serial, mFocusedSurface);
            }
        }
    }

    mFocusedSurface = surface;

    // Update text input focus.
    if (mCompositor && mCompositor->textInput()) {
        mCompositor->textInput()->setFocus(surface);
    }

    // Send pointer enter + keyboard enter to new surface
    if (mFocusedSurface) {
        for (auto* ptr : mPointers) {
            if (wl_resource_get_client(ptr) == newClient) {
                wl_pointer_send_enter(ptr, serial, mFocusedSurface,
                                      wl_fixed_from_int(0), wl_fixed_from_int(0));
            }
        }
        for (auto* kb : mKeyboards) {
            if (wl_resource_get_client(kb) == newClient) {
                struct wl_array keys;
                wl_array_init(&keys);
                wl_keyboard_send_enter(kb, serial, mFocusedSurface, &keys);
                wl_array_release(&keys);
            }
        }
    }
}

// --- Input dispatch ---

void WaylandSeat::sendPointerMotion(uint32_t timeMs, double x, double y) {
    if (!mFocusedSurface) return;
    struct wl_client* client = wl_resource_get_client(mFocusedSurface);

    std::lock_guard<std::mutex> lock(mResourcesMutex);
    for (auto* ptr : mPointers) {
        if (wl_resource_get_client(ptr) == client) {
            wl_pointer_send_motion(ptr, timeMs,
                                   wl_fixed_from_double(x),
                                   wl_fixed_from_double(y));
            if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
                wl_pointer_send_frame(ptr);
            }
        }
    }
}

void WaylandSeat::sendPointerButton(uint32_t timeMs, uint32_t button, bool pressed) {
    if (!mFocusedSurface) return;
    struct wl_client* client = wl_resource_get_client(mFocusedSurface);
    uint32_t serial = nextSerial();

    std::lock_guard<std::mutex> lock(mResourcesMutex);
    for (auto* ptr : mPointers) {
        if (wl_resource_get_client(ptr) == client) {
            wl_pointer_send_button(ptr, serial, timeMs, button,
                                   pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                           : WL_POINTER_BUTTON_STATE_RELEASED);
            if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
                wl_pointer_send_frame(ptr);
            }
        }
    }
}

void WaylandSeat::sendPointerAxis(uint32_t timeMs, uint32_t axis, double value) {
    if (!mFocusedSurface) return;
    struct wl_client* client = wl_resource_get_client(mFocusedSurface);

    std::lock_guard<std::mutex> lock(mResourcesMutex);
    for (auto* ptr : mPointers) {
        if (wl_resource_get_client(ptr) == client) {
            wl_pointer_send_axis(ptr, timeMs, axis, wl_fixed_from_double(value));
            if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
                wl_pointer_send_frame(ptr);
            }
        }
    }
}

void WaylandSeat::sendKey(uint32_t timeMs, uint32_t evdevKey, bool pressed) {
    if (!mFocusedSurface) return;
    struct wl_client* client = wl_resource_get_client(mFocusedSurface);
    uint32_t serial = nextSerial();

    std::lock_guard<std::mutex> lock(mResourcesMutex);
    for (auto* kb : mKeyboards) {
        if (wl_resource_get_client(kb) == client) {
            wl_keyboard_send_key(kb, serial, timeMs, evdevKey,
                                 pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                         : WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
}

} // namespace android
