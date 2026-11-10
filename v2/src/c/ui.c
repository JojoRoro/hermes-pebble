#include "ui.h"
#include "app.h"

// ---- type and text -------------------------------------------------------------------------

static GFont s_fonts[F_COUNT];

GFont ui_font(FontId id) {
  static const char *const keys[F_COUNT] = {
    FONT_KEY_GOTHIC_14, FONT_KEY_GOTHIC_14_BOLD, FONT_KEY_GOTHIC_18, FONT_KEY_GOTHIC_18_BOLD,
    FONT_KEY_GOTHIC_24, FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_28, FONT_KEY_GOTHIC_28_BOLD,
    FONT_KEY_LECO_20_BOLD_NUMBERS, FONT_KEY_LECO_36_BOLD_NUMBERS,
  };
  if (!s_fonts[id]) s_fonts[id] = fonts_get_system_font(keys[id]);
  return s_fonts[id];
}

void ui_text(GContext *ctx, const char *text, FontId font, GRect box, GColor color,
             GTextAlignment align, GTextOverflowMode overflow) {
  if (!text || !text[0]) return;
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, ui_font(font), box, overflow, align, NULL);
}

int ui_text_height(const char *text, FontId font, int width) {
  if (!text || !text[0]) return 0;
  return graphics_text_layout_get_content_size(text, ui_font(font), GRect(0, 0, width, 4000),
                                               GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
}

int ui_band(GContext *ctx, const char *label) {
  graphics_context_set_fill_color(ctx, C_NIGHT);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, 28), 0, GCornerNone);
  ui_text(ctx, label, F18B, GRect(10, 1, SCREEN_W - 20, 22), C_GOLD, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  return 28;
}

void ui_age(char *out, size_t size, int32_t seconds) {
  if (seconds < 60) snprintf(out, size, "now");
  else if (seconds < 3600) snprintf(out, size, "%d min", (int)(seconds / 60));
  else if (seconds < 86400) snprintf(out, size, "%d h", (int)(seconds / 3600));
  else snprintf(out, size, "%d d", (int)(seconds / 86400));
}

// ---- icons ---------------------------------------------------------------------------------

static void line(GContext *ctx, int x0, int y0, int x1, int y1) {
  graphics_draw_line(ctx, GPoint(x0, y0), GPoint(x1, y1));
}

void ui_icon(GContext *ctx, Icon icon, GPoint c, GColor color) {
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 3);
  const int x = c.x, y = c.y;
  switch (icon) {
    case ICON_MIC:
      graphics_fill_rect(ctx, GRect(x - 4, y - 11, 9, 15), 4, GCornersAll);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_arc(ctx, GRect(x - 8, y - 9, 17, 17), GOvalScaleModeFitCircle,
                        DEG_TO_TRIGANGLE(90), DEG_TO_TRIGANGLE(270));
      line(ctx, x, y + 8, x, y + 11);
      line(ctx, x - 4, y + 11, x + 4, y + 11);
      break;
    case ICON_RECENT:
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_circle(ctx, c, 9);
      line(ctx, x, y, x, y - 5);
      line(ctx, x, y, x + 4, y + 2);
      break;
    case ICON_MORE:
      graphics_fill_circle(ctx, GPoint(x - 8, y), 2);
      graphics_fill_circle(ctx, GPoint(x, y), 2);
      graphics_fill_circle(ctx, GPoint(x + 8, y), 2);
      break;
    case ICON_MENU:
      line(ctx, x - 7, y - 6, x + 7, y - 6);
      line(ctx, x - 7, y, x + 7, y);
      line(ctx, x - 7, y + 6, x + 7, y + 6);
      break;
    case ICON_SEND:
      line(ctx, x - 8, y, x + 7, y);
      line(ctx, x + 1, y - 6, x + 7, y);
      line(ctx, x + 1, y + 6, x + 7, y);
      break;
    case ICON_UNDO:
      line(ctx, x - 7, y, x + 8, y);
      line(ctx, x - 1, y - 6, x - 7, y);
      line(ctx, x - 1, y + 6, x - 7, y);
      break;
    case ICON_REDO:
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_arc(ctx, GRect(x - 8, y - 8, 17, 17), GOvalScaleModeFitCircle,
                        DEG_TO_TRIGANGLE(40), DEG_TO_TRIGANGLE(330));
      line(ctx, x - 5, y - 8, x - 5, y - 2);
      line(ctx, x - 5, y - 8, x + 1, y - 8);
      break;
    case ICON_CLOSE:
      line(ctx, x - 6, y - 6, x + 6, y + 6);
      line(ctx, x - 6, y + 6, x + 6, y - 6);
      break;
    case ICON_CHECK:
      line(ctx, x - 7, y, x - 2, y + 5);
      line(ctx, x - 2, y + 5, x + 7, y - 5);
      break;
    case ICON_STOP:
      graphics_fill_rect(ctx, GRect(x - 6, y - 6, 13, 13), 2, GCornersAll);
      break;
    case ICON_PLAY:
      graphics_context_set_stroke_width(ctx, 1);
      for (int dy = -7; dy <= 7; dy++) {
        line(ctx, x - 5, y + dy, x - 5 + (7 - abs(dy)) * 12 / 7, y + dy);
      }
      break;
    case ICON_UP:
      line(ctx, x - 6, y + 3, x, y - 3);
      line(ctx, x, y - 3, x + 6, y + 3);
      break;
    case ICON_DOWN:
      line(ctx, x - 6, y - 3, x, y + 3);
      line(ctx, x, y + 3, x + 6, y - 3);
      break;
    case ICON_SPACE:
      line(ctx, x - 8, y - 2, x - 8, y + 5);
      line(ctx, x - 8, y + 5, x + 8, y + 5);
      line(ctx, x + 8, y + 5, x + 8, y - 2);
      break;
    case ICON_NONE:
      break;
  }
  graphics_context_set_stroke_width(ctx, 1);
}

