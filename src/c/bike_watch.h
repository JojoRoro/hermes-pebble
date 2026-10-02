#ifndef HERMES_BIKE_WATCH_H
#define HERMES_BIKE_WATCH_H
#include "bike_v2.h"

// 50 Hz motion plus heart-rate sampling is costly; pause an unattended test.
#define BIKE_IDLE_TIMEOUT_SECONDS 600u

typedef struct {
  BikeModel model;
  BikeV2 revised;
  BikeDesk desk;
  TextLayer *title, *labels, *score, *old_score, *detail, *footer;
  AppTimer *timer;
  char score_text[16], old_score_text[16], detail_text[224];
  uint32_t now, last_motion, hr_observed, started, step_checked, walking_until;
  HealthValue steps;
  uint16_t bpm;
  bool active, accel_ok, health_ok, hr_available, hr_requested, hr_known, paused, steps_known, details;
} BikeView;
static Window *s_bike_window;
static BikeView *s_bike;

static bool bike_walking(void) {
  return s_bike->walking_until && (int32_t)(s_bike->walking_until - s_bike->now) > 0;
}

static void bike_steps(void) {
  time_t now = s_bike->now;
  uint32_t elapsed = s_bike->now - s_bike->step_checked;
  if (elapsed < 4u) return;
  s_bike->step_checked = s_bike->now;
  if (!s_bike->health_ok || !(health_service_metric_accessible(HealthMetricStepCount, now, now) & HealthServiceAccessibilityMaskAvailable)) {
    s_bike->steps_known = false;
    return;
  }
  HealthValue steps = health_service_sum_today(HealthMetricStepCount);
  if (steps < 0) { s_bike->steps_known = false; return; }
  if (s_bike->steps_known && elapsed <= 10u && steps >= s_bike->steps && steps - s_bike->steps >= 4) {
    s_bike->walking_until = s_bike->now + 12u;
    // Do not leave a high candidate visible until the next motion window.
    if (s_bike->revised.score > 20u) s_bike->revised.score = 20u;
    s_bike->revised.streak = 0;
    s_bike->revised.likely = false;
  }
  s_bike->steps = steps;
  s_bike->steps_known = true;
}

/* Only touch a layer when its text changed; every set marks the window dirty. */
static void bike_set_text(TextLayer *layer, char *buffer, size_t size, const char *text) {
  if (strncmp(buffer, text, size) == 0) return;
  snprintf(buffer, size, "%s", text);
  text_layer_set_text(layer, buffer);
}

static void bike_draw(void) {
  if (!s_bike) return;
  char score[sizeof(s_bike->score_text)];
  char old_score[sizeof(s_bike->old_score_text)];
  char detail[sizeof(s_bike->detail_text)];
  uint32_t age = s_bike->hr_known ? s_bike->now - s_bike->hr_observed : UINT32_MAX;
  if (s_bike->paused) {
    snprintf(score, sizeof(score), "--%%");
    snprintf(old_score, sizeof(old_score), "--%%");
    snprintf(detail, sizeof(detail), "Paused to save battery\nSELECT to resume");
  } else {
    if (s_bike->revised.ready) snprintf(score, sizeof(score), "%u%%", s_bike->revised.score);
    else snprintf(score, sizeof(score), "--%%");
    if (s_bike->model.ready) snprintf(old_score, sizeof(old_score), "%u%%",
      bike_probability(&s_bike->model, s_bike->bpm, age));
    else snprintf(old_score, sizeof(old_score), "--%%");
    char hr[48];
    if (s_bike->hr_known) snprintf(hr, sizeof(hr), "HR %u / %lus ago", s_bike->bpm, (unsigned long)age);
    else snprintf(hr, sizeof(hr), "HR: %s", s_bike->hr_available ? "waiting" : "unavailable");
    const char *state = !s_bike->accel_ok ? "Motion unavailable" :
      s_bike->now - s_bike->last_motion >= 3u ? "Waiting for motion sensor" :
      !s_bike->revised.ready ? "Collecting 4s of motion" :
      bike_walking() ? "Steps: possible walking" :
      s_bike->revised.desk_match ? "Matches learned desk" :
      s_bike->revised.likely ? "Ride candidate (test)" :
      s_bike->revised.target >= 70u ? "Checking sustained motion" : "No strong ride evidence";
    if (s_bike->desk.learning) {
      snprintf(detail, sizeof(detail), "Type normally at your desk\nLearning: %u / 20 seconds\n%s\nNew score waits for learning",
        s_bike->desk.windows * 4u, !s_bike->accel_ok ? "Motion unavailable" :
        s_bike->now - s_bike->last_motion >= 3u ? "Waiting for motion sensor" : "Keep typing until complete");
    } else if (s_bike->details && s_bike->revised.ready) {
      snprintf(detail, sizeof(detail), "Buzz %u mg / fast %u%%\nSteady blocks %u/8\nPeaks %u / tilt %u mg\nSteps %s / desk %s\n%s (old only)",
        s_bike->revised.features[0], s_bike->revised.features[1], s_bike->revised.features[2],
        s_bike->revised.features[3], s_bike->revised.features[4],
        !s_bike->steps_known ? "n/a" : bike_walking() ? "yes" : "quiet",
        s_bike->desk.learned ? "set" : "unset", hr);
    } else {
      snprintf(detail, sizeof(detail), "%s\n%s\n%s\nNew ignores heart rate\nHeuristic, not probability", state,
        s_bike->desk.learned ? "Desk learned for this test" : "UP: learn your typing", hr);
    }
  }
  bike_set_text(s_bike->score, s_bike->score_text, sizeof(s_bike->score_text), score);
  bike_set_text(s_bike->old_score, s_bike->old_score_text, sizeof(s_bike->old_score_text), old_score);
  bike_set_text(s_bike->detail, s_bike->detail_text, sizeof(s_bike->detail_text), detail);
}

