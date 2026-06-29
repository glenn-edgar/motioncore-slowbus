#!/usr/bin/env luajit
-- pico.lua -- bench CLI for the Pico bus_controller over USB-CDC (libcomm).
--
--   luajit pico.lua                 # auto-find port, decode REGISTER + ping appcore
--   luajit pico.lua info            # decode the device's OP_REGISTER identity
--   luajit pico.lua listen [secs]   # passive: decode DBG_LOG text + name frames
--   luajit pico.lua ping            # appcore (KB0) liveness round-trip
--   luajit pico.lua port            # just print the detected Pico CDC port
--
-- Read-only except `ping`/exec, which send a single OP_SHELL_EXEC and read the
-- matching reply. Pass a port as the last arg to override auto-detection.

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

local function pick_port(explicit)
    if explicit and explicit ~= "" then return explicit end
    local found = pl.enumerate()
    if #found == 0 then
        io.stderr:write("no Pico (USB " .. pl.PICO_VID .. ") ttyACM found.\n" ..
                        "  - is it running the app (not in BOOTSEL)?\n" ..
                        "  - pass a port explicitly: luajit pico.lua <cmd> /dev/ttyACMx\n")
        os.exit(1)
    end
    return found[1].port, found[1].serial
end

local function frame_str(f)
    local name = pl.OP_NAME[f.cmd] or string.format("0x%04X", f.cmd)
    if f.cmd == pl.OP_DBG_LOG then
        local s = {}
        for i = 1, #f.payload do s[i] = string.char(f.payload[i]) end
        return string.format("DBG_LOG: %s", (table.concat(s):gsub("%s+$", "")))
    end
    return string.format("%-16s addr=0x%02X seq=%d len=%d", name, f.addr, f.seq, #f.payload)
end

local function cmd_listen(port, secs)
    secs = tonumber(secs) or 3.0
    local p, ser = pick_port(port)
    io.write(string.format("[listen] %s%s for %.1fs (read-only)\n",
             p, ser and (" ["..ser.."]") or "", secs))
    local lk = pl.open(p, 1.0)
    local n = 0
    lk:listen(secs, function(f) n = n + 1; io.write("  " .. frame_str(f) .. "\n") end)
    lk:close()
    io.write(string.format("[listen] %d frame(s)\n", n))
    return n
end

local function cmd_info(port)
    local p, ser = pick_port(port)
    local lk = pl.open(p, 1.0)
    local r = lk:info(1.5)
    lk:close()
    if not r then io.stderr:write("[info] no OP_REGISTER seen in 1.5s\n"); os.exit(2) end
    io.write(string.format(
        "[info] %s  class=0x%08X inst=0x%08X commission=%d\n" ..
        "       uid=%s  usb=%s:%s  fw=%d  build=%d  reg_v%d\n",
        p, r.class_id, r.instance, r.commission, r.uid, r.vid, r.pid,
        r.fw_version, r.build_date, r.ver))
    return r
end

local function cmd_ping(port)
    local p, ser = pick_port(port)
    local lk = pl.open(p, 1.0)
    local ok, r = pcall(function() return lk:ping() end)
    lk:close()
    if not ok then io.stderr:write("[ping] FAILED: " .. tostring(r) .. "\n"); os.exit(2) end
    io.write(string.format("[ping] OK  %s%s  uptime=%dms  boot#%d  kb0_ver=%d\n",
             p, ser and (" ["..ser.."]") or "", r.uptime_ms, r.boot, r.kb0_ver))
end

local sub = arg[1]
if sub == "listen" then
    cmd_listen(arg[3], arg[2])
elseif sub == "ping" then
    cmd_ping(arg[2])
elseif sub == "info" then
    cmd_info(arg[2])
elseif sub == "port" then
    local p, ser = pick_port(arg[2]); io.write(p .. (ser and ("  "..ser) or "") .. "\n")
elseif sub == nil then
    cmd_info(nil)
    cmd_ping(nil)
else
    -- treat a bare arg as a port override for the default action
    cmd_info(sub)
    cmd_ping(sub)
end
