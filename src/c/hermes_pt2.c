#include <pebble.h>
#include "protocol.h"
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UI_BODY_BUFFER_SIZE 2048u
#define UI_ACTION_BUFFER_SIZE 128u
#define UI_ERROR_BUFFER_SIZE 256u
#define UI_RECENT_LABEL_SIZE 128u
#define UI_RESULT_BUFFER_SIZE 768u
#define SCROLL_STEP 48u
#define MAX_TEXT_HEIGHT 30000u

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
#define HERMES_ACTION_NEXT_RESULT 11u
#define HERMES_ACTION_STATUS 12u
#define HERMES_ACTION_BACK 13u

#define HERMES_MENU_MAIN 0u
#define HERMES_MENU_RECENT 1u
#define HERMES_MENU_ACTIONS 2u

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
  uint8_t wait_ticks;
  uint16_t elapsed_ticks;
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
  uint8_t chunk_lengths[HERMES_MAX_CHUNKS];
  uint8_t chunks[HERMES_MAX_CHUNKS][HERMES_CHUNK_PAYLOAD_SIZE];
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

static Window *s_window;
static MenuLayer *s_menu_layer;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_header_layer;
static char s_header_text[80];
static TextLayer *s_body_text_layer;
static TextLayer *s_action_text_layer;
static bool s_app_message_open;
static bool s_handshake_ready;
static bool s_click_guard;
static bool s_storage_corrupt;
static bool s_identity_valid;
static bool s_discard_in_progress;
static bool s_menu_dispatching;
static uint8_t s_screen;
static uint8_t s_previous_screen;
static uint8_t s_menu_kind;
static uint8_t s_capture_mode;
static uint8_t s_dictation_mode;
static uint8_t s_click_guard_ticks;
static uint8_t s_visible_status;
static uint8_t s_visible_error;
static uint8_t s_visible_flags;
static uint8_t s_visible_item_kind;
static uint8_t s_result_more;
static uint8_t s_recent_count;
static uint16_t s_scroll_offset;
static uint16_t s_scroll_content_height;
static uint16_t s_inbound_elapsed_ticks;
static uint32_t s_counter;
static uint32_t s_generation;
static uint32_t s_install_hash;
static uint32_t s_visible_capture_id;
static uint32_t s_visible_item_id;
static uint32_t s_result_offset;
static uint32_t s_result_total_bytes;
static uint32_t s_result_next_offset;
static uint32_t s_discard_capture_id;
static uint32_t s_discard_transfer_id;
static uint32_t s_transfer_sequence;
static uint8_t s_install_id[16];
static char s_capture_text[HERMES_MAX_DICTATION_BYTES + 1u];
static char s_visible_input[HERMES_MAX_DICTATION_BYTES + 1u];
static char s_visible_output[UI_RESULT_BUFFER_SIZE + 1u];
static char s_body_text[UI_BODY_BUFFER_SIZE];
static char s_action_text[UI_ACTION_BUFFER_SIZE];
static char s_error_text[UI_ERROR_BUFFER_SIZE];
static char s_recent_raw[UI_RESULT_BUFFER_SIZE + 1u];
static char s_result_raw[UI_RESULT_BUFFER_SIZE + 1u];
static RecentItem s_recent_items[HERMES_MAX_RECENT_ITEMS];
static uint8_t s_action_options[8];
static uint8_t s_action_count;
static PendingCapture s_pending;
static OutboundTransfer s_outbound;
static InboundTransfer s_inbound;
static DictationSession *s_dictation_session;
static AppTimer *s_timer;
static char s_menu_labels[8][UI_RECENT_LABEL_SIZE];
static uint16_t s_menu_count;

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
static void ui_action(uint8_t action);
static void ui_scroll(int delta);
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
static void timer_ensure(void);
static void timer_maybe_cancel(void);

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
static bool read_bytes(DictionaryIterator *iter, uint8_t key, uint8_t *value, uint16_t capacity, uint16_t *length, bool required);
static bool common_fields_equal(const InboundTransfer *message, uint8_t protocol_version, uint8_t kind, uint32_t transfer_id, uint32_t capture_id, uint8_t chunk_count);

static uint32_t next_transfer_id(void);
static uint32_t mix_capture_id(uint32_t counter);
static const char *status_text(uint8_t status);
static const char *error_text(uint32_t code);
static const char *dictation_failure_text(int status);
static void set_body_text(const char *text);
static void build_menu(const char *title, const char *const *labels, uint16_t count, uint8_t menu_kind);
static void add_scroll_content(const char *body, const char *actions, uint8_t screen);

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

static void ui_window_load(Window *window) {
  (void)window;
  ui_rebuild();
}

static void ui_window_unload(Window *window) {
  (void)window;
  ui_destroy_content();
}

static void ui_destroy_content(void) {
  if (s_header_layer != NULL) {
    text_layer_destroy(s_header_layer);
    s_header_layer = NULL;
  }
  if (s_body_text_layer != NULL) {
    text_layer_destroy(s_body_text_layer);
    s_body_text_layer = NULL;
  }
  if (s_action_text_layer != NULL) {
    text_layer_destroy(s_action_text_layer);
    s_action_text_layer = NULL;
  }
  if (s_scroll_layer != NULL) {
    scroll_layer_destroy(s_scroll_layer);
    s_scroll_layer = NULL;
  }
  if (s_menu_layer != NULL) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
  }
  s_scroll_offset = 0;
  s_scroll_content_height = 0;
}

static void set_body_text(const char *text) {
  size_t length;
  if (text == s_body_text) {
    return;
  }
  length = strlen(text);
  if (length >= sizeof(s_body_text)) {
    length = sizeof(s_body_text) - 1u;
  }
  memcpy(s_body_text, text, length);
  s_body_text[length] = '\0';
}

