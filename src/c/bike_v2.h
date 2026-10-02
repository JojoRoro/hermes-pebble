#ifndef HERMES_BIKE_V2_H
#define HERMES_BIKE_V2_H
#include "bike_core.h"

// Conservative, experimental watch-only evidence. These are heuristic scores,
// not probabilities. Keep BikeModel unchanged for the live legacy comparison.
#define BIKE_DESK_WINDOWS 5u
#define BIKE_FEATURES 5u
typedef struct {
  // Vibration, fast fraction, sustained half-seconds, peak/RMS energy, tilt drift.
  uint16_t low[BIKE_FEATURES], high[BIKE_FEATURES];
  uint8_t windows;
  bool learning, learned;
} BikeDesk;

typedef struct {
  int32_t gravity[3], block_gravity[3];
  int16_t previous[3];
  uint32_t last_ms, ignore_until_ms, motion_energy, change_energy;
  uint32_t block_energy, block_peak, tilt_energy;
  uint16_t samples, block_active, crest, features[BIKE_FEATURES];
  uint8_t good_blocks, streak, score, target;
  bool seeded, haptic_pause, ready, desk_match, likely;
} BikeV2;

static void bike_desk_begin(BikeDesk *desk) {
  memset(desk, 0, sizeof(*desk));
  desk->learning = true;
}

static bool bike_desk_observe(BikeDesk *desk, const uint16_t *features) {
  if (desk->learning) {
    for (unsigned i = 0; i < BIKE_FEATURES; i++) {
      if (!desk->windows || features[i] < desk->low[i]) desk->low[i] = features[i];
      if (!desk->windows || features[i] > desk->high[i]) desk->high[i] = features[i];
    }
    if (++desk->windows >= BIKE_DESK_WINDOWS) {
      desk->learning = false;
      desk->learned = true;
    }
    return true;
  }
  if (!desk->learned) return false;
  const uint16_t margins[BIKE_FEATURES] = {20, 8, 1, 2, 30};
  for (unsigned i = 0; i < BIKE_FEATURES; i++) {
    uint32_t margin = desk->high[i] / 5u + margins[i];
    if ((uint32_t)features[i] + margin < desk->low[i] ||
        features[i] > desk->high[i] + margin) return false;
  }
  return true;
}

// Call only on completed, timestamp-validated four-second windows. Steps are a
// negative cue only: absence of steps cannot establish riding.
static void bike_v2_finish(BikeV2 *m, BikeDesk *desk, bool walking) {
  uint32_t motion = m->motion_energy / BIKE_WINDOW_SAMPLES;
  uint32_t delta = m->change_energy / BIKE_WINDOW_SAMPLES;
  uint32_t fast = delta * 25u / (motion + 1u);
  m->features[0] = (uint16_t)bike_sqrt(delta * 16u);
  m->features[1] = fast > 100u ? 100u : fast;
  m->features[2] = m->good_blocks;
  m->features[3] = m->crest / 8u;
  m->features[4] = (uint16_t)bike_sqrt(m->tilt_energy * 16u);
  m->desk_match = bike_desk_observe(desk, m->features);

  // Quiet gaps / brief key impacts no longer pool into a high window average.
  // Strength alone never suffices. Stable grip is a veto, not positive evidence.
  uint32_t strength = bike_scale(m->features[0], 30u, 125u);
  uint32_t frequency = bike_scale(m->features[1], 8u, 24u);
  uint32_t coverage = bike_scale(m->good_blocks, 4u, 8u);
  uint32_t steady = 100u - bike_scale(m->features[4], 100u, 350u);
  uint32_t target = strength * frequency / 100u * coverage / 100u * steady / 100u;
  target = target * 95u / 100u;
  if (m->features[1] > 85u && target > 30u) target = 30u; // Alternating shocks / aliasing.
  if (walking && target > 20u) target = 20u;
  if (m->desk_match && target > 20u) target = 20u;
  if (desk->learning) target = 0;
  m->target = target;
  if (target >= 70u) { if (m->streak < 3u) m->streak++; }
  else m->streak = 0;
  // Three consecutive strong windows before even a candidate may reach 70.
  if (m->streak < 3u && target > 59u) target = 59u;
  m->score = target > m->score ? (m->score + target + 1u) / 2u
                             : (m->score + 2u * target) / 3u;
  m->likely = m->streak >= 3u && m->score >= 70u;
  m->ready = true;
  m->samples = m->good_blocks = m->crest = 0;
  m->motion_energy = m->change_energy = m->tilt_energy = 0;
}

static void bike_v2_sample(BikeV2 *m, BikeDesk *desk, int16_t x, int16_t y, int16_t z,
                           uint32_t ms, bool did_vibrate, bool walking) {
  if (did_vibrate) {
    memset(m, 0, sizeof(*m));
    if (desk->learning) bike_desk_begin(desk);
    m->haptic_pause = true;
    m->ignore_until_ms = ms + 300u;
    return;
  }
  if (m->haptic_pause) {
    if ((int32_t)(ms - m->ignore_until_ms) < 0) return;
    m->haptic_pause = false;
  }
  if (m->seeded) {
    int32_t elapsed = (int32_t)(ms - m->last_ms);
    if (!elapsed) return;
    if (elapsed < 10 || elapsed > 50) {
      memset(m, 0, sizeof(*m));
      if (desk->learning) bike_desk_begin(desk);
    }
  }
  const int16_t axis[3] = {x, y, z};
  m->last_ms = ms;
  if (!m->seeded) {
    for (unsigned i = 0; i < 3; i++) m->gravity[i] = m->previous[i] = m->block_gravity[i] = axis[i];
    m->seeded = true;
    return;
  }
  uint32_t change = 0;
  for (unsigned i = 0; i < 3; i++) {
    m->gravity[i] += ((int32_t)axis[i] - m->gravity[i]) / 32;
    m->motion_energy += bike_square((int32_t)axis[i] - m->gravity[i]);
    change += bike_square((int32_t)axis[i] - m->previous[i]);
    m->previous[i] = axis[i];
  }
  m->change_energy += change;
  m->block_energy += change;
  if (change > m->block_peak) m->block_peak = change;
  if (change >= 40u) m->block_active++;
  if (++m->samples % 25u == 0) {
    uint32_t mean = m->block_energy / 25u;
    uint32_t crest = m->block_peak / (mean + 1u);
    m->crest += crest;
    if (m->block_active >= 15u && mean >= 76u && crest <= 6u) m->good_blocks++;
    uint32_t tilt = 0;
    for (unsigned i = 0; i < 3; i++) {
      tilt += bike_square(m->gravity[i] - m->block_gravity[i]);
      m->block_gravity[i] = m->gravity[i];
    }
    if (tilt > m->tilt_energy) m->tilt_energy = tilt;
    m->block_energy = m->block_peak = m->block_active = 0;
  }
  if (m->samples == BIKE_WINDOW_SAMPLES) bike_v2_finish(m, desk, walking);
}
#endif
