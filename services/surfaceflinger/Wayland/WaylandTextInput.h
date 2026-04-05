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

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "text-input-unstable-v3-server-protocol.h"

namespace android {

class WaylandCompositor;

class WaylandTextInput {
public:
    explicit WaylandTextInput(WaylandCompositor* compositor);

    struct wl_global* createGlobal(struct wl_display* display);

    // Called by WaylandSeat when keyboard focus changes.
    void setFocus(struct wl_resource* surface);

    // Called from binder thread (via pending events) to send text events to client.
    void sendCommitString(const char* text);
    void sendPreeditString(const char* text, int32_t cursorBegin, int32_t cursorEnd);
    void sendDeleteSurroundingText(uint32_t beforeLength, uint32_t afterLength);
    void sendDone();

private:
    // Per text_input_v3 object state.
    struct TextInputState {
        struct wl_resource* resource = nullptr;
        struct wl_resource* focusedSurface = nullptr;
        bool pendingEnabled = false;
        bool enabled = false;
        uint32_t commitSerial = 0; // counts client commit requests
        // Pending state (applied on commit)
        uint32_t pendingContentHint = 0;
        uint32_t pendingContentPurpose = 0;
        int32_t pendingCursorX = 0;
        int32_t pendingCursorY = 0;
        int32_t pendingCursorW = 0;
        int32_t pendingCursorH = 0;
        std::string pendingSurroundingText;
        int32_t pendingSurroundingCursor = 0;
        int32_t pendingSurroundingAnchor = 0;
        bool hasPendingSurrounding = false;
        // Current state (after commit)
        uint32_t contentHint = 0;
        uint32_t contentPurpose = 0;
    };

    // Manager global
    static void bindManager(struct wl_client* client, void* data,
                             uint32_t version, uint32_t id);
    static void managerDestroy(struct wl_client* client, struct wl_resource* resource);
    static void managerGetTextInput(struct wl_client* client, struct wl_resource* resource,
                                     uint32_t id, struct wl_resource* seat);
    static const struct ::zwp_text_input_manager_v3_interface kManagerImpl;

    // Text input object
    static void textInputDestroy(struct wl_client* client, struct wl_resource* resource);
    static void textInputEnable(struct wl_client* client, struct wl_resource* resource);
    static void textInputDisable(struct wl_client* client, struct wl_resource* resource);
    static void textInputSetSurroundingText(struct wl_client* client, struct wl_resource* resource,
                                             const char* text, int32_t cursor, int32_t anchor);
    static void textInputSetTextChangeCause(struct wl_client* client, struct wl_resource* resource,
                                             uint32_t cause);
    static void textInputSetContentType(struct wl_client* client, struct wl_resource* resource,
                                         uint32_t hint, uint32_t purpose);
    static void textInputSetCursorRectangle(struct wl_client* client, struct wl_resource* resource,
                                             int32_t x, int32_t y, int32_t width, int32_t height);
    static void textInputCommit(struct wl_client* client, struct wl_resource* resource);
    static const struct ::zwp_text_input_v3_interface kTextInputImpl;

    WaylandCompositor* mCompositor;

    std::mutex mResourcesMutex;
    std::vector<TextInputState*> mTextInputs;

    // Currently focused surface (follows keyboard focus).
    struct wl_resource* mFocusedSurface = nullptr;

    // Find the active (enabled) text input for the focused surface's client.
    TextInputState* findActiveTextInput();
};

} // namespace android
