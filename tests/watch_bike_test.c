// Deterministic sensor fixtures test behavior, not field accuracy/calibration.
#include "../src/c/bike_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static uint32_t clock_ms = 1000;
static void motion(BikeModel *model, int seconds, double hz, int amplitude, int rotation, bool haptic) {
  for (int i = 0; i < seconds * 50; i++) {
    int16_t signal = (int16_t)(amplitude * sin(6.283185307179586 * hz * i / 50));
    clock_ms += 20;
    bike_sample(model, rotation ? 1000 : signal, rotation ? signal : 0,
      rotation ? 0 : 1000, clock_ms, haptic);
  }
}

int main(void) {
  BikeModel still = {0}, walk = {0}, ride = {0}, turned = {0}, noise = {0}, haptic = {0};
  motion(&still, 24, 0, 0, 0, false);
  motion(&noise, 24, 13, 5, 0, false);
  motion(&walk, 24, 2, 600, 0, false);
  motion(&ride, 24, 10, 200, 0, false);
  motion(&turned, 24, 10, 200, 1, false);
  motion(&haptic, 24, 10, 200, 0, true);
  assert(still.ready && still.motion_score == 0);
  assert(noise.motion_score == 0 && walk.motion_score < 15);
  assert(ride.ready && ride.motion_score >= 75);
  assert(ride.motion_score == turned.motion_score); // Wrist-axis permutation is immaterial.
  assert(!haptic.ready && haptic.motion_score == 0);
  assert(bike_probability(&still, 170, 0) == 0); // Exercise HR alone cannot imply riding.
  unsigned base = bike_probability(&ride, 0, 0);
  unsigned fresh = bike_probability(&ride, 150, 0);
  unsigned old = bike_probability(&ride, 150, 360);
  assert(fresh > old && old > base && fresh <= 99);
  assert(bike_probability(&ride, 150, 600) == base);
  assert(bike_probability(&ride, 150, UINT32_MAX) == base);
  assert(bike_probability(&ride, 250, 0) == base);
  assert(bike_probability(&ride, 70, 0) == base); // Coasting is not penalized by low HR.
  clock_ms = ride.last_ms;
  motion(&ride, 12, 0, 0, 0, false);
  assert(ride.motion_score < 10);

  BikeModel knock = {0};
  for (int i = 0; i < 1200; i++) {
    clock_ms += 20;
    bike_sample(&knock, i == 201 ? 1800 : 0, 0, 1000, clock_ms, false);
  }
  assert(knock.motion_score == 0);
  // A timing gap invalidates old evidence; duplicate timestamps cannot create evidence.
  BikeModel gap = turned;
  bike_sample(&gap, 0, 0, 1000, gap.last_ms + 1000, false);
  assert(!gap.ready && gap.motion_score == 0);
  for (int i = 0; i < 2000; i++) bike_sample(&gap, i % 2 ? INT16_MAX : INT16_MIN, 0, 1000, gap.last_ms, false);
  assert(!gap.ready);
  // Timestamp wrap and extreme input remain bounded without signed overflow.
  memset(&gap, 0, sizeof(gap));
  clock_ms = UINT32_MAX - 1000;
  motion(&gap, 24, 10, 200, 0, false);
  assert(gap.ready && gap.motion_score >= 75);
  for (int i = 0; i < 1000; i++) {
    clock_ms += 20;
    bike_sample(&gap, i % 2 ? INT16_MAX : INT16_MIN, INT16_MIN, INT16_MAX, clock_ms, false);
    assert(bike_probability(&gap, 150, 0) <= 99);
  }
  printf("Bike fixtures: idle/noise/walking/impulses/haptics rejected; rapid vibration=%u%%, fresh HR=%u%%; stale HR, rotation, timing and bounds passed\n", base, fresh);
  return 0;
}
