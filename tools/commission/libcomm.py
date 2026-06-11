"""libcomm.py -- frame protocol + Dongle class for the SAMD21 config chip.

Wire protocol (see README / project spec):
  - SLIP framing (RFC 1055) around each frame body.
  - Body = header + payload + 1-byte CRC-8/AUTOSAR over (header+payload).
  - Shell command transport rides on top via OP_SHELL_EXEC / OP_SHELL_REPLY.

This module's framing layer is pure stdlib. `serial` and `cbor2` are imported
lazily inside the methods that need them so that the framing self-test
(`python3 libcomm.py`) runs with nothing but the standard library installed.
"""

import struct

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# SLIP framing (RFC 1055)
SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

# Transport opcodes (the `cmd` field of a frame header)
OP_SHELL_EXEC = 0x0109   # host -> chip
OP_SHELL_REPLY = 0x0011  # chip -> host

# Dongle state-machine handshake (must reach OPERATIONAL before shell-exec is legal).
OP_REGISTER_ACK = 0x0103       # host -> chip: BOOT -> L1_DONE (only if commissioned)
OP_COMMISSION_SET = 0x0105     # host -> chip: set instance_id (fresh/UNCOMMISSIONED chip)
OP_OPERATIONAL_BEGIN = 0x0108  # host -> chip: L1_DONE -> OPERATIONAL

SHELL_STATUS_OK = 0

# Shell command IDs (the shell `command_id` inside an exec body)
CMD_FILE_BEGIN = 0x0123
CMD_FILE_DATA = 0x0124
CMD_FILE_COMMIT = 0x0125
CMD_FILE_LIST = 0x0126
CMD_REG_READ = 0x0127
CMD_REG_WRITE = 0x0128
CMD_REG_READN = 0x0129

# Sizing
COMM_PAYLOAD_MAX = 128
FILE_CHUNK_MAX = 100  # <=100 bytes per CMD_FILE_DATA chunk

# I2C register addresses (reached via CMD_REG_*)
REG_WHO_AM_I = 0x00   # == 0x5A
REG_VERSION = 0x01
REG_MODE = 0x02       # rw
REG_STATUS = 0x03
REG_I2C_ADDR = 0x05
REG_UNIQUE_ID = 0x06  # 0x06..0x0D, 8 bytes

WHO_AM_I_EXPECTED = 0x5A

# FILE bank registers
REG_FILE_NAME = 0x50  # data-port: 4 writes, cursor auto-advances
REG_FILE_CTRL = 0x51  # 1=OPEN, 2=CLOSE, 3=LIST_FIRST, 4=LIST_NEXT
REG_FILE_STAT = 0x52  # 0=OK/open, 1=NOT_FOUND
REG_FILE_SIZE = 0x53
REG_FILE_SEEK = 0x54
REG_FILE_DATA = 0x55  # data-port: CMD_REG_READN streams it

FILE_CTRL_OPEN = 1
FILE_CTRL_CLOSE = 2
FILE_CTRL_LIST_FIRST = 3
FILE_CTRL_LIST_NEXT = 4

FILE_STAT_OK = 0
FILE_STAT_NOT_FOUND = 1

# USB identifiers for enumeration
USB_VID = 0x2886
USB_PID = 0x802F

# Header sizes
M2S_HEADER_LEN = 5
S2M_HEADER_LEN = 7


# ---------------------------------------------------------------------------
# CRC-8/AUTOSAR
# ---------------------------------------------------------------------------

def crc8_autosar(data: bytes) -> int:
    """CRC-8/AUTOSAR: poly 0x2F, init 0xFF, refin/refout false, xorout 0xFF."""
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x2F) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc ^ 0xFF


# ---------------------------------------------------------------------------
# SLIP framing
# ---------------------------------------------------------------------------

def slip_encode(body: bytes) -> bytes:
    """Wrap `body` in a SLIP frame: END + escaped(body) + END."""
    out = bytearray([SLIP_END])
    for b in body:
        if b == SLIP_END:
            out += bytes([SLIP_ESC, SLIP_ESC_END])
        elif b == SLIP_ESC:
            out += bytes([SLIP_ESC, SLIP_ESC_ESC])
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


