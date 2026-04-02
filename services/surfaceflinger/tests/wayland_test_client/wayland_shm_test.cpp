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

// Minimal Wayland SHM test client for the SurfaceFlinger Wayland compositor.
// CPU-rendered: no EGL, no GPU. Uses wl_shm to share pixel buffers.
// Renders animated color bars to validate the wl_shm → GraphicBuffer → Layer path.

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

static const int WIDTH = 256;
static const int HEIGHT = 256;
static const int STRIDE = WIDTH * 4; // ARGB8888
static const int BUF_SIZE = STRIDE * HEIGHT * 2; // double-buffered

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_surface* surface;
static struct xdg_surface* xdg_surf;
static struct xdg_toplevel* toplevel;

static bool configured = false;
static bool running = true;
static int frame_count = 0;

static int shm_fd = -1;
static uint8_t* shm_data = nullptr;
static struct wl_shm_pool* pool = nullptr;
static struct wl_buffer* buffers[2] = {};
static bool buffer_busy[2] = {};
static int current_buf = 0;

// --- Shared memory allocation ---

static int create_shm_file(int size) {
    // memfd_create is available on Android/Linux
    int fd = memfd_create("wayland_shm_test", MFD_CLOEXEC);
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

// --- Buffer release listener ---

static void buffer_release(void* data, struct wl_buffer* /*buffer*/) {
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(data));
    buffer_busy[idx] = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

// --- wl_shm listener ---

static void shm_format(void* /*data*/, struct wl_shm* /*shm*/, uint32_t format) {
    if (format == WL_SHM_FORMAT_ARGB8888) {
        printf("  wl_shm: ARGB8888 supported\n");
    } else if (format == WL_SHM_FORMAT_XRGB8888) {
        printf("  wl_shm: XRGB8888 supported\n");
    }
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

// --- xdg listeners ---

static void wm_base_ping(void* /*data*/, struct xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void* /*data*/, struct xdg_surface* surf, uint32_t serial) {
    xdg_surface_ack_configure(surf, serial);
    configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener_impl = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void* /*data*/, struct xdg_toplevel* /*tl*/,
                               int32_t /*width*/, int32_t /*height*/,
                               struct wl_array* /*states*/) {
}

static void toplevel_close(void* /*data*/, struct xdg_toplevel* /*tl*/) {
    running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

// --- Registry listener ---

static void registry_global(void* /*data*/, struct wl_registry* registry,
                            uint32_t name, const char* interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = static_cast<struct wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             version < 4 ? version : 4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = static_cast<struct wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1));
        wl_shm_add_listener(shm, &shm_listener, nullptr);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = static_cast<struct xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, nullptr);
    }
}

