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
#define LOG_TAG "WaylandDmabuf"

#include "WaylandDmabuf.h"
#include "WaylandCompositor.h"

#include <algorithm>

#include <errno.h>
#include <drm_fourcc.h>
#include <linux-dmabuf-unstable-v1-server-protocol.h>
#include <log/log.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace android {

namespace {
constexpr uint32_t kDmabufVersion = 4;
constexpr const char* kRenderNode = "/dev/dri/renderD128";
constexpr uint32_t kMaxPlanes = 4;

// Formats we advertise. Keep minimal for MVP.
struct FormatModifier {
    uint32_t format;
    uint64_t modifier;
};

const FormatModifier kSupportedFormats[] = {
        {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR},
        {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR},
        {DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR},
        {DRM_FORMAT_XBGR8888, DRM_FORMAT_MOD_LINEAR},
        {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID},
        {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID},
        {DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_INVALID},
        {DRM_FORMAT_XBGR8888, DRM_FORMAT_MOD_INVALID},
};
} // anonymous namespace

// --- wl_buffer interface for dmabuf-backed buffers ---

const struct wl_buffer_interface WaylandDmabufBuffer::kBufferImpl = {
        .destroy = WaylandDmabufBuffer::bufferDestroy,
};

void WaylandDmabufBuffer::bufferDestroy(struct wl_client* /*client*/,
                                         struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandDmabufBuffer::onBufferDestroy(struct wl_resource* resource) {
    auto* buf = static_cast<WaylandDmabufBuffer*>(wl_resource_get_user_data(resource));
    if (buf) {
        if (buf->compositor) {
            buf->compositor->notifyBufferDestroyed(resource);
        }
        for (auto& plane : buf->planes) {
            if (plane.fd >= 0) {
                close(plane.fd);
                plane.fd = -1;
            }
        }
        delete buf;
    }
}

// --- zwp_linux_dmabuf_v1 interface ---

const struct zwp_linux_dmabuf_v1_interface WaylandDmabuf::kDmabufImpl = {
        .destroy = WaylandDmabuf::dmabufDestroy,
        .create_params = WaylandDmabuf::dmabufCreateParams,
        .get_default_feedback = WaylandDmabuf::dmabufGetDefaultFeedback,
        .get_surface_feedback = WaylandDmabuf::dmabufGetSurfaceFeedback,
};

struct wl_global* WaylandDmabuf::createGlobal(struct wl_display* display,
                                                WaylandCompositor* compositor) {
    return wl_global_create(display, &zwp_linux_dmabuf_v1_interface,
                            kDmabufVersion, compositor, bind);
}

void WaylandDmabuf::bind(struct wl_client* client, void* data,
                          uint32_t version, uint32_t id) {
    auto* compositor = static_cast<WaylandCompositor*>(data);
    int ver = static_cast<int>(std::min(version, kDmabufVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kDmabufImpl, compositor, nullptr);

    // v4+: format/modifier events are deprecated; clients use get_default_feedback.
    if (ver < 4) {
        for (const auto& fm : kSupportedFormats) {
            zwp_linux_dmabuf_v1_send_format(resource, fm.format);
            if (ver >= 3) {
                zwp_linux_dmabuf_v1_send_modifier(resource, fm.format,
                                                   fm.modifier >> 32,
                                                   fm.modifier & 0xFFFFFFFF);
            }
        }
    }

    ALOGI("zwp_linux_dmabuf_v1 bound (v%u)", ver);
}

void WaylandDmabuf::dmabufDestroy(struct wl_client* /*client*/,
                                   struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandDmabuf::dmabufCreateParams(struct wl_client* client,
                                         struct wl_resource* resource,
                                         uint32_t paramsId) {
    auto* compositor = static_cast<WaylandCompositor*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);

    struct wl_resource* paramsResource =
            wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, ver, paramsId);
    if (!paramsResource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* params = new ParamsData();
    params->compositor = compositor;
    params->paramsResource = paramsResource;

    wl_resource_set_implementation(paramsResource, &kParamsImpl, params, onParamsDestroy);
}

// --- zwp_linux_buffer_params_v1 interface ---

const struct zwp_linux_buffer_params_v1_interface WaylandDmabuf::kParamsImpl = {
        .destroy = WaylandDmabuf::paramsDestroy,
        .add = WaylandDmabuf::paramsAdd,
        .create = WaylandDmabuf::paramsCreate,
        .create_immed = WaylandDmabuf::paramsCreateImmed,
};

void WaylandDmabuf::onParamsDestroy(struct wl_resource* resource) {
    auto* params = static_cast<ParamsData*>(wl_resource_get_user_data(resource));
    if (params) {
        // Close any fds that weren't consumed
        for (auto& plane : params->planes) {
            if (plane.fd >= 0) {
                close(plane.fd);
            }
        }
        delete params;
    }
}

void WaylandDmabuf::paramsDestroy(struct wl_client* /*client*/,
                                   struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandDmabuf::paramsAdd(struct wl_client* /*client*/, struct wl_resource* resource,
                               int32_t fd, uint32_t planeIdx, uint32_t offset,
                               uint32_t stride, uint32_t modifierHi, uint32_t modifierLo) {
    auto* params = static_cast<ParamsData*>(wl_resource_get_user_data(resource));

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params already used");
        close(fd);
        return;
    }

    if (planeIdx >= kMaxPlanes) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane index %u too large", planeIdx);
        close(fd);
        return;
    }

    // Grow planes vector if needed
    if (params->planes.size() <= planeIdx) {
        params->planes.resize(planeIdx + 1);
    }

    if (params->planes[planeIdx].fd >= 0) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane %u already set", planeIdx);
        close(fd);
        return;
    }

    WaylandDmabufBuffer::Plane& plane = params->planes[planeIdx];
    plane.fd = fd;
    plane.offset = offset;
    plane.stride = stride;
    plane.modifier = (static_cast<uint64_t>(modifierHi) << 32) | modifierLo;
}