/* Shared chrome keeps navigation and phone-link state visible on every screen. */
static void ui_add_header(const char *title) {
  Layer *root = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root);
  snprintf(s_header_text, sizeof(s_header_text), "%s  /  %s", title,
           s_handshake_ready ? "LINKED" : "OFFLINE");
  s_header_layer = text_layer_create(GRect(0, 0, bounds.size.w, 38));
  text_layer_set_background_color(s_header_layer, GColorBlack);
  text_layer_set_text_color(s_header_layer, GColorWhite);
  text_layer_set_font(s_header_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_header_layer, GTextAlignmentCenter);
  text_layer_set_text(s_header_layer, s_header_text);
  layer_add_child(root, text_layer_get_layer(s_header_layer));
}

static const char *ui_screen_title(uint8_t screen) {
  switch (screen) {
    case HERMES_SCREEN_REVIEW: return "Review";
    case HERMES_SCREEN_STATUS: return "Request";
    case HERMES_SCREEN_RESULT: return "Answer";
    case HERMES_SCREEN_RECOVERY: return "Saved draft";
    case HERMES_SCREEN_DICTATION: return "Dictation";
    case HERMES_SCREEN_CONNECTING: return "Connect";
    case HERMES_SCREEN_ERROR: return "Needs attention";
    case HERMES_SCREEN_RECENT: return "Recent";
    default: return "Hermes";
  }
}

