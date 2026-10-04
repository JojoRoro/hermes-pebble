#include <pebble.h>
#include "protocol.h"
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ink_core.h"

#define UI_BODY_BUFFER_SIZE 1024u
#define UI_MENU_LABEL_SIZE 32u
#define UI_ERROR_BUFFER_SIZE 256u
#define UI_RECENT_LABEL_SIZE 128u
#define UI_RESULT_BUFFER_SIZE 768u
#define UI_RESULT_TEXT_SIZE 8192u
#define RESULT_WINDOW_HEIGHT 20000u
#define MAX_TEXT_HEIGHT 30000u
// Keep one line of the previous page visible when paging through text.
#define SCROLL_PAGE_OVERLAP 30
#define SCROLL_REPEAT_MS 250u
#define MENU_REPEAT_MS 120u
// Fetch the next answer chunk only when the reader is this many screens from the end.
#define RESULT_PREFETCH_SCREENS 2

#define UI_HEADER_HEIGHT 24
#define UI_HINT_HEIGHT 20
#define UI_MARGIN 8
#define UI_BLOCK_MAX 8u
#define UI_ACCENT GColorCobaltBlue

#define UI_BLOCK_TEXT 0u
#define UI_BLOCK_BODY 1u
#define UI_BLOCK_PILL 2u
#define UI_BLOCK_LABEL 3u
#define UI_BLOCK_QUOTE 4u
#define UI_BLOCK_ICON 5u
#define UI_BLOCK_TITLE 6u
#define UI_BLOCK_CAPTION 7u

// Menu rows and icons share identifiers so each row knows its glyph and action.
#define ROW_ASK 0u
#define ROW_NOTE 1u
#define ROW_INK 2u
#define ROW_RECENT 3u
#define ROW_STATUS 4u
#define ROW_NEW 5u
#define ROW_SETTINGS 6u
#define ROW_DRAFT 7u
#define ROW_TOUCH 8u
#define ROW_RECONNECT 9u
#define ROW_BIKE 10u
#define ROW_ERROR 11u

// Ready work runs almost immediately; nothing polls on a fixed interval.
#define TIMER_SOON_MS 50u
#define TIMER_POLL_MS 1000u

#define HERMES_OUT_IDLE 0u
#define HERMES_OUT_SENDING 1u
#define HERMES_OUT_WAIT_CHUNK 2u
#define HERMES_OUT_RETRY_WAIT 3u
#define HERMES_OUT_WAIT_REPLY 4u
#define HERMES_OUT_WAIT_RECEIPT 5u

#define HERMES_SCREEN_MENU 0u
#define HERMES_SCREEN_REVIEW 1u
#define HERMES_SCREEN_STATUS 2u
#define HERMES_SCREEN_RESULT 3u
#define HERMES_SCREEN_RECENT 4u
#define HERMES_SCREEN_ACTIONS 5u
#define HERMES_SCREEN_RECOVERY 6u
#define HERMES_SCREEN_DICTATION 7u
#define HERMES_SCREEN_ERROR 8u
#define HERMES_SCREEN_CONNECTING 9u
#define HERMES_SCREEN_SETTINGS 10u
#define HERMES_SCREEN_INK 11u
#define HERMES_SCREEN_INK_STATUS 12u
#define HERMES_SCREEN_INK_ACTIONS 13u
#define HERMES_MENU_INK 4u

#define HERMES_ACTION_NONE 0u
#define HERMES_ACTION_SEND 1u
#define HERMES_ACTION_SAVE_NOTE 2u
#define HERMES_ACTION_REDICTATE 3u
#define HERMES_ACTION_CANCEL 4u
#define HERMES_ACTION_STOP 5u
#define HERMES_ACTION_FETCH_RESULT 6u
#define HERMES_ACTION_RECENT 7u
#define HERMES_ACTION_MENU 8u
#define HERMES_ACTION_RESUME 9u
#define HERMES_ACTION_DISCARD 10u
#define HERMES_ACTION_STATUS 12u
#define HERMES_ACTION_BACK 13u
#define HERMES_ACTION_REPLY 14u
#define HERMES_ACTION_SEND_VOICE 15u
#define HERMES_ACTION_PLAY_VOICE 16u

#define HERMES_MENU_MAIN 0u
#define HERMES_MENU_RECENT 1u
#define HERMES_MENU_ACTIONS 2u
#define HERMES_MENU_SETTINGS 3u

#define HERMES_CAPTURE_REQUEST 0u
#define HERMES_CAPTURE_NOTE 1u

typedef struct {
  uint8_t operation;
  uint32_t capture_id;
  uint32_t transfer_id;
  uint32_t generation;
  uint16_t length;
  uint8_t chunk_count;
  uint8_t chunk_lengths[HERMES_MAX_CHUNKS];
  char text[HERMES_MAX_DICTATION_BYTES + 1u];
} PendingCapture;

typedef struct {
  bool active;
  uint8_t phase;
  uint8_t kind;
  uint8_t expected_kind;
  uint8_t chunk_count;
  uint8_t next_chunk;
  uint8_t retry_count;
  uint32_t deadline_ms;
  uint16_t length;
  uint32_t transfer_id;
  uint32_t capture_id;
  uint32_t generation;
  uint32_t item_id;
  uint32_t page_offset;
  uint32_t total_bytes;
  uint32_t correlation_id;
  uint8_t status;
  uint8_t error_code;
  uint8_t item_kind;
  uint8_t item_state;
  uint8_t page_count;
  uint8_t flags;
  uint8_t chunk_lengths[HERMES_MAX_CHUNKS];
  uint16_t chunk_offsets[HERMES_MAX_CHUNKS];
  uint8_t payload[HERMES_MAX_TRANSFER_BYTES];
} OutboundTransfer;

typedef struct {
  bool active;
  uint8_t protocol_version;
  uint8_t kind;
  uint8_t chunk_count;
  uint8_t received_mask;
  uint16_t chunk_lengths[HERMES_MAX_CHUNKS];
  uint16_t chunk_offsets[HERMES_MAX_CHUNKS];
  uint8_t chunks[HERMES_MAX_TRANSFER_BYTES];
  uint16_t total_length;
  uint32_t transfer_id;
  uint32_t capture_id;
  uint32_t correlation_id;
  uint32_t item_id;
  uint32_t page_offset;
  uint32_t total_bytes;
  uint32_t generation;
  uint8_t status;
  uint8_t error_code;
  uint8_t item_state;
  uint8_t item_kind;
  uint8_t page_count;
  uint8_t flags;
  uint16_t payload_length;
  uint8_t payload[HERMES_MAX_TRANSFER_BYTES];
} InboundTransfer;

typedef struct {
  uint32_t capture_id;
  uint8_t kind;
  uint8_t state;
  bool more;
  char label[UI_RECENT_LABEL_SIZE];
} RecentItem;

typedef struct {
  const char *text;
  uint8_t kind;
  uint8_t icon;
  GColor color;
  int16_t y;
  int16_t h;
} UiBlock;

typedef struct ResultWindow {
  uint32_t offset;
  struct ResultWindow *previous;
} ResultWindow;

static Window *s_window;
static MenuLayer *s_menu_layer;
static ScrollLayer *s_scroll_layer;
static Layer *s_header_layer;
static char s_header_text[40];
static Layer *s_content_layer;
static Layer *s_overlay_layer;
static TextLayer *s_hint_layer;
static UiBlock s_blocks[UI_BLOCK_MAX];
static uint8_t s_block_count;
static bool s_select_tab;
static bool s_blocks_centered;
static uint8_t s_result_answer_block;
static int16_t s_result_text_height;
static bool s_result_height_valid;
static uint32_t s_reply_notify_capture_id;
static uint32_t s_reply_notified_capture_id;
// Voice consent lasts only for this open watch session and this capture.
static uint32_t s_voice_capture_id;
static bool s_voice_requested;
static bool s_voice_play_pending;
static char s_voice_status[64]; // One-line footer; full errors stay in phone Diagnostics.
static bool s_app_message_open;
static bool s_handshake_ready;
static bool s_exiting;
static bool s_touch_navigation_enabled = true;
static bool s_stay_on_menu;
static uint32_t s_phone_probe_id;
static uint32_t s_follow_capture_id;
static uint32_t s_visible_generation;
static uint32_t s_auto_result_capture_id;
static bool s_auto_result_requested;
static bool s_result_loading;
static bool s_result_prefetch;
static bool s_result_retry;
static bool s_result_scroll_to_end;
static bool s_result_window_full;
static bool s_storage_corrupt;
static bool s_identity_valid;
static bool s_discard_in_progress;
static uint8_t s_screen;
static uint8_t s_previous_screen;
static uint8_t s_menu_kind;
static uint8_t s_capture_mode;
static uint8_t s_dictation_mode;
static uint8_t s_visible_status;
static uint8_t s_visible_error;
static uint8_t s_visible_flags;
static uint8_t s_visible_item_kind;
static uint8_t s_result_more;
static uint8_t s_recent_count;
static uint16_t s_scroll_offset;
static uint16_t s_result_scroll_offset;
static uint16_t s_scroll_content_height;
static uint32_t s_inbound_deadline_ms;
static uint32_t s_counter;
static uint32_t s_generation;
static uint32_t s_install_hash;
static uint32_t s_visible_capture_id;
static uint32_t s_visible_item_id;
static uint32_t s_result_offset;
static uint32_t s_result_total_bytes;
static uint32_t s_result_next_offset;
static uint32_t s_result_window_offset;
static uint32_t s_result_window_end;
static ResultWindow *s_result_previous_windows;
static uint32_t s_discard_capture_id;
static uint32_t s_discard_transfer_id;
static uint32_t s_transfer_sequence;
static uint8_t s_install_id[16];
static char s_capture_text[HERMES_MAX_DICTATION_BYTES + 1u];
static char s_visible_input[HERMES_MAX_DICTATION_BYTES + 1u];
// Keep the full answer window in runtime RAM, below the SDK's static-image limit.
static char *s_visible_output;
static char s_result_page[UI_RESULT_BUFFER_SIZE + 1u];
static char s_body_text[UI_BODY_BUFFER_SIZE];
static char s_error_text[UI_ERROR_BUFFER_SIZE];
static RecentItem s_recent_items[HERMES_MAX_RECENT_ITEMS];
static uint8_t s_action_options[8];
static uint8_t s_action_count;
static PendingCapture s_pending;
static OutboundTransfer s_outbound;
static InboundTransfer s_inbound;
static DictationSession *s_dictation_session;
static AppTimer *s_timer;
// Only action menus draw these; main, settings and recent rows draw from their own state.
static char s_menu_labels[9][UI_MENU_LABEL_SIZE];
static uint8_t s_menu_rows[9];
static uint16_t s_menu_count;
static uint8_t s_ink[INK_CAPACITY];
static uint16_t s_ink_length = INK_HEADER;
static uint32_t s_ink_capture;
static uint32_t s_ink_checksum;
static uint16_t s_ink_offset;
static bool s_ink_saved;
static bool s_ink_corrupt;
static bool s_ink_sync_requested;
static char s_ink_status[160];
static void ink_open(void);
static void ink_show(void);
static void ink_cleanup(void);
static void ink_load(void);
static void ink_try_send(void);
static void ink_receipt(const InboundTransfer *message);
static void ink_failed(void);
static void ink_menu_select(uint16_t row);
static void ink_click_config(void);
static void ink_connection(bool connected);
static void ink_schedule_sync(void);

static void ui_rebuild(void);
static void ui_show_menu(void);
static void ui_show_review(void);
static void ui_show_status(void);
static void ui_show_result(void);
static void ui_show_recent(void);
static void ui_show_recovery(void);
static void ui_show_dictation(void);
static void ui_show_error(uint32_t code, const char *text);
static void ui_show_actions(void);
static void ui_destroy_content(void);
static void ui_show_settings(void);
static bool should_auto_fetch_result(void);
static bool voice_reply_available(void);
static bool voice_shortcut_available(bool repeating);
static void cancel_auto_result(void);
static void touch_navigation_apply(void);
static void touch_navigation_load(void);
static bool touch_navigation_save(bool enabled);
static void ui_action(uint8_t action);
static void ui_scroll(int delta);
static void result_reset(void);
static bool result_move_window(bool forward);
static void start_result_chunk(uint32_t capture_id, uint32_t offset);
static bool result_append_page(uint32_t offset, uint32_t total, bool more);
static void ui_window_load(Window *window);
static void ui_window_unload(Window *window);
static void ui_click_config(void *context);
static void ui_select_click(ClickRecognizerRef recognizer, void *context);
static void ui_up_click(ClickRecognizerRef recognizer, void *context);
static void ui_down_click(ClickRecognizerRef recognizer, void *context);
static void ui_menu_select_click(ClickRecognizerRef recognizer, void *context);
static void ui_menu_up_click(ClickRecognizerRef recognizer, void *context);
static void ui_menu_down_click(ClickRecognizerRef recognizer, void *context);
static void ui_back_click(ClickRecognizerRef recognizer, void *context);
static void menu_select(void *context, MenuLayer *menu_layer, MenuIndex *selection);
static void action_menu_select(void *context, MenuLayer *menu_layer, MenuIndex *selection);

static void storage_init(void);
static bool storage_read_u32(uint32_t key, uint32_t *value);
static bool storage_write_u32(uint32_t key, uint32_t value);
static bool storage_load_pending(void);
static bool storage_save_pending(const PendingCapture *pending);
static bool storage_clear_pending(void);
static bool storage_next_capture_id(uint32_t *capture_id);
static bool storage_write_generation(uint32_t generation);
static uint32_t pending_crc(const PendingCapture *pending);
static void pending_make_chunk_plan(const uint8_t *data, uint16_t length, uint8_t *lengths, uint16_t *offsets, uint8_t *count);
static bool utf8_valid(const uint8_t *data, uint16_t length);
static bool is_utf8_continuation(uint8_t value);

static void outbound_start(uint8_t kind, uint8_t expected_kind, uint32_t transfer_id, uint32_t capture_id, uint32_t generation, uint32_t item_id, uint32_t page_offset, uint32_t total_bytes, uint8_t status, uint8_t error_code, uint8_t item_kind, uint8_t item_state, uint8_t page_count, uint8_t flags, const uint8_t *payload, uint16_t length);
static void outbound_finish(void);
static void outbound_failed(uint32_t error_code, const char *text);
static void outbound_send_current_chunk(void);
static void outbound_schedule_retry(void);
static void outbound_timeout(void);
static void outbound_message_sent(DictionaryIterator *iterator, void *context);
static void outbound_message_failed(DictionaryIterator *iterator, AppMessageResult result, void *context);
static void timer_tick(void *context);
static void timer_update(void);
static bool result_prefetch_due(void);

static void inbox_received(DictionaryIterator *iter, void *context);
static void inbox_dropped(AppMessageResult result, void *context);
static void process_inbound_transfer(void);
static void inbound_reset(void);
static void inbound_error(uint32_t code, const char *text);
static bool inbound_correlation_matches(const InboundTransfer *message);
static void process_phone_message(const InboundTransfer *message);
static void process_durable_receipt(const InboundTransfer *message);
static void process_status_update(const InboundTransfer *message);
static void process_recent_page(const InboundTransfer *message);
static void process_result_page(const InboundTransfer *message);
static void process_conversation_ack(const InboundTransfer *message);
static void process_structured_error(const InboundTransfer *message);

static void start_dictation(uint8_t mode);
static void stop_dictation(void);
static void dictation_callback(DictationSession *session, DictationSessionStatus status, char *transcript, void *context);
static void start_handshake(void);
static void start_fetch_recent(void);
static void start_fetch_result(uint32_t capture_id, uint32_t offset);
static void start_new_conversation(void);
static void start_stop_request(uint32_t capture_id);
static void start_discard_capture(uint32_t capture_id);
static void prepare_capture(uint8_t operation);
static void resume_pending_capture(void);

static bool read_u8(DictionaryIterator *iter, uint8_t key, uint8_t *value, bool required);
static bool read_u32(DictionaryIterator *iter, uint8_t key, uint32_t *value, bool required);
static bool read_bytes(DictionaryIterator *iter, uint8_t key, const uint8_t **value, uint16_t capacity, uint16_t *length, bool required);
static bool common_fields_equal(const InboundTransfer *message, uint8_t protocol_version, uint8_t kind, uint32_t transfer_id, uint32_t capture_id, uint8_t chunk_count);

static uint32_t next_transfer_id(void);
static uint32_t mix_capture_id(uint32_t counter);
static const char *status_text(uint8_t status);
static const char *error_text(uint32_t code);
static const char *dictation_failure_text(int status);
static void build_menu(const char *title, const char *const *labels, uint16_t count, uint8_t menu_kind);

static bool json_string_in_span(const char *json, size_t length, size_t start, size_t end, const char *key, char *output, size_t capacity);
static bool json_uint_in_span(const char *json, size_t length, size_t start, size_t end, const char *key, uint32_t *value);
static bool json_bool_in_span(const char *json, size_t length, size_t start, size_t end, const char *key, bool *value);
static bool json_object_span(const char *json, size_t length, size_t start, size_t *begin, size_t *end);
static bool parse_recent_payload(const char *json, uint16_t length);
static bool parse_result_payload(const char *json, uint16_t length);
static bool json_array_position(const char *json, uint16_t length, const char *key, size_t *position);
static void json_skip_space(const char *json, size_t length, size_t *position);
static bool json_hex(char value, uint8_t *result);
static size_t json_encode_utf8(uint32_t codepoint, char *output, size_t capacity);

#include "audio_watch.h"
#include "bike_watch.h"

static bool audio_dictation_active(void) { return s_dictation_session != NULL; }

static bool audio_send_status(uint32_t session, uint32_t total, uint32_t checksum,
                              uint32_t received, uint8_t status) {
  if (s_exiting || !s_app_message_open || s_outbound.active) return false;
  outbound_start(HERMES_KIND_AUDIO_STATUS, HERMES_KIND_NONE, next_transfer_id(),
    session, checksum, 0u, received, total, status, 0u, HERMES_ITEM_KIND_NONE,
    0u, 0u, 0u, NULL, 0u);
  return s_outbound.active;
}

static void ui_window_load(Window *window) {
  (void)window;
  ui_rebuild();
}

static void ui_window_unload(Window *window) {
  (void)window;
  ui_destroy_content();
}

static void ui_destroy_content(void) {
  ink_cleanup();
  if (s_header_layer != NULL) {
    layer_destroy(s_header_layer);
    s_header_layer = NULL;
  }
  if (s_hint_layer != NULL) {
    text_layer_destroy(s_hint_layer);
    s_hint_layer = NULL;
  }
  if (s_overlay_layer != NULL) {
    layer_destroy(s_overlay_layer);
    s_overlay_layer = NULL;
  }
  if (s_content_layer != NULL) {
    layer_destroy(s_content_layer);
    s_content_layer = NULL;
  }
  if (s_scroll_layer != NULL) {
    scroll_layer_destroy(s_scroll_layer);
    s_scroll_layer = NULL;
  }
  if (s_menu_layer != NULL) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
  }
  s_block_count = 0u;
  s_select_tab = false;
  s_scroll_offset = 0;
  s_scroll_content_height = 0;
}

static const char *status_short(uint8_t status) {
  switch (status) {
    case HERMES_STATUS_WAITING_PHONE: return "Sending to phone";
    case HERMES_STATUS_SAVED_QUEUED: return "Queued on phone";
    case HERMES_STATUS_SUBMITTING: return "Submitting";
    case HERMES_STATUS_ACCEPTED: return "Accepted";
    case HERMES_STATUS_WORKING: return "Working";
    case HERMES_STATUS_APPROVAL_NEEDED: return "Needs approval";
    case HERMES_STATUS_COMPLETED: return "Answered";
    case HERMES_STATUS_FAILED: return "Failed";
    case HERMES_STATUS_STOPPING: return "Stopping";
    case HERMES_STATUS_CANCELLED: return "Cancelled";
    case HERMES_STATUS_INTERRUPTED: return "Interrupted";
    case HERMES_STATUS_OUTCOME_UNKNOWN: return "Outcome unknown";
    case HERMES_STATUS_NOTE_SAVED: return "Saved";
    case HERMES_STATUS_WAITING_PROFILE: return "Waiting for profile";
    case HERMES_STATUS_DISCARDED: return "Discarded";
    default: return "No status";
  }
}

static GColor status_color(uint8_t status) {
  switch (status) {
    case HERMES_STATUS_COMPLETED:
    case HERMES_STATUS_NOTE_SAVED:
      return GColorIslamicGreen;
    case HERMES_STATUS_FAILED:
    case HERMES_STATUS_INTERRUPTED:
      return GColorDarkCandyAppleRed;
    case HERMES_STATUS_APPROVAL_NEEDED:
    case HERMES_STATUS_OUTCOME_UNKNOWN:
    case HERMES_STATUS_WAITING_PROFILE:
      return GColorWindsorTan;
    case HERMES_STATUS_NONE:
    case HERMES_STATUS_CANCELLED:
    case HERMES_STATUS_DISCARDED:
      return GColorDarkGray;
    default:
      return UI_ACCENT;
  }
}

static GColor ui_row_color(uint8_t row) {
  switch (row) {
    case ROW_ASK:
    case ROW_RECONNECT:
      return GColorCobaltBlue;
    case ROW_NOTE:
    case ROW_BIKE:
      return GColorIslamicGreen;
    case ROW_INK: return GColorPurple;
    case ROW_RECENT: return GColorBlueMoon;
    case ROW_STATUS: return GColorWindsorTan;
    case ROW_NEW: return GColorMidnightGreen;
    case ROW_DRAFT: return GColorOrange;
    case ROW_ERROR: return GColorDarkCandyAppleRed;
    default: return GColorDarkGray;
  }
}

