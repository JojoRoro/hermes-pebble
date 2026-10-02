/* Included after the watch's shared transport and persistence helpers. */
static Layer *s_ink_layer;
static AppTimer *s_ink_advance_timer;
static AppTimer *s_ink_animation_timer;
static AppTimer *s_ink_retry_timer;
// Unconfirmed syncs back off from 30 seconds to 5 minutes between attempts.
#define INK_RETRY_MIN_MS 30000u
#define INK_RETRY_MAX_MS 300000u
static uint32_t s_ink_retry_ms = INK_RETRY_MIN_MS;
static bool s_ink_touch_subscribed;
static bool s_ink_pen_down;
static uint16_t s_ink_stroke_start;
static int s_ink_slide;

static void ink_cancel_timer(AppTimer **timer) {
  if (*timer) app_timer_cancel(*timer);
  *timer = NULL;
}

static void ink_end_stroke(void) {
  if (s_ink_pen_down) {
    if (s_ink_length > s_ink_stroke_start && s_ink_length + 2u <= INK_CAPACITY) {
      s_ink[s_ink_length++] = INK_MARKER;
      s_ink[s_ink_length++] = INK_PEN_UP;
    }
    s_ink_pen_down = false;
  }
}

static void ink_cleanup(void) {
  ink_end_stroke();
  ink_cancel_timer(&s_ink_advance_timer);
  ink_cancel_timer(&s_ink_animation_timer);
  s_ink_slide = 0;
  if (s_ink_touch_subscribed) {
    touch_service_unsubscribe();
    s_ink_touch_subscribed = false;
    touch_navigation_apply();
  }
  if (s_ink_layer) { layer_destroy(s_ink_layer); s_ink_layer = NULL; }
}

static bool ink_clear_saved(void) {
  /* Commit marker is removed first. Orphaned chunks are never loaded. */
  if (persist_exists(INK_STORAGE_META)) persist_delete(INK_STORAGE_META);
  if (persist_exists(INK_STORAGE_META)) return false;
  for (uint16_t i = 0; i < INK_STORAGE_CHUNKS; i++) persist_delete(INK_STORAGE_BASE + i);
  s_ink_saved = s_ink_corrupt = s_ink_sync_requested = false;
  s_ink_capture = 0;
  s_ink_length = INK_HEADER;
  s_ink_offset = 0;
  return true;
}

static bool ink_store(void) {
  uint8_t meta[20] = {0};
  if (s_ink_saved || s_ink_corrupt || !ink_valid(s_ink, s_ink_length)) return false;
  /* Clear an incomplete earlier attempt before writing a new immutable note. */
  if (persist_exists(INK_STORAGE_META)) return false;
  for (uint16_t offset = 0; offset < s_ink_length; offset += INK_STORAGE_CHUNK) {
    uint16_t count = s_ink_length - offset;
    if (count > INK_STORAGE_CHUNK) count = INK_STORAGE_CHUNK;
    if (persist_write_data(INK_STORAGE_BASE + offset / INK_STORAGE_CHUNK, s_ink + offset, count) != count) return false;
  }
  memcpy(meta, "HIS1", 4);
  encode_u32(meta, 4, s_ink_capture);
  encode_u32(meta, 8, s_ink_length);
  encode_u32(meta, 12, ink_crc(s_ink, s_ink_length));
  encode_u32(meta, 16, ink_crc(meta, 16));
  return persist_write_data(INK_STORAGE_META, meta, sizeof(meta)) == (int)sizeof(meta);
}

static void ink_load(void) {
  uint8_t meta[20];
  s_ink_saved = false;
  s_ink_length = INK_HEADER;
  s_ink_corrupt = persist_exists(INK_STORAGE_META);
  if (!s_ink_corrupt) return;
  if (persist_read_data(INK_STORAGE_META, meta, sizeof(meta)) != (int)sizeof(meta) ||
      memcmp(meta, "HIS1", 4) || decode_u32(meta, 16) != ink_crc(meta, 16)) return;
  uint32_t length = decode_u32(meta, 8);
  if (length < 20u || length > INK_CAPACITY || !decode_u32(meta, 4)) return;
  for (uint16_t offset = 0; offset < length; offset += INK_STORAGE_CHUNK) {
    uint16_t count = length - offset;
    if (count > INK_STORAGE_CHUNK) count = INK_STORAGE_CHUNK;
    if (persist_read_data(INK_STORAGE_BASE + offset / INK_STORAGE_CHUNK, s_ink + offset, count) != count) return;
  }
  if (!ink_valid(s_ink, (uint16_t)length) || decode_u32(meta, 12) != ink_crc(s_ink, (uint16_t)length)) return;
  s_ink_length = (uint16_t)length;
  s_ink_capture = decode_u32(meta, 4);
  s_ink_checksum = decode_u32(meta, 12);
  s_ink_offset = 0;
  s_ink_saved = true;
  s_ink_corrupt = false;
  snprintf(s_ink_status, sizeof(s_ink_status), "Saved on watch.\n\nYour handwritten note will sync when the phone is available.");
}