static void bike_accel(AccelData *data, uint32_t count) {
  if (!s_bike || !s_bike->active || !s_bike->accel_ok) return;
  uint32_t previous_ms = s_bike->model.last_ms;
  bool seeded = s_bike->model.seeded;
  s_bike->now = (uint32_t)time(NULL);
  for (uint32_t i = 0; i < count; i++) {
    bike_sample(&s_bike->model, data[i].x, data[i].y, data[i].z,
      (uint32_t)data[i].timestamp, data[i].did_vibrate);
    bike_v2_sample(&s_bike->revised, &s_bike->desk, data[i].x, data[i].y, data[i].z,
      (uint32_t)data[i].timestamp, data[i].did_vibrate, bike_walking());
  }
  // Repeated callbacks carrying the same timestamp must not keep an old high
  // score alive. Freshness tracks advancing samples, not callback activity.
  if (s_bike->model.seeded && (!seeded || s_bike->model.last_ms != previous_ms)) {
    s_bike->last_motion = s_bike->now;
  }
}

static void bike_health(HealthEventType event, void *context) {
  (void)context;
  if (!s_bike || !s_bike->active) return;
  if (event == HealthEventSignificantUpdate) {
    s_bike->steps_known = false; // Day rollover / Health cache reset is not movement.
    s_bike->walking_until = 0;
  }
  if (event != HealthEventHeartRateUpdate) return;
  time_t now = time(NULL);
  if (!(health_service_metric_accessible(HealthMetricHeartRateRawBPM, now, now) & HealthServiceAccessibilityMaskAvailable)) return;
  HealthValue bpm = health_service_peek_current_value(HealthMetricHeartRateRawBPM);
  if (bpm < 30 || bpm > 220) return;
  s_bike->bpm = (uint16_t)bpm;
  s_bike->hr_observed = (uint32_t)now;
  s_bike->hr_known = true;
}

static void bike_stop(Window *window);

static void bike_tick(void *context) {
  (void)context;
  if (!s_bike || !s_bike->active) return;
  s_bike->timer = NULL;
  s_bike->now = (uint32_t)time(NULL);
  bike_steps();
  if (s_bike->now - s_bike->last_motion >= 3u) {
    memset(&s_bike->model, 0, sizeof(s_bike->model));
    memset(&s_bike->revised, 0, sizeof(s_bike->revised));
    if (s_bike->desk.learning) bike_desk_begin(&s_bike->desk);
  }
  if (s_bike->now - s_bike->started >= BIKE_IDLE_TIMEOUT_SECONDS) {
    bike_stop(NULL);
    s_bike->paused = true;
    bike_draw();
    return;
  }
  bike_draw();
  s_bike->timer = app_timer_register(1000u, bike_tick, NULL);
}

static void bike_stop(Window *window) {
  (void)window;
  if (!s_bike || !s_bike->active) return;
  s_bike->active = false;
  accel_data_service_unsubscribe();
  if (s_bike->hr_requested) health_service_set_heart_rate_sample_period(0);
  if (s_bike->health_ok) health_service_events_unsubscribe();
  if (s_bike->timer) app_timer_cancel(s_bike->timer);
  s_bike->timer = NULL;
}

static void bike_start(Window *window) {
  (void)window;
  if (!s_bike) return;
  memset(&s_bike->model, 0, sizeof(s_bike->model));
  memset(&s_bike->revised, 0, sizeof(s_bike->revised));
  // A partial training run must contain five uninterrupted test windows.
  if (s_bike->desk.learning) bike_desk_begin(&s_bike->desk);
  s_bike->now = s_bike->last_motion = s_bike->started = (uint32_t)time(NULL);
  s_bike->step_checked = s_bike->now - 4u;
  s_bike->steps_known = false;
  s_bike->walking_until = 0;
  s_bike->hr_known = s_bike->hr_requested = s_bike->paused = false;
  s_bike->active = true;
  // One-second delivery batches halve callback overhead; scoring still uses
  // 50 Hz samples and the same four-second windows in both models.
  accel_data_service_subscribe(50, bike_accel);
  s_bike->accel_ok = accel_service_set_sampling_rate(ACCEL_SAMPLING_50HZ) == 0;
  s_bike->health_ok = health_service_events_subscribe(bike_health, NULL);
  bike_steps();
  time_t now = time(NULL);
  HealthServiceAccessibilityMask access = health_service_metric_accessible(HealthMetricHeartRateRawBPM, now, now);
  s_bike->hr_available = s_bike->health_ok && !(access & (HealthServiceAccessibilityMaskNoPermission | HealthServiceAccessibilityMaskNotSupported));
  if (s_bike->hr_available) {
    s_bike->hr_requested = health_service_set_heart_rate_sample_period(15);
  }
  // A cached BPM at entry has unknown age. Only a new HR update gains weight.
  bike_draw();
  s_bike->timer = app_timer_register(1000u, bike_tick, NULL);
}

