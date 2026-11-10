// Asking: live transcription on the phone, Pebble dictation as the fallback, and the review step.
#include "app.h"
#include "ui.h"

// ---- listening -----------------------------------------------------------------------------

typedef enum { PH_STARTING, PH_LIVE, PH_FINISHING, PH_EMPTY } Phase;

#define LISTEN_MAX_SECONDS 60
#define FINISH_TIMEOUT_MS 30000

static Window *s_window;
static Layer *s_layer;
static Phase s_phase;
static char s_text[TEXT_MAX + 1];
static time_t s_started;
static AppTimer *s_tick;
static AppTimer *s_timeout;
static bool s_cancelled;

static void finish(void);

static void redraw(void) {
  if (s_layer) layer_mark_dirty(s_layer);
}

static void draw(Layer *layer, GContext *ctx) {
  const GRect full = layer_get_bounds(layer);
  // The system's Listening bar owns the bottom of the screen while the microphone is on.
  const GRect area = layer_get_unobstructed_bounds(layer);
  graphics_context_set_fill_color(ctx, C_NIGHT);
  graphics_fill_rect(ctx, full, 0, GCornerNone);

  const char *label = s_phase == PH_STARTING ? "STARTING" : s_phase == PH_LIVE ? "LISTENING"
                      : s_phase == PH_FINISHING ? "TRANSCRIBING" : "NOTHING HEARD";
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_fill_color(ctx, s_phase == PH_LIVE ? C_GOLD : C_HAZE);
  graphics_fill_circle(ctx, GPoint(17, 18), 5);
  ui_text(ctx, label, F18B, GRect(30, 5, 120, 22), C_GOLD, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  if (s_phase == PH_LIVE) {
    static char elapsed[16];
    const int seconds = (int)(time(NULL) - s_started);
    snprintf(elapsed, sizeof(elapsed), "%d:%02d", seconds / 60, seconds % 60);
    ui_text(ctx, elapsed, F18B, GRect(140, 5, SCREEN_W - 152, 22), C_HAZE, GTextAlignmentRight,
            GTextOverflowModeTrailingEllipsis);
  }

  const GRect box = GRect(12, 32, SCREEN_W - 24, area.size.h - 38);
  if (s_text[0]) {
    // The newest words stay in view: text taller than the box is anchored to its bottom.
    const int h = ui_text_height(s_text, F28B, box.size.w);
    graphics_context_set_text_color(ctx, C_PAPER);
    if (h <= box.size.h) {
      graphics_draw_text(ctx, s_text, ui_font(F28B), box, GTextOverflowModeWordWrap,
                         GTextAlignmentLeft, NULL);
    } else {
      // Clip by drawing from above the box and covering the overflow with the header.
      graphics_draw_text(ctx, s_text, ui_font(F28B),
                         GRect(box.origin.x, box.origin.y + box.size.h - h, box.size.w, h + 8),
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      graphics_context_set_fill_color(ctx, C_NIGHT);
      graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, 30), 0, GCornerNone);
      graphics_context_set_fill_color(ctx, s_phase == PH_LIVE ? C_GOLD : C_HAZE);
      graphics_fill_circle(ctx, GPoint(17, 18), 5);
      ui_text(ctx, label, F18B, GRect(30, 5, 120, 22), C_GOLD, GTextAlignmentLeft,
              GTextOverflowModeTrailingEllipsis);
    }
  } else {
    const char *hint = s_phase == PH_EMPTY ? "Press the middle button to try again."
                       : s_phase == PH_FINISHING ? "One moment." : "Go ahead.";
    ui_text(ctx, hint, F28B, box, C_HAZE, GTextAlignmentLeft, GTextOverflowModeWordWrap);
  }

  GRect beside;
  if (layer_get_mic_accessory_bounds(layer, &beside)) {
    ui_text(ctx, "Middle button when done", F18B,
            GRect(beside.origin.x + 8, beside.origin.y + (beside.size.h - 24) / 2, beside.size.w - 12, 24),
            C_PAPER, GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  } else if (s_phase == PH_EMPTY) {
    ui_rail(ctx, ICON_NONE, ICON_MIC, ICON_NONE, true);
  }
}

static void stop_timers(void) {
  if (s_tick) app_timer_cancel(s_tick);
  if (s_timeout) app_timer_cancel(s_timeout);
  s_tick = NULL;
  s_timeout = NULL;
}

static void tick(void *data) {
  s_tick = NULL;
  if (s_phase != PH_LIVE) return;
  if (time(NULL) - s_started >= LISTEN_MAX_SECONDS) {
    finish();
    return;
  }
  redraw();
  s_tick = app_timer_register(1000, tick, NULL);
}

static void give_up(void *data) {
  s_timeout = NULL;
  // The phone never came back with a transcript.
  if (s_window) window_stack_remove(s_window, false);
  dialog_push(&(DialogSpec){
    .label = "SPEECH", .title = "No transcript", .progress = -1,
    .body = "The phone did not finish transcribing. Pebble dictation still works.",
    .select_icon = ICON_MIC, .select_label = "Pebble dictation", .on_select = dictation_start });
}

static void finish(void) {
  if (s_phase != PH_LIVE && s_phase != PH_STARTING) return;
  mic_stream_to_phone_stop();
  s_phase = PH_FINISHING;
  stop_timers();
  s_timeout = app_timer_register(FINISH_TIMEOUT_MS, give_up, NULL);
  redraw();
}

static void stream_started(void *context) {
  if (s_phase != PH_STARTING) return;
  s_phase = PH_LIVE;
  s_started = time(NULL);
  if (!s_tick) s_tick = app_timer_register(1000, tick, NULL);
  redraw();
}

static void stream_stopped(MicStreamStopReason reason, void *context) {
  if (!s_window || s_cancelled) return;
  if (reason == MicStreamStopReasonPhone && s_phase == PH_STARTING) {
    // Refused before any audio: the permission is missing, or the phone side is not running.
    stop_timers();
    window_stack_remove(s_window, false);
    dialog_push(&(DialogSpec){
      .label = "MICROPHONE", .title = "Phone said no", .progress = -1,
      .body = "Allow the microphone for Hermes 2 in the Pebble app. Pebble dictation works now.",
      .select_icon = ICON_MIC, .select_label = "Pebble dictation", .on_select = dictation_start });
    return;
  }
  // A notification or the system took the microphone: transcribe what was said so far.
  if (s_phase == PH_LIVE || s_phase == PH_STARTING) {
    s_phase = PH_FINISHING;
    stop_timers();
    s_timeout = app_timer_register(FINISH_TIMEOUT_MS, give_up, NULL);
    redraw();
  }
}

static void start(void) {
  s_text[0] = 0;
  s_cancelled = false;
  s_phase = PH_STARTING;
  comm_send(W_LISTEN, 0, 1, 0, 0, NULL, NULL, 0);
  const MicStreamStartResult result = mic_stream_to_phone_start(
      (MicStreamHandlers){ .started = stream_started, .stopped = stream_stopped }, NULL);
  if (result != MicStreamStartOk) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "mic stream: %d", (int)result);
    window_stack_remove(s_window, false);
    if (result == MicStreamStartErrUnavailable) {
      dictation_start();
    } else {
      dialog_push(&(DialogSpec){ .label = "MICROPHONE", .title = "Microphone busy", .progress = -1,
                                 .body = "Something else is using the microphone. Try again in a moment." });
    }
    return;
  }
  redraw();
}

