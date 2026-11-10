# Hermes 2

A Pebble Time 2 (emery) watch app for talking to your own
Nous Hermes Agent server. You speak a question on the watch,
Hermes works on it, and the answer appears on the watch as text. There is no Android companion
app: everything on the phone runs inside the Pebble mobile app, as a Pebble Plugin plus the app's
PebbleKit JS.

What it does:

- Ask by voice, review the transcript, send. Follow-ups continue the same Hermes conversation;
  after a chosen idle time (default 4 hours) the next question starts a new one.
- Live progress (queued, working, needs approval, answered, failed, stopped, ...). A running
  request can be stopped. The app never approves anything in Hermes for you.
- Answers are cleaned of Markdown and characters the watch fonts cannot draw, and are paged to
  the watch only as you read. The last three items can be reopened.
- Voice memos recorded to the watch's flash, transcribed on the phone and sent to Hermes as a
  message.
- Handwritten notes drawn on the touch screen, kept on the phone and viewable on the settings
  page. They are not recognised or sent to Hermes.
- Other apps and assistants on the phone can hand Hermes a task through the plugin's
  `send_to_agent` action; the reply is read in the watch app.

## Architecture

```
watch (C app, src/c)
   |  AppMessage over Bluetooth
PebbleKit JS (src/pkjs)  -- runs only while the watch app is open
   |  Pebble.invokeAction -> watch_rpc
Hermes plugin (src/plugin/index.js)  -- started fresh for every request
   |  HTTPS
Hermes Agent API  (/health, /v1/models, /v1/runs)
```

- **Watch app** (`src/c`): menus, review, progress, reading, memo recording and the note pad.
- **PKJS bridge** (`src/pkjs/index.js`, `lib.js`): carries AppMessages, splits answers into pages
  that never cut a UTF-8 character, receives memos (IMA ADPCM, checked by length and CRC-32, resumed
  after an interruption) and notes, and drives polling, since a plugin has no timers. Questions the
  plugin cannot take yet wait in an outbox.
- **Plugin** (`src/plugin/index.js`): the only code that talks to Hermes. It owns the settings,
  history (newest 12 finished items plus anything in flight), conversation state and the
  credentials. Each question has a request id used as the idempotency key, so a retry never starts
  a second run. Brief replies are requested through the run's `instructions`.
- **Settings page** (`src/config.html`): bundled in the pbw, self-contained, and talks live to the
  plugin (`load`, `save`, `test`, `newConversation`, `clearHistory`, `clearNotes`) and, while the
  watch app is open, to PKJS (`refresh`, `sttDownload`, `sttCancel`, plus pushed download progress).
  The API key and the extra header value are write-only: they go to `Pebble.secureStorage` and are
  never sent back to the page.
- **Speech**: by default speech is recognised on the phone. The watch streams microphone audio to
  PKJS, which runs a Moonshine or Whisper model with transformers.js and onnxruntime-web (WASM,
  single-threaded) inside the PKJS WebView. The engine is a prebuilt bundle
  (`src/pkjs/stt_engine.json`) that is evaluated at runtime; the model files
  (`src/pkjs/stt_catalog.json`) and the WASM runtime are downloaded once from Hugging Face and
  jsDelivr at pinned revisions, each checked against its SHA-256, and cached in IndexedDB. Where that
  cannot run, or if you choose it, the watch uses standard Pebble dictation instead.

## Setup

1. Pebble mobile app 1.16 or later, with plugins turned on: Settings → Plugins → Plugins /
   permissions preview.
2. Install the pbw. Allow Hermes 2 the watch microphone. Internet access to `huggingface.co`,
   `hf.co` and `cdn.jsdelivr.net` is declared for the speech model download; your Hermes server's
   domain is your own, so the Pebble app asks for it the first time the plugin connects.
3. Open the app's settings page (Apps → Plugins, or the app's settings in the Pebble app). Enter
   the Hermes URL (the API root, with or without `/v1`; must be `https://`, plain `http://` is only
   accepted for `localhost`, `127.0.0.1` and the emulator's `10.0.2.2`) and the API key. If a proxy
   in front of Hermes needs its own header, turn on the extra access header and give its name and
   value. Save, then Test connection.
4. Pick a speech model. The first download is the model plus about 14 MB of runtime:

   | Model          | Model size | Languages          |
   | -------------- | ---------- | ------------------ |
   | Moonshine Tiny | 31 MB      | English (default)  |
   | Moonshine Base | 64 MB      | English            |
   | Whisper Tiny   | 42 MB      | multilingual       |
   | Whisper Base   | 76 MB      | multilingual       |

   The download runs in the Pebble app while Hermes 2 is open on the watch; it starts on first
   use, or from the settings page. Wi-Fi recommended.

## Building

Needs pebble-tool 5.1.2 and SDK 4.44 (plugins need SDK 4.44 and pebble-tool 5.1.0 or later).

```
npm run engine          # once: generates src/pkjs/stt_engine.json (needed by the build and tests)
npm test                # unit tests, Node built-ins only (node --test)
pebble build            # build/v2.pbw, bundles the plugin and the settings page
```

`src/pkjs/stt_engine.json` (about 520 KB) is generated, not committed (GitHub's secret scanner
flags a random-looking string in the minified bundle). `npm run engine` installs the pinned
build dependencies in `tools/stt` (about 210 MB) and builds it with esbuild in well under a
second; the output is byte-identical between runs, so a rebuild with an unchanged lock file
produces no diff. `node tools/stt/build.mjs --catalog` also refreshes the model catalogue (file
sizes and hashes) from Hugging Face.

### Testing tools

- `node tools/mock-hermes.mjs [port] [api-key]`: a stand-in Hermes API server. Words in the
  question steer it: `approval` waits for approval, `fail` fails, `long` gives a long Markdown
  answer, `slow` takes a minute.
- `python3 tools/emu-configure.py http://127.0.0.1:8642 test-key`: points the emulator's copy of
  the app at a server by writing the plugin's storage directly, so the settings page is not
  needed. Run it with the emulator stopped (`pebble kill`), then `pebble install --emulator emery`.
- `node tools/config-preview.mjs [out-dir]`: opens the settings page in headless Chromium with the
  real plugin code against the mock, checks the main flows and writes phone-sized screenshots.
  Needs `npm run engine` first (for playwright-core) and a Chromium (`CHROMIUM_PATH`, or
  Playwright's download).
- `node tools/stt/test-browser.mjs <cache-dir> <model-id> <audio-file>`: runs the speech engine
  in Chromium configured like the Android PKJS WebView.

In the emulator, on-phone speech is not available; questions use Pebble dictation.

## Limitations

- Answers are text only. Spoken replies on the watch speaker were left out on purpose.
- Memos and notes move to the phone only while the Hermes 2 app is open on the watch, because
  plugins and PKJS do not run in the background. Memos wait in the watch's flash until then.
- The phone shows no notifications when an answer is ready; the watch app shows it and vibrates once (not during Quiet Time).
- On-phone speech needs the Android Pebble app's WebView (WebAssembly and IndexedDB in PKJS).
  On iOS and in the emulator the app falls back to Pebble dictation, which needs the phone online.
- Not yet verified on real hardware; tested in the emulator and with the unit tests.
