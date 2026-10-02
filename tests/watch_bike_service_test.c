#include <pebble.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "../src/c/bike_watch.h"

static bool subscribe_ok = true;
static int accel_result;
static HealthServiceAccessibilityMask access = HealthServiceAccessibilityMaskAvailable;
static HealthValue current_bpm = 145;
static HealthValue current_steps = 100;
static unsigned accel_starts, accel_stops, health_stops, timers, requested_period;
static time_t test_now = 1000;
time_t time(time_t *out) { if (out) *out = test_now; return test_now; }
AppTimer *app_timer_register(uint32_t ms, AppTimerCallback cb, void *ctx) {
  (void)cb; (void)ctx; assert(ms == 1000); timers++; return (AppTimer *)1;
}
void app_timer_cancel(AppTimer *timer) { (void)timer; timers--; }
void accel_data_service_subscribe(uint32_t count, AccelDataHandler cb) { assert(count == 50 && cb == bike_accel); accel_starts++; }
void accel_data_service_unsubscribe(void) { accel_stops++; }
int accel_service_set_sampling_rate(AccelSamplingRate rate) { assert(rate == ACCEL_SAMPLING_50HZ); return accel_result; }
bool health_service_events_subscribe(HealthEventHandler handler, void *ctx) { (void)ctx; assert(handler == bike_health); return subscribe_ok; }
bool health_service_events_unsubscribe(void) { health_stops++; return true; }
bool health_service_set_heart_rate_sample_period(uint16_t period) { requested_period = period; return true; }
HealthServiceAccessibilityMask health_service_metric_accessible(HealthMetric metric, time_t start, time_t end) {
  (void)start; (void)end; assert(metric == HealthMetricHeartRateRawBPM || metric == HealthMetricStepCount); return access;
}
HealthValue health_service_sum_today(HealthMetric metric) { assert(metric == HealthMetricStepCount); return current_steps; }
HealthValue health_service_peek_current_value(HealthMetric metric) { assert(metric == HealthMetricHeartRateRawBPM); return current_bpm; }
void text_layer_set_text(TextLayer *layer, const char *text) { (void)layer; (void)text; }