static void select(void) {
  if (s_phase == PH_EMPTY) start();
  else finish();
}

static void back(void) {
  s_cancelled = true;
  mic_stream_to_phone_stop();
  comm_send(W_LISTEN, 0, 0, 0, 0, NULL, NULL, 0);
  window_stack_pop(true);
}

static void tap(GPoint p) { select(); }

static const UiKeys s_keys = { .select = select, .back = back };
static const UiTouch s_touch = { .tap = tap };

static void appear(Window *window) { ui_focus(&s_keys, &s_touch); }

static void disappear(Window *window) {
  ui_blur();
  if (mic_stream_to_phone_is_active()) mic_stream_to_phone_stop();
}

static void unload(Window *window) {
  stop_timers();
  layer_destroy(s_layer);
  window_destroy(s_window);
  s_layer = NULL;
  s_window = NULL;
}

void listen_push(void) {
  if (s_window) return;
  s_window = window_create();
  window_set_background_color(s_window, C_NIGHT);
  s_layer = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(window_get_root_layer(s_window), s_layer);
  ui_keys(s_window, &s_keys);
  window_set_window_handlers(s_window, (WindowHandlers){
    .appear = appear, .disappear = disappear, .unload = unload });
  window_stack_push(s_window, true);
  start();
}

void listen_on_msg(const Msg *m) {
  if (!s_window || s_cancelled) return;
  switch (m->op) {
    case P_PARTIAL:
      if (s_phase == PH_LIVE || s_phase == PH_FINISHING) {
        strncpy(s_text, m->txt, TEXT_MAX);
        s_text[TEXT_MAX] = 0;
        redraw();
      }
      break;
    case P_VAD:
      if (s_phase == PH_LIVE) finish();
      break;
    case P_FINAL:
      if (s_phase == PH_LIVE || s_phase == PH_STARTING) mic_stream_to_phone_stop();
      stop_timers();
      if (m->a == FINAL_OK && m->txt[0]) {
        static char text[TEXT_MAX + 1];
        strncpy(text, m->txt, TEXT_MAX);
        text[TEXT_MAX] = 0;
        window_stack_remove(s_window, false);
        review_push(text);
      } else if (m->a == FINAL_UNAVAILABLE) {
        window_stack_remove(s_window, false);
        dictation_start();
      } else {
        s_text[0] = 0;
        s_phase = PH_EMPTY;
        redraw();
      }
      break;
  }
}

