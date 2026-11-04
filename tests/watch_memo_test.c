// Drive the production voice memo state machine (memo_watch.h) against a simulated
// microphone, outbox and phone. Uses tests/memo_stub/pebble.h, not the SDK.
#include <pebble.h>
#include <assert.h>
#include <math.h>
#include "../src/c/protocol.h"

#define UI_HEADER_HEIGHT 24
#define UI_HINT_HEIGHT 20
#define UI_MARGIN 8
#define UI_ACCENT GColorCobaltBlue

typedef struct {
  uint32_t capture_id, generation, total_bytes;
  uint8_t status, error_code;
} InboundTransfer;
static struct { bool active; } s_outbound;
static bool s_exiting, s_app_message_open = true;
static uint32_t s_capture_counter = 100u, s_transfer_counter = 5000u;
static bool storage_next_capture_id(uint32_t *capture_id) { *capture_id = ++s_capture_counter; return true; }
static uint32_t next_transfer_id(void) { return ++s_transfer_counter; }

struct DictionaryIterator {
  uint32_t kind, transfer, capture, offset, total, crc, flags, version;
  uint8_t payload[1024];
  uint16_t length;
};
static bool read_u32(DictionaryIterator *iter, uint8_t key, uint32_t *value, bool required) {
  (void)required;
  if (key != HERMES_KEY_TRANSFER_ID) return false;
  *value = iter->transfer;
  return true;
}

#include "../src/c/memo_watch.h"
static uint32_t speaker_stops;
static void memo_quiet_speaker(void) { speaker_stops++; }

// ---- Simulated platform -------------------------------------------------------------
// The firmware side of mic_shim.h: a function table reached through the app header.
static MemoFirmwareMicHandlers mic;
static bool mic_active;
static MicCaptureStartResult mic_result = MicCaptureStartOk;
static void *firmware_table[683];
const PebbleProcessInfo __pbl_app_info = {
  .header = "PBLAPP", .sym_table_addr = offsetof(PebbleProcessInfo, loader_table), .loader_table = firmware_table,
};
#ifdef MEMO_TEST_OLD_FIRMWARE
static WatchInfoVersion firmware = { 4, 42, 0 };
#else
static WatchInfoVersion firmware = { 4, 43, 0 };
#endif
WatchInfoVersion watch_info_get_firmware_version(void) { return firmware; }
static uint32_t firmware_mic_start(MemoFirmwareMicHandlers handlers, void *context) {
  (void)context;
  assert(handlers.data && handlers.stopped && !mic_active);
  if (mic_result != MicCaptureStartOk) return 0xa5a5a500u | mic_result; // Short enum: upper bits are junk.
  mic = handlers;
  mic_active = true;
  return 0x12345600u;
}
static void firmware_mic_stop(void) { mic_active = false; }
static size_t free_heap = 100000u;
static bool connected = true, outbox_busy, in_flight;
static uint32_t outbox_limit = 1024u, sends, vibes;
static DictionaryIterator dict;
static AppTimerCallback timer_callback;
static uint32_t timer_ms;
static int timers_registered;

