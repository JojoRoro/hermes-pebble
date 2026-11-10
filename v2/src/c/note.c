// Handwritten notes: draw one character at a time; the strokes are kept, not recognised.
//
// A note is a byte stream. Points are x,y pairs on a 120-wide grid (values below 0xF0).
//   0xFD pen up    0xFE space    0xFF end of character
#include "app.h"
#include "ui.h"

#define KEY_META 200
#define KEY_DATA 201               // ..208
#define NOTE_MAX 2048
#define PEN_UP 0xFD
#define SPACE 0xFE
#define CHAR_END 0xFF

#define PAD_X 6
#define PAD_Y 62
#define PAD_W 144               // leaves the room gauge clear of the select disc
#define PAD_H 160
#define COMMIT_MS 800

typedef struct {
  int32_t id;
  uint16_t len;
  uint32_t crc;
  uint8_t pending;                 // written but not yet confirmed by the phone
} NoteMeta;

static uint8_t s_note[NOTE_MAX];
static NoteMeta s_meta;
static bool s_loaded;

static Window *s_window;
static Layer *s_layer;
static uint8_t s_cur[400];         // the character being drawn
static uint16_t s_cur_len;
static GPoint s_last;
static bool s_pen_down;
static AppTimer *s_commit;
static bool s_changed;             // edited since the screen opened

static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = ~0u;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
  }
  return ~crc;
}

static void load(void) {
  if (s_loaded) return;
  s_loaded = true;
  memset(&s_meta, 0, sizeof(s_meta));
  if (persist_read_data(KEY_META, &s_meta, sizeof(s_meta)) != (int)sizeof(s_meta) || s_meta.len > NOTE_MAX) {
    memset(&s_meta, 0, sizeof(s_meta));
    return;
  }
  for (int piece = 0; piece * 256 < s_meta.len; piece++) {
    const int want = s_meta.len - piece * 256 > 256 ? 256 : s_meta.len - piece * 256;
    persist_read_data(KEY_DATA + piece, s_note + piece * 256, want);
  }
}

static void save(void) {
  for (int piece = 0; piece * 256 < s_meta.len; piece++) {
    const int n = s_meta.len - piece * 256 > 256 ? 256 : s_meta.len - piece * 256;
    persist_write_data(KEY_DATA + piece, s_note + piece * 256, n);
  }
  s_meta.crc = crc32(s_note, s_meta.len);
  s_meta.id = app_new_id();        // an edited note is a new version to the phone
  s_meta.pending = s_meta.len > 0;
  persist_write_data(KEY_META, &s_meta, sizeof(s_meta));
}

bool note_pending(void) {
  load();
  return s_meta.pending && s_meta.len > 0;
}

void note_sync(void) {
  if (!note_pending() || !g_phone.seen) return;
  for (uint16_t offset = 0; offset < s_meta.len; offset += 1024) {
    const uint16_t n = s_meta.len - offset > 1024 ? 1024 : s_meta.len - offset;
    comm_send(W_NOTE, s_meta.id, offset, s_meta.len, (int32_t)s_meta.crc, NULL, s_note + offset, n);
  }
}

void note_on_msg(const Msg *m) {
  load();
  if (m->rid != s_meta.id) return;
  // The phone has it: the watch copy is no longer needed.
  for (int piece = 0; piece < 8; piece++) persist_delete(KEY_DATA + piece);
  persist_delete(KEY_META);
  memset(&s_meta, 0, sizeof(s_meta));
  if (s_layer) layer_mark_dirty(s_layer);
}

// ---- drawing -------------------------------------------------------------------------------

// Draws one stored character into a cell. Returns the index after it.
static uint16_t draw_char(GContext *ctx, const uint8_t *data, uint16_t at, uint16_t end, GRect cell) {
  bool have = false;
  GPoint prev = GPointZero;
  while (at < end) {
    const uint8_t b = data[at];
    if (b == CHAR_END || b == SPACE) return at + 1;
    if (b == PEN_UP) {
      have = false;
      at++;
      continue;
    }
    if (at + 1 >= end) return end;
    const GPoint p = GPoint(cell.origin.x + data[at] * cell.size.w / 120,
                            cell.origin.y + data[at + 1] * cell.size.h / 128);
    if (have) graphics_draw_line(ctx, prev, p);
    prev = p;
    have = true;
    at += 2;
  }
  return end;
}

