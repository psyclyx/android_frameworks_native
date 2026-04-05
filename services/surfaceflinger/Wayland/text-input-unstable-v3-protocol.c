/* Generated from text-input-unstable-v3 protocol */

#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface wl_seat_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface zwp_text_input_v3_interface;

/* zwp_text_input_manager_v3 */

static const struct wl_interface *text_input_manager_types[] = {
    &zwp_text_input_v3_interface,
    &wl_seat_interface,
};

/* destroy(), get_text_input(id<text_input>, seat<wl_seat>) */
static const struct wl_message text_input_manager_requests[] = {
    { "destroy", "", NULL },
    { "get_text_input", "no", text_input_manager_types },
};

const struct wl_interface zwp_text_input_manager_v3_interface = {
    "zwp_text_input_manager_v3", 1,
    2, text_input_manager_requests,
    0, NULL,
};

/* zwp_text_input_v3 */

static const struct wl_interface *text_input_types[] = {
    &wl_surface_interface,
};

/*
 * Requests:
 *   destroy()
 *   enable()
 *   disable()
 *   set_surrounding_text(text: string, cursor: int, anchor: int)
 *   set_text_change_cause(cause: uint)
 *   set_content_type(hint: uint, purpose: uint)
 *   set_cursor_rectangle(x: int, y: int, width: int, height: int)
 *   commit()
 */
static const struct wl_message text_input_requests[] = {
    { "destroy", "", NULL },
    { "enable", "", NULL },
    { "disable", "", NULL },
    { "set_surrounding_text", "sii", NULL },
    { "set_text_change_cause", "u", NULL },
    { "set_content_type", "uu", NULL },
    { "set_cursor_rectangle", "iiii", NULL },
    { "commit", "", NULL },
};

/*
 * Events:
 *   enter(surface: object<wl_surface>)
 *   leave(surface: object<wl_surface>)
 *   preedit_string(text: string|null, cursor_begin: int, cursor_end: int)
 *   commit_string(text: string|null)
 *   delete_surrounding_text(before_length: uint, after_length: uint)
 *   done(serial: uint)
 */
static const struct wl_message text_input_events[] = {
    { "enter", "o", text_input_types },
    { "leave", "o", text_input_types },
    { "preedit_string", "?sii", NULL },
    { "commit_string", "?s", NULL },
    { "delete_surrounding_text", "uu", NULL },
    { "done", "u", NULL },
};

const struct wl_interface zwp_text_input_v3_interface = {
    "zwp_text_input_v3", 1,
    8, text_input_requests,
    6, text_input_events,
};
