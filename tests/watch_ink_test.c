#define main watch_application_main
#include "../src/c/hermes_pt2.c"
#undef main
#include <assert.h>
#undef fopen
#undef fwrite
#undef fclose

static uint8_t stored[32][256];
static size_t sizes[32];
static int writes_left = -1;
static int timers;
bool persist_exists(uint32_t key) { return key < 32 && sizes[key] != 0; }
int persist_read_data(uint32_t key, void *out, size_t size) {
  if (!persist_exists(key)) return -1;
  size_t count = sizes[key] < size ? sizes[key] : size;
  memcpy(out, stored[key], count); return (int)count;
}
int persist_write_data(uint32_t key, const void *data, size_t size) {
  if (key >= 32 || size > 256 || writes_left == 0) return -1;
  size_t total = size + 32;
  for (int i = 0; i < 32; i++) if ((uint32_t)i != key && sizes[i]) total += sizes[i] + 32;
  if (total > 4096) return -1;
  if (writes_left > 0) writes_left--;
  memcpy(stored[key], data, size); sizes[key] = size; return (int)size;
}
status_t persist_delete(uint32_t key) { if (key < 32) sizes[key] = 0; return 0; }
AppTimer *app_timer_register(uint32_t ms, AppTimerCallback callback, void *context) {
  (void)ms; (void)callback; (void)context; timers++; return (AppTimer *)(uintptr_t)timers;
}
void app_timer_cancel(AppTimer *timer) { (void)timer; }
void layer_mark_dirty(Layer *layer) { (void)layer; }

static void seal(void) {
  memcpy(s_ink, "HIN1", 4); s_ink[4] = INK_WIDTH; s_ink[5] = INK_HEIGHT;
  s_ink[6] = (uint8_t)(s_ink_length - INK_HEADER); s_ink[7] = (s_ink_length - INK_HEADER) >> 8;
  encode_u32(s_ink, 8, 1700000000);
  encode_u32(s_ink, 12, ink_crc(s_ink + INK_HEADER, s_ink_length - INK_HEADER));
}
static void touch(TouchEventType type, int x, int y) {
  TouchEvent event = {.type = type, .x = x + 28, .y = y + 35}; ink_touch(&event, NULL);
}
int main(int argc, char **argv) {
  assert(ink_crc((const uint8_t *)"123456789", 9) == 0xcbf43926u);
  s_screen = HERMES_SCREEN_INK;
  touch(TouchEvent_Touchdown, 20, 20);
  for (int i = 21; i <= 130; i++) touch(TouchEvent_PositionUpdate, i, i);
  touch(TouchEvent_Liftoff, 130, 130);
  assert(s_ink_length == INK_HEADER + 6); // a straight line simplified to endpoints
  touch(TouchEvent_Touchdown, 80, 12); touch(TouchEvent_Liftoff, 80, 12); // second stroke of same letter
  assert(ink_cell_start(s_ink_length) == INK_HEADER);
  seal(); assert(ink_valid(s_ink, s_ink_length));
  if (argc > 1) {
    FILE *f = fopen(argv[1], "wb"); assert(f); assert(fwrite(s_ink, 1, s_ink_length, f) == s_ink_length); fclose(f);
  }
  ink_advance(NULL);
  assert(ink_cell_start(s_ink_length) == s_ink_length);
  ink_undo(NULL, NULL); // reopening preserves both strokes for a dot/crossbar
  assert(ink_cell_start(s_ink_length) == INK_HEADER);
  ink_undo(NULL, NULL); assert(s_ink_length == INK_HEADER + 6);
  ink_space(NULL, NULL); assert(s_ink[s_ink_length - 1] == INK_SPACE);
  ink_undo(NULL, NULL); assert(s_ink_length == INK_HEADER + 6);
  ink_undo(NULL, NULL); assert(s_ink_length == INK_HEADER);

  // Existing maximum transcript and preferences coexist with a full ink note.
  PendingCapture capture = {.operation = HERMES_PENDING_REQUEST, .capture_id = 4, .transfer_id = 8, .length = 1024};
  memset(capture.text, 'a', 1024); assert(storage_save_pending(&capture));
  uint8_t identity[16] = {1};
  assert(persist_write_data(HERMES_STORAGE_KEY_INSTALL, identity, 16) == 16);
  assert(storage_write_u32(HERMES_STORAGE_KEY_COUNTER, 10));
  assert(storage_write_u32(HERMES_STORAGE_KEY_GENERATION, 0));
  assert(storage_write_u32(HERMES_STORAGE_KEY_TOUCH_NAVIGATION, 1));
  s_ink_length = INK_CAPACITY;
  for (uint16_t i = INK_HEADER; i < INK_CAPACITY; i += 2) { s_ink[i] = i % INK_WIDTH; s_ink[i + 1] = 50; }
  s_ink[INK_CAPACITY - 2] = INK_MARKER; s_ink[INK_CAPACITY - 1] = INK_PEN_UP;
  seal(); assert(ink_valid(s_ink, s_ink_length));
  s_ink_capture = 123;
  writes_left = 3;
  assert(!ink_store()); assert(!persist_exists(INK_STORAGE_META));
  ink_load(); assert(!s_ink_saved && !s_ink_corrupt);
  s_ink_length = INK_CAPACITY; writes_left = -1;
  assert(ink_store());
  memset(s_ink, 0, sizeof(s_ink)); ink_load();
  assert(s_ink_saved && !s_ink_corrupt && s_ink_capture == 123 && s_ink_length == INK_CAPACITY);
  assert(!ink_store()); // never overwrite an unacknowledged note
  assert(storage_load_pending() && s_pending.length == 1024);
  stored[INK_STORAGE_BASE][22] ^= 1; ink_load();
  assert(s_ink_corrupt && !s_ink_saved);
  assert(ink_clear_saved());
  assert(storage_load_pending() && s_pending.capture_id == 4);
  // Capacity reserves pen-up; even a final stroke stays structurally valid.
  s_ink_length = INK_CAPACITY - 4; s_ink_stroke_start = s_ink_length; s_ink_pen_down = true;
  ink_add_point(1, 1); ink_add_point(5, 7); ink_end_stroke();
  assert(s_ink_length == INK_CAPACITY && s_ink[INK_CAPACITY - 1] == INK_PEN_UP);
  puts("Ink drawing, simplification, undo, capacity, persistence and recovery checks passed");
}