void ui_rail(GContext *ctx, Icon up, Icon select, Icon down, bool dark) {
  const GColor fg = dark ? C_PAPER : C_INK;
  const int x = SCREEN_W - 18;
  if (up != ICON_NONE) ui_icon(ctx, up, GPoint(x, BTN_UP_Y), fg);
  if (down != ICON_NONE) ui_icon(ctx, down, GPoint(x, BTN_DOWN_Y), fg);
  if (select != ICON_NONE) {
    // The primary action leans in from the edge, next to its button.
    graphics_context_set_antialiased(ctx, true);
    graphics_context_set_fill_color(ctx, dark ? C_GOLD : C_NIGHT);
    graphics_fill_circle(ctx, GPoint(SCREEN_W - 12, BTN_SELECT_Y), 27);
    ui_icon(ctx, select, GPoint(x - 2, BTN_SELECT_Y), dark ? C_NIGHT : C_GOLD);
  }
}

// Three feathers: the mark.
void ui_wing(GContext *ctx, GPoint o, GColor color) {
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 3);
  line(ctx, o.x, o.y + 4, o.x + 18, o.y);
  line(ctx, o.x + 2, o.y + 10, o.x + 15, o.y + 7);
  line(ctx, o.x + 4, o.y + 16, o.x + 12, o.y + 14);
  graphics_context_set_stroke_width(ctx, 1);
}

// ---- keys and touch ------------------------------------------------------------------------

static const UiKeys *s_keys;
static const UiTouch *s_touch;
static bool s_touch_on;
static GPoint s_down;
static int16_t s_last_y;
static bool s_dragging;

static void click_up(ClickRecognizerRef r, void *context) {
  const UiKeys *k = context;
  if (k && k->up) k->up();
}
static void click_select(ClickRecognizerRef r, void *context) {
  const UiKeys *k = context;
  if (k && k->select) k->select();
}
static void click_down(ClickRecognizerRef r, void *context) {
  const UiKeys *k = context;
  if (k && k->down) k->down();
}
static void click_back(ClickRecognizerRef r, void *context) {
  const UiKeys *k = context;
  if (k && k->back) k->back();
}
static void click_select_long(ClickRecognizerRef r, void *context) {
  const UiKeys *k = context;
  if (k && k->select_long) k->select_long();
}