static GPoint ui_offset(GPoint c, int dx, int dy) {
  return GPoint(c.x + dx, c.y + dy);
}

/* Glyphs are drawn from primitives so the app needs no image resources. */
static void ui_draw_icon(GContext *ctx, uint8_t icon, GPoint c, int r, GColor badge, bool inverted) {
  GColor background = inverted ? GColorWhite : badge;
  GColor glyph = inverted ? badge : GColorWhite;
  int u = r / 2;
  graphics_context_set_fill_color(ctx, background);
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_fill_color(ctx, glyph);
  graphics_context_set_stroke_color(ctx, glyph);
  graphics_context_set_stroke_width(ctx, r >= 18 ? 3 : 2);
  switch (icon) {
    case ROW_ASK:
      // Microphone: slim capsule, cradle arc, stem and base.
      graphics_fill_rect(ctx, GRect(c.x - u / 2 + 1, c.y - u - 1, u - 1, u + 3), (u - 1) / 2, GCornersAll);
      graphics_draw_arc(ctx, GRect(c.x - u + 1, c.y - u + 2, 2 * u - 2, 2 * u - 2), GOvalScaleModeFitCircle,
          DEG_TO_TRIGANGLE(90), DEG_TO_TRIGANGLE(270));
      graphics_draw_line(ctx, ui_offset(c, 0, u), ui_offset(c, 0, u + u / 3));
      graphics_draw_line(ctx, ui_offset(c, -u / 2, u + u / 3), ui_offset(c, u / 2, u + u / 3));
      break;
    case ROW_NOTE:
      graphics_fill_rect(ctx, GRect(c.x - u + 1, c.y - u, 2 * u - 2, 2 * u), 1, GCornersAll);
      graphics_context_set_stroke_color(ctx, background);
      graphics_context_set_stroke_width(ctx, 1);
      for (int line = -u / 2; line <= u / 2; line += u / 2) {
        graphics_draw_line(ctx, ui_offset(c, -u / 2, line), ui_offset(c, u / 2, line));
      }
      break;
    case ROW_INK:
      graphics_context_set_stroke_width(ctx, r >= 18 ? 5 : 3);
      graphics_draw_line(ctx, ui_offset(c, -u + 2, u - 2), ui_offset(c, u, -u));
      graphics_fill_circle(ctx, ui_offset(c, -u + 1, u - 1), 1);
      break;
    case ROW_RECENT:
      graphics_draw_circle(ctx, c, u + 1);
      graphics_draw_line(ctx, c, ui_offset(c, 0, -u + 1));
      graphics_draw_line(ctx, c, ui_offset(c, u / 2 + 1, 0));
      break;
    case ROW_STATUS:
      // Progress list: three bars of falling length.
      for (int bar = 0; bar < 3; bar++) {
        int y = (bar - 1) * (u * 2 / 3 + 1);
        graphics_draw_line(ctx, ui_offset(c, -u + 1, y), ui_offset(c, u - 1 - bar * (u / 2), y));
      }
      break;
    case ROW_NEW:
      graphics_draw_line(ctx, ui_offset(c, -u, 0), ui_offset(c, u, 0));
      graphics_draw_line(ctx, ui_offset(c, 0, -u), ui_offset(c, 0, u));
      break;
    case ROW_DRAFT:
    case ROW_ERROR:
      graphics_context_set_stroke_width(ctx, r >= 18 ? 4 : 3);
      graphics_draw_line(ctx, ui_offset(c, 0, -u), ui_offset(c, 0, u / 3));
      graphics_fill_circle(ctx, ui_offset(c, 0, u), r >= 18 ? 2 : 1);
      break;
    case ROW_RECONNECT:
      graphics_draw_arc(ctx, GRect(c.x - u, c.y - u, 2 * u, 2 * u), GOvalScaleModeFitCircle,
          DEG_TO_TRIGANGLE(70), DEG_TO_TRIGANGLE(360));
      graphics_draw_line(ctx, ui_offset(c, 1, -u), ui_offset(c, -u / 2, -u - u / 2));
      graphics_draw_line(ctx, ui_offset(c, 1, -u), ui_offset(c, -u / 2, -u + u / 2));
      break;
    case ROW_BIKE:
      graphics_context_set_stroke_width(ctx, r >= 18 ? 2 : 1);
      graphics_draw_circle(ctx, ui_offset(c, -u + 1, u / 2), u / 2 + 1);
      graphics_draw_circle(ctx, ui_offset(c, u - 1, u / 2), u / 2 + 1);
      graphics_draw_line(ctx, ui_offset(c, -u + 1, u / 2), ui_offset(c, 0, -u / 3));
      graphics_draw_line(ctx, ui_offset(c, 0, -u / 3), ui_offset(c, u - 1, u / 2));
      graphics_draw_line(ctx, ui_offset(c, -u / 3, -u / 2 - 1), ui_offset(c, u / 3, -u / 2 - 1));
      break;
    default: {
      // Settings and touch navigation share a simple gear.
      for (int angle = 0; angle < 180; angle += 45) {
        int32_t trig = DEG_TO_TRIGANGLE(angle);
        int dx = (int)(sin_lookup(trig) * (u + 2) / TRIG_MAX_RATIO);
        int dy = (int)(cos_lookup(trig) * (u + 2) / TRIG_MAX_RATIO);
        graphics_draw_line(ctx, ui_offset(c, -dx, -dy), ui_offset(c, dx, dy));
      }
      graphics_fill_circle(ctx, c, u);
      graphics_context_set_fill_color(ctx, background);
      graphics_fill_circle(ctx, c, u / 2);
      break;
    }
  }
}

static void ui_header_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, UI_ACCENT);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_header_text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(UI_MARGIN, -1, bounds.size.w - 80, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  // A quiet green dot when linked; spell out the offline state.
  graphics_context_set_fill_color(ctx, s_handshake_ready ? GColorGreen : GColorMelon);
  graphics_fill_circle(ctx, GPoint(bounds.size.w - 12, bounds.size.h / 2), 4);
  if (!s_handshake_ready) {
    graphics_draw_text(ctx, "offline", fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(bounds.size.w - 80, 2, 60, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  }
}

static void ui_add_header(const char *title) {
  Layer *root = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root);
  snprintf(s_header_text, sizeof(s_header_text), "%s", title);
  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, UI_HEADER_HEIGHT));
  if (s_header_layer == NULL) return;
  layer_set_update_proc(s_header_layer, ui_header_draw);
  layer_add_child(root, s_header_layer);
}

static void ui_link_changed(void) {
  if (s_header_layer != NULL) layer_mark_dirty(s_header_layer);
  if (s_menu_layer != NULL) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

static const char *ui_block_font(uint8_t kind) {
  switch (kind) {
    case UI_BLOCK_TEXT: return FONT_KEY_GOTHIC_24;
    case UI_BLOCK_PILL: return FONT_KEY_GOTHIC_18_BOLD;
    case UI_BLOCK_LABEL: return FONT_KEY_GOTHIC_14_BOLD;
    case UI_BLOCK_TITLE: return FONT_KEY_GOTHIC_24_BOLD;
    default: return FONT_KEY_GOTHIC_18;
  }
}

static int16_t ui_text_height(const char *text, const char *font, int width, GTextAlignment alignment) {
  GSize size = graphics_text_layout_get_content_size(text, fonts_get_system_font(font),
      GRect(0, 0, width, MAX_TEXT_HEIGHT), GTextOverflowModeWordWrap, alignment);
  return (int16_t)size.h;
}

static int ui_text_width(void) {
  return layer_get_bounds(window_get_root_layer(s_window)).size.w - 2 * UI_MARGIN;
}

/* One answer measurement per change; layout and the window bound share it. */
static int16_t result_text_height(void) {
  if (!s_result_height_valid) {
    s_result_text_height = ui_text_height(s_visible_output, FONT_KEY_GOTHIC_24, ui_text_width(), GTextAlignmentLeft);
    s_result_height_valid = true;
  }
  return s_result_text_height;
}

static void ui_block(uint8_t kind, const char *text, GColor color, uint8_t icon) {
  if (s_block_count >= UI_BLOCK_MAX) return;
  s_blocks[s_block_count++] = (UiBlock){ .text = text, .kind = kind, .icon = icon, .color = color };
}

static void ui_content_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int top = s_scroll_layer != NULL ? -scroll_layer_get_content_offset(s_scroll_layer).y : 0;
  int view = s_scroll_layer != NULL ? layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h : bounds.size.h;
  int width = bounds.size.w - 2 * UI_MARGIN;
  GTextAlignment alignment = s_blocks_centered ? GTextAlignmentCenter : GTextAlignmentLeft;
  for (uint8_t i = 0u; i < s_block_count; i++) {
    const UiBlock *block = &s_blocks[i];
    // Skip blocks outside the viewport; text layout is the expensive part of a redraw.
    if (block->y + block->h < top || block->y > top + view) continue;
    GRect frame = GRect(UI_MARGIN, block->y, width, block->h);
    GTextAlignment block_alignment = alignment;
    graphics_context_set_text_color(ctx, GColorBlack);
    switch (block->kind) {
      case UI_BLOCK_ICON:
        ui_draw_icon(ctx, block->icon, GPoint(bounds.size.w / 2, block->y + block->h / 2), 22, block->color, false);
        continue;
      case UI_BLOCK_PILL:
        graphics_context_set_fill_color(ctx, block->color);
        graphics_fill_rect(ctx, frame, 6, GCornersAll);
        graphics_context_set_text_color(ctx, GColorWhite);
        frame = GRect(UI_MARGIN + 8, block->y + 1, width - 16, block->h - 2);
        block_alignment = GTextAlignmentLeft;
        break;
      case UI_BLOCK_QUOTE:
        graphics_context_set_fill_color(ctx, block->color);
        graphics_fill_rect(ctx, GRect(UI_MARGIN, block->y + 5, 3, block->h - 8), 1, GCornersAll);
        frame = GRect(UI_MARGIN + 9, block->y, width - 9, block->h);
        block_alignment = GTextAlignmentLeft;
        break;
      case UI_BLOCK_LABEL:
      case UI_BLOCK_CAPTION:
        graphics_context_set_text_color(ctx, block->color);
        break;
      default:
        break;
    }
    graphics_draw_text(ctx, block->text, fonts_get_system_font(ui_block_font(block->kind)), frame,
        GTextOverflowModeWordWrap, block_alignment, NULL);
  }
}

static void ui_overlay_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  if (s_select_tab) {
    // Points at the middle-right SELECT button: it opens this screen's actions.
    int middle = layer_get_bounds(window_get_root_layer(s_window)).size.h / 2 - layer_get_frame(layer).origin.y;
    graphics_context_set_fill_color(ctx, UI_ACCENT);
    graphics_fill_rect(ctx, GRect(bounds.size.w - 4, middle - 14, 6, 28), 3, GCornersLeft);
  }
  if (s_scroll_content_height > bounds.size.h) {
    int track = bounds.size.h - 8;
    int thumb = track * bounds.size.h / s_scroll_content_height;
    int range = s_scroll_content_height - bounds.size.h;
    if (thumb < 12) thumb = 12;
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx, GRect(bounds.size.w - 3, 4 + (track - thumb) * s_scroll_offset / range, 2, thumb),
        0, GCornerNone);
  }
}

/* Lay out the queued blocks under a header, with an optional footer hint. */
static void ui_present(uint8_t screen, const char *title, bool centered, bool select_tab, const char *hint) {
  Layer *root;
  GRect bounds;
  int16_t view;
  int16_t y = 6;
  int width;
  if (s_window == NULL) return;
  root = window_get_root_layer(s_window);
  bounds = layer_get_bounds(root);
  width = bounds.size.w - 2 * UI_MARGIN;
  view = bounds.size.h - UI_HEADER_HEIGHT - (hint != NULL ? UI_HINT_HEIGHT : 0);
  s_blocks_centered = centered;
  for (uint8_t i = 0u; i < s_block_count; i++) {
    UiBlock *block = &s_blocks[i];
    GTextAlignment alignment = centered ? GTextAlignmentCenter : GTextAlignmentLeft;
    const char *font = ui_block_font(block->kind);
    if (block->kind == UI_BLOCK_ICON) {
      block->h = 52;
    } else if (block->kind == UI_BLOCK_PILL) {
      block->h = ui_text_height(block->text, font, width - 16, GTextAlignmentLeft) + 8;
    } else if (block->kind == UI_BLOCK_QUOTE) {
      block->h = ui_text_height(block->text, font, width - 9, GTextAlignmentLeft) + 6;
    } else if (block->h == 0) {
      block->h = ui_text_height(block->text, font, width, alignment) + (block->kind == UI_BLOCK_TEXT ? 8 : 4);
    }
    block->y = y;
    y += block->h + (block->kind == UI_BLOCK_LABEL ? 0 : 6);
  }
  y += 6;
  if (centered && y < view) {
    int16_t shift = (view - y) / 2;
    for (uint8_t i = 0u; i < s_block_count; i++) s_blocks[i].y += shift;
  }
  if (y < view) y = view;
  ui_add_header(title);
  s_scroll_layer = scroll_layer_create(GRect(0, UI_HEADER_HEIGHT, bounds.size.w, view));
  s_content_layer = layer_create(GRect(0, 0, bounds.size.w, y));
  if (s_scroll_layer == NULL || s_content_layer == NULL) return;
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);
  layer_set_update_proc(s_content_layer, ui_content_draw);
  scroll_layer_add_child(s_scroll_layer, s_content_layer);
  scroll_layer_set_content_size(s_scroll_layer, GSize(bounds.size.w, y));
  scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, 0), false);
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
  s_overlay_layer = layer_create(GRect(0, UI_HEADER_HEIGHT, bounds.size.w, view));
  if (s_overlay_layer != NULL) {
    layer_set_update_proc(s_overlay_layer, ui_overlay_draw);
    layer_add_child(root, s_overlay_layer);
  }
  if (hint != NULL) {
    s_hint_layer = text_layer_create(GRect(0, bounds.size.h - UI_HINT_HEIGHT, bounds.size.w, UI_HINT_HEIGHT));
    if (s_hint_layer != NULL) {
      text_layer_set_font(s_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
      text_layer_set_background_color(s_hint_layer, GColorLightGray);
      text_layer_set_text_color(s_hint_layer, GColorBlack);
      text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
      text_layer_set_text(s_hint_layer, hint);
      layer_add_child(root, text_layer_get_layer(s_hint_layer));
    }
  }
  s_select_tab = select_tab;
  s_scroll_offset = 0u;
  s_scroll_content_height = (uint16_t)y;
  s_screen = screen;
  window_set_click_config_provider(s_window, ui_click_config);
}

/* A centered notice: large glyph, title, and explanation. */
static void ui_show_notice(uint8_t screen, const char *header, uint8_t icon, const char *title, const char *text, const char *hint) {
  ui_destroy_content();
  ui_block(UI_BLOCK_ICON, NULL, ui_row_color(icon), icon);
  ui_block(UI_BLOCK_TITLE, title, GColorBlack, 0u);
  ui_block(UI_BLOCK_CAPTION, text, GColorDarkGray, 0u);
  ui_present(screen, header, true, false, hint);
}

static const char *ui_row_title(uint8_t row) {
  switch (row) {
    case ROW_ASK: return "Ask Hermes";
    case ROW_NOTE: return "Save note";
    case ROW_INK: return "Handwritten note";
    case ROW_RECENT: return "Recent";
    case ROW_STATUS: return "Status";
    case ROW_NEW: return "New conversation";
    case ROW_SETTINGS: return "Settings";
    case ROW_DRAFT: return "Saved draft";
    case ROW_TOUCH: return "Touch navigation";
    case ROW_RECONNECT: return "Reconnect phone";
    default: return "Bike detection test";
  }
}

static const char *ui_row_subtitle(uint8_t row) {
  switch (row) {
    case ROW_ASK: return "Dictate, review, send";
    case ROW_NOTE: return "Keep a note on your phone";
    case ROW_INK: return "Draw letters, sync later";
    case ROW_RECENT: return "Requests, notes & answers";
    case ROW_STATUS: return s_visible_capture_id != 0u ? status_short(s_visible_status) : "Your last request";
    case ROW_NEW: return "Start with a fresh context";
    case ROW_SETTINGS: return "Touch, connection, tools";
    case ROW_DRAFT: return "Not sent yet · open to resume";
    case ROW_TOUCH: return s_touch_navigation_enabled ? "On · swipe to navigate" : "Off · buttons only";
    case ROW_RECONNECT: return s_handshake_ready ? "Linked · test again" : "Not linked · try again";
    default: return "Cycling estimate (beta)";
  }
}

static uint16_t menu_get_rows(MenuLayer *menu_layer, uint16_t section, void *context) {
  (void)menu_layer; (void)section; (void)context;
  return s_menu_count;
}

static bool menu_has_subtitles(void) {
  return s_menu_kind == HERMES_MENU_MAIN || s_menu_kind == HERMES_MENU_SETTINGS || s_menu_kind == HERMES_MENU_RECENT;
}

static int16_t menu_row_height(MenuLayer *menu, MenuIndex *index, void *context) {
  (void)menu; (void)index; (void)context;
  return menu_has_subtitles() ? 52 : 44;
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *index, void *context) {
  (void)context;
  if (index->row >= s_menu_count) return;
  GRect bounds = layer_get_bounds(cell_layer);
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  const char *title = s_menu_labels[index->row];
  const char *subtitle = NULL;
  char recent_subtitle[48];
  int x = 12;
  graphics_context_set_text_color(ctx, highlighted ? GColorWhite : GColorBlack);
  if (s_menu_kind == HERMES_MENU_MAIN || s_menu_kind == HERMES_MENU_SETTINGS) {
    uint8_t row = s_menu_rows[index->row];
    ui_draw_icon(ctx, row, GPoint(24, bounds.size.h / 2), 14, ui_row_color(row), highlighted);
    title = ui_row_title(row);
    subtitle = ui_row_subtitle(row);
    x = 46;
  } else if (s_menu_kind == HERMES_MENU_RECENT && index->row < s_recent_count) {
    // Glyph shows the kind; its color shows the state.
    const RecentItem *item = &s_recent_items[index->row];
    title = item->label;
    bool note = item->kind == HERMES_ITEM_KIND_NOTE;
    ui_draw_icon(ctx, note ? ROW_NOTE : ROW_ASK, GPoint(24, bounds.size.h / 2), 14, status_color(item->state), highlighted);
    snprintf(recent_subtitle, sizeof(recent_subtitle), "%s · %s", note ? "Note" : "Request", status_short(item->state));
    subtitle = recent_subtitle;
    x = 46;
  } else if (!highlighted && (strncmp(title, "Discard", 7) == 0 || strncmp(title, "Stop", 4) == 0 ||
                              strncmp(title, "Cancel", 6) == 0)) {
    graphics_context_set_text_color(ctx, GColorDarkCandyAppleRed);
  }
  if (subtitle != NULL) {
    graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(x, 3, bounds.size.w - x - 8, 24), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : GColorDarkGray);
    graphics_draw_text(ctx, subtitle, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(x, 26, bounds.size.w - x - 8, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  } else {
    graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(x, (bounds.size.h - 24) / 2 - 2, bounds.size.w - x - 8, 24),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

static void build_menu(const char *title, const char *const *labels, uint16_t count, uint8_t menu_kind) {
  if (s_window == NULL || count == 0u) return;
  ui_destroy_content();
  s_menu_count = count > 9u ? 9u : count;
  for (uint16_t i = 0u; i < s_menu_count; i++) {
    snprintf(s_menu_labels[i], sizeof(s_menu_labels[i]), "%s", labels[i]);
  }
  s_menu_kind = menu_kind;
  Layer *root_layer = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root_layer);
  ui_add_header(title);
  s_menu_layer = menu_layer_create(GRect(0, UI_HEADER_HEIGHT, bounds.size.w, bounds.size.h - UI_HEADER_HEIGHT));
  if (s_menu_layer == NULL) return;
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_get_rows,
    .draw_row = menu_draw_row,
    .get_cell_height = menu_row_height,
  });
  menu_layer_set_normal_colors(s_menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu_layer, UI_ACCENT, GColorWhite);
  layer_add_child(root_layer, menu_layer_get_layer(s_menu_layer));
  window_set_click_config_provider(s_window, ui_click_config);
}

static void ui_rebuild(void) {
  if (s_exiting) return;
  if (s_stay_on_menu) s_screen = HERMES_SCREEN_MENU;
  ui_destroy_content();
  switch (s_screen) {
    case HERMES_SCREEN_MENU:
      ui_show_menu();
      break;
    case HERMES_SCREEN_SETTINGS:
      ui_show_settings();
      break;
    case HERMES_SCREEN_INK:
      ink_show();
      break;
    case HERMES_SCREEN_INK_STATUS:
      ui_show_notice(HERMES_SCREEN_INK_STATUS, "Handwriting", ROW_INK,
          s_ink_saved ? "Waiting to sync" : "Note delivered", s_ink_status,
          s_ink_saved ? "SELECT: retry sync now" : "SELECT: new note");
      break;
    case HERMES_SCREEN_REVIEW:
      ui_show_review();
      break;
    case HERMES_SCREEN_STATUS:
      ui_show_status();
      break;
    case HERMES_SCREEN_RESULT:
      ui_show_result();
      break;
    case HERMES_SCREEN_RECENT:
      ui_show_recent();
      break;
    case HERMES_SCREEN_ACTIONS:
      ui_show_actions();
      break;
    case HERMES_SCREEN_RECOVERY:
      ui_show_recovery();
      break;
    case HERMES_SCREEN_DICTATION:
      ui_show_dictation();
      break;
    case HERMES_SCREEN_ERROR:
      ui_show_notice(HERMES_SCREEN_ERROR, "Hermes", ROW_ERROR, "Needs attention", s_error_text, "SELECT: main menu");
      break;
    case HERMES_SCREEN_CONNECTING:
      ui_show_notice(HERMES_SCREEN_CONNECTING, "Connect", ROW_RECONNECT, "Connecting…",
          "Waiting for the phone. If this takes long, open Hermes Pebble on Android and select your Pebble host in Setup.", NULL);
      break;
    default:
      s_screen = HERMES_SCREEN_MENU;
      ui_show_menu();
      break;
  }
}