int main(void) {
  BikeView view = {0}; s_bike = &view;
  bike_start(NULL);
  assert(view.active && requested_period == 15 && accel_starts == 1 && timers == 1);
  assert(!view.hr_known); // A cached 145 BPM is not automatically fresh.
  bike_health(HealthEventSignificantUpdate, NULL);
  assert(!view.hr_known);
  bike_health(HealthEventHeartRateUpdate, NULL);
  assert(view.hr_known && view.bpm == 145 && view.hr_observed == 1000);
  current_bpm = 0;
  bike_health(HealthEventHeartRateUpdate, NULL);
  assert(view.bpm == 145); // Missing reading cannot manufacture a new observation.
  for (int i = 0; i < 5; i++) { test_now++; timers--; bike_tick(NULL); }
  assert(!view.model.ready && view.now == 1005);
  bike_stop(NULL);
  assert(!view.active && requested_period == 0 && accel_stops == 1 && health_stops == 1 && timers == 0);
  bike_health(HealthEventHeartRateUpdate, NULL);
  bike_stop(NULL);
  assert(accel_stops == 1 && health_stops == 1); // Idempotent lifecycle cleanup.
  access = HealthServiceAccessibilityMaskNoPermission;
  bike_start(NULL);
  assert(!view.hr_available && !view.hr_known && !view.hr_requested && requested_period == 0);
  AccelData sample = {.x = 0, .y = 0, .z = 1000, .timestamp = 1000};
  bike_accel(&sample, 1);
  assert(view.model.seeded); // Motion still works with denied health access.
  bike_stop(NULL);
  subscribe_ok = false;
  access = HealthServiceAccessibilityMaskAvailable;
  bike_start(NULL);
  assert(!view.health_ok && !view.hr_available && !view.hr_requested);
  bike_stop(NULL);
  assert(timers == 0 && health_stops == 2 && accel_starts == accel_stops);
  // An unattended test pauses itself, releasing sensors and its timer; SELECT resumes.
  subscribe_ok = true;
  bike_start(NULL);
  test_now += BIKE_IDLE_TIMEOUT_SECONDS;
  timers--; bike_tick(NULL);
  assert(view.paused && !view.active && timers == 0 && requested_period == 0 && accel_starts == accel_stops);
  bike_reset_click(NULL, NULL);
  assert(!view.paused && view.active && timers == 1 && requested_period == 15);
  bike_stop(NULL);
  assert(timers == 0 && accel_starts == accel_stops);
  // Both columns run on the same samples. The legacy model still gets HR;
  // the revised score is independent of it, and stale samples clear both.
  bike_start(NULL);
  const int16_t wave[] = {0, 190, 117, -117, -190};
  uint32_t sensor_ms = 1000;
  for (int i = 0; i < 1001; i++) {
    sample.x = wave[i % 5]; sample.timestamp = sensor_ms += 20;
    bike_accel(&sample, 1);
    if (i % 50 == 0) { test_now++; timers--; bike_tick(NULL); }
  }
  assert(view.revised.ready && view.revised.likely && view.model.ready);
  unsigned revised = view.revised.score;
  current_bpm = 170;
  bike_health(HealthEventHeartRateUpdate, NULL);
  bike_draw();
  assert(view.revised.score == revised);
  assert(atoi(view.old_score_text) == bike_probability(&view.model, 170, 0));
  uint32_t last_fresh = view.last_motion;
  test_now++;
  bike_accel(&sample, 1); // A duplicate callback must not refresh motion age.
  assert(view.last_motion == last_fresh);
  // Recent cumulative step changes suppress v2 immediately; cached totals,
  // day rollover, inaccessible health, and missing updates do not create steps.
  current_steps += 5;
  test_now += 4; view.now = test_now; bike_steps();
  assert(bike_walking() && !view.revised.likely && view.revised.score <= 20);
  current_steps = 0;
  bike_health(HealthEventSignificantUpdate, NULL);
  test_now += 4; view.now = test_now; bike_steps();
  assert(!bike_walking() && view.steps_known);
  test_now += 3; timers--; bike_tick(NULL);
  assert(!view.revised.ready && !view.model.ready);
  assert(strcmp(view.score_text, "--%") == 0 && strcmp(view.old_score_text, "--%") == 0);
  // Learning, reset, detail toggle, and clearing have distinct semantics.
  bike_learn_click(NULL, NULL);
  assert(view.desk.learning && view.desk.windows == 0 && !view.revised.ready);
  for (int i = 0; i < 1001; i++) {
    sample.x = wave[i % 5]; sample.timestamp = sensor_ms += 20;
    bike_accel(&sample, 1);
  }
  assert(view.desk.learned && !view.desk.learning && view.revised.desk_match);
  bike_reset_click(NULL, NULL);
  assert(view.desk.learned && !view.revised.ready && !view.model.ready);
  bike_detail_click(NULL, NULL);
  assert(view.details);
  bike_clear_click(NULL, NULL);
  assert(!view.desk.learned && !view.desk.learning);
  bike_learn_click(NULL, NULL);
  view.desk.windows = 2;
  test_now += 3; timers--; bike_tick(NULL);
  assert(view.desk.learning && view.desk.windows == 0);
  bike_stop(NULL);
  accel_result = -1;
  bike_start(NULL);
  bike_accel(&sample, 1);
  assert(!view.accel_ok && !view.revised.seeded && !view.model.seeded);
  bike_stop(NULL);
  assert(timers == 0 && accel_starts == accel_stops);
  s_bike = NULL;
  puts("Bike comparison, sensor lifecycle, HR independence, steps/rollover, calibration, stale motion, and cleanup passed");
  return 0;
}
