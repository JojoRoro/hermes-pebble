#ifndef HERMES_AUDIO_CACHE_H
#define HERMES_AUDIO_CACHE_H

// Migration only: remove leftover v0.1.17 audio pages. New replies never write
// flash. These keys are separate from settings, captures and ink.
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

static void audio_cache_cleanup(void) {
  // Bound flash work per event so Back and the rest of the UI remain responsive.
  if (s_audio_cache_pages != 0u) {
    uint32_t key = AUDIO_CACHE_BASE + s_audio_cache_pages - 1u;
    persist_delete(key);
    if (persist_exists(key)) return; // Retain the marker and retry a failed erase.
    s_audio_cache_pages--;
  }
  if (s_audio_cache_pages == 0u) persist_delete(AUDIO_CACHE_META);
}
#endif
