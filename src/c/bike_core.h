#ifndef HERMES_BIKE_CORE_H
#define HERMES_BIKE_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Experimental wrist-motion score, not a calibrated statistical probability.
// 50 Hz, non-overlapping four-second windows; no raw samples are retained.
#define BIKE_WINDOW_SAMPLES 200u
typedef struct {
  int32_t gravity[3];
  int16_t previous[3];
  uint32_t last_ms, ignore_until_ms, motion_energy, change_energy;
  uint16_t samples, active_samples, vibration_mg;
  uint8_t fast_percent, motion_score;
  bool seeded, ready, haptic_pause;
} BikeModel;

static uint32_t bike_sqrt(uint32_t value) {
  uint32_t root = 0, bit = 1u << 30;
  while (bit > value) bit >>= 2;
  while (bit) {
    if (value >= root + bit) { value -= root + bit; root = (root >> 1) + bit; }
    else root >>= 1;
    bit >>= 2;
  }
  return root;
}

static uint32_t bike_scale(uint32_t value, uint32_t low, uint32_t high) {
  if (value <= low) return 0;
  if (value >= high) return 100;
  return (value - low) * 100u / (high - low);
}

static uint32_t bike_square(int32_t value) {
  // Clamp outliers and scale before squaring: all 200-sample sums fit uint32_t.
  if (value > 2000) value = 2000;
  if (value < -2000) value = -2000;
  value /= 4;
  return (uint32_t)(value * value);
}

static void bike_sample(BikeModel *model, int16_t x, int16_t y, int16_t z,
                        uint32_t ms, bool did_vibrate) {
  if (did_vibrate) {
    memset(model, 0, sizeof(*model));
    model->haptic_pause = true;
    model->ignore_until_ms = ms + 300u;
    return;
  }
  if (model->haptic_pause) {
    if ((int32_t)(ms - model->ignore_until_ms) < 0) return;
    model->haptic_pause = false;
  }
  if (model->seeded) {
    int32_t elapsed = (int32_t)(ms - model->last_ms);
    if (elapsed == 0) return; // Duplicates cannot inflate the window.
    if (elapsed < 10 || elapsed > 50) memset(model, 0, sizeof(*model));
  }
  const int16_t axis[3] = {x, y, z};
  model->last_ms = ms;
  if (!model->seeded) {
    for (unsigned i = 0; i < 3; i++) model->gravity[i] = model->previous[i] = axis[i];
    model->seeded = true;
    return;
  }
  uint32_t change = 0;
  for (unsigned i = 0; i < 3; i++) {
    model->gravity[i] += ((int32_t)axis[i] - model->gravity[i]) / 32;
    model->motion_energy += bike_square((int32_t)axis[i] - model->gravity[i]);
    change += bike_square((int32_t)axis[i] - model->previous[i]);
    model->previous[i] = axis[i];
  }
  model->change_energy += change;
  if (change >= 40u) model->active_samples++; // About 25 mg vector change.
  if (++model->samples < BIKE_WINDOW_SAMPLES) return;

  uint32_t motion = model->motion_energy / BIKE_WINDOW_SAMPLES;
  uint32_t delta = model->change_energy / BIKE_WINDOW_SAMPLES;
  model->vibration_mg = (uint16_t)bike_sqrt(delta * 16u);
  uint32_t fast = delta * 25u / (motion + 1u);
  model->fast_percent = fast > 100u ? 100u : (uint8_t)fast;
  // Changes between adjacent samples emphasize road buzz; slow arm swings
  // have a small change/motion ratio. Isolated knocks lack sustained activity.
  uint32_t strength = bike_scale(model->vibration_mg, 25u, 160u);
  uint32_t frequency = bike_scale(model->fast_percent, 6u, 28u);
  uint32_t sustained = bike_scale(model->active_samples * 100u / BIKE_WINDOW_SAMPLES, 20u, 65u);
  uint32_t target = strength * frequency * sustained / 10000u * 85u / 100u;
  // Require repeated evidence to rise; let the score fall faster after stopping.
  model->motion_score = target > model->motion_score
      ? (model->motion_score + target + 1u) / 2u
      : (model->motion_score + 2u * target) / 3u;
  model->ready = true;
  model->samples = model->active_samples = 0u;
  model->motion_energy = model->change_energy = 0u;
}

static uint8_t bike_probability(const BikeModel *model, uint16_t bpm, uint32_t age_seconds) {
  if (!model->ready) return 0;
  uint32_t score = model->motion_score;
  // HR only corroborates motion; it cannot identify riding by itself.
  // Full weight for one minute, fading to zero by ten minutes. Unknown age is ignored.
  if (score >= 25u && bpm >= 30u && bpm <= 220u && age_seconds < 600u) {
    uint32_t freshness = age_seconds <= 60u ? 100u : (600u - age_seconds) * 100u / 540u;
    score += bike_scale(bpm, 95u, 145u) * 15u * freshness / 10000u;
  }
  return score > 99u ? 99u : (uint8_t)score;
}
#endif