static void ui_show_menu(void) {
  const char *labels[9];
  uint8_t count = 0u;
  if (s_storage_corrupt || s_pending.operation != HERMES_PENDING_NONE) {
    s_menu_rows[count++] = ROW_DRAFT;
  } else {
    s_menu_rows[count++] = ROW_ASK;
    s_menu_rows[count++] = ROW_NOTE;
  }
  s_menu_rows[count++] = ROW_INK;
  s_menu_rows[count++] = ROW_RECENT;
  s_menu_rows[count++] = ROW_STATUS;
  s_menu_rows[count++] = ROW_NEW;
  s_menu_rows[count++] = ROW_SETTINGS;
  for (uint8_t i = 0u; i < count; i++) labels[i] = ui_row_title(s_menu_rows[i]);
  s_screen = HERMES_SCREEN_MENU;
  build_menu("Hermes", labels, count, HERMES_MENU_MAIN);
}

static void ui_show_settings(void) {
  const char *labels[3];
  cancel_auto_result();
  s_menu_rows[0] = ROW_TOUCH;
  s_menu_rows[1] = ROW_RECONNECT;
  s_menu_rows[2] = ROW_BIKE;
  for (uint8_t i = 0u; i < 3u; i++) labels[i] = ui_row_title(s_menu_rows[i]);
  s_screen = HERMES_SCREEN_SETTINGS;
  build_menu("Settings", labels, 3u, HERMES_MENU_SETTINGS);
}

static void touch_navigation_apply(void) {
#ifdef _PBL_API_EXISTS_app_touch_navigation_enable
  app_touch_navigation_enable(s_touch_navigation_enabled);
#endif
}

static void touch_navigation_load(void) {
  uint32_t enabled = 1u;
  // Missing/invalid preferences default to On; capture storage stays independent.
  s_touch_navigation_enabled = !storage_read_u32(HERMES_STORAGE_KEY_TOUCH_NAVIGATION, &enabled) || enabled != 0u;
  touch_navigation_apply();
}

static bool touch_navigation_save(bool enabled) {
  if (!storage_write_u32(HERMES_STORAGE_KEY_TOUCH_NAVIGATION, enabled ? 1u : 0u)) return false;
  s_touch_navigation_enabled = enabled;
  touch_navigation_apply();
  return true;
}

static void ui_show_review(void) {
  bool note = s_capture_mode == HERMES_CAPTURE_NOTE;
  ui_destroy_content();
  ui_block(UI_BLOCK_LABEL, note ? "NOTE TO SAVE" : "YOU SAID", GColorDarkGray, 0u);
  ui_block(UI_BLOCK_TEXT, s_capture_text, GColorBlack, 0u);
  ui_present(HERMES_SCREEN_REVIEW, note ? "Review note" : "Review", false, true,
      note ? "SELECT: save or redo" : "SELECT: send or redo");
}

/* Copy at most limit bytes without splitting a UTF-8 sequence. */
static size_t ui_copy_preview(char *output, size_t capacity, const char *text, size_t limit) {
  size_t length = strlen(text);
  bool cut = length > limit;
  if (cut) {
    length = limit;
    while (length > 0u && is_utf8_continuation((uint8_t)text[length])) length--;
  }
  if (length + 4u > capacity) return 0u;
  memcpy(output, text, length);
  if (cut) {
    memcpy(output + length, "…", 3u);
    length += 3u;
  }
  output[length] = '\0';
  return length + 1u;
}

static void ui_status_caption(const char *text) {
  size_t used = strlen(s_body_text);
  snprintf(s_body_text + used, sizeof(s_body_text) - used, "%s%s", used != 0u ? "\n" : "", text);
}

static void ui_show_status(void) {
  bool note = s_visible_item_kind == HERMES_ITEM_KIND_NOTE;
  size_t used;
  ui_destroy_content();
  s_body_text[0] = '\0';
  if (!s_handshake_ready) ui_status_caption("Phone not linked. Reconnect from Settings.");
  if (s_visible_error != HERMES_ERROR_NONE) ui_status_caption(error_text(s_visible_error));
  if (s_visible_flags & HERMES_FLAG_MORE) ui_status_caption("More on your phone.");
  if (s_voice_capture_id == s_visible_capture_id && s_voice_status[0]) ui_status_caption(s_voice_status);
  if (s_result_loading) ui_status_caption("Loading the answer…");
  if (s_pending.operation != HERMES_PENDING_NONE) ui_status_caption("Your draft stays on this watch until the phone saves it.");
  if (s_visible_status == HERMES_STATUS_NONE && s_visible_input[0] == '\0') ui_status_caption("Choose Ask Hermes in the menu to start.");
  ui_block(UI_BLOCK_PILL, s_visible_status == HERMES_STATUS_NONE ? "Ready for a new request" : status_text(s_visible_status),
      status_color(s_visible_status), 0u);
  if (s_body_text[0] != '\0') ui_block(UI_BLOCK_CAPTION, s_body_text, GColorDarkGray, 0u);
  if (s_visible_input[0] != '\0') {
    ui_block(UI_BLOCK_LABEL, note ? "YOUR NOTE" : "YOU ASKED", GColorDarkGray, 0u);
    ui_block(UI_BLOCK_QUOTE, s_visible_input, UI_ACCENT, 0u);
  }
  used = strlen(s_body_text) + 1u;
  if (s_visible_output[0] != '\0' && used < sizeof(s_body_text) &&
      ui_copy_preview(s_body_text + used, sizeof(s_body_text) - used, s_visible_output, 600u) != 0u) {
    ui_block(UI_BLOCK_LABEL, "ANSWER", GColorDarkGray, 0u);
    ui_block(UI_BLOCK_BODY, s_body_text + used, GColorBlack, 0u);
  }
  ui_present(HERMES_SCREEN_STATUS, note ? "Note" : "Request", false, true, NULL);
}

static void ui_show_result(void) {
  uint16_t offset = s_result_scroll_offset;
  ui_destroy_content();
  if (s_visible_input[0] != '\0' && s_result_window_offset == 0u) {
    ui_block(UI_BLOCK_LABEL, s_visible_item_kind == HERMES_ITEM_KIND_NOTE ? "YOUR NOTE" : "YOU ASKED", GColorDarkGray, 0u);
    ui_block(UI_BLOCK_QUOTE, s_visible_input, UI_ACCENT, 0u);
  }
  if (s_visible_output[0] != '\0') {
    ui_block(UI_BLOCK_TEXT, s_visible_output, GColorBlack, 0u);
    s_blocks[s_block_count - 1u].h = result_text_height() + 8;
  } else {
    ui_block(UI_BLOCK_CAPTION, "No answer text yet. SELECT to refresh.", GColorDarkGray, 0u);
  }
  s_result_answer_block = s_block_count - 1u;
  ui_present(HERMES_SCREEN_RESULT, "Answer", false, true, s_result_retry ? "DOWN: retry loading" : (s_voice_status[0] ? s_voice_status :
      voice_reply_available() ? "UP at top: play voice" : NULL));
  ui_scroll(s_result_scroll_to_end ? MAX_TEXT_HEIGHT : offset);
  if (s_result_more && !s_result_window_full && !s_result_retry && !s_result_loading &&
      s_result_next_offset != s_result_window_end) s_result_prefetch = true;
  timer_update();
}

/* A new chunk extends the answer in place, keeping layers and reading position. */
static void ui_result_refresh(void) {
  UiBlock *block;
  int16_t view;
  int16_t height;
  if (s_screen != HERMES_SCREEN_RESULT || s_content_layer == NULL || s_hint_layer != NULL ||
      s_result_answer_block >= s_block_count || s_blocks[s_result_answer_block].kind != UI_BLOCK_TEXT) {
    ui_show_result();
    return;
  }
  block = &s_blocks[s_result_answer_block];
  block->h = result_text_height() + 8;
  view = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h;
  height = block->y + block->h + 12;
  if (height < view) height = view;
  s_scroll_content_height = (uint16_t)height;
  layer_set_frame(s_content_layer, GRect(0, 0, layer_get_bounds(s_content_layer).size.w, height));
  scroll_layer_set_content_size(s_scroll_layer, GSize(layer_get_bounds(s_content_layer).size.w, height));
  ui_scroll(s_result_scroll_to_end ? MAX_TEXT_HEIGHT : 0);
  layer_mark_dirty(s_content_layer);
  timer_update();
}

static void ui_show_recent(void) {
  const char *labels[HERMES_MAX_RECENT_ITEMS];
  uint16_t count = s_recent_count;
  uint16_t i;
  for (i = 0u; i < count; i++) {
    labels[i] = s_recent_items[i].label;
  }
  if (count == 0u) {
    ui_show_notice(HERMES_SCREEN_RECENT, "Recent", ROW_RECENT, "Nothing recent",
        "Requests and notes appear here once your phone has saved them.", "SELECT: refresh");
    return;
  }
  s_screen = HERMES_SCREEN_RECENT;
  build_menu("Recent", labels, count, HERMES_MENU_RECENT);
}

static void ui_show_recovery(void) {
  size_t used;
  if (s_storage_corrupt) {
    ui_show_notice(HERMES_SCREEN_RECOVERY, "Saved draft", ROW_ERROR, "Draft storage damaged",
        "The watch will not overwrite it. Choose Discard to request removal from the phone.", NULL);
    s_select_tab = true;
    return;
  }
  if (s_pending.operation == HERMES_PENDING_NONE) {
    ui_show_notice(HERMES_SCREEN_RECOVERY, "Saved draft", ROW_DRAFT, "No saved draft", "Everything has reached your phone.", NULL);
    return;
  }
  ui_destroy_content();
  snprintf(s_body_text, sizeof(s_body_text), "%s · conversation %lu%s%s",
      s_pending.operation == HERMES_PENDING_NOTE ? "Note" : "Request", (unsigned long)s_pending.generation,
      s_visible_error != HERMES_ERROR_NONE ? "\nLast error: " : "",
      s_visible_error != HERMES_ERROR_NONE ? error_text(s_visible_error) : "");
  used = strlen(s_body_text) + 1u;
  snprintf(s_body_text + used, sizeof(s_body_text) - used, "Resume reuses capture %lu, so nothing is sent twice.",
      (unsigned long)s_pending.capture_id);
  ui_block(UI_BLOCK_PILL, "Not sent yet", GColorOrange, 0u);
  ui_block(UI_BLOCK_CAPTION, s_body_text, GColorDarkGray, 0u);
  ui_block(UI_BLOCK_QUOTE, s_pending.text, GColorOrange, 0u);
  ui_block(UI_BLOCK_CAPTION, s_body_text + used, GColorDarkGray, 0u);
  ui_present(HERMES_SCREEN_RECOVERY, "Saved draft", false, true, NULL);
}

static void ui_show_dictation(void) {
  ui_show_notice(HERMES_SCREEN_DICTATION, "Dictation", ROW_ASK, "Listening…",
      "Speak now. You'll review the text before anything is sent.", NULL);
  s_select_tab = true;
}

static void ui_show_error(uint32_t code, const char *text) {
  s_visible_error = (uint8_t)code;
  snprintf(s_error_text, sizeof(s_error_text), "%s", text);
  s_screen = HERMES_SCREEN_ERROR;
  ui_rebuild();
}

static void ui_show_actions(void) {
  const char *labels[8];
  uint8_t actions[8];
  uint8_t count = 0u;
  uint8_t original = s_previous_screen;
  if (original == HERMES_SCREEN_REVIEW) {
    if (s_capture_mode == HERMES_CAPTURE_NOTE) {
      labels[count] = "Save local note";
      actions[count++] = HERMES_ACTION_SAVE_NOTE;
    } else {
      labels[count] = "Send to Hermes";
      actions[count++] = HERMES_ACTION_SEND;
      labels[count] = "Send + voice reply";
      actions[count++] = HERMES_ACTION_SEND_VOICE;
      labels[count] = "Save as local note";
      actions[count++] = HERMES_ACTION_SAVE_NOTE;
    }
    labels[count] = "Dictate again";
    actions[count++] = HERMES_ACTION_REDICTATE;
    labels[count] = "Cancel review";
    actions[count++] = HERMES_ACTION_CANCEL;
  } else if (original == HERMES_SCREEN_STATUS) {
    if (s_visible_capture_id != 0u && s_visible_status != HERMES_STATUS_COMPLETED && s_visible_status != HERMES_STATUS_CANCELLED && s_visible_status != HERMES_STATUS_FAILED && s_visible_status != HERMES_STATUS_INTERRUPTED) {
      labels[count] = "Stop request";
      actions[count++] = HERMES_ACTION_STOP;
    }
    if (s_visible_capture_id != 0u && (s_visible_status == HERMES_STATUS_COMPLETED || s_visible_status == HERMES_STATUS_OUTCOME_UNKNOWN || s_visible_status == HERMES_STATUS_ACCEPTED || s_visible_status == HERMES_STATUS_WORKING)) {
      labels[count] = "Fetch result";
      actions[count++] = HERMES_ACTION_FETCH_RESULT;
    }
    labels[count] = "Recent";
    actions[count++] = HERMES_ACTION_RECENT;
    labels[count] = "Back to status";
    actions[count++] = HERMES_ACTION_STATUS;
  } else if (original == HERMES_SCREEN_RESULT) {
    if (s_visible_item_kind == HERMES_ITEM_KIND_REQUEST &&
        s_visible_status == HERMES_STATUS_COMPLETED && s_visible_generation == s_generation && s_pending.operation == HERMES_PENDING_NONE) {
      labels[count] = "Reply to Hermes";
      actions[count++] = HERMES_ACTION_REPLY;
    }
    if (voice_reply_available()) {
      labels[count] = "Play voice reply";
      actions[count++] = HERMES_ACTION_PLAY_VOICE;
    }
    labels[count] = "Refresh answer";
    actions[count++] = HERMES_ACTION_FETCH_RESULT;
    labels[count] = "Recent";
    actions[count++] = HERMES_ACTION_RECENT;
    labels[count] = "Status";
    actions[count++] = HERMES_ACTION_STATUS;
  } else if (original == HERMES_SCREEN_RECOVERY) {
    if (!s_storage_corrupt && s_pending.operation != HERMES_PENDING_NONE) {
      labels[count] = "Resume send";
      actions[count++] = HERMES_ACTION_RESUME;
    }
    if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
      labels[count] = "Discard pending capture";
      actions[count++] = HERMES_ACTION_DISCARD;
    }
    labels[count] = "Back to recovery";
    actions[count++] = HERMES_ACTION_BACK;
  } else if (original == HERMES_SCREEN_DICTATION) {
    labels[count] = "Dictate again";
    actions[count++] = HERMES_ACTION_REDICTATE;
    labels[count] = "Cancel dictation";
    actions[count++] = HERMES_ACTION_CANCEL;
  } else {
    labels[count] = "Main menu";
    actions[count++] = HERMES_ACTION_MENU;
  }
  memcpy(s_action_options, actions, count);
  s_action_count = count;
  s_screen = HERMES_SCREEN_ACTIONS;
  build_menu("Actions", labels, count, HERMES_MENU_ACTIONS);
}

static void menu_select(void *context, MenuLayer *menu_layer, MenuIndex *selection) {
  (void)context;
  (void)menu_layer;
  if (selection == NULL) {
    return;
  }
  s_stay_on_menu = false;
  if (s_menu_kind == HERMES_MENU_MAIN && selection->row < s_menu_count) {
    switch (s_menu_rows[selection->row]) {
      case ROW_ASK:
        start_dictation(HERMES_CAPTURE_REQUEST);
        break;
      case ROW_NOTE:
        start_dictation(HERMES_CAPTURE_NOTE);
        break;
      case ROW_INK:
        ink_open();
        break;
      case ROW_RECENT:
        start_fetch_recent();
        break;
      case ROW_STATUS:
        s_screen = HERMES_SCREEN_STATUS;
        ui_rebuild();
        break;
      case ROW_NEW:
        start_new_conversation();
        break;
      case ROW_SETTINGS:
        ui_show_settings();
        break;
      case ROW_DRAFT:
        s_screen = HERMES_SCREEN_RECOVERY;
        ui_rebuild();
        break;
      default:
        break;
    }
  } else if (s_menu_kind == HERMES_MENU_INK) {
    ink_menu_select(selection->row);
  } else if (s_menu_kind == HERMES_MENU_SETTINGS && selection->row < s_menu_count) {
    switch (s_menu_rows[selection->row]) {
      case ROW_TOUCH:
        if (touch_navigation_save(!s_touch_navigation_enabled)) ui_show_settings();
        else ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "Could not save touch navigation. Please try again.");
        break;
      case ROW_RECONNECT:
        start_handshake();
        break;
      case ROW_BIKE:
        cancel_auto_result();
        s_stay_on_menu = true;
        bike_open();
        break;
      default:
        break;
    }
  } else if (s_menu_kind == HERMES_MENU_RECENT) {
    if (selection->row < s_recent_count) {
      s_visible_capture_id = s_recent_items[selection->row].capture_id;
      s_visible_item_kind = s_recent_items[selection->row].kind;
      s_visible_status = s_recent_items[selection->row].state;
      s_visible_error = HERMES_ERROR_NONE;
      s_visible_flags = 0u;
      s_visible_input[0] = '\0';
      s_visible_output[0] = '\0';
      s_result_height_valid = false;
      s_result_offset = 0u;
      s_result_total_bytes = 0u;
      start_fetch_result(s_visible_capture_id, 0u);
    }
  }
}

static void action_menu_select(void *context, MenuLayer *menu_layer, MenuIndex *selection) {
  (void)context;
  (void)menu_layer;
  if (selection != NULL && selection->row < s_action_count) {
    ui_action(s_action_options[selection->row]);
  }
}

static void ui_scroll(int delta) {
  int next;
  int maximum;
  if (s_scroll_layer == NULL) {
    return;
  }
  maximum = (int)s_scroll_content_height - (int)(layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h);
  if (maximum < 0) {
    maximum = 0;
  }
  next = (int)s_scroll_offset + delta;
  if (next < 0) {
    next = 0;
  }
  if (next > maximum) {
    next = maximum;
  }
  s_scroll_offset = (uint16_t)next;
  if (s_screen == HERMES_SCREEN_RESULT) s_result_scroll_offset = s_scroll_offset;
  scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -(int)s_scroll_offset), false);
  if (s_overlay_layer != NULL) layer_mark_dirty(s_overlay_layer);
  if (result_prefetch_due()) timer_update();
}

/* Buttons page through text, keeping one line of context. */
static int ui_page_step(void) {
  int view;
  if (s_scroll_layer == NULL) return 0;
  view = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h;
  return view > 2 * SCROLL_PAGE_OVERLAP ? view - SCROLL_PAGE_OVERLAP : view;
}

static void ui_select_click(ClickRecognizerRef recognizer, void *context) {
  s_stay_on_menu = false;
  (void)recognizer;
  (void)context;
  if (s_screen == HERMES_SCREEN_ERROR) {
    ui_action(HERMES_ACTION_MENU);
  } else if (s_screen != HERMES_SCREEN_MENU && s_screen != HERMES_SCREEN_RECENT && s_screen != HERMES_SCREEN_ACTIONS) {
    s_previous_screen = s_screen;
    ui_show_actions();
  }
}

static bool voice_reply_available(void) {
  return s_visible_capture_id != 0u && s_visible_item_kind == HERMES_ITEM_KIND_REQUEST &&
      s_visible_status == HERMES_STATUS_COMPLETED && s_visible_output[0] != '\0';
}

static bool voice_shortcut_available(bool repeating) {
  // Holding Up to scroll into the top must not start speech. A new press does.
  return !repeating && s_screen == HERMES_SCREEN_RESULT && s_scroll_offset == 0u &&
      s_result_window_offset == 0u && s_result_previous_windows == NULL &&
      !s_voice_play_pending && voice_reply_available();
}

static void ui_up_click(ClickRecognizerRef recognizer, void *context) {
  (void)context;
  if (voice_shortcut_available(click_recognizer_is_repeating(recognizer))) {
    ui_action(HERMES_ACTION_PLAY_VOICE);
    return;
  }
  if (s_screen == HERMES_SCREEN_RESULT && s_scroll_offset == 0u && !s_result_loading &&
      result_move_window(false)) return;
  s_result_scroll_to_end = false;
  ui_scroll(-ui_page_step());
}

static void ui_down_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  uint16_t before = s_scroll_offset;
  ui_scroll(ui_page_step());
  if (s_screen == HERMES_SCREEN_RESULT && s_result_retry && !s_result_loading) {
    s_result_retry = false;
    s_result_prefetch = true;
    ui_show_result();
  } else if (s_screen == HERMES_SCREEN_RESULT && before == s_scroll_offset &&
      s_result_more && !s_result_loading && !s_result_prefetch) {
    result_move_window(true);
  }
}

