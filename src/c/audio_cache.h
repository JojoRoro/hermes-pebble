#ifndef HERMES_AUDIO_CACHE_H
#define HERMES_AUDIO_CACHE_H

// A temporary spool, never a resumable playback record. These keys are separate
// from settings, captures and ink. The marker is written before any audio page
// so an interrupted upload can be erased on the next launch.
#define AUDIO_CACHE_META 1000u
#define AUDIO_CACHE_BASE 1001u
#define AUDIO_CACHE_PAGE 256u
#define AUDIO_CACHE_MAX_PAGES ((HERMES_AUDIO_REPLY_MAX_BYTES + AUDIO_CACHE_PAGE - 1u) / AUDIO_CACHE_PAGE)
static uint32_t s_audio_cache_pages;

static void audio_cache_recover(void) {
  uint32_t pages = 0u;
  if (persist_exists(AUDIO_CACHE_META)) {
    if (persist_read_data(AUDIO_CACHE_META, &pages, sizeof(pages)) != (int)sizeof(pages) ||
        pages == 0u || pages > AUDIO_CACHE_MAX_PAGES) pages = AUDIO_CACHE_MAX_PAGES;
  }
  s_audio_cache_pages = pages;
}

static bool audio_cache_prepare(uint32_t total) {
  if (persist_get_max_size() < total + 8192u) return false;
  uint32_t pages = (total + AUDIO_CACHE_PAGE - 1u) / AUDIO_CACHE_PAGE;
  // A new request can replace an old cache while its cleanup is still running.
  if (pages < s_audio_cache_pages) pages = s_audio_cache_pages;
  if (persist_write_data(AUDIO_CACHE_META, &pages, sizeof(pages)) != (int)sizeof(pages)) return false;
  s_audio_cache_pages = pages;
  return true;
}

static bool audio_cache_write(uint32_t offset, const uint8_t *data, uint32_t length) {
  for (uint32_t used = 0u; used < length; used += AUDIO_CACHE_PAGE) {
    uint32_t count = length - used;
    if (count > AUDIO_CACHE_PAGE) count = AUDIO_CACHE_PAGE;
    if (persist_write_data(AUDIO_CACHE_BASE + (offset + used) / AUDIO_CACHE_PAGE,
                           data + used, count) != (int)count) return false;
  }
  return true;
}

static bool audio_cache_read(uint32_t offset, uint8_t *data, uint32_t length) {
  for (uint32_t used = 0u; used < length; used += AUDIO_CACHE_PAGE) {
    uint32_t count = length - used;
    if (count > AUDIO_CACHE_PAGE) count = AUDIO_CACHE_PAGE;
    if (persist_read_data(AUDIO_CACHE_BASE + (offset + used) / AUDIO_CACHE_PAGE,
                          data + used, count) != (int)count) return false;
  }
  return true;
}

static void audio_cache_cleanup(void) {
  // Bound flash work per event so Back and the rest of the UI remain responsive.
  for (uint8_t i = 0u; i < 16u && s_audio_cache_pages != 0u; i++) {
    uint32_t key = AUDIO_CACHE_BASE + s_audio_cache_pages - 1u;
    persist_delete(key);
    if (persist_exists(key)) return; // Retain the marker and retry a failed erase.
    s_audio_cache_pages--;
  }
  if (s_audio_cache_pages == 0u) persist_delete(AUDIO_CACHE_META);
}
#endif
