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
    Image.frombytes('RGB',(len(rows[0])//3,len(rows)),b''.join(bytes(row) for row in rows)).save(args.output / (name + '.png'))

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
for i in range(5): click(QemuButton.Button.Down)
capture('reconnect-selected')
click(QemuButton.Button.Select)
receive() # Transport ACK only: simulate no application response.
click(QemuButton.Button.Back)
click(QemuButton.Button.Back)
time.sleep(.3)
capture('exited')
assert not faults, faults
print('Startup handshake, correlated phone probe, rapid menu navigation and double-Back exercised.',flush=True)
service.shutdown()