struct wl_resource* WaylandDmabuf::createBuffer(struct wl_client* client, uint32_t bufferId,
                                                  ParamsData* params, int32_t width,
                                                  int32_t height, uint32_t format,
                                                  uint32_t flags) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    // Verify all planes have valid fds (no gaps)
    for (size_t i = 0; i < params->planes.size(); i++) {
        if (params->planes[i].fd < 0) {
            return nullptr;
        }
    }

    if (params->planes.empty()) {
        return nullptr;
    }

    // Create the wl_buffer resource
    struct wl_resource* bufferResource =
            wl_resource_create(client, &wl_buffer_interface, 1, bufferId);
    if (!bufferResource) {
        return nullptr;
    }

    // Create buffer metadata — dup the fds so params can be destroyed independently
    auto* buf = new WaylandDmabufBuffer();
    buf->bufferType = WaylandBufferType::Dmabuf;
    buf->compositor = params->compositor;
    buf->width = width;
    buf->height = height;
    buf->format = format;
    buf->flags = flags;
    buf->planes.resize(params->planes.size());
    for (size_t i = 0; i < params->planes.size(); i++) {
        int dupFd = dup(params->planes[i].fd);
        if (dupFd < 0) {
            ALOGE("dup() failed for plane %zu: %s", i, strerror(errno));
            // Close already-duped fds and clean up
            for (size_t j = 0; j < i; j++) {
                close(buf->planes[j].fd);
            }
            delete buf;
            wl_resource_destroy(bufferResource);
            return nullptr;
        }
        buf->planes[i].fd = dupFd;
        buf->planes[i].offset = params->planes[i].offset;
        buf->planes[i].stride = params->planes[i].stride;
        buf->planes[i].modifier = params->planes[i].modifier;
    }

    wl_resource_set_implementation(bufferResource, &WaylandDmabufBuffer::kBufferImpl,
                                    buf, WaylandDmabufBuffer::onBufferDestroy);

    ALOGD("Created dmabuf wl_buffer: %dx%d fmt=0x%08x planes=%zu",
          width, height, format, buf->planes.size());
    return bufferResource;
}

void WaylandDmabuf::paramsCreate(struct wl_client* client, struct wl_resource* resource,
                                  int32_t width, int32_t height, uint32_t format,
                                  uint32_t flags) {
    auto* params = static_cast<ParamsData*>(wl_resource_get_user_data(resource));

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params already used");
        return;
    }
    params->used = true;

    struct wl_resource* buffer = createBuffer(client, 0, params, width, height, format, flags);
    if (buffer) {
        zwp_linux_buffer_params_v1_send_created(resource, buffer);
    } else {
        zwp_linux_buffer_params_v1_send_failed(resource);
    }
}

