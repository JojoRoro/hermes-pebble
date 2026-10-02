"""Emery cycling-score UI and sensor smoke with synthetic input, not field validation."""
import argparse
import queue
import time
from pathlib import Path
from uuid import UUID
from PIL import Image
from pebble_tool.commands.base import PebbleCommand
from pebble_tool.commands.install import ToolAppInstaller
from pebble_tool.commands.emucontrol import send_data_to_qemu, QemuButton, QemuAccel, QemuAccelSample
from libpebble2.services.appmessage import AppMessageService, Uint32, ByteArray
from libpebble2.services.screenshot import Screenshot
from libpebble2.protocol.logs import AppLogMessage, AppLogShippingControl

parser = argparse.ArgumentParser(parents=PebbleCommand._shared_parser())
parser.add_argument('--pbw', default='build/hermes-pebble.pbw')
parser.add_argument('--output', type=Path, default=Path('build/bike-smoke'))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
connection = PebbleCommand()._connect(args)
faults = []
def log(packet):
    print(packet.message, flush=True)
    if 'App fault!' in str(packet.message): faults.append(str(packet.message))
connection.register_endpoint(AppLogMessage, log)
connection.send_packet(AppLogShippingControl(enable=True))
messages = queue.Queue()
service = AppMessageService(connection)
app_id = UUID('7d07aa22-7d13-48c1-a400-2602a5ae4647')
service.register_handler('appmessage', lambda tx, app, data: messages.put(data) if app == app_id else None)
def click(button):
    send_data_to_qemu(connection.transport, QemuButton(state=button))
    time.sleep(.08)
    send_data_to_qemu(connection.transport, QemuButton(state=0))
    time.sleep(.2)
def capture(name):
    rows = Screenshot(connection).grab_image()
    picture = Image.frombytes('RGB', (len(rows[0]) // 3, len(rows)), b''.join(bytes(row) for row in rows))
    picture.save(args.output / (name + '.png'))
    return picture.crop((0, 35, picture.width, 90)).tobytes()
def motion(values):
    send_data_to_qemu(connection.transport, QemuAccel(samples=[QemuAccelSample(x=x, y=0, z=1000) for x in values]))
try:
    ToolAppInstaller(connection, args.pbw, quiet=True).install()
    startup = messages.get(timeout=20)
    assert startup[1] == 1
    fields = {i: Uint32(0) for i in range(18)}
    fields.update({0: Uint32(1), 1: Uint32(101), 2: Uint32(8001), 5: Uint32(1), 6: ByteArray(b''), 17: Uint32(startup[2])})
    service.send_message(app_id, fields)
    time.sleep(.4)
    for _ in range(8): click(QemuButton.Button.Down)
    capture('bike-menu')
    click(QemuButton.Button.Select)
    motion([0])
    capture('collecting')
    time.sleep(6)
    idle = capture('stationary')
    # The emulator consumes its injected sample queue once; it does not loop it.
    for _ in range(9):
        motion([0, 190, 117, -117, -190] * 20)
        time.sleep(2)
    assert capture('vibration') != idle, 'Score did not respond to synthetic road vibration'
    click(QemuButton.Button.Select)
    assert capture('reset') != idle
    motion([0])
    time.sleep(6)
    assert capture('stationary-again') == idle, 'Reset retained old motion evidence'
    click(QemuButton.Button.Back)
    capture('back-to-menu')
    # Re-enter the retained window object; unload/load must recreate its state safely.
    for _ in range(8): click(QemuButton.Button.Down)
    click(QemuButton.Button.Select)
    capture('reopened')
    click(QemuButton.Button.Back)
    click(QemuButton.Button.Back)
    time.sleep(.5)
    assert not faults, faults
    print('Bike menu, score changes, reset, reopen and Back passed.', flush=True)
finally:
    motion([0])
    service.shutdown()
