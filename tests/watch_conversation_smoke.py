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

def receive(correlation=None, kind=1):
    deadline = time.monotonic() + 20
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

def send(kind, capture_id=0, correlation=0, status=0, flags=0, payload=b'', offset=0, total=0):
    global transfer
    transfer += 1
    chunks = [payload[i:i+192] for i in range(0, len(payload), 192)] or [b'']
    for i, chunk in enumerate(chunks):
        fields = {key: Uint32(0) for key in range(18)}
        fields.update({0:Uint32(1), 1:Uint32(kind), 2:Uint32(transfer), 3:Uint32(capture_id),
                       4:Uint32(i), 5:Uint32(len(chunks)), 6:ByteArray(chunk), 7:Uint32(status),
                       9:Uint32(42), 10:Uint32(status), 11:Uint32(1), 12:Uint32(offset),
                       14:Uint32(total), 15:Uint32(7), 16:Uint32(flags), 17:Uint32(correlation)})
        service.send_message(app_id, fields)
        time.sleep(.15)

def dictate(words):
    app = voice_sessions.get(timeout=10)
    time.sleep(.5)
    voice.send_stop_audio()
    voice.send_dictation_result(TranscriptionResult.Success, [words.split()], app_uuid=app)
    time.sleep(1.5)
    capture('after-dictation')

def send_review():
    click(QemuButton.Button.Select)  # Review actions
    capture('review-actions')
    click(QemuButton.Button.Select)  # Send
    return receive(kind=2)

def receipt(request):
    send(102, request[3], request[2], status=2, flags=8)

def finish(request, text):
    send(103, request[3], status=7)
    # No user input here: completion must trigger FETCH_RESULT automatically.
    fetch = receive(kind=6)
    assert fetch[3] == request[3] and fetch[12] == 0
    payload = json.dumps(dict(captureId=request[3], itemId=42, state=7, output=text, more=False)).encode()
    send(105, request[3], fetch[2], status=7, payload=payload, total=len(text.encode()))
    time.sleep(.3)
    return capture('automatic-answer')

ToolAppInstaller(connection,args.pbw,quiet=True).install()
startup = receive()
send(101, correlation=startup[2])
time.sleep(.3)
click(QemuButton.Button.Select)  # Ask Hermes
# Local voice fixture: never calls a speech service or Hermes.
dictate('Tell me about the moon')
first = send_review()
assert first[15] == 7
receipt(first)
send(103, first[3], status=5)
capture('working')
answer = finish(first, 'The Moon orbits Earth. Reply to ask a follow-up question.')
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
print('Automatic answer, duplicate status, Reply generation, fast completion/receipt race, and Back passed.', flush=True)
service.shutdown()