static void add_scroll_content(const char *body, const char *actions, uint8_t screen) {
  Layer *root_layer;
  GRect bounds;
  GRect scroll_frame;
  GRect action_frame;
  GSize content_size;
  uint16_t content_height;
  uint16_t view_height;
  size_t length;

  if (s_window == NULL) {
    return;
  }
  root_layer = window_get_root_layer(s_window);
  bounds = layer_get_bounds(root_layer);
  view_height = bounds.size.h > 66 ? (uint16_t)(bounds.size.h - 66) : bounds.size.h;
  GSize measured = graphics_text_layout_get_content_size(body,
      fonts_get_system_font(FONT_KEY_GOTHIC_24), GRect(0, 0, bounds.size.w - 16, MAX_TEXT_HEIGHT),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  content_height = measured.h + 12;
  if (content_height < view_height) {
    content_height = view_height;
  }
  scroll_frame = GRect(0, 40, bounds.size.w, view_height);
  content_size = GSize(bounds.size.w, content_height);
  ui_destroy_content();
  ui_add_header(ui_screen_title(screen));
  s_scroll_layer = scroll_layer_create(scroll_frame);
  s_body_text_layer = text_layer_create(GRect(8, 0, bounds.size.w - 16, content_height));
  text_layer_set_font(s_body_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_overflow_mode(s_body_text_layer, GTextOverflowModeWordWrap);
  text_layer_set_text(s_body_text_layer, body);
  text_layer_set_text_alignment(s_body_text_layer, GTextAlignmentLeft);
  layer_add_child(scroll_layer_get_layer(s_scroll_layer), text_layer_get_layer(s_body_text_layer));
  scroll_layer_set_content_size(s_scroll_layer, content_size);
  scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, 0), false);
  action_frame = GRect(0, bounds.size.h - 24, bounds.size.w, 24);
  s_action_text_layer = text_layer_create(action_frame);
  length = strlen(actions);
  if (length >= sizeof(s_action_text)) {
    length = sizeof(s_action_text) - 1u;
  }
  memcpy(s_action_text, actions, length);
  s_action_text[length] = '\0';
  text_layer_set_font(s_action_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_background_color(s_action_text_layer, GColorBlack);
  text_layer_set_text_color(s_action_text_layer, GColorWhite);
  text_layer_set_text(s_action_text_layer, s_action_text);
  text_layer_set_text_alignment(s_action_text_layer, GTextAlignmentCenter);
  layer_add_child(root_layer, scroll_layer_get_layer(s_scroll_layer));
  layer_add_child(root_layer, text_layer_get_layer(s_action_text_layer));
  s_scroll_offset = 0u;
  s_scroll_content_height = content_height;
  s_screen = screen;
  window_set_click_config_provider(s_window, ui_click_config);
}

static uint16_t menu_get_rows(MenuLayer *menu_layer, uint16_t section, void *context) {
  (void)menu_layer; (void)section; (void)context;
  return s_menu_count;
}

static int16_t menu_row_height(MenuLayer *menu, MenuIndex *index, void *context) {
  (void)menu; (void)index; (void)context;
  return s_menu_kind == HERMES_MENU_MAIN ? 60 : 50;
}

static const char *menu_subtitle(const char *label) {
  if (strcmp(label, "Ask Hermes") == 0) return "Dictate, review, send";
  if (strcmp(label, "Save note") == 0) return "Keep a note on your phone";
  if (strcmp(label, "Recent") == 0) return "Requests, notes & answers";
  if (strcmp(label, "New conversation") == 0) return "Start with a fresh context";
  if (strcmp(label, "Status") == 0) return "Check your last request";
  if (strcmp(label, "Reconnect") == 0) return "Test the phone connection";
  return "Continue your saved draft";
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *index, void *context) {
  (void)context;
  if (index->row >= s_menu_count) return;
  GRect bounds = layer_get_bounds(cell_layer);
  const char *label = s_menu_labels[index->row];
  graphics_context_set_text_color(ctx, menu_cell_layer_is_highlighted(cell_layer) ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(10, 3, bounds.size.w - 20, s_menu_kind == HERMES_MENU_MAIN ? 25 : 44),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  if (s_menu_kind == HERMES_MENU_MAIN) {
    graphics_draw_text(ctx, menu_subtitle(label), fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(10, 30, bounds.size.w - 20, 25), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

static void build_menu(const char *title, const char *const *labels, uint16_t count, uint8_t menu_kind) {
  if (s_window == NULL || count == 0u) return;
  ui_destroy_content();
  s_menu_count = count > 8u ? 8u : count;
  for (uint16_t i = 0u; i < s_menu_count; i++) {
    snprintf(s_menu_labels[i], sizeof(s_menu_labels[i]), "%s", labels[i]);
  }
  s_menu_kind = menu_kind;
  Layer *root_layer = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root_layer);
  ui_add_header(title);
  s_menu_layer = menu_layer_create(GRect(0, 40, bounds.size.w, bounds.size.h - 64));
  if (s_menu_layer == NULL) return;
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_get_rows,
    .draw_row = menu_draw_row,
    .get_cell_height = menu_row_height,
  });
  menu_layer_set_normal_colors(s_menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu_layer, GColorCobaltBlue, GColorWhite);
  layer_add_child(root_layer, menu_layer_get_layer(s_menu_layer));
  s_action_text_layer = text_layer_create(GRect(0, bounds.size.h - 24, bounds.size.w, 24));
  text_layer_set_font(s_action_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text_alignment(s_action_text_layer, GTextAlignmentCenter);
  text_layer_set_text(s_action_text_layer, "UP / DOWN   •   SELECT to open");
  layer_add_child(root_layer, text_layer_get_layer(s_action_text_layer));
  window_set_click_config_provider(s_window, ui_click_config);
}

static void ui_rebuild(void) {
  ui_destroy_content();
  switch (s_screen) {
    case HERMES_SCREEN_MENU:
      ui_show_menu();
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
      add_scroll_content(s_error_text, "SELECT: menu   BACK: menu", HERMES_SCREEN_ERROR);
      break;
    case HERMES_SCREEN_CONNECTING:
      set_body_text("Waiting for phone…\n\nOpen Hermes Pebble on Android and select your Pebble host in Setup.");
      add_scroll_content(s_body_text, "BACK: menu", HERMES_SCREEN_CONNECTING);
      break;
    default:
      s_screen = HERMES_SCREEN_MENU;
      ui_show_menu();
      break;
  }
}

static void ui_show_menu(void) {
  const char *labels[6];
  uint16_t count = 0u;
  if (s_storage_corrupt || s_pending.operation != HERMES_PENDING_NONE) {
    labels[count++] = "Saved draft";
    labels[count++] = "Recent";
    labels[count++] = "Status";
    labels[count++] = "New conversation";
  } else {
    labels[count++] = "Ask Hermes";
    labels[count++] = "Save note";
    labels[count++] = "Recent";
    labels[count++] = "New conversation";
    labels[count++] = "Status";
  }
  labels[count++] = "Reconnect";
  s_screen = HERMES_SCREEN_MENU;
  build_menu("Hermes", labels, count, HERMES_MENU_MAIN);
}

static void ui_show_review(void) {
  snprintf(s_body_text, sizeof(s_body_text), "%s", s_capture_text);
  add_scroll_content(s_body_text, "SELECT: actions   BACK: actions", HERMES_SCREEN_REVIEW);
}

static void ui_show_status(void) {
  const char *label = status_text(s_visible_status);
  const char *error = s_visible_error == HERMES_ERROR_NONE ? "" : error_text(s_visible_error);
  int written = snprintf(s_body_text, sizeof(s_body_text), "%s\n", label);
  if (written < 0 || (size_t)written >= sizeof(s_body_text)) {
    snprintf(s_body_text, sizeof(s_body_text), "Hermes status unavailable.");
  }
  if (!s_handshake_ready) {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "Phone link not verified. Use Reconnect from the menu.\n");
  }
  if (s_visible_error != HERMES_ERROR_NONE) {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "Error: %s\n", error);
  }
  if (s_visible_flags & HERMES_FLAG_MORE) {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "More on phone\n");
  }
  if (s_visible_input[0] != '\0') {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nRequest:\n%s\n", s_visible_input);
  }
  if (s_visible_output[0] != '\0') {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nAnswer:\n%s\n", s_visible_output);
  }
  if (s_pending.operation != HERMES_PENDING_NONE) {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nYour draft is safe on this watch until the phone confirms it is saved.\n");
  }
  add_scroll_content(s_body_text, "SELECT: actions   BACK: menu", HERMES_SCREEN_STATUS);
}

static void ui_show_result(void) {
  snprintf(s_body_text, sizeof(s_body_text), "%s", status_text(s_visible_status));
  if (s_visible_input[0] != '\0') {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nRequest:\n%s\n", s_visible_input);
  }
  if (s_visible_output[0] != '\0') {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nAnswer:\n%s\n", s_visible_output);
  } else {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nNo answer text on this page.\n");
  }
  if (s_result_more) {
    snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nMore on phone\n");
  }
  add_scroll_content(s_body_text, "SELECT: actions   BACK: status", HERMES_SCREEN_RESULT);
}

static void ui_show_recent(void) {
  const char *labels[HERMES_MAX_RECENT_ITEMS];
  uint16_t count = s_recent_count;
  uint16_t i;
  for (i = 0u; i < count; i++) {
    labels[i] = s_recent_items[i].label;
  }
  if (count == 0u) {
    s_screen = HERMES_SCREEN_RECENT;
    add_scroll_content("No recent items are available on the phone.", "SELECT: refresh   BACK: menu", HERMES_SCREEN_RECENT);
    return;
  }
  s_screen = HERMES_SCREEN_RECENT;
  build_menu("Recent", labels, count, HERMES_MENU_RECENT);
}

