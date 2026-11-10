// Home: one press to talk, with the conversation it will continue in view.
#include "app.h"
#include "ui.h"

static Window *s_window;
static Layer *s_layer;

static void status_line(char *out, size_t size, GColor *dot) {
  *dot = GColorIslamicGreen;
  if (!comm_connected()) {
    snprintf(out, size, "No phone");
    *dot = GColorRed;
  } else if (!g_phone.seen) {
    snprintf(out, size, "Finding phone");
    *dot = C_LINE;
  } else if (!(g_phone.flags & PF_CONFIGURED)) {
    snprintf(out, size, "Set up on phone");
    *dot = GColorOrange;
  } else if (draft_exists()) {
    snprintf(out, size, "Sending question");
    *dot = GColorOrange;
  } else if (last_question_state() >= 0 && !state_is_final(last_question_state()) &&
             last_question_state() != ST_UNKNOWN) {
    // The last question is still out: say where it is.
    snprintf(out, size, "%s", state_label(last_question_state()));
    *dot = state_color(last_question_state());
  } else if (g_phone.flags & PF_DOWNLOADING) {
    snprintf(out, size, "Speech model %d%%", g_phone.percent);
    *dot = C_GOLD;
  } else if (memo_queued_count() > 0) {
    snprintf(out, size, "%d memo%s to send", memo_queued_count(), memo_queued_count() == 1 ? "" : "s");
    *dot = C_GOLD;
  } else {
    snprintf(out, size, "Ready");
  }
  if (draft_exists() && (!comm_connected() || !g_phone.seen)) {
    snprintf(out, size, "Question saved");
    *dot = GColorOrange;
  }
}

static void draw(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_NIGHT);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  ui_wing(ctx, GPoint(12, 12), C_GOLD);
  ui_text(ctx, "HERMES", F18B, GRect(37, 6, 90, 22), C_GOLD, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  static char clock[8];
  // No AM/PM: the header has room for the time only.
  const time_t now = time(NULL);
  strftime(clock, sizeof(clock), clock_is_24h_style() ? "%H:%M" : "%I:%M", localtime(&now));
  if (clock[0] == '0' && !clock_is_24h_style()) memmove(clock, clock + 1, strlen(clock));
  const int w = SCREEN_W - 12 - RAIL_W - 6;
  // The clock ends where the text column ends, clear of the rail and its Recent icon.
  ui_text(ctx, clock, F18B, GRect(12 + w - 70, 6, 70, 22), C_HAZE, GTextAlignmentRight,
          GTextOverflowModeTrailingEllipsis);

  const bool continuing = !g_prefs.next_new_conv && last_question()[0] &&
                          (!g_phone.seen || (g_phone.flags & PF_CONV_ACTIVE));
  const char *label = continuing ? "CONTINUE" : "NEW CONVERSATION";
  ui_text(ctx, label, F18B, GRect(12, 50, w, 22), C_GOLD, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  if (continuing) {
    ui_text(ctx, last_question(), F28B, GRect(12, 68, w, 90), C_PAPER, GTextAlignmentLeft,
            GTextOverflowModeTrailingEllipsis);
    static char age[24], sub[32];
    ui_age(age, sizeof(age), (int32_t)(time(NULL) - last_question_time()));
    snprintf(sub, sizeof(sub), "%s%s", age, strcmp(age, "now") ? " ago" : "");
    const int used = ui_text_height(last_question(), F28B, w);
    ui_text(ctx, sub, F18, GRect(12, 72 + (used > 84 ? 84 : used), w, 22), C_HAZE,
            GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  } else {
    ui_text(ctx, "Ask Hermes", F28B, GRect(12, 68, w, 62), C_PAPER, GTextAlignmentLeft,
            GTextOverflowModeWordWrap);
    ui_text(ctx, "Press the middle button and speak.", F18, GRect(12, 104, w, 44), C_HAZE,
            GTextAlignmentLeft, GTextOverflowModeWordWrap);
  }

  static char status[32];
  GColor dot;
  status_line(status, sizeof(status), &dot);
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_fill_color(ctx, dot);
  graphics_fill_circle(ctx, GPoint(17, SCREEN_H - 19), 5);
  ui_text(ctx, status, F18B, GRect(30, SCREEN_H - 32, w - 16, 22), C_PAPER, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);

  ui_rail(ctx, ICON_RECENT, ICON_MIC, ICON_MORE, true);
}

static void up(void) { recent_push(); }
static void select(void) { ask_start(); }
static void down(void) { more_push(); }
static void tap(GPoint p) { ask_start(); }

static const UiKeys s_keys = { .up = up, .select = select, .down = down };
static const UiTouch s_touch = { .tap = tap };

static void tick(struct tm *now, TimeUnits changed) {
  if (s_layer) layer_mark_dirty(s_layer);
}

static void appear(Window *window) {
  ui_focus(&s_keys, &s_touch);
  tick_timer_service_subscribe(MINUTE_UNIT, tick);
}

static void disappear(Window *window) {
  ui_blur();
  tick_timer_service_unsubscribe();
}

void home_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, C_NIGHT);
  s_layer = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(window_get_root_layer(s_window), s_layer);
  ui_keys(s_window, &s_keys);
  window_set_window_handlers(s_window, (WindowHandlers){ .appear = appear, .disappear = disappear });
  window_stack_push(s_window, true);
}

void home_refresh(void) {
  if (s_layer) layer_mark_dirty(s_layer);
}

bool home_on_top(void) { return s_window && window_stack_get_top_window() == s_window; }
