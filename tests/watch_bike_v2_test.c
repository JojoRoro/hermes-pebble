// Synthetic adversarial fixtures, not recordings or a field accuracy claim.
#include "../src/c/bike_v2.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

typedef struct { BikeModel old; BikeV2 revised; BikeDesk desk; uint32_t ms; } Test;
static void sample(Test *t, int16_t x, int16_t y, int16_t z, bool haptic, bool walking) {
  t->ms += 20;
  bike_sample(&t->old, x, y, z, t->ms, haptic);
  bike_v2_sample(&t->revised, &t->desk, x, y, z, t->ms, haptic, walking);
  assert(t->revised.score <= 95u);
}

enum Signal { STILL, NOISE, WALK, ROAD, TYPING, BURSTS, SHOCKS, ALTERNATING };
static void feed(Test *t, enum Signal kind, int count, bool rotate, bool walking) {
  for (int i = 0; i < count; i++) {
    double phase = (t->ms / 20u) * 6.283185307179586 / 50.0;
    int x = 0, y = 0;
    switch (kind) {
      case STILL: break;
      case NOISE: x = (i * 17 % 11) - 5; break;
      case WALK: x = 600 * sin(2 * phase); break;
      case ROAD: x = 170 * sin(9 * phase) + 50 * sin(13 * phase); y = 90 * sin(7 * phase); break;
      // Fast keystroke-like impacts; intentionally adversarial to v1's average.
      case TYPING: x = i % 6 == 0 ? 260 : i % 6 == 1 ? -110 : 0; break;
      case BURSTS: x = i % 50 < 25 ? 260 * sin(10 * phase) : 0; break;
      case SHOCKS: x = i % 200 == 0 ? 1800 : 0; break;
      case ALTERNATING: x = i % 2 ? 500 : -500; break;
    }
    sample(t, rotate ? 1000 : x, rotate ? x : y, rotate ? y : 1000, false, walking);
  }
}

int main(void) {
  Test idle = {0}, noise = {0}, walk = {0}, road = {0}, turned = {0}, typing = {0}, burst = {0}, shock = {0}, alternating = {0};
  feed(&idle, STILL, 1201, false, false);
  feed(&noise, NOISE, 1201, false, false);
  feed(&walk, WALK, 1201, false, false);
  feed(&road, ROAD, 401, false, false);
  assert(road.revised.ready && !road.revised.likely && road.revised.score < 70);
  feed(&road, ROAD, 800, false, false);
  feed(&turned, ROAD, 1201, true, false);
  feed(&typing, TYPING, 1201, false, false);
  feed(&burst, BURSTS, 1201, false, false);
  feed(&shock, SHOCKS, 1201, false, false);
  feed(&alternating, ALTERNATING, 1201, false, false);
  printf("Synthetic scores new/old: road %u/%u, typing %u/%u, bursts %u/%u, walking %u/%u\n",
    road.revised.score, bike_probability(&road.old, 70, 0), typing.revised.score, bike_probability(&typing.old, 70, 0),
    burst.revised.score, bike_probability(&burst.old, 70, 0), walk.revised.score, bike_probability(&walk.old, 70, 0));
  assert(idle.revised.ready && idle.revised.score == 0 && noise.revised.score == 0);
  assert(walk.revised.score < 15 && shock.revised.score < 15);
  assert(typing.old.motion_score >= 40 && typing.revised.score < 25);
  assert(burst.old.motion_score >= 30 && burst.revised.score < 35);
  assert(alternating.revised.score <= 30);
  assert(road.revised.score >= 70 && road.revised.likely);
  assert(road.revised.score == turned.revised.score);
  assert(memcmp(road.revised.features, turned.revised.features, sizeof(road.revised.features)) == 0);
  // HR is absent from v2 inputs; a fit rider has the exact same path as anyone else.
  assert(bike_probability(&road.old, 150, 0) > bike_probability(&road.old, 70, 0));
  feed(&road, STILL, 401, false, false);
  assert(!road.revised.likely && road.revised.score < 15);
  feed(&turned, ROAD, 401, true, true);
  assert(!turned.revised.likely && turned.revised.score < 30);

  // Optional personalized suppression learns a feature envelope, not a BPM or
  // vibration threshold. Even continuous desk-like vibration can be vetoed.
  Test learned = {0};
  bike_desk_begin(&learned.desk);
  feed(&learned, ROAD, 801, false, false);
  assert(learned.desk.learning && learned.desk.windows == 4 && learned.revised.score == 0);
  feed(&learned, ROAD, 400, false, false);
  assert(learned.desk.learned && !learned.desk.learning && learned.revised.desk_match && learned.revised.score <= 20);
  memset(&learned.revised, 0, sizeof(learned.revised));
  feed(&learned, ROAD, 1201, false, false);
  assert(learned.revised.score <= 20); // Reset preserves the session baseline.
  memset(&learned.desk, 0, sizeof(learned.desk));
  feed(&learned, ROAD, 1200, false, false);
  assert(learned.revised.likely); // Clear removes the veto.
  uint16_t different[BIKE_FEATURES] = {100, 25, 8, 3, 5};
  bike_desk_begin(&learned.desk);
  for (int i = 0; i < 5; i++) assert(bike_desk_observe(&learned.desk, different));
  different[0] = 200;
  assert(!bike_desk_observe(&learned.desk, different));

  // Invalid timing/haptics revoke the candidate and cannot finish training.
  bike_desk_begin(&learned.desk);
  feed(&learned, ROAD, 401, false, false);
  assert(learned.desk.windows > 0);
  learned.ms += 1000;
  feed(&learned, ROAD, 1, false, false);
  assert(!learned.revised.ready && learned.desk.windows == 0);
  for (int i = 0; i < 300; i++) bike_v2_sample(&learned.revised, &learned.desk, 300, 0, 1000, learned.ms, false, false);
  assert(!learned.revised.ready && learned.desk.windows == 0);
  sample(&learned, 300, 0, 1000, true, false);
  assert(!learned.revised.ready && !learned.revised.likely);
  feed(&learned, ROAD, 10, false, false);
  assert(!learned.revised.seeded);
  Test wrap = {.ms = UINT32_MAX - 1000};
  feed(&wrap, ROAD, 1201, false, false);
  assert(wrap.revised.ready && wrap.revised.likely);
  for (int i = 0; i < 1200; i++) sample(&wrap, i % 2 ? INT16_MIN : INT16_MAX, INT16_MAX, INT16_MIN, false, false);
  puts("Bike v2 persistence, typing/impact suppression, steps, calibration, rotation, timing and bounds passed");
}
