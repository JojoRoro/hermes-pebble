#ifndef HERMES_INK_CORE_H
#define HERMES_INK_CORE_H

/* HIN1: fixed-size coordinates, stroke/character/space markers, no recognition. */
#define INK_CAPACITY 2048u
#define INK_HEADER 16u
#define INK_WIDTH 160u
#define INK_HEIGHT 144u
#define INK_MARKER 255u
#define INK_PEN_UP 0u
#define INK_ADVANCE 1u
#define INK_SPACE 2u
#define INK_STORAGE_META 16u
#define INK_STORAGE_BASE 17u
#define INK_STORAGE_CHUNK 240u
#define INK_STORAGE_CHUNKS ((INK_CAPACITY + INK_STORAGE_CHUNK - 1u) / INK_STORAGE_CHUNK)
#define INK_LOG_TAG 0x48494e31u
#define INK_LOG_ITEM_SIZE 212u

/* At most 3,168 value bytes + conservative 32 bytes/key overhead = 4,000.
 * Includes a full existing transcript, ink, commit records and preferences. */
_Static_assert(INK_CAPACITY + 20u + 1024u + 48u + 28u + 26u * 32u <= 4096u,
               "All watch persistence must fit the 4 KiB allowance");

static uint32_t ink_crc(const uint8_t *data, uint16_t length) {
  uint32_t crc = 0xffffffffu;
  for (uint16_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  return ~crc;
}

static bool ink_valid(const uint8_t *data, uint16_t length) {
  if (length < INK_HEADER + 4u || length > INK_CAPACITY || (length & 1u) ||
      memcmp(data, "HIN1", 4) || data[4] != INK_WIDTH || data[5] != INK_HEIGHT ||
      (uint16_t)(data[6] | ((uint16_t)data[7] << 8)) != length - INK_HEADER) return false;
  uint32_t expected = (uint32_t)data[12] | ((uint32_t)data[13] << 8) |
      ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);
  if (expected != ink_crc(data + INK_HEADER, length - INK_HEADER)) return false;
  bool point = false, open = false;
  for (uint16_t i = INK_HEADER; i < length; i += 2) {
    if (data[i] == INK_MARKER) {
      if (data[i + 1] > INK_SPACE || (open && data[i + 1] != INK_PEN_UP)) return false;
      open = false;
    } else {
      if (data[i] >= INK_WIDTH || data[i + 1] >= INK_HEIGHT) return false;
      open = point = true;
    }
  }
  return point && !open;
}
#endif