static uint8_t menu_row_count(void) {
  if (s_menu_kind == HERMES_MENU_ACTIONS) {
    return s_action_count;
  }
  if (s_menu_kind == HERMES_MENU_RECENT) {
    return s_recent_count;
  }
  return (uint8_t)s_menu_count;
}

static void ui_menu_select_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  MenuIndex selection;
  int16_t selected;
  (void)context;
  if (s_menu_layer == NULL) {
    if (s_screen == HERMES_SCREEN_RECENT) {
      start_fetch_recent();
    }
    return;
  }
  selected = (int16_t)menu_layer_get_selected_index(s_menu_layer).row;
  if (selected < 0) {
    return;
  }
  memset(&selection, 0, sizeof(selection));
  selection.section = 0u;
  selection.row = (uint16_t)selected;
  if (s_menu_kind == HERMES_MENU_ACTIONS) {
    action_menu_select(NULL, s_menu_layer, &selection);
  } else {
    menu_select(NULL, s_menu_layer, &selection);
  }
}

static void ui_menu_move_click(int delta) {
  int16_t selected;
  int16_t next;
  uint8_t count;
  if (s_menu_layer == NULL) {
    return;
  }
  count = menu_row_count();
  if (count == 0u) {
    return;
  }
  selected = (int16_t)menu_layer_get_selected_index(s_menu_layer).row;
  if (selected < 0) {
    selected = 0;
  }
  next = selected + delta;
  if (next < 0) {
    next = 0;
  }
  if (next >= count) {
    next = (int16_t)count - 1;
  }
  menu_layer_set_selected_index(s_menu_layer, (MenuIndex){ .section = 0, .row = (uint16_t)next }, MenuRowAlignCenter, false);
}

static void ui_menu_up_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  ui_menu_move_click(-1);
}

static void ui_menu_down_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  ui_menu_move_click(1);
}

/* Back always moves toward the system launcher; pending storage is never cleared. */
static void cancel_auto_result(void) {
  s_voice_play_pending = false;
  s_follow_capture_id = 0u;
  s_auto_result_capture_id = 0u;
  s_auto_result_requested = false;
  s_result_loading = false;
  s_result_prefetch = false;
  s_result_retry = false;
  if (s_outbound.active && s_outbound.kind == HERMES_KIND_FETCH_RESULT) {
    s_outbound.active = false;
    s_outbound.phase = HERMES_OUT_IDLE;
    s_inbound.active = false;
  }
}

static bool should_auto_fetch_result(void) {
  return !s_exiting && !s_stay_on_menu && !s_auto_result_requested &&
      s_follow_capture_id != 0u && s_follow_capture_id == s_visible_capture_id &&
      s_visible_item_kind == HERMES_ITEM_KIND_REQUEST &&
      s_visible_status == HERMES_STATUS_COMPLETED && s_visible_error == HERMES_ERROR_NONE;
}

static bool navigation_back(void) {
  cancel_auto_result();
  if (s_outbound.kind != HERMES_KIND_INK_BLOCK) {
    s_outbound.active = false;
    s_outbound.phase = HERMES_OUT_IDLE;
  }
  s_inbound.active = false;
  s_phone_probe_id = 0u;
  if (s_screen == HERMES_SCREEN_MENU) {
    s_exiting = true;
    return true;
  }
  if (s_screen == HERMES_SCREEN_ACTIONS && s_previous_screen != HERMES_SCREEN_ACTIONS &&
      s_previous_screen <= HERMES_SCREEN_SETTINGS) {
    s_screen = s_previous_screen;
  } else {
    s_screen = HERMES_SCREEN_MENU;
  }
  s_stay_on_menu = s_screen == HERMES_SCREEN_MENU;
  return false;
}

static void ui_back_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  s_voice_capture_id = 0u;
  s_voice_status[0] = '\0';
  if (s_audio.phase == 1u || s_audio.phase == 2u) audio_terminal(HERMES_AUDIO_CANCELLED);
  if (s_screen == HERMES_SCREEN_INK_ACTIONS && !s_ink_corrupt) {
    ink_open();
    return;
  }
  stop_dictation();
  bool exit_app = navigation_back();
  timer_update();
  if (exit_app) window_stack_pop(false);
  else ui_rebuild();
}

static void ui_click_config(void *context) {
  (void)context;
  if (s_screen == HERMES_SCREEN_INK || s_screen == HERMES_SCREEN_INK_STATUS) {
    ink_click_config();
    return;
  }
  if (s_menu_layer != NULL) {
    window_single_click_subscribe(BUTTON_ID_SELECT, ui_menu_select_click);
    window_single_repeating_click_subscribe(BUTTON_ID_UP, MENU_REPEAT_MS, ui_menu_up_click);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, MENU_REPEAT_MS, ui_menu_down_click);
    window_single_click_subscribe(BUTTON_ID_BACK, ui_back_click);
    return;
  }
  window_single_click_subscribe(BUTTON_ID_SELECT, s_screen == HERMES_SCREEN_RECENT ? ui_menu_select_click : ui_select_click);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, SCROLL_REPEAT_MS, ui_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, SCROLL_REPEAT_MS, ui_down_click);
  window_single_click_subscribe(BUTTON_ID_BACK, ui_back_click);
}

static void ui_action(uint8_t action) {
  switch (action) {
    case HERMES_ACTION_REPLY:
      // A normal request in the current generation reuses the phone's session.
      start_dictation(HERMES_CAPTURE_REQUEST);
      break;
    case HERMES_ACTION_PLAY_VOICE:
      if (voice_reply_available()) {
        s_voice_capture_id = s_visible_capture_id;
        s_voice_play_pending = true;
        s_voice_requested = true; // Only the queued fetch below may request speech.
        snprintf(s_voice_status, sizeof(s_voice_status), "Requesting voice reply");
        s_screen = HERMES_SCREEN_RESULT;
        ui_rebuild();
        timer_update();
      }
      break;
    case HERMES_ACTION_SEND_VOICE:
      prepare_capture(HERMES_PENDING_REQUEST);
      if (s_pending.operation == HERMES_PENDING_REQUEST && s_screen == HERMES_SCREEN_STATUS) {
        s_voice_capture_id = s_pending.capture_id;
        s_voice_requested = false;
        snprintf(s_voice_status, sizeof(s_voice_status), "Voice reply enabled");
        ui_rebuild();
      }
      break;
    case HERMES_ACTION_SEND:
      if (s_capture_mode == HERMES_CAPTURE_NOTE) {
        prepare_capture(HERMES_PENDING_NOTE);
      } else {
        prepare_capture(HERMES_PENDING_REQUEST);
      }
      break;
    case HERMES_ACTION_SAVE_NOTE:
      prepare_capture(HERMES_PENDING_NOTE);
      break;
    case HERMES_ACTION_REDICTATE:
      if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
        s_screen = HERMES_SCREEN_RECOVERY;
        ui_rebuild();
      } else {
        stop_dictation();
        start_dictation(s_capture_mode == HERMES_CAPTURE_NOTE ? HERMES_CAPTURE_NOTE : HERMES_CAPTURE_REQUEST);
      }
      break;
    case HERMES_ACTION_CANCEL:
      stop_dictation();
      if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
        s_screen = HERMES_SCREEN_RECOVERY;
        ui_rebuild();
      } else {
        s_capture_text[0] = '\0';
        s_screen = HERMES_SCREEN_MENU;
        ui_rebuild();
      }
      break;
    case HERMES_ACTION_STOP:
      if (s_visible_capture_id != 0u) {
        start_stop_request(s_visible_capture_id);
      }
      break;
    case HERMES_ACTION_FETCH_RESULT:
      if (s_visible_capture_id != 0u) {
        start_fetch_result(s_visible_capture_id, 0u);
      }
      break;
    case HERMES_ACTION_RECENT:
      start_fetch_recent();
      break;
    case HERMES_ACTION_MENU:
      cancel_auto_result();
      s_stay_on_menu = true;
      s_screen = HERMES_SCREEN_MENU;
      ui_rebuild();
      break;
    case HERMES_ACTION_RESUME:
      if (s_storage_corrupt) {
        ui_rebuild();
      } else if (s_pending.operation != HERMES_PENDING_NONE) {
        resume_pending_capture();
      } else {
        s_screen = HERMES_SCREEN_MENU;
        ui_rebuild();
      }
      break;
    case HERMES_ACTION_DISCARD:
      if (s_storage_corrupt) {
        start_discard_capture(s_discard_capture_id);
      } else if (s_pending.operation != HERMES_PENDING_NONE) {
        start_discard_capture(s_pending.capture_id);
      }
      break;
    case HERMES_ACTION_STATUS:
      s_screen = HERMES_SCREEN_STATUS;
      ui_rebuild();
      break;
    case HERMES_ACTION_BACK:
      s_screen = s_previous_screen == HERMES_SCREEN_RECOVERY ? HERMES_SCREEN_RECOVERY : s_previous_screen;
      ui_rebuild();
      break;
    default:
      break;
  }
}

static const char *status_text(uint8_t status) {
  switch (status) {
    case HERMES_STATUS_NONE:
      return "None";
    case HERMES_STATUS_WAITING_PHONE:
      return "Waiting for phone";
    case HERMES_STATUS_SAVED_QUEUED:
      return "Saved and queued on phone";
    case HERMES_STATUS_SUBMITTING:
      return "Submitting to Hermes";
    case HERMES_STATUS_ACCEPTED:
      return "Accepted by Hermes";
    case HERMES_STATUS_WORKING:
      return "Hermes working";
    case HERMES_STATUS_APPROVAL_NEEDED:
      return "Approval needed in Hermes";
    case HERMES_STATUS_COMPLETED:
      return "Completed";
    case HERMES_STATUS_FAILED:
      return "Failed";
    case HERMES_STATUS_STOPPING:
      return "Stop requested";
    case HERMES_STATUS_CANCELLED:
      return "Cancelled";
    case HERMES_STATUS_INTERRUPTED:
      return "Interrupted";
    case HERMES_STATUS_OUTCOME_UNKNOWN:
      return "Outcome unknown - review before retrying";
    case HERMES_STATUS_NOTE_SAVED:
      return "Local note saved";
    case HERMES_STATUS_WAITING_PROFILE:
      return "Waiting for previous connection profile";
    case HERMES_STATUS_DISCARDED:
      return "Discarded";
    default:
      return "Unknown status";
  }
}

static const char *error_text(uint32_t code) {
  switch (code) {
    case HERMES_ERROR_NONE:
      return "None";
    case HERMES_ERROR_PROTOCOL_VERSION:
      return "Update the watch and phone apps";
    case HERMES_ERROR_UNSUPPORTED_KIND:
      return "Unsupported message";
    case HERMES_ERROR_MALFORMED:
      return "Malformed message";
    case HERMES_ERROR_TRANSFER_BOUND:
      return "Transfer exceeds a bound";
    case HERMES_ERROR_DUPLICATE_CHUNK:
      return "Conflicting duplicate chunk";
    case HERMES_ERROR_DURABLE_STORAGE:
      return "Phone could not store the command";
    case HERMES_ERROR_CONNECTION_SETTINGS:
      return "Connection settings are incomplete";
    case HERMES_ERROR_AUTHENTICATION:
      return "Authentication rejected";
    case HERMES_ERROR_NETWORK_TLS:
      return "Network or TLS failure";
    case HERMES_ERROR_REDIRECT:
      return "Redirect rejected";
    case HERMES_ERROR_UNSUPPORTED_ROUTE:
      return "Required Hermes route is unsupported";
    case HERMES_ERROR_INVALID_RESPONSE:
      return "Invalid Hermes response";
    case HERMES_ERROR_UNKNOWN_OUTCOME:
      return "Submission outcome unknown";
    case HERMES_ERROR_NOT_FOUND:
      return "Command or result not found";
    case HERMES_ERROR_IDEMPOTENCY_CONFLICT:
      return "Durable state or idempotency conflict";
    case HERMES_ERROR_SERVER:
      return "Hermes server error";
    case HERMES_ERROR_NOTE_STORAGE:
      return "Local note could not be saved";
    case HERMES_ERROR_WATCH_UNAVAILABLE:
      return "Watch or phone host unavailable";
    default:
      return "Unknown error";
  }
}

static const char *dictation_failure_text(int status) {
  switch ((DictationSessionStatus)status) {
    case DictationSessionStatusSuccess: return "Dictation completed.";
    case DictationSessionStatusFailureNoSpeechDetected: return "No speech detected. Try again.";
    case DictationSessionStatusFailureConnectivityError: return "Check the phone and internet connection.";
    case DictationSessionStatusFailureDisabled: return "Dictation is disabled by the phone service.";
    case DictationSessionStatusFailureTranscriptionRejected:
    case DictationSessionStatusFailureTranscriptionRejectedWithError:
    case DictationSessionStatusFailureSystemAborted: return "Dictation cancelled. Nothing was sent.";
    case DictationSessionStatusFailureInternalError:
    case DictationSessionStatusFailureRecognizerError:
    default: return "Dictation failed. Nothing was sent. Try again.";
  }
}

static bool is_utf8_continuation(uint8_t value) {
  return (value & 0xc0u) == 0x80u;
}

static bool utf8_valid(const uint8_t *data, uint16_t length) {
  uint16_t i = 0u;
  while (i < length) {
    uint8_t first = data[i];
    if (first == 0u) {
      return false;
    }
    if (first <= 0x7fu) {
      i++;
      continue;
    }
    if (first >= 0xc2u && first <= 0xdfu) {
      if (i + 1u >= length || !is_utf8_continuation(data[i + 1u])) {
        return false;
      }
      i += 2u;
      continue;
    }
    if (first == 0xe0u) {
      if (i + 2u >= length || data[i + 1u] < 0xa0u || data[i + 1u] > 0xbfu || !is_utf8_continuation(data[i + 2u])) {
        return false;
      }
      i += 3u;
      continue;
    }
    if ((first >= 0xe1u && first <= 0xecu) || (first >= 0xeeu && first <= 0xefu)) {
      if (i + 2u >= length || !is_utf8_continuation(data[i + 1u]) || !is_utf8_continuation(data[i + 2u])) {
        return false;
      }
      i += 3u;
      continue;
    }
    if (first == 0xedu) {
      if (i + 2u >= length || data[i + 1u] < 0x80u || data[i + 1u] > 0x9fu || !is_utf8_continuation(data[i + 2u])) {
        return false;
      }
      i += 3u;
      continue;
    }
    if (first == 0xf0u) {
      if (i + 3u >= length || data[i + 1u] < 0x90u || data[i + 1u] > 0xbfu || !is_utf8_continuation(data[i + 2u]) || !is_utf8_continuation(data[i + 3u])) {
        return false;
      }
      i += 4u;
      continue;
    }
    if (first >= 0xf1u && first <= 0xf3u) {
      if (i + 3u >= length || !is_utf8_continuation(data[i + 1u]) || !is_utf8_continuation(data[i + 2u]) || !is_utf8_continuation(data[i + 3u])) {
        return false;
      }
      i += 4u;
      continue;
    }
    if (first == 0xf4u) {
      if (i + 3u >= length || data[i + 1u] < 0x80u || data[i + 1u] > 0x8fu || !is_utf8_continuation(data[i + 2u]) || !is_utf8_continuation(data[i + 3u])) {
        return false;
      }
      i += 4u;
      continue;
    }
    return false;
  }
  return true;
}

static void pending_make_chunk_plan(const uint8_t *data, uint16_t length, uint8_t *chunk_lengths, uint16_t *offsets, uint8_t *count) {
  uint16_t position = 0u;
  uint8_t used = 0u;
  if (length == 0u) {
    chunk_lengths[0] = 0u;
    offsets[0] = 0u;
    *count = 1u;
    return;
  }
  while (position < length && used < HERMES_MAX_CHUNKS) {
    uint16_t remaining = (uint16_t)(length - position);
    uint16_t take = remaining > HERMES_CHUNK_PAYLOAD_SIZE ? HERMES_CHUNK_PAYLOAD_SIZE : remaining;
    while (take > 0u && (uint16_t)(position + take) < length && is_utf8_continuation(data[position + take])) {
      take--;
    }
    if (take == 0u) {
      *count = 0u;
      return;
    }
    offsets[used] = position;
    chunk_lengths[used] = (uint8_t)take;
    position = (uint16_t)(position + take);
    used++;
  }
  if (position != length) {
    *count = 0u;
    return;
  }
  *count = used;
}

