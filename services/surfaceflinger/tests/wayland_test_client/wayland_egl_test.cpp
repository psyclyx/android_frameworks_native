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

// Wayland EGL test client for the SurfaceFlinger Wayland compositor.
//
// Uses Android-native EGL (EGL_DEFAULT_DISPLAY) to render a spinning triangle
// via GLES2, then exports the rendered GraphicBuffer as a dmabuf and submits
// it to the Wayland compositor via zwp_linux_dmabuf_v1.
//
// This works because Android's EGL only supports EGL_PLATFORM_ANDROID_KHR,
// not EGL_PLATFORM_WAYLAND_KHR. Instead of wl_egl_window, we render to
// EGLImage-backed FBOs created from GraphicBuffers, then export the dmabuf
// fd from the buffer's native handle.

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <linux-dmabuf-unstable-v1-client-protocol.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <ui/GraphicBuffer.h>
#include <cutils/native_handle.h>
#include <drm_fourcc.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace android;

static const int WIDTH = 512;
static const int HEIGHT = 512;
static const int NUM_BUFFERS = 2;

// --- Wayland globals ---

static struct wl_display* wl_dpy;
static struct wl_compositor* wl_comp;
static struct xdg_wm_base* wm_base;
static struct zwp_linux_dmabuf_v1* dmabuf_iface;
static struct wl_surface* wl_surf;
static struct xdg_surface* xdg_surf;
static struct xdg_toplevel* toplevel;
static bool configured = false;
static bool running = true;

// --- Per-buffer state ---

struct RenderBuffer {
    sp<GraphicBuffer> gb;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint fbo = 0;
    GLuint rbo = 0;
    struct wl_buffer* wl_buf = nullptr;
    bool busy = false; // owned by compositor
    int dmabuf_fd = -1;
};

static RenderBuffer buffers[NUM_BUFFERS];
static int current_buf = 0;

// --- EGL state ---

static EGLDisplay egl_display;
static EGLContext egl_context;

// --- GL program ---

static GLuint program;
static GLint loc_pos, loc_color, loc_angle;

// --- xdg_wm_base listener ---

static void wm_base_ping(void*, struct xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

// --- xdg_surface listener ---

static void xdg_surface_configure(void*, struct xdg_surface* surf, uint32_t serial) {
    xdg_surface_ack_configure(surf, serial);
    configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener_impl = {
    .configure = xdg_surface_configure,
};

// --- xdg_toplevel listener ---

static void toplevel_configure(void*, struct xdg_toplevel*, int32_t, int32_t,
                               struct wl_array*) {}

static void toplevel_close(void*, struct xdg_toplevel*) {
    running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

// --- wl_buffer listener ---

static void buffer_release(void* data, struct wl_buffer* /*buf*/) {
    auto* rb = static_cast<RenderBuffer*>(data);
    rb->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

// --- Registry listener ---

static void registry_global(void*, struct wl_registry* registry,
                            uint32_t name, const char* interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl_comp = static_cast<struct wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             version < 4 ? version : 4));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = static_cast<struct xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, nullptr);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        dmabuf_iface = static_cast<struct zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                             version < 3 ? version : 3));
    }
}

static void registry_global_remove(void*, struct wl_registry*, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// --- EGL/GL setup ---

static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES_fn;

static bool init_egl() {
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay(EGL_DEFAULT_DISPLAY) failed\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return false;
    }
    printf("EGL %d.%d initialized\n", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    // We don't need a window surface — we render to FBOs backed by EGLImages.
    // Create a surfaceless context or a 1x1 pbuffer.
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &num_configs) ||
        num_configs == 0) {
        fprintf(stderr, "eglChooseConfig failed\n");
        return false;
    }

    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError());
        return false;
    }

    // Create a tiny pbuffer to make current (we render to FBOs, not this surface).
    EGLint pbuf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface pbuf = eglCreatePbufferSurface(egl_display, config, pbuf_attribs);
    if (pbuf == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreatePbufferSurface failed: 0x%x\n", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(egl_display, pbuf, pbuf, egl_context)) {
        fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
        return false;
    }

    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));

    glEGLImageTargetRenderbufferStorageOES_fn =
        reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
            eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (!glEGLImageTargetRenderbufferStorageOES_fn) {
        fprintf(stderr, "glEGLImageTargetRenderbufferStorageOES not available\n");
        return false;
    }

    return true;
}

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
    }
    return shader;
}

static bool init_gl() {
    const char* vs =
        "attribute vec2 pos;\n"
        "attribute vec3 color;\n"
        "varying vec3 v_color;\n"
        "uniform float angle;\n"
        "void main() {\n"
        "  float c = cos(angle);\n"
        "  float s = sin(angle);\n"
        "  mat2 rot = mat2(c, s, -s, c);\n"
        "  gl_Position = vec4(rot * pos, 0.0, 1.0);\n"
        "  v_color = color;\n"
        "}\n";

    const char* fs =
        "precision mediump float;\n"
        "varying vec3 v_color;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(v_color, 1.0);\n"
        "}\n";

    GLuint vsh = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint fsh = compile_shader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsh);
    glAttachShader(program, fsh);
    glLinkProgram(program);

    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        fprintf(stderr, "Program link error: %s\n", log);
        return false;
    }

    loc_pos = glGetAttribLocation(program, "pos");
    loc_color = glGetAttribLocation(program, "color");
    loc_angle = glGetUniformLocation(program, "angle");
    return true;
}

