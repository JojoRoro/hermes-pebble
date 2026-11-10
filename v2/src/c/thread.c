// One question and its answer: live progress, then the reply under the question.
// Recent is the list of the last three.
#include "app.h"
#include "ui.h"
#include "menu.h"

#define ANSWER_CAP 12000
#define BLOCKS_MAX 24
#define TEXT_W (SCREEN_W - 24)
// Answer text sits on a grid of lines. A line's box is drawn LINE_TOP above its slot; its ink
// starts INK_TOP below the box top and ends INK_BOTTOM below it.
#define LINE_TOP 4
#define INK_TOP 7
#define INK_BOTTOM 30
#define PAGE_LINES 7               // lines moved per press: two of the old page stay in view
#define FOOTER_H 44

typedef struct {
  uint16_t start;
  uint16_t len;
  int16_t h;
  bool para;
} Block;

static Window *s_window;
static Layer *s_layer;
static int32_t s_rid;
static int s_state;
static char s_question[200];
static char s_detail[120];
static char *s_text;
static int s_loaded, s_stored, s_total;
static bool s_requesting;
static Block s_blocks[BLOCKS_MAX];
static int s_nblocks;
static int s_scroll;
static int s_content_h;
static time_t s_opened;
static AppTimer *s_timer;
static int s_line;                 // the answer font's line pitch

static void redraw(void) {
  if (s_layer) layer_mark_dirty(s_layer);
}

static int question_h(void) {
  int h = ui_text_height(s_question, F18B, TEXT_W);
  if (h > 66) h = 66;
  return h + 36;
}

// Where the answer's first line slot starts, in content coordinates.
static int answer_top(void) { return question_h() + 6; }

// The scroll offset that puts line `index` at the top of the screen.
static int line_scroll(int index) { return answer_top() + (INK_TOP - LINE_TOP - 3) + index * s_line; }

// The first line whose ink is wholly below the top edge, and the last wholly above the bottom.
static int first_full_line(void) {
  const int top = answer_top() - LINE_TOP - s_scroll;      // screen y of line 0's box
  int index = 0;
  while (top + index * s_line + INK_TOP < 0) index++;
  return index;
}

static int last_full_line(void) {
  const int top = answer_top() - LINE_TOP - s_scroll;
  int index = first_full_line();
  while (top + (index + 1) * s_line + INK_BOTTOM <= SCREEN_H) index++;
  return index;
}

static int max_scroll(void) {
  const int want = s_content_h - SCREEN_H;
  if (want <= 0 || s_state != ST_ANSWERED || s_nblocks == 0) return want > 0 ? want : 0;
  // The last stop is on the grid too, so the top line there is whole.
  int index = 0;
  while (line_scroll(index) < want) index++;
  return line_scroll(index);
}

static void request_page(void) {
  if (s_requesting || s_state != ST_ANSWERED) return;
  if (s_loaded >= s_total && s_total > 0) return;
  if (s_stored >= ANSWER_CAP || s_nblocks >= BLOCKS_MAX) return;
  if (comm_send(W_OPEN, s_rid, s_loaded, 0, 0, NULL, NULL, 0)) s_requesting = true;
}

static void clamp(void) {
  const int max = max_scroll();
  if (s_scroll > max) s_scroll = max;
  if (s_scroll < 0) s_scroll = 0;
  // Fetch the next piece only when the reader gets near the end of what is here.
  if (s_content_h - s_scroll < SCREEN_H * 2) request_page();
}