static int char_count(void) {
  int n = 0;
  for (uint16_t i = 0; i < s_meta.len; i++) n += s_note[i] == CHAR_END || s_note[i] == SPACE;
  return n;
}

static void draw(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_PAPER);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  const int band = ui_band(ctx, s_meta.pending ? "NOTE" : "NEW NOTE");

  // What has been written so far: the last characters, small.
  const int cell_w = 18, shown = (SCREEN_W - 16 - RAIL_W) / cell_w;
  const int total = char_count();
  int skip = total > shown ? total - shown : 0;
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_stroke_color(ctx, C_INK);
  graphics_context_set_stroke_width(ctx, 1);
  uint16_t at = 0;
  int column = 0;
  while (at < s_meta.len) {
    if (skip > 0) {
      while (at < s_meta.len && s_note[at] != CHAR_END && s_note[at] != SPACE) at++;
      at++;
      skip--;
      continue;
    }
    const GRect cell = GRect(8 + column * cell_w, band + 4, cell_w - 3, 22);
    if (s_note[at] == SPACE) {
      // A space shows as a short mark on the line, so pressing it visibly does something.
      graphics_context_set_stroke_color(ctx, C_LINE);
      graphics_draw_line(ctx, GPoint(cell.origin.x + 3, cell.origin.y + 20),
                         GPoint(cell.origin.x + cell.size.w - 3, cell.origin.y + 20));
      graphics_context_set_stroke_color(ctx, C_INK);
    }
    at = draw_char(ctx, s_note, at, s_meta.len, cell);
    column++;
  }
  graphics_context_set_stroke_color(ctx, C_LINE);
  graphics_draw_line(ctx, GPoint(8, band + 29), GPoint(SCREEN_W - RAIL_W - 8, band + 29));

  // The pad, with a baseline to write on.
  graphics_context_set_stroke_color(ctx, C_LINE);
  graphics_draw_round_rect(ctx, GRect(PAD_X, PAD_Y, PAD_W, PAD_H), 8);
  graphics_draw_line(ctx, GPoint(PAD_X + 10, PAD_Y + PAD_H * 3 / 4), GPoint(PAD_X + PAD_W - 10, PAD_Y + PAD_H * 3 / 4));
  if (s_cur_len == 0 && total == 0) {
    ui_text(ctx, "Draw a letter here", F18, GRect(PAD_X, PAD_Y + 50, PAD_W, 22), C_DIM,
            GTextAlignmentCenter, GTextOverflowModeTrailingEllipsis);
  }
  graphics_context_set_stroke_color(ctx, C_INK);
  graphics_context_set_stroke_width(ctx, 4);
  draw_char(ctx, s_cur, 0, s_cur_len, GRect(PAD_X, PAD_Y, PAD_W, PAD_H));
  graphics_context_set_stroke_width(ctx, 1);

  // Room left.
  const int used = s_meta.len * (PAD_H - 20) / NOTE_MAX;
  graphics_context_set_fill_color(ctx, C_LINE);
  graphics_fill_rect(ctx, GRect(PAD_X + PAD_W + 3, PAD_Y + 10, 3, PAD_H - 20), 1, GCornersAll);
  graphics_context_set_fill_color(ctx, C_GOLD);
  if (used > 0) graphics_fill_rect(ctx, GRect(PAD_X + PAD_W + 3, PAD_Y + 10, 3, used), 1, GCornersAll);

  ui_rail(ctx, ICON_UNDO, ICON_CHECK, ICON_SPACE, false);
}

static void append(const uint8_t *data, uint16_t len) {
  if (s_meta.len + len > NOTE_MAX) {
    vibes_short_pulse();           // full
    return;
  }
  memcpy(s_note + s_meta.len, data, len);
  s_meta.len += len;
  s_meta.pending = 1;
  s_changed = true;
}

static void commit(void *data) {
  s_commit = NULL;
  if (s_pen_down || s_cur_len == 0) return;
  if (s_cur_len + 1u <= sizeof(s_cur)) s_cur[s_cur_len++] = CHAR_END;
  append(s_cur, s_cur_len);
  s_cur_len = 0;
  layer_mark_dirty(s_layer);
}

