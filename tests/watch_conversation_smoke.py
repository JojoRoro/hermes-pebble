"""Automatic answer and follow-up smoke; run with the Pebble CLI Python environment."""
import argparse
import queue
import time
from pathlib import Path
from uuid import UUID
from PIL import Image
from pebble_tool.commands.base import PebbleCommand
from pebble_tool.commands.install import ToolAppInstaller
from pebble_tool.commands.emucontrol import send_data_to_qemu, QemuButton
from libpebble2.services.appmessage import AppMessageService, Uint32, ByteArray
from libpebble2.services.screenshot import Screenshot

parser = argparse.ArgumentParser(parents=PebbleCommand._shared_parser())
parser.add_argument('--pbw', default='build/hermes-pebble.pbw')
parser.add_argument('--voice-only', action='store_true')
parser.add_argument('--recent-voice-only', action='store_true')
parser.add_argument('--long-scroll-only', action='store_true')
parser.add_argument('--output', type=Path, default=Path('build/conversation-smoke'))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
command = PebbleCommand()
connection = command._connect(args)
from libpebble2.protocol.logs import AppLogMessage, AppLogShippingControl
faults = []
def log(packet):
    print('WATCH LOG', packet.filename, packet.line_number, packet.message, flush=True)
    if 'App fault!' in str(packet.message):
        faults.append(str(packet.message))
connection.register_endpoint(AppLogMessage, log)
connection.send_packet(AppLogShippingControl(enable=True))
messages = queue.Queue()
service = AppMessageService(connection)
app_id = UUID('7d07aa22-7d13-48c1-a400-2602a5ae4647')
service.register_handler('appmessage', lambda tx, app, data: messages.put(data) if app == app_id else None)

