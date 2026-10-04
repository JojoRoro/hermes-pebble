"""Transfer the bundled speech clip to the emery emulator; speaker audibility requires hardware."""
import argparse
import queue
import time
from pathlib import Path
from uuid import UUID
from pebble_tool.commands.base import PebbleCommand
from pebble_tool.commands.install import ToolAppInstaller
from libpebble2.services.appmessage import AppMessageService, Uint32, ByteArray
from libpebble2.protocol.logs import AppLogMessage, AppLogShippingControl

parser = argparse.ArgumentParser(parents=PebbleCommand._shared_parser())
parser.add_argument('--pbw', default='build/hermes-pebble.pbw')
parser.add_argument('--full-reply', action='store_true', help='Stream a 24-second compressed reply while playback is active')
args = parser.parse_args()
connection = PebbleCommand()._connect(args)
faults = []
speaker_finishes = []
def log(packet):
    if 'App fault!' in str(packet.message): faults.append(str(packet.message))
    if 'Audio ' in str(packet.message): print(packet.message, flush=True)
    if 'Audio speaker finish reason=' in str(packet.message): speaker_finishes.append(str(packet.message))
connection.register_endpoint(AppLogMessage, log)
connection.send_packet(AppLogShippingControl(enable=True))
messages = queue.Queue()
service = AppMessageService(connection)
app_id = UUID('7d07aa22-7d13-48c1-a400-2602a5ae4647')
service.register_handler('appmessage', lambda tx, app, data: messages.put(data) if app == app_id else None)
transport_receipts = queue.Queue()
service.register_handler('ack', lambda tx, app: transport_receipts.put((tx, True)))
service.register_handler('nack', lambda tx, app: transport_receipts.put((tx, False)))
transfer = 9000
clip = Path('android/app/src/main/assets/watch_test.s8').read_bytes()
if args.full_reply:
    clip *= 14
audio_format = 3 if args.full_reply else 1
encoded = Path("build/audio-stream.adpcm").read_bytes() if args.full_reply else clip
checksum = 0x811c9dc5
for byte in encoded: checksum = ((checksum ^ byte) * 0x01000193) & 0xffffffff

def receive(kind, correlation=None, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = messages.get(timeout=max(.1, deadline - time.monotonic()))
        if data.get(1) == kind and (correlation is None or data.get(17) == correlation): return data
    raise AssertionError('No matching watch response')

def send(kind, payload=b'', offset=0, correlation=0):
    global transfer
    transfer += 1
    chunk_size = 768 if kind == 111 else 192
    chunks = [payload[i:i + chunk_size] for i in range(0, len(payload), chunk_size)] or [b'']
    indexed = list(enumerate(chunks))
    if kind == 111:
        indexed.reverse() # The compact assembler must preserve logical chunk order.
        if not args.full_reply:
            indexed.insert(1, indexed[0]) # A retry must not consume space twice.
    for index, chunk in indexed:
        fields = {i: Uint32(0) for i in range(18)}
        fields.update({0: Uint32(1), 1: Uint32(kind), 2: Uint32(transfer), 3: Uint32(777),
            4: Uint32(index), 5: Uint32(len(chunks)), 6: ByteArray(chunk), 12: Uint32(offset),
            14: Uint32(len(clip)), 15: Uint32(checksum if kind != 101 else 0),
            16: Uint32(audio_format if kind != 101 else 0), 17: Uint32(correlation)})
        tx = service.send_message(app_id, fields)
        if args.full_reply:
            # Match Android's sendDataToPebble behavior: never flood dictionaries
            # before the watch transport acknowledges the preceding one.
            while True:
                ack_tx, accepted = transport_receipts.get(timeout=10)
                if ack_tx == tx:
                    assert accepted, 'Watch transport rejected a dictionary'
                    break
            time.sleep(.075)
        else:
            time.sleep(.10)
    return transfer

try:
    ToolAppInstaller(connection, args.pbw, quiet=True).install()
    startup = receive(1)
    send(101, correlation=startup[2])
    time.sleep(.3)
    request = send(110)
    ready = receive(10, request)
    assert ready[7] == 1, ready
    # Full wire path must accept non-UTF-8 PCM and reject an incomplete play.
    request = send(112)
    assert receive(10, request)[7] == 6
    upload_started = time.monotonic()
    block_size = 1529 if args.full_reply else 1024
    playback_started = None
    play_request = None
    encoded_offset = 0
    for offset in range(0, len(clip), block_size):
        samples = min(block_size, len(clip) - offset)
        if args.full_reply:
            length = 4 + samples // 2
            payload = encoded[encoded_offset:encoded_offset + length]
            encoded_offset += length
        else:
            payload = clip[offset:offset + samples]
        while True:
            request = send(111, payload, offset)
            reply = receive(10, request)
            if reply[7] != 11: break
            assert reply[12] == offset
            time.sleep(.10)
        assert reply[7] == 2 and reply[12] == offset + samples, reply
        if args.full_reply and play_request is None and offset + samples >= 12288:
            playback_started = time.monotonic()
            print(f"Starting playback after {offset + samples}/{len(clip)} samples, {playback_started - upload_started:.3f}s initial buffering.", flush=True)
            play_request = send(112)
            assert offset + samples < len(clip) / 10
    upload_elapsed = time.monotonic() - upload_started
    if play_request is None:
        playback_started = time.monotonic()
        play_request = send(112)
    request = play_request
    result = receive(10, request, timeout=len(clip) / 8000 + 15)
    assert result[7] == 3, result
    assert result[12] == len(clip) and result[15] == checksum
    elapsed = time.monotonic() - playback_started
    # Emulator wall-clock duration is a coarse integration check.
    # Exact sample count is checked by the host pump test; this catches gross
    # speed errors and upload-sized gaps, not small backend clock differences.
    expected = len(clip) / 8000
    assert expected * .9 - .25 <= elapsed <= expected * 1.1 + 1, f'Unexpected playback duration: {elapsed:.3f}s'
    request = send(112)  # Same session must acknowledge without replaying.
    assert receive(10, request)[7] == 3
    assert len(speaker_finishes) == 1, speaker_finishes
    assert not faults, faults
    print(f'Audio transfer and single playback verified: {len(clip)} bytes, checksum {checksum:08x}, upload {upload_elapsed:.3f}s, playback {elapsed:.3f}s.')
finally:
    service.shutdown()
