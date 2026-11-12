// Memo screens: the recorder and the list of what is waiting.
#include "app.h"
#include "ui.h"
#include "menu.h"

// ---- recorder ------------------------------------------------------------------------------

static Window *s_window;
static Layer *s_layer;
static AppTimer *s_timer;
static bool s_failed;

static void draw(Layer *layer, GContext *ctx) {
  const GRect area = layer_get_unobstructed_bounds(layer);
  graphics_context_set_fill_color(ctx, C_NIGHT);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_fill_color(ctx, s_failed ? C_HAZE : GColorRed);
  graphics_fill_circle(ctx, GPoint(17, 18), 5);
  ui_text(ctx, s_failed ? "MEMO" : "RECORDING", F18B, GRect(30, 5, 150, 22), C_GOLD,
          GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  if (s_failed) {
    ui_text(ctx, "Microphone not available. Allow it for Hermes 2 in the Pebble app.", F24B,
            GRect(12, 40, SCREEN_W - 24, 160), C_PAPER, GTextAlignmentLeft, GTextOverflowModeWordWrap);
    return;
  }
  static char clock[8], left[24];
  const int seconds = memo_record_seconds();
  snprintf(clock, sizeof(clock), "%d:%02d", seconds / 60, seconds % 60);
  ui_text(ctx, clock, F_NUM36, GRect(12, 34, 150, 44), C_PAPER, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  snprintf(left, sizeof(left), "of %d:%02d", MEMO_MAX_SECONDS / 60, MEMO_MAX_SECONDS % 60);
  ui_text(ctx, left, F18, GRect(12, 78, 150, 22), C_HAZE, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  // What each button does, written beside it: the two ways to stop must never be confused.
  const int text_right = SCREEN_W - RAIL_W - 6;
  ui_text(ctx, "Send to Hermes", F18B, GRect(12, BTN_SELECT_Y - 13, text_right - 12, 22), C_GOLD,
          GTextAlignmentRight, GTextOverflowModeTrailingEllipsis);
  // The system's microphone bar covers the bottom: the keep button's icon and label ride above it.
  int stop_y = BTN_DOWN_Y;
  if (stop_y > area.size.h - 16) stop_y = area.size.h - 16;
  ui_text(ctx, "Keep on watch", F18B, GRect(12, stop_y - 13, text_right - 12, 22), C_PAPER,
          GTextAlignmentRight, GTextOverflowModeTrailingEllipsis);
  // Level.
  const int w = text_right - 12;
  const int y = BTN_SELECT_Y + 20;
  graphics_context_set_fill_color(ctx, GColorDukeBlue);
  graphics_fill_rect(ctx, GRect(12, y, w, 8), 4, GCornersAll);
  int level = memo_record_level() * 3;
  if (level > 100) level = 100;
  if (level > 3) {
    graphics_context_set_fill_color(ctx, C_GOLD);
    graphics_fill_rect(ctx, GRect(12, y, w * level / 100 < 8 ? 8 : w * level / 100, 8), 4, GCornersAll);
  }
  GRect beside;
  if (layer_get_mic_accessory_bounds(layer, &beside)) {
    ui_text(ctx, "Saved on the watch", F18B,
            GRect(beside.origin.x + 8, beside.origin.y + (beside.size.h - 24) / 2, beside.size.w - 12, 24),
            C_PAPER, GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  }
  ui_rail(ctx, ICON_NONE, ICON_SEND, ICON_NONE, true);
  ui_icon(ctx, ICON_STOP, GPoint(SCREEN_W - 18, stop_y), C_PAPER);
}

// ---- what happened, said plainly ----------------------------------------------------------

static AppTimer *s_note_timer;
static int32_t s_stopped_id;       // a recording that ended on its own, waiting for a choice

static void note_timeout(void *data) {
  s_note_timer = NULL;
  dialog_pop();
}

static void note_closed(void) {
  if (s_note_timer) app_timer_cancel(s_note_timer);
  s_note_timer = NULL;
}

// A short-lived confirmation; Back closes it sooner.
static void note(const char *title, const char *body) {
  dialog_push(&(DialogSpec){ .label = "MEMO", .title = title, .body = body, .progress = -1,
                             .on_close = note_closed });
  if (s_note_timer) app_timer_cancel(s_note_timer);
  s_note_timer = app_timer_register(2500, note_timeout, NULL);
}

static void say_sent(void) {
  note("Sending to Hermes",
       g_phone.seen ? "Your phone turns it into text and sends it to Hermes as a message."
                    : "It goes to your phone when Hermes 2 can reach it, then on to Hermes.");
}

static void say_kept(void) {
  note("Kept on watch", "Nothing was sent. Play or send it from More, Memos.");
}

static void stopped_send(void) {
  const int32_t id = s_stopped_id;
  s_stopped_id = 0;
  memo_send(id);
  home_refresh();
  say_sent();
}

static void stopped_keep(void) {
  s_stopped_id = 0;
  say_kept();
}

static void done(bool send) {
  if (s_timer) app_timer_cancel(s_timer);
  s_timer = NULL;
  const int32_t id = memo_record_stop();
  if (id && send) memo_send(id);
  window_stack_remove(s_window, true);
  home_refresh();
  if (!id) note("Too short", "Nothing was kept. Record for at least a second.");
  else if (send) say_sent();
  else say_kept();
}

static void tick(void *data) {
  s_timer = NULL;
  if (!s_window) return;
  if (!s_failed && !memo_recording()) {
    // Out of time or room, or something took the microphone: keep what there is, and ask.
    const bool full = memo_record_seconds() + 1 >= MEMO_MAX_SECONDS || memo_free_seconds() == 0;
    if (s_timer) app_timer_cancel(s_timer);
    s_timer = NULL;
    const int32_t id = memo_record_stop();
    window_stack_remove(s_window, true);
    home_refresh();
    if (!id) {
      note("Recording stopped", "Something took the microphone before anything was recorded.");
      return;
    }
    vibes_short_pulse();
    s_stopped_id = id;
    dialog_push(&(DialogSpec){
      .label = "MEMO", .title = "Recording stopped", .progress = -1,
      .body = full ? "That is as long as a memo can be. It is saved on the watch."
                   : "Something took the microphone. What was recorded is saved on the watch.",
      .select_icon = ICON_SEND, .select_label = "Send to Hermes", .on_select = stopped_send,
      .down_icon = ICON_STOP, .down_label = "Keep on watch", .on_down = stopped_keep });
    return;
  }
  layer_mark_dirty(s_layer);
  s_timer = app_timer_register(250, tick, NULL);
}

static void stop_and_send(void) { if (s_failed) window_stack_remove(s_window, true); else done(true); }
static void stop_and_keep(void) { if (s_failed) window_stack_remove(s_window, true); else done(false); }

static const UiKeys s_keys = { .select = stop_and_send, .down = stop_and_keep, .back = stop_and_keep };

// Only the buttons end a recording. The touch rail's zones are fixed while the microphone bar
// moves the keep button up to their boundary, and a finger on a button can brush the screen's
// edge: a touch could send what was meant to be kept, or the reverse. Touches are swallowed.
static void ignore_touch(const TouchEvent *e) {}
static const UiTouch s_touch = { .raw = ignore_touch, .no_rail = true };

static void appear(Window *window) { ui_focus(&s_keys, &s_touch); }
static void disappear(Window *window) { ui_blur(); }

static void unload(Window *window) {
  if (s_timer) app_timer_cancel(s_timer);
  s_timer = NULL;
  if (memo_recording()) memo_record_stop();
  layer_destroy(s_layer);
  window_destroy(s_window);
  s_layer = NULL;
  s_window = NULL;
}

void memo_record_push(void) {
  if (s_window) return;
  if (memo_free_seconds() < 5) {
    dialog_push(&(DialogSpec){ .label = "MEMO", .title = "Watch is full", .progress = -1,
                               .body = "Send or delete a memo to make room for a new one." });
    return;
  }
  memo_play_stop();
  s_window = window_create();
  window_set_background_color(s_window, C_NIGHT);
  s_layer = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(window_get_root_layer(s_window), s_layer);
  ui_keys(s_window, &s_keys);
  window_set_window_handlers(s_window, (WindowHandlers){
    .appear = appear, .disappear = disappear, .unload = unload });
  window_stack_push(s_window, true);
  s_failed = !memo_record_start();
  s_timer = app_timer_register(250, tick, NULL);
}

// ---- list ----------------------------------------------------------------------------------

static ListScreen *s_list;
static int32_t s_picked;

// Newest first.
static const Memo *row_memo(int index) { return memo_at(memo_count() - 1 - index); }

static int memos_count(void) { return memo_count(); }

static void memos_row(int index, ListRow *row) {
  const Memo *m = row_memo(index);
  if (!m) return;
  static char when[12];
  struct tm *t = localtime(&m->recorded);
  strftime(when, sizeof(when), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  snprintf(row->title, sizeof(row->title), "%d:%02d at %s", m->seconds / 60, m->seconds % 60, when);
  row->has_accent = true;
  const int percent = memo_progress(m->id);
  if (memo_playing() && s_picked == m->id) {
    snprintf(row->sub, sizeof(row->sub), "Playing");
    row->accent = C_GOLD;
  } else if (m->status == MEMO_DRAFT) {
    snprintf(row->sub, sizeof(row->sub), "Not sent yet");
    row->accent = C_LINE;
  } else if (m->status == MEMO_ON_PHONE) {
    snprintf(row->sub, sizeof(row->sub), "On phone");
    row->accent = GColorIslamicGreen;
  } else if (percent >= 0 && g_phone.seen) {
    snprintf(row->sub, sizeof(row->sub), "Sending %d%%", percent);
    row->accent = GColorVividCerulean;
  } else {
    snprintf(row->sub, sizeof(row->sub), "Waiting for phone");
    row->accent = GColorOrange;
  }
}

static void act_play(void) {
  if (memo_playing()) {
    memo_play_stop();
  } else {
    const char *title = NULL, *body = NULL;
    switch (memo_play(s_picked)) {
      case MEMO_PLAY_OK: break;
      case MEMO_PLAY_QUIET_TIME:
        title = "Staying quiet";
        body = "Quiet Time is on. Playback works again when it ends.";
        break;
      case MEMO_PLAY_MUTED:
        title = "Staying quiet";
        body = "The watch speaker is muted. Turn it on in Sounds & Haptics.";
        break;
      case MEMO_PLAY_BUSY:
        title = "Speaker busy";
        body = "The speaker would not start. Wait a moment and try again.";
        break;
      case MEMO_PLAY_UNAVAILABLE:
        title = "Can't play";
        body = "This memo can't be played on the watch right now.";
        break;
    }
    if (title) dialog_push(&(DialogSpec){ .label = "MEMO", .title = title, .progress = -1, .body = body });
  }
  list_reload(s_list);
}

static void act_send(void) {
  memo_send(s_picked);
  list_reload(s_list);
  home_refresh();
}

static void act_delete(void) {
  memo_delete(s_picked);
  list_reload(s_list);
  home_refresh();
}

static void memos_select(int index) {
  const Memo *m = row_memo(index);
  if (!m) return;
  s_picked = m->id;
  static MenuItem items[3];
  int n = 0;
  if (m->status != MEMO_ON_PHONE) items[n++] = (MenuItem){ memo_playing() ? "Stop playing" : "Play", act_play };
  if (m->status == MEMO_DRAFT) items[n++] = (MenuItem){ "Send to Hermes", act_send };
  items[n++] = (MenuItem){ m->status == MEMO_ON_PHONE ? "Remove from list" : "Delete", act_delete };
  menu_open(items, n);
}

static void memos_unload(void) {
  memo_play_stop();
  s_list = NULL;
}

static const ListSpec s_spec = {
  .heading = "MEMOS", .empty = "No memos. Record one from More.", .count = memos_count,
  .row = memos_row, .select = memos_select, .unload = memos_unload,
};

void memos_push(void) { s_list = list_push(&s_spec); }
void memos_refresh(void) { list_reload(s_list); }