class SlipDecoder:
    """Incremental SLIP decoder.

    Feed bytes with `feed()`; it returns a list of complete, unescaped frame
    bodies decoded so far. Leading/empty frames (e.g. back-to-back END bytes)
    are skipped.
    """

    def __init__(self):
        self._buf = bytearray()
        self._esc = False
        self._in_frame = False

    def feed(self, data: bytes) -> list:
        frames = []
        for b in data:
            if b == SLIP_END:
                if self._in_frame and self._buf:
                    frames.append(bytes(self._buf))
                self._buf = bytearray()
                self._esc = False
                self._in_frame = True
                continue
            # Any non-END byte starts/continues a frame.
            self._in_frame = True
            if self._esc:
                if b == SLIP_ESC_END:
                    self._buf.append(SLIP_END)
                elif b == SLIP_ESC_ESC:
                    self._buf.append(SLIP_ESC)
                else:
                    # Protocol violation; pass the raw byte through.
                    self._buf.append(b)
                self._esc = False
            elif b == SLIP_ESC:
                self._esc = True
            else:
                self._buf.append(b)
        return frames


# ---------------------------------------------------------------------------
# Frame encode / decode
# ---------------------------------------------------------------------------

def encode_m2s(addr: int, cmd: int, seq: int, payload: bytes) -> bytes:
    """Build a full m2s wire frame: SLIP(END + header+payload+crc + END)."""
    header = bytes([
        addr & 0xFF,
        cmd & 0xFF,
        (cmd >> 8) & 0xFF,
        seq & 0xFF,
        len(payload) & 0xFF,
    ])
    body = header + payload
    crc = crc8_autosar(body)
    return slip_encode(body + bytes([crc]))


def parse_s2m(body: bytes):
    """Parse a SLIP-decoded s2m body.

    Returns (addr, cmd, seq, ack_seq, ack_status, payload).
    Raises ValueError on short frame or CRC mismatch.
    """
    if len(body) < S2M_HEADER_LEN + 1:
        raise ValueError("s2m frame too short: %d bytes" % len(body))
    crc_rx = body[-1]
    hdr_pl = body[:-1]
    crc_calc = crc8_autosar(hdr_pl)
    if crc_rx != crc_calc:
        raise ValueError(
            "CRC mismatch: got 0x%02X expected 0x%02X" % (crc_rx, crc_calc)
        )
    addr = hdr_pl[0]
    cmd = hdr_pl[1] | (hdr_pl[2] << 8)
    seq = hdr_pl[3]
    ack_seq = hdr_pl[4]
    ack_status = hdr_pl[5]
    payload_len = hdr_pl[6]
    payload = hdr_pl[S2M_HEADER_LEN:S2M_HEADER_LEN + payload_len]
    return addr, cmd, seq, ack_seq, ack_status, payload


# ---------------------------------------------------------------------------
# Dongle
# ---------------------------------------------------------------------------