void layer_mark_dirty(Layer *layer) { (void)layer; }
Layer *layer_create(GRect frame) { (void)frame; return (Layer *)malloc(1); }
void layer_destroy(Layer *layer) { free(layer); }
void layer_set_update_proc(Layer *layer, LayerUpdateProc proc) { (void)layer; (void)proc; }
void layer_add_child(Layer *parent, Layer *child) { (void)parent; (void)child; }
GRect layer_get_bounds(const Layer *layer) { (void)layer; return GRect(0, 0, 200, 228); }
Window *window_create(void) { return (Window *)malloc(1); }
void window_destroy(Window *window) { free(window); }
Layer *window_get_root_layer(const Window *window) { (void)window; return NULL; }
void window_set_click_config_provider(Window *window, ClickConfigProvider provider) { (void)window; (void)provider; }
void window_set_window_handlers(Window *window, WindowHandlers handlers) { (void)window; (void)handlers; }
void window_stack_push(Window *window, bool animated) { (void)window; (void)animated; }
Window *window_stack_pop(bool animated) { (void)animated; return NULL; }
void window_single_click_subscribe(ButtonId button, ClickHandler handler) { (void)button; (void)handler; }
AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback callback, void *data) {
  (void)data; timer_callback = callback; timer_ms = timeout_ms; timers_registered++; return (AppTimer *)1;
}
void app_timer_cancel(AppTimer *timer) { (void)timer; timer_callback = NULL; }
size_t heap_bytes_free(void) { return free_heap; }
// Wall clock for coverage: speak() advances it by each frame's 20 ms (or more when frames drop).
static uint64_t clock_ms = 1700000000000ull;
uint16_t time_ms(time_t *tloc, uint16_t *out_ms) {
  if (tloc) *tloc = (time_t)(clock_ms / 1000u);
  if (out_ms) *out_ms = (uint16_t)(clock_ms % 1000u);
  return (uint16_t)(clock_ms % 1000u);
}
void vibes_short_pulse(void) { vibes++; }
bool connection_service_peek_pebble_app_connection(void) { return connected; }
AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator) {
  if (outbox_busy || in_flight) return APP_MSG_BUSY;
  memset(&dict, 0, sizeof(dict));
  *iterator = &dict;
  return APP_MSG_OK;
}
AppMessageResult app_message_outbox_send(void) { in_flight = true; sends++; return APP_MSG_OK; }
DictionaryResult dict_write_uint8(DictionaryIterator *iter, uint32_t key, uint8_t value) {
  if (key == HERMES_KEY_MESSAGE_KIND) iter->kind = value;
  if (key == HERMES_KEY_FLAGS) iter->flags = value;
  if (key == HERMES_KEY_PROTOCOL_VERSION) iter->version = value;
  return DICT_OK;
}
DictionaryResult dict_write_uint32(DictionaryIterator *iter, uint32_t key, uint32_t value) {
  if (key == HERMES_KEY_TRANSFER_ID) iter->transfer = value;
  if (key == HERMES_KEY_CAPTURE_ID) iter->capture = value;
  if (key == HERMES_KEY_PAGE_OFFSET) iter->offset = value;
  if (key == HERMES_KEY_TOTAL_BYTES) iter->total = value;
  if (key == HERMES_KEY_CONVERSATION_GENERATION) iter->crc = value;
  return DICT_OK;
}
DictionaryResult dict_write_data(DictionaryIterator *iter, uint32_t key, const uint8_t *data, uint16_t size) {
  assert(key == HERMES_KEY_PAYLOAD);
  // Header and eight other tuples take about 80 bytes of the 1,024-byte outbox.
  if (size + 80u > outbox_limit) return DICT_NOT_ENOUGH_STORAGE;
  memcpy(iter->payload, data, size);
  iter->length = size;
  return DICT_OK;
}
GFont fonts_get_system_font(const char *key) { return (GFont)key; }
void graphics_context_set_fill_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_context_set_stroke_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_context_set_text_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_fill_rect(GContext *ctx, GRect rect, uint16_t radius, GCornerMask corners) { (void)ctx; (void)rect; (void)radius; (void)corners; }
void graphics_fill_circle(GContext *ctx, GPoint p, uint16_t radius) { (void)ctx; (void)p; (void)radius; }
void graphics_draw_round_rect(GContext *ctx, GRect rect, uint16_t radius) { (void)ctx; (void)rect; (void)radius; }
void graphics_draw_text(GContext *ctx, const char *text, GFont font, GRect box, GTextOverflowMode mode,
                        GTextAlignment alignment, void *attributes) {
  (void)ctx; (void)font; (void)box; (void)mode; (void)alignment; (void)attributes;
  assert(text != NULL && strlen(text) < 200u);
}

// ---- Simulated phone ----------------------------------------------------------------
static uint8_t phone[200000];
static uint32_t phone_length;

