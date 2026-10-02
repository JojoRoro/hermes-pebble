#include <pebble.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "../src/c/bike_watch.h"

static bool subscribe_ok = true;
static HealthServiceAccessibilityMask access = HealthServiceAccessibilityMaskAvailable;
static HealthValue current_bpm = 145;
static unsigned accel_starts, accel_stops, health_stops, timers, requested_period;
static time_t test_now = 1000;
time_t time(time_t *out) { if (out) *out = test_now; return test_now; }
AppTimer *app_timer_register(uint32_t ms, AppTimerCallback cb, void *ctx) {
  (void)cb; (void)ctx; assert(ms == 1000); timers++; return (AppTimer *)1;
}
void app_timer_cancel(AppTimer *timer) { (void)timer; timers--; }
void accel_data_service_subscribe(uint32_t count, AccelDataHandler cb) { assert(count == 25 && cb == bike_accel); accel_starts++; }
void accel_data_service_unsubscribe(void) { accel_stops++; }
int accel_service_set_sampling_rate(AccelSamplingRate rate) { assert(rate == ACCEL_SAMPLING_50HZ); return 0; }
bool health_service_events_subscribe(HealthEventHandler handler, void *ctx) { (void)ctx; assert(handler == bike_health); return subscribe_ok; }
bool health_service_events_unsubscribe(void) { health_stops++; return true; }
bool health_service_set_heart_rate_sample_period(uint16_t period) { requested_period = period; return true; }
HealthServiceAccessibilityMask health_service_metric_accessible(HealthMetric metric, time_t start, time_t end) {
  (void)start; (void)end; assert(metric == HealthMetricHeartRateRawBPM); return access;
}
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
  s_bike = NULL;
  puts("Bike sensor lifecycle, denied/unavailable HR, fresh-event gating, stale motion, and sampling reset passed");
  return 0;
}
