"""Emery transport/navigation smoke; run with the Pebble CLI Python environment."""
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
parser.add_argument('--output', type=Path, default=Path('build/watch-smoke'))
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

def receive(correlation=None):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        data = messages.get(timeout=max(.1, deadline-time.monotonic()))
        print('Watch RX kind', data.get(1), 'transfer', data.get(2), 'correlation', data.get(17), flush=True)
        if data.get(1) == 1 and (correlation is None or data.get(17) == correlation): return data
    raise AssertionError('No matching handshake')

def reply(transfer, correlation):
    fields = {i: Uint32(0) for i in range(18)}
    fields.update({0:Uint32(1),1:Uint32(101),2:Uint32(transfer),5:Uint32(1),6:ByteArray(b''),17:Uint32(correlation)})
    service.send_message(app_id, fields)

def capture(name):
    rows=Screenshot(connection).grab_image()
    picture = Image.frombytes('RGB',(len(rows[0])//3,len(rows)),b''.join(bytes(row) for row in rows))
    picture.save(args.output / (name + '.png'))
    return picture.crop((8, 48, picture.width - 8, picture.height - 30)).tobytes()

def touch_row(name):
    # Settings also shows the live link state; compare only the touch preference row.
    rows=Screenshot(connection).grab_image()
    picture = Image.frombytes('RGB',(len(rows[0])//3,len(rows)),b''.join(bytes(row) for row in rows))
    picture.save(args.output / (name + '.png'))
    return picture.crop((0, 24, picture.width, 76)).tobytes()

def click(button):
    send_data_to_qemu(connection.transport,QemuButton(state=button))
    time.sleep(.08)
    send_data_to_qemu(connection.transport,QemuButton(state=0))
    time.sleep(.18)

ToolAppInstaller(connection,args.pbw,quiet=True).install()
startup=receive()
time.sleep(.3)
capture('before-reply')
assert startup.get(17)==0
reply(4001,startup[2])
time.sleep(.3)
capture('linked')
reply(4002,0)
probe=receive(4002)
reply(4003,probe[2])
time.sleep(.3)
capture('probe-linked')
for i in range(6): click(QemuButton.Button.Down)
capture('settings-selected')
click(QemuButton.Button.Select)  # Settings
click(QemuButton.Button.Down)
capture('reconnect-selected')
click(QemuButton.Button.Select)
request = receive()
# Long diagnostic uses the same text-page renderer as answers/review/status.
text = b'First line\nSecond line\nThird line\nFourth line\nFifth line\nSixth line\nSeventh line\nLast line'
fields = {i: Uint32(0) for i in range(18)}
fields.update({0: Uint32(1), 1: Uint32(107), 2: Uint32(4004),
               5: Uint32(1), 6: ByteArray(text), 8: Uint32(2), 17: Uint32(request[2])})
service.send_message(app_id, fields)
time.sleep(.3)
top = capture('text-top')
click(QemuButton.Button.Down)
assert capture('text-down') != top, 'Down did not move the text'
click(QemuButton.Button.Up)
assert capture('text-up') == top, 'Up did not return to the original text'
for _ in range(12): click(QemuButton.Button.Down)
bottom = capture('text-bottom')
click(QemuButton.Button.Down)
assert capture('text-bottom-clamped') == bottom, 'Scrolling beyond the bottom'
for _ in range(12): click(QemuButton.Button.Up)
assert capture('text-top-restored') == top, 'Could not return to the top'

click(QemuButton.Button.Back)
for _ in range(6): click(QemuButton.Button.Down)
click(QemuButton.Button.Select)
on = touch_row('settings-on')
click(QemuButton.Button.Select)
off = touch_row('settings-off')
assert on != off, 'Touch preference did not toggle'
click(QemuButton.Button.Back)
click(QemuButton.Button.Back)
time.sleep(.3)
capture('exited')
from libpebble2.protocol.apps import AppRunState, AppRunStateStart
connection.send_packet(AppRunState(command=1, data=AppRunStateStart(uuid=app_id)))
reopened = receive()
reply(4005, reopened[2])
time.sleep(.3)
for _ in range(6): click(QemuButton.Button.Down)
click(QemuButton.Button.Select)
assert touch_row('settings-after-restart') == off, 'Touch preference was not preserved'
click(QemuButton.Button.Select)
assert touch_row('settings-on-restored') == on, 'Could not re-enable touch with buttons'
click(QemuButton.Button.Back)
for _ in range(2): click(QemuButton.Button.Down)
click(QemuButton.Button.Select)
blank_ink = capture('handwriting-canvas')
click(QemuButton.Button.Select)
assert capture('handwriting-empty-not-saved') == blank_ink, 'Empty ink was saved or drawing was closed'
click(QemuButton.Button.Back)
click(QemuButton.Button.Back)
assert not faults, faults
print('Handshakes, text scrolling bounds, Settings persistence, handwriting menu/canvas/empty-save, and Back exercised.',flush=True)
service.shutdown()