/* The phone stores the in-flight block and ACKs it; returns the block size. */
static uint16_t phone_ack(void) {
  assert(in_flight && dict.kind == HERMES_KIND_MEMO_BLOCK && dict.version == HERMES_PROTOCOL_VERSION);
  assert(dict.offset <= phone_length);
  memcpy(phone + dict.offset, dict.payload, dict.length);
  if (dict.offset + dict.length > phone_length) phone_length = dict.offset + dict.length;
  assert(((dict.flags & HERMES_FLAG_MORE) != 0) == (dict.offset + dict.length < dict.total));
  in_flight = false;
  uint16_t length = dict.length;
  assert(memo_on_sent(&dict));
  return length;
}

static void phone_nack(void) { assert(in_flight); in_flight = false; assert(memo_on_failed(&dict)); }

static InboundTransfer receipt(bool ok) {
  return (InboundTransfer){ .capture_id = s_memo.capture, .generation = s_memo.crc, .total_bytes = s_memo.length,
    .status = ok ? HERMES_STATUS_NOTE_SAVED : HERMES_STATUS_FAILED, .error_code = ok ? 0u : HERMES_ERROR_MALFORMED };
}

static void fire_timer(void) {
  assert(timer_callback != NULL);
  AppTimerCallback callback = timer_callback;
  timer_callback = NULL;
  callback(NULL);
}

/* Delivers one frame per frame_ms of wall clock: 20 ms is real time, more means drops. */
static void speak_paced(uint32_t frames, uint32_t frame_ms, int16_t dc) {
  int16_t frame[320];
  static uint32_t phase;
  for (uint32_t f = 0; f < frames && s_memo.state == MEMO_RECORDING; f++) {
    for (int i = 0; i < 320; i++, phase++) frame[i] = (int16_t)(dc + 9000.0 * sin(phase * 0.17));
    clock_ms += frame_ms;
    mic.data(frame, 320u, NULL);
  }
}
static void speak(uint32_t frames) { speak_paced(frames, 20u, 0); }

static void draw(void) { memo_draw(NULL, NULL); }

static void record(uint32_t frames) {
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_RECORDING && mic_active);
  draw();
  speak(frames);
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_READY && !mic_active && s_memo.buffer && s_memo.length > MEMO_HEADER);
  assert(s_memo.crc == memo_crc32(s_memo.buffer, s_memo.length));
  draw();
}

int main(void) {
  firmware_table[MEMO_MIC_SLOT_START] = (void *)firmware_mic_start;
  firmware_table[MEMO_MIC_SLOT_STOP] = (void *)firmware_mic_stop;
#ifdef MEMO_TEST_OLD_FIRMWARE
  // Firmware 4.42's table ends before these slots: never touch them, explain instead.
  memo_open();
  memo_load(NULL);
  draw();
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !mic_active && !s_memo.buffer && strstr(s_memo.status, "4.43"));
  memo_shutdown();
  printf("Voice memo on firmware 4.42: microphone slots untouched, update message shown\n");
  return 0;
