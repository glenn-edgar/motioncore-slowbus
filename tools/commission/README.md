# tools/commission — SAMD21 config commissioning (Python)

Writes named CBOR config files to a SAMD21 over USB-CDC (ttyACM); the Pico reads
them back by name over I2C. Design: [docs/samd21-config-fs.md](../../docs/samd21-config-fs.md).

## Setup — WSL/Debian needs a venv (PEP 668)
System Python is externally-managed, so `pip install` into it is refused. Create
a venv; do **not** use `--break-system-packages`:
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r tools/commission/requirements.txt
```

## Use (planned)
```bash
# JSON source -> canonical CBOR -> flash, under the name "node"
python tools/commission/commission.py --port /dev/ttyACM0 put node config/node.json
python tools/commission/commission.py --port /dev/ttyACM0 list
python tools/commission/commission.py --port /dev/ttyACM0 get node      # read back, decode CBOR
```
Live commissioning runs on the **Pi** (the dongle enumerates there). From a WSL
dev box, pass the device through with `usbipd-win` first (see
docs/toolchain-wsl.md). `commission.py` itself is not written yet — see the
phasing in the design doc.
