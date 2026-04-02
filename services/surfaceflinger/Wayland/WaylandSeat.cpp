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

#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

#include <log/log.h>
#include <wayland-server-protocol.h>

namespace android {

namespace {
// We advertise v5 but only handle the release request (added in v5).
// No input events are sent; this is a stub so clients don't abort.
constexpr uint32_t kSeatVersion = 5;
} // anonymous namespace

// --- wl_seat ---

const struct ::wl_seat_interface WaylandSeat::kSeatImpl = {
        .get_pointer = WaylandSeat::seatGetPointer,
        .get_keyboard = WaylandSeat::seatGetKeyboard,
        .get_touch = WaylandSeat::seatGetTouch,
        .release = WaylandSeat::seatRelease,
};

struct wl_global* WaylandSeat::createGlobal(struct wl_display* display) {
    return wl_global_create(display, &wl_seat_interface, kSeatVersion,
                            nullptr, bind);
}

void WaylandSeat::bind(struct wl_client* client, void* /*data*/,
                        uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kSeatVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &wl_seat_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kSeatImpl, nullptr, nullptr);

    // Advertise pointer + keyboard capabilities.
    wl_seat_send_capabilities(resource,
                              WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    if (ver >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, "default");
    }

    ALOGI("wl_seat bound (v%u)", ver);
}

void WaylandSeat::seatGetPointer(struct wl_client* client,
                                  struct wl_resource* resource,
                                  uint32_t id) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* pointer =
            wl_resource_create(client, &wl_pointer_interface, ver, id);
    if (!pointer) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(pointer, &kPointerImpl, nullptr, nullptr);
}

void WaylandSeat::seatGetKeyboard(struct wl_client* client,
                                   struct wl_resource* resource,
                                   uint32_t id) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* keyboard =
            wl_resource_create(client, &wl_keyboard_interface, ver, id);
    if (!keyboard) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(keyboard, &kKeyboardImpl, nullptr, nullptr);

    // Send a no-keymap event so clients that require a keymap don't stall.
    // WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP = 0, with fd=-1 and size=0.
    // However, the protocol requires a valid fd. Send an empty fd via /dev/null.
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
        wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0);
        close(fd);
    }
}

void WaylandSeat::seatGetTouch(struct wl_client* /*client*/,
                                struct wl_resource* /*resource*/,
                                uint32_t /*id*/) {
    // Touch not advertised in capabilities; ignore.
}

void WaylandSeat::seatRelease(struct wl_client* /*client*/,
                               struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

// --- wl_pointer (stub) ---

const struct ::wl_pointer_interface WaylandSeat::kPointerImpl = {
        .set_cursor = WaylandSeat::pointerSetCursor,
        .release = WaylandSeat::pointerRelease,
};

void WaylandSeat::pointerSetCursor(struct wl_client* /*client*/,
                                    struct wl_resource* /*resource*/,
                                    uint32_t /*serial*/,
                                    struct wl_resource* /*surface*/,
                                    int32_t /*hotspot_x*/,
                                    int32_t /*hotspot_y*/) {
    // Stub: no cursor rendering.
}

void WaylandSeat::pointerRelease(struct wl_client* /*client*/,
                                  struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

// --- wl_keyboard (stub) ---

const struct ::wl_keyboard_interface WaylandSeat::kKeyboardImpl = {
        .release = WaylandSeat::keyboardRelease,
};

void WaylandSeat::keyboardRelease(struct wl_client* /*client*/,
                                   struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

} // namespace android