// ---- Pebble dictation ----------------------------------------------------------------------

static DictationSession *s_dictation;

static void dictated(DictationSession *session, DictationSessionStatus status, char *transcription,
                     void *context) {
  if (status == DictationSessionStatusSuccess && transcription && transcription[0]) {
    review_push(transcription);
  }
}

void listen_deinit(void) {
  if (s_dictation) dictation_session_destroy(s_dictation);
  s_dictation = NULL;
}

void dictation_start(void) {
  if (dialog_open()) dialog_pop();
  if (!s_dictation) s_dictation = dictation_session_create(TEXT_MAX, dictated, NULL);
  if (!s_dictation) {
    dialog_push(&(DialogSpec){ .label = "SPEECH", .title = "No dictation", .progress = -1,
                               .body = "Pebble dictation is not available right now." });
    return;
  }
  // The review screen is the confirmation step, so the system's own is skipped.
  dictation_session_enable_confirmation(s_dictation, false);
  dictation_session_enable_error_dialogs(s_dictation, true);
  dictation_session_start(s_dictation);
}

// ---- review --------------------------------------------------------------------------------

static Window *s_review;
static Layer *s_review_layer;
static char s_question[TEXT_MAX + 1];
static int s_review_scroll;
static int32_t s_review_rid;       // a saved draft keeps its id, so a resend is never a second request

static void review_draw(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_PAPER);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  const int w = SCREEN_W - 12 - RAIL_W - 4;
  // Long questions step down a size before they need scrolling.
  FontId font = F24B;
  int h = ui_text_height(s_question, font, w);
  if (h > SCREEN_H - 40) {
    font = F18B;
    h = ui_text_height(s_question, font, w);
  }
  int max = h - (SCREEN_H - 40);
  if (max < 0) max = 0;
  if (s_review_scroll > max) s_review_scroll = max;
  ui_text(ctx, s_question, font, GRect(12, 30 - s_review_scroll, w, h + 8), C_INK, GTextAlignmentLeft,
          GTextOverflowModeWordWrap);
  ui_band(ctx, g_prefs.next_new_conv ? "NEW CONVERSATION" : "SEND THIS?");
  ui_rail(ctx, ICON_REDO, ICON_SEND, ICON_CLOSE, false);
}

static void review_send(void) {
  const int32_t rid = s_review_rid ? s_review_rid : app_new_id();
  s_review_rid = 0;
  const uint8_t flags = g_prefs.next_new_conv ? ASK_NEW_CONV : 0;
  // Saved first: the question outlives the app, the link and the phone until it is confirmed.
  draft_save(rid, flags, s_question);
  comm_send(W_ASK, rid, flags, 0, 0, s_question, NULL, 0);
  last_question_set(rid, s_question);
  g_prefs.next_new_conv = false;
  window_stack_remove(s_review, false);
  thread_push(rid, s_question, ST_WAITING_PHONE);
}

static void review_redo(void) {
  window_stack_remove(s_review, false);
  ask_start();
}

static void review_cancel(void) { window_stack_pop(true); }

static void review_drag(int dy) {
  s_review_scroll -= dy;
  if (s_review_scroll < 0) s_review_scroll = 0;
  layer_mark_dirty(s_review_layer);
}

static const UiKeys s_review_keys = { .up = review_redo, .select = review_send, .down = review_cancel };
static const UiTouch s_review_touch = { .drag = review_drag };

static void review_appear(Window *window) { ui_focus(&s_review_keys, &s_review_touch); }
static void review_disappear(Window *window) { ui_blur(); }

static void review_unload(Window *window) {
  layer_destroy(s_review_layer);
  window_destroy(s_review);
  s_review_layer = NULL;
  s_review = NULL;
}

void review_push_draft(int32_t rid, const char *text) {
  review_push(text);
  s_review_rid = rid;
}

void review_push(const char *text) {
  s_review_rid = 0;
  strncpy(s_question, text, TEXT_MAX);
  s_question[TEXT_MAX] = 0;
  s_review_scroll = 0;
  if (s_review) {
    layer_mark_dirty(s_review_layer);
    return;
  }
  s_review = window_create();
  window_set_background_color(s_review, C_PAPER);
  s_review_layer = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_review_layer, review_draw);
  layer_add_child(window_get_root_layer(s_review), s_review_layer);
  ui_keys(s_review, &s_review_keys);
  window_set_window_handlers(s_review, (WindowHandlers){
    .appear = review_appear, .disappear = review_disappear, .unload = review_unload });
  window_stack_push(s_review, true);
}
