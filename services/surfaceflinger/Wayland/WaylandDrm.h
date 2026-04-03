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

namespace android {

class WaylandCompositor;

// Implements the wl_drm protocol (version 2).
// Mesa's EGL Wayland platform uses this to discover the DRM render node
// for GPU-accelerated rendering. The compositor advertises the device path
// and handles the (no-op for render nodes) authentication handshake.
class WaylandDrm {
public:
    static struct wl_global* createGlobal(struct wl_display* display,
                                           WaylandCompositor* compositor);

private:
    static void bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id);
};

} // namespace android