static uint16_t ink_cell_start(uint16_t end) {
  uint16_t start = INK_HEADER;
  for (uint16_t i = INK_HEADER; i + 1u < end; i += 2u) {
    if (s_ink[i] == INK_MARKER && s_ink[i + 1] != INK_PEN_UP) start = i + 2u;
  }
  return start;
}

static bool ink_cell_has_points(uint16_t start, uint16_t end) {
  for (uint16_t i = start; i < end; i += 2u) if (s_ink[i] != INK_MARKER) return true;
  return false;
}

static void ink_draw_cell(GContext *ctx, uint16_t start, uint16_t end, int x) {
  bool down = false;
  GPoint last = GPoint(0, 0);
  for (uint16_t i = start; i + 1u < end; i += 2u) {
    if (s_ink[i] == INK_MARKER) { down = false; continue; }
    GPoint p = GPoint(x + s_ink[i], 35 + s_ink[i + 1]);
    if (down) graphics_draw_line(ctx, last, p);
    else graphics_fill_circle(ctx, p, 1);
    last = p;
    down = true;
  }
}

static void ink_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, UI_ACCENT);
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, UI_HEADER_HEIGHT), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  char title[48];
  snprintf(title, sizeof(title), "%u%% left", (unsigned)((INK_CAPACITY - s_ink_length) * 100u / (INK_CAPACITY - INK_HEADER)));
  graphics_draw_text(ctx, "Handwriting", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(UI_MARGIN, -1, bounds.size.w - 80, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(bounds.size.w - 80, 2, 72, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_rect(ctx, GRect(27, 34, INK_WIDTH + 2, INK_HEIGHT + 2));
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 3);
  uint16_t current = ink_cell_start(s_ink_length);
  if (current > INK_HEADER) ink_draw_cell(ctx, ink_cell_start(current - 2u), current - 2u, 28 - (int)INK_WIDTH + s_ink_slide);
  ink_draw_cell(ctx, current, s_ink_length, 28 + s_ink_slide);
  graphics_draw_text(ctx, s_ink_length + 4u > INK_CAPACITY ? "Full: undo or save" : "Pause to advance / UP undo",
      fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 181, bounds.size.w, 19), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "DOWN space / SELECT save\nBACK options", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(0, 198, bounds.size.w, 30), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void ink_animate(void *context) {
  (void)context;
  s_ink_animation_timer = NULL;
  if (!s_ink_layer) return;
  s_ink_slide -= 20;
  if (s_ink_slide < 0) s_ink_slide = 0;
  layer_mark_dirty(s_ink_layer);
  if (s_ink_slide) s_ink_animation_timer = app_timer_register(20, ink_animate, NULL);
}

static void ink_advance(void *context) {
  (void)context;
  s_ink_advance_timer = NULL;
  if (s_screen != HERMES_SCREEN_INK || s_ink_pen_down || s_ink_length + 2u > INK_CAPACITY ||
      !ink_cell_has_points(ink_cell_start(s_ink_length), s_ink_length)) return;
  s_ink[s_ink_length++] = INK_MARKER;
  s_ink[s_ink_length++] = INK_ADVANCE;
  s_ink_slide = INK_WIDTH;
  ink_cancel_timer(&s_ink_animation_timer);
  s_ink_animation_timer = app_timer_register(20, ink_animate, NULL);
}

static void ink_add_point(uint8_t x, uint8_t y) {
  if (s_ink_length >= s_ink_stroke_start + 2u) {
    int dx = (int)x - s_ink[s_ink_length - 2], dy = (int)y - s_ink[s_ink_length - 1];
    if (dx * dx + dy * dy < 4) return;
    if (s_ink_length >= s_ink_stroke_start + 4u) {
      int ax = s_ink[s_ink_length - 2] - s_ink[s_ink_length - 4];
      int ay = s_ink[s_ink_length - 1] - s_ink[s_ink_length - 3];
      int cross = ax * dy - ay * dx;
      /* Replace the middle point only within one pixel of a forward line. */
      if (ax * dx + ay * dy >= 0 && cross * cross <= ax * ax + ay * ay) {
        s_ink[s_ink_length - 2] = x; s_ink[s_ink_length - 1] = y;
        return;
      }
    }
  }
  if (s_ink_length + 4u <= INK_CAPACITY) { // Reserve pen-up even when full.
    s_ink[s_ink_length++] = x;
    s_ink[s_ink_length++] = y;
  }
}

static void ink_touch(const TouchEvent *event, void *context) {
  (void)context;
  if (s_screen != HERMES_SCREEN_INK || s_ink_saved || event->non_navigational) return;
  if (event->type == TouchEvent_Touchdown) {
    if (event->x < 28 || event->x >= 188 || event->y < 35 || event->y >= 179) return;
    ink_cancel_timer(&s_ink_advance_timer);
    ink_cancel_timer(&s_ink_animation_timer);
    s_ink_slide = 0;
    ink_end_stroke();
    s_ink_stroke_start = s_ink_length;
    s_ink_pen_down = true;
  }
  if (s_ink_pen_down) {
    int x = event->x - 28, y = event->y - 35;
    if (x < 0) x = 0;
    if (x >= (int)INK_WIDTH) x = INK_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= (int)INK_HEIGHT) y = INK_HEIGHT - 1;
    ink_add_point((uint8_t)x, (uint8_t)y);
    if (event->type == TouchEvent_Liftoff) {
      ink_end_stroke();
      s_ink_advance_timer = app_timer_register(1000, ink_advance, NULL);
    }
    if (s_ink_layer) layer_mark_dirty(s_ink_layer);
  }
}

static void ink_show(void) {
  ui_destroy_content();
  s_screen = HERMES_SCREEN_INK;
  app_touch_navigation_enable(false);
  s_ink_layer = layer_create(layer_get_bounds(window_get_root_layer(s_window)));
  if (!s_ink_layer) {
    touch_navigation_apply();
    ui_show_error(HERMES_ERROR_WATCH_UNAVAILABLE, "Could not open drawing area.");
    return;
  }
  layer_set_update_proc(s_ink_layer, ink_draw);
  layer_add_child(window_get_root_layer(s_window), s_ink_layer);
  touch_service_subscribe(ink_touch, NULL);
  s_ink_touch_subscribed = true;
  window_set_click_config_provider(s_window, ui_click_config);
}

static void ink_open(void) {
  cancel_auto_result();
  s_stay_on_menu = false;
  if (s_ink_corrupt) {
    const char *labels[] = {"Keep saved copy", "Discard damaged ink"};
    s_screen = HERMES_SCREEN_INK_ACTIONS;
    build_menu("Ink storage damaged", labels, 2, HERMES_MENU_INK);
    return;
  }
  if (s_ink_saved) {
    s_screen = HERMES_SCREEN_INK_STATUS;
    ink_schedule_sync();
    ui_rebuild();
    return;
  }
  ink_show();
}

static void ink_spool(void) {
  /* An extra delivery route, never the only durable copy. */
  DataLoggingSessionRef session = data_logging_create(INK_LOG_TAG, DATA_LOGGING_BYTE_ARRAY, INK_LOG_ITEM_SIZE, true);
  if (!session) return;
  uint8_t record[INK_LOG_ITEM_SIZE];
  for (uint16_t offset = 0; offset < s_ink_length; offset += HERMES_CHUNK_PAYLOAD_SIZE) {
    uint16_t count = s_ink_length - offset;
    if (count > HERMES_CHUNK_PAYLOAD_SIZE) count = HERMES_CHUNK_PAYLOAD_SIZE;
    memset(record, 0, sizeof(record));
    memcpy(record, "IHC1", 4);
    encode_u32(record, 4, s_ink_capture);
    record[8] = (uint8_t)s_ink_length; record[9] = s_ink_length >> 8;
    record[10] = (uint8_t)offset; record[11] = offset >> 8;
    record[12] = (uint8_t)count; record[13] = count >> 8;
    encode_u32(record, 16, s_ink_checksum);
    memcpy(record + 20, s_ink + offset, count);
    if (data_logging_log(session, record, 1) != DATA_LOGGING_SUCCESS) break;
  }
  data_logging_finish(session);
}

static void ink_save(void) {
  ink_end_stroke();
  ink_cancel_timer(&s_ink_advance_timer);
  if (!ink_cell_has_points(INK_HEADER, s_ink_length)) { vibes_short_pulse(); return; }
  memcpy(s_ink, "HIN1", 4);
  s_ink[4] = INK_WIDTH; s_ink[5] = INK_HEIGHT;
  s_ink[6] = (uint8_t)(s_ink_length - INK_HEADER); s_ink[7] = (s_ink_length - INK_HEADER) >> 8;
  encode_u32(s_ink, 8, (uint32_t)time(NULL));
  encode_u32(s_ink, 12, ink_crc(s_ink + INK_HEADER, s_ink_length - INK_HEADER));
  if (!storage_next_capture_id(&s_ink_capture) || !ink_store()) {
    ui_show_error(HERMES_ERROR_NOTE_STORAGE, "Could not save handwriting. Your drawing is still in memory; reopen Handwritten note to retry.");
    return;
  }
  s_ink_saved = true;
  s_ink_offset = 0;
  s_ink_checksum = ink_crc(s_ink, s_ink_length);
  snprintf(s_ink_status, sizeof(s_ink_status), "Saved on watch.\n\nWill sync to your phone when connected. Enable note sync notifications on the phone for an alert.");
  s_screen = HERMES_SCREEN_INK_STATUS;
  ink_spool();
  ui_rebuild();
  ink_schedule_sync();
  ink_try_send();
}

static void ink_retry(void *context) {
  (void)context;
  s_ink_retry_timer = NULL;
  ink_schedule_sync();
}

static void ink_schedule_sync(void) {
  if (!s_exiting && s_ink_saved && !s_ink_corrupt && connection_service_peek_pebble_app_connection()) {
    s_ink_sync_requested = true;
    timer_update();
  }
}

static void ink_try_send(void) {
  if (s_outbound.active || s_inbound.active) return;
  s_ink_sync_requested = false;
  if (!s_ink_saved || s_ink_corrupt || !connection_service_peek_pebble_app_connection()) return;
  uint16_t count = s_ink_length - s_ink_offset;
  if (count > HERMES_CHUNK_PAYLOAD_SIZE) count = HERMES_CHUNK_PAYLOAD_SIZE;
  outbound_start(HERMES_KIND_INK_BLOCK, HERMES_KIND_INK_RECEIPT, next_transfer_id(), s_ink_capture,
      s_ink_checksum, 0, s_ink_offset, s_ink_length, 0, 0, HERMES_ITEM_KIND_NONE, 0, 0, 0,
      s_ink + s_ink_offset, count);
}

static void ink_failed(void) {
  s_ink_sync_requested = false;
  snprintf(s_ink_status, sizeof(s_ink_status), "Saved on watch.\n\nPhone has not confirmed delivery. We will retry; check the phone host and matching app versions.");
  ink_cancel_timer(&s_ink_retry_timer);
  if (s_ink_saved) {
    s_ink_retry_timer = app_timer_register(s_ink_retry_ms, ink_retry, NULL);
    s_ink_retry_ms = s_ink_retry_ms >= INK_RETRY_MAX_MS / 2u ? INK_RETRY_MAX_MS : s_ink_retry_ms * 2u;
  }
  if (s_screen == HERMES_SCREEN_INK_STATUS) ui_rebuild();
}

static void ink_receipt(const InboundTransfer *message) {
  if (!s_ink_saved || !s_outbound.active || s_outbound.kind != HERMES_KIND_INK_BLOCK ||
      message->capture_id != s_ink_capture || message->generation != s_ink_checksum ||
      message->error_code || !(message->flags & HERMES_FLAG_DURABLE_COMMIT) ||
      message->total_bytes != s_ink_length || message->page_offset != s_ink_offset + s_outbound.length) return;
  uint16_t next = (uint16_t)message->page_offset;
  if (next == s_ink_length && message->status != HERMES_STATUS_NOTE_SAVED) return;
  outbound_finish();
  s_ink_retry_ms = INK_RETRY_MIN_MS;
  if (next == s_ink_length) {
    if (!ink_clear_saved()) { ink_failed(); return; }
    ink_cancel_timer(&s_ink_retry_timer);
    snprintf(s_ink_status, sizeof(s_ink_status), "Saved on phone.\n\nOpen Local notes in Hermes Pebble to view your handwriting.");
    if (s_screen == HERMES_SCREEN_INK_STATUS) { vibes_short_pulse(); ui_rebuild(); }
  } else {
    s_ink_offset = next;
    ink_schedule_sync();
  }
}

static void ink_connection(bool connected) {
  if (!connected) {
    s_handshake_ready = false;
    s_ink_sync_requested = false;
    ui_link_changed();
  } else {
    // A fresh connection is the best time to retry; restart the backoff.
    s_ink_retry_ms = INK_RETRY_MIN_MS;
    ink_cancel_timer(&s_ink_retry_timer);
    ink_schedule_sync();
  }
}

static void ink_select(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_screen == HERMES_SCREEN_INK) ink_save();
  else if (s_ink_saved) { s_ink_retry_ms = INK_RETRY_MIN_MS; ink_schedule_sync(); ink_try_send(); }
  else ink_open();
}