// --- Buffer allocation ---

static int get_dmabuf_fd(const sp<GraphicBuffer>& gb) {
    const native_handle_t* handle = gb->getNativeBuffer()->handle;
    if (!handle || handle->numFds < 1) {
        return -1;
    }
    // The first fd in the native handle is the dmabuf fd for single-plane formats.
    return handle->data[0];
}

static bool init_buffers() {
    auto eglCreateImageKHR_fn = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    if (!eglCreateImageKHR_fn) {
        fprintf(stderr, "eglCreateImageKHR not available\n");
        return false;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        RenderBuffer& rb = buffers[i];

        // Allocate a GPU-renderable GraphicBuffer.
        // Use ABGR8888 (DRM_FORMAT_ABGR8888) which maps to Android RGBA_8888.
        rb.gb = sp<GraphicBuffer>::make(
            WIDTH, HEIGHT, PIXEL_FORMAT_RGBA_8888, 1u,
            static_cast<uint64_t>(
                GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
                GRALLOC_USAGE_HW_COMPOSER),
            "WaylandEglTest");

        if (rb.gb->initCheck() != NO_ERROR) {
            fprintf(stderr, "GraphicBuffer alloc failed for buffer %d\n", i);
            return false;
        }

        // Get dmabuf fd for Wayland submission.
        rb.dmabuf_fd = get_dmabuf_fd(rb.gb);
        if (rb.dmabuf_fd < 0) {
            fprintf(stderr, "Failed to get dmabuf fd for buffer %d\n", i);
            return false;
        }
        printf("Buffer %d: %dx%d stride=%d dmabuf_fd=%d\n",
               i, WIDTH, HEIGHT, rb.gb->getStride(), rb.dmabuf_fd);

        // Create EGLImage from GraphicBuffer.
        EGLClientBuffer clientBuf = reinterpret_cast<EGLClientBuffer>(rb.gb->getNativeBuffer());
        EGLint img_attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        rb.image = eglCreateImageKHR_fn(egl_display, EGL_NO_CONTEXT,
                                         EGL_NATIVE_BUFFER_ANDROID, clientBuf, img_attrs);
        if (rb.image == EGL_NO_IMAGE_KHR) {
            fprintf(stderr, "eglCreateImageKHR failed for buffer %d: 0x%x\n",
                    i, eglGetError());
            return false;
        }

        // Create FBO + renderbuffer backed by the EGLImage.
        glGenRenderbuffers(1, &rb.rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rb.rbo);
        glEGLImageTargetRenderbufferStorageOES_fn(GL_RENDERBUFFER, rb.image);

        glGenFramebuffers(1, &rb.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, rb.fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, rb.rbo);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "FBO incomplete for buffer %d: 0x%x\n", i, status);
            return false;
        }

        // Create wl_buffer from dmabuf.
        struct zwp_linux_buffer_params_v1* params =
            zwp_linux_dmabuf_v1_create_params(dmabuf_iface);

        uint32_t stride = rb.gb->getStride() * 4; // bytes per row (RGBA = 4 bpp)
        // DRM_FORMAT_ABGR8888 = Android RGBA_8888
        zwp_linux_buffer_params_v1_add(params, rb.dmabuf_fd, 0, 0, stride, 0, 0);
        rb.wl_buf = zwp_linux_buffer_params_v1_create_immed(
            params, WIDTH, HEIGHT, DRM_FORMAT_ABGR8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);

        if (!rb.wl_buf) {
            fprintf(stderr, "Failed to create wl_buffer for buffer %d\n", i);
            return false;
        }

        wl_buffer_add_listener(rb.wl_buf, &buffer_listener, &rb);
        printf("Buffer %d: wl_buffer created, FBO ready\n", i);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

// --- Rendering ---

