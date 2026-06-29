-- libcomm.lua -- LuaJIT port of libcomm.py: talk to a SAMD21 config chip over
-- USB-CDC (SLIP + CRC-8/AUTOSAR framing, synchronous OP_SHELL_EXEC reg/file
-- protocol). Serial + codec lifted from the prototype's commission.lua; the
-- shell/reg/file ops mirror libcomm.py. Includes file READ-BACK (file_get) so
-- commissioning can verify what it wrote.

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
]]
local C = ffi.C
local O_RDWR, O_NOCTTY, TCSANOW, B115200, POLLIN = 0x0002, 0x100, 0, 4098, 0x001
local O_NONBLOCK = 0x800   -- so open() never blocks on a wedged CDC (no carrier); reads use poll
local VMIN, VTIME = 6, 5

local M = {}
M.USB_VID, M.USB_PID = "2886", "802f"
M.WHO_AM_I_EXPECTED = 0x5A
M.REG_WHO_AM_I, M.REG_MODE, M.REG_STATUS, M.REG_UNIQUE_ID = 0x00, 0x02, 0x03, 0x06
M.MODE_NAME = { [0]="IDLE",[1]="GPIO",[2]="ADC",[3]="MIXED",[4]="SERVO",[5]="COUNTER" }

local SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD
local OP_SHELL_EXEC, OP_SHELL_REPLY = 0x0109, 0x0011
local SHELL_STATUS_OK = 0
local CMD_FILE_BEGIN, CMD_FILE_DATA, CMD_FILE_COMMIT, CMD_FILE_LIST = 0x0123, 0x0124, 0x0125, 0x0126
local CMD_REG_READ, CMD_REG_WRITE, CMD_REG_READN = 0x0127, 0x0128, 0x0129
local CMD_OFFLINE, CMD_FILE_FORMAT = 0x012A, 0x012B
local FILE_CHUNK_MAX = 100
local REG_FILE_NAME, REG_FILE_CTRL, REG_FILE_STAT, REG_FILE_SIZE, REG_FILE_DATA = 0x50, 0x51, 0x52, 0x53, 0x55
local FILE_CTRL_OPEN, FILE_STAT_OK = 1, 0

-- ---- codec ----------------------------------------------------------------
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