static uint32_t pending_crc(const PendingCapture *pending) {
  uint32_t crc = 0xffffffffu;
  uint8_t encoded[HERMES_STORAGE_META_SIZE];
  size_t i;
  memset(encoded, 0, sizeof(encoded));

  encoded[0] = (uint8_t)(pending->operation & 0xffu);
  encoded[1] = (uint8_t)(pending->capture_id & 0xffu);
  encoded[2] = (uint8_t)((pending->capture_id >> 8) & 0xffu);
  encoded[3] = (uint8_t)((pending->capture_id >> 16) & 0xffu);
  encoded[4] = (uint8_t)((pending->capture_id >> 24) & 0xffu);
  encoded[5] = (uint8_t)(pending->transfer_id & 0xffu);
  encoded[6] = (uint8_t)((pending->transfer_id >> 8) & 0xffu);
  encoded[7] = (uint8_t)((pending->transfer_id >> 16) & 0xffu);
  encoded[8] = (uint8_t)((pending->transfer_id >> 24) & 0xffu);
  encoded[9] = (uint8_t)(pending->generation & 0xffu);
  encoded[10] = (uint8_t)((pending->generation >> 8) & 0xffu);
  encoded[11] = (uint8_t)((pending->generation >> 16) & 0xffu);
  encoded[12] = (uint8_t)((pending->generation >> 24) & 0xffu);
  encoded[13] = (uint8_t)(pending->length & 0xffu);
  encoded[14] = (uint8_t)((pending->length >> 8) & 0xffu);
  encoded[15] = (uint8_t)pending->chunk_count;
  for (i = 0u; i < HERMES_STORAGE_META_SIZE; i++) {
    crc ^= encoded[i];
    for (uint8_t bit = 0u; bit < 8u; bit++) {
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  for (i = 0u; i < pending->length; i++) {
    crc ^= pending->text[i];
    for (uint8_t bit = 0u; bit < 8u; bit++) {
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return crc ^ 0xffffffffu;
}

static void encode_u32(uint8_t *buffer, size_t offset, uint32_t value) {
  buffer[offset] = (uint8_t)(value & 0xffu);
  buffer[offset + 1u] = (uint8_t)((value >> 8) & 0xffu);
  buffer[offset + 2u] = (uint8_t)((value >> 16) & 0xffu);
  buffer[offset + 3u] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t decode_u32(const uint8_t *buffer, size_t offset) {
  return (uint32_t)buffer[offset] | ((uint32_t)buffer[offset + 1u] << 8) | ((uint32_t)buffer[offset + 2u] << 16) | ((uint32_t)buffer[offset + 3u] << 24);
}

static void encode_pending_meta(const PendingCapture *pending, uint8_t *encoded, uint8_t commit) {
  memset(encoded, 0, HERMES_STORAGE_META_SIZE);
  encode_u32(encoded, 0u, HERMES_PENDING_MAGIC);
  encoded[4] = HERMES_PENDING_FORMAT_VERSION;
  encoded[5] = pending->operation;
  encoded[6] = pending->chunk_count;
  encoded[7] = commit;
  encode_u32(encoded, 8u, pending->capture_id);
  encode_u32(encoded, 12u, pending->transfer_id);
  encode_u32(encoded, 16u, pending->generation);
  encode_u32(encoded, 20u, pending->length);
  encode_u32(encoded, 24u, pending_crc(pending));
  encode_u32(encoded, 28u, s_counter);
  encode_u32(encoded, 32u, s_install_hash);
  encode_u32(encoded, 36u, HERMES_PENDING_MAGIC ^ HERMES_STORAGE_META_SIZE);
}

static bool decode_pending_meta(const uint8_t *encoded, PendingCapture *pending) {
  if (decode_u32(encoded, 0u) != HERMES_PENDING_MAGIC || encoded[4] != HERMES_PENDING_FORMAT_VERSION || encoded[7] != HERMES_PENDING_COMMIT) {
    return false;
  }
  if (decode_u32(encoded, 36u) != (HERMES_PENDING_MAGIC ^ HERMES_STORAGE_META_SIZE)) {
    return false;
  }
  pending->operation = encoded[5];
  pending->chunk_count = encoded[6];
  pending->capture_id = decode_u32(encoded, 8u);
  pending->transfer_id = decode_u32(encoded, 12u);
  pending->generation = decode_u32(encoded, 16u);
  pending->length = (uint16_t)decode_u32(encoded, 20u);
  if ((pending->operation != HERMES_PENDING_REQUEST && pending->operation != HERMES_PENDING_NOTE) || pending->capture_id == 0u || pending->transfer_id == 0u) {
    return false;
  }
  if (pending->length == 0u || pending->length > HERMES_MAX_DICTATION_BYTES || pending->chunk_count == 0u || pending->chunk_count > HERMES_MAX_CHUNKS) {
    return false;
  }
  return true;
}

static void storage_delete_chunks(void) {
  uint8_t i;
  for (i = 0u; i < HERMES_MAX_CHUNKS; i++) {
    (void)persist_delete(HERMES_STORAGE_KEY_CHUNK_BASE + i);
  }
}

static bool storage_read_u32(uint32_t key, uint32_t *value) {
  uint8_t encoded[4];
  if (persist_read_data(key, encoded, sizeof(encoded)) != (int)sizeof(encoded)) {
    return false;
  }
  *value = decode_u32(encoded, 0u);
  return true;
}

static bool storage_write_u32(uint32_t key, uint32_t value) {
  uint8_t encoded[4];
  encode_u32(encoded, 0u, value);
  return persist_write_data(key, encoded, sizeof(encoded)) == (int)sizeof(encoded);
}

static void storage_generate_install_id(uint8_t *output) {
  size_t i;
  unsigned int seed = (unsigned int)time(NULL);
  uintptr_t address = (uintptr_t)(void *)output;
  srand(seed ^ (unsigned int)address);
  for (i = 0u; i < 16u; i++) {
    output[i] = (uint8_t)(rand() ^ (unsigned int)(i * 37u) ^ (unsigned int)(time(NULL) + i));
  }
  output[0] = (uint8_t)(output[0] | 1u);
  output[15] = (uint8_t)(output[15] | 0x80u);
  s_install_hash = 2166136261u;
  for (i = 0u; i < 16u; i++) {
    s_install_hash ^= output[i];
    s_install_hash *= 16777619u;
  }
  if (s_install_hash == 0u) {
    s_install_hash = 1u;
    output[0] = 1u;
  }
}

static void storage_init(void) {
  bool valid = true;
  if (persist_read_data(HERMES_STORAGE_KEY_INSTALL, s_install_id, sizeof(s_install_id)) != (int)sizeof(s_install_id)) {
    if (persist_exists(HERMES_STORAGE_KEY_INSTALL)) {
      s_identity_valid = false;
      s_storage_corrupt = true;
      return;
    }
    storage_generate_install_id(s_install_id);
    valid = persist_write_data(HERMES_STORAGE_KEY_INSTALL, s_install_id, sizeof(s_install_id)) == (int)sizeof(s_install_id);
  } else {
    size_t i;
    s_install_hash = 2166136261u;
    for (i = 0u; i < 16u; i++) {
      s_install_hash ^= s_install_id[i];
      s_install_hash *= 16777619u;
    }
    if (s_install_hash == 0u) {
      s_install_hash = 1u;
    }
  }
  if (!storage_read_u32(HERMES_STORAGE_KEY_COUNTER, &s_counter)) {
    s_counter = 0u;
    valid = !persist_exists(HERMES_STORAGE_KEY_COUNTER) && storage_write_u32(HERMES_STORAGE_KEY_COUNTER, s_counter) && valid;
  }
  if (!storage_read_u32(HERMES_STORAGE_KEY_GENERATION, &s_generation)) {
    s_generation = 0u;
    valid = !persist_exists(HERMES_STORAGE_KEY_GENERATION) && storage_write_u32(HERMES_STORAGE_KEY_GENERATION, s_generation) && valid;
  }
  s_identity_valid = valid;
  if (!storage_load_pending()) s_storage_corrupt = true;
}

static bool storage_next_capture_id(uint32_t *capture_id) {
  if (!s_identity_valid) return false;
  do {
    if (s_counter == UINT32_MAX) return false;
    uint32_t next = s_counter + 1u;
    if (!storage_write_u32(HERMES_STORAGE_KEY_COUNTER, next)) return false;
    s_counter = next;
    *capture_id = mix_capture_id(next);
  } while (*capture_id == 0u);
  return true;
}

static bool storage_write_generation(uint32_t generation) {
  if (!storage_write_u32(HERMES_STORAGE_KEY_GENERATION, generation)) {
    return false;
  }
  s_generation = generation;
  return true;
}

static bool storage_save_pending(const PendingCapture *pending) {
  uint8_t chunk_lengths[HERMES_MAX_CHUNKS] = {0};
  uint16_t offsets[HERMES_MAX_CHUNKS];
  uint8_t encoded[HERMES_STORAGE_META_SIZE];
  static PendingCapture stored;
  uint8_t count;
  uint8_t i;
  if (pending == NULL || pending->length == 0u || pending->length > HERMES_MAX_DICTATION_BYTES || !utf8_valid((const uint8_t *)pending->text, pending->length)) {
    return false;
  }
  count = (uint8_t)((pending->length + HERMES_STORAGE_CHUNK_SIZE - 1u) / HERMES_STORAGE_CHUNK_SIZE);
  if (count == 0u || count > HERMES_MAX_CHUNKS) {
    return false;
  }
  for (i = 0u; i < count; i++) {
    uint16_t offset = (uint16_t)i * HERMES_STORAGE_CHUNK_SIZE;
    uint16_t remaining = (uint16_t)(pending->length - offset);
    chunk_lengths[i] = (uint8_t)(remaining > HERMES_STORAGE_CHUNK_SIZE ? HERMES_STORAGE_CHUNK_SIZE : remaining);
    offsets[i] = offset;
  }
  storage_delete_chunks();
  for (i = 0u; i < count; i++) {
    if (persist_write_data(HERMES_STORAGE_KEY_CHUNK_BASE + i, pending->text + offsets[i], chunk_lengths[i]) != chunk_lengths[i]) {
      storage_delete_chunks();
      (void)persist_delete(HERMES_STORAGE_KEY_PENDING_META);
      return false;
    }
  }
  stored = *pending;
  stored.chunk_count = count;
  memcpy(stored.chunk_lengths, chunk_lengths, sizeof(chunk_lengths));
  encode_pending_meta(&stored, encoded, 0u);
  if (persist_write_data(HERMES_STORAGE_KEY_PENDING_META, encoded, sizeof(encoded)) != (int)sizeof(encoded)) {
    storage_delete_chunks();
    (void)persist_delete(HERMES_STORAGE_KEY_PENDING_META);
    return false;
  }
  encode_pending_meta(&stored, encoded, HERMES_PENDING_COMMIT);
  if (persist_write_data(HERMES_STORAGE_KEY_PENDING_META, encoded, sizeof(encoded)) != (int)sizeof(encoded)) {
    storage_delete_chunks();
    (void)persist_delete(HERMES_STORAGE_KEY_PENDING_META);
    return false;
  }
  return true;
}

static bool storage_load_pending(void) {
  uint8_t encoded[HERMES_STORAGE_META_SIZE];
  static PendingCapture loaded;
  uint8_t count;
  uint8_t i;
  uint32_t expected_crc;
  uint32_t stored_crc;
  if (!persist_exists(HERMES_STORAGE_KEY_PENDING_META)) {
    storage_delete_chunks();
    memset(&s_pending, 0, sizeof(s_pending));
    s_storage_corrupt = false;
    s_discard_capture_id = 0u;
    return true;
  }
  if (persist_read_data(HERMES_STORAGE_KEY_PENDING_META, encoded, sizeof(encoded)) != (int)sizeof(encoded)) {
    s_storage_corrupt = true;
    s_discard_capture_id = 0u;
    return false;
  }
  if (encoded[7] != HERMES_PENDING_COMMIT) {
    storage_delete_chunks();
    (void)persist_delete(HERMES_STORAGE_KEY_PENDING_META);
    memset(&s_pending, 0, sizeof(s_pending));
    s_storage_corrupt = false;
    s_discard_capture_id = 0u;
    return true;
  }
  memset(&loaded, 0, sizeof(loaded));
  if (!decode_pending_meta(encoded, &loaded)) {
    s_storage_corrupt = true;
    s_discard_capture_id = decode_u32(encoded, 8u);
    return false;
  }
  if (decode_u32(encoded, 28u) != s_counter || decode_u32(encoded, 32u) != s_install_hash) {
    s_storage_corrupt = true;
    s_discard_capture_id = loaded.capture_id;
    return false;
  }
  count = (uint8_t)((loaded.length + HERMES_STORAGE_CHUNK_SIZE - 1u) / HERMES_STORAGE_CHUNK_SIZE);
  if (loaded.chunk_count != count) {
    s_storage_corrupt = true;
    s_discard_capture_id = loaded.capture_id;
    return false;
  }
  if (loaded.length == 0u) {
    s_storage_corrupt = true;
    s_discard_capture_id = loaded.capture_id;
    return false;
  }
  for (i = 0u; i < count; i++) {
    uint16_t expected_length = i + 1u < count ? HERMES_STORAGE_CHUNK_SIZE : (uint16_t)(loaded.length - (uint16_t)i * HERMES_STORAGE_CHUNK_SIZE);
    if (expected_length == 0u || expected_length > HERMES_STORAGE_CHUNK_SIZE || persist_read_data(HERMES_STORAGE_KEY_CHUNK_BASE + i, loaded.text + (uint16_t)i * HERMES_STORAGE_CHUNK_SIZE, expected_length) != expected_length) {
      s_storage_corrupt = true;
      s_discard_capture_id = loaded.capture_id;
      return false;
    }
  }
  loaded.text[loaded.length] = '\0';
  stored_crc = decode_u32(encoded, 24u);
  expected_crc = pending_crc(&loaded);
  if (stored_crc != expected_crc || !utf8_valid((const uint8_t *)loaded.text, loaded.length)) {
    s_storage_corrupt = true;
    s_discard_capture_id = loaded.capture_id;
    return false;
  }
  s_pending = loaded;
  s_discard_capture_id = loaded.capture_id;
  s_storage_corrupt = false;
  return true;
}

static bool storage_clear_pending(void) {
  bool success = true;
  uint8_t i;
  for (i = 0u; i < HERMES_MAX_CHUNKS; i++) {
    if (persist_exists(HERMES_STORAGE_KEY_CHUNK_BASE + i)) {
      persist_delete(HERMES_STORAGE_KEY_CHUNK_BASE + i);
      if (persist_exists(HERMES_STORAGE_KEY_CHUNK_BASE + i)) {
        success = false;
      }
    }
  }
  if (persist_exists(HERMES_STORAGE_KEY_PENDING_META)) {
    persist_delete(HERMES_STORAGE_KEY_PENDING_META);
    if (persist_exists(HERMES_STORAGE_KEY_PENDING_META)) {
      success = false;
    }
  }
  if (!success) {
    return false;
  }
  memset(&s_pending, 0, sizeof(s_pending));
  s_storage_corrupt = false;
  s_discard_capture_id = 0u;
  return true;
}

static uint32_t next_transfer_id(void) {
  uint32_t value;
  s_transfer_sequence++;
  value = s_install_hash ^ (s_transfer_sequence * 0x9e3779b9u) ^ (uint32_t)time(NULL) ^ (uint32_t)s_counter;
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  if (value == 0u) {
    value = 1u;
  }
  return value;
}

static uint32_t mix_capture_id(uint32_t counter) {
  uint32_t value = s_install_hash ^ (counter * 0x9e3779b9u);
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

static uint32_t clock_ms(void) {
  time_t seconds = 0;
  uint16_t milliseconds = 0u;
  time_ms(&seconds, &milliseconds);
  return (uint32_t)seconds * 1000u + milliseconds;
}

static uint32_t deadline_after(uint32_t seconds) {
  return clock_ms() + seconds * 1000u;
}

static int32_t deadline_remaining(uint32_t deadline, uint32_t now) {
  int32_t remaining = (int32_t)(deadline - now);
  // A wall-clock step backwards would postpone every wait; treat it as expired.
  return remaining > (int32_t)(HERMES_DURABLE_RECEIPT_TIMEOUT_SECONDS * 1000u) ? 0 : remaining;
}

static void outbound_wait(uint8_t phase, uint32_t seconds) {
  s_outbound.phase = phase;
  s_outbound.deadline_ms = deadline_after(seconds);
}

static bool result_prefetch_due(void) {
  int view;
  if (!s_result_prefetch || s_screen != HERMES_SCREEN_RESULT || s_scroll_layer == NULL) return false;
  // Lazy loading: fetch more text only when the reader approaches the end.
  view = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h;
  return (int)s_scroll_offset + view * (RESULT_PREFETCH_SCREENS + 1) >= (int)s_scroll_content_height;
}

static bool timer_work_ready(void) {
  bool idle = !s_outbound.active && !s_inbound.active;
  return (s_voice_play_pending && idle) || (s_ink_sync_requested && idle) || (s_phone_probe_id != 0u && !s_outbound.active) ||
      (s_auto_result_capture_id != 0u && idle && s_pending.operation == HERMES_PENDING_NONE) ||
      (result_prefetch_due() && idle);
}

/* Wake only for the next deadline or ready work, never on a fixed tick. */
static void timer_update(void) {
  uint32_t now;
  int32_t delay = INT32_MAX;
  if (s_timer != NULL) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  if (s_exiting) return;
  now = clock_ms();
  if (s_outbound.active) {
    int32_t remaining = s_outbound.phase == HERMES_OUT_SENDING ? (int32_t)TIMER_POLL_MS :
        deadline_remaining(s_outbound.deadline_ms, now);
    if (remaining < delay) delay = remaining;
  }
  if (s_inbound.active) {
    int32_t remaining = deadline_remaining(s_inbound_deadline_ms, now);
    if (remaining < delay) delay = remaining;
  }
  if (timer_work_ready() && delay > (int32_t)TIMER_SOON_MS) delay = (int32_t)TIMER_SOON_MS;
  if (delay == INT32_MAX) return;
  if (delay < 1) delay = 1;
  s_timer = app_timer_register((uint32_t)delay, timer_tick, NULL);
}

static void timer_tick(void *context) {
  uint32_t now;
  (void)context;
  s_timer = NULL;
  if (s_exiting) return;
  now = clock_ms();
  if (s_ink_sync_requested && !s_outbound.active && !s_inbound.active) ink_try_send();
  if (s_phone_probe_id != 0u && !s_outbound.active) {
    outbound_start(HERMES_KIND_HANDSHAKE, HERMES_KIND_HANDSHAKE_ACK, next_transfer_id(),
      0u, s_generation, 0u, 0u, 0u, HERMES_STATUS_NONE, HERMES_ERROR_NONE,
      HERMES_ITEM_KIND_NONE, HERMES_STATUS_NONE, 0u, 0u, NULL, 0u);
  }
  if (s_auto_result_capture_id != 0u && !s_outbound.active && !s_inbound.active &&
      s_pending.operation == HERMES_PENDING_NONE) {
    uint32_t capture = s_auto_result_capture_id;
    s_auto_result_capture_id = 0u;
    if (should_auto_fetch_result() && capture == s_visible_capture_id) {
      s_auto_result_requested = true;
      start_fetch_result(capture, 0u);
    }
  }
  // Wait for any text page/receipt already in flight before requesting replay.
  if (s_voice_play_pending && !s_outbound.active && !s_inbound.active) {
    s_voice_play_pending = false;
    if (s_voice_capture_id != 0u && s_voice_capture_id == s_visible_capture_id &&
        s_visible_status == HERMES_STATUS_COMPLETED) {
      s_voice_requested = false;
      start_fetch_result(s_voice_capture_id, 0u);
    }
  }
  if (result_prefetch_due() && !s_outbound.active && !s_inbound.active) {
    s_result_prefetch = false;
    start_result_chunk(s_visible_capture_id, s_result_next_offset);
  }
  if (s_inbound.active && deadline_remaining(s_inbound_deadline_ms, now) <= 0) {
    inbound_error(HERMES_ERROR_TRANSFER_BOUND, "The phone transfer timed out before all chunks arrived.");
  }
  if (s_outbound.active && s_outbound.phase != HERMES_OUT_SENDING &&
      deadline_remaining(s_outbound.deadline_ms, now) <= 0) {
    if (s_outbound.phase == HERMES_OUT_RETRY_WAIT) {
      s_outbound.phase = HERMES_OUT_SENDING;
      outbound_send_current_chunk();
    } else {
      outbound_timeout();
    }
  }
  timer_update();
}

static void outbound_start(uint8_t kind, uint8_t expected_kind, uint32_t transfer_id, uint32_t capture_id, uint32_t generation, uint32_t item_id, uint32_t page_offset, uint32_t total_bytes, uint8_t status, uint8_t error_code, uint8_t item_kind, uint8_t item_state, uint8_t page_count, uint8_t flags, const uint8_t *payload, uint16_t length) {
  uint8_t chunk_lengths[HERMES_MAX_CHUNKS];
  uint16_t chunk_offsets[HERMES_MAX_CHUNKS];
  uint8_t chunk_count;
  if (!s_app_message_open || s_outbound.active || length > HERMES_MAX_TRANSFER_BYTES || (payload == NULL && length != 0u)) {
    return;
  }
  if (transfer_id == 0u) {
    transfer_id = next_transfer_id();
  }
  memset(&s_outbound, 0, sizeof(s_outbound));
  s_outbound.active = true;
  s_outbound.phase = HERMES_OUT_SENDING;
  s_outbound.kind = kind;
  s_outbound.expected_kind = expected_kind;
  s_outbound.transfer_id = transfer_id;
  if (kind == HERMES_KIND_HANDSHAKE) {
    s_outbound.correlation_id = s_phone_probe_id;
    s_phone_probe_id = 0u;
  } else if (kind == HERMES_KIND_AUDIO_STATUS) {
    s_outbound.correlation_id = s_audio_reply.request;
  }
  s_outbound.capture_id = capture_id;
  s_outbound.generation = generation;
  s_outbound.item_id = item_id;
  s_outbound.page_offset = page_offset;
  s_outbound.total_bytes = total_bytes;
  s_outbound.status = status;
  s_outbound.error_code = error_code;
  s_outbound.item_kind = item_kind;
  s_outbound.item_state = item_state;
  s_outbound.page_count = page_count;
  s_outbound.flags = flags;
  s_outbound.length = length;
  if (payload != NULL && length > 0u) {
    memcpy(s_outbound.payload, payload, length);
  }
  if (length == 0u) {
    s_outbound.chunk_count = 1u;
    s_outbound.chunk_lengths[0] = 0u;
    s_outbound.chunk_offsets[0] = 0u;
  } else if (kind == HERMES_KIND_INK_BLOCK && length <= HERMES_CHUNK_PAYLOAD_SIZE) {
    s_outbound.chunk_count = 1u;
    s_outbound.chunk_lengths[0] = (uint8_t)length;
    s_outbound.chunk_offsets[0] = 0u;
  } else if (!utf8_valid(s_outbound.payload, length)) {
    memset(&s_outbound, 0, sizeof(s_outbound));
    return;
  } else {
    pending_make_chunk_plan(s_outbound.payload, length, chunk_lengths, chunk_offsets, &chunk_count);
    if (chunk_count == 0u) {
      memset(&s_outbound, 0, sizeof(s_outbound));
      return;
    }
    s_outbound.chunk_count = chunk_count;
    memcpy(s_outbound.chunk_lengths, chunk_lengths, sizeof(chunk_lengths));
    memcpy(s_outbound.chunk_offsets, chunk_offsets, sizeof(chunk_offsets));
  }
  if (kind == HERMES_KIND_SUBMIT_REQUEST || kind == HERMES_KIND_SAVE_NOTE) {
    s_visible_status = HERMES_STATUS_WAITING_PHONE;
  } else if (kind == HERMES_KIND_STOP_REQUEST) {
    s_visible_status = HERMES_STATUS_STOPPING;
  }
  outbound_send_current_chunk();
}

static bool outbound_write_dictionary(DictionaryIterator *iter) {
  uint8_t index = s_outbound.next_chunk;
  uint16_t offset = s_outbound.chunk_offsets[index];
  uint8_t length = s_outbound.chunk_lengths[index];
  if (dict_write_uint8(iter, HERMES_KEY_PROTOCOL_VERSION, (uint8_t)HERMES_PROTOCOL_VERSION) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_MESSAGE_KIND, s_outbound.kind) != DICT_OK) return false;
  if (dict_write_uint32(iter, HERMES_KEY_TRANSFER_ID, s_outbound.transfer_id) != DICT_OK) return false;
  if (dict_write_uint32(iter, HERMES_KEY_CAPTURE_ID, s_outbound.capture_id) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_CHUNK_INDEX, index) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_CHUNK_COUNT, s_outbound.chunk_count) != DICT_OK) return false;
  if (dict_write_data(iter, HERMES_KEY_PAYLOAD, s_outbound.payload + offset, length) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_STATUS, s_outbound.status) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_ERROR_CODE, s_outbound.error_code) != DICT_OK) return false;
  if (dict_write_uint32(iter, HERMES_KEY_ITEM_ID, s_outbound.item_id) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_ITEM_STATE, s_outbound.item_state) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_ITEM_KIND, s_outbound.item_kind) != DICT_OK) return false;
  if (dict_write_uint32(iter, HERMES_KEY_PAGE_OFFSET, s_outbound.page_offset) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_PAGE_COUNT, s_outbound.page_count) != DICT_OK) return false;
  if (dict_write_uint32(iter, HERMES_KEY_TOTAL_BYTES, s_outbound.total_bytes) != DICT_OK) return false;
  if (dict_write_uint32(iter, HERMES_KEY_CONVERSATION_GENERATION, s_outbound.generation) != DICT_OK) return false;
  if (dict_write_uint8(iter, HERMES_KEY_FLAGS, s_outbound.flags) != DICT_OK) return false;
  if (dict_write_uint32(iter, HERMES_KEY_CORRELATION_ID, s_outbound.correlation_id) != DICT_OK) return false;
  return true;
}

static void outbound_send_current_chunk(void) {
  DictionaryIterator *iter = NULL;
  AppMessageResult result;
  if (!s_outbound.active || s_outbound.phase != HERMES_OUT_SENDING || s_outbound.next_chunk >= s_outbound.chunk_count) {
    return;
  }
  result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK || iter == NULL) {
    outbound_schedule_retry();
    return;
  }
  if (!outbound_write_dictionary(iter)) {
    outbound_failed(HERMES_ERROR_TRANSFER_BOUND, "Could not encode the watch message.");
    return;
  }
  if (app_message_outbox_send() != APP_MSG_OK) {
    outbound_schedule_retry();
    return;
  }
  outbound_wait(HERMES_OUT_WAIT_CHUNK, HERMES_CHUNK_TIMEOUT_SECONDS);
  timer_update();
}

