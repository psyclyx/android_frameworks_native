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
#define LOG_TAG "WaylandLayerShell"

#include "WaylandLayerShell.h"
#include "WaylandCompositor.h"
#include "WaylandSurface.h"

#include <algorithm>

#include <gui/LayerState.h>
#include <gui/TransactionState.h>
#include <log/log.h>

#include "DisplayDevice.h"
#include "SurfaceFlinger.h"

namespace android {

namespace {

constexpr uint32_t kLayerShellVersion = 4; // up to on_demand keyboard interactivity

// Z-order mapping for layer types.  These sit below the xdg_toplevel
// z-value (0x7FFFFFFE) so layer surfaces don't cover normal windows
// unless they're in the overlay layer.
int32_t zOrderForLayer(uint32_t layer) {
    switch (layer) {
        case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return 0x10000000;
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return 0x20000000;
        case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return 0x60000000;
        case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return 0x7FFFFFFF;
        default: return 0x60000000;
    }
}

} // anonymous namespace

void WaylandLayerShell::getDisplaySize(SurfaceFlinger& flinger,
                                        int32_t& outWidth, int32_t& outHeight) {
    outWidth = 1080;
    outHeight = 2400;
    const auto display = flinger.getPacesetterDisplay();
    if (display) {
        outWidth = display->getWidth();
        outHeight = display->getHeight();
    }
}

// --- zwlr_layer_shell_v1 ---

const struct zwlr_layer_shell_v1_interface WaylandLayerShell::kShellImpl = {
        .get_layer_surface = getLayerSurface,
        .destroy = shellDestroy,
};

struct wl_global* WaylandLayerShell::createGlobal(struct wl_display* display,
                                                    WaylandCompositor* compositor) {
    return wl_global_create(display, &zwlr_layer_shell_v1_interface, kLayerShellVersion,
                            compositor, bind);
}

void WaylandLayerShell::bind(struct wl_client* client, void* data,
                              uint32_t version, uint32_t id) {
    int ver = static_cast<int>(std::min(version, kLayerShellVersion));
    struct wl_resource* resource =
            wl_resource_create(client, &zwlr_layer_shell_v1_interface, ver, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kShellImpl, data, nullptr);
    ALOGI("zwlr_layer_shell_v1 bound (v%u)", ver);
}

void WaylandLayerShell::shellDestroy(struct wl_client* /*client*/,
                                      struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandLayerShell::getLayerSurface(struct wl_client* client,
                                         struct wl_resource* resource,
                                         uint32_t id, struct wl_resource* surface,
                                         struct wl_resource* /*output*/, uint32_t layer,
                                         const char* ns) {
    auto* compositor = static_cast<WaylandCompositor*>(wl_resource_get_user_data(resource));
    int ver = wl_resource_get_version(resource);

    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "invalid layer %u", layer);
        return;
    }

