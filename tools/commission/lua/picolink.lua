-- picolink.lua -- LuaJIT USB-CDC client for the Pico bus_controller (and slave).
--
-- The Pico firmware speaks the vendored libcomm wire codec (SLIP + CRC-8/AUTOSAR,
-- m2s 5-byte header / s2m 7-byte header) -- see port/rp2040/vendor/libcomm/frame.*
-- and opcodes.h. This is the host side of that link.
--
-- It is deliberately SEPARATE from the SAMD21 libcomm.lua: that module hardwires
-- the shell address to 1 and is moving out to ~/xiao_blocks. Here the frame addr
-- is a parameter, because the BC routes by it: addr 0 = the BC's own local shell,
-- addr 0xFB = the core1 appcore (KB0 monitor / API), addr 1 = s2m from the device
-- (REGISTER / DBG_LOG). Read-only `listen()` decodes everything; `exec()` does a
-- request/reply round-trip matched on req_id.

local ffi = require("ffi")
local bit = require("bit")

ffi.cdef [[
    int   open(const char *path, int flags);
    int   close(int fd);
    long  read(int fd, void *buf, unsigned long n);
    long  write(int fd, const void *buf, unsigned long n);
    char *strerror(int errnum);
    struct termios { unsigned int c_iflag,c_oflag,c_cflag,c_lflag;
                     unsigned char c_line; unsigned char c_cc[32];
                     unsigned int c_ispeed,c_ospeed; };
    int  tcgetattr(int fd, struct termios *t);
    int  tcsetattr(int fd, int act, const struct termios *t);
    void cfmakeraw(struct termios *t);
    int  cfsetspeed(struct termios *t, unsigned int speed);
    struct pollfd { int fd; short events; short revents; };
    int  poll(struct pollfd *fds, unsigned long n, int timeout_ms);
    struct timeval { long tv_sec; long tv_usec; };
    int  gettimeofday(struct timeval *tv, void *tz);
    int  usleep(unsigned int usec);
    /* TCP server (agent mode): the BC dials in over WiFi, we accept + speak host_link. */
    int  socket(int domain, int type, int protocol);
    int  bind(int fd, const void *addr, unsigned addrlen);
    int  listen(int fd, int backlog);
    int  accept(int fd, void *addr, unsigned *addrlen);
    int  connect(int fd, const void *addr, unsigned addrlen);
    long recvfrom(int fd, void *buf, unsigned long n, int flags, void *addr, unsigned *addrlen);
    int  setsockopt(int fd, int level, int optname, const void *optval, unsigned optlen);
    unsigned short htons(unsigned short x);
    struct sockaddr_in { short sin_family; unsigned short sin_port; unsigned int sin_addr; char sin_zero[8]; };
]]
local C = ffi.C
local O_RDWR, O_NOCTTY, O_NONBLOCK, TCSANOW, B115200, POLLIN = 0x0002, 0x100, 0x800, 0, 4098, 0x001

local M = {}

-- Pico (RP2040/RP2350) USB VID. The running app CDC enumerates as 2e8a:000a;
-- the BOOTSEL mass-storage device is 2e8a:0003/000f and won't match here.
M.PICO_VID = "2e8a"

-- ---- wire opcodes (port/rp2040/vendor/libcomm/opcodes.h) -------------------
M.OP_REGISTER, M.OP_HEARTBEAT, M.OP_PONG          = 0x0001, 0x0002, 0x0005
M.OP_NAK, M.OP_MANIFEST_REPLY                     = 0x0007, 0x0008
M.OP_DBG_LOG, M.OP_SHELL_REPLY                    = 0x0010, 0x0011
M.OP_POLL_REPLY, M.OP_EVENT                       = 0x0012, 0x0013
M.OP_SHELL_EXEC, M.OP_PING                        = 0x0109, 0x0104
M.SHELL_STATUS_OK = 0

M.ADDR_LOCAL_SHELL = 0x00   -- BC's own shell (on_local_shell)
M.ADDR_APPCORE     = 0xFB   -- core1 virtual slave (KB0 monitor / API)
M.CMD_MON_PING     = 0x0200 -- KB0 liveness round-trip
M.CMD_GPIO_WRITE   = 0x0101 -- [port][pin][level]
M.CMD_GPIO_READ    = 0x0102 -- [port][pin] -> [level]
M.CMD_INTERLOCK_CLEAR  = 0x0210 -- Thread 2: global clear of latched trips
M.CMD_INTERLOCK_STATUS = 0x0211 -- Thread 2: [ver][gveto][n] + n*[slot][state][tf][latched]

