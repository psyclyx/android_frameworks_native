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
#include <wayland-server-protocol.h>

#include <cstdint>

namespace android {

// Manages wl_seat global (stub). Advertises pointer + keyboard capabilities
// but sends no input events. Enough for EGL clients that query seat but
// don't require input to render.
class WaylandSeat {
public:
    static struct wl_global* createGlobal(struct wl_display* display);

private:
    // wl_seat
    static void bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id);
    static void seatGetPointer(struct wl_client* client, struct wl_resource* resource,
                                uint32_t id);
    static void seatGetKeyboard(struct wl_client* client, struct wl_resource* resource,
                                 uint32_t id);
    static void seatGetTouch(struct wl_client* client, struct wl_resource* resource,
                              uint32_t id);
    static void seatRelease(struct wl_client* client, struct wl_resource* resource);

    static const struct wl_seat_interface kSeatImpl;

    // wl_pointer (stub)
    static void pointerSetCursor(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t serial, struct wl_resource* surface,
                                  int32_t hotspot_x, int32_t hotspot_y);
    static void pointerRelease(struct wl_client* client, struct wl_resource* resource);

    static const struct wl_pointer_interface kPointerImpl;

    // wl_keyboard (stub)
    static void keyboardRelease(struct wl_client* client, struct wl_resource* resource);

    static const struct wl_keyboard_interface kKeyboardImpl;
};

} // namespace android