static void ink_undo(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  ink_end_stroke();
  ink_cancel_timer(&s_ink_advance_timer);
  ink_cancel_timer(&s_ink_animation_timer);
  s_ink_slide = 0;
  if (s_ink_length <= INK_HEADER) return;
  if (s_ink[s_ink_length - 2] == INK_MARKER && s_ink[s_ink_length - 1] != INK_PEN_UP) {
    s_ink_length -= 2; // Reopen previous letter, including its strokes.
  } else {
    if (s_ink[s_ink_length - 2] == INK_MARKER) s_ink_length -= 2;
    while (s_ink_length > INK_HEADER && s_ink[s_ink_length - 2] != INK_MARKER) s_ink_length -= 2;
  }
  if (s_ink_layer) layer_mark_dirty(s_ink_layer);
}

static void ink_space(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  ink_end_stroke();
  ink_cancel_timer(&s_ink_advance_timer);
  ink_cancel_timer(&s_ink_animation_timer);
  s_ink_slide = 0;
  if (s_ink_length + 2u <= INK_CAPACITY) {
    s_ink[s_ink_length++] = INK_MARKER; s_ink[s_ink_length++] = INK_SPACE;
  }
  if (s_ink_layer) layer_mark_dirty(s_ink_layer);
}

static void ink_back(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (s_audio.phase == 1u || s_audio.phase == 2u) audio_terminal(HERMES_AUDIO_CANCELLED);
  ink_end_stroke();
  if (s_screen != HERMES_SCREEN_INK || s_ink_length == INK_HEADER) { ui_back_click(recognizer, context); return; }
  const char *labels[] = {"Continue drawing", "Save handwritten note", "Discard drawing"};
  s_screen = HERMES_SCREEN_INK_ACTIONS;
  build_menu("Handwriting", labels, 3, HERMES_MENU_INK);
}

static void ink_menu_select(uint16_t row) {
  if (s_ink_corrupt) {
    if (row == 1 && !ink_clear_saved()) { ui_show_error(HERMES_ERROR_NOTE_STORAGE, "Could not clear damaged ink."); return; }
    s_screen = HERMES_SCREEN_MENU;
    ui_rebuild();
  } else if (row == 0) ink_open();
  else if (row == 1) ink_save();
  else {
    s_ink_length = INK_HEADER;
    s_screen = HERMES_SCREEN_MENU;
    ui_rebuild();
  }
}

static void ink_click_config(void) {
  window_single_click_subscribe(BUTTON_ID_SELECT, ink_select);
  window_single_click_subscribe(BUTTON_ID_BACK, ink_back);
  if (s_screen == HERMES_SCREEN_INK) {
    window_single_click_subscribe(BUTTON_ID_UP, ink_undo);
    window_single_click_subscribe(BUTTON_ID_DOWN, ink_space);
  } else {
    window_single_click_subscribe(BUTTON_ID_UP, ui_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN, ui_down_click);
  }
}

static void ink_shutdown(void) {
  ink_cleanup();
  ink_cancel_timer(&s_ink_retry_timer);
}