M.OP_NAME = {
    [0x0001]="REGISTER", [0x0002]="HEARTBEAT", [0x0005]="PONG",
    [0x0006]="COMMISSION_REPLY", [0x0007]="NAK", [0x0008]="MANIFEST_REPLY",
    [0x0010]="DBG_LOG", [0x0011]="SHELL_REPLY", [0x0012]="POLL_REPLY",
    [0x0013]="EVENT", [0x0014]="RS485_FRAME_RX", [0x0015]="BUS_SLAVE_DOWN",
    [0x0016]="BUS_SLAVE_UP", [0x0017]="BUS_SLAVE_FLAGGED", [0x0018]="BUS_CMD_ACK",
    [0x0019]="BUS_CMD_NAK", [0x001A]="BUS_INTERLOCK_MSG", [0x001B]="BUS_STATUS_REPORT",
}

-- ---- codec ----------------------------------------------------------------
local SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD

local function crc8(t, n)
    local crc = 0xFF
    for i = 1, n do
        crc = bit.bxor(crc, t[i])
        for _ = 1, 8 do
            if bit.band(crc, 0x80) ~= 0 then crc = bit.band(bit.bxor(bit.lshift(crc, 1), 0x2F), 0xFF)
            else crc = bit.band(bit.lshift(crc, 1), 0xFF) end
        end
    end
    return bit.band(bit.bxor(crc, 0xFF), 0xFF)
end
M._crc8 = crc8

