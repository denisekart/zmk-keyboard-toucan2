#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/mouse_mover_state_changed.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/display/widgets/battery_status.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/split/central.h>

#if defined(CONFIG_TOUCAN_STATUS_SCREEN) && CONFIG_TOUCAN_STATUS_SCREEN == 2
#include "battery_arc.h"
#include "battery_arc_peripheral.h"
#include "profile_arc.h"
#include "output_arc.h"
#include "chart.h"
#else
#include "battery.h"
#include "battery_peripheral.h"
#include "profile.h"
#include "output.h"
#endif

#if defined(CONFIG_TOUCAN_STATUS_SCREEN) && CONFIG_TOUCAN_STATUS_SCREEN == 1
#include "layer_logo.h"
#elif defined(CONFIG_TOUCAN_STATUS_SCREEN) && CONFIG_TOUCAN_STATUS_SCREEN == 2
#include "layer_arc.h"
#else
#include "layer.h"
#endif

#include "screen.h"
#include "sleep.h"

static bool mm_on;       /* mouse mover toggle state; event-driven, off at boot */
static uint8_t mm_profile; /* 0 straight, 1 curves; shown by the glyph's steam */

/* "Coffee break" glyph: 14 px wide x 21 rows (bit 15-c = column c, set =
 * ink), 2 px strokes everywhere so it reads at arm's length. Three steam
 * wisps rise 7 px off the rim: straight columns = straight walk profile,
 * zigzag-sheared wisps = natural curves. Drawn pixel-exact with set_px at
 * the bottom-left; the WPM chart ends 35 px above the glyph, the layer
 * text 20 px clear horizontally. */
static const uint16_t mm_steam_straight[6] = {
    0x1B60, /* cols 3-4, 6-7, 9-10 */
    0x1B60, 0x1B60, 0x1B60, 0x1B60, 0x1B60,
};
static const uint16_t mm_steam_wavy[6] = {
    0x0DB0, /* zigzag band A: cols 4-5, 7-8, 10-11 */
    0x0DB0,
    0x36C0, /* band B shears left: 2-3, 5-6, 8-9 */
    0x36C0,
    0x0DB0,
    0x0DB0,
};
static const uint16_t mm_cup[15] = {
    0x0000, /* air gap: steam reads off the rim */
    0x3FF0, /* cols 2-11: rim */
    0x3FF0,
    0x3030, /* walls cols 2-3, 10-11 */
    0x3030,
    0x303F, /* handle top bar cols 12-15 */
    0x3033, /* handle outer col 14-15, 2 px hole */
    0x3033,
    0x303F, /* handle bottom bar */
    0x3030,
    0x3030,
    0x1FF8, /* taper */
    0x0FF0, /* base */
    0x0FF0,
    0x0FF0,
};

static void draw_mouse_mover_indicator(lv_obj_t *canvas) {
    const int x = 6;   /* left margin */
    const int top = SCREEN_HEIGHT - 27; /* 6 px bottom margin below row 161 */
    const uint16_t *steam =
        (mm_profile == 1) ? mm_steam_wavy : mm_steam_straight;
    for (int r = 0; r < 21; r++) {
        uint16_t bits = (r < 6) ? steam[r] : mm_cup[r - 6];
        for (int c = 0; c < 16; c++) {
            if (bits & (0x8000u >> c)) {
                lv_canvas_set_px(canvas, x + c, top + r, LVGL_FOREGROUND);
            }
        }
    }
}

struct connection_status_state {
    bool connected;
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/**
 * Draw buffers
 **/

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);
    fill_background(canvas);

    if (is_sleep_screen_active()) {
        draw_sleep_screen(canvas);
        return;
    }

    // Draw widgets
    draw_output_status(canvas, state);
    #if defined(CONFIG_TOUCAN_STATUS_SCREEN) && CONFIG_TOUCAN_STATUS_SCREEN == 2
    draw_chart_status(canvas, state);
    #endif
    draw_layer_status(canvas, state);
    draw_profile_status(canvas, state);
    draw_battery_status(canvas, state);
    draw_battery_peripheral_status(canvas, state);
    if (mm_on) {
        draw_mouse_mover_indicator(canvas);
    }
}

/**
 * Battery status
 **/
// L
static void set_battery_status(struct zmk_widget_screen *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state);

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