static void registry_global_remove(void* /*data*/, struct wl_registry* /*registry*/,
                                   uint32_t /*name*/) {
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// --- Rendering ---

static void draw_frame(int buf_idx, int frame) {
    uint32_t* pixels = reinterpret_cast<uint32_t*>(
        shm_data + buf_idx * STRIDE * HEIGHT);

    // Animated vertical color bars that shift each frame
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int bar = ((x + frame * 2) / 32) % 4;
            uint32_t color;
            switch (bar) {
                case 0: color = 0xFFFF0000; break; // red
                case 1: color = 0xFF00FF00; break; // green
                case 2: color = 0xFF0000FF; break; // blue
                default: color = 0xFFFFFF00; break; // yellow
            }
            // Add a horizontal gradient for visual interest
            uint32_t r = (color >> 16) & 0xFF;
            uint32_t g = (color >> 8) & 0xFF;
            uint32_t b = color & 0xFF;
            uint32_t fade = static_cast<uint32_t>(255 * y / HEIGHT);
            r = r * fade / 255;
            g = g * fade / 255;
            b = b * fade / 255;
            pixels[y * WIDTH + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

// --- Frame callback ---

static void frame_done(void* data, struct wl_callback* cb, uint32_t /*time*/);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void submit_frame() {
    // Find a free buffer (prefer alternating)
    int idx = current_buf;
    if (buffer_busy[idx]) {
        idx = 1 - idx;
        if (buffer_busy[idx]) {
            // Both busy — skip frame
            return;
        }
    }
    current_buf = 1 - idx;

    draw_frame(idx, frame_count);
    buffer_busy[idx] = true;

    wl_surface_attach(surface, buffers[idx], 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);

    struct wl_callback* cb = wl_surface_frame(surface);
    wl_callback_add_listener(cb, &frame_listener, nullptr);

    wl_surface_commit(surface);
    frame_count++;

    if (frame_count % 60 == 0) {
        printf("Frame %d\n", frame_count);
    }
}

static void frame_done(void* /*data*/, struct wl_callback* cb, uint32_t /*time*/) {
    wl_callback_destroy(cb);
    submit_frame();
}

// --- Main ---

int main(int argc, char** argv) {
    int max_frames = 300;
    if (argc > 1) {
        max_frames = atoi(argv[1]);
        if (max_frames <= 0) max_frames = 300;
    }

    display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "Failed to connect to Wayland display.\n"
                "Set XDG_RUNTIME_DIR=/data/wayland and WAYLAND_DISPLAY=wayland-0\n");
        return 1;
    }
    printf("Connected to Wayland display\n");

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(display);

    if (!compositor) { fprintf(stderr, "No wl_compositor\n"); return 1; }
    if (!shm) { fprintf(stderr, "No wl_shm\n"); return 1; }
    if (!wm_base) { fprintf(stderr, "No xdg_wm_base\n"); return 1; }

    // Do another roundtrip to receive wl_shm format events
    wl_display_roundtrip(display);

    // Create SHM pool
    shm_fd = create_shm_file(BUF_SIZE);
    if (shm_fd < 0) return 1;

    shm_data = static_cast<uint8_t*>(
        mmap(nullptr, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (shm_data == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return 1;
    }

    pool = wl_shm_create_pool(shm, shm_fd, BUF_SIZE);
    if (!pool) { fprintf(stderr, "wl_shm_create_pool failed\n"); return 1; }

    // Create double buffers from the pool
    buffers[0] = wl_shm_pool_create_buffer(pool, 0,
        WIDTH, HEIGHT, STRIDE, WL_SHM_FORMAT_ARGB8888);
    buffers[1] = wl_shm_pool_create_buffer(pool, STRIDE * HEIGHT,
        WIDTH, HEIGHT, STRIDE, WL_SHM_FORMAT_ARGB8888);

    wl_buffer_add_listener(buffers[0], &buffer_listener, reinterpret_cast<void*>(0));
    wl_buffer_add_listener(buffers[1], &buffer_listener, reinterpret_cast<void*>(1));

    printf("SHM pool created: %d bytes, 2 buffers (%dx%d ARGB8888)\n",
           BUF_SIZE, WIDTH, HEIGHT);

    // Create surface
    surface = wl_compositor_create_surface(compositor);
    xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener_impl, nullptr);

    toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, nullptr);
    xdg_toplevel_set_title(toplevel, "wayland_shm_test");

    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    if (!configured) {
        fprintf(stderr, "Surface not configured after roundtrip\n");
        return 1;
    }

    // Submit first frame
    printf("Rendering %d frames (SHM, CPU-rendered)...\n", max_frames);
    submit_frame();

    while (running && frame_count < max_frames) {
        if (wl_display_dispatch(display) < 0) {
            fprintf(stderr, "wl_display_dispatch error\n");
            break;
        }
    }

    printf("Done. Rendered %d frames.\n", frame_count);

    // Cleanup
    wl_buffer_destroy(buffers[0]);
    wl_buffer_destroy(buffers[1]);
    wl_shm_pool_destroy(pool);
    munmap(shm_data, BUF_SIZE);
    close(shm_fd);

    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surf);
    wl_surface_destroy(surface);
    wl_shm_destroy(shm);
    wl_display_disconnect(display);

    return 0;
}