static void commit_now(void) {
  if (s_commit) app_timer_cancel(s_commit);
  commit(NULL);
}

static void add_point(int x, int y) {
  if (s_cur_len + 3u >= sizeof(s_cur)) return;
  int gx = (x - PAD_X) * 120 / PAD_W, gy = (y - PAD_Y) * 128 / PAD_H;
  gx = gx < 0 ? 0 : gx > 119 ? 119 : gx;
  gy = gy < 0 ? 0 : gy > 127 ? 127 : gy;
  s_cur[s_cur_len++] = (uint8_t)gx;
  s_cur[s_cur_len++] = (uint8_t)gy;
}

static void undo(void);
static void finish(void);
static void space(void);

static void touched(const TouchEvent *e) {
  const bool in_pad = e->x >= PAD_X && e->x < PAD_X + PAD_W + 6 && e->y >= PAD_Y - 6;
  switch (e->type) {
    case TouchEvent_Touchdown:
      if (!in_pad) {
        // The right edge still works as the three buttons.
        if (e->x >= SCREEN_W - 44) {
          if (e->y < 76) undo();
          else if (e->y < 152) finish();
          else space();
        }
        return;
      }
      if (s_commit) app_timer_cancel(s_commit);
      s_commit = NULL;
      if (s_cur_len > 0 && s_cur_len + 1u < sizeof(s_cur)) s_cur[s_cur_len++] = PEN_UP;
      s_pen_down = true;
      s_last = GPoint(e->x, e->y);
      add_point(e->x, e->y);
      break;
    case TouchEvent_PositionUpdate:
      if (!s_pen_down) return;
      if (abs(e->x - s_last.x) + abs(e->y - s_last.y) < 5) return;
      s_last = GPoint(e->x, e->y);
      add_point(e->x, e->y);
      break;
    case TouchEvent_Liftoff:
      if (!s_pen_down) return;
      s_pen_down = false;
      // A pause after lifting the finger ends the character.
      s_commit = app_timer_register(COMMIT_MS, commit, NULL);
      break;
  }
  layer_mark_dirty(s_layer);
}

static void undo(void) {
  if (s_cur_len > 0) {
    if (s_commit) app_timer_cancel(s_commit);
    s_commit = NULL;
    s_cur_len = 0;
  } else if (s_meta.len > 0) {
    // Back to just after the previous character's end.
    uint16_t at = s_meta.len - 1;
    while (at > 0 && s_note[at - 1] != CHAR_END && s_note[at - 1] != SPACE) at--;
    s_meta.len = at;
    s_meta.pending = 1;
    s_changed = true;
  }
  layer_mark_dirty(s_layer);
}

static void space(void) {
  commit_now();
  const uint8_t b = SPACE;
  append(&b, 1);
  layer_mark_dirty(s_layer);
}

static void finish(void) {
  commit_now();
  // Unchanged, it keeps its id: the phone already has or is getting this version.
  if (s_changed) save();
  s_changed = false;
  note_sync();
  window_stack_pop(true);
}

static const UiKeys s_keys = { .up = undo, .select = finish, .down = space, .back = finish };
static const UiTouch s_touch = { .raw = touched, .always = true };

static void appear(Window *window) { ui_focus(&s_keys, &s_touch); }
static void disappear(Window *window) { ui_blur(); }

static void unload(Window *window) {
  if (s_commit) app_timer_cancel(s_commit);
  s_commit = NULL;
  layer_destroy(s_layer);
  window_destroy(s_window);
  s_layer = NULL;
  s_window = NULL;
}

void note_push(void) {
  if (s_window) return;
  load();
  s_cur_len = 0;
  s_pen_down = false;
  s_changed = false;
  s_window = window_create();
  window_set_background_color(s_window, C_PAPER);
  s_layer = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(window_get_root_layer(s_window), s_layer);
  ui_keys(s_window, &s_keys);
  window_set_window_handlers(s_window, (WindowHandlers){
    .appear = appear, .disappear = disappear, .unload = unload });
  window_stack_push(s_window, true);
}
