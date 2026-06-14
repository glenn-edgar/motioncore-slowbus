-- configs_two_pico.lua -- the two-Pico bench layout (4 SAMD21s, no interlocks).
-- Pico1 (bus controller): adc=boot_store(fixed), mixed=lite.
-- Pico2 (rs485 slave of pico1, i2c sub-master): gpio=boot_store(fixed), counter=lite.
local ct = require("config_tree")

local function build()
  local b = ct.new({ root = "slow_bus" })

  b:begin_i2c_system("pico1")
    b:device("adc",   { i2c=0x50, i2c_fixed=true, boot_store=true, sub="ADC" })
    b:device("mixed", { i2c=0x20, type="lite", sub="MIXED",
                        pins={ A1="adc", D8="in:up", D6="out" } })
    b:begin_boot(); b:end_boot()                     -- adc roster: {adc, mixed}
  b:end_i2c_system()

  b:begin_i2c_system("pico2")
    b:device("gpio",  { i2c=0x50, i2c_fixed=true, boot_store=true, sub="GPIO",
                        pins={ D0="in:up",D1="in:up",D2="in:up",D3="in:up",
                               D7="out:0",D8="out:0",D9="out:0",D10="out:0" } })
    b:device("counter",{ i2c=0x20, type="lite", sub="COUNTER", rate=1000,
                        pins={ D2="count:up:rising", A0="dac", D1="adc", D9="out", D10="in" } })
    b:begin_boot(); b:end_boot()                     -- gpio roster: {gpio, counter}
  b:end_i2c_system()

  return b
end
return { build = build }
