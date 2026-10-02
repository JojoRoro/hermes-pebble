# Bike detection comparison

**Settings › Bike detection test** compares **NEW** (left) and **OLD** (right) using the same live 50 Hz accelerometer samples. These percentages are experimental evidence scores, **not calibrated probabilities**. A high score does not enable any action. This is a test toward future bike-only features, not background detection.

The old model is unchanged from 0.1.11, including its optional fresh-heart-rate bonus. The new model never uses BPM, resting BPM, or HR elevation: a fit rider at a low heart rate receives exactly the same motion score as someone with a high heart rate.

## Controls and field test

- Wait four seconds for the first score, then at least 12–20 seconds for sustained evidence.
- **Down** switches between the explanation and feature details while retaining both scores.
- **Up** begins a 20-second desk-learning session. Stay seated and type normally until it finishes. Five complete motion windows learn a local feature envelope. This is optional; first compare the defaults while typing and riding.
- **Select** clears both models' accumulated evidence, retaining a completed desk baseline. It restarts an unfinished learning session. **Hold Up** clears the baseline and resets both scores.
- The learned baseline lasts while this test screen exists, including Select and auto-pause/resume. **Back** leaves the test and discards it. Covering the screen releases subscriptions; returning starts fresh evidence and retains a completed baseline.
- After ten minutes the test pauses and releases its sensors/timer. **Select** resumes it.

Compare ordinary typing, sitting still, walking, a leisurely smooth-road ride, rough road, coasting, stopping, and (if convenient) riding as a passenger. Record both scores after 20–30 seconds and whether the desk baseline was set. For surprises, press Down and note buzz, fast fraction, steady blocks, peaks, and tilt. It is useful to compare a ride with and without the learned baseline; similar desk and cycling motion may cause it to suppress a real ride. No recording from the user's keyboard or bike has been used to tune this release.

## Available sensors and what they can establish

