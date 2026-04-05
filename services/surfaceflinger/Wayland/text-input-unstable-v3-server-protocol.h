/* Generated from text-input-unstable-v3 protocol */

#ifndef TEXT_INPUT_UNSTABLE_V3_SERVER_PROTOCOL_H
#define TEXT_INPUT_UNSTABLE_V3_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

struct zwp_text_input_v3;
struct zwp_text_input_manager_v3;

#ifndef ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_ENUM
#define ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_ENUM
enum zwp_text_input_v3_change_cause {
    ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD = 0,
    ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER = 1,
};
#endif

#ifndef ZWP_TEXT_INPUT_V3_CONTENT_HINT_ENUM
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_ENUM
enum zwp_text_input_v3_content_hint {
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE = 0x0,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_COMPLETION = 0x1,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK = 0x2,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_AUTO_CAPITALIZATION = 0x4,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_LOWERCASE = 0x8,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_UPPERCASE = 0x10,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_TITLECASE = 0x20,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT = 0x40,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA = 0x80,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_LATIN = 0x100,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE = 0x200,
};
#endif

#ifndef ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ENUM
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ENUM
enum zwp_text_input_v3_content_purpose {
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL = 0,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ALPHA = 1,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DIGITS = 2,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER = 3,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PHONE = 4,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL = 5,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_EMAIL = 6,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NAME = 7,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD = 8,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN = 9,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATE = 10,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TIME = 11,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATETIME = 12,
    ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL = 13,
};
#endif

extern const struct wl_interface zwp_text_input_v3_interface;
extern const struct wl_interface zwp_text_input_manager_v3_interface;

/**
 * zwp_text_input_manager_v3 - requests
 */
struct zwp_text_input_manager_v3_interface {
    /** destroy the manager */
    void (*destroy)(struct wl_client *client,
                    struct wl_resource *resource);
    /** create a new text input object */
    void (*get_text_input)(struct wl_client *client,
                           struct wl_resource *resource,
                           uint32_t id,
                           struct wl_resource *seat);
};

/**
 * zwp_text_input_v3 - requests
 */
struct zwp_text_input_v3_interface {
    /** destroy */
    void (*destroy)(struct wl_client *client,
                    struct wl_resource *resource);
    /** enable text input */
    void (*enable)(struct wl_client *client,
                   struct wl_resource *resource);
    /** disable text input */
    void (*disable)(struct wl_client *client,
                    struct wl_resource *resource);
    /** set surrounding text */
    void (*set_surrounding_text)(struct wl_client *client,
                                 struct wl_resource *resource,
                                 const char *text,
                                 int32_t cursor,
                                 int32_t anchor);
    /** set text change cause */
    void (*set_text_change_cause)(struct wl_client *client,
                                  struct wl_resource *resource,
                                  uint32_t cause);
    /** set content type */
    void (*set_content_type)(struct wl_client *client,
                             struct wl_resource *resource,
                             uint32_t hint,
                             uint32_t purpose);
    /** set cursor rectangle */
    void (*set_cursor_rectangle)(struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t x,
                                 int32_t y,
                                 int32_t width,
                                 int32_t height);
    /** commit state */
    void (*commit)(struct wl_client *client,
                   struct wl_resource *resource);
};

/* Event opcodes */
#define ZWP_TEXT_INPUT_V3_ENTER 0
#define ZWP_TEXT_INPUT_V3_LEAVE 1
#define ZWP_TEXT_INPUT_V3_PREEDIT_STRING 2
#define ZWP_TEXT_INPUT_V3_COMMIT_STRING 3
#define ZWP_TEXT_INPUT_V3_DELETE_SURROUNDING_TEXT 4
#define ZWP_TEXT_INPUT_V3_DONE 5

static inline void
zwp_text_input_v3_send_enter(struct wl_resource *resource_,
                              struct wl_resource *surface)
{
    wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_ENTER, surface);
}

static inline void
zwp_text_input_v3_send_leave(struct wl_resource *resource_,
                              struct wl_resource *surface)
{
    wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_LEAVE, surface);
}

static inline void
zwp_text_input_v3_send_preedit_string(struct wl_resource *resource_,
                                       const char *text,
                                       int32_t cursor_begin,
                                       int32_t cursor_end)
{
    wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_PREEDIT_STRING,
                           text, cursor_begin, cursor_end);
}

static inline void
zwp_text_input_v3_send_commit_string(struct wl_resource *resource_,
                                      const char *text)
{
    wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_COMMIT_STRING, text);
}

static inline void
zwp_text_input_v3_send_delete_surrounding_text(struct wl_resource *resource_,
                                                uint32_t before_length,
                                                uint32_t after_length)
{
    wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_DELETE_SURROUNDING_TEXT,
                           before_length, after_length);
}

static inline void
zwp_text_input_v3_send_done(struct wl_resource *resource_,
                             uint32_t serial)
{
    wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_DONE, serial);
}

#ifdef  __cplusplus
}
#endif

#endif
