# Experimental cycling score

**Settings › Bike detection test** on the watch opens an independent screen with a large 0–99% estimate, motion summary, and heart-rate observation age. It shows `--%` until a complete motion window is available. Select resets evidence. Leaving or covering the screen unsubscribes from accelerometer and health events, cancels its timer, and restores automatic heart-rate sampling. Returning starts fresh. After 10 minutes the test pauses itself, releases the accelerometer, heart-rate request, and timer, and shows `--%`; Select resumes it. The test runs locally, retains only aggregate features in RAM, and does not enable automatic audio or background cycling detection.

## Motion model

The [accelerometer API](https://developer.repebble.com/docs/c/Foundation/Event_Service/AccelerometerService/) supports 50 Hz sampling in 25-sample batches, with three axes and timestamps. The model processes 200 samples (four seconds) per window. All axes contribute symmetrically, so it does not require a particular wrist orientation. It uses a slow gravity estimate, motion energy after gravity removal, energy of changes between adjacent samples, and the proportion of sustained changes. Slow arm swings have a low change-to-motion ratio; road-like buzz tends to raise that ratio.

| Feature | Initial heuristic |
| --- | --- |
| Gravity estimate | Per-axis exponential average, 1/32 update |
| Rapid-change strength | Ramps from 25 to 160 mg RMS vector change |
| Fast-motion fraction | Adjacent-change energy / (4 × dynamic-motion energy), ramp from 6% to 28% |
| Sustained changes | At least 20% of samples above about 25 mg; full weight at 65% |
| Motion score | Product of those three normalized features, capped at 85 |
| Temporal smoothing | Halfway toward a rising target each window; two-thirds toward a falling target |

Clipped/scaled integer arithmetic bounds extreme samples and avoids retaining sample arrays. Duplicate timestamps cannot increase evidence. Gaps or an incompatible sample rate invalidate the window. Samples marked `did_vibrate` reset the motion evidence and impose a 300 ms cooldown, preventing the watch's own haptic motor from being classified as a ride. Missing callbacks invalidate the displayed score after three seconds.

## Heart rate

The [HealthService API](https://developer.repebble.com/docs/c/Foundation/Event_Service/HealthService/) exposes raw BPM, update events, accessibility checks, and a requested sample interval. The package declares the health capability. While the screen is visible, it requests a 15-second sample period when health access is available; this is a suggestion to firmware, not a delivery guarantee. Permission denial, unsupported metrics, or missing readings leave a motion-only score.

A cached BPM at entry has unknown age and receives no weight. A valid raw BPM (30–220) observed on a new heart-rate update is timestamped. With a motion score of at least 25, BPM from 95 to 145 ramps the bonus from zero to 15 points. Low heart rate does not penalize coasting. The bonus has full weight for 60 seconds, decays linearly to zero at 600 seconds, and cannot push the display above 99%. Age is elapsed wall time since the event, not a count of redraws; it is the app's observation age rather than a sensor-provided physical measurement timestamp.

This is an initial hand-tuned heuristic, not a trained classifier or calibrated probability. The thresholds were checked with synthetic signals, not a labeled outdoor cycling dataset. Vehicle vibration, tools, and deliberate hand shaking can produce false positives. Smooth-road riding, different bikes/surfaces, grip and wrist placement, and motion-corrupted heart rate can produce false negatives or misleading boosts. The percentage should guide experimentation, not be treated as measured accuracy.

## Checks and field testing

`watch_bike_test.c` exercises stationary noise, slow walking-like motion, sustained rapid vibration, isolated shocks, axis rotation, haptics, heart-rate aging, stopping, gaps, repeated timestamps, wraparound, and extreme values. `watch_bike_service_test.c` checks permission/failure paths, fresh-event gating, subscription/timer cleanup, and cancellation of the sample-period request. `watch_bike_smoke.py` checks the actual watch UI and sensor path with emulator accelerometer input.

For field evaluation, compare the score while sitting, walking, riding on smooth and rough surfaces, and traveling as a passenger. Note wrist position, surface, percentage, and the displayed HR age after 15–20 seconds. No ride has been recorded or labeled for tuning in this release.
