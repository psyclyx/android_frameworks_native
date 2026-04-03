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
#define LOG_TAG "WaylandDataDevice"

#include "WaylandDataDevice.h"

#include <algorithm>
#include <log/log.h>
#include <wayland-server-protocol.h>

namespace android {

namespace {

constexpr uint32_t kDataDeviceManagerVersion = 3;

// --- wl_data_offer (stub) ---

// --- wl_data_source (stub) ---

void dataSourceOffer(struct wl_client*, struct wl_resource*, const char*) {}
void dataSourceDestroy(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
void dataSourceSetActions(struct wl_client*, struct wl_resource*, uint32_t) {}

const struct wl_data_source_interface kDataSourceImpl = {
    .offer = dataSourceOffer,
    .destroy = dataSourceDestroy,
    .set_actions = dataSourceSetActions,
};

// --- wl_data_device (stub) ---

void dataDeviceStartDrag(struct wl_client*, struct wl_resource*, struct wl_resource*,
                         struct wl_resource*, struct wl_resource*, uint32_t) {}
void dataDeviceSetSelection(struct wl_client*, struct wl_resource*,
                            struct wl_resource*, uint32_t) {}
void dataDeviceRelease(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

const struct wl_data_device_interface kDataDeviceImpl = {
    .start_drag = dataDeviceStartDrag,
    .set_selection = dataDeviceSetSelection,
    .release = dataDeviceRelease,
};

// --- wl_data_device_manager ---

void managerCreateDataSource(struct wl_client* client, struct wl_resource* resource,
                             uint32_t id) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* source =
            wl_resource_create(client, &wl_data_source_interface, ver, id);
    if (!source) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(source, &kDataSourceImpl, nullptr, nullptr);
}

void managerGetDataDevice(struct wl_client* client, struct wl_resource* resource,
                          uint32_t id, struct wl_resource* /*seat*/) {
    int ver = wl_resource_get_version(resource);
    struct wl_resource* device =
            wl_resource_create(client, &wl_data_device_interface, ver, id);
    if (!device) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(device, &kDataDeviceImpl, nullptr, nullptr);
}

const struct wl_data_device_manager_interface kManagerImpl = {
    .create_data_source = managerCreateDataSource,
    .get_data_device = managerGetDataDevice,
};

void bindManager(struct wl_client* client, void* /*data*/, uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kDataDeviceManagerVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &wl_data_device_manager_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kManagerImpl, nullptr, nullptr);
}

} // anonymous namespace

struct wl_global* WaylandDataDevice::createGlobal(struct wl_display* display) {
    return wl_global_create(display, &wl_data_device_manager_interface,
                            kDataDeviceManagerVersion, nullptr, bindManager);
}

} // namespace android