static void outbound_schedule_retry(void) {
  uint8_t delay;
  if (!s_outbound.active) {
    return;
  }
  if (s_outbound.retry_count >= HERMES_MAX_RETRY_COUNT) {
    outbound_failed(HERMES_ERROR_WATCH_UNAVAILABLE, "No phone reply. Open Android Setup and select your Pebble host. Then choose Reconnect here. Drafts stay saved.");
    return;
  }
  s_outbound.retry_count++;
  switch (s_outbound.retry_count) {
    case 1u:
      delay = HERMES_RETRY_DELAY_1_SECONDS;
      break;
    case 2u:
      delay = HERMES_RETRY_DELAY_2_SECONDS;
      break;
    case 3u:
      delay = HERMES_RETRY_DELAY_4_SECONDS;
      break;
    default:
      delay = HERMES_RETRY_DELAY_8_SECONDS;
      break;
  }
  outbound_wait(HERMES_OUT_RETRY_WAIT, delay);
  timer_update();
}

static void outbound_timeout(void) {
  if (!s_outbound.active) {
    return;
  }
  s_outbound.next_chunk = 0u;
  outbound_schedule_retry();
}

static void outbound_finish(void) {
  s_outbound.active = false;
  s_outbound.phase = HERMES_OUT_IDLE;
  timer_update();
}

static void outbound_failed(uint32_t error_code, const char *text) {
  if (s_outbound.kind == HERMES_KIND_AUDIO_STATUS) {
    outbound_finish(); // A lost diagnostic reply must not replace the conversation UI.
    return;
  }
  if (s_outbound.kind == HERMES_KIND_INK_BLOCK) {
    outbound_finish();
    ink_failed();
    return;
  }
  if (s_screen == HERMES_SCREEN_INK || s_screen == HERMES_SCREEN_INK_ACTIONS) {
    outbound_finish(); // A failed background handshake must not close the drawing.
    return;
  }
  if (s_outbound.kind == HERMES_KIND_FETCH_RESULT && s_visible_output[0] != '\0' &&
      (s_screen == HERMES_SCREEN_RESULT ||
       (s_screen == HERMES_SCREEN_ACTIONS && s_previous_screen == HERMES_SCREEN_RESULT))) {
    outbound_finish();
    s_result_loading = false;
    s_result_prefetch = false;
    s_result_retry = true;
    if (s_screen == HERMES_SCREEN_RESULT) ui_show_result();
    timer_update();
    return;
  }
  bool keep_pending = s_outbound.kind == HERMES_KIND_SUBMIT_REQUEST || s_outbound.kind == HERMES_KIND_SAVE_NOTE || s_outbound.kind == HERMES_KIND_DISCARD_CAPTURE;
  uint32_t capture_id = s_outbound.capture_id;
  s_outbound.active = false;
  s_outbound.phase = HERMES_OUT_IDLE;
  s_result_loading = false;
  s_auto_result_capture_id = 0u;
  if (text != NULL) {
    snprintf(s_error_text, sizeof(s_error_text), "%s", text);
  }
  if (keep_pending && (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt)) {
    s_visible_capture_id = capture_id;
    s_visible_status = HERMES_STATUS_OUTCOME_UNKNOWN;
    s_visible_error = error_code;
    s_screen = HERMES_SCREEN_RECOVERY;
  } else {
    s_visible_error = error_code;
    s_screen = HERMES_SCREEN_ERROR;
  }
  timer_update();
  if (!keep_pending) snprintf(s_error_text, sizeof(s_error_text), "%s", text);
  ui_rebuild();
}

static void outbound_message_sent(DictionaryIterator *iterator, void *context) {
  if (!s_outbound.active) {
    return;
  }
  (void)context;
  uint32_t transfer_id = 0;
  if (!read_u32(iterator, HERMES_KEY_TRANSFER_ID, &transfer_id, true) || transfer_id != s_outbound.transfer_id) return;
  if (s_outbound.kind == HERMES_KIND_HANDSHAKE && s_phone_probe_id != 0u) {
    outbound_finish();
    timer_update();
    return;
  }
  if (s_outbound.next_chunk + 1u < s_outbound.chunk_count) {
    s_outbound.next_chunk++;
    s_outbound.phase = HERMES_OUT_SENDING;
    outbound_send_current_chunk();
    return;
  }
  if (s_outbound.kind == HERMES_KIND_AUDIO_STATUS) {
    outbound_finish();
    return;
  }
  if (s_outbound.kind == HERMES_KIND_SUBMIT_REQUEST || s_outbound.kind == HERMES_KIND_SAVE_NOTE) {
    outbound_wait(HERMES_OUT_WAIT_RECEIPT, HERMES_DURABLE_RECEIPT_TIMEOUT_SECONDS);
  } else {
    outbound_wait(HERMES_OUT_WAIT_REPLY, HERMES_CHUNK_TIMEOUT_SECONDS);
  }
  timer_update();
}

static void outbound_message_failed(DictionaryIterator *iterator, AppMessageResult result, void *context) {
  (void)context; (void)result;
  uint32_t transfer_id = 0;
  if (!read_u32(iterator, HERMES_KEY_TRANSFER_ID, &transfer_id, true) || transfer_id != s_outbound.transfer_id) return;
  if (s_outbound.active) {
    if (s_outbound.kind == HERMES_KIND_HANDSHAKE && s_phone_probe_id != 0u) {
      outbound_finish();
      timer_update();
    } else {
      outbound_schedule_retry();
    }
  }
}

static void start_dictation(uint8_t mode) {
  DictationSessionStatus start_status;
  if (s_dictation_session != NULL || s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
    if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
      s_screen = HERMES_SCREEN_RECOVERY;
      ui_rebuild();
    }
    return;
  }
  cancel_auto_result();
  s_voice_capture_id = 0u;
  s_voice_status[0] = '\0';
  if (s_audio.phase == 1u || s_audio.phase == 2u) audio_terminal(HERMES_AUDIO_CANCELLED);
  s_dictation_mode = mode == HERMES_CAPTURE_NOTE ? HERMES_CAPTURE_NOTE : HERMES_CAPTURE_REQUEST;
  s_capture_mode = s_dictation_mode;
  s_capture_text[0] = '\0';
  s_dictation_session = dictation_session_create(0, dictation_callback, NULL);
  if (s_dictation_session == NULL) {
    ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "Dictation is unavailable on this watch or phone.");
    return;
  }
  dictation_session_enable_confirmation(s_dictation_session, false);
  start_status = dictation_session_start(s_dictation_session);
  if (start_status != DictationSessionStatusSuccess) {
    {
      DictationSession *failed_session = s_dictation_session;
      s_dictation_session = NULL;
      dictation_session_destroy(failed_session);
    }
    ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, dictation_failure_text((int)start_status));
    return;
  }
  s_screen = HERMES_SCREEN_DICTATION;
  ui_rebuild();
}

static void stop_dictation(void) {
  if (s_dictation_session != NULL) {
    DictationSession *session = s_dictation_session;
    s_dictation_session = NULL;
    dictation_session_destroy(session);
  }
}

static void dictation_callback(DictationSession *session, DictationSessionStatus status, char *transcript, void *context) {
  size_t length = 0u;
  bool accepted = false;
  (void)context;
  if (session != s_dictation_session) {
    if (session != NULL) {
      dictation_session_destroy(session);
    }
    return;
  }
  s_dictation_session = NULL;
  if (session != NULL) {
    if (status == DictationSessionStatusSuccess && transcript != NULL) {
      while (length <= HERMES_MAX_DICTATION_BYTES && transcript[length] != '\0') {
        length++;
      }
      if (length > 0u && length <= HERMES_MAX_DICTATION_BYTES && utf8_valid((const uint8_t *)transcript, (uint16_t)length)) {
        memcpy(s_capture_text, transcript, length);
        s_capture_text[length] = '\0';
        accepted = true;
      }
    }
    dictation_session_destroy(session);
  }
  if (accepted) {
    s_capture_mode = s_dictation_mode;
    s_screen = HERMES_SCREEN_REVIEW;
    ui_rebuild();
  } else {
    ui_show_error(HERMES_ERROR_MALFORMED, dictation_failure_text((int)status));
  }
}

static void start_handshake(void) {
  if (!s_app_message_open) {
    ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "Watch messaging is unavailable. Reopen Hermes.");
    return;
  }
  if (s_outbound.active) {
    if (s_outbound.kind != HERMES_KIND_HANDSHAKE) {
      ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "A transfer is in progress. Back returns to the menu and keeps your saved draft.");
      return;
    }
    if (s_outbound.phase == HERMES_OUT_WAIT_CHUNK) {
      s_screen = HERMES_SCREEN_CONNECTING;
      ui_rebuild();
      return;
    }
    outbound_finish();
  }
  s_handshake_ready = false;
  s_screen = HERMES_SCREEN_CONNECTING;
  ui_rebuild();
  outbound_start(HERMES_KIND_HANDSHAKE, HERMES_KIND_HANDSHAKE_ACK, next_transfer_id(), 0u, s_generation, 0u, 0u, 0u, HERMES_STATUS_NONE, HERMES_ERROR_NONE, HERMES_ITEM_KIND_NONE, HERMES_STATUS_NONE, 0u, 0u, NULL, 0u);
}

static void prepare_capture(uint8_t operation) {
  static PendingCapture candidate;
  uint32_t capture_id;
  uint8_t kind;
  uint8_t expected_kind;
  uint16_t length;
  if (operation != HERMES_PENDING_REQUEST && operation != HERMES_PENDING_NOTE) {
    return;
  }
  if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
    s_screen = HERMES_SCREEN_RECOVERY;
    ui_rebuild();
    return;
  }
  length = (uint16_t)strlen(s_capture_text);
  if (length == 0u || length > HERMES_MAX_DICTATION_BYTES || !utf8_valid((const uint8_t *)s_capture_text, length)) {
    ui_show_error(HERMES_ERROR_MALFORMED, "The transcript is empty or invalid. Dictate again.");
    return;
  }
  if (!storage_next_capture_id(&capture_id)) {
    ui_show_error(operation == HERMES_PENDING_NOTE ? HERMES_ERROR_NOTE_STORAGE : HERMES_ERROR_DURABLE_STORAGE, "The watch capture counter could not be persisted.");
    return;
  }
  memset(&candidate, 0, sizeof(candidate));
  candidate.operation = operation;
  candidate.capture_id = capture_id;
  candidate.transfer_id = next_transfer_id();
  candidate.generation = s_generation;
  candidate.length = length;
  memcpy(candidate.text, s_capture_text, length);
  candidate.text[length] = '\0';
  if (!storage_save_pending(&candidate)) {
    ui_show_error(operation == HERMES_PENDING_NOTE ? HERMES_ERROR_NOTE_STORAGE : HERMES_ERROR_DURABLE_STORAGE, "The pending capture could not be committed on the watch.");
    return;
  }
  s_voice_capture_id = 0u;
  s_voice_status[0] = '\0';
  s_pending = candidate;
  s_discard_capture_id = capture_id;
  cancel_auto_result();
  s_follow_capture_id = operation == HERMES_PENDING_REQUEST ? capture_id : 0u;
  if (operation == HERMES_PENDING_REQUEST) s_reply_notify_capture_id = capture_id;
  s_visible_generation = s_generation;
  s_visible_capture_id = capture_id;
  s_visible_item_id = 0u;
  s_visible_status = HERMES_STATUS_WAITING_PHONE;
  s_visible_error = HERMES_ERROR_NONE;
  s_visible_flags = 0u;
  s_visible_item_kind = operation == HERMES_PENDING_NOTE ? HERMES_ITEM_KIND_NOTE : HERMES_ITEM_KIND_REQUEST;
  s_result_more = 0u;
  s_visible_output[0] = '\0';
  s_result_height_valid = false;
  memcpy(s_visible_input, s_capture_text, length + 1u);
  if (operation == HERMES_PENDING_REQUEST) {
    kind = HERMES_KIND_SUBMIT_REQUEST;
    expected_kind = HERMES_KIND_DURABLE_RECEIPT;
  } else {
    kind = HERMES_KIND_SAVE_NOTE;
    expected_kind = HERMES_KIND_DURABLE_RECEIPT;
  }
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
  outbound_start(kind, expected_kind, candidate.transfer_id, candidate.capture_id, candidate.generation, 0u, 0u, 0u, HERMES_STATUS_WAITING_PHONE, HERMES_ERROR_NONE, s_visible_item_kind, HERMES_STATUS_WAITING_PHONE, 0u, 0u, (const uint8_t *)candidate.text, candidate.length);
  if (!s_outbound.active) {
    ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "The pending capture is retained, but the transfer could not be started.");
  }
}

static void resume_pending_capture(void) {
  uint8_t kind;
  if (s_storage_corrupt || s_pending.operation == HERMES_PENDING_NONE || s_outbound.active) {
    if (s_outbound.active) {
      ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "Another transfer is already in progress.");
    }
    return;
  }
  if (s_pending.operation == HERMES_PENDING_REQUEST) {
    kind = HERMES_KIND_SUBMIT_REQUEST;
  } else {
    kind = HERMES_KIND_SAVE_NOTE;
  }
  cancel_auto_result();
  s_follow_capture_id = s_pending.operation == HERMES_PENDING_REQUEST ? s_pending.capture_id : 0u;
  if (s_pending.operation == HERMES_PENDING_REQUEST) s_reply_notify_capture_id = s_pending.capture_id;
  s_visible_generation = s_pending.generation;
  s_visible_capture_id = s_pending.capture_id;
  s_visible_item_kind = s_pending.operation == HERMES_PENDING_NOTE ? HERMES_ITEM_KIND_NOTE : HERMES_ITEM_KIND_REQUEST;
  s_visible_status = HERMES_STATUS_WAITING_PHONE;
  s_visible_error = HERMES_ERROR_NONE;
  memcpy(s_visible_input, s_pending.text, s_pending.length + 1u);
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
  outbound_start(kind, HERMES_KIND_DURABLE_RECEIPT, s_pending.transfer_id, s_pending.capture_id, s_pending.generation, 0u, 0u, 0u, HERMES_STATUS_WAITING_PHONE, HERMES_ERROR_NONE, s_visible_item_kind, HERMES_STATUS_WAITING_PHONE, 0u, 0u, (const uint8_t *)s_pending.text, s_pending.length);
  if (!s_outbound.active) {
    ui_show_error(s_pending.operation == HERMES_PENDING_NOTE ? HERMES_ERROR_NOTE_STORAGE : HERMES_ERROR_DURABLE_STORAGE, "The pending capture is retained and can be resumed later.");
  }
}

static void start_fetch_recent(void) {
  cancel_auto_result();
  if (!s_app_message_open || s_outbound.active) {
    if (!s_app_message_open) {
      ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "The Pebble phone host is unavailable.");
    }
    return;
  }
  s_screen = HERMES_SCREEN_CONNECTING;
  ui_rebuild();
  outbound_start(HERMES_KIND_FETCH_RECENT, HERMES_KIND_RECENT_PAGE, next_transfer_id(), 0u, s_generation, 0u, 0u, 0u, HERMES_STATUS_NONE, HERMES_ERROR_NONE, HERMES_ITEM_KIND_NONE, HERMES_STATUS_NONE, HERMES_MAX_RECENT_ITEMS, 0u, NULL, 0u);
}

static void result_reset(void) {
  while (s_result_previous_windows != NULL) {
    ResultWindow *previous = s_result_previous_windows->previous;
    free(s_result_previous_windows);
    s_result_previous_windows = previous;
  }
  s_visible_output[0] = '\0';
  s_result_height_valid = false;
  s_result_window_offset = 0u;
  s_result_window_end = 0u;
  s_result_next_offset = 0u;
  s_result_total_bytes = 0u;
  s_result_more = 0u;
  s_result_prefetch = false;
  s_result_retry = false;
  s_result_scroll_to_end = false;
  s_result_window_full = false;
  s_result_scroll_offset = s_scroll_offset = 0u;
}

// Keep a bounded text window in RAM. At its edges, the same Up/Down buttons
// load adjacent text; history stores only byte offsets, never another answer.
static bool result_move_window(bool forward) {
  if (s_outbound.active || s_inbound.active || !s_app_message_open) return false;
  uint32_t offset;
  uint32_t end = 0u;
  if (forward) {
    if (!s_result_more || s_result_next_offset <= s_result_window_offset) return false;
    ResultWindow *previous = malloc(sizeof(ResultWindow));
    if (previous == NULL) {
      ui_show_error(HERMES_ERROR_TRANSFER_BOUND, "Could not load more text. The full answer is saved on your phone.");
      return true;
    }
    previous->offset = s_result_window_offset;
    previous->previous = s_result_previous_windows;
    s_result_previous_windows = previous;
    offset = s_result_next_offset;
  } else {
    if (s_result_previous_windows == NULL) return false;
    end = s_result_window_offset;
    ResultWindow *previous = s_result_previous_windows;
    offset = previous->offset;
    s_result_previous_windows = previous->previous;
    free(previous);
  }
  s_visible_output[0] = '\0';
  s_result_height_valid = false;
  s_result_window_offset = s_result_next_offset = offset;
  s_result_window_end = end;
  s_result_more = 0u;
  s_result_prefetch = false;
  s_result_retry = false;
  s_result_scroll_to_end = !forward;
  s_result_window_full = false;
  s_result_scroll_offset = s_scroll_offset = 0u;
  s_screen = HERMES_SCREEN_STATUS;
  start_result_chunk(s_visible_capture_id, offset);
  return true;
}

static void start_result_chunk(uint32_t capture_id, uint32_t offset) {
  if (!s_app_message_open || s_outbound.active || capture_id == 0u) {
    if (!s_app_message_open) {
      ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "The Pebble phone host is unavailable.");
    }
    return;
  }
  s_result_offset = offset;
  s_result_loading = true;
  if (s_screen == HERMES_SCREEN_STATUS) ui_rebuild();
  outbound_start(HERMES_KIND_FETCH_RESULT, HERMES_KIND_RESULT_PAGE, next_transfer_id(), capture_id, s_generation, 0u, offset, 0u, HERMES_STATUS_NONE, HERMES_ERROR_NONE, s_visible_item_kind, s_visible_status, 0u,
      offset == 0u && capture_id == s_voice_capture_id && !s_voice_requested &&
      s_visible_status == HERMES_STATUS_COMPLETED ? HERMES_FLAG_VOICE_REPLY : 0u, NULL, 0u);
  if (s_outbound.active && (s_outbound.flags & HERMES_FLAG_VOICE_REPLY)) s_voice_requested = true;
}

static void start_fetch_result(uint32_t capture_id, uint32_t offset) {
  if (!s_app_message_open || s_outbound.active || capture_id == 0u) return;
  result_reset();
  s_result_window_offset = s_result_next_offset = offset;
  s_screen = HERMES_SCREEN_STATUS;
  start_result_chunk(capture_id, offset);
}

// Reject gaps, empty nonterminal chunks, and inconsistent lengths before
// mutating the visible text. A full window leaves the next byte unread.
static bool result_append_page(uint32_t offset, uint32_t total, bool more) {
  size_t page_length = strlen(s_result_page);
  size_t used = strlen(s_visible_output);
  if (offset != s_result_next_offset || offset > total || page_length > total - offset ||
      (more && page_length == 0u) || more != (offset + page_length < total) ||
      (s_result_window_end != 0u && offset + page_length > s_result_window_end)) return false;
  s_result_total_bytes = total;
  s_result_more = more;
  s_result_prefetch = false;
  if (page_length > UI_RESULT_TEXT_SIZE - used) {
    s_result_more = 1u;
    s_result_window_full = true;
    return true;
  }
  memcpy(s_visible_output + used, s_result_page, page_length + 1u);
  s_result_height_valid = false;
  s_result_next_offset = offset + (uint32_t)page_length;
  s_result_prefetch = more && s_result_next_offset != s_result_window_end;
  return true;
}

static void start_new_conversation(void) {
  if (!s_app_message_open || s_outbound.active || s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
    if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
      s_screen = HERMES_SCREEN_RECOVERY;
      ui_rebuild();
    }
    return;
  }
  s_screen = HERMES_SCREEN_CONNECTING;
  ui_rebuild();
  outbound_start(HERMES_KIND_START_CONVERSATION, HERMES_KIND_NEW_CONVERSATION_ACK, next_transfer_id(), 0u, s_generation, 0u, 0u, 0u, HERMES_STATUS_NONE, HERMES_ERROR_NONE, HERMES_ITEM_KIND_NONE, HERMES_STATUS_NONE, 0u, 0u, NULL, 0u);
}

static void start_stop_request(uint32_t capture_id) {
  if (!s_app_message_open || s_outbound.active || capture_id == 0u) {
    if (!s_app_message_open) {
      ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "The Pebble phone host is unavailable.");
    }
    return;
  }
  s_visible_capture_id = capture_id;
  s_visible_status = HERMES_STATUS_STOPPING;
  s_visible_error = HERMES_ERROR_NONE;
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
  outbound_start(HERMES_KIND_STOP_REQUEST, HERMES_KIND_STATUS_UPDATE, next_transfer_id(), capture_id, s_generation, s_visible_item_id, 0u, 0u, HERMES_STATUS_STOPPING, HERMES_ERROR_NONE, s_visible_item_kind, HERMES_STATUS_STOPPING, 0u, HERMES_FLAG_STOP_REQUESTED, NULL, 0u);
}