static void ui_show_recovery(void) {
  if (s_storage_corrupt) {
    set_body_text("Pending capture storage is damaged.\n\nThe watch will not overwrite it. Use Discard to request durable removal from the phone.");
  } else if (s_pending.operation == HERMES_PENDING_NONE) {
    set_body_text("No pending capture.");
  } else {
    snprintf(s_body_text, sizeof(s_body_text), "Recover pending capture\n\nKind: %s\nCapture: %lu\nConversation: %lu\n\n%s\n\nResume uses the persisted transfer and capture ID. No second command will be created.", s_pending.operation == HERMES_PENDING_NOTE ? "local note" : "Hermes request", (unsigned long)s_pending.capture_id, (unsigned long)s_pending.generation, s_pending.text);
    if (s_visible_error != HERMES_ERROR_NONE) {
      snprintf(s_body_text + strlen(s_body_text), sizeof(s_body_text) - strlen(s_body_text), "\nLast error: %s", error_text(s_visible_error));
    }
  }
  add_scroll_content(s_body_text, "SELECT: actions   BACK: stay", HERMES_SCREEN_RECOVERY);
}

static void ui_show_dictation(void) {
  set_body_text("Listening...\n\nUse the phone dictation service. The transcript will require review before anything is sent.");
  add_scroll_content(s_body_text, "SELECT: actions   BACK: cancel", HERMES_SCREEN_DICTATION);
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
    if (s_result_more) {
      labels[count] = "Next result page";
      actions[count++] = HERMES_ACTION_NEXT_RESULT;
    }
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
  } else if (original == HERMES_SCREEN_ERROR) {
    labels[count] = "Main menu";
    actions[count++] = HERMES_ACTION_MENU;
  } else {
    labels[count] = "Main menu";
    actions[count++] = HERMES_ACTION_MENU;
  }
  if (count == 0u) {
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
  if (s_click_guard && !s_menu_dispatching) {
    return;
  }
  if (selection == NULL) {
    return;
  }
  s_click_guard = true;
  s_click_guard_ticks = 1u;
  timer_ensure();
  if (s_menu_kind == HERMES_MENU_MAIN) {
    if (selection->row == s_menu_count - 1u) {
      start_handshake();
      return;
    }
    if (s_storage_corrupt || s_pending.operation != HERMES_PENDING_NONE) {
      switch (selection->row) {
        case 0u:
          s_screen = HERMES_SCREEN_RECOVERY;
          ui_rebuild();
          break;
        case 1u:
          start_fetch_recent();
          break;
        case 2u:
          s_screen = HERMES_SCREEN_STATUS;
          ui_rebuild();
          break;
        case 3u:
          start_new_conversation();
          break;
        default:
          break;
      }
    } else {
      switch (selection->row) {
        case 0u:
          start_dictation(HERMES_CAPTURE_REQUEST);
          break;
        case 1u:
          start_dictation(HERMES_CAPTURE_NOTE);
          break;
        case 2u:
          start_fetch_recent();
          break;
        case 3u:
          start_new_conversation();
          break;
        case 4u:
          s_screen = HERMES_SCREEN_STATUS;
          ui_rebuild();
          break;
        default:
          break;
      }
    }
  } else if (s_menu_kind == HERMES_MENU_RECENT) {
    if (selection->row < s_recent_count) {
      s_visible_capture_id = s_recent_items[selection->row].capture_id;
      s_visible_item_kind = s_recent_items[selection->row].kind;
      s_visible_status = s_recent_items[selection->row].state;
      s_visible_error = HERMES_ERROR_NONE;
      s_visible_flags = 0u;
      s_visible_output[0] = '\0';
      s_result_offset = 0u;
      s_result_total_bytes = 0u;
      start_fetch_result(s_visible_capture_id, 0u);
    }
  }
}

static void action_menu_select(void *context, MenuLayer *menu_layer, MenuIndex *selection) {
  (void)context;
  (void)menu_layer;
  if (s_click_guard && !s_menu_dispatching) {
    return;
  }
  if (selection != NULL && selection->row < s_action_count) {
    s_click_guard = true;
    s_click_guard_ticks = 1u;
    timer_ensure();
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
  scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -(int)s_scroll_offset), false);
}

static void ui_select_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (s_click_guard) {
    return;
  }
  s_click_guard = true;
  s_click_guard_ticks = 1u;
  timer_ensure();
  if (s_screen == HERMES_SCREEN_ERROR) {
    ui_action(HERMES_ACTION_MENU);
  } else if (s_screen != HERMES_SCREEN_MENU && s_screen != HERMES_SCREEN_RECENT && s_screen != HERMES_SCREEN_ACTIONS) {
    s_previous_screen = s_screen;
    ui_show_actions();
  }
}

static void ui_up_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (s_click_guard) {
    return;
  }
  s_click_guard = true;
  s_click_guard_ticks = 1u;
  timer_ensure();
  ui_scroll(-SCROLL_STEP);
}

static void ui_down_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (s_click_guard) {
    return;
  }
  s_click_guard = true;
  s_click_guard_ticks = 1u;
  timer_ensure();
  ui_scroll(SCROLL_STEP);
}

static uint8_t menu_row_count(void) {
  if (s_menu_kind == HERMES_MENU_ACTIONS) {
    return s_action_count;
  }
  if (s_menu_kind == HERMES_MENU_RECENT) {
    return s_recent_count;
  }
  return s_storage_corrupt || s_pending.operation != HERMES_PENDING_NONE ? 4u : 5u;
}