#endif
  assert(memo_mic_available());
  memo_open();
  memo_load(NULL);
  draw();

  // Happy path: 3 s of speech, ACK-paced 768-byte blocks, receipt frees the clip.
  record(150u);
  assert(speaker_stops == 1u); // Reply playback is stopped before the microphone opens.
  assert(s_memo.encoder.samples == 150u * 160u - 3u);
  assert(s_memo.encoder.coverage >= 99u && s_memo.buffer[7] == s_memo.encoder.coverage);
  assert(strstr(s_memo.status, "missed") == NULL);
  uint32_t expected = s_memo.length, crc = s_memo.crc, capture = s_memo.capture;
  static uint8_t original[200000];
  memcpy(original, s_memo.buffer, expected);
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_SENDING && in_flight && dict.offset == 0u && dict.length == MEMO_BLOCK_BYTES);
  assert(dict.capture == capture && dict.crc == crc && dict.total == expected);
  assert(strcmp(memo_subtitle(), "Sending… 0%") == 0);
  uint32_t blocks = 0;
  while (s_memo.state == MEMO_SENDING) { phone_ack(); blocks++; draw(); }
  assert(blocks == (expected + MEMO_BLOCK_BYTES - 1u) / MEMO_BLOCK_BYTES);
  assert(s_memo.state == MEMO_WAIT_RECEIPT && phone_length == expected && memcmp(phone, original, expected) == 0);
  InboundTransfer wrong = receipt(true);
  wrong.generation ^= 1u;
  memo_receipt(&wrong);
  assert(s_memo.state == MEMO_WAIT_RECEIPT);
  InboundTransfer ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT && s_memo.buffer == NULL && vibes == 1u);
  draw();
  memo_receipt(&ok); // Duplicate receipt is harmless.
  assert(s_memo.state == MEMO_SENT);

  // A missing receipt resends the final block; the phone's repeated receipt completes it.
  memo_select(NULL, NULL);
  speak(60u);
  memo_select(NULL, NULL);
  assert(s_memo.capture == capture + 1u);
  memo_select(NULL, NULL);
  phone_length = 0u;
  uint32_t final_offset = 0u;
  while (s_memo.state == MEMO_SENDING) { final_offset = dict.offset; phone_ack(); }
  assert(s_memo.state == MEMO_WAIT_RECEIPT && timer_ms == MEMO_RECEIPT_TIMEOUT_MS);
  fire_timer();
  assert(s_memo.state == MEMO_SENDING && in_flight && dict.offset == final_offset);
  phone_ack();
  ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT);

  // A receipt that overtakes the final ACK completes the memo; the late ACK is ignored.
  memo_select(NULL, NULL);
  speak(30u);
  memo_select(NULL, NULL);
  memo_select(NULL, NULL);
  while (dict.offset + dict.length < dict.total) phone_ack();
  ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT && !s_memo.in_flight);
  in_flight = false;
  assert(!memo_on_sent(&dict));

  // NACKs back off, shrink blocks after repeated refusals, then give up with the clip kept.
  memo_select(NULL, NULL);
  speak(100u);
  memo_select(NULL, NULL);
  memo_select(NULL, NULL);
  phone_nack();
  assert(s_memo.block == MEMO_BLOCK_BYTES && timer_ms == 500u);
  fire_timer();
  phone_nack();
  assert(s_memo.block == MEMO_BLOCK_BYTES / 2u);
  fire_timer();
  assert(in_flight && dict.length == MEMO_BLOCK_BYTES / 2u && dict.offset == 0u);
  phone_ack();
  assert(s_memo.retries == 0u && s_memo.offset == MEMO_BLOCK_BYTES / 2u);
  for (int i = 0; i <= (int)MEMO_MAX_RETRIES; i++) { phone_nack(); if (s_memo.state == MEMO_SENDING) fire_timer(); }
  assert(s_memo.state == MEMO_FAILED && s_memo.buffer != NULL && s_memo.block == MEMO_MIN_BLOCK_BYTES);
  draw();
  // Retry restarts from zero at full block size; a checksum rejection is reported.
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_SENDING && dict.offset == 0u && dict.length == MEMO_BLOCK_BYTES);
  while (s_memo.state == MEMO_SENDING) phone_ack();
  InboundTransfer bad = receipt(false);
  memo_receipt(&bad);
  assert(s_memo.state == MEMO_FAILED && s_memo.buffer != NULL);
  // Delete needs a confirming second press; UP cancels.
  memo_down(NULL, NULL);
  assert(s_memo.confirm_delete && s_memo.buffer);
  draw();
  memo_up(NULL, NULL);
  assert(!s_memo.confirm_delete && s_memo.buffer);
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && s_memo.buffer == NULL);

  // The main transport has priority; a busy outbox and a small host outbox are tolerated.
  record(40u);
  s_outbound.active = true;
  memo_select(NULL, NULL);
  assert(!in_flight && timer_ms == MEMO_YIELD_MS);
  s_outbound.active = false;
  outbox_busy = true;
  fire_timer();
  assert(!in_flight && timer_ms == MEMO_YIELD_MS);
  outbox_busy = false;
  outbox_limit = 512u;
  fire_timer();
  assert(!in_flight && s_memo.block == MEMO_BLOCK_BYTES / 2u);
  fire_timer();
  assert(in_flight && dict.length == 384u);
  while (s_memo.state == MEMO_SENDING) phone_ack();
  ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT);
  outbox_limit = 1024u;

  // Dropped frames: only one in five arrives, so the memo says it has gaps.
  memo_select(NULL, NULL);
  speak_paced(50u, 100u, 0);
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_READY && s_memo.encoder.coverage >= 19u && s_memo.encoder.coverage <= 20u);
  assert(s_memo.buffer[7] == s_memo.encoder.coverage && strstr(s_memo.status, "missed audio: only"));
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);

  // The level meter ignores a DC offset: silence at +12000 reads empty, speech reads high.
  memo_select(NULL, NULL);
  int16_t still[320];
  for (int i = 0; i < 320; i++) still[i] = 12000;
  for (int i = 0; i < 20; i++) mic.data(still, 320u, NULL);
  assert(s_memo.level == 0u);
  speak_paced(5u, 20u, 12000);
  assert(s_memo.level >= 10u);
  memo_down(NULL, NULL); // Ignored while recording.
  memo_select(NULL, NULL);
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE);

  // Disconnected: fail fast, keep the clip.
  record(40u);
  connected = false;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_FAILED && s_memo.buffer);
  connected = true;
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);

  // Microphone refusals leave nothing allocated and explain why.
  mic_result = MicCaptureStartErrPermissionDenied;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer && strstr(s_memo.status, "Microphone not allowed"));
  mic_result = MicCaptureStartOk;

  // Memory: the buffer follows free heap; too little refuses; a full buffer stops itself.
  free_heap = MEMO_HEAP_RESERVE + 1000u;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer && strstr(s_memo.status, "memory"));
  free_heap = MEMO_HEAP_RESERVE + MEMO_HEADER + 4u * MEMO_BYTES_PER_SECOND;
  memo_select(NULL, NULL);
  assert(s_memo.capacity == MEMO_HEADER + 4u * MEMO_BYTES_PER_SECOND);
  speak(1000u);
  assert(s_memo.state == MEMO_READY && s_memo.length == s_memo.capacity && !mic_active && vibes == 5u);
  assert(memo_encoder_seconds(&s_memo.encoder) == 4u);
  free_heap = 100000u;
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);

  // Interruptions keep what was recorded; revocation deletes it; too-short clips are dropped.
  memo_select(NULL, NULL);
  speak(50u);
  mic_active = false;
  mic.stopped(0xffffff00u | MicCaptureStopReasonInterrupted, NULL);
  assert(s_memo.state == MEMO_READY && s_memo.buffer && strstr(s_memo.status, "interrupted"));
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);
  memo_select(NULL, NULL);
  speak(50u);
  mic_active = false;
  mic.stopped(MicCaptureStopReasonPermissionRevoked, NULL);
  mic.stopped(MicCaptureStopReasonFocusLost, NULL); // A late callback after the clip is gone is ignored.
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer);
  memo_select(NULL, NULL);
  speak(10u);
  memo_back(NULL, NULL); // Back while recording stops instead of leaving.
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer && strstr(s_memo.status, "Too short"));
  memo_select(NULL, NULL);
  speak(50u);
  memo_disappear(NULL);
  assert(s_memo.state == MEMO_READY && !mic_active);

  memo_unload(NULL);
  memo_shutdown();
  assert(s_memo.buffer == NULL && s_memo_window == NULL);
  printf("Voice memo watch flow: record, ACK-paced %u-byte blocks, receipts, resend, backoff, "
         "block shrink, priority, memory bounds, mic refusals, interruptions, coverage and DC-free meter passed\n", MEMO_BLOCK_BYTES);
  return 0;
}