static void start_discard_capture(uint32_t capture_id) {
  if (!s_app_message_open || s_outbound.active || capture_id == 0u) {
    if (!s_app_message_open) {
      ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "The Pebble phone host is unavailable.");
    }
    return;
  }
  s_discard_capture_id = capture_id;
  s_discard_transfer_id = next_transfer_id();
  s_discard_in_progress = true;
  s_visible_capture_id = capture_id;
  s_visible_status = HERMES_STATUS_WAITING_PHONE;
  s_visible_error = HERMES_ERROR_NONE;
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
  outbound_start(HERMES_KIND_DISCARD_CAPTURE, HERMES_KIND_CAPTURE_DISCARDED, s_discard_transfer_id, capture_id, s_generation, 0u, 0u, 0u, HERMES_STATUS_DISCARDED, HERMES_ERROR_NONE, HERMES_ITEM_KIND_NONE, HERMES_STATUS_DISCARDED, 0u, 0u, NULL, 0u);
}

static bool read_u8(DictionaryIterator *iter, uint8_t key, uint8_t *value, bool required) {
  Tuple *tuple = dict_find(iter, key);
  if (tuple == NULL) {
    return !required;
  }
  if (tuple->type == TUPLE_UINT && tuple->length == 1) {
    *value = tuple->value->uint8;
    return true;
  }
  if (tuple->type == TUPLE_UINT && tuple->length == 2) {
    if (tuple->value->uint16 > UINT8_MAX) {
      return false;
    }
    *value = (uint8_t)tuple->value->uint16;
    return true;
  }
  if (tuple->type == TUPLE_UINT && tuple->length == 4) {
    if (tuple->value->uint32 > UINT8_MAX) {
      return false;
    }
    *value = (uint8_t)tuple->value->uint32;
    return true;
  }
  if (tuple->type == TUPLE_INT && tuple->length == 4) {
    if (tuple->value->int32 < 0 || tuple->value->int32 > UINT8_MAX) {
      return false;
    }
    *value = (uint8_t)tuple->value->int32;
    return true;
  }
  return false;
}

static bool read_u32(DictionaryIterator *iter, uint8_t key, uint32_t *value, bool required) {
  Tuple *tuple = dict_find(iter, key);
  if (tuple == NULL) {
    return !required;
  }
  if (tuple->type == TUPLE_UINT && tuple->length == 4) {
    *value = tuple->value->uint32;
    return true;
  }
  if (tuple->type == TUPLE_UINT && tuple->length == 2) {
    *value = tuple->value->uint16;
    return true;
  }
  if (tuple->type == TUPLE_UINT && tuple->length == 1) {
    *value = tuple->value->uint8;
    return true;
  }
  if (tuple->type == TUPLE_INT && tuple->length == 4) {
    if (tuple->value->int32 < 0) {
      return false;
    }
    *value = (uint32_t)tuple->value->int32;
    return true;
  }
  return false;
}

static bool read_bytes(DictionaryIterator *iter, uint8_t key, const uint8_t **value, uint16_t capacity, uint16_t *length, bool required) {
  Tuple *tuple = dict_find(iter, key);
  *length = 0u;
  *value = (const uint8_t *)"";
  if (tuple == NULL) {
    return !required;
  }
  if (tuple->type != TUPLE_BYTE_ARRAY || tuple->length > capacity) {
    return false;
  }
  *value = tuple->value->data;
  *length = (uint16_t)tuple->length;
  return true;
}

static bool common_fields_equal(const InboundTransfer *message, uint8_t protocol_version, uint8_t kind, uint32_t transfer_id, uint32_t capture_id, uint8_t chunk_count) {
  return message->protocol_version == protocol_version && message->kind == kind && message->transfer_id == transfer_id && message->capture_id == capture_id && message->chunk_count == chunk_count;
}

static void inbound_reset(void) {
  memset(&s_inbound, 0, sizeof(s_inbound));
}

static void inbound_error(uint32_t code, const char *text) {
  inbound_reset();
  ui_show_error(code, text);
}

static void inbox_dropped(AppMessageResult result, void *context) {
  (void)context;
  if (result != APP_MSG_OK) {
    if (s_outbound.active) {
      outbound_schedule_retry();
    }
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  uint8_t protocol_version;
  uint8_t kind;
  uint8_t chunk_index;
  uint8_t chunk_count;
  uint32_t transfer_id;
  uint32_t capture_id;
  uint32_t correlation_id;
  uint32_t item_id;
  uint32_t page_offset;
  uint32_t total_bytes;
  uint32_t generation;
  uint8_t status;
  uint8_t error_code;
  uint8_t item_state;
  uint8_t item_kind;
  uint8_t page_count;
  uint8_t flags;
  const uint8_t *payload; // Borrow the tuple until it is copied into the bounded assembler.
  uint16_t payload_length;
  bool first_chunk;
  bool duplicate;
  capture_id = 0u;
  correlation_id = 0u;
  item_id = 0u;
  page_offset = 0u;
  total_bytes = 0u;
  generation = 0u;
  status = HERMES_STATUS_NONE;
  error_code = HERMES_ERROR_NONE;
  item_state = HERMES_STATUS_NONE;
  item_kind = HERMES_ITEM_KIND_NONE;
  page_count = 0u;
  flags = 0u;
  (void)context;
  if (!read_u8(iter, HERMES_KEY_PROTOCOL_VERSION, &protocol_version, true) || !read_u8(iter, HERMES_KEY_MESSAGE_KIND, &kind, true) || !read_u32(iter, HERMES_KEY_TRANSFER_ID, &transfer_id, true) || !read_u32(iter, HERMES_KEY_CAPTURE_ID, &capture_id, false) || !read_u8(iter, HERMES_KEY_CHUNK_INDEX, &chunk_index, true) || !read_u8(iter, HERMES_KEY_CHUNK_COUNT, &chunk_count, true)) {
    inbound_error(HERMES_ERROR_MALFORMED, "Missing or invalid required phone fields.");
    return;
  }
  if (protocol_version != HERMES_PROTOCOL_VERSION) {
    inbound_error(HERMES_ERROR_PROTOCOL_VERSION, "The phone and watch use incompatible protocol versions. Update both apps.");
    return;
  }
  if (kind < HERMES_KIND_HANDSHAKE_ACK || kind > HERMES_KIND_VOICE_STATUS) {
    inbound_error(HERMES_ERROR_UNSUPPORTED_KIND, "The phone sent an unsupported message kind.");
    return;
  }
  if (transfer_id == 0u || chunk_count == 0u || chunk_count > HERMES_MAX_CHUNKS || chunk_index >= chunk_count) {
    inbound_error(HERMES_ERROR_TRANSFER_BOUND, "Invalid phone transfer ID or chunk range.");
    return;
  }
  if (!read_u32(iter, HERMES_KEY_CORRELATION_ID, &correlation_id, false) || !read_u32(iter, HERMES_KEY_ITEM_ID, &item_id, false) || !read_u8(iter, HERMES_KEY_STATUS, &status, false) || !read_u8(iter, HERMES_KEY_ERROR_CODE, &error_code, false) || !read_u8(iter, HERMES_KEY_ITEM_STATE, &item_state, false) || !read_u8(iter, HERMES_KEY_ITEM_KIND, &item_kind, false) || !read_u32(iter, HERMES_KEY_PAGE_OFFSET, &page_offset, false) || !read_u8(iter, HERMES_KEY_PAGE_COUNT, &page_count, false) || !read_u32(iter, HERMES_KEY_TOTAL_BYTES, &total_bytes, false) || !read_u32(iter, HERMES_KEY_CONVERSATION_GENERATION, &generation, false) || !read_u8(iter, HERMES_KEY_FLAGS, &flags, false) || !read_bytes(iter, HERMES_KEY_PAYLOAD, &payload, HERMES_AUDIO_CHUNK_PAYLOAD_SIZE, &payload_length, false)) {
    inbound_error(HERMES_ERROR_MALFORMED, "Invalid phone field type or chunk size.");
    return;
  }
  if (payload_length > (kind == HERMES_KIND_AUDIO_BLOCK ? HERMES_AUDIO_CHUNK_PAYLOAD_SIZE : HERMES_CHUNK_PAYLOAD_SIZE)) {
    inbound_error(HERMES_ERROR_TRANSFER_BOUND, "The phone sent a chunk outside its payload limit.");
    return;
  }
  if (!s_inbound.active && s_inbound.transfer_id == transfer_id) {
    if (s_inbound.chunk_count == chunk_count && s_inbound.chunk_lengths[chunk_index] == payload_length &&
        memcmp(s_inbound.chunks + s_inbound.chunk_offsets[chunk_index], payload, payload_length) == 0) return;
    inbound_error(HERMES_ERROR_DUPLICATE_CHUNK, "Completed transfer was repeated with different bytes.");
    return;
  }
  if (s_inbound.active && s_inbound.transfer_id != transfer_id) {
    inbound_error(HERMES_ERROR_MALFORMED, "A second phone transfer arrived before the current transfer completed.");
    return;
  }
  first_chunk = !s_inbound.active;
  if (first_chunk) {
    memset(&s_inbound, 0, sizeof(s_inbound));
    s_inbound.active = true;
    s_inbound_deadline_ms = deadline_after(HERMES_CHUNK_TIMEOUT_SECONDS);
    timer_update();
    s_inbound.protocol_version = protocol_version;
    s_inbound.kind = kind;
    s_inbound.transfer_id = transfer_id;
    s_inbound.capture_id = capture_id;
    s_inbound.chunk_count = chunk_count;
    s_inbound.correlation_id = correlation_id;
    s_inbound.item_id = item_id;
    s_inbound.status = status;
    s_inbound.error_code = error_code;
    s_inbound.item_state = item_state;
    s_inbound.item_kind = item_kind;
    s_inbound.page_offset = page_offset;
    s_inbound.page_count = page_count;
    s_inbound.total_bytes = total_bytes;
    s_inbound.generation = generation;
    s_inbound.flags = flags;
  } else if (!common_fields_equal(&s_inbound, protocol_version, kind, transfer_id, capture_id, chunk_count) || s_inbound.correlation_id != correlation_id || s_inbound.item_id != item_id || s_inbound.status != status || s_inbound.error_code != error_code || s_inbound.item_state != item_state || s_inbound.item_kind != item_kind || s_inbound.page_offset != page_offset || s_inbound.page_count != page_count || s_inbound.total_bytes != total_bytes || s_inbound.generation != generation || s_inbound.flags != flags) {
    inbound_error(HERMES_ERROR_DUPLICATE_CHUNK, "Chunks for one transfer carried conflicting metadata.");
    return;
  }
  duplicate = (s_inbound.received_mask & (uint8_t)(1u << chunk_index)) != 0u;
  if (duplicate) {
    if (s_inbound.chunk_lengths[chunk_index] != payload_length || memcmp(s_inbound.chunks + s_inbound.chunk_offsets[chunk_index], payload, payload_length) != 0) {
      inbound_error(HERMES_ERROR_DUPLICATE_CHUNK, "A duplicate chunk did not match the original bytes.");
      return;
    }
    return;
  }
  if ((uint16_t)s_inbound.total_length + payload_length > HERMES_MAX_TRANSFER_BYTES) {
    inbound_error(HERMES_ERROR_TRANSFER_BOUND, "Phone transfer exceeds 1024 bytes.");
    return;
  }
  s_inbound.chunk_offsets[chunk_index] = s_inbound.total_length;
  memcpy(s_inbound.chunks + s_inbound.chunk_offsets[chunk_index], payload, payload_length);
  s_inbound.chunk_lengths[chunk_index] = payload_length;
  s_inbound_deadline_ms = deadline_after(HERMES_CHUNK_TIMEOUT_SECONDS);
  s_inbound.total_length = (uint16_t)(s_inbound.total_length + payload_length);
  s_inbound.received_mask = (uint8_t)(s_inbound.received_mask | (uint8_t)(1u << chunk_index));
  if (s_inbound.received_mask == (uint8_t)((1u << chunk_count) - 1u)) {
    process_inbound_transfer();
  }
}

static void process_inbound_transfer(void) {
  uint16_t offset = 0u;
  uint8_t i;
  if (!s_inbound.active) return;
  // App callbacks run serially. Reassemble in the existing global buffer: a
  // local payload plus a full InboundTransfer copy overflows the watch stack.
  for (i = 0u; i < s_inbound.chunk_count; i++) {
    if (offset > HERMES_MAX_TRANSFER_BYTES || s_inbound.chunk_lengths[i] > HERMES_MAX_TRANSFER_BYTES - offset) {
      inbound_error(HERMES_ERROR_TRANSFER_BOUND, "Reassembled phone transfer is too large.");
      return;
    }
    memcpy(s_inbound.payload + offset, s_inbound.chunks + s_inbound.chunk_offsets[i], s_inbound.chunk_lengths[i]);
    offset = (uint16_t)(offset + s_inbound.chunk_lengths[i]);
  }
  if (offset != s_inbound.total_length ||
      (s_inbound.kind != HERMES_KIND_AUDIO_BLOCK && !utf8_valid(s_inbound.payload, offset))) {
    inbound_error(HERMES_ERROR_MALFORMED, "Phone payload is not valid UTF-8.");
    return;
  }
  s_inbound.payload_length = offset;
  s_inbound.active = false;
  process_phone_message(&s_inbound);
}

static bool inbound_correlation_matches(const InboundTransfer *message) {
  if (message->kind == HERMES_KIND_STATUS_UPDATE) {
    if (message->capture_id != 0u && message->capture_id != s_visible_capture_id && message->capture_id != s_reply_notify_capture_id && message->capture_id != (s_pending.operation != HERMES_PENDING_NONE ? s_pending.capture_id : 0u)) {
      return false;
    }
    if (message->correlation_id != 0u && s_outbound.active && message->correlation_id != s_outbound.transfer_id) {
      return false;
    }
    return true;
  }
  if (message->kind == HERMES_KIND_DURABLE_RECEIPT) {
    if (s_pending.operation != HERMES_PENDING_NONE && message->capture_id == s_pending.capture_id) {
      return message->correlation_id == s_pending.transfer_id;
    }
    return s_visible_capture_id == message->capture_id && s_outbound.active && message->correlation_id == s_outbound.transfer_id;
  }
  if (message->kind == HERMES_KIND_CAPTURE_DISCARDED) {
    if (message->capture_id != s_discard_capture_id) {
      return false;
    }
    return message->correlation_id == s_discard_transfer_id;
  }
  if (message->kind == HERMES_KIND_STRUCTURED_ERROR) {
    return message->correlation_id == 0u || (s_outbound.active && message->correlation_id == s_outbound.transfer_id);
  }
  if (!s_outbound.active || message->correlation_id != s_outbound.transfer_id) {
    return false;
  }
  return message->kind == s_outbound.expected_kind;
}

static bool queue_phone_probe(const InboundTransfer *message) {
  if (s_exiting || message->kind != HERMES_KIND_HANDSHAKE_ACK ||
      message->correlation_id != 0u || message->error_code != HERMES_ERROR_NONE ||
      message->transfer_id == 0u) return false;
  s_phone_probe_id = message->transfer_id;
  if (s_outbound.active && s_outbound.kind == HERMES_KIND_HANDSHAKE &&
      s_outbound.phase != HERMES_OUT_WAIT_CHUNK) {
    s_outbound.active = false;
    s_outbound.phase = HERMES_OUT_IDLE;
  }
  return true;
}

static void process_phone_message(const InboundTransfer *message) {
  if (s_exiting) return;
  if (message->kind == HERMES_KIND_VOICE_STATUS) {
    if (message->capture_id == s_voice_capture_id && s_voice_capture_id != 0u) {
      size_t length = message->payload_length < sizeof(s_voice_status) ? message->payload_length : sizeof(s_voice_status) - 1u;
      while (length > 0u && length < message->payload_length && is_utf8_continuation(message->payload[length])) length--;
      memcpy(s_voice_status, message->payload, length);
      s_voice_status[length] = '\0';
      if (s_screen == HERMES_SCREEN_RESULT || s_screen == HERMES_SCREEN_STATUS) ui_rebuild();
    }
    return;
  }
  if (message->kind >= HERMES_KIND_AUDIO_BEGIN && message->kind <= HERMES_KIND_AUDIO_CANCEL) {
    // ItemId binds every voice clip to the user's current opt-in. Back also
    // rejects clips still being synthesized or waiting between transfers.
    if (message->item_id != 0u && message->item_id != s_voice_capture_id) {
      audio_reply(message->capture_id, message->transfer_id, message->total_bytes,
          message->generation, 0u, HERMES_AUDIO_CANCELLED);
      return;
    }
    audio_handle(message->kind, message->capture_id, message->transfer_id, message->total_bytes,
      message->generation, message->flags, message->page_offset, message->payload, message->payload_length);
    return;
  }
  if (queue_phone_probe(message)) {
    timer_update();
    return;
  }
  if (!inbound_correlation_matches(message)) {
    return;
  }
  switch (message->kind) {
    case HERMES_KIND_INK_RECEIPT:
      ink_receipt(message);
      break;
    case HERMES_KIND_HANDSHAKE_ACK:
      if (message->error_code != HERMES_ERROR_NONE) {
        outbound_finish();
        ui_show_error(message->error_code, error_text(message->error_code));
        return;
      }
      if (!storage_write_generation(message->generation)) {
        outbound_finish();
        ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "The conversation generation could not be persisted.");
        return;
      }
      s_handshake_ready = true;
      ui_link_changed();
      outbound_finish();
      ink_schedule_sync();
      if (s_screen == HERMES_SCREEN_CONNECTING || s_screen == HERMES_SCREEN_MENU || s_screen == HERMES_SCREEN_ERROR) {
        s_screen = HERMES_SCREEN_MENU;
        ui_rebuild();
      }
      break;
    case HERMES_KIND_DURABLE_RECEIPT:
      process_durable_receipt(message);
      break;
    case HERMES_KIND_STATUS_UPDATE:
      process_status_update(message);
      break;
    case HERMES_KIND_RECENT_PAGE:
      process_recent_page(message);
      break;
    case HERMES_KIND_RESULT_PAGE:
      process_result_page(message);
      break;
    case HERMES_KIND_NEW_CONVERSATION_ACK:
      process_conversation_ack(message);
      break;
    case HERMES_KIND_STRUCTURED_ERROR:
      process_structured_error(message);
      break;
    case HERMES_KIND_CAPTURE_DISCARDED:
      if (message->capture_id == s_discard_capture_id) {
        outbound_finish();
        if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
          if (!storage_clear_pending()) {
            ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "The phone acknowledged discard, but watch storage could not be cleared.");
            return;
          }
        }
        s_discard_in_progress = false;
        s_discard_transfer_id = 0u;
        s_visible_status = HERMES_STATUS_DISCARDED;
        s_visible_error = HERMES_ERROR_NONE;
        s_capture_text[0] = '\0';
        s_screen = HERMES_SCREEN_MENU;
        ui_rebuild();
      }
      break;
    default:
      inbound_error(HERMES_ERROR_UNSUPPORTED_KIND, "The phone sent an unsupported response kind.");
      break;
  }
}

static void process_durable_receipt(const InboundTransfer *message) {
  uint8_t expected_kind;
  if (message->error_code != HERMES_ERROR_NONE) {
    outbound_finish();
    ui_show_error(message->error_code, error_text(message->error_code));
    return;
  }
  if (s_pending.operation != HERMES_PENDING_NONE) {
    if (message->capture_id != s_pending.capture_id) {
      return;
    }
  } else if (s_visible_capture_id != message->capture_id) {
    return;
  }
  expected_kind = s_pending.operation == HERMES_PENDING_NOTE ? HERMES_ITEM_KIND_NOTE : (s_pending.operation == HERMES_PENDING_NONE ? s_visible_item_kind : HERMES_ITEM_KIND_REQUEST);
  if (message->item_kind != HERMES_ITEM_KIND_NONE && message->item_kind != expected_kind) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "The phone receipt identified a different item kind.");
    return;
  }
  if (!(message->flags & HERMES_FLAG_DURABLE_COMMIT) ||
      (message->status != HERMES_STATUS_SAVED_QUEUED && message->status != HERMES_STATUS_NOTE_SAVED)) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "The phone receipt did not confirm durable storage.");
    return;
  }
  s_visible_capture_id = message->capture_id;
  s_visible_item_id = message->item_id;
  if (s_visible_status != HERMES_STATUS_COMPLETED) s_visible_status = message->status;
  s_visible_error = HERMES_ERROR_NONE;
  s_visible_flags = message->flags;
  s_visible_item_kind = expected_kind;
  if (s_pending.operation != HERMES_PENDING_NONE && message->capture_id == s_pending.capture_id) {
    if (!storage_clear_pending()) {
      outbound_finish();
      ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "The command was acknowledged, but the watch could not delete its pending copy.");
      return;
    }
  }
  outbound_finish();
  s_discard_in_progress = false;
  if (s_screen == HERMES_SCREEN_INK || s_screen == HERMES_SCREEN_INK_ACTIONS || s_screen == HERMES_SCREEN_INK_STATUS) return;
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
}

static void notify_reply_arrival(uint32_t capture_id, uint8_t status) {
  if (capture_id == 0u || capture_id != s_reply_notify_capture_id || status != HERMES_STATUS_COMPLETED) return;
  s_reply_notify_capture_id = 0u; // Consume even in Quiet Time; duplicates never buzz later.
  if (capture_id == s_reply_notified_capture_id) return;
  s_reply_notified_capture_id = capture_id;
  if (!quiet_time_is_active()) vibes_short_pulse();
}

