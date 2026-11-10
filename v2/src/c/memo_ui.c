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
  ui_text(ctx, clock, F_NUM36, GRect(12, 44, 150, 44), C_PAPER, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  snprintf(left, sizeof(left), "of %d:%02d", MEMO_MAX_SECONDS / 60, MEMO_MAX_SECONDS % 60);
  ui_text(ctx, left, F18, GRect(12, 90, 150, 22), C_HAZE, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  // Level.
  const int w = SCREEN_W - 24 - RAIL_W;
  const int y = area.size.h - 20;
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
  // The system's microphone bar covers the bottom: the stop hint rides just above it.
  int stop_y = BTN_DOWN_Y;
  if (stop_y > area.size.h - 16) stop_y = area.size.h - 16;
  ui_icon(ctx, ICON_STOP, GPoint(SCREEN_W - 18, stop_y), C_PAPER);
}

static void done(bool send) {
  const int32_t id = memo_record_stop();
  if (id && send) memo_send(id);
  window_stack_pop(true);
  home_refresh();
}

static void tick(void *data) {
  s_timer = NULL;
  if (!s_window) return;
  if (!s_failed && !memo_recording()) {
    // Out of room or interrupted: keep what there is, unsent.
    done(false);
    return;
  }
  layer_mark_dirty(s_layer);
  s_timer = app_timer_register(250, tick, NULL);
}

static void stop_and_send(void) { if (s_failed) window_stack_pop(true); else done(true); }
static void stop_and_keep(void) { if (s_failed) window_stack_pop(true); else done(false); }

static const UiKeys s_keys = { .select = stop_and_send, .down = stop_and_keep, .back = stop_and_keep };

static void appear(Window *window) { ui_focus(&s_keys, NULL); }
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
  } else if (!memo_play(s_picked)) {
    dialog_push(&(DialogSpec){ .label = "MEMO", .title = "Staying quiet", .progress = -1,
                               .body = "The speaker is muted or Quiet Time is on." });
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