class Dongle:
    """Talks to one SAMD21 config chip over USB-CDC (ttyACM)."""

    def __init__(self, port, timeout=1.0):
        import serial  # lazy
        self.port = port
        self.timeout = timeout
        self._ser = serial.Serial(port, 115200, timeout=timeout)
        import time as _t
        self._seq = 0
        # Seed req_id uniquely per process so a stale reply left in the chip's
        # TX ring from an earlier session can't match by a colliding req_id.
        self._req_id = int(_t.monotonic() * 1000.0) & 0xFFFF
        self._decoder = SlipDecoder()
        self._handshake()

    def close(self):
        try:
            self._ser.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- low-level ----------------------------------------------------------

    def _next_seq(self):
        self._seq = (self._seq + 1) & 0xFF
        return self._seq

    def _next_req_id(self):
        self._req_id = (self._req_id + 1) & 0xFFFF
        return self._req_id

    def _write_frame(self, cmd, payload=b""):
        self._ser.write(encode_m2s(1, cmd, self._next_seq(), bytes(payload)))
        self._ser.flush()

    def _drain(self, dur):
        import time
        t = time.monotonic()
        while time.monotonic() - t < dur:
            self._ser.read(256)
        self._ser.reset_input_buffer()

    def _handshake(self):
        """Advance the dongle BOOT -> L1_DONE -> OPERATIONAL so shell-exec is
        legal. Assumes the chip is already commissioned (instance_id set); a
        fresh UNCOMMISSIONED chip first needs OP_COMMISSION_SET (+ reboot),
        not yet automated here.
        """
        import time
        time.sleep(0.3)                      # let boot frames (REGISTER/DBG_LOG) flow
        self._drain(0.4)
        self._write_frame(OP_REGISTER_ACK)   # BOOT -> L1_DONE
        time.sleep(0.1)
        self._write_frame(OP_OPERATIONAL_BEGIN)  # L1_DONE -> OPERATIONAL
        time.sleep(0.15)
        self._drain(0.2)                     # clear transition logs + heartbeats
        self._decoder = SlipDecoder()

    def shell_exec(self, command_id, args=b"") -> tuple:
        """Send OP_SHELL_EXEC, await the matching OP_SHELL_REPLY.

        Returns (status:int, result:bytes). Raises TimeoutError if no matching
        reply arrives within `timeout`.
        """
        import time
        req_id = self._next_req_id()
        exec_body = struct.pack("<HH", req_id, command_id) + bytes(args)
        frame = encode_m2s(1, OP_SHELL_EXEC, self._next_seq(), exec_body)
        self._ser.reset_input_buffer()   # drop stale/heartbeat frames before this request
        self._decoder = SlipDecoder()
        self._ser.write(frame)
        self._ser.flush()

        deadline = time.monotonic() + self.timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    "no OP_SHELL_REPLY for request_id %d" % req_id
                )
            chunk = self._ser.read(256)
            if not chunk:
                continue
            for body in self._decoder.feed(chunk):
                try:
                    _addr, cmd, _seq, _ack_seq, _ack_st, payload = parse_s2m(body)
                except ValueError:
                    continue  # bad CRC / short frame; ignore
                if cmd != OP_SHELL_REPLY:
                    continue
                if len(payload) < 3:
                    continue
                rx_req_id, status = struct.unpack("<HB", payload[:3])
                if rx_req_id != req_id:
                    continue
                result = payload[3:]
                return status, result

    # -- register access ----------------------------------------------------

    def reg_read(self, reg: int) -> int:
        status, result = self.shell_exec(CMD_REG_READ, bytes([reg & 0xFF]))
        if status != SHELL_STATUS_OK:
            raise IOError("reg_read(0x%02X) status %d" % (reg, status))
        if len(result) < 1:
            raise IOError("reg_read(0x%02X) empty result" % reg)
        return result[0]

    def reg_write(self, reg: int, val: int) -> None:
        status, _ = self.shell_exec(
            CMD_REG_WRITE, bytes([reg & 0xFF, val & 0xFF])
        )
        if status != SHELL_STATUS_OK:
            raise IOError(
                "reg_write(0x%02X, 0x%02X) status %d" % (reg, val, status)
            )

    def reg_readn(self, reg: int, n: int) -> bytes:
        status, result = self.shell_exec(
            CMD_REG_READN, bytes([reg & 0xFF, n & 0xFF])
        )
        if status != SHELL_STATUS_OK:
            raise IOError("reg_readn(0x%02X, %d) status %d" % (reg, n, status))
        return result[:n]

    # -- high-level helpers -------------------------------------------------

    def whoami(self) -> int:
        return self.reg_read(REG_WHO_AM_I)

    def chip_uid(self) -> bytes:
        return self.reg_readn(REG_UNIQUE_ID, 8)

    def mode(self, set=None) -> int:
        """Read MODE (reg 0x02); if `set` given, write it first. Returns value."""
        if set is not None:
            self.reg_write(REG_MODE, set & 0xFF)
        return self.reg_read(REG_MODE)

    # -- file transport (write side, via shell file commands) ---------------

    @staticmethod
    def _name4(name) -> bytes:
        """4-char name, space-padded / truncated to exactly 4 bytes."""
        if isinstance(name, str):
            raw = name.encode("ascii")
        else:
            raw = bytes(name)
        raw = raw[:4]
        return raw + b" " * (4 - len(raw))

    def file_put(self, name, data: bytes) -> None:
        """Upload `data` under 4-char `name`: BEGIN -> DATA chunks -> COMMIT."""
        n4 = self._name4(name)
        status, _ = self.shell_exec(CMD_FILE_BEGIN, n4)
        if status != SHELL_STATUS_OK:
            raise IOError("FILE_BEGIN(%r) status %d" % (name, status))

        view = memoryview(data)
        off = 0
        while off < len(view):
            chunk = bytes(view[off:off + FILE_CHUNK_MAX])
            status, _ = self.shell_exec(CMD_FILE_DATA, chunk)
            if status != SHELL_STATUS_OK:
                raise IOError(
                    "FILE_DATA @%d status %d" % (off, status)
                )
            off += len(chunk)

        status, _ = self.shell_exec(CMD_FILE_COMMIT, b"")
        if status != SHELL_STATUS_OK:
            raise IOError("FILE_COMMIT(%r) status %d" % (name, status))

    def file_list(self) -> list:
        """Return [(name:str, length:int), ...] via CMD_FILE_LIST."""
        status, result = self.shell_exec(CMD_FILE_LIST, b"")
        if status != SHELL_STATUS_OK:
            raise IOError("FILE_LIST status %d" % status)
        if len(result) < 1:
            return []
        count = result[0]
        out = []
        pos = 1
        for _ in range(count):
            if pos + 5 > len(result):
                break
            name = result[pos:pos + 4].decode("ascii", "replace").rstrip()
            length = result[pos + 4]
            out.append((name, length))
            pos += 5
        return out

    def file_get(self, name) -> bytes:
        """Read a file by name by driving the FILE bank via reg_* access."""
        n4 = self._name4(name)
        # Burst the 4 name bytes into the FILE_NAME data-port (cursor advances).
        for b in n4:
            self.reg_write(REG_FILE_NAME, b)
        # Open it.
        self.reg_write(REG_FILE_CTRL, FILE_CTRL_OPEN)
        if self.reg_read(REG_FILE_STAT) != FILE_STAT_OK:
            raise FileNotFoundError("file %r not found" % name)
        size = self.reg_read(REG_FILE_SIZE)
        data = self.reg_readn(REG_FILE_DATA, size)
        return data