static void click_config(void *context) {
  const UiKeys *k = context;
  if (k->repeat) {
    window_single_repeating_click_subscribe(BUTTON_ID_UP, 110, click_up);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 110, click_down);
  } else {
    window_single_click_subscribe(BUTTON_ID_UP, click_up);
    window_single_click_subscribe(BUTTON_ID_DOWN, click_down);
  }
  window_single_click_subscribe(BUTTON_ID_SELECT, click_select);
  if (k->select_long) window_long_click_subscribe(BUTTON_ID_SELECT, 500, click_select_long, NULL);
  if (k->back) window_single_click_subscribe(BUTTON_ID_BACK, click_back);
}

void ui_keys(Window *window, const UiKeys *keys) {
  window_set_click_config_provider_with_context(window, click_config, (void *)keys);
}

static void touch_handler(const TouchEvent *e, void *context) {
  if (s_touch && s_touch->raw) {
    s_touch->raw(e);
    return;
  }
  if (e->non_navigational) return;
  switch (e->type) {
    case TouchEvent_Touchdown:
      s_down = GPoint(e->x, e->y);
      s_last_y = e->y;
      s_dragging = false;
      break;
    case TouchEvent_PositionUpdate:
      if (!s_dragging && abs(e->y - s_down.y) > 10) s_dragging = true;
      if (s_dragging && s_touch && s_touch->drag) s_touch->drag(e->y - s_last_y);
      s_last_y = e->y;
      break;
    case TouchEvent_Liftoff:
      if (s_dragging) break;
      if (e->x - s_down.x > 70 && abs(e->y - s_down.y) < 40) {
        // A swipe to the right goes back.
        if (s_keys && s_keys->back) s_keys->back();
        else window_stack_pop(true);
      } else if (s_down.x >= SCREEN_W - 48 && !(s_touch && s_touch->no_rail) && s_keys) {
        if (s_down.y < 76) { if (s_keys->up) s_keys->up(); }
        else if (s_down.y < 152) { if (s_keys->select) s_keys->select(); }
        else if (s_keys->down) s_keys->down();
      } else if (s_touch && s_touch->tap) {
        s_touch->tap(s_down);
      }
      break;
  }
}

void ui_focus(const UiKeys *keys, const UiTouch *touch) {
  s_keys = keys;
  s_touch = touch;
  const bool want = g_prefs.touch || (touch && touch->always);
  if (want && !s_touch_on) {
    touch_service_subscribe(touch_handler, NULL);
    s_touch_on = true;
  } else if (!want && s_touch_on) {
    touch_service_unsubscribe();
    s_touch_on = false;
  }
}

void ui_blur(void) {
  s_keys = NULL;
  s_touch = NULL;
  if (s_touch_on) {
    touch_service_unsubscribe();
    s_touch_on = false;
  }
}

// ---- list screen ---------------------------------------------------------------------------

// Four rows fill the space under the band exactly, so a row is never cut in half.
#define ROW_H 50
#define BAND_H 28

struct ListScreen {
  Window *window;
  Layer *layer;
  const ListSpec *spec;
  int selected;
  int scroll;
};

static ListScreen *s_list;       // the list on top

static int list_count(ListScreen *l) { return l->spec->count ? l->spec->count() : 0; }

static void list_clamp(ListScreen *l) {
  const int count = list_count(l);
  if (l->selected >= count) l->selected = count - 1;
  if (l->selected < 0) l->selected = 0;
  const int view = SCREEN_H - 28;
  if (l->selected * ROW_H < l->scroll) l->scroll = l->selected * ROW_H;
  if ((l->selected + 1) * ROW_H > l->scroll + view) l->scroll = (l->selected + 1) * ROW_H - view;
  const int max = count * ROW_H - view;
  if (l->scroll > max) l->scroll = max;
  if (l->scroll < 0) l->scroll = 0;
}

