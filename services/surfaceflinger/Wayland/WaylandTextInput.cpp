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
#define LOG_TAG "WaylandTextInput"

#include "WaylandTextInput.h"
#include "WaylandCompositor.h"

#include <algorithm>
#include <log/log.h>

#include "text-input-unstable-v3-server-protocol.h"

namespace android {

namespace {
constexpr uint32_t kManagerVersion = 1;
} // anonymous namespace

// --- zwp_text_input_manager_v3 ---

const struct ::zwp_text_input_manager_v3_interface WaylandTextInput::kManagerImpl = {
        .destroy = WaylandTextInput::managerDestroy,
        .get_text_input = WaylandTextInput::managerGetTextInput,
};

// --- zwp_text_input_v3 ---

const struct ::zwp_text_input_v3_interface WaylandTextInput::kTextInputImpl = {
        .destroy = WaylandTextInput::textInputDestroy,
        .enable = WaylandTextInput::textInputEnable,
        .disable = WaylandTextInput::textInputDisable,
        .set_surrounding_text = WaylandTextInput::textInputSetSurroundingText,
        .set_text_change_cause = WaylandTextInput::textInputSetTextChangeCause,
        .set_content_type = WaylandTextInput::textInputSetContentType,
        .set_cursor_rectangle = WaylandTextInput::textInputSetCursorRectangle,
        .commit = WaylandTextInput::textInputCommit,
};

WaylandTextInput::WaylandTextInput(WaylandCompositor* compositor)
      : mCompositor(compositor) {}

struct wl_global* WaylandTextInput::createGlobal(struct wl_display* display) {
    return wl_global_create(display, &::zwp_text_input_manager_v3_interface,
                            kManagerVersion, this, bindManager);
}

void WaylandTextInput::bindManager(struct wl_client* client, void* data,
                                     uint32_t version, uint32_t id) {
    auto* self = static_cast<WaylandTextInput*>(data);
    int ver = static_cast<int>(std::min(version, kManagerVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &::zwp_text_input_manager_v3_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kManagerImpl, self, nullptr);
    ALOGI("zwp_text_input_manager_v3 bound (v%u)", ver);
}

void WaylandTextInput::managerDestroy(struct wl_client* /*client*/,
                                        struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandTextInput::managerGetTextInput(struct wl_client* client,
                                             struct wl_resource* resource,
                                             uint32_t id,
                                             struct wl_resource* /*seat*/) {
    auto* self = static_cast<WaylandTextInput*>(wl_resource_get_user_data(resource));

    struct wl_resource* tiResource =
            wl_resource_create(client, &::zwp_text_input_v3_interface, 1, id);
    if (!tiResource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* state = new TextInputState();
    state->resource = tiResource;

    wl_resource_set_implementation(tiResource, &kTextInputImpl, self,
            [](struct wl_resource* res) {
                auto* ti = static_cast<WaylandTextInput*>(wl_resource_get_user_data(res));
                if (!ti) return;
                std::lock_guard<std::mutex> lock(ti->mResourcesMutex);
                for (auto it = ti->mTextInputs.begin(); it != ti->mTextInputs.end(); ++it) {
                    if ((*it)->resource == res) {
                        delete *it;
                        ti->mTextInputs.erase(it);
                        break;
                    }
                }
            });

    {
        std::lock_guard<std::mutex> lock(self->mResourcesMutex);
        self->mTextInputs.push_back(state);
    }

    // If there's a focused surface belonging to this client, send enter.
    if (self->mFocusedSurface &&
        wl_resource_get_client(self->mFocusedSurface) == client) {
        state->focusedSurface = self->mFocusedSurface;
        zwp_text_input_v3_send_enter(tiResource, self->mFocusedSurface);
    }

    ALOGI("Created zwp_text_input_v3 for client");
}

// --- Text input request handlers ---

void WaylandTextInput::textInputDestroy(struct wl_client* /*client*/,
                                          struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandTextInput::textInputEnable(struct wl_client* /*client*/,
                                         struct wl_resource* resource) {
    auto* self = static_cast<WaylandTextInput*>(wl_resource_get_user_data(resource));
    std::lock_guard<std::mutex> lock(self->mResourcesMutex);
    for (auto* state : self->mTextInputs) {
        if (state->resource == resource) {
            state->pendingEnabled = true;
            ALOGI("text_input enable (pending)");
            break;
        }
    }
}

void WaylandTextInput::textInputDisable(struct wl_client* /*client*/,
                                          struct wl_resource* resource) {
    auto* self = static_cast<WaylandTextInput*>(wl_resource_get_user_data(resource));
    std::lock_guard<std::mutex> lock(self->mResourcesMutex);
    for (auto* state : self->mTextInputs) {
        if (state->resource == resource) {
            state->pendingEnabled = false;
            ALOGI("text_input disable (pending)");
            break;
        }
    }
}

void WaylandTextInput::textInputSetSurroundingText(struct wl_client* /*client*/,
                                                      struct wl_resource* resource,
                                                      const char* text,
                                                      int32_t cursor, int32_t anchor) {
    auto* self = static_cast<WaylandTextInput*>(wl_resource_get_user_data(resource));
    std::lock_guard<std::mutex> lock(self->mResourcesMutex);
    for (auto* state : self->mTextInputs) {
        if (state->resource == resource) {
            state->pendingSurroundingText = text ? text : "";
            state->pendingSurroundingCursor = cursor;
            state->pendingSurroundingAnchor = anchor;
            state->hasPendingSurrounding = true;
            break;
        }
    }
}

void WaylandTextInput::textInputSetTextChangeCause(struct wl_client* /*client*/,
                                                      struct wl_resource* /*resource*/,
                                                      uint32_t /*cause*/) {
    // Noted but not forwarded — Android IME doesn't have this concept.
}

void WaylandTextInput::textInputSetContentType(struct wl_client* /*client*/,
                                                 struct wl_resource* resource,
                                                 uint32_t hint, uint32_t purpose) {
    auto* self = static_cast<WaylandTextInput*>(wl_resource_get_user_data(resource));
    std::lock_guard<std::mutex> lock(self->mResourcesMutex);
    for (auto* state : self->mTextInputs) {
        if (state->resource == resource) {
            state->pendingContentHint = hint;
            state->pendingContentPurpose = purpose;
            break;
        }
    }
}

void WaylandTextInput::textInputSetCursorRectangle(struct wl_client* /*client*/,
                                                      struct wl_resource* resource,
                                                      int32_t x, int32_t y,
                                                      int32_t width, int32_t height) {
    auto* self = static_cast<WaylandTextInput*>(wl_resource_get_user_data(resource));
    std::lock_guard<std::mutex> lock(self->mResourcesMutex);
    for (auto* state : self->mTextInputs) {
        if (state->resource == resource) {
            state->pendingCursorX = x;
            state->pendingCursorY = y;
            state->pendingCursorW = width;
            state->pendingCursorH = height;
            break;
        }
    }
}

void WaylandTextInput::textInputCommit(struct wl_client* /*client*/,
                                         struct wl_resource* resource) {
    auto* self = static_cast<WaylandTextInput*>(wl_resource_get_user_data(resource));

    TextInputState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(self->mResourcesMutex);
        for (auto* s : self->mTextInputs) {
            if (s->resource == resource) {
                state = s;
                break;
            }
        }
    }
    if (!state) return;

    state->commitSerial++;
    bool wasEnabled = state->enabled;
    state->enabled = state->pendingEnabled;
    state->contentHint = state->pendingContentHint;
    state->contentPurpose = state->pendingContentPurpose;

    // Find the layerId for this surface so we can talk to the Java side.
    int layerId = -1;
    if (state->focusedSurface) {
        WaylandSurface* ws = self->mCompositor->findSurface(state->focusedSurface);
        if (ws) layerId = static_cast<int>(ws->layerId);
    }

    if (state->enabled && !wasEnabled && layerId >= 0) {
        // Text input just enabled — show the keyboard.
        ALOGI("text_input committed enable: layerId=%d hint=0x%x purpose=%u cursor=(%d,%d %dx%d)",
              layerId, state->contentHint, state->contentPurpose,
              state->pendingCursorX, state->pendingCursorY,
              state->pendingCursorW, state->pendingCursorH);
        self->mCompositor->requestShowTextInput(layerId,
                state->contentHint, state->contentPurpose,
                state->pendingCursorX, state->pendingCursorY,
                state->pendingCursorW, state->pendingCursorH);
    } else if (!state->enabled && wasEnabled && layerId >= 0) {
        // Text input just disabled — hide the keyboard.
        ALOGI("text_input committed disable: layerId=%d", layerId);
        self->mCompositor->requestHideTextInput(layerId);
    } else if (state->enabled && layerId >= 0) {
        // State update while enabled — forward surrounding text and cursor rect.
        if (state->hasPendingSurrounding) {
            self->mCompositor->requestUpdateSurroundingText(layerId,
                    state->pendingSurroundingText.c_str(),
                    state->pendingSurroundingCursor,
                    state->pendingSurroundingAnchor);
        }
        // Always update cursor rectangle if enabled.
        self->mCompositor->requestUpdateCursorRectangle(layerId,
                state->pendingCursorX, state->pendingCursorY,
                state->pendingCursorW, state->pendingCursorH);
    }

    // Reset pending surrounding text state after commit.
    state->hasPendingSurrounding = false;
    state->pendingSurroundingText.clear();
    state->pendingSurroundingCursor = 0;
    state->pendingSurroundingAnchor = 0;
}

// --- Focus management ---

void WaylandTextInput::setFocus(struct wl_resource* surface) {
    if (mFocusedSurface == surface) return;

    struct wl_client* oldClient = mFocusedSurface ? wl_resource_get_client(mFocusedSurface) : nullptr;
    struct wl_client* newClient = surface ? wl_resource_get_client(surface) : nullptr;

    std::lock_guard<std::mutex> lock(mResourcesMutex);

    // Send leave to text inputs of old client.
    if (mFocusedSurface) {
        for (auto* state : mTextInputs) {
            if (wl_resource_get_client(state->resource) == oldClient) {
                zwp_text_input_v3_send_leave(state->resource, mFocusedSurface);
                state->focusedSurface = nullptr;
                state->enabled = false;
                state->pendingEnabled = false;
            }
        }
    }

    mFocusedSurface = surface;

    // Send enter to text inputs of new client.
    if (mFocusedSurface) {
        for (auto* state : mTextInputs) {
            if (wl_resource_get_client(state->resource) == newClient) {
                zwp_text_input_v3_send_enter(state->resource, mFocusedSurface);
                state->focusedSurface = mFocusedSurface;
            }
        }
    }
}

// --- Event dispatch (called on Wayland thread) ---

WaylandTextInput::TextInputState* WaylandTextInput::findActiveTextInput() {
    if (!mFocusedSurface) return nullptr;
    struct wl_client* client = wl_resource_get_client(mFocusedSurface);
    std::lock_guard<std::mutex> lock(mResourcesMutex);
    for (auto* state : mTextInputs) {
        if (state->enabled && wl_resource_get_client(state->resource) == client) {
            return state;
        }
    }
    return nullptr;
}

void WaylandTextInput::sendCommitString(const char* text) {
    auto* state = findActiveTextInput();
    if (!state) return;
    zwp_text_input_v3_send_commit_string(state->resource, text);
}

void WaylandTextInput::sendPreeditString(const char* text, int32_t cursorBegin, int32_t cursorEnd) {
    auto* state = findActiveTextInput();
    if (!state) return;
    zwp_text_input_v3_send_preedit_string(state->resource, text, cursorBegin, cursorEnd);
}

void WaylandTextInput::sendDeleteSurroundingText(uint32_t beforeLength, uint32_t afterLength) {
    auto* state = findActiveTextInput();
    if (!state) return;
    zwp_text_input_v3_send_delete_surrounding_text(state->resource, beforeLength, afterLength);
}

void WaylandTextInput::sendDone() {
    auto* state = findActiveTextInput();
    if (!state) return;
    zwp_text_input_v3_send_done(state->resource, state->commitSerial);
}

} // namespace android