// R
static void set_battery_peripheral_status(struct zmk_widget_screen *widget,
                               struct battery_peripheral_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging_p = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    uint8_t level;
    zmk_split_central_get_peripheral_battery_level(0, &level);

    widget->state.battery_p = level;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_peripheral_status_update_cb(struct battery_peripheral_status_state state) {
    struct zmk_widget_screen *widget;

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_peripheral_status(widget, state); }
}

static struct battery_peripheral_status_state battery_peripheral_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);


    return (struct battery_peripheral_status_state){
        .level = ev->state_of_charge,
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_peripheral_status, struct battery_peripheral_status_state,
                            battery_peripheral_status_update_cb, battery_peripheral_status_get_state);

ZMK_SUBSCRIPTION(widget_battery_peripheral_status, zmk_peripheral_battery_state_changed);

/**
 * Layer status
 **/

static void set_layer_status(struct zmk_widget_screen *widget, struct layer_status_state state) {
    widget->state.layer_index = zmk_keymap_highest_layer_active();
    draw_top(widget->obj, widget->cbuf3, &widget->state);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    uint8_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state) {
        .index = index
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

/**
 * Output status
 **/

static void set_output_status(struct zmk_widget_screen *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

/**
 * Activity state handling for sleep screen
 **/

static void force_redraw_all_widgets(void) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        draw_top(widget->obj, widget->cbuf, &widget->state);
    }
}

static int display_activity_event_handler(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        set_sleep_screen_active(false);
        // No need to force a redraw, it will happen automatically if really coming back from sleep (ACTIVE also comes after IDLE)
        //force_redraw_all_widgets();
        break;
    case ZMK_ACTIVITY_SLEEP:
        set_sleep_screen_active(true);
        force_redraw_all_widgets();
        // Force LVGL to process pending updates and flush to display hardware
        // before the CPU enters deep sleep
        lv_task_handler();
        lv_refr_now(NULL);
        break;
    default:
        break; // ignore other states (like IDLE)
    }
    return 0;
}

ZMK_LISTENER(nice_view_gem_display, display_activity_event_handler);
ZMK_SUBSCRIPTION(nice_view_gem_display, zmk_activity_state_changed);

static int display_mouse_mover_event_handler(const zmk_event_t *eh) {
    const struct zmk_mouse_mover_state_changed *ev = as_zmk_mouse_mover_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }
    mm_on = ev->enabled;
    mm_profile = ev->profile;
    force_redraw_all_widgets();
    return 0;
}

ZMK_LISTENER(widget_mouse_mover, display_mouse_mover_event_handler);
ZMK_SUBSCRIPTION(widget_mouse_mover, zmk_mouse_mover_state_changed);

#if defined(CONFIG_TOUCAN_STATUS_SCREEN) && CONFIG_TOUCAN_STATUS_SCREEN == 2
/**
 * WPM status
 */
    static void set_chart_status(struct zmk_widget_screen *widget, struct chart_status_state state) {
    widget->state.wpm = state.wpm;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void chart_status_update_cb(struct chart_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_chart_status(widget, state);
    }
}

static struct chart_status_state chart_status_get_state(const zmk_event_t *eh) {
    const struct zmk_wpm_state_changed *ev = as_zmk_wpm_state_changed(eh);
    return (struct chart_status_state){
        .wpm = (ev != NULL) ? ev->state : zmk_wpm_get_state(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_chart_status, struct chart_status_state,
                            chart_status_update_cb, chart_status_get_state);
ZMK_SUBSCRIPTION(widget_chart_status, zmk_wpm_state_changed);
#endif

/**
 * Initialization
 **/

int zmk_widget_screen_init(struct zmk_widget_screen *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, SCREEN_WIDTH, SCREEN_HEIGHT);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, SCREEN_WIDTH, SCREEN_HEIGHT, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_battery_peripheral_status_init();
    widget_layer_status_init();
    widget_output_status_init();

    #if defined(CONFIG_TOUCAN_STATUS_SCREEN) && CONFIG_TOUCAN_STATUS_SCREEN == 2
    widget_chart_status_init();
    #endif

    return 0;
}

lv_obj_t *zmk_widget_screen_obj(struct zmk_widget_screen *widget) { return widget->obj; }

