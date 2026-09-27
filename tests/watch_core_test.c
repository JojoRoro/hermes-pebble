// Host-only tests using the real SDK declarations and production parser/storage.
#define main watch_application_main
#include "../src/c/hermes_pt2.c"
#undef main
#include <assert.h>

static uint8_t stored[32][256];
static size_t sizes[32];
static bool fail_writes;

bool persist_exists(const uint32_t key) { return key < 32 && sizes[key] != 0; }
int persist_read_data(const uint32_t key, void *buffer, const size_t size) {
  if (!persist_exists(key)) return -1;
  size_t count = size < sizes[key] ? size : sizes[key];
  memcpy(buffer, stored[key], count);
  return (int)count;
}
int persist_write_data(const uint32_t key, const void *data, const size_t size) {
  if (fail_writes || key >= 32 || size > 256) return -1;
  memcpy(stored[key], data, size);
  sizes[key] = size;
  return (int)size;
}
status_t persist_delete(const uint32_t key) {
  if (key >= 32) return -1;
  sizes[key] = 0;
  return 0;
}

int main(void) {
  uint32_t value = 999;
  assert(!storage_read_u32(20, &value));
  assert(value == 999);
  fail_writes = true;
  assert(!storage_write_u32(20, 42));
  fail_writes = false;
  assert(storage_write_u32(20, 42));
  assert(storage_read_u32(20, &value) && value == 42);
  sizes[20] = 2;
  assert(!storage_read_u32(20, &value));

  PendingCapture capture = { .operation = HERMES_PENDING_REQUEST, .capture_id = 5,
    .transfer_id = 9, .generation = 0, .length = 1024 };
  memset(capture.text, 'a', capture.length);
  assert(storage_save_pending(&capture));
  assert(storage_load_pending());
  assert(s_pending.capture_id == 5 && s_pending.generation == 0 && s_pending.length == 1024);
  assert(memcmp(s_pending.text, capture.text, 1024) == 0);
  stored[HERMES_STORAGE_KEY_CHUNK_BASE][1] ^= 1;
  assert(!storage_load_pending());
  fail_writes = true;
  assert(!storage_save_pending(&capture));
  fail_writes = false;

  const char *result = "{\"captureId\":5,\"itemId\":2,\"state\":7,\"output\":\"quoted \\\"more\\\":true and \\ud83d\\ude80\",\"more\":false}";
  s_result_more = 0;
  assert(parse_result_payload(result, (uint16_t)strlen(result)));
  assert(strcmp(s_visible_output, "quoted \"more\":true and \xf0\x9f\x9a\x80") == 0);
  assert(s_result_more == 0);
  const char *recent = "{\"items\":[{\"captureId\":5,\"itemId\":2,\"state\":7,\"kind\":1,\"preview\":\"Hello\"}]}";
  assert(parse_recent_payload(recent, (uint16_t)strlen(recent)));
  assert(s_recent_count == 1 && s_recent_items[0].capture_id == 5);
  assert(!utf8_valid((const uint8_t *)"\xc0\x80", 2));
  assert(utf8_valid((const uint8_t *)"\xf0\x9f\x9a\x80", 4));
  uint8_t lengths[HERMES_MAX_CHUNKS];
  uint16_t offsets[HERMES_MAX_CHUNKS];
  uint8_t count;
  uint8_t text[1024];
  for (size_t i = 0; i < sizeof(text); i += 4) memcpy(text + i, "\xf0\x9f\x9a\x80", 4);
  pending_make_chunk_plan(text, sizeof(text), lengths, offsets, &count);
  assert(count == 6);
  size_t total = 0;
  for (uint8_t i = 0; i < count; i++) {
    assert(lengths[i] <= 192 && utf8_valid(text + offsets[i], lengths[i]));
    total += lengths[i];
  }
  assert(total == sizeof(text));
  // Every navigation state must reach the launcher with rapid Back presses,
  // including recovery with a persisted or corrupt pending capture.
  for (uint8_t screen = HERMES_SCREEN_MENU; screen <= HERMES_SCREEN_CONNECTING; screen++) {
    for (uint8_t previous = HERMES_SCREEN_MENU; previous <= HERMES_SCREEN_CONNECTING; previous++) {
      s_screen = screen;
      s_previous_screen = previous;
      s_exiting = false;
      s_stay_on_menu = false;
      s_pending = capture;
      s_storage_corrupt = true;
      s_phone_probe_id = 123;
      s_outbound.active = true;
      s_outbound.kind = HERMES_KIND_SUBMIT_REQUEST;
      bool exited = false;
      for (int press = 0; press < 3 && !exited; press++) exited = navigation_back();
      assert(exited && s_exiting && !s_outbound.active && s_phone_probe_id == 0);
      assert(memcmp(&s_pending, &capture, sizeof(capture)) == 0);
      assert(s_storage_corrupt); // Back never discards recoverable work.
    }
  }
  s_exiting = false;
  s_menu_kind = HERMES_MENU_MAIN;
  s_menu_count = 6;
  assert(menu_row_count() == 6); // Reconnect is the final row, not beyond the scroll limit.
  s_menu_count = 5;
  assert(menu_row_count() == 5); // Pending-draft menu also includes Reconnect.

  // Phone-originated probes prompt a correlated watch handshake, without
  // completing an unrelated request or pretending a transport ACK is success.
  InboundTransfer probe = { .kind = HERMES_KIND_HANDSHAKE_ACK, .transfer_id = 51 };
  s_handshake_ready = false;
  s_outbound = (OutboundTransfer){ .active = true, .kind = HERMES_KIND_SUBMIT_REQUEST,
    .transfer_id = 99, .phase = HERMES_OUT_WAIT_RECEIPT };
  assert(queue_phone_probe(&probe));
  assert(s_phone_probe_id == 51 && s_outbound.active && s_outbound.transfer_id == 99);
  assert(!s_handshake_ready);
  s_outbound.kind = HERMES_KIND_HANDSHAKE;
  s_outbound.phase = HERMES_OUT_WAIT_REPLY;
  assert(queue_phone_probe(&probe));
  assert(!s_outbound.active); // Replace a stuck startup handshake at the next timer tick.
  s_outbound.active = true;
  s_outbound.phase = HERMES_OUT_WAIT_CHUNK;
  assert(queue_phone_probe(&probe) && s_outbound.active); // Never overwrite an in-flight outbox.
  probe.correlation_id = 99;
  assert(!queue_phone_probe(&probe));
  s_outbound.expected_kind = HERMES_KIND_HANDSHAKE_ACK;
  assert(inbound_correlation_matches(&probe));
  probe.correlation_id = 98;
  assert(!inbound_correlation_matches(&probe));
  s_exiting = true;
  probe.correlation_id = 0;
  assert(!queue_phone_probe(&probe));

  puts("Watch storage, parser, UTF-8, Back navigation, menu, and handshake checks passed");
  return 0;
}