static void bike_reset_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  bike_stop(s_bike_window);
  bike_start(s_bike_window);
}

static void bike_learn_click(ClickRecognizerRef recognizer, void *context) {
  if (!s_bike) return;
  bike_desk_begin(&s_bike->desk);
  bike_reset_click(recognizer, context);
}

static void bike_clear_click(ClickRecognizerRef recognizer, void *context) {
  if (!s_bike) return;
  memset(&s_bike->desk, 0, sizeof(s_bike->desk));
  bike_reset_click(recognizer, context);
}

static void bike_detail_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  if (!s_bike) return;
  s_bike->details = !s_bike->details;
  bike_draw();
}

static void bike_click_config(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, bike_reset_click);
  window_single_click_subscribe(BUTTON_ID_UP, bike_learn_click);
  window_long_click_subscribe(BUTTON_ID_UP, 800, bike_clear_click, NULL);
  window_single_click_subscribe(BUTTON_ID_DOWN, bike_detail_click);
}

static TextLayer *bike_label(GRect frame, const char *font, const char *text) {
  TextLayer *layer = text_layer_create(frame);
  if (!layer) return NULL;
  text_layer_set_font(layer, fonts_get_system_font(font));
  text_layer_set_text_alignment(layer, GTextAlignmentCenter);
  text_layer_set_text(layer, text);
  layer_add_child(window_get_root_layer(s_bike_window), text_layer_get_layer(layer));
  return layer;
}

static void bike_load(Window *window) {
  s_bike = calloc(1, sizeof(*s_bike));
  if (!s_bike) { window_stack_pop(false); return; }
  GRect b = layer_get_bounds(window_get_root_layer(window));
  s_bike->title = bike_label(GRect(0, 0, b.size.w, 24), FONT_KEY_GOTHIC_18_BOLD, "Bike test: compare");
  s_bike->labels = bike_label(GRect(0, 24, b.size.w, 21), FONT_KEY_GOTHIC_18_BOLD, "NEW                  OLD");
  s_bike->score = bike_label(GRect(0, 45, b.size.w / 2, 49), FONT_KEY_BITHAM_42_BOLD, "--%");
  s_bike->old_score = bike_label(GRect(b.size.w / 2, 45, b.size.w / 2, 49), FONT_KEY_BITHAM_42_BOLD, "--%");
  s_bike->detail = bike_label(GRect(4, 96, b.size.w - 8, b.size.h - 132), FONT_KEY_GOTHIC_18, "");
  s_bike->footer = bike_label(GRect(0, b.size.h - 34, b.size.w, 34), FONT_KEY_GOTHIC_14_BOLD,
    "SEL: reset   DOWN: details\nUP: learn desk / hold: clear");
  if (!s_bike->title || !s_bike->labels || !s_bike->score || !s_bike->old_score || !s_bike->detail || !s_bike->footer) { window_stack_pop(false); return; }
  // Same chrome as the main app: accent header, light hint footer.
  text_layer_set_background_color(s_bike->title, GColorCobaltBlue);
  text_layer_set_text_color(s_bike->title, GColorWhite);
  text_layer_set_background_color(s_bike->footer, GColorLightGray);
}

static void bike_unload(Window *window) {
  bike_stop(window);
  if (!s_bike) return;
  if (s_bike->title) text_layer_destroy(s_bike->title);
  if (s_bike->labels) text_layer_destroy(s_bike->labels);
  if (s_bike->score) text_layer_destroy(s_bike->score);
  if (s_bike->old_score) text_layer_destroy(s_bike->old_score);
  if (s_bike->detail) text_layer_destroy(s_bike->detail);
  if (s_bike->footer) text_layer_destroy(s_bike->footer);
  free(s_bike);
  s_bike = NULL;
}

static void bike_open(void) {
  if (!s_bike_window) {
    s_bike_window = window_create();
    if (!s_bike_window) return;
    window_set_click_config_provider(s_bike_window, bike_click_config);
    window_set_window_handlers(s_bike_window, (WindowHandlers){
      .load = bike_load, .unload = bike_unload, .appear = bike_start, .disappear = bike_stop,
    });
  }
  window_stack_push(s_bike_window, true);
}

static void bike_shutdown(void) {
  if (s_bike_window) window_destroy(s_bike_window);
  s_bike_window = NULL;
}
#endif