void WaylandDmabuf::paramsCreateImmed(struct wl_client* client, struct wl_resource* resource,
                                        uint32_t bufferId, int32_t width, int32_t height,
                                        uint32_t format, uint32_t flags) {
    auto* params = static_cast<ParamsData*>(wl_resource_get_user_data(resource));

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params already used");
        return;
    }
    params->used = true;

    struct wl_resource* buffer = createBuffer(client, bufferId, params, width, height,
                                               format, flags);
    if (!buffer) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                               "failed to create buffer");
    }
}

// --- zwp_linux_dmabuf_feedback_v1 ---

static void feedbackDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface kFeedbackImpl = {
        .destroy = feedbackDestroy,
};

// Send all feedback events on a newly created feedback resource.
static void sendFeedback(struct wl_resource* resource) {
    // 1. Build format table: packed array of {uint32 format, uint32 pad, uint64 modifier}.
    const size_t entrySize = 16;
    const size_t tableSize = std::size(kSupportedFormats) * entrySize;
    int tableFd = static_cast<int>(
            syscall(__NR_memfd_create, "dmabuf_fmt_table", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (tableFd < 0) {
        ALOGE("memfd_create failed: %s", strerror(errno));
        return;
    }
    if (ftruncate(tableFd, tableSize) != 0) {
        ALOGE("ftruncate failed: %s", strerror(errno));
        close(tableFd);
        return;
    }
    void* map = mmap(nullptr, tableSize, PROT_READ | PROT_WRITE, MAP_SHARED, tableFd, 0);
    if (map == MAP_FAILED) {
        ALOGE("mmap failed: %s", strerror(errno));
        close(tableFd);
        return;
    }
    uint8_t* ptr = static_cast<uint8_t*>(map);
    for (const auto& fm : kSupportedFormats) {
        uint32_t format = fm.format;
        uint32_t pad = 0;
        uint64_t modifier = fm.modifier;
        memcpy(ptr, &format, 4);
        memcpy(ptr + 4, &pad, 4);
        memcpy(ptr + 8, &modifier, 8);
        ptr += entrySize;
    }
    munmap(map, tableSize);

    zwp_linux_dmabuf_feedback_v1_send_format_table(resource, tableFd, tableSize);
    close(tableFd);

    // 2. Get dev_t for the DRM render node.
    struct stat st;
    if (stat(kRenderNode, &st) != 0) {
        ALOGE("stat(%s) failed: %s", kRenderNode, strerror(errno));
        return;
    }
    dev_t dev = st.st_rdev;
    struct wl_array devArray;
    wl_array_init(&devArray);
    memcpy(wl_array_add(&devArray, sizeof(dev)), &dev, sizeof(dev));

    // 3. Send main_device.
    zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &devArray);

    // 4. Send a single tranche.
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &devArray);

    // Build indices array (all format table entries).
    struct wl_array indicesArray;
    wl_array_init(&indicesArray);
    for (uint16_t i = 0; i < static_cast<uint16_t>(std::size(kSupportedFormats)); i++) {
        uint16_t* slot = static_cast<uint16_t*>(wl_array_add(&indicesArray, sizeof(uint16_t)));
        *slot = i;
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &indicesArray);
    wl_array_release(&indicesArray);

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);

    wl_array_release(&devArray);

    // 5. Signal done.
    zwp_linux_dmabuf_feedback_v1_send_done(resource);

    ALOGI("Sent dmabuf feedback: device=%s, %zu format-modifier pairs",
          kRenderNode, std::size(kSupportedFormats));
}

void WaylandDmabuf::dmabufGetDefaultFeedback(struct wl_client* client,
                                               struct wl_resource* resource,
                                               uint32_t id) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* feedback =
            wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, ver, id);
    if (!feedback) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(feedback, &kFeedbackImpl, nullptr, nullptr);
    sendFeedback(feedback);
}

void WaylandDmabuf::dmabufGetSurfaceFeedback(struct wl_client* client,
                                               struct wl_resource* resource,
                                               uint32_t id,
                                               struct wl_resource* /*surface*/) {
    // Same as default feedback for now.
    dmabufGetDefaultFeedback(client, resource, id);
}

} // namespace android
