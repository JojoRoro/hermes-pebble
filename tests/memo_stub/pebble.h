/* Minimal stand-in for the parts of <pebble.h> that memo_watch.h uses, so its state
 * machine runs on the host without the SDK. Declarations mirror SDK 4.33.1. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int16_t x, y; } GPoint;
typedef struct { int16_t w, h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct { uint8_t argb; } GColor;
#define GPoint(x, y) ((GPoint){ (int16_t)(x), (int16_t)(y) })
#define GRect(x, y, w, h) ((GRect){ { (int16_t)(x), (int16_t)(y) }, { (int16_t)(w), (int16_t)(h) } })
#define GColorWhite ((GColor){ 0xff })
#define GColorBlack ((GColor){ 0xc0 })
#define GColorRed ((GColor){ 0xf0 })
#define GColorLightGray ((GColor){ 0xea })
#define GColorDarkGray ((GColor){ 0xd5 })
#define GColorIslamicGreen ((GColor){ 0xc8 })
#define GColorCobaltBlue ((GColor){ 0xc6 })
typedef enum { GCornerNone = 0, GCornersAll = 15 } GCornerMask;
typedef enum { GTextOverflowModeTrailingEllipsis = 1 } GTextOverflowMode;
typedef enum { GTextAlignmentLeft, GTextAlignmentCenter, GTextAlignmentRight } GTextAlignment;
typedef struct GContext GContext;
typedef struct Layer Layer;
typedef struct Window Window;
typedef void *GFont;
typedef void (*LayerUpdateProc)(Layer *layer, GContext *ctx);
typedef void *ClickRecognizerRef;
typedef void (*ClickHandler)(ClickRecognizerRef recognizer, void *context);
typedef void (*ClickConfigProvider)(void *context);
typedef enum { BUTTON_ID_BACK, BUTTON_ID_UP, BUTTON_ID_SELECT, BUTTON_ID_DOWN } ButtonId;
typedef void (*WindowHandler)(Window *window);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;
typedef struct AppTimer AppTimer;
typedef void (*AppTimerCallback)(void *data);

typedef struct DictionaryIterator DictionaryIterator;
typedef enum { DICT_OK = 0, DICT_NOT_ENOUGH_STORAGE = 2 } DictionaryResult;
typedef enum { APP_MSG_OK = 0, APP_MSG_BUSY = 64 } AppMessageResult;

#define FONT_KEY_GOTHIC_14 "g14"
#define FONT_KEY_GOTHIC_14_BOLD "g14b"
#define FONT_KEY_GOTHIC_18 "g18"
#define FONT_KEY_GOTHIC_18_BOLD "g18b"
#define FONT_KEY_BITHAM_42_BOLD "b42"

#define APP_LOG_LEVEL_WARNING 50
#define APP_LOG(level, fmt, ...) ((void)(level))

/* Like SDK 4.33.1: no microphone API; mic_shim.h reaches it through the firmware table. */
typedef struct { uint8_t major, minor, patch; } WatchInfoVersion;
WatchInfoVersion watch_info_get_firmware_version(void);

void layer_mark_dirty(Layer *layer);
Layer *layer_create(GRect frame);
void layer_destroy(Layer *layer);
void layer_set_update_proc(Layer *layer, LayerUpdateProc proc);
void layer_add_child(Layer *parent, Layer *child);
GRect layer_get_bounds(const Layer *layer);
Window *window_create(void);
void window_destroy(Window *window);
Layer *window_get_root_layer(const Window *window);
void window_set_click_config_provider(Window *window, ClickConfigProvider provider);
void window_set_window_handlers(Window *window, WindowHandlers handlers);
void window_stack_push(Window *window, bool animated);
Window *window_stack_pop(bool animated);
void window_single_click_subscribe(ButtonId button, ClickHandler handler);
AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback callback, void *data);
void app_timer_cancel(AppTimer *timer);
size_t heap_bytes_free(void);
void vibes_short_pulse(void);
bool connection_service_peek_pebble_app_connection(void);
AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator);
AppMessageResult app_message_outbox_send(void);
DictionaryResult dict_write_uint8(DictionaryIterator *iter, uint32_t key, uint8_t value);
DictionaryResult dict_write_uint32(DictionaryIterator *iter, uint32_t key, uint32_t value);
DictionaryResult dict_write_data(DictionaryIterator *iter, uint32_t key, const uint8_t *data, uint16_t size);
GFont fonts_get_system_font(const char *key);
void graphics_context_set_fill_color(GContext *ctx, GColor color);
void graphics_context_set_stroke_color(GContext *ctx, GColor color);
void graphics_context_set_text_color(GContext *ctx, GColor color);
void graphics_fill_rect(GContext *ctx, GRect rect, uint16_t radius, GCornerMask corners);
void graphics_fill_circle(GContext *ctx, GPoint p, uint16_t radius);
void graphics_draw_round_rect(GContext *ctx, GRect rect, uint16_t radius);
void graphics_draw_text(GContext *ctx, const char *text, GFont font, GRect box, GTextOverflowMode mode,
                        GTextAlignment alignment, void *attributes);