    struct wl_resource* lsResource =
            wl_resource_create(client, &zwlr_layer_surface_v1_interface, ver, id);
    if (!lsResource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    auto* ls = new WaylandLayerSurface();
    ls->compositor = compositor;
    ls->resource = lsResource;
    ls->wlSurface = surface;
    ls->layer = layer;
    ls->ns = ns ? ns : "";

    wl_resource_set_implementation(lsResource, &kSurfaceImpl, ls, onLayerSurfaceDestroy);

    // Set z-order for the layer on the underlying SF layer.
    WaylandSurface* ws = compositor->findSurface(surface);
    if (ws) {
        ws->layerSurface = lsResource;

        TransactionState txn;
        ComposerState cs;
        cs.state.what = layer_state_t::eLayerChanged;
        cs.state.surface = ws->handle;
        cs.state.z = zOrderForLayer(layer);
        txn.mComposerStates.push_back(std::move(cs));
        txn.mId = (static_cast<uint64_t>(ws->layerId) << 32) | 0x30000000;
        compositor->flinger().setTransactionState(std::move(txn), nullptr);
    }

    ALOGI("zwlr_layer_surface_v1 created: layer=%u ns='%s'", layer, ls->ns.c_str());
}

// --- zwlr_layer_surface_v1 ---

const struct zwlr_layer_surface_v1_interface WaylandLayerShell::kSurfaceImpl = {
        .set_size = surfSetSize,
        .set_anchor = surfSetAnchor,
        .set_exclusive_zone = surfSetExclusiveZone,
        .set_margin = surfSetMargin,
        .set_keyboard_interactivity = surfSetKeyboardInteractivity,
        .get_popup = surfGetPopup,
        .ack_configure = surfAckConfigure,
        .destroy = surfDestroy,
        .set_layer = surfSetLayer,
        .set_exclusive_edge = surfSetExclusiveEdge,
};

void WaylandLayerShell::surfSetSize(struct wl_client* /*client*/, struct wl_resource* resource,
                                     uint32_t width, uint32_t height) {
    auto* ls = static_cast<WaylandLayerSurface*>(wl_resource_get_user_data(resource));
    ls->desiredWidth = width;
    ls->desiredHeight = height;
}

void WaylandLayerShell::surfSetAnchor(struct wl_client* /*client*/, struct wl_resource* resource,
                                       uint32_t anchor) {
    auto* ls = static_cast<WaylandLayerSurface*>(wl_resource_get_user_data(resource));
    ls->anchor = anchor;
}

void WaylandLayerShell::surfSetExclusiveZone(struct wl_client* /*client*/,
                                              struct wl_resource* resource, int32_t zone) {
    auto* ls = static_cast<WaylandLayerSurface*>(wl_resource_get_user_data(resource));
    ls->exclusiveZone = zone;
}

void WaylandLayerShell::surfSetMargin(struct wl_client* /*client*/, struct wl_resource* resource,
                                       int32_t top, int32_t right, int32_t bottom, int32_t left) {
    auto* ls = static_cast<WaylandLayerSurface*>(wl_resource_get_user_data(resource));
    ls->marginTop = top;
    ls->marginRight = right;
    ls->marginBottom = bottom;
    ls->marginLeft = left;
}

void WaylandLayerShell::surfSetKeyboardInteractivity(struct wl_client* /*client*/,
                                                      struct wl_resource* resource,
                                                      uint32_t interactivity) {
    auto* ls = static_cast<WaylandLayerSurface*>(wl_resource_get_user_data(resource));
    ls->keyboardInteractivity = interactivity;
}

void WaylandLayerShell::surfGetPopup(struct wl_client* /*client*/,
                                      struct wl_resource* /*resource*/,
                                      struct wl_resource* /*popup*/) {
    // Popups on layer surfaces not supported yet.
}

void WaylandLayerShell::surfAckConfigure(struct wl_client* /*client*/,
                                          struct wl_resource* /*resource*/,
                                          uint32_t /*serial*/) {
    // Accept ack; we don't gate commits on configure serial.
}

void WaylandLayerShell::surfDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

void WaylandLayerShell::surfSetLayer(struct wl_client* /*client*/, struct wl_resource* resource,
                                      uint32_t layer) {
    auto* ls = static_cast<WaylandLayerSurface*>(wl_resource_get_user_data(resource));
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
                               "invalid layer %u", layer);
        return;
    }
    ls->layer = layer;
}

void WaylandLayerShell::surfSetExclusiveEdge(struct wl_client* /*client*/,
                                              struct wl_resource* /*resource*/,
                                              uint32_t /*edge*/) {
    // Accepted but not used in layout computation yet.
}

void WaylandLayerShell::onLayerSurfaceDestroy(struct wl_resource* resource) {
    auto* ls = static_cast<WaylandLayerSurface*>(wl_resource_get_user_data(resource));
    if (ls) {
        // Clear the back-reference on the WaylandSurface.
        WaylandSurface* ws = ls->compositor->findSurface(ls->wlSurface);
        if (ws) {
            ws->layerSurface = nullptr;
        }
        // Reset overscan if this surface had an exclusive zone.
        if (ls->exclusiveZone > 0) {
            ls->compositor->requestSetExclusiveZones(0, 0, 0, 0);
        }
    }
    delete ls;
}