def receive(correlation=None, kind=1, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = messages.get(timeout=max(.1, deadline-time.monotonic()))
        print('Watch RX kind', data.get(1), 'transfer', data.get(2), 'correlation', data.get(17), flush=True)
        if data.get(1) == kind and (correlation is None or data.get(17) == correlation): return data
    raise AssertionError(f'No watch message with kind {kind} and correlation {correlation}')

def capture(name):
    rows=Screenshot(connection).grab_image()
    picture = Image.frombytes('RGB',(len(rows[0])//3,len(rows)),b''.join(bytes(row) for row in rows))
    picture.save(args.output / (name + '.png'))
    return picture.crop((8, 48, picture.width - 8, picture.height - 30)).tobytes()

def click(button):
    send_data_to_qemu(connection.transport,QemuButton(state=button))
    time.sleep(.08)
    send_data_to_qemu(connection.transport,QemuButton(state=0))
    time.sleep(.18)

from libpebble2.services.voice import VoiceService, SetupResult, TranscriptionResult
import json
class FixtureVoiceService(VoiceService):
    def _handle_audio_frame(self, session_id, frame_data):
        # No speech engine: inject transcripts below and ignore buffered audio.
        # libpebble2's stale-frame path otherwise calls send_stop_audio incorrectly.
        pass
voice = FixtureVoiceService(connection)
voice_sessions = queue.Queue()
def setup_voice(app, encoder):
    voice.send_session_setup_result(SetupResult.Success, app)
    voice_sessions.put(app)
voice.register_handler('session_setup', setup_voice)
transfer = 5000

def send(kind, capture_id=0, correlation=0, status=0, flags=0, payload=b'', offset=0, total=0, item_id=42, generation=7):
    global transfer
    transfer += 1
    chunks = []
    start = 0
    while start < len(payload):
        end = min(start + 192, len(payload))
        while end < len(payload) and payload[end] & 0xc0 == 0x80:
            end -= 1
        chunks.append(payload[start:end])
        start = end
    chunks = chunks or [b'']
    for i, chunk in enumerate(chunks):
        fields = {key: Uint32(0) for key in range(18)}
        fields.update({0:Uint32(1), 1:Uint32(kind), 2:Uint32(transfer), 3:Uint32(capture_id),
                       4:Uint32(i), 5:Uint32(len(chunks)), 6:ByteArray(chunk), 7:Uint32(status),
                       9:Uint32(item_id), 10:Uint32(status), 11:Uint32(1), 12:Uint32(offset),
                       14:Uint32(total), 15:Uint32(generation), 16:Uint32(flags), 17:Uint32(correlation)})
        service.send_message(app_id, fields)
        time.sleep(.15)

def dictate(words):
    app = voice_sessions.get(timeout=10)
    time.sleep(.5)
    voice.send_stop_audio()
    voice.send_dictation_result(TranscriptionResult.Success, [words.split()], app_uuid=app)
    time.sleep(1.5)
    capture('after-dictation')

def send_review(voice_reply=False):
    click(QemuButton.Button.Select)  # Review actions
    capture('review-actions')
    if voice_reply: click(QemuButton.Button.Down)
    capture('voice-review-actions' if voice_reply else 'text-review-actions')
    click(QemuButton.Button.Select)  # Send
    return receive(kind=2)

def receipt(request):
    send(102, request[3], request[2], status=2, flags=8)

def deliver_page(fetch, text, end=None, generation=7):
    data = text.encode()
    offset = fetch[12]
    assert offset < len(data)
    stop = min(offset + 520, len(data), end if end is not None else len(data))
    while stop < len(data) and data[stop] & 0xc0 == 0x80:
        stop -= 1
    more = stop < len(data)
    payload = json.dumps(dict(captureId=fetch[3], itemId=42, state=7,
                              output=data[offset:stop].decode(), more=more), ensure_ascii=False).encode()
    assert len(payload) <= 768
    send(105, fetch[3], fetch[2], status=7, flags=int(more), payload=payload,
         offset=offset, total=len(data), generation=generation)
    time.sleep(.3)
    return stop

def fetch_while_reading(timeout=30, button=QemuButton.Button.Down):
    # The watch asks for more text only as the reader nears the loaded end.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            data = messages.get(timeout=.6)
        except queue.Empty:
            click(button)
            continue
        print('Watch RX kind', data.get(1), 'transfer', data.get(2), 'correlation', data.get(17), flush=True)
        if data.get(1) == 6: return data
    raise AssertionError('The watch did not request more text while scrolling')

def finish(request, text):
    send(103, request[3], status=7)
    # No user input here: completion must trigger FETCH_RESULT automatically.
    fetch = receive(kind=6)
    assert fetch[3] == request[3] and fetch[12] == 0 and not (fetch[16] & 16)
    offset = deliver_page(fetch, text)
    time.sleep(1)
    assert messages.empty(), 'The first screen must not download the whole answer'
    capture('reading-first-chunk')
    part = 1
    while offset < len(text.encode()):
        fetch = fetch_while_reading()
        assert fetch[3] == request[3] and fetch[12] == offset
        reading = capture('reading-before-chunk-' + str(part))
        if part == 1:
            # A chunk arriving during Actions must leave that menu open.
            click(QemuButton.Button.Select)
            actions = capture('actions-during-next-chunk')
        offset = deliver_page(fetch, text)
        if part == 1:
            assert capture('actions-after-next-chunk') == actions
            click(QemuButton.Button.Back)
        assert capture('reading-after-chunk-' + str(part)) == reading, 'Incoming text reset the reading position'
        part += 1
    assert part >= 5, 'Fixture must exceed the old result and body buffers'
    return capture('automatic-answer')

ToolAppInstaller(connection,args.pbw,quiet=True).install()
startup = receive()
send(101, correlation=startup[2])
time.sleep(.3)
if args.voice_only:
    click(QemuButton.Button.Select)
    dictate('Say hello out loud')
    request = send_review(voice_reply=True)
    receipt(request)
    send(103, request[3], status=7)
    fetch = receive(kind=6)
    assert fetch[3] == request[3] and fetch[16] & 16
    deliver_page(fetch, 'Hello from Hermes.')
    send(114, request[3], payload=b'Preparing voice on phone')
    capture('voice-preparing')
    send(110, capture_id=800, flags=1, total=2, item_id=request[3])
    assert receive(kind=10)[7] == 1, 'Opted-in capture must accept audio'
    click(QemuButton.Button.Back)
    send(110, capture_id=801, flags=1, total=2, item_id=request[3])
    assert receive(kind=10)[7] == 8, 'Back must reject later clips'
    click(QemuButton.Button.Select)
    dictate('Text only please')
    regular = send_review()
    receipt(regular)
    send(103, regular[3], status=7)
    fetch = receive(kind=6)
    assert not (fetch[16] & 16), 'Regular send must not opt in'
    deliver_page(fetch, 'This is a text reply.')
    send(110, capture_id=802, flags=1, total=2, item_id=request[3])
    assert receive(kind=10)[7] == 8, 'An old capture must not speak during a new request'
    assert not faults, faults
    capture('text-after-voice')
    print('Voice opt-in, result flag, accepted audio, Back cancellation between clips, and text-only send passed.', flush=True)
    service.shutdown()
    raise SystemExit(0)
if not args.long_scroll_only and not args.recent_voice_only:
    click(QemuButton.Button.Select)  # Ask Hermes
    # Local voice fixture: never calls a speech service or Hermes.
    dictate('Tell me about the moon')
    first = send_review()
    assert first[15] == 7
    receipt(first)
    send(103, first[3], status=5)
    capture('working')
    long_answer = 'The Moon orbits Earth. Reply to ask a follow-up question.\n\n' + (
        'The Moon has mountains and craters. A rocket 🚀 takes several days to reach it. '
        'Its gravity is weaker than Earth\'s, so astronauts can jump higher.\n\n') * 20 + 'END OF COMPLETE ANSWER.'
    answer = finish(first, long_answer)
    # Repeated completion must preserve the displayed reply and scroll position.
    send(103, first[3], status=7)
    assert capture('answer-after-duplicate-status') == answer
    click(QemuButton.Button.Select)
    capture('reply-action')
    click(QemuButton.Button.Select)
    dictate('How long does it take')
    # Late status for the previous turn must not replace this new review.
    review = capture('follow-up-review')
    send(103, first[3], status=7)
    assert capture('review-after-late-status') == review
    second = send_review()
    assert second[3] != first[3] and second[15] == first[15], 'Reply must keep the conversation generation'
    # Fast completion can arrive before the durable receipt.
    send(103, second[3], status=7)
    receipt(second)
    fetch = receive(kind=6)
    assert fetch[3] == second[3]
    text = 'About 27.3 days relative to the stars.'
    payload = json.dumps(dict(captureId=second[3], itemId=42, state=7, output=text, more=False)).encode()
    send(105, second[3], fetch[2], status=7, payload=payload, total=len(text))
    time.sleep(.3)
    capture('follow-up-answer')
    click(QemuButton.Button.Back)
    menu = capture('menu-after-back')
    send(103, second[3], status=7)
    assert capture('menu-after-late-status') == menu
    click(QemuButton.Button.Back)
    assert not faults, faults
    print('Automatic joined answer, stable scrolling, Actions preservation, Reply generation, fast completion/receipt race, and Back passed.', flush=True)
    service.shutdown()
    raise SystemExit(0)
# Open a saved completed request without dictation or any live Hermes call.
# Separate runs keep the SDK emulator's long-lived connection out of this test.
for _ in range(3): click(QemuButton.Button.Down)
click(QemuButton.Button.Select)  # Recent
recent = receive(kind=5)
second = {3: 4242}
payload = json.dumps(dict(items=[dict(captureId=4242, itemId=42, kind=1, state=7, preview='Long answer')])).encode()
send(104, correlation=recent[2], payload=payload)
time.sleep(.3)
click(QemuButton.Button.Select)
fetch = receive(kind=6)
if args.recent_voice_only:
    assert not (fetch[16] & 16), 'Opening an existing answer must stay silent'
    text = 'Saved answer from an older conversation.\n\n' * 45
    deliver_page(fetch, text, generation=6)
    click(QemuButton.Button.Select)
    capture('recent-voice-actions')
    click(QemuButton.Button.Select)  # Play voice reply; Reply is unavailable for generation 6.
    first_play = receive(kind=6)
    assert first_play[3] == 4242 and first_play[12] == 0 and first_play[16] & 16
    deliver_page(first_play, text, generation=6)
    send(114, 4242, payload=b'Voice reply finished')
    # Select playback while a later text page is still in flight.
    pending_page = fetch_while_reading()
    assert pending_page[12] > 0 and not (pending_page[16] & 16)
    click(QemuButton.Button.Select)
    click(QemuButton.Button.Select)
    deliver_page(pending_page, text, generation=6)
    replay = receive(kind=6)
    assert replay[3] == 4242 and replay[12] == 0 and replay[16] & 16
    assert replay[2] != first_play[2], 'Explicit replay needs a fresh request identity'
    deliver_page(replay, text, generation=6)
    send(110, capture_id=900, flags=1, total=2, item_id=4242)
    assert receive(kind=10)[7] == 1
    click(QemuButton.Button.Select)
    click(QemuButton.Button.Down)  # Refresh answer
    click(QemuButton.Button.Select)
    refresh = receive(kind=6)
    assert not (refresh[16] & 16), 'Refresh must not replay speech'
    deliver_page(refresh, text, generation=6)
    click(QemuButton.Button.Back)
    send(110, capture_id=901, flags=1, total=2, item_id=4242)
    assert receive(kind=10)[7] == 8
    assert not faults, faults
    print('Recent answer voice action, old conversation, repeat playback, queued playback during text loading, silent refresh, and Back passed.', flush=True)
    service.shutdown()
    raise SystemExit(0)
large_answer = ('A long answer continues here with mountains, stars, and the Moon. ' * 150) + 'FINAL SENTINEL.'
offset = 0
used = 0
boundary = None
while offset < len(large_answer.encode()):
    assert fetch[12] == offset
    stop = deliver_page(fetch, large_answer)
    if used + stop - offset > 8192:
        boundary = offset
        # Hold Down until the watch reaches its RAM window boundary. The same
        # button must request adjacent text without opening Actions.
        send_data_to_qemu(connection.transport, QemuButton(state=QemuButton.Button.Down))
        try:
            fetch = receive(kind=6, timeout=45)
        finally:
            send_data_to_qemu(connection.transport, QemuButton(state=0))
        assert fetch[12] == boundary
        used = 0
        continue
    used += stop - offset
    offset = stop
    # Reading drives the next request; this also keeps emulator standby from
    # disabling the screenshot service while the fixture sends text.
    if offset < len(large_answer.encode()):
        fetch = fetch_while_reading()
assert boundary is not None
print('Reached the final text window', flush=True)
# Up pages to the top of the final window, then reloads the preceding one.
fetch = fetch_while_reading(button=QemuButton.Button.Up)
assert fetch[12] == 0, 'Up must reload the preceding window'
offset = 0
while offset < boundary:
    assert fetch[12] == offset
    # Down during an in-flight chunk cannot advance beyond this window.
    click(QemuButton.Button.Down)
    offset = deliver_page(fetch, large_answer, end=boundary)
    if offset < boundary:
        fetch = receive(kind=6)
print('Reloaded the preceding text window', flush=True)
click(QemuButton.Button.Back)
click(QemuButton.Button.Back)
assert not faults, faults
print('Forward/backward long-answer windows passed.', flush=True)
service.shutdown()