static void draw_frame(int buf_idx, float angle) {
    RenderBuffer& rb = buffers[buf_idx];
    glBindFramebuffer(GL_FRAMEBUFFER, rb.fbo);
    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);

    const float verts[] = {
         0.0f,  0.6f,  1.0f, 0.0f, 0.0f,
        -0.5f, -0.4f,  0.0f, 1.0f, 0.0f,
         0.5f, -0.4f,  0.0f, 0.0f, 1.0f,
    };

    glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts);
    glEnableVertexAttribArray(loc_pos);
    glVertexAttribPointer(loc_color, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(loc_color);
    glUniform1f(loc_angle, angle);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(loc_pos);
    glDisableVertexAttribArray(loc_color);

    // Ensure rendering is complete before submitting buffer to compositor.
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// --- Frame callback ---

static struct wl_callback* frame_cb;
static int frame_count = 0;

static void frame_done(void* data, struct wl_callback* cb, uint32_t);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void frame_done(void*, struct wl_callback* cb, uint32_t) {
    wl_callback_destroy(cb);

    // Find a free buffer.
    int buf_idx = -1;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        int idx = (current_buf + i) % NUM_BUFFERS;
        if (!buffers[idx].busy) {
            buf_idx = idx;
            break;
        }
    }
    if (buf_idx < 0) {
        // All buffers busy — skip this frame.
        fprintf(stderr, "All buffers busy, skipping frame %d\n", frame_count);
        frame_cb = wl_surface_frame(wl_surf);
        wl_callback_add_listener(frame_cb, &frame_listener, nullptr);
        wl_surface_commit(wl_surf);
        return;
    }

    float angle = frame_count * 0.02f;
    draw_frame(buf_idx, angle);
    frame_count++;
    current_buf = (buf_idx + 1) % NUM_BUFFERS;

    buffers[buf_idx].busy = true;
    wl_surface_attach(wl_surf, buffers[buf_idx].wl_buf, 0, 0);
    wl_surface_damage(wl_surf, 0, 0, WIDTH, HEIGHT);

    frame_cb = wl_surface_frame(wl_surf);
    wl_callback_add_listener(frame_cb, &frame_listener, nullptr);
    wl_surface_commit(wl_surf);

    if (frame_count % 60 == 0) {
        printf("Frame %d\n", frame_count);
    }
}

// --- Main ---

int main(int argc, char** argv) {
    int max_frames = 300;
    if (argc > 1) {
        max_frames = atoi(argv[1]);
        if (max_frames <= 0) max_frames = 300;
    }

    // 1. Connect to Wayland.
    wl_dpy = wl_display_connect(nullptr);
    if (!wl_dpy) {
        fprintf(stderr, "Failed to connect to Wayland display.\n"
                "Set XDG_RUNTIME_DIR=/data/wayland and WAYLAND_DISPLAY=wayland-0\n");
        return 1;
    }
    printf("Connected to Wayland display\n");

    struct wl_registry* registry = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(wl_dpy);

    if (!wl_comp) { fprintf(stderr, "No wl_compositor\n"); return 1; }
    if (!wm_base) { fprintf(stderr, "No xdg_wm_base\n"); return 1; }
    if (!dmabuf_iface) { fprintf(stderr, "No zwp_linux_dmabuf_v1\n"); return 1; }

    // 2. Create surface + xdg_toplevel.
    wl_surf = wl_compositor_create_surface(wl_comp);
    xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, wl_surf);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener_impl, nullptr);
    toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, nullptr);
    xdg_toplevel_set_title(toplevel, "wayland_egl_test");
    wl_surface_commit(wl_surf);
    wl_display_roundtrip(wl_dpy);

    if (!configured) {
        fprintf(stderr, "Surface not configured\n");
        return 1;
    }

    // 3. Initialize Android EGL (default display + pbuffer).
    if (!init_egl()) {
        fprintf(stderr, "EGL init failed\n");
        return 1;
    }

    if (!init_gl()) {
        fprintf(stderr, "GL init failed\n");
        return 1;
    }

    // 4. Allocate render buffers with EGLImage-backed FBOs + wl_buffer from dmabuf.
    if (!init_buffers()) {
        fprintf(stderr, "Buffer init failed\n");
        return 1;
    }

    // 5. Render first frame and kick off frame callback loop.
    draw_frame(0, 0.0f);
    buffers[0].busy = true;
    wl_surface_attach(wl_surf, buffers[0].wl_buf, 0, 0);
    wl_surface_damage(wl_surf, 0, 0, WIDTH, HEIGHT);
    frame_cb = wl_surface_frame(wl_surf);
    wl_callback_add_listener(frame_cb, &frame_listener, nullptr);
    wl_surface_commit(wl_surf);
    frame_count = 1;
    current_buf = 1;

    printf("Rendering %d frames...\n", max_frames);
    while (running && frame_count < max_frames) {
        if (wl_display_dispatch(wl_dpy) < 0) {
            fprintf(stderr, "wl_display_dispatch error\n");
            break;
        }
    }

    printf("Done. Rendered %d frames.\n", frame_count);

    // Cleanup: GL objects first (requires current context), then EGL, then Wayland.
    auto eglDestroyImageKHR_fn = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (buffers[i].fbo) glDeleteFramebuffers(1, &buffers[i].fbo);
        if (buffers[i].rbo) glDeleteRenderbuffers(1, &buffers[i].rbo);
    }
    glDeleteProgram(program);
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (eglDestroyImageKHR_fn && buffers[i].image != EGL_NO_IMAGE_KHR)
            eglDestroyImageKHR_fn(egl_display, buffers[i].image);
        if (buffers[i].wl_buf) wl_buffer_destroy(buffers[i].wl_buf);
    }
    eglDestroyContext(egl_display, egl_context);
    eglTerminate(egl_display);

    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surf);
    wl_surface_destroy(wl_surf);
    wl_display_disconnect(wl_dpy);

    return 0;
}