# ---------------------------------------------------------------------------
# Enumeration
# ---------------------------------------------------------------------------

def enumerate_dongles() -> list:
    """Return [{'port':..., 'serial':...}, ...] for matching USB-CDC dongles."""
    from serial.tools import list_ports  # lazy
    out = []
    for p in list_ports.comports():
        if p.vid == USB_VID and p.pid == USB_PID:
            out.append({"port": p.device, "serial": p.serial_number})
    return out


# ---------------------------------------------------------------------------
# Self-test (stdlib only)
# ---------------------------------------------------------------------------

def selftest():
    # 1) CRC vector.
    crc = crc8_autosar(b"123456789")
    assert crc == 0xDF, "CRC-8/AUTOSAR check: got 0x%02X expected 0xDF" % crc

    # 2) m2s frame round-trip with a plain payload.
    addr, cmd, seq = 1, OP_SHELL_EXEC, 7
    payload = bytes([0x10, 0x20, 0x30, 0x40])
    frame = encode_m2s(addr, cmd, seq, payload)
    assert frame[0] == SLIP_END and frame[-1] == SLIP_END

    dec = SlipDecoder()
    bodies = dec.feed(frame)
    assert len(bodies) == 1, "expected 1 decoded body, got %d" % len(bodies)
    body = bodies[0]

    # Verify header fields.
    assert body[0] == addr
    assert (body[1] | (body[2] << 8)) == cmd
    assert body[3] == seq
    assert body[4] == len(payload)
    got_payload = body[M2S_HEADER_LEN:M2S_HEADER_LEN + len(payload)]
    assert got_payload == payload, "payload mismatch"

    # Verify CRC byte (last byte) over header+payload.
    crc_rx = body[-1]
    crc_calc = crc8_autosar(body[:-1])
    assert crc_rx == crc_calc, "frame CRC mismatch"

    # 3) Escape path: payload containing END (0xC0) and ESC (0xDB) bytes.
    esc_payload = bytes([0xC0, 0xDB, 0x00, 0xC0, 0xDB, 0xFF])
    frame2 = encode_m2s(2, CMD_REG_READN, 0xAB, esc_payload)
    # The wire frame must NOT contain a raw 0xC0/0xDB inside the body region.
    inner = frame2[1:-1]  # strip leading/trailing END
    # After escaping, the only way 0xC0 appears is as a frame delimiter, which
    # we've stripped; inner should contain no raw END byte.
    assert SLIP_END not in inner, "raw END leaked into escaped body"

    dec2 = SlipDecoder()
    bodies2 = dec2.feed(frame2)
    assert len(bodies2) == 1
    body2 = bodies2[0]
    got2 = body2[M2S_HEADER_LEN:M2S_HEADER_LEN + len(esc_payload)]
    assert got2 == esc_payload, "escaped payload round-trip failed: %r" % got2
    assert body2[-1] == crc8_autosar(body2[:-1]), "escaped frame CRC mismatch"

    # 4) parse_s2m round-trip (build a synthetic s2m body and parse it).
    s2m_hdr = bytes([1, OP_SHELL_REPLY & 0xFF, (OP_SHELL_REPLY >> 8) & 0xFF,
                     5, 7, SHELL_STATUS_OK, 3])
    s2m_pl = struct.pack("<HB", 0x1234, 0x00)  # req_id=0x1234, status byte
    s2m_body = s2m_hdr + s2m_pl
    s2m_full = s2m_body + bytes([crc8_autosar(s2m_body)])
    a, c, sq, asq, ast, pl = parse_s2m(s2m_full)
    assert a == 1 and c == OP_SHELL_REPLY and sq == 5
    assert asq == 7 and ast == SHELL_STATUS_OK and pl == s2m_pl

    print("PASS")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(selftest())
