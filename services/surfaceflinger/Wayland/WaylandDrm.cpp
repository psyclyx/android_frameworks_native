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
#define LOG_TAG "WaylandDrm"

#include "WaylandDrm.h"
#include "wayland-drm-server-protocol.h"

#include <algorithm>
#include <drm_fourcc.h>
#include <log/log.h>
#include <unistd.h>

// Defined in wayland-drm-protocol.c — avoids C++/C name collision between
// the 'struct wl_drm_interface' vtable type and the 'wl_drm_interface' variable.
extern "C" const struct wl_interface* wl_drm_interface_ptr();

namespace android {

namespace {

constexpr uint32_t kDrmVersion = 2;
constexpr const char* kRenderNode = "/dev/dri/renderD128";

const uint32_t kFormats[] = {
        DRM_FORMAT_ARGB8888,
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ABGR8888,
        DRM_FORMAT_XBGR8888,
};

// --- wl_drm request handlers (file-local) ---

void drmAuthenticate(struct wl_client* /*client*/,
                     struct wl_resource* resource,
                     uint32_t /*id*/) {
    // Render nodes do not require DRM authentication — just confirm.
    wl_drm_send_authenticated(resource);
}

void drmCreateBuffer(struct wl_client* /*client*/,
                     struct wl_resource* resource,
                     uint32_t /*id*/, uint32_t /*name*/,
                     int32_t /*width*/, int32_t /*height*/,
                     uint32_t /*stride*/, uint32_t /*format*/) {
    wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_NAME,
                           "wl_drm.create_buffer not supported, use linux-dmabuf");
}

void drmCreatePlanarBuffer(struct wl_client* /*client*/,
                           struct wl_resource* resource,
                           uint32_t /*id*/, uint32_t /*name*/,
                           int32_t /*width*/, int32_t /*height*/,
                           uint32_t /*format*/,
                           int32_t /*offset0*/, int32_t /*stride0*/,
                           int32_t /*offset1*/, int32_t /*stride1*/,
                           int32_t /*offset2*/, int32_t /*stride2*/) {
    wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_NAME,
                           "wl_drm.create_planar_buffer not supported, use linux-dmabuf");
}

void drmCreatePrimeBuffer(struct wl_client* /*client*/,
                          struct wl_resource* resource,
                          uint32_t /*id*/, int32_t fd,
                          int32_t /*width*/, int32_t /*height*/,
                          uint32_t /*format*/,
                          int32_t /*offset0*/, int32_t /*stride0*/,
                          int32_t /*offset1*/, int32_t /*stride1*/,
                          int32_t /*offset2*/, int32_t /*stride2*/) {
    close(fd);
    wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_NAME,
                           "wl_drm.create_prime_buffer not supported, use linux-dmabuf");
}

const struct wl_drm_interface kDrmImpl = {
        .authenticate = drmAuthenticate,
        .create_buffer = drmCreateBuffer,
        .create_planar_buffer = drmCreatePlanarBuffer,
        .create_prime_buffer = drmCreatePrimeBuffer,
};

} // anonymous namespace

struct wl_global* WaylandDrm::createGlobal(struct wl_display* display,
                                             WaylandCompositor* compositor) {
    return wl_global_create(display, wl_drm_interface_ptr(), kDrmVersion, compositor, bind);
}

void WaylandDrm::bind(struct wl_client* client, void* data,
                       uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kDrmVersion));
    struct wl_resource* resource =
            wl_resource_create(client, wl_drm_interface_ptr(), ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kDrmImpl, data, nullptr);

    wl_drm_send_device(resource, kRenderNode);

    for (uint32_t fmt : kFormats) {
        wl_drm_send_format(resource, fmt);
    }

    if (ver >= 2) {
        wl_drm_send_capabilities(resource, WL_DRM_CAPABILITY_PRIME);
    }

    ALOGI("wl_drm bound (v%d), device=%s", ver, kRenderNode);
}

} // namespace android