static void ui_menu_select_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  MenuIndex selection;
  int16_t selected;
  (void)context;
  if (s_click_guard) {
    return;
  }
  if (s_menu_layer == NULL) {
    if (s_screen == HERMES_SCREEN_RECENT) {
      s_click_guard = true;
      s_click_guard_ticks = 1u;
      timer_ensure();
      start_fetch_recent();
    }
    return;
  }
  s_click_guard = true;
  s_click_guard_ticks = 1u;
  timer_ensure();
  selected = (int16_t)menu_layer_get_selected_index(s_menu_layer).row;
  if (selected < 0) {
    return;
  }
  memset(&selection, 0, sizeof(selection));
  selection.section = 0u;
  selection.row = (uint16_t)selected;
  s_menu_dispatching = true;
  if (s_menu_kind == HERMES_MENU_ACTIONS) {
    action_menu_select(NULL, s_menu_layer, &selection);
  } else {
    menu_select(NULL, s_menu_layer, &selection);
  }
  s_menu_dispatching = false;
}

static void ui_menu_move_click(int delta) {
  int16_t selected;
  int16_t next;
  uint8_t count;
  if (s_click_guard || s_menu_layer == NULL) {
    return;
  }
  s_click_guard = true;
  s_click_guard_ticks = 1u;
  timer_ensure();
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

static void ui_back_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (s_screen == HERMES_SCREEN_MENU) {
    window_stack_pop(true);
    return;
  }
  if (s_click_guard) {
    return;
  }
  s_click_guard = true;
  s_click_guard_ticks = 1u;
  timer_ensure();
  if (s_screen == HERMES_SCREEN_ACTIONS) {
    switch (s_previous_screen) {
      case HERMES_SCREEN_REVIEW:
        s_screen = HERMES_SCREEN_REVIEW;
        ui_rebuild();
        break;
      case HERMES_SCREEN_STATUS:
        s_screen = HERMES_SCREEN_STATUS;
        ui_rebuild();
        break;
      case HERMES_SCREEN_RESULT:
        s_screen = HERMES_SCREEN_RESULT;
        ui_rebuild();
        break;
      case HERMES_SCREEN_RECOVERY:
        s_screen = HERMES_SCREEN_RECOVERY;
        ui_rebuild();
        break;
      case HERMES_SCREEN_DICTATION:
        s_screen = HERMES_SCREEN_DICTATION;
        ui_rebuild();
        break;
      case HERMES_SCREEN_ERROR:
        ui_action(HERMES_ACTION_MENU);
        break;
      default:
        ui_action(HERMES_ACTION_MENU);
        break;
    }
  } else if (s_screen == HERMES_SCREEN_DICTATION) {
    stop_dictation();
    ui_action(HERMES_ACTION_MENU);
  } else if (s_screen == HERMES_SCREEN_RECOVERY) {
    ui_rebuild();
  } else {
    if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
      s_screen = HERMES_SCREEN_RECOVERY;
    } else {
      ui_action(HERMES_ACTION_MENU);
    }
  }
}

static void ui_click_config(void *context) {
  (void)context;
  if (s_screen == HERMES_SCREEN_MENU || s_screen == HERMES_SCREEN_RECENT) {
    window_single_click_subscribe(BUTTON_ID_SELECT, ui_menu_select_click);
    window_single_click_subscribe(BUTTON_ID_UP, ui_menu_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN, ui_menu_down_click);
    window_single_click_subscribe(BUTTON_ID_BACK, ui_back_click);
    return;
  }
  if (s_screen == HERMES_SCREEN_ACTIONS) {
    window_single_click_subscribe(BUTTON_ID_SELECT, ui_menu_select_click);
    window_single_click_subscribe(BUTTON_ID_UP, ui_menu_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN, ui_menu_down_click);
    window_single_click_subscribe(BUTTON_ID_BACK, ui_back_click);
    return;
  }
  window_single_click_subscribe(BUTTON_ID_SELECT, ui_select_click);
  window_single_click_subscribe(BUTTON_ID_UP, ui_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, ui_down_click);
  window_single_click_subscribe(BUTTON_ID_BACK, ui_back_click);
}

