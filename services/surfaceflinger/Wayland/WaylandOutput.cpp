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
#define LOG_TAG "WaylandOutput"

#include "WaylandOutput.h"

#include <algorithm>

#include <log/log.h>
#include <wayland-server-protocol.h>

#include "DisplayDevice.h"
#include "SurfaceFlinger.h"

namespace android {

namespace {
constexpr uint32_t kOutputVersion = 2; // geometry + mode + done + scale
} // anonymous namespace

const struct wl_output_interface WaylandOutput::kImpl = {
        .release = WaylandOutput::outputRelease,
};

struct wl_global* WaylandOutput::createGlobal(struct wl_display* display,
                                               SurfaceFlinger* flinger) {
    return wl_global_create(display, &wl_output_interface, kOutputVersion,
                            flinger, bind);
}

void WaylandOutput::bind(struct wl_client* client, void* data,
                          uint32_t version, uint32_t id) {
    auto* flinger = static_cast<SurfaceFlinger*>(data);
    int ver = static_cast<int>(std::min(version, kOutputVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &wl_output_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kImpl, flinger, nullptr);

    // Query display info from SF.
    int32_t width = 1920;
    int32_t height = 1080;
    int32_t refreshMHz = 60000;

    const auto display = flinger->getPacesetterDisplay();
    if (display) {
        width = display->getWidth();
        height = display->getHeight();
        const auto& mode = display->refreshRateSelector().getActiveMode();
        refreshMHz = static_cast<int32_t>(mode.fps.getValue() * 1000.0f);
    }

    wl_output_send_geometry(resource,
                            0, 0,           // x, y position
                            0, 0,           // physical size in mm (unknown)
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "Android",
                            "SurfaceFlinger",
                            WL_OUTPUT_TRANSFORM_NORMAL);

    wl_output_send_mode(resource,
                        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        width, height,
                        refreshMHz);

    if (ver >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        // Scale based on resolution: phones with >=1080px width need 2x
        // to make Wayland app content usable at phone DPI.
        int32_t scale = (width >= 1080) ? 2 : 1;
        wl_output_send_scale(resource, scale);
    }

    if (ver >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
    }

    ALOGI("wl_output bound (v%u): %dx%d @ %d mHz", ver, width, height, refreshMHz);
}

void WaylandOutput::outputRelease(struct wl_client* /*client*/,
                                   struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

} // namespace android
