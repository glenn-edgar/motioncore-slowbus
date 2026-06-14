-- flash.lua -- reprogram a SAMD21 with the vendored firmware UF2.
--
-- A chip can't be commissioned until it runs the current firmware (real unique
-- serial + the register interface). This automates: 1200-baud touch -> wait for
-- the UF2 bootloader drive -> mount + cp the vendored UF2 -> re-verify. If the
-- chip's *current* firmware lacks the 1200-touch handler (factory/old chips), it
-- prompts for a physical RST double-tap -- the one unavoidable manual step until
-- a hardware reset line exists. Needs root for mount/cp (run commission w/ sudo).

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
local lc = require("libcomm")

local M = {}
M.UF2 = _dir .. "../firmware/register_dongle.uf2"     -- the vendored working binary

-- Known non-unique USB serials that old/blank firmware reports.
local PLACEHOLDER = { ["0123456789ABCDEF"] = true }

-- Does this chip need (re)flashing? Returns (bool, reason).
function M.needs_flash(chip)
    if not chip.serial or chip.serial == "" or PLACEHOLDER[chip.serial:upper()] then
        return true, "placeholder/no serial"
    end
    local ok, dg = pcall(lc.open, chip.port, 0.8)
    if not ok then return true, "open failed" end
    local okw, who = pcall(function() return dg:whoami() end)
    dg:close()
    if not okw or who ~= lc.WHO_AM_I_EXPECTED then return true, "no whoami=0x5A" end
    return false, "ok"
end

-- Is a UF2 bootloader (Arduino-LABEL) mass-storage drive present? -> /dev/sdX
function M.find_drive()
    local pf = io.popen("lsblk -nr -o NAME,LABEL 2>/dev/null")
    if not pf then return nil end
    local drv
    for line in pf:lines() do
        local name, label = line:match("^(%S+)%s+(%S+)")
        if label == "Arduino" then drv = "/dev/" .. name; break end
    end
    pf:close()
    return drv
end

local function poll_drive(secs)
    for _ = 1, secs do
        local d = M.find_drive(); if d then return d end
        lc.sleep(1)
    end
    return nil
end

-- Write the UF2 to a present bootloader drive and wait for the app to come back
-- on current firmware. Returns the post-flash chip {port, serial}. Raises on fail.
local function write_and_await(drv, uf2)
    io.write("  bootloader at " .. drv .. " -- writing " .. uf2 .. "\n")
    local cmd = string.format(
        "sudo mkdir -p /mnt/xiao && sudo mount %s /mnt/xiao && sudo cp %q /mnt/xiao/ && sync && sudo umount /mnt/xiao",
        drv, uf2)
    if os.execute(cmd) ~= 0 then error("flash mount/cp failed (need sudo)") end
    io.write("  flashed -- waiting for re-enumerate with whoami=0x5A...\n")
    lc.sleep(3)
    for _ = 1, 25 do
        for _, c in ipairs(lc.enumerate()) do
            local ok, dg = pcall(lc.open, c.port, 0.8)
            if ok then
                local okw, who = pcall(function() return dg:whoami() end)
                dg:close()
                if okw and who == lc.WHO_AM_I_EXPECTED and not PLACEHOLDER[(c.serial or ""):upper()] then
                    io.write("  back on " .. c.port .. " serial=" .. tostring(c.serial) .. "\n")
                    return c
                end
            end
        end
        lc.sleep(1)
    end
    error("chip did not return on current firmware after flash")
end

-- A chip already in the bootloader (002f + Arduino drive) -- enumerate (802f)
-- can't see it. Flash it directly. Returns the post-flash chip, or nil if no
-- bootloader drive is present.
function M.flash_from_bootloader(uf2)
    local drv = M.find_drive()
    if not drv then return nil end
    return write_and_await(drv, uf2 or M.UF2)
end

-- Flash the vendored UF2 onto an app-mode `chip` {port, serial}: 1200-touch (or,
-- if the firmware lacks the handler, a prompted physical double-tap) -> drive ->
-- write. Returns the post-flash chip.
function M.flash(chip, uf2)
    uf2 = uf2 or M.UF2
    local f = io.open(uf2, "r"); if not f then error("firmware not found: " .. uf2) end; f:close()

    local drv = M.find_drive()                       -- maybe already in bootloader
    if not drv then
        io.write("  reprogram " .. chip.port .. ": 1200-touch...\n")
        lc.touch_1200(chip.port)
        drv = poll_drive(8)
    end
    if not drv then
        io.write("  >> no bootloader -- DOUBLE-TAP the RST pads now (twice, quickly). Waiting ~45s...\n")
        io.flush()
        drv = poll_drive(45)
    end
    if not drv then error("no UF2 bootloader drive appeared (double-tap timing is finicky; retry)") end
    return write_and_await(drv, uf2)
end

-- Ensure `chip` runs the current firmware; flash if needed or if force. Returns
-- the (possibly updated) chip.
function M.ensure(chip, force)
    local need, why = M.needs_flash(chip)
    if force or need then
        io.write(string.format("firmware: %s (%s) -> reflashing\n", chip.port, force and "forced" or why))
        return M.flash(chip)
    end
    return chip
end

return M
