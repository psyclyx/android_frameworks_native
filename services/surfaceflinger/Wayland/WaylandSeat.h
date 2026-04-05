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
#include <mutex>
#include <vector>

namespace android {

class WaylandCompositor;

class WaylandSeat {
public:
    explicit WaylandSeat(WaylandCompositor* compositor);
    ~WaylandSeat();

    struct wl_global* createGlobal(struct wl_display* display);

    // Set keyboard/pointer focus to a surface. nullptr = unfocus.
    void setFocus(struct wl_resource* surface);

    // Input dispatch — called from binder thread, dispatched on Wayland event loop.
    void sendPointerMotion(uint32_t timeMs, double x, double y);
    void sendPointerButton(uint32_t timeMs, uint32_t button, bool pressed);
    void sendPointerAxis(uint32_t timeMs, uint32_t axis, double value);
    void sendKey(uint32_t timeMs, uint32_t evdevKey, bool pressed);

    uint32_t nextSerial();

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

    // wl_pointer
    static void pointerSetCursor(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t serial, struct wl_resource* surface,
                                  int32_t hotspot_x, int32_t hotspot_y);
    static void pointerRelease(struct wl_client* client, struct wl_resource* resource);
    static const struct wl_pointer_interface kPointerImpl;

    // wl_keyboard
    static void keyboardRelease(struct wl_client* client, struct wl_resource* resource);
    static const struct wl_keyboard_interface kKeyboardImpl;

    void sendKeymapToKeyboard(struct wl_resource* keyboard);

    WaylandCompositor* mCompositor;
    uint32_t mSerial = 0;

    // Keymap data (created once at init, fd kept open)
    int mKeymapFd = -1;
    uint32_t mKeymapSize = 0;

    // Tracked resources — all pointer/keyboard resources from all clients.
    std::mutex mResourcesMutex;
    std::vector<struct wl_resource*> mPointers;
    std::vector<struct wl_resource*> mKeyboards;

    // Currently focused surface
    struct wl_resource* mFocusedSurface = nullptr;
};

} // namespace android