static void list_draw(Layer *layer, GContext *ctx) {
  ListScreen *l = *(ListScreen **)layer_get_data(layer);
  graphics_context_set_fill_color(ctx, C_PAPER);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  const int count = list_count(l);
  static ListRow row;
  for (int i = 0; i < count; i++) {
    const int y = 28 + i * ROW_H - l->scroll;
    if (y + ROW_H <= 28 || y >= SCREEN_H) continue;
    memset(&row, 0, sizeof(row));
    l->spec->row(i, &row);
    const bool on = i == l->selected;
    if (on) {
      graphics_context_set_fill_color(ctx, C_NIGHT);
      graphics_fill_rect(ctx, GRect(4, y + 2, SCREEN_W - 9, ROW_H - 4), 8, GCornersAll);
    } else if (i + 1 != l->selected && i + 1 < count) {
      graphics_context_set_stroke_color(ctx, C_LINE);
      graphics_draw_line(ctx, GPoint(14, y + ROW_H - 1), GPoint(SCREEN_W - 14, y + ROW_H - 1));
    }
    const int text_w = SCREEN_W - 28 - (row.has_accent ? 18 : 0);
    const bool one_line = !row.sub[0];
    ui_text(ctx, row.title, F24B, GRect(14, y + (one_line ? 8 : -1), text_w, 30),
            on ? C_PAPER : C_INK, GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
    ui_text(ctx, row.sub, F18, GRect(14, y + 23, text_w, 22), on ? C_HAZE : C_DIM,
            GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
    if (row.has_accent) {
      graphics_context_set_antialiased(ctx, true);
      graphics_context_set_fill_color(ctx, on ? C_PAPER : C_INK);
      graphics_fill_circle(ctx, GPoint(SCREEN_W - 22, y + ROW_H / 2), 8);
      graphics_context_set_fill_color(ctx, row.accent);
      graphics_fill_circle(ctx, GPoint(SCREEN_W - 22, y + ROW_H / 2), 6);
    }
  }
  if (count == 0) {
    ui_text(ctx, l->spec->empty, F24, GRect(16, 84, SCREEN_W - 32, 120), C_DIM,
            GTextAlignmentCenter, GTextOverflowModeWordWrap);
  }
  // More above or below: a thin bar on the right edge says how much and where.
  const int view = SCREEN_H - BAND_H;
  if (count * ROW_H > view) {
    const int total = count * ROW_H;
    const int bar = view * view / total;
    const int pos = BAND_H + (view - bar) * l->scroll / (total - view);
    graphics_context_set_fill_color(ctx, C_LINE);
    graphics_fill_rect(ctx, GRect(SCREEN_W - 3, BAND_H, 2, view), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, C_NIGHT);
    graphics_fill_rect(ctx, GRect(SCREEN_W - 3, pos, 2, bar), 0, GCornerNone);
  }
  ui_band(ctx, l->spec->heading);
}

static void list_up(void) {
  if (!s_list) return;
  if (s_list->selected > 0) s_list->selected--;
  list_clamp(s_list);
  layer_mark_dirty(s_list->layer);
}

static void list_down(void) {
  if (!s_list) return;
  s_list->selected++;
  list_clamp(s_list);
  layer_mark_dirty(s_list->layer);
}

static void list_select(void) {
  if (!s_list || list_count(s_list) == 0) return;
  if (s_list->spec->select) s_list->spec->select(s_list->selected);
}

static void list_tap(GPoint p) {
  if (!s_list || p.y < 28) return;
  const int index = (p.y - 28 + s_list->scroll) / ROW_H;
  if (index >= list_count(s_list)) return;
  s_list->selected = index;
  layer_mark_dirty(s_list->layer);
  list_select();
}

static void list_drag(int dy) {
  if (!s_list) return;
  const int view = SCREEN_H - 28;
  int max = list_count(s_list) * ROW_H - view;
  if (max < 0) max = 0;
  s_list->scroll -= dy;
  if (s_list->scroll < 0) s_list->scroll = 0;
  if (s_list->scroll > max) s_list->scroll = max;
  layer_mark_dirty(s_list->layer);
}

static const UiKeys s_list_keys = { .up = list_up, .select = list_select, .down = list_down, .repeat = true };
static const UiTouch s_list_touch = { .tap = list_tap, .drag = list_drag, .no_rail = true };

static void list_appear(Window *window) {
  s_list = window_get_user_data(window);
  if (s_list->spec->appear) s_list->spec->appear();
  list_clamp(s_list);
  ui_focus(&s_list_keys, &s_list_touch);
}

static void list_disappear(Window *window) {
  ui_blur();
  if (s_list == window_get_user_data(window)) s_list = NULL;
}

static void list_unload(Window *window) {
  ListScreen *l = window_get_user_data(window);
  if (l->spec->unload) l->spec->unload();
  layer_destroy(l->layer);
  window_destroy(l->window);
  free(l);
}

ListScreen *list_push(const ListSpec *spec) {
  ListScreen *l = malloc(sizeof(ListScreen));
  if (!l) return NULL;
  memset(l, 0, sizeof(*l));
  l->spec = spec;
  l->window = window_create();
  window_set_user_data(l->window, l);
  window_set_background_color(l->window, C_PAPER);
  l->layer = layer_create_with_data(GRect(0, 0, SCREEN_W, SCREEN_H), sizeof(ListScreen *));
  *(ListScreen **)layer_get_data(l->layer) = l;
  layer_set_update_proc(l->layer, list_draw);
  layer_add_child(window_get_root_layer(l->window), l->layer);
  ui_keys(l->window, &s_list_keys);
  window_set_window_handlers(l->window, (WindowHandlers){
    .appear = list_appear, .disappear = list_disappear, .unload = list_unload });
  window_stack_push(l->window, true);
  return l;
}

void list_reload(ListScreen *l) {
  if (!l) return;
  list_clamp(l);
  layer_mark_dirty(l->layer);
}

int list_selected(ListScreen *l) { return l ? l->selected : 0; }

// ---- dialog --------------------------------------------------------------------------------

static Window *s_dialog;
static Layer *s_dialog_layer;
static DialogSpec s_spec;
static char s_dialog_label[24], s_dialog_title[48], s_dialog_body[200];
static char s_dialog_select[20], s_dialog_down[20];

static void copy(char *to, size_t size, const char *from) {
  strncpy(to, from ? from : "", size - 1);
  to[size - 1] = 0;
}

#define LEGEND_H 30

// A key to the rail: the same icon as on the button, then what it does. The primary action
// repeats the rail's gold disc.
static void legend(GContext *ctx, int y, Icon icon, const char *label, bool primary) {
  if (!label[0]) return;
  const GPoint c = GPoint(25, y + LEGEND_H / 2);
  graphics_context_set_antialiased(ctx, true);
  if (primary) {
    graphics_context_set_fill_color(ctx, C_GOLD);
    graphics_fill_circle(ctx, c, 13);
  } else {
    graphics_context_set_stroke_color(ctx, GColorCobaltBlue);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, c, 12);
    graphics_context_set_stroke_width(ctx, 1);
  }
  ui_icon(ctx, icon, c, primary ? C_NIGHT : C_PAPER);
  ui_text(ctx, label, F18B, GRect(46, y + 3, SCREEN_W - 46 - RAIL_W + 4, 22), C_PAPER,
          GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
}

static void dialog_draw(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, C_NIGHT);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  const bool has_rail = s_spec.on_select || s_spec.on_down;
  // With a rail, text keeps clear of the select disc.
  const int w = has_rail ? SCREEN_W - 12 - RAIL_W - 10 : SCREEN_W - 24;
  int y = 10;
  if (s_dialog_label[0]) {
    ui_wing(ctx, GPoint(12, y + 2), C_GOLD);
    ui_text(ctx, s_dialog_label, F18B, GRect(36, y - 3, w - 24, 22), C_GOLD, GTextAlignmentLeft,
            GTextOverflowModeTrailingEllipsis);
    y += 26;
  }
  // A title that would wrap steps down a size, leaving the room to the body.
  FontId title_font = F28B;
  int title_h = ui_text_height(s_dialog_title, title_font, w);
  if (title_h > 36) {
    title_font = F24B;
    title_h = ui_text_height(s_dialog_title, title_font, w);
  }
  ui_text(ctx, s_dialog_title, title_font, GRect(12, y - 4, w, title_h + 6), C_PAPER,
          GTextAlignmentLeft, GTextOverflowModeWordWrap);
  y += title_h + 4;
  // The legend's last row lines up with the down button, so the rail's down icon shares its row.
  const int legend_rows = (s_dialog_select[0] ? 1 : 0) + (s_dialog_down[0] ? 1 : 0);
  const int legend_top = BTN_DOWN_Y + LEGEND_H / 2 - legend_rows * LEGEND_H;
  int floor = legend_rows ? legend_top - 4 : SCREEN_H - 8;
  if (s_spec.progress >= 0) {
    floor -= 18;
    graphics_context_set_fill_color(ctx, GColorDukeBlue);
    graphics_fill_rect(ctx, GRect(12, floor + 4, w, 8), 4, GCornersAll);
    graphics_context_set_fill_color(ctx, C_GOLD);
    const int filled = w * (s_spec.progress > 100 ? 100 : s_spec.progress) / 100;
    if (filled > 0) graphics_fill_rect(ctx, GRect(12, floor + 4, filled < 8 ? 8 : filled, 8), 4, GCornersAll);
  }
  ui_text(ctx, s_dialog_body, F18B, GRect(12, y, w, floor - y), C_HAZE, GTextAlignmentLeft,
          GTextOverflowModeTrailingEllipsis);
  int ly = legend_top;
  if (s_dialog_select[0]) { legend(ctx, ly, s_spec.select_icon, s_dialog_select, true); ly += LEGEND_H; }
  if (s_dialog_down[0]) legend(ctx, ly, s_spec.down_icon, s_dialog_down, false);
  if (has_rail) {
    ui_rail(ctx, ICON_NONE, s_spec.on_select ? s_spec.select_icon : ICON_NONE,
            s_spec.on_down ? s_spec.down_icon : ICON_NONE, true);
  }
}

static void dialog_select(void) { if (s_spec.on_select) s_spec.on_select(); }
static void dialog_down(void) { if (s_spec.on_down) s_spec.on_down(); }
static const UiKeys s_dialog_keys = { .select = dialog_select, .down = dialog_down };

static void dialog_appear(Window *w) { ui_focus(&s_dialog_keys, NULL); }
static void dialog_disappear(Window *w) { ui_blur(); }

static void dialog_unload(Window *w) {
  void (*on_close)(void) = s_spec.on_close;
  layer_destroy(s_dialog_layer);
  window_destroy(s_dialog);
  s_dialog = NULL;
  s_dialog_layer = NULL;
  if (on_close) on_close();
}

void dialog_push(const DialogSpec *spec) {
  // A dialog replaced in place is closed as far as its owner is concerned.
  void (*replaced)(void) = s_dialog ? s_spec.on_close : NULL;
  s_spec = *spec;
  copy(s_dialog_label, sizeof(s_dialog_label), spec->label);
  copy(s_dialog_title, sizeof(s_dialog_title), spec->title);
  copy(s_dialog_body, sizeof(s_dialog_body), spec->body);
  copy(s_dialog_select, sizeof(s_dialog_select), spec->on_select ? spec->select_label : "");
  copy(s_dialog_down, sizeof(s_dialog_down), spec->on_down ? spec->down_label : "");
  if (s_dialog) {
    // Already showing: the same window takes the new content.
    layer_mark_dirty(s_dialog_layer);
    if (replaced && replaced != spec->on_close) replaced();
    return;
  }
  s_dialog = window_create();
  window_set_background_color(s_dialog, C_NIGHT);
  s_dialog_layer = layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  layer_set_update_proc(s_dialog_layer, dialog_draw);
  layer_add_child(window_get_root_layer(s_dialog), s_dialog_layer);
  ui_keys(s_dialog, &s_dialog_keys);
  window_set_window_handlers(s_dialog, (WindowHandlers){
    .appear = dialog_appear, .disappear = dialog_disappear, .unload = dialog_unload });
  window_stack_push(s_dialog, true);
}

void dialog_update(const char *title, const char *body, int progress) {
  if (!s_dialog) return;
  if (title) copy(s_dialog_title, sizeof(s_dialog_title), title);
  if (body) copy(s_dialog_body, sizeof(s_dialog_body), body);
  s_spec.progress = progress;
  layer_mark_dirty(s_dialog_layer);
}

bool dialog_open(void) { return s_dialog != NULL; }

void dialog_pop(void) {
  if (s_dialog) window_stack_remove(s_dialog, true);
}
