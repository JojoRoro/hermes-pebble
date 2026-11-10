// Drawing kit: palette, type, icons, the button rail, and shared screens.
#pragma once
#include <pebble.h>

// Midnight and courier gold for the voice screens, paper and ink for reading.
#define C_NIGHT GColorOxfordBlue
#define C_GOLD  GColorChromeYellow
#define C_PAPER GColorWhite
#define C_INK   GColorBlack
#define C_DIM   GColorDarkGray       // secondary text on paper
#define C_HAZE  GColorPictonBlue     // secondary text on night
#define C_LINE  GColorLightGray

#define SCREEN_W 200
#define SCREEN_H 228
#define RAIL_W 38
// Where the three right-hand buttons sit beside the screen.
#define BTN_UP_Y 40
#define BTN_SELECT_Y 114
#define BTN_DOWN_Y 188

typedef enum { F14, F14B, F18, F18B, F24, F24B, F28, F28B, F_NUM20, F_NUM36, F_COUNT } FontId;
GFont ui_font(FontId id);

typedef enum {
  ICON_NONE, ICON_MIC, ICON_RECENT, ICON_MORE, ICON_SEND, ICON_REDO, ICON_CLOSE, ICON_CHECK,
  ICON_STOP, ICON_PLAY, ICON_UP, ICON_DOWN, ICON_UNDO, ICON_SPACE, ICON_MENU,
} Icon;

void ui_icon(GContext *ctx, Icon icon, GPoint c, GColor color);
// The three button hints down the right edge. The select action sits in a disc.
void ui_rail(GContext *ctx, Icon up, Icon select, Icon down, bool dark);
void ui_wing(GContext *ctx, GPoint origin, GColor color);
void ui_text(GContext *ctx, const char *text, FontId font, GRect box, GColor color,
             GTextAlignment align, GTextOverflowMode overflow);
int ui_text_height(const char *text, FontId font, int width);
// A heading band across the top of a paper screen. Returns its height.
int ui_band(GContext *ctx, const char *label);
void ui_age(char *out, size_t size, int32_t seconds);

// ---- input -------------------------------------------------------------------------------

typedef struct {
  void (*up)(void);
  void (*select)(void);
  void (*down)(void);
  void (*back)(void);          // NULL: the system pops the window
  void (*select_long)(void);
  bool repeat;                 // up and down repeat while held
} UiKeys;

typedef struct {
  void (*tap)(GPoint p);       // a tap left of the rail
  void (*drag)(int dy);        // vertical finger movement since the last call
  void (*raw)(const TouchEvent *e);   // everything, for drawing
  bool no_rail;                // taps on the right edge are not button presses
  bool always;                 // needs touch even when touch navigation is off
} UiTouch;

void ui_keys(Window *window, const UiKeys *keys);
// Call from a window's appear handler, and ui_blur() from its disappear handler.
void ui_focus(const UiKeys *keys, const UiTouch *touch);
void ui_blur(void);

// ---- list screen ---------------------------------------------------------------------------

typedef struct {
  char title[72];
  char sub[56];
  GColor accent;
  bool has_accent;
} ListRow;

typedef struct {
  const char *heading;
  const char *empty;
  int (*count)(void);
  void (*row)(int index, ListRow *out);
  void (*select)(int index);
  void (*appear)(void);
  void (*unload)(void);
} ListSpec;

typedef struct ListScreen ListScreen;
ListScreen *list_push(const ListSpec *spec);
void list_reload(ListScreen *list);
int list_selected(ListScreen *list);

// ---- dialog --------------------------------------------------------------------------------

typedef struct {
  const char *label;           // small caps line above the title
  const char *title;
  const char *body;
  Icon select_icon, down_icon;
  const char *select_label, *down_label;
  void (*on_select)(void);
  void (*on_down)(void);
  void (*on_close)(void);
  int progress;                // 0..100, or -1 for none
} DialogSpec;

// One dialog at a time. Text is copied.
void dialog_push(const DialogSpec *spec);
void dialog_update(const char *title, const char *body, int progress);
bool dialog_open(void);
void dialog_pop(void);