-- encode an m2s frame -> wire string. addr=1 (shell). payload is a byte string.
local function encode_m2s(cmd, seq, payload)
    local body = { 1, bit.band(cmd, 0xFF), bit.band(bit.rshift(cmd, 8), 0xFF), seq, #payload }
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

-- incremental s2m SLIP decoder. on_frame({cmd, seq, payload=<byte array>})
local function make_decoder() return { in_frame = false, escaped = false, buf = {} } end
local function decoder_feed(dec, b, on_frame)
    if b == SLIP_END then
        local f = dec.buf
        if dec.in_frame and #f >= 8 then
            local len = f[7]
            if #f == 7 + len + 1 and crc8(f, #f - 1) == f[#f] then
                local payload = {}
                for i = 8, 7 + len do payload[#payload + 1] = f[i] end
                on_frame({ cmd = bit.bor(f[2], bit.lshift(f[3], 8)), seq = f[4], payload = payload })
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
            local pid = usb and read1(usb .. "/idProduct")
            if vid and pid and vid:lower() == M.USB_VID and pid:lower() == M.USB_PID then
                out[#out + 1] = { port = path, serial = read1(usb .. "/serial") }
            end
        end
    end
    pf:close()
    return out
end

-- ---- Dongle ---------------------------------------------------------------
local Dongle = {}
Dongle.__index = Dongle

local function now_ms()
    local tv = ffi.new("struct timeval"); C.gettimeofday(tv, nil)
    return tonumber(tv.tv_sec) * 1000 + tonumber(tv.tv_usec) / 1000
end

function M.open(port, timeout)
    local fd = C.open(port, bit.bor(O_RDWR, O_NOCTTY, O_NONBLOCK))
    if fd < 0 then error("open(" .. port .. "): " .. ffi.string(C.strerror(ffi.errno()))) end
    local tio = ffi.new("struct termios")
    C.tcgetattr(fd, tio); C.cfmakeraw(tio); C.cfsetspeed(tio, B115200)
    tio.c_cc[VMIN], tio.c_cc[VTIME] = 0, 0
    C.tcsetattr(fd, TCSANOW, tio)
    local self = setmetatable({ fd = fd, port = port, timeout = timeout or 1.0, seq = 0,
                                req = math.floor(now_ms()) % 65536 }, Dongle)
    return self
end

function Dongle:close() if self.fd and self.fd >= 0 then C.close(self.fd); self.fd = -1 end end

-- 1200-baud touch: open the port at B1200 and close it -- the running app (if it
-- has the handler) jumps to the UF2 bootloader. No-op if the app lacks it.
function M.touch_1200(port)
    local fd = C.open(port, bit.bor(O_RDWR, O_NOCTTY, O_NONBLOCK))
    if fd < 0 then return false end
    local tio = ffi.new("struct termios")
    C.tcgetattr(fd, tio); C.cfmakeraw(tio); C.cfsetspeed(tio, 9)   -- 9 = B1200
    C.tcsetattr(fd, TCSANOW, tio)
    C.usleep(300000)
    C.close(fd)
    return true
end

function M.sleep(s) C.usleep(math.floor(s * 1e6)) end

local _rbuf = ffi.new("uint8_t[?]", 1024)
function Dongle:_drain()
    local pfd = ffi.new("struct pollfd[1]"); pfd[0].fd = self.fd; pfd[0].events = POLLIN
    for _ = 1, 64 do                        -- BOUNDED: a wedged/streaming chip would
        if C.poll(pfd, 1, 0) <= 0 then break end          -- otherwise loop forever here
        if tonumber(C.read(self.fd, _rbuf, 1024)) <= 0 then break end
    end
end

function Dongle:shell_exec(cmd, payload, timeout)
    payload = payload or ""
    timeout = timeout or self.timeout
    self:_drain()
    self.req = bit.band(self.req + 1, 0xFFFF)
    self.seq = bit.band(self.seq + 1, 0xFF)
    local body = string.char(bit.band(self.req, 0xFF), bit.band(bit.rshift(self.req, 8), 0xFF),
                             bit.band(cmd, 0xFF), bit.band(bit.rshift(cmd, 8), 0xFF)) .. payload
    local wire = encode_m2s(OP_SHELL_EXEC, self.seq, body)
    if tonumber(C.write(self.fd, wire, #wire)) ~= #wire then error("short write") end

    local dec = make_decoder()
    local deadline = now_ms() + timeout * 1000
    local st, res
    local pfd = ffi.new("struct pollfd[1]"); pfd[0].fd = self.fd; pfd[0].events = POLLIN
    while now_ms() < deadline do
        local left = math.max(0, math.floor(deadline - now_ms()))
        if C.poll(pfd, 1, left) > 0 then
            local n = tonumber(C.read(self.fd, _rbuf, 1024))
            for i = 0, n - 1 do
                decoder_feed(dec, _rbuf[i], function(f)
                    if f.cmd == OP_SHELL_REPLY and #f.payload >= 3 then
                        if f.payload[1] + f.payload[2] * 256 == self.req then
                            st = f.payload[3]
                            local r = {}
                            for j = 4, #f.payload do r[#r + 1] = string.char(f.payload[j]) end
                            res = table.concat(r)
                        end
                    end
                end)
                if st ~= nil then break end
            end
        end
        if st ~= nil then break end
    end
    if st == nil then error(string.format("timeout waiting for reply to cmd 0x%X", cmd)) end
    return st, res or ""
end

-- ---- register access ------------------------------------------------------
function Dongle:reg_read(reg)
    local st, r = self:shell_exec(CMD_REG_READ, string.char(bit.band(reg, 0xFF)))
    if st ~= SHELL_STATUS_OK then error(string.format("reg_read(0x%02X) status %d", reg, st)) end
    return r:byte(1)
end
function Dongle:reg_write(reg, val)
    local st = self:shell_exec(CMD_REG_WRITE, string.char(bit.band(reg, 0xFF), bit.band(val, 0xFF)))
    if st ~= SHELL_STATUS_OK then error(string.format("reg_write(0x%02X) status %d", reg, st)) end
end
function Dongle:reg_readn(reg, n)
    local st, r = self:shell_exec(CMD_REG_READN, string.char(bit.band(reg, 0xFF), bit.band(n, 0xFF)))
    if st ~= SHELL_STATUS_OK then error(string.format("reg_readn(0x%02X,%d) status %d", reg, n, st)) end
    return r:sub(1, n)
end

function Dongle:whoami() return self:reg_read(M.REG_WHO_AM_I) end
function Dongle:mode(set)
    if set ~= nil then self:reg_write(M.REG_MODE, set) end
    return self:reg_read(M.REG_MODE)
end
function Dongle:status() return self:reg_read(M.REG_STATUS) end
function Dongle:chip_uid()
    local b = self:reg_readn(M.REG_UNIQUE_ID, 8)
    local h = {}; for i = 1, #b do h[i] = string.format("%02X", b:byte(i)) end
    return table.concat(h)
end

-- ---- file transport -------------------------------------------------------
local function name4(name) return (name .. "    "):sub(1, 4) end

function Dongle:offline()
    local st = self:shell_exec(CMD_OFFLINE)
    if st ~= SHELL_STATUS_OK then error("OFFLINE rejected, status " .. st) end
end
function Dongle:file_format()
    local st = self:shell_exec(CMD_FILE_FORMAT, "", math.max(self.timeout, 5.0))
    if st ~= SHELL_STATUS_OK then error("FILE_FORMAT status " .. st) end
end
function Dongle:file_put(name, data)
    local st = self:shell_exec(CMD_FILE_BEGIN, name4(name))
    if st ~= SHELL_STATUS_OK then error("FILE_BEGIN status " .. st) end
    local off = 1
    while off <= #data do
        local chunk = data:sub(off, off + FILE_CHUNK_MAX - 1)
        st = self:shell_exec(CMD_FILE_DATA, chunk)
        if st ~= SHELL_STATUS_OK then error("FILE_DATA status " .. st) end
        off = off + #chunk
    end
    st = self:shell_exec(CMD_FILE_COMMIT)
    if st ~= SHELL_STATUS_OK then error("FILE_COMMIT status " .. st) end
end
function Dongle:file_list()
    local st, r = self:shell_exec(CMD_FILE_LIST)
    if st ~= SHELL_STATUS_OK then error("FILE_LIST status " .. st) end
    local out = {}
    if #r < 1 then return out end
    local count, pos = r:byte(1), 2
    for _ = 1, count do
        if pos + 4 > #r then break end
        local nm = r:sub(pos, pos + 3):gsub("%s+$", "")
        out[#out + 1] = { name = nm, length = r:byte(pos + 4) }
        pos = pos + 5
    end
    return out
end
function Dongle:file_get(name)
    local n4 = name4(name)
    for i = 1, 4 do self:reg_write(REG_FILE_NAME, n4:byte(i)) end
    self:reg_write(REG_FILE_CTRL, FILE_CTRL_OPEN)
    if self:reg_read(REG_FILE_STAT) ~= FILE_STAT_OK then error("file '" .. name .. "' not found") end
    local size = self:reg_read(REG_FILE_SIZE)
    return self:reg_readn(REG_FILE_DATA, size)
end

return M