void WaylandLayerShell::sendConfigure(WaylandLayerSurface* ls) {
    int32_t displayW, displayH;
    getDisplaySize(ls->compositor->flinger(), displayW, displayH);

    uint32_t w = ls->desiredWidth;
    uint32_t h = ls->desiredHeight;

    const uint32_t anchorH = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    const uint32_t anchorV = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                             ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

    // If anchored to both horizontal edges and width is 0, fill display width.
    if (w == 0 && (ls->anchor & anchorH) == anchorH) {
        w = static_cast<uint32_t>(displayW) -
            static_cast<uint32_t>(ls->marginLeft + ls->marginRight);
    }
    // If anchored to both vertical edges and height is 0, fill display height.
    if (h == 0 && (ls->anchor & anchorV) == anchorV) {
        h = static_cast<uint32_t>(displayH) -
            static_cast<uint32_t>(ls->marginTop + ls->marginBottom);
    }

    ls->configureSerial++;
    zwlr_layer_surface_v1_send_configure(ls->resource, ls->configureSerial, w, h);
    ls->configured = true;

    ALOGI("layer_surface configure: serial=%u size=%ux%u anchor=0x%x",
          ls->configureSerial, w, h, ls->anchor);
}

void WaylandLayerShell::applyLayout(WaylandLayerSurface* ls) {
    WaylandSurface* ws = ls->compositor->findSurface(ls->wlSurface);
    if (!ws) return;

    int32_t displayW, displayH;
    getDisplaySize(ls->compositor->flinger(), displayW, displayH);

    // Determine surface size — use committed buffer size if desired size is 0.
    int32_t w = ls->desiredWidth > 0 ? static_cast<int32_t>(ls->desiredWidth) : 0;
    int32_t h = ls->desiredHeight > 0 ? static_cast<int32_t>(ls->desiredHeight) : 0;

    const uint32_t anchorH = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    const uint32_t anchorV = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                             ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

    if (w == 0 && (ls->anchor & anchorH) == anchorH) {
        w = displayW - ls->marginLeft - ls->marginRight;
    }
    if (h == 0 && (ls->anchor & anchorV) == anchorV) {
        h = displayH - ls->marginTop - ls->marginBottom;
    }

    // Compute position based on anchor and margins.
    int32_t x = 0, y = 0;

    if ((ls->anchor & anchorH) == anchorH) {
        // Horizontally stretched — position at left margin.
        x = ls->marginLeft;
    } else if (ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
        x = ls->marginLeft;
    } else if (ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
        x = displayW - w - ls->marginRight;
    } else {
        // Centered horizontally.
        x = (displayW - w) / 2;
    }

    if ((ls->anchor & anchorV) == anchorV) {
        // Vertically stretched — position at top margin.
        y = ls->marginTop;
    } else if (ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
        y = ls->marginTop;
    } else if (ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
        y = displayH - h - ls->marginBottom;
    } else {
        // Centered vertically.
        y = (displayH - h) / 2;
    }

    // Update z-order (layer may have changed via set_layer).
    int32_t z = zOrderForLayer(ls->layer);

    TransactionState txn;
    ComposerState cs;
    cs.state.what = layer_state_t::ePositionChanged | layer_state_t::eLayerChanged
                  | layer_state_t::eAlphaChanged;
    cs.state.surface = ws->handle;
    cs.state.x = static_cast<float>(x);
    cs.state.y = static_cast<float>(y);
    cs.state.z = z;
    cs.state.color.a = 1.0f; // unhide: layer was created with alpha=0
    txn.mComposerStates.push_back(std::move(cs));
    txn.mId = (static_cast<uint64_t>(ws->layerId) << 32) | 0x31000000;
    ls->compositor->flinger().setTransactionState(std::move(txn), nullptr);

    ALOGD("layer_surface layout: pos=(%d,%d) z=%d layer=%u anchor=0x%x",
          x, y, z, ls->layer, ls->anchor);

    // Apply exclusive zone to Android display overscan.
    if (ls->exclusiveZone > 0) {
        int32_t ezTop = 0, ezRight = 0, ezBottom = 0, ezLeft = 0;
        // Determine which edge the exclusive zone applies to.
        bool anchorT = ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        bool anchorB = ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        bool anchorL = ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        bool anchorR = ls->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

        if (anchorT && !anchorB) ezTop = ls->exclusiveZone;
        else if (anchorB && !anchorT) ezBottom = ls->exclusiveZone;
        else if (anchorL && !anchorR) ezLeft = ls->exclusiveZone;
        else if (anchorR && !anchorL) ezRight = ls->exclusiveZone;
        // If anchored to both edges of an axis (e.g. top+bottom), exclusive zone
        // is meaningless per the protocol spec.

        if (ezTop || ezRight || ezBottom || ezLeft) {
            ls->compositor->requestSetExclusiveZones(ezTop, ezRight, ezBottom, ezLeft);
        }
    }
}

} // namespace android