static void ui_action(uint8_t action) {
  switch (action) {
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
      if (s_pending.operation != HERMES_PENDING_NONE || s_storage_corrupt) {
        s_screen = HERMES_SCREEN_RECOVERY;
      } else {
        s_screen = HERMES_SCREEN_MENU;
      }
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
    case HERMES_ACTION_NEXT_RESULT:
      if (s_visible_capture_id != 0u) {
        start_fetch_result(s_visible_capture_id, s_result_next_offset);
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
  PendingCapture stored;
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
  PendingCapture loaded;
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

static void timer_ensure(void) {
  if (s_timer == NULL) s_timer = app_timer_register(1000u, timer_tick, NULL);
}

static void timer_maybe_cancel(void) {
  if (!s_outbound.active && s_click_guard_ticks == 0u && !s_inbound.active) {
    if (s_timer != NULL) app_timer_cancel(s_timer);
    s_timer = NULL;
    s_click_guard = false;
  }
}

static void timer_tick(void *context) {
  (void)context;
  s_timer = NULL;
  if (s_click_guard_ticks > 0u) {
    s_click_guard_ticks--;
    if (s_click_guard_ticks == 0u) {
      s_click_guard = false;
    }
  }
  if (s_inbound.active) {
    s_inbound_elapsed_ticks++;
    if (s_inbound_elapsed_ticks >= HERMES_CHUNK_TIMEOUT_SECONDS) {
      inbound_error(HERMES_ERROR_TRANSFER_BOUND, "The phone transfer timed out before all chunks arrived.");
    }
  }
  if (s_outbound.active) {
    if (s_outbound.phase == HERMES_OUT_RETRY_WAIT) {
      if (s_outbound.wait_ticks > 0u) {
        s_outbound.wait_ticks--;
      }
      if (s_outbound.wait_ticks == 0u) {
        s_outbound.phase = HERMES_OUT_SENDING;
        s_outbound.elapsed_ticks = 0u;
        outbound_send_current_chunk();
      }
    } else if (s_outbound.phase == HERMES_OUT_WAIT_CHUNK) {
      s_outbound.elapsed_ticks++;
      if (s_outbound.elapsed_ticks >= HERMES_CHUNK_TIMEOUT_SECONDS) {
        outbound_timeout();
      }
    } else if (s_outbound.phase == HERMES_OUT_WAIT_REPLY) {
      s_outbound.elapsed_ticks++;
      if (s_outbound.elapsed_ticks >= HERMES_CHUNK_TIMEOUT_SECONDS) {
        outbound_timeout();
      }
    } else if (s_outbound.phase == HERMES_OUT_WAIT_RECEIPT) {
      s_outbound.elapsed_ticks++;
      if (s_outbound.elapsed_ticks >= HERMES_DURABLE_RECEIPT_TIMEOUT_SECONDS) {
        outbound_timeout();
      }
    }
  }
  if (s_outbound.active || s_click_guard_ticks > 0u || s_inbound.active) timer_ensure();
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
  s_outbound.phase = HERMES_OUT_WAIT_CHUNK;
  s_outbound.elapsed_ticks = 0u;
  timer_ensure();
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
  s_outbound.wait_ticks = delay;
  s_outbound.phase = HERMES_OUT_RETRY_WAIT;
  s_outbound.elapsed_ticks = 0u;
  timer_ensure();
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
  timer_maybe_cancel();
}

static void outbound_failed(uint32_t error_code, const char *text) {
  bool keep_pending = s_outbound.kind == HERMES_KIND_SUBMIT_REQUEST || s_outbound.kind == HERMES_KIND_SAVE_NOTE || s_outbound.kind == HERMES_KIND_DISCARD_CAPTURE;
  uint32_t capture_id = s_outbound.capture_id;
  s_outbound.active = false;
  s_outbound.phase = HERMES_OUT_IDLE;
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
  timer_maybe_cancel();
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
  if (s_outbound.next_chunk + 1u < s_outbound.chunk_count) {
    s_outbound.next_chunk++;
    s_outbound.phase = HERMES_OUT_SENDING;
    s_outbound.elapsed_ticks = 0u;
    outbound_send_current_chunk();
    return;
  }
  s_outbound.phase = s_outbound.kind == HERMES_KIND_SUBMIT_REQUEST || s_outbound.kind == HERMES_KIND_SAVE_NOTE ? HERMES_OUT_WAIT_RECEIPT : HERMES_OUT_WAIT_REPLY;
  s_outbound.elapsed_ticks = 0u;
  timer_ensure();
}

static void outbound_message_failed(DictionaryIterator *iterator, AppMessageResult result, void *context) {
  (void)context; (void)result;
  uint32_t transfer_id = 0;
  if (!read_u32(iterator, HERMES_KEY_TRANSFER_ID, &transfer_id, true) || transfer_id != s_outbound.transfer_id) return;
  if (s_outbound.active) {
    outbound_schedule_retry();
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
  if (!s_app_message_open || s_outbound.active) {
    return;
  }
  s_handshake_ready = false;
  s_screen = HERMES_SCREEN_CONNECTING;
  ui_rebuild();
  outbound_start(HERMES_KIND_HANDSHAKE, HERMES_KIND_HANDSHAKE_ACK, next_transfer_id(), 0u, s_generation, 0u, 0u, 0u, HERMES_STATUS_NONE, HERMES_ERROR_NONE, HERMES_ITEM_KIND_NONE, HERMES_STATUS_NONE, 0u, 0u, NULL, 0u);
}

static void prepare_capture(uint8_t operation) {
  PendingCapture candidate;
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
  s_pending = candidate;
  s_discard_capture_id = capture_id;
  s_visible_capture_id = capture_id;
  s_visible_item_id = 0u;
  s_visible_status = HERMES_STATUS_WAITING_PHONE;
  s_visible_error = HERMES_ERROR_NONE;
  s_visible_flags = 0u;
  s_visible_item_kind = operation == HERMES_PENDING_NOTE ? HERMES_ITEM_KIND_NOTE : HERMES_ITEM_KIND_REQUEST;
  s_result_more = 0u;
  s_visible_output[0] = '\0';
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

static void start_fetch_result(uint32_t capture_id, uint32_t offset) {
  if (!s_app_message_open || s_outbound.active || capture_id == 0u) {
    if (!s_app_message_open) {
      ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "The Pebble phone host is unavailable.");
    }
    return;
  }
  s_result_offset = offset;
  s_result_more = 0u;
  s_screen = HERMES_SCREEN_CONNECTING;
  ui_rebuild();
  outbound_start(HERMES_KIND_FETCH_RESULT, HERMES_KIND_RESULT_PAGE, next_transfer_id(), capture_id, s_generation, 0u, offset, 0u, HERMES_STATUS_NONE, HERMES_ERROR_NONE, s_visible_item_kind, s_visible_status, 0u, 0u, NULL, 0u);
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

static bool read_bytes(DictionaryIterator *iter, uint8_t key, uint8_t *value, uint16_t capacity, uint16_t *length, bool required) {
  Tuple *tuple = dict_find(iter, key);
  *length = 0u;
  if (tuple == NULL) {
    return !required;
  }
  if (tuple->type != TUPLE_BYTE_ARRAY || tuple->length > capacity) {
    return false;
  }
  if (tuple->length > 0u) {
    memcpy(value, tuple->value->data, tuple->length);
  }
  *length = (uint16_t)tuple->length;
  return true;
}

static bool common_fields_equal(const InboundTransfer *message, uint8_t protocol_version, uint8_t kind, uint32_t transfer_id, uint32_t capture_id, uint8_t chunk_count) {
  return message->protocol_version == protocol_version && message->kind == kind && message->transfer_id == transfer_id && message->capture_id == capture_id && message->chunk_count == chunk_count;
}

static void inbound_reset(void) {
  memset(&s_inbound, 0, sizeof(s_inbound));
  s_inbound_elapsed_ticks = 0;
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
  uint8_t payload[HERMES_CHUNK_PAYLOAD_SIZE];
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
    inbound_error(HERMES_ERROR_MALFORMED, "The phone sent a message with missing or invalid required fields.");
    return;
  }
  if (protocol_version != HERMES_PROTOCOL_VERSION) {
    inbound_error(HERMES_ERROR_PROTOCOL_VERSION, "The phone and watch use incompatible protocol versions. Update both apps.");
    return;
  }
  if (kind < HERMES_KIND_HANDSHAKE_ACK || kind > HERMES_KIND_CAPTURE_DISCARDED) {
    inbound_error(HERMES_ERROR_UNSUPPORTED_KIND, "The phone sent an unsupported message kind.");
    return;
  }
  if (transfer_id == 0u || chunk_count == 0u || chunk_count > HERMES_MAX_CHUNKS || chunk_index >= chunk_count) {
    inbound_error(HERMES_ERROR_TRANSFER_BOUND, "The phone sent an invalid transfer identifier or chunk range.");
    return;
  }
  if (!read_u32(iter, HERMES_KEY_CORRELATION_ID, &correlation_id, false) || !read_u32(iter, HERMES_KEY_ITEM_ID, &item_id, false) || !read_u8(iter, HERMES_KEY_STATUS, &status, false) || !read_u8(iter, HERMES_KEY_ERROR_CODE, &error_code, false) || !read_u8(iter, HERMES_KEY_ITEM_STATE, &item_state, false) || !read_u8(iter, HERMES_KEY_ITEM_KIND, &item_kind, false) || !read_u32(iter, HERMES_KEY_PAGE_OFFSET, &page_offset, false) || !read_u8(iter, HERMES_KEY_PAGE_COUNT, &page_count, false) || !read_u32(iter, HERMES_KEY_TOTAL_BYTES, &total_bytes, false) || !read_u32(iter, HERMES_KEY_CONVERSATION_GENERATION, &generation, false) || !read_u8(iter, HERMES_KEY_FLAGS, &flags, false) || !read_bytes(iter, HERMES_KEY_PAYLOAD, payload, sizeof(payload), &payload_length, false)) {
    inbound_error(HERMES_ERROR_MALFORMED, "The phone sent a field with the wrong type or an oversized payload chunk.");
    return;
  }
  if (payload_length > HERMES_CHUNK_PAYLOAD_SIZE) {
    inbound_error(HERMES_ERROR_TRANSFER_BOUND, "The phone sent a chunk outside the 192-byte limit.");
    return;
  }
  if (!s_inbound.active && s_inbound.transfer_id == transfer_id) {
    if (s_inbound.chunk_count == chunk_count && s_inbound.chunk_lengths[chunk_index] == payload_length &&
        memcmp(s_inbound.chunks[chunk_index], payload, payload_length) == 0) return;
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
    s_inbound_elapsed_ticks = 0u;
    timer_ensure();
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
    if (s_inbound.chunk_lengths[chunk_index] != payload_length || memcmp(s_inbound.chunks[chunk_index], payload, payload_length) != 0) {
      inbound_error(HERMES_ERROR_DUPLICATE_CHUNK, "A duplicate chunk did not match the original bytes.");
      return;
    }
    return;
  }
  if ((uint16_t)s_inbound.total_length + payload_length > HERMES_MAX_TRANSFER_BYTES) {
    inbound_error(HERMES_ERROR_TRANSFER_BOUND, "The phone transfer exceeded the 1024-byte logical limit.");
    return;
  }
  memcpy(s_inbound.chunks[chunk_index], payload, payload_length);
  s_inbound.chunk_lengths[chunk_index] = (uint8_t)payload_length;
  s_inbound_elapsed_ticks = 0u;
  s_inbound.total_length = (uint16_t)(s_inbound.total_length + payload_length);
  s_inbound.received_mask = (uint8_t)(s_inbound.received_mask | (uint8_t)(1u << chunk_index));
  if (s_inbound.received_mask == (uint8_t)((1u << chunk_count) - 1u)) {
    process_inbound_transfer();
  }
}

static void process_inbound_transfer(void) {
  uint8_t payload[HERMES_MAX_TRANSFER_BYTES];
  uint16_t offset = 0u;
  uint8_t i;
  InboundTransfer complete;
  if (!s_inbound.active) {
    return;
  }
  for (i = 0u; i < s_inbound.chunk_count; i++) {
    if (offset > HERMES_MAX_TRANSFER_BYTES || s_inbound.chunk_lengths[i] > HERMES_MAX_TRANSFER_BYTES - offset) {
      inbound_error(HERMES_ERROR_TRANSFER_BOUND, "The reassembled phone transfer exceeded its bound.");
      return;
    }
    memcpy(payload + offset, s_inbound.chunks[i], s_inbound.chunk_lengths[i]);
    offset = (uint16_t)(offset + s_inbound.chunk_lengths[i]);
  }
  if (offset != s_inbound.total_length || !utf8_valid(payload, offset)) {
    inbound_error(HERMES_ERROR_MALFORMED, "The phone transfer was not valid UTF-8 text.");
    return;
  }
  complete = s_inbound;
  memcpy(complete.payload, payload, offset);
  complete.payload_length = offset;
  s_inbound.active = false;
  process_phone_message(&complete);
}

static bool inbound_correlation_matches(const InboundTransfer *message) {
  if (message->kind == HERMES_KIND_STATUS_UPDATE) {
    if (message->capture_id != 0u && message->capture_id != s_visible_capture_id && message->capture_id != (s_pending.operation != HERMES_PENDING_NONE ? s_pending.capture_id : 0u)) {
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

static void process_phone_message(const InboundTransfer *message) {
  if (!inbound_correlation_matches(message)) {
    return;
  }
  switch (message->kind) {
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
      outbound_finish();
      if (s_storage_corrupt || s_pending.operation != HERMES_PENDING_NONE) {
        s_screen = HERMES_SCREEN_RECOVERY;
        ui_rebuild();
      } else if (s_screen != HERMES_SCREEN_DICTATION && s_screen != HERMES_SCREEN_REVIEW && s_screen != HERMES_SCREEN_ERROR) {
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
  s_visible_status = message->status;
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
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
}

static void process_status_update(const InboundTransfer *message) {
  if (message->capture_id == 0u) {
    return;
  }
  if (message->capture_id != s_visible_capture_id && (s_pending.operation == HERMES_PENDING_NONE || message->capture_id != s_pending.capture_id)) {
    return;
  }
  s_visible_capture_id = message->capture_id;
  s_visible_item_id = message->item_id;
  s_visible_status = message->status;
  s_visible_error = message->error_code;
  s_visible_flags = message->flags;
  s_visible_item_kind = message->item_kind == HERMES_ITEM_KIND_NONE ? s_visible_item_kind : message->item_kind;
  if (s_outbound.active && s_outbound.kind == HERMES_KIND_STOP_REQUEST && (message->correlation_id == 0u || message->correlation_id == s_outbound.transfer_id)) {
    outbound_finish();
  }
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
}

static void process_recent_page(const InboundTransfer *message) {
  size_t length = message->payload_length;
  if (message->error_code != HERMES_ERROR_NONE) {
    outbound_finish();
    ui_show_error(message->error_code, error_text(message->error_code));
    return;
  }
  if (length > sizeof(s_recent_raw) - 1u) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The recent page was larger than the watch limit.");
    return;
  }
  memcpy(s_recent_raw, message->payload, length);
  s_recent_raw[length] = '\0';
  outbound_finish();
  if (!parse_recent_payload(s_recent_raw, (uint16_t)length)) {
    ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The phone recent page was not valid JSON.");
    return;
  }
  s_screen = HERMES_SCREEN_RECENT;
  ui_rebuild();
}

static void process_result_page(const InboundTransfer *message) {
  size_t length = message->payload_length;
  if (message->error_code != HERMES_ERROR_NONE) {
    outbound_finish();
    ui_show_error(message->error_code, error_text(message->error_code));
    return;
  }
  if (length > sizeof(s_result_raw) - 1u) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The result page was larger than the watch limit.");
    return;
  }
  if (message->capture_id != s_visible_capture_id && s_outbound.capture_id != message->capture_id) {
    outbound_finish();
    ui_show_error(HERMES_ERROR_NOT_FOUND, "The result page did not match the requested capture.");
    return;
  }
  memcpy(s_result_raw, message->payload, length);
  s_result_raw[length] = '\0';
  s_result_offset = message->page_offset;
  s_result_total_bytes = message->total_bytes;
  s_result_more = (message->flags & HERMES_FLAG_MORE) != 0u;
  s_result_next_offset = message->page_offset;
  outbound_finish();
  if (!parse_result_payload(s_result_raw, (uint16_t)length)) {
    ui_show_error(HERMES_ERROR_INVALID_RESPONSE, "The phone result page was not valid JSON.");
    return;
  }
  if (s_result_more) {
    size_t page_bytes = strlen(s_visible_output);
    if (page_bytes <= UINT32_MAX - message->page_offset) {
      s_result_next_offset = message->page_offset + (uint32_t)page_bytes;
    }
  }
  if (message->status != HERMES_STATUS_NONE) {
    s_visible_status = message->status;
  }
  if (message->item_id != 0u) {
    s_visible_item_id = message->item_id;
  }
  if (message->capture_id != 0u) {
    s_visible_capture_id = message->capture_id;
  }
  s_screen = HERMES_SCREEN_RESULT;
  ui_rebuild();
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
  s_visible_status = HERMES_STATUS_NONE;
  s_visible_error = HERMES_ERROR_NONE;
  s_screen = HERMES_SCREEN_STATUS;
  ui_rebuild();
}

static void process_structured_error(const InboundTransfer *message) {
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
        snprintf(item->label, sizeof(item->label), "%s: %s", status_text(item->state), input);
      } else if (preview[0] != '\0') {
        snprintf(item->label, sizeof(item->label), "%s: %s", status_text(item->state), preview);
      } else {
        snprintf(item->label, sizeof(item->label), "%s: Capture %lu", status_text(item->state), (unsigned long)capture_id);
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
  char input[HERMES_MAX_DICTATION_BYTES + 1u];
  char output[UI_RESULT_BUFFER_SIZE + 1u];
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
  memset(input, 0, sizeof(input));
  memset(output, 0, sizeof(output));
  if (!json_string_in_span(json, length, begin, end, "input", input, sizeof(input))) {
    input[0] = '\0';
  }
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
  memcpy(s_visible_input, input, strlen(input) + 1u);
  memcpy(s_visible_output, output, strlen(output) + 1u);
  return true;
}

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
  s_window = window_create();
  if (s_window == NULL) {
    return 1;
  }
  window_set_click_config_provider(s_window, ui_click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = ui_window_load,
    .unload = ui_window_unload
  });
  s_screen = HERMES_SCREEN_MENU;
  storage_init();
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
  }
  app_event_loop();
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
  window_destroy(s_window);
  s_window = NULL;
  return 0;
}