-- encode an m2s frame -> wire string. `addr` selects the destination route.
local function encode_m2s(addr, cmd, seq, payload)
    local body = { addr, bit.band(cmd, 0xFF), bit.band(bit.rshift(cmd, 8), 0xFF), seq, #payload }
    for i = 1, #payload do body[#body + 1] = payload:byte(i) end
    body[#body + 1] = crc8(body, #body)
    local wire = { SLIP_END }
    for _, b in ipairs(body) do
        if b == SLIP_END then wire[#wire+1] = SLIP_ESC; wire[#wire+1] = SLIP_ESC_END
        elseif b == SLIP_ESC then wire[#wire+1] = SLIP_ESC; wire[#wire+1] = SLIP_ESC_ESC
        else wire[#wire+1] = b end
    end
    wire[#wire + 1] = SLIP_END
    local out = {}
    for i = 1, #wire do out[i] = string.char(wire[i]) end
    return table.concat(out)
end
M._encode_m2s = encode_m2s

-- incremental s2m SLIP decoder. on_frame({addr,cmd,seq,ack_seq,ack_status,payload=<byte array>})
local function make_decoder() return { in_frame = false, escaped = false, buf = {} } end
local function decoder_feed(dec, b, on_frame)
    if b == SLIP_END then
        local f = dec.buf
        if dec.in_frame and #f >= 8 then
            local len = f[7]                          -- s2m header is 7 bytes
            if #f == 7 + len + 1 and crc8(f, #f - 1) == f[#f] then
                local payload = {}
                for i = 8, 7 + len do payload[#payload + 1] = f[i] end
                on_frame({ addr = f[1], cmd = bit.bor(f[2], bit.lshift(f[3], 8)),
                           seq = f[4], ack_seq = f[5], ack_status = f[6], payload = payload })
            end
        end
        dec.in_frame, dec.escaped, dec.buf = true, false, {}
        return
    end
    if not dec.in_frame then return end
    if dec.escaped then
        if b == SLIP_ESC_END then dec.buf[#dec.buf+1] = SLIP_END
        elseif b == SLIP_ESC_ESC then dec.buf[#dec.buf+1] = SLIP_ESC
        else dec.buf[#dec.buf+1] = b end
        dec.escaped = false
    elseif b == SLIP_ESC then dec.escaped = true
    else dec.buf[#dec.buf+1] = b end
end
M._make_decoder, M._decoder_feed = make_decoder, decoder_feed

-- ---- enumeration (sysfs) --------------------------------------------------
local function read1(path) local f = io.open(path, "r"); if not f then return nil end
    local s = (f:read("*l") or ""):gsub("^%s+", ""):gsub("%s+$", ""); f:close(); return s end

function M.enumerate()
    local out = {}
    local pf = io.popen("ls -1 /dev/ttyACM* 2>/dev/null")
    if not pf then return out end
    for path in pf:lines() do
        local name = path:match("([^/]+)$")
        local rl = io.popen("readlink -f /sys/class/tty/" .. name .. "/device 2>/dev/null")
        local real = rl and rl:read("*l"); if rl then rl:close() end
        if real and real ~= "" then
            local usb = real:match("^(.*)/[^/]+$")
            local vid = usb and read1(usb .. "/idVendor")
            if vid and vid:lower() == M.PICO_VID then
                out[#out + 1] = { port = path, serial = read1(usb .. "/serial") }
            end
        end
    end
    pf:close()
    return out
end

-- ---- Link -----------------------------------------------------------------
local Link = {}
Link.__index = Link

local function now_ms()
    local tv = ffi.new("struct timeval"); C.gettimeofday(tv, nil)
    return tonumber(tv.tv_sec) * 1000 + tonumber(tv.tv_usec) / 1000
end
M.sleep = function(s) C.usleep(math.floor(s * 1e6)) end

function M.open(port, timeout)
    local fd = C.open(port, bit.bor(O_RDWR, O_NOCTTY, O_NONBLOCK))
    if fd < 0 then error("open(" .. port .. "): " .. ffi.string(C.strerror(ffi.errno()))) end
    local tio = ffi.new("struct termios")
    C.tcgetattr(fd, tio); C.cfmakeraw(tio); C.cfsetspeed(tio, B115200)
    tio.c_cc[6], tio.c_cc[5] = 0, 0   -- VMIN=0, VTIME=0: non-blocking reads, gated by poll()
    C.tcsetattr(fd, TCSANOW, tio)
    return setmetatable({ fd = fd, port = port, timeout = timeout or 1.0, seq = 0,
                          req = math.floor(now_ms()) % 65536 }, Link)
end

-- Agent mode: listen on `port`, wait for the BC to dial in (over WiFi), accept ONE
-- connection, and return a Link wrapping that socket. Same host_link codec as a serial
-- Link, so exec()/listen() work unchanged. `accept_timeout` (s, default 30) bounds the wait.
function M.listen_tcp(port, timeout, accept_timeout)
    local AF_INET, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR = 2, 1, 1, 2
    local srv = C.socket(AF_INET, SOCK_STREAM, 0)
    if srv < 0 then error("socket(): " .. ffi.string(C.strerror(ffi.errno()))) end
    local one = ffi.new("int[1]", 1)
    C.setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, one, ffi.sizeof("int"))
    local a = ffi.new("struct sockaddr_in"); a.sin_family = AF_INET
    a.sin_port = C.htons(port); a.sin_addr = 0   -- INADDR_ANY
    if C.bind(srv, a, ffi.sizeof("struct sockaddr_in")) ~= 0 then
        C.close(srv); error("bind(" .. port .. "): " .. ffi.string(C.strerror(ffi.errno())))
    end
    if C.listen(srv, 4) ~= 0 then C.close(srv); error("listen()") end
    local pfd = ffi.new("struct pollfd[1]"); pfd[0].fd = srv; pfd[0].events = POLLIN
    local deadline = now_ms() + (accept_timeout or 30) * 1000
    local got = false
    while now_ms() < deadline do
        local left = math.max(0, math.floor(deadline - now_ms()))
        if C.poll(pfd, 1, left) > 0 then got = true; break end
    end
    if not got then C.close(srv); error("listen_tcp: no BC dialed in within " .. (accept_timeout or 30) .. "s") end
    local cli = C.accept(srv, nil, nil)
    C.close(srv)   -- one-shot: stop accepting once the BC is connected
    if cli < 0 then error("accept(): " .. ffi.string(C.strerror(ffi.errno()))) end
    return setmetatable({ fd = cli, port = "tcp:" .. port, timeout = timeout or 2.0, seq = 0,
                          req = math.floor(now_ms()) % 65536 }, Link)
end

-- Agent mode, UDP: bind `port`, wait for the BC's first datagram to learn its address,
-- connect() the socket to it (so the Link's read/write target the BC), and return a Link.
-- Connectionless: no accept/re-dial — if the BC's addr changes, the caller re-opens.
function M.recv_udp(port, timeout, accept_timeout)
    local AF_INET, SOCK_DGRAM, SOL_SOCKET, SO_REUSEADDR = 2, 2, 1, 2
    local fd = C.socket(AF_INET, SOCK_DGRAM, 0)
    if fd < 0 then error("socket(udp): " .. ffi.string(C.strerror(ffi.errno()))) end
    local one = ffi.new("int[1]", 1)
    C.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, one, ffi.sizeof("int"))
    local a = ffi.new("struct sockaddr_in"); a.sin_family = AF_INET
    a.sin_port = C.htons(port); a.sin_addr = 0   -- INADDR_ANY
    if C.bind(fd, a, ffi.sizeof("struct sockaddr_in")) ~= 0 then
        C.close(fd); error("bind(udp " .. port .. "): " .. ffi.string(C.strerror(ffi.errno())))
    end
    local pfd = ffi.new("struct pollfd[1]"); pfd[0].fd = fd; pfd[0].events = POLLIN
    local deadline = now_ms() + (accept_timeout or 30) * 1000
    local got = false
    while now_ms() < deadline do
        local left = math.max(0, math.floor(deadline - now_ms()))
        if C.poll(pfd, 1, left) > 0 then got = true; break end
    end
    if not got then C.close(fd); error("recv_udp: no datagram within " .. (accept_timeout or 30) .. "s") end
    local peer = ffi.new("struct sockaddr_in")
    local plen = ffi.new("unsigned int[1]", ffi.sizeof("struct sockaddr_in"))
    local tmp  = ffi.new("uint8_t[256]")
    C.recvfrom(fd, tmp, 256, 0, peer, plen)       -- learn the BC addr (this datagram is discarded)
    if C.connect(fd, peer, ffi.sizeof("struct sockaddr_in")) ~= 0 then
        C.close(fd); error("connect(udp peer): " .. ffi.string(C.strerror(ffi.errno())))
    end
    return setmetatable({ fd = fd, port = "udp:" .. port, timeout = timeout or 2.0, seq = 0,
                          req = math.floor(now_ms()) % 65536 }, Link)
end

function Link:close() if self.fd and self.fd >= 0 then C.close(self.fd); self.fd = -1 end end

local _rbuf = ffi.new("uint8_t[?]", 1024)

-- Passively decode every s2m frame for `duration` seconds. on_frame(frame).
-- Uses a PERSISTENT per-link decoder (self.dec) shared with exec(): a frame whose bytes
-- span two reads (across listen/exec calls or the gap between them) is preserved instead of
-- dropped on a fresh decoder. SLIP framing self-syncs, so a stale partial is harmless.
function Link:listen(duration, on_frame)
    local dec = self.dec; if not dec then dec = make_decoder(); self.dec = dec end
    local deadline = now_ms() + duration * 1000
    local pfd = ffi.new("struct pollfd[1]"); pfd[0].fd = self.fd; pfd[0].events = POLLIN
    while now_ms() < deadline do
        local left = math.max(0, math.floor(deadline - now_ms()))
        if C.poll(pfd, 1, left) > 0 then
            local n = tonumber(C.read(self.fd, _rbuf, 1024))
            if n and n > 0 then
                for i = 0, n - 1 do decoder_feed(dec, _rbuf[i], on_frame) end
            end
        end
    end
end

-- Request/reply round-trip. Sends OP_SHELL_EXEC to `addr` with body
-- [req_id u16][cmd u16][args], waits for the OP_SHELL_REPLY whose req_id matches.
-- Returns (status:int, result:string). Errors on timeout.
-- `on_other` (optional): called for every decoded frame that is NOT the awaited reply
-- (e.g. OP_BUS_FEEDBACK / OP_DBG_LOG). Without it those frames are dropped while exec
-- waits -- a continuous-feedback consumer (the agent) passes it so nothing is lost mid-exec.
function Link:exec(addr, cmd, args, timeout, on_other)
    args = args or ""
    timeout = timeout or self.timeout
    self.req = bit.band(self.req + 1, 0xFFFF)
    self.seq = bit.band(self.seq + 1, 0xFF)
    local body = string.char(bit.band(self.req, 0xFF), bit.band(bit.rshift(self.req, 8), 0xFF),
                             bit.band(cmd, 0xFF), bit.band(bit.rshift(cmd, 8), 0xFF)) .. args
    local wire = encode_m2s(addr, M.OP_SHELL_EXEC, self.seq, body)
    if tonumber(C.write(self.fd, wire, #wire)) ~= #wire then error("short write") end

    local dec = self.dec; if not dec then dec = make_decoder(); self.dec = dec end  -- persistent (shared with listen)
    local deadline = now_ms() + timeout * 1000
    local st, res
    local pfd = ffi.new("struct pollfd[1]"); pfd[0].fd = self.fd; pfd[0].events = POLLIN
    while now_ms() < deadline do
        local left = math.max(0, math.floor(deadline - now_ms()))
        if C.poll(pfd, 1, left) > 0 then
            local n = tonumber(C.read(self.fd, _rbuf, 1024))
            for i = 0, (n or 0) - 1 do
                decoder_feed(dec, _rbuf[i], function(f)
                    if f.cmd == M.OP_SHELL_REPLY and #f.payload >= 3
                       and f.payload[1] + f.payload[2] * 256 == self.req then
                        st = f.payload[3]
                        local r = {}
                        for j = 4, #f.payload do r[#r + 1] = string.char(f.payload[j]) end
                        res = table.concat(r)
                    elseif on_other then
                        on_other(f)                 -- forward non-reply frames (feedback) instead of dropping
                    end
                end)
                if st ~= nil then break end
            end
        end
        if st ~= nil then break end
    end
    if st == nil then error(string.format("timeout waiting for reply to cmd 0x%04X @ addr 0x%02X", cmd, addr)) end
    return st, res or ""
end

-- Decode an OP_REGISTER v2 payload (38 B, little-endian) into a table. `b` is the
-- decoder's 1-indexed byte array. Layout: core/host_link.c::emit_register.
local function le(b, i, n) local v = 0; for k = 0, n-1 do v = v + (b[i+k] or 0) * (256^k) end; return v end
function M.parse_register(b)
    if #b < 38 then return nil end
    local uid = {}
    for i = 11, 18 do uid[#uid+1] = string.format("%02X", b[i]) end  -- RP UID = first 8 of 16
    return {
        ver        = b[1],
        class_id   = le(b, 2, 4),
        instance   = le(b, 6, 4),
        commission = b[10],
        uid        = table.concat(uid),
        vid        = string.format("%04x", le(b, 27, 2)),
        pid        = string.format("%04x", le(b, 29, 2)),
        fw_version = le(b, 31, 4),
        build_date = le(b, 35, 4),
    }
end

-- Listen up to `timeout` s for the first OP_REGISTER and return its decoded
-- identity table, or nil on timeout. The BC re-announces ~2 Hz until ACKed.
function Link:info(timeout)
    local out
    self:listen(timeout or 1.5, function(f)
        if not out and f.cmd == M.OP_REGISTER then out = M.parse_register(f.payload) end
    end)
    return out
end

-- Appcore (core1 KB0) liveness ping. Returns {uptime_ms, boot, kb0_ver}.
function Link:ping(timeout)
    local st, r = self:exec(M.ADDR_APPCORE, M.CMD_MON_PING, "", timeout)
    if st ~= M.SHELL_STATUS_OK then error("MON_PING status " .. st) end
    if #r < 7 then error("MON_PING short reply (" .. #r .. " B)") end
    local b = { r:byte(1, #r) }
    return {
        uptime_ms = b[1] + b[2]*256 + b[3]*65536 + b[4]*16777216,
        boot      = b[5] + b[6]*256,
        kb0_ver   = b[7],
    }
end

-- Thread-2 interlock status at `addr` (ADDR_APPCORE for the local node, or a slave
-- bus address via the master relay). Returns { ver, gveto, slots = { [slot] =
-- {state, tf, latched} } } or nil. Wire form v1: [ver][gveto][n] + n*[s][st][tf][la].
function Link:il_status(addr, timeout)
    local st, r = self:exec(addr, M.CMD_INTERLOCK_STATUS, "", timeout)
    if st ~= M.SHELL_STATUS_OK or not r or #r < 3 then return nil end
    local out = { ver = r:byte(1), gveto = r:byte(2), slots = {} }
    local n, p = r:byte(3), 4
    for _ = 1, n do
        if p + 3 > #r then break end
        out.slots[r:byte(p)] = { state = r:byte(p+1), tf = r:byte(p+2), latched = r:byte(p+3) }
        p = p + 4
    end
    return out
end

-- Request a global clear of all latched trips at `addr`. Returns the shell status.
function Link:il_clear(addr, timeout)
    return (self:exec(addr, M.CMD_INTERLOCK_CLEAR, "", timeout))
end

return M