The current [Pebble specifications](https://repebble.com/) list a six-axis IMU, compass, and heart-rate sensor for Time 2. Hardware presence is different from access through the app's SDK. This project uses Pebble C SDK 4.33.1; its installed header and the [C event-service reference](https://developer.repebble.com/docs/c/Foundation/Event_Service/) were checked.

| Source | Use / limitation |
| --- | --- |
| Three-axis acceleration | Main evidence: continuous vibration, distribution over time, and gravity-vector drift. Does not measure speed or prove translation. |
| Steps via HealthService | A fresh increase in today's cumulative steps is a negative walking cue. No steps is neutral. Denial/missing data leaves motion-only scoring. Counts can be delayed or mistaken. |
| Heart rate | Displayed for context and used only by OLD. NEW ignores it entirely, including high BPM. |
| Gyroscope in the six-axis IMU | No raw gyroscope subscription in the public C SDK used here. We do not claim to read it. Gravity drift is derived from acceleration, not a gyroscope measurement. |
| Compass | The [compass service](https://developer.repebble.com/docs/c/Foundation/Event_Service/CompassService/) reports watch orientation, not travel direction or speed. Straight riding and typing can both hold a heading; wrist turns and nearby metal can change it. Not sampled or rewarded in this version. |
| Ambient light | Health minute records expose coarse light levels. Brightness does not distinguish a bike from a desk by a window, and darkness does not rule out a ride. Not used. |
| Microphone / touch | Dictation and touch are available, but wind, speech, taps, or lack of touches do not establish cycling. No microphone capture is started by Bike Test. |
| GPS / altitude / oxygen | No onboard GPS or public raw barometer/SpO2 stream is exposed to this app by the SDK. No speed, altitude, or oxygen measurements are fabricated. |

For future automatic activation, watch evidence should be corroborated by phone movement/activity classification or a bike-mounted speed/cadence sensor. GPS speed alone also confuses cycling with a car. A bike-specific signal or explicit ride mode is stronger evidence than wrist motion alone. An eventual state machine should require sustained entry evidence, define how junction stops are handled, and revoke confidence when sensor data becomes stale. Those integrations and automatic actions are not enabled here.

## New motion model

The [accelerometer service](https://developer.repebble.com/docs/c/Foundation/Event_Service/AccelerometerService/) request remains 50 Hz, now delivered in 50-sample (one-second) batches. This halves callback frequency without changing either model's signal or window size. It also avoids the SDK emulator's backward timestamps between half-second batches. Both models consume the same values and timestamps independently. Each new-model window contains 200 samples (four seconds), split into eight half-second blocks. Integer arithmetic retains aggregate features only.

1. Remove gravity with a per-axis 1/32 exponential average. Measure dynamic energy and adjacent-sample change energy using all three axes.
2. Qualify each half-second block only if at least 15 of its 25 changes exceed roughly 25 mg, mean change strength exceeds roughly 35 mg RMS, and its largest change energy is at most six times its mean. Bursty impacts and quiet gaps cannot pool into one strong four-second average.
3. Multiply vibration strength (30–125 mg ramp), fast-motion fraction (8–24% ramp), sustained-block coverage (4–8 ramp), and a stable-grip factor (full through 100 mg of gravity drift per half-second, zero by 350 mg). Stable orientation alone earns nothing. Extremely alternating motion (fast fraction above 85%) is capped at 30.
4. Recent steps or a match to the learned desk envelope cap the target at 20. The step cue compares today's cumulative totals every four seconds; four or more new steps hold the cue for twelve seconds. Large sampling gaps, negative counts, Health resets, and day rollover rebaseline rather than manufacture movement.
5. Three consecutive windows with a target of at least 70 are required before the score can rise above 59. Scores rise halfway toward the target and fall two-thirds toward it each window. `Ride candidate (test)` requires that sustained evidence and a displayed score of at least 70. The cap is 95. Loss of strong evidence immediately removes the candidate label, although the numeric score decays rather than jumping to zero.

The typing baseline records ranges for vibration, fast fraction, sustained-block count, peak/mean energy ratio, and gravity drift over five complete windows. A matching window must fall within all five ranges, expanded by a 20% allowance plus a small feature-specific margin. It is a negative example, not a trained activity classifier. Learning displays progress, keeps the new score low, and restarts after sensor gaps or haptics. A missed sensor cannot complete learning.

Clipped/scaled arithmetic bounds extreme values. Duplicate timestamps cannot increase evidence. Timing gaps or incompatible timing invalidate motion history. Samples marked `did_vibrate` clear it and impose a 300 ms cooldown. Missing callbacks clear both displays to `--%` after three seconds. Exit, covering, and auto-pause cancel timers, accelerometer and Health subscriptions, and the HR sampling request. Text layers redraw only on changes. There are no persistent samples, files, phone messages, or server uploads.

## Old model and HR comparison

`bike_core.h` remains unchanged: multiply rapid-change strength (25–160 mg), fast fraction (6–28%), and active sample fraction (20–65%), capped at 85, with temporal smoothing. It does not inspect continuity within each half-second. This explains how typing can score high even when seated.

Only OLD receives an HR bonus: when motion is at least 25, BPM of 95–145 ramps a bonus from zero to 15. Its weight is full for 60 seconds after a fresh event, declines to zero by 600 seconds, and the score is capped at 99. Cached BPM at entry has unknown age and no weight. Age is time since the app observed a valid event, not a physical measurement timestamp. While visible the test continues to request a 15-second sample interval for a fair legacy comparison; firmware may deliver less often or not at all. NEW adds no extra HR sampling.

## Evidence and limits

The native suites exercise the actual legacy/new models and sensor lifecycle. Synthetic fixtures include stationary noise, walking-like oscillation, mixed-frequency continuous vibration, typing-like impulses, intermittent bursts, isolated knocks, alternating shocks, stops, steps, desk learning/reset/clear, axis permutation, haptics, stale callbacks, timestamp gaps/duplicates/wraparound, and extreme input. Sanitizer checks cover integer/memory bounds. Service tests verify both columns use the same stream and HR never changes the new model.

These are behavioral regressions, not measured sensitivity or specificity. Continuous typing vibration, power tools, vehicles, or hand shaking can still fool a watch-only heuristic. Smooth-road riding, a loose watch, grip changes, bike-induced false steps, and an overlapping learned desk envelope can lower the new score. This version intentionally makes a strong positive harder to obtain; field comparisons are needed before it should control any feature.