static void process_status_update(const InboundTransfer *message) {
  // Completion can arrive while browsing elsewhere or before the result fetch.
  notify_reply_arrival(message->capture_id, message->status);
  if (message->capture_id == 0u) return;
  if (message->capture_id != s_visible_capture_id &&
      (s_pending.operation == HERMES_PENDING_NONE || message->capture_id != s_pending.capture_id)) return;
  // Do not let repeated/delayed statuses replace an answer or a new draft.
  bool viewing_status = s_screen == HERMES_SCREEN_STATUS;
  bool has_answer = s_screen == HERMES_SCREEN_RESULT ||
      (s_screen == HERMES_SCREEN_ACTIONS && s_previous_screen == HERMES_SCREEN_RESULT);
  s_visible_capture_id = message->capture_id;
  s_visible_item_id = message->item_id;
  if (!(s_visible_status == HERMES_STATUS_COMPLETED && message->status != HERMES_STATUS_COMPLETED)) {
    s_visible_status = message->status;
    s_visible_error = message->error_code;
  }
  s_visible_flags = message->flags;
  s_visible_item_kind = message->item_kind == HERMES_ITEM_KIND_NONE ? s_visible_item_kind : message->item_kind;
  if (s_outbound.active && s_outbound.kind == HERMES_KIND_STOP_REQUEST &&
      (message->correlation_id == 0u || message->correlation_id == s_outbound.transfer_id)) outbound_finish();
  if (!has_answer && should_auto_fetch_result()) {
    s_auto_result_capture_id = message->capture_id;
    timer_update();
  }
  if (viewing_status) ui_rebuild();
}

static void process_recent_page(const InboundTransfer *message) {
  size_t length = message->payload_length;
  if (message->error_code != HERMES_ERROR_NONE) {
    outbound_finish();
    ui_show_error(message->error_code, error_text(message->error_code));
    return;
  }
  if (length > UI_RESULT_BUFFER_SIZE) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The recent page was larger than the watch limit.");
    return;
  }
  outbound_finish();
  if (!parse_recent_payload((const char *)message->payload, (uint16_t)length)) {
    ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The phone recent page was not valid JSON.");
    return;
  }
  s_screen = HERMES_SCREEN_RECENT;
  ui_rebuild();
}

static void process_result_page(const InboundTransfer *message) {
  s_result_loading = false;
  size_t length = message->payload_length;
  if (message->error_code != HERMES_ERROR_NONE) {
    outbound_failed(message->error_code, error_text(message->error_code));
    return;
  }
  if (length > UI_RESULT_BUFFER_SIZE) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The result page was larger than the watch limit.");
    return;
  }
  if (message->capture_id != s_outbound.capture_id || message->page_offset != s_result_offset) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_NOT_FOUND, "The result page did not match the requested capture.");
    return;
  }
  s_visible_generation = message->generation;
  s_result_more = (message->flags & HERMES_FLAG_MORE) != 0u;
  size_t used = strlen(s_visible_output);
  if (!parse_result_payload((const char *)message->payload, (uint16_t)length) ||
      s_visible_capture_id != message->capture_id ||
      !result_append_page(message->page_offset, message->total_bytes, s_result_more != 0u)) {
    outbound_failed(HERMES_ERROR_INVALID_RESPONSE, "The phone result text was inconsistent. Refresh the answer.");
    return;
  }
  // Dense newline-heavy answers also need a bounded drawing height. Keep the
  // unread chunk on the phone and load it when Down reaches this window's end.
  // This single measurement is reused by the layout below.
  if (s_window != NULL && result_text_height() >= (int16_t)RESULT_WINDOW_HEIGHT && used != 0u) {
    s_visible_output[used] = '\0';
    s_result_height_valid = false;
    s_result_next_offset = message->page_offset;
    s_result_more = 1u;
    s_result_prefetch = false;
    s_result_window_full = true;
  }
  outbound_finish();
  notify_reply_arrival(message->capture_id, message->status);
  if (message->status != HERMES_STATUS_NONE) {
    s_visible_status = message->status;
  }
  if (message->item_id != 0u) {
    s_visible_item_id = message->item_id;
  }
  if (message->capture_id != 0u) {
    s_visible_capture_id = message->capture_id;
  }
  if (s_screen == HERMES_SCREEN_RESULT) {
    ui_result_refresh();
  } else if (s_screen == HERMES_SCREEN_STATUS) {
    ui_show_result();
  }
  if (!s_result_prefetch) s_result_scroll_to_end = false;
}

static void process_conversation_ack(const InboundTransfer *message) {
  uint32_t previous = message->page_offset;
  uint32_t next = message->generation;
  if (message->payload_length > 0u) {
    if (!json_uint_in_span((const char *)message->payload, message->payload_length, 0u, message->payload_length, "previous", &previous) || !json_uint_in_span((const char *)message->payload, message->payload_length, 0u, message->payload_length, "new", &next)) {
      if (message->page_offset == 0u || message->generation == 0u) {
        outbound_finish();
        ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The conversation acknowledgment was malformed.");
        return;
      }
      previous = message->page_offset;
      next = message->generation;
    }
  }
  if (s_generation == UINT32_MAX || (previous != 0u && previous != s_generation)) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_IDEMPOTENCY_CONFLICT, "The conversation generation was not current.");
    return;
  }
  if (next == 0u) {
    next = previous == 0u ? s_generation + 1u : previous + 1u;
  }
  if (next <= s_generation && next != s_generation + 1u) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_IDEMPOTENCY_CONFLICT, "The conversation generation was not newer.");
    return;
  }
  if (!storage_write_generation(next)) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_DURABLE_STORAGE, "The new conversation generation could not be persisted.");
    return;
  }
  outbound_finish();
  // The previous exchange belongs to the old conversation; do not show it as current.
  result_reset();
  s_visible_capture_id = 0u;
  s_visible_item_kind = HERMES_ITEM_KIND_NONE;
  s_visible_input[0] = '\0';
  s_visible_flags = 0u;
  s_visible_status = HERMES_STATUS_NONE;
  s_visible_error = HERMES_ERROR_NONE;
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
}

static void process_structured_error(const InboundTransfer *message) {
  if (s_outbound.active && s_outbound.kind == HERMES_KIND_INK_BLOCK) {
    outbound_finish();
    ink_failed();
    return;
  }
  s_result_loading = false;
  char detail[UI_ERROR_BUFFER_SIZE];
  uint32_t code = message->error_code;
  size_t length = message->payload_length;
  if (length >= sizeof(detail)) {
    length = sizeof(detail) - 1u;
  }
  memcpy(detail, message->payload, length);
  detail[length] = '\0';
  if (code == HERMES_ERROR_NONE) {
    code = HERMES_ERROR_MALFORMED;
  }
  if (s_outbound.active && s_outbound.kind == HERMES_KIND_FETCH_RESULT) {
    outbound_failed(code, detail[0] == '\0' ? error_text(code) : detail);
    return;
  }
  if (s_outbound.active) {
    outbound_finish();
  }
  if (detail[0] == '\0') {
    ui_show_error(code, error_text(code));
  } else {
    ui_show_error(code, detail);
  }
}

static void json_skip_space(const char *json, size_t length, size_t *position) {
  while (*position < length && (json[*position] == ' ' || json[*position] == '\n' || json[*position] == '\r' || json[*position] == '\t')) {
    (*position)++;
  }
}

static bool json_hex(char value, uint8_t *result) {
  if (value >= '0' && value <= '9') {
    *result = (uint8_t)(value - '0');
    return true;
  }
  if (value >= 'a' && value <= 'f') {
    *result = (uint8_t)(value - 'a' + 10);
    return true;
  }
  if (value >= 'A' && value <= 'F') {
    *result = (uint8_t)(value - 'A' + 10);
    return true;
  }
  return false;
}

static size_t json_encode_utf8(uint32_t codepoint, char *output, size_t capacity) {
  if (codepoint <= 0x7fu) {
    if (capacity < 1u) {
      return 0u;
    }
    output[0] = (char)codepoint;
    return 1u;
  }
  if (codepoint <= 0x7ffu) {
    if (capacity < 2u) {
      return 0u;
    }
    output[0] = (char)(0xc0u | (codepoint >> 6));
    output[1] = (char)(0x80u | (codepoint & 0x3fu));
    return 2u;
  }
  if (codepoint <= 0xffffu) {
    if (capacity < 3u) {
      return 0u;
    }
    output[0] = (char)(0xe0u | (codepoint >> 12));
    output[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
    output[2] = (char)(0x80u | (codepoint & 0x3fu));
    return 3u;
  }
  if (codepoint <= 0x10ffffu) {
    if (capacity < 4u) {
      return 0u;
    }
    output[0] = (char)(0xf0u | (codepoint >> 18));
    output[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
    output[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
    output[3] = (char)(0x80u | (codepoint & 0x3fu));
    return 4u;
  }
  return 0u;
}

static bool json_value_position(const char *json, size_t length, size_t start, size_t end, const char *key, size_t *position) {
  if (start >= end || end > length) return false;
  unsigned depth = 0u;
  for (size_t i = start; i < end; i++) {
    if (json[i] == '{' || json[i] == '[') { depth++; continue; }
    if (json[i] == '}' || json[i] == ']') { if (depth == 0u) return false; depth--; continue; }
    if (json[i] != '"') continue;
    size_t begin = ++i;
    bool escaped = false;
    for (; i < end; i++) {
      if (json[i] == '\\') { escaped = true; i++; continue; }
      if (json[i] == '"') break;
    }
    if (i >= end) return false;
    size_t after = i + 1u;
    json_skip_space(json, end, &after);
    if (depth == 1u && !escaped && after < end && json[after] == ':' &&
        i - begin == strlen(key) && memcmp(json + begin, key, i - begin) == 0) {
      *position = after + 1u;
      json_skip_space(json, end, position);
      return true;
    }
  }
  return false;
}

static bool json_string_in_span(const char *json, size_t length, size_t start, size_t end, const char *key, char *output, size_t capacity) {
  size_t position;
  size_t used = 0u;
  if (output == NULL || capacity == 0u || !json_value_position(json, length, start, end, key, &position)) return false;
  if (position >= end || json[position] != '"') {
    return false;
  }
  position++;
  while (position < end) {
    unsigned char value = (unsigned char)json[position++];
    if (value == '"') {
      output[used] = '\0';
      return true;
    }
    if (value < 0x20u) {
      return false;
    }
    if (value != '\\') {
      if (used + 1u >= capacity) {
        return false;
      }
      output[used++] = (char)value;
      continue;
    }
    if (position >= end) {
      return false;
    }
    value = (unsigned char)json[position++];
    if (value == '"' || value == '\\' || value == '/') {
      if (used + 1u >= capacity) {
        return false;
      }
      output[used++] = (char)value;
      continue;
    }
    if (value == 'b' || value == 'f' || value == 'n' || value == 'r' || value == 't') {
      char decoded;
      if (value == 'b') {
        decoded = '\b';
      } else if (value == 'f') {
        decoded = '\f';
      } else if (value == 'n') {
        decoded = '\n';
      } else if (value == 'r') {
        decoded = '\r';
      } else {
        decoded = '\t';
      }
      if (used + 1u >= capacity) {
        return false;
      }
      output[used++] = decoded;
      continue;
    }
    if (value != 'u' || position + 4u > end) {
      return false;
    }
    {
      uint8_t high1;
      uint8_t high2;
      uint8_t high3;
      uint8_t high4;
      uint32_t codepoint;
      if (!json_hex(json[position], &high1) || !json_hex(json[position + 1u], &high2) || !json_hex(json[position + 2u], &high3) || !json_hex(json[position + 3u], &high4)) {
        return false;
      }
      codepoint = ((uint32_t)high1 << 12) | ((uint32_t)high2 << 8) | ((uint32_t)high3 << 4) | high4;
      position += 4u;
      if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
        uint8_t low1;
        uint8_t low2;
        uint8_t low3;
        uint8_t low4;
        uint32_t low;
        if (position + 6u > end || json[position] != '\\' || json[position + 1u] != 'u') {
          return false;
        }
        if (!json_hex(json[position + 2u], &low1) || !json_hex(json[position + 3u], &low2) || !json_hex(json[position + 4u], &low3) || !json_hex(json[position + 5u], &low4)) {
          return false;
        }
        low = ((uint32_t)low1 << 12) | ((uint32_t)low2 << 8) | ((uint32_t)low3 << 4) | low4;
        if (low < 0xdc00u || low > 0xdfffu) {
          return false;
        }
        codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) + (low - 0xdc00u);
        position += 6u;
      } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
        return false;
      }
      {
        size_t written = json_encode_utf8(codepoint, output + used, capacity - used - 1u);
        if (written == 0u) {
          return false;
        }
        used += written;
      }
    }
  }
  return false;
}

static bool json_uint_in_span(const char *json, size_t length, size_t start, size_t end, const char *key, uint32_t *value) {
  size_t position;
  uint32_t result = 0u;
  if (value == NULL || !json_value_position(json, length, start, end, key, &position)) return false;
  if (position >= end || json[position] < '0' || json[position] > '9') {
    return false;
  }
  while (position < end && json[position] >= '0' && json[position] <= '9') {
    uint32_t digit = (uint32_t)(json[position] - '0');
    if (result > (UINT32_MAX - digit) / 10u) {
      return false;
    }
    result = result * 10u + digit;
    position++;
  }
  *value = result;
  return true;
}

static bool json_bool_in_span(const char *json, size_t length, size_t start, size_t end, const char *key, bool *value) {
  size_t position;
  if (value == NULL || !json_value_position(json, length, start, end, key, &position)) return false;
  if (position + 4u <= end && memcmp(json + position, "true", 4u) == 0) { *value = true; return true; }
  if (position + 5u <= end && memcmp(json + position, "false", 5u) == 0) { *value = false; return true; }
  return false;
}

static bool json_object_span(const char *json, size_t length, size_t start, size_t *begin, size_t *end) {
  size_t position = start;
  size_t depth = 0u;
  bool in_string = false;
  bool escaped = false;
  json_skip_space(json, length, &position);
  if (position >= length || json[position] != '{') {
    return false;
  }
  for (; position < length; position++) {
    char value = json[position];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (value == '\\') {
        escaped = true;
      } else if (value == '"') {
        in_string = false;
      }
      continue;
    }
    if (value == '"') {
      in_string = true;
    } else if (value == '{') {
      depth++;
      if (depth == 1u) {
        *begin = position;
      }
    } else if (value == '}') {
      if (depth == 0u) {
        return false;
      }
      depth--;
      if (depth == 0u) {
        *end = position + 1u;
        return true;
      }
    }
  }
  return false;
}

static bool json_array_position(const char *json, uint16_t length, const char *key, size_t *position) {
  size_t value;
  if (!json_value_position(json, length, 0u, length, key, &value) || value >= length || json[value] != '[') return false;
  *position = value + 1u;
  return true;
}

static bool parse_recent_payload(const char *json, uint16_t length) {
  size_t position;
  size_t object_begin;
  size_t object_end;
  uint8_t count = 0u;
  s_recent_count = 0u;
  if (!json_array_position(json, length, "items", &position)) {
    return false;
  }
  while (position < length && count < HERMES_MAX_RECENT_ITEMS) {
    while (position < length && (json[position] == ' ' || json[position] == '\n' || json[position] == '\r' || json[position] == '\t' || json[position] == ',')) {
      position++;
    }
    if (position >= length || json[position] == ']') {
      break;
    }
    if (!json_object_span(json, length, position, &object_begin, &object_end)) {
      return false;
    }
    {
      RecentItem *item = &s_recent_items[count];
      char input[UI_RECENT_LABEL_SIZE];
      char preview[UI_RECENT_LABEL_SIZE];
      uint32_t capture_id;
      uint32_t state = HERMES_STATUS_NONE;
      uint32_t kind = HERMES_ITEM_KIND_REQUEST;
      bool more = false;
      memset(item, 0, sizeof(*item));
      memset(input, 0, sizeof(input));
      memset(preview, 0, sizeof(preview));
      if (!json_uint_in_span(json, length, object_begin, object_end, "captureId", &capture_id)) {
        return false;
      }
      if (json_uint_in_span(json, length, object_begin, object_end, "state", &state)) {
        if (state > UINT8_MAX) {
          return false;
        }
        item->state = (uint8_t)state;
      }
      if (json_uint_in_span(json, length, object_begin, object_end, "kind", &kind)) {
        if (kind > HERMES_ITEM_KIND_NOTE) {
          return false;
        }
        item->kind = (uint8_t)kind;
      }
      if (!json_string_in_span(json, length, object_begin, object_end, "input", input, sizeof(input)) && !json_string_in_span(json, length, object_begin, object_end, "preview", preview, sizeof(preview))) {
        input[0] = '\0';
        preview[0] = '\0';
      }
      if (preview[0] == '\0' && input[0] != '\0') {
        snprintf(item->label, sizeof(item->label), "%s", input);
      } else if (preview[0] != '\0') {
        snprintf(item->label, sizeof(item->label), "%s", preview);
      } else {
        snprintf(item->label, sizeof(item->label), "Capture %lu", (unsigned long)capture_id);
      }
      if (json_bool_in_span(json, length, object_begin, object_end, "more", &more)) {
        item->more = more;
      }
      item->capture_id = capture_id;
      count++;
    }
    position = object_end;
  }
  json_skip_space(json, length, &position);
  if (position >= length || json[position] != ']') {
    return false;
  }
  position++;
  json_skip_space(json, length, &position);
  if (position >= length || json[position++] != '}') return false;
  json_skip_space(json, length, &position);
  if (position != length) return false;
  s_recent_count = count;
  return true;
}

static bool parse_result_payload(const char *json, uint16_t length) {
  size_t begin;
  size_t end;
  uint32_t capture_id;
  uint32_t item_id;
  uint32_t state;
  bool more = false;
  static char input[HERMES_MAX_DICTATION_BYTES + 1u];
  static char output[UI_RESULT_BUFFER_SIZE + 1u];
  if (!json_object_span(json, length, 0u, &begin, &end)) {
    return false;
  }
  json_skip_space(json, length, &end);
  if (end != length) {
    return false;
  }
  if (!json_uint_in_span(json, length, begin, end, "captureId", &capture_id)) {
    return false;
  }
  if (s_outbound.active && s_outbound.kind == HERMES_KIND_FETCH_RESULT &&
      capture_id != s_outbound.capture_id) return false;
  memset(input, 0, sizeof(input));
  memset(output, 0, sizeof(output));
  // Phone pages normally omit the request; keep the watch's own copy then.
  bool has_input = json_string_in_span(json, length, begin, end, "input", input, sizeof(input));
  if (!json_string_in_span(json, length, begin, end, "output", output, sizeof(output))) {
    return false;
  }
  if (json_uint_in_span(json, length, begin, end, "itemId", &item_id)) {
    s_visible_item_id = item_id;
  }
  if (json_uint_in_span(json, length, begin, end, "state", &state)) {
    if (state > UINT8_MAX) {
      return false;
    }
    s_visible_status = (uint8_t)state;
  }
  if (json_bool_in_span(json, length, begin, end, "more", &more) && more) {
    s_result_more = 1u;
  }
  s_visible_capture_id = capture_id;
  if (has_input) memcpy(s_visible_input, input, strlen(input) + 1u);
  memcpy(s_result_page, output, strlen(output) + 1u);
  return true;
}

#include "ink_watch.h"

static void handle_launch_reason(AppLaunchReason reason) {
  bool quick_launch = false;
  switch (reason) {
    case APP_LAUNCH_QUICK_LAUNCH:
      quick_launch = true;
      break;
    case APP_LAUNCH_USER:
    case APP_LAUNCH_SYSTEM:
    default:
      break;
  }
  if (s_storage_corrupt || s_pending.operation != HERMES_PENDING_NONE) {
    s_screen = HERMES_SCREEN_RECOVERY;
  } else if (quick_launch) {
    start_dictation(HERMES_CAPTURE_REQUEST);
  } else {
    s_screen = HERMES_SCREEN_MENU;
  }
  ui_rebuild();
}

int main(void) {
  AppMessageResult open_result;
  AppLaunchReason reason;
  s_visible_output = calloc(UI_RESULT_TEXT_SIZE + 1u, 1u);
  if (s_visible_output == NULL) return 1;
  s_window = window_create();
  if (s_window == NULL) {
    free(s_visible_output);
    return 1;
  }
  window_set_click_config_provider(s_window, ui_click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = ui_window_load,
    .unload = ui_window_unload
  });
  s_screen = HERMES_SCREEN_MENU;
  storage_init();
  audio_cache_recover(); // Interrupted audio is discarded, never resumed.
  audio_schedule();
  ink_load();
  touch_navigation_load();
  connection_service_subscribe((ConnectionHandlers){ .pebble_app_connection_handler = ink_connection });
  open_result = app_message_open(HERMES_APP_MESSAGE_INBOX_SIZE, HERMES_APP_MESSAGE_OUTBOX_SIZE);
  if (open_result == APP_MSG_OK) {
    s_app_message_open = true;
    app_message_register_inbox_received(inbox_received);
    app_message_register_inbox_dropped(inbox_dropped);
    app_message_register_outbox_sent(outbound_message_sent);
    app_message_register_outbox_failed(outbound_message_failed);
  }
  window_stack_push(s_window, true);
  if (!s_app_message_open) {
    ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "The Pebble AppMessage channel could not be opened.");
  } else {
    reason = launch_reason();
    start_handshake();
    handle_launch_reason(reason);
    ink_schedule_sync();
  }
  app_event_loop();
  bike_shutdown();
  audio_shutdown();
  connection_service_unsubscribe();
  ink_shutdown();
  stop_dictation();
  if (s_timer != NULL) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  if (s_app_message_open) {
    app_message_deregister_callbacks();
    s_app_message_open = false;
  }
  ui_destroy_content();
  result_reset();
  free(s_visible_output);
  s_visible_output = NULL;
  window_destroy(s_window);
  s_window = NULL;
  return 0;
}