// The answer has no rail: up and down page, the middle button opens the actions.
static void draw_footer(GContext *ctx, int y) {
  // All of it or none of it: never half a line of text at the bottom edge.
  if (y + 36 > SCREEN_H) return;
  graphics_context_set_fill_color(ctx, C_PAPER);
  graphics_fill_rect(ctx, GRect(0, y, SCREEN_W, FOOTER_H), 0, GCornerNone);
  if (s_loaded < s_total) {
    const bool capped = s_stored >= ANSWER_CAP || s_nblocks >= BLOCKS_MAX;
    ui_text(ctx, capped ? "The rest is in Hermes." : "Loading more...", F18, GRect(12, y + 6, TEXT_W, 22),
            C_DIM, GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
    return;
  }
  graphics_context_set_stroke_color(ctx, C_LINE);
  graphics_draw_line(ctx, GPoint(12, y + 6), GPoint(SCREEN_W - 12, y + 6));
  ui_icon(ctx, ICON_MENU, GPoint(21, y + 25), C_DIM);
  ui_text(ctx, "Middle button: reply", F18, GRect(38, y + 13, TEXT_W - 26, 22), C_DIM,
          GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
}

static void draw(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_PAPER);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  int y = -s_scroll;
  const int qh = question_h();
  const bool working = !state_is_final(s_state) && s_state != ST_ANSWERED;
  if (y + qh > 0) {
    graphics_context_set_fill_color(ctx, C_NIGHT);
    graphics_fill_rect(ctx, GRect(0, y, SCREEN_W, qh), 0, GCornerNone);
    ui_text(ctx, "YOU ASKED", F14B, GRect(12, y + 5, TEXT_W, 18), C_GOLD, GTextAlignmentLeft,
            GTextOverflowModeTrailingEllipsis);
    if (working) {
      // How long it has been going, from when it was asked.
      static char elapsed[16];
      int seconds = (int)(time(NULL) - s_opened);
      if (seconds < 0) seconds = 0;
      if (seconds < 3600) snprintf(elapsed, sizeof(elapsed), "%d:%02d", seconds / 60, seconds % 60);
      else snprintf(elapsed, sizeof(elapsed), "%d h", seconds / 3600);
      ui_text(ctx, elapsed, F14B, GRect(SCREEN_W - 92, y + 5, 80, 18), C_HAZE, GTextAlignmentRight,
              GTextOverflowModeTrailingEllipsis);
    }
    ui_text(ctx, s_question, F18B, GRect(12, y + 20, TEXT_W, qh - 30), C_PAPER, GTextAlignmentLeft,
            GTextOverflowModeTrailingEllipsis);
  }
  y += qh + 6;

  if (s_state == ST_ANSWERED && s_nblocks > 0) {
    const int text_top = y;
    for (int i = 0; i < s_nblocks; i++) {
      const Block *b = &s_blocks[i];
      if (b->para && i > 0) y += s_line;
      if (y + b->h > 0 && y < SCREEN_H) {
        // Each piece is its own string, so only what is on screen gets laid out.
        char saved = s_text[b->start + b->len];
        s_text[b->start + b->len] = 0;
        ui_text(ctx, s_text + b->start, F24B, GRect(12, y - LINE_TOP, TEXT_W, b->h + 8), C_INK,
                GTextAlignmentLeft, GTextOverflowModeWordWrap);
        s_text[b->start + b->len] = saved;
      }
      y += b->h;
    }
    // A line the bottom edge would cut in half waits for the next page instead.
    const int box0 = text_top - LINE_TOP;
    int cut = box0 + (last_full_line() + 1) * s_line + INK_TOP - 1;
    if (cut < 0) cut = 0;
    if (cut < SCREEN_H) {
      graphics_context_set_fill_color(ctx, C_PAPER);
      graphics_fill_rect(ctx, GRect(0, cut, SCREEN_W, SCREEN_H - cut), 0, GCornerNone);
    }
    draw_footer(ctx, y + 4);
    // Where the reader is. Unloaded pages count at the density of the loaded ones.
    int total_h = s_content_h;
    if (s_loaded > 0 && s_total > s_loaded) {
      const int text_h = s_content_h - FOOTER_H - (qh + 6);
      const int ratio = s_total * 64 / s_loaded;           // 32-bit: no 64-bit division code
      total_h = (qh + 6) + text_h / 4 * ratio / 16 + FOOTER_H;
    }
    if (total_h > SCREEN_H) {
      const int track = SCREEN_H - 8;
      int bar = track * SCREEN_H / total_h;
      if (bar < 16) bar = 16;
      int pos = 4 + (track - bar) * s_scroll / (total_h - SCREEN_H);
      if (pos > 4 + track - bar) pos = 4 + track - bar;
      graphics_context_set_fill_color(ctx, C_GOLD);
      graphics_fill_rect(ctx, GRect(SCREEN_W - 5, pos, 3, bar), 1, GCornersAll);
    }
    return;
  }

  // Progress.
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_fill_color(ctx, state_color(s_state));
  graphics_fill_circle(ctx, GPoint(19, y + 20), 7);
  const int w = SCREEN_W - 36 - RAIL_W;
  const char *label = s_state == ST_ANSWERED ? "Loading" : state_label(s_state);
  const int lh = ui_text_height(label, F28B, w);
  ui_text(ctx, label, F28B, GRect(34, y, w, lh + 8), C_INK, GTextAlignmentLeft, GTextOverflowModeWordWrap);
  y += lh + 8;

  const char *hint = s_detail;
  if (!hint[0]) {
    switch (s_state) {
      case ST_WAITING_PHONE: hint = "Saved on the watch until your phone has it."; break;
      case ST_QUEUED: hint = "On your phone, waiting to reach Hermes."; break;
      case ST_APPROVAL: hint = "Hermes is waiting for you to approve a step. Do that in Hermes."; break;
      case ST_STOPPED: hint = "You stopped it. Work already done stays done."; break;
      case ST_CANCELLED: hint = "It was cancelled before it finished."; break;
      case ST_UNKNOWN: hint = "Hermes may or may not have done this. Check before asking again."; break;
      case ST_FAILED: hint = "Hermes could not finish this one."; break;
      default: hint = ""; break;
    }
  }
  // Beside the rail, clear of the disc, down to the bottom edge.
  ui_text(ctx, hint, F18, GRect(12, y, SCREEN_W - 26 - RAIL_W, SCREEN_H - 6 - y), C_DIM,
          GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);

  if (working || s_state == ST_STOPPING) {
    ui_rail(ctx, ICON_NONE, s_state == ST_STOPPING ? ICON_NONE : ICON_STOP, ICON_MENU, false);
  } else {
    ui_rail(ctx, ICON_NONE, ICON_MENU, ICON_NONE, false);
  }
}

static void timer(void *data) {
  s_timer = NULL;
  if (!s_window || state_is_final(s_state)) return;
  // A status ask every few seconds covers an update that got lost on the way.
  if ((time(NULL) - s_opened) % 6 == 5 && s_state != ST_WAITING_PHONE && comm_idle()) {
    comm_send(W_OPEN, s_rid, -1, 0, 0, NULL, NULL, 0);
  }
  redraw();
  s_timer = app_timer_register(1000, timer, NULL);
}

static void reset_text(void) {
  s_loaded = 0;
  s_stored = 0;
  s_nblocks = 0;
  s_requesting = false;
  s_scroll = 0;
  s_content_h = answer_top();
}

static void set_state(int state) {
  if (state == s_state) return;
  s_state = state;
  if (state_is_final(state) && s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  } else if (!state_is_final(state) && !s_timer && s_window) {
    s_timer = app_timer_register(1000, timer, NULL);
  }
}

// ---- actions -------------------------------------------------------------------------------

static void act_stop(void) {
  comm_send(W_STOP, s_rid, 0, 0, 0, NULL, NULL, 0);
  if (s_state == ST_WAITING_PHONE) {
    // It never left the watch.
    if (draft_rid() == s_rid) draft_clear();
    set_state(ST_CANCELLED);
  } else {
    set_state(ST_STOPPING);
  }
  redraw();
}

static void act_refresh(void) {
  reset_text();
  comm_send(W_OPEN, s_rid, s_state == ST_ANSWERED ? 0 : -1, 0, 0, NULL, NULL, 0);
  if (s_state == ST_ANSWERED) s_requesting = true;
  redraw();
}

static void act_reply(void) {
  g_prefs.next_new_conv = false;
  ask_start();
}

static void act_new(void) {
  g_prefs.next_new_conv = true;
  ask_start();
}

static void act_again(void) {
  static char text[sizeof(s_question)];
  strncpy(text, s_question, sizeof(text));
  window_stack_remove(s_window, false);
  review_push(text);
}

static void open_menu(void) {
  static MenuItem items[4];
  int n = 0;
  if (s_state == ST_ANSWERED) {
    items[n++] = (MenuItem){ "Reply to Hermes", act_reply };
    items[n++] = (MenuItem){ "New conversation", act_new };
    items[n++] = (MenuItem){ "Fetch again", act_refresh };
  } else if (state_is_final(s_state)) {
    items[n++] = (MenuItem){ "Ask again", act_again };
    items[n++] = (MenuItem){ "Check again", act_refresh };
  } else {
    if (s_state != ST_STOPPING) items[n++] = (MenuItem){ "Stop", act_stop };
    items[n++] = (MenuItem){ "Check now", act_refresh };
  }
  menu_open(items, n);
}

static void select(void) {
  if (!state_is_final(s_state) && s_state != ST_STOPPING) act_stop();
  else open_menu();
}

// Paging keeps whole lines: each stop puts a line's top at the top of the screen.
static void up(void) {
  if (s_state != ST_ANSWERED || s_nblocks == 0 || s_scroll == 0) return;
  const int first = first_full_line();
  s_scroll = 0;
  // Back to the very top once the question's screen shows everything above this line.
  if (first - 1 > last_full_line()) {
    const int index = first - PAGE_LINES;
    s_scroll = index > 0 ? line_scroll(index) : line_scroll(0);
  }
  clamp();
  redraw();
}

static void down(void) {
  if (s_state != ST_ANSWERED) {
    open_menu();
    return;
  }
  if (s_nblocks == 0) return;
  int index = last_full_line() - 1;
  if (index < first_full_line() + 1) index = first_full_line() + 1;
  const int target = line_scroll(index);
  s_scroll = target > s_scroll ? target : s_scroll + s_line;
  clamp();
  redraw();
}

static void drag(int dy) {
  s_scroll -= dy;
  clamp();
  redraw();
}

static void tap(GPoint p) { open_menu(); }

static const UiKeys s_keys = { .up = up, .select = select, .down = down, .repeat = true };
static const UiTouch s_touch = { .drag = drag, .tap = tap };

static void appear(Window *window) { ui_focus(&s_keys, &s_touch); }
static void disappear(Window *window) { ui_blur(); }

static void unload(Window *window) {
  if (s_timer) app_timer_cancel(s_timer);
  s_timer = NULL;
  free(s_text);
  s_text = NULL;
  layer_destroy(s_layer);
  window_destroy(s_window);
  s_layer = NULL;
  s_window = NULL;
  s_rid = 0;
}

void thread_push(int32_t rid, const char *question, int state) {
  if (s_window) window_stack_remove(s_window, false);
  s_rid = rid;
  s_state = -1;
  s_detail[0] = 0;
  s_total = 0;
  strncpy(s_question, question ? question : "", sizeof(s_question) - 1);
  s_question[sizeof(s_question) - 1] = 0;
  reset_text();
  s_opened = time(NULL);
  if (!s_line) {
    s_line = ui_text_height("A\nA", F24B, TEXT_W) - ui_text_height("A", F24B, TEXT_W);
    if (s_line < 20 || s_line > 40) s_line = 24;
  }
  s_window = window_create();
  window_set_background_color(s_window, C_PAPER);
  s_layer = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(window_get_root_layer(s_window), s_layer);
  ui_keys(s_window, &s_keys);
  window_set_window_handlers(s_window, (WindowHandlers){
    .appear = appear, .disappear = disappear, .unload = unload });
  window_stack_push(s_window, true);
  set_state(state);
  if (state != ST_WAITING_PHONE) {
    comm_send(W_OPEN, rid, state == ST_ANSWERED ? 0 : -1, 0, 0, NULL, NULL, 0);
    if (state == ST_ANSWERED) s_requesting = true;
  }
}

static void add_page(const Msg *m) {
  s_requesting = false;
  if (m->a != s_loaded || s_nblocks >= BLOCKS_MAX) return;
  s_total = m->b;
  const int len = (int)strlen(m->txt);
  // c carries where the next page starts (the phone drops the break between pages) and whether
  // this page opens a paragraph.
  const int next = m->c >> 1;
  if (!s_text) s_text = malloc(ANSWER_CAP + PAGE_MAX + BLOCKS_MAX + 2);
  if (!s_text || len == 0 || next <= s_loaded || s_stored + len > ANSWER_CAP + PAGE_MAX) return;
  memcpy(s_text + s_stored, m->txt, len + 1);
  Block *b = &s_blocks[s_nblocks];
  b->start = (uint16_t)s_stored;
  b->len = (uint16_t)len;
  b->para = (m->c & 1) != 0;
  // Whole lines only, so every page lands on the same grid.
  const int h = ui_text_height(s_text + s_stored, F24B, TEXT_W);
  b->h = (int16_t)(((h + s_line / 2) / s_line) * s_line);
  if (b->h < s_line) b->h = s_line;
  s_content_h += b->h + ((b->para && s_nblocks > 0) ? s_line : 0);
  if (s_nblocks == 0) s_content_h += FOOTER_H + 4;
  s_nblocks++;
  s_stored += len + 1;
  s_loaded = next;
  clamp();
}

void thread_on_msg(const Msg *m) {
  if (!s_window || m->rid != s_rid) return;
  switch (m->op) {
    case P_STORED:
      if (s_state == ST_WAITING_PHONE) set_state(ST_QUEUED);
      break;
    case P_ITEM:
      if (m->a == ST_ANSWERED && s_state != ST_ANSWERED) reset_text();
      set_state(m->a);
      s_total = m->b;
      // The clock counts from when it was asked, not from when this screen opened.
      if (m->c > 0) s_opened = time(NULL) - m->c;
      strncpy(s_detail, m->txt2, sizeof(s_detail) - 1);
      s_detail[sizeof(s_detail) - 1] = 0;
      if (!s_question[0] && m->txt[0]) {
        const int before = answer_top();
        strncpy(s_question, m->txt, sizeof(s_question) - 1);
        s_content_h += answer_top() - before;
      }
      if (s_state == ST_ANSWERED && s_nblocks == 0) request_page();
      break;
    case P_PAGE:
      add_page(m);
      break;
  }
  redraw();
}

// ---- recent --------------------------------------------------------------------------------

#define RECENT_MAX 3

typedef struct {
  int32_t rid;
  int32_t age;
  uint8_t state;
  char question[72];
} Recent;

static ListScreen *s_recent;
static Recent s_items[RECENT_MAX];
static int s_recent_count;
static bool s_recent_loaded;

static int recent_count(void) { return s_recent_count; }

static void recent_row(int index, ListRow *row) {
  const Recent *r = &s_items[index];
  static char age[16];
  ui_age(age, sizeof(age), r->age);
  strncpy(row->title, r->question, sizeof(row->title) - 1);
  snprintf(row->sub, sizeof(row->sub), "%s, %s", state_label(r->state), age);
  row->has_accent = true;
  row->accent = state_color(r->state);
}

static void recent_select(int index) {
  thread_push(s_items[index].rid, s_items[index].question, s_items[index].state);
}

static void recent_appear(void) {
  comm_send(W_RECENT, 0, RECENT_MAX, 0, 0, NULL, NULL, 0);
}

static void recent_unload(void) { s_recent = NULL; }

static ListSpec s_recent_spec = {
  .heading = "RECENT", .empty = "Asking your phone", .count = recent_count, .row = recent_row,
  .select = recent_select, .appear = recent_appear, .unload = recent_unload,
};

void recent_push(void) {
  s_recent_spec.empty = !comm_connected() ? "Recent answers are on your phone, which is not connected."
                        : s_recent_loaded ? "Nothing asked yet." : "Asking your phone";
  s_recent = list_push(&s_recent_spec);
}

void recent_on_msg(const Msg *m) {
  s_recent_loaded = true;
  s_recent_count = m->b > RECENT_MAX ? RECENT_MAX : m->b;
  if (m->b == 0) s_recent_spec.empty = "Nothing asked yet.";
  if (m->a >= 0 && m->a < s_recent_count) {
    Recent *r = &s_items[m->a];
    r->rid = m->rid;
    r->state = (uint8_t)(m->c & 15);
    r->age = m->c >> 4;
    strncpy(r->question, m->txt, sizeof(r->question) - 1);
    r->question[sizeof(r->question) - 1] = 0;
  }
  list_reload(s_recent);
}
