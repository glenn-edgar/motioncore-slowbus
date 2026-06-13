"""bench.py -- host-side UNIFIED bench across SAMD21 modes.

One API -- role(pin) / read(pin) / write(pin, val) by Seeed silkscreen name -- that
translates to whatever each mode exposes over the register interface, so test and
diagnostic code is mode-agnostic. The firmware keeps its per-mode register maps
(they collide, so a single shared bench window isn't possible); this class hides
that behind a consistent model:

  - GPIO  : the expander bitmaps -- role from IODIR (0x10), read GPIO (0x13),
            write OLAT (0x14). D6 (INT) and D4/D5 (I2C) are not writable.
  - COUNTER / SERVO : the per-pin bench registers 0x15-0x1E
            (SEL/GPO/GPI/STAT/ROLE), role-validated in firmware.

ADC reads: read_adc(pin) does a 16x one-shot on an `adc`-role pad in COUNTER/SERVO
(request -> poll -> get), or the instantaneous A1 sample in ADC mode. ADC mode's
windowed min/max/avg/AC-rms come from read_adc_stats(window).
"""

import time

REG_MODE = 0x02
MODE_IDLE, MODE_GPIO, MODE_ADC, MODE_MIXED, MODE_SERVO, MODE_COUNTER = 0, 1, 2, 3, 4, 5

# GPIO expander: channel bitmap order (IODIR/GPIO/OLAT bit i = channel i).
GPIO_CH = ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10')
G_IODIR, G_GPPU, G_OLAT, G_GPIO, G_OD = 0x10, 0x11, 0x14, 0x13, 0x18

# COUNTER/SERVO per-pin bench: Seeed pad index + the 0x15-0x1E registers.
SEEED_IDX = {n: i for i, n in enumerate(
    ('D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'D9', 'D10'))}
B_SEL, B_GPO, B_GPI, B_STAT, B_ROLE = 0x15, 0x16, 0x17, 0x1D, 0x1E
B_ADCRQ, B_ADCST, B_ADCV = 0x18, 0x19, 0x1B   # request / pending-status / u16 result
BENCH_OK, BENCH_BAD_PIN, BENCH_WRONG_ROLE, BENCH_UNSUPPORTED = 0, 1, 2, 3
ROLE_NAME = {0: 'none', 1: 'in', 2: 'out', 3: 'adc', 4: 'dac', 6: 'oc', 7: 'oc:up'}

# ADC mode (signal-processing): single channel A1. Windowed stats + instantaneous.
A_WIN_SEL, A_SEQ, A_MIN, A_MAX, A_AVG, A_RMS, A_LATEST = 0x11, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x1E
ADC_WINDOWS = {'khz1': 0, 'hz100': 1, 'hz10': 2}

A_ALIAS = {'A0': 'D0', 'A1': 'D1', 'A2': 'D2', 'A3': 'D3', 'A6': 'D6',
           'A7': 'D7', 'A8': 'D8', 'A9': 'D9', 'A10': 'D10'}


class BenchError(Exception):
    pass


class Bench:
    """Mode-aware bench over a libcomm.Dongle. Construct after the chip is in its
    final mode (reads MODE at construction; call refresh() if you switch modes)."""

    def __init__(self, dg):
        self.dg = dg
        self.refresh()

    def refresh(self):
        self.mode = self.dg.reg_read(REG_MODE)
        return self

    @staticmethod
    def _norm(pin):
        p = str(pin).upper()
        return A_ALIAS.get(p, p)

    def _seeed(self, p):
        idx = SEEED_IDX.get(p)
        if idx is None:
            raise BenchError("unknown pin %r" % p)
        return idx

    # -- role: 'in'/'out'/'adc'/'dac'/'none', or 'int'/'i2c'/'reserved' -----------
    def role(self, pin):
        p = self._norm(pin)
        if self.mode == MODE_GPIO:
            if p == 'D6':
                return 'int'
            if p in ('D4', 'D5'):
                return 'i2c'
            if p not in GPIO_CH:
                raise BenchError("%s is not a GPIO channel" % p)
            bit = GPIO_CH.index(p)
            if (self.dg.reg_read(G_IODIR) >> bit) & 1:
                return 'in'
            if (self.dg.reg_read(G_OD) >> bit) & 1:            # open-collector output
                return 'oc:up' if (self.dg.reg_read(G_GPPU) >> bit) & 1 else 'oc'
            return 'out'
        if self.mode in (MODE_COUNTER, MODE_SERVO):
            self.dg.reg_write(B_SEL, self._seeed(p))
            if self.dg.reg_read(B_STAT) == BENCH_BAD_PIN:
                return 'reserved'              # a counter/servo/interlock/I2C pad
            return ROLE_NAME.get(self.dg.reg_read(B_ROLE), '?')
        raise BenchError("bench not supported in mode %d yet" % self.mode)

    # -- read a digital level (0/1) ----------------------------------------------
    def read(self, pin):
        p = self._norm(pin)
        if self.mode == MODE_GPIO:
            if p not in GPIO_CH:
                raise BenchError("%s is not a readable GPIO channel" % p)
            return (self.dg.reg_read(G_GPIO) >> GPIO_CH.index(p)) & 1
        if self.mode in (MODE_COUNTER, MODE_SERVO):
            self.dg.reg_write(B_SEL, self._seeed(p))
            v = self.dg.reg_read(B_GPI)
            st = self.dg.reg_read(B_STAT)
            if st != BENCH_OK:
                raise BenchError("read %s: bench status %d (%s)" % (p, st, _stat_name(st)))
            return v
        raise BenchError("bench not supported in mode %d yet" % self.mode)

    # -- drive an output pin (never the interlock) -------------------------------
    def write(self, pin, val):
        p = self._norm(pin)
        if self.mode == MODE_GPIO:
            if p == 'D6':
                raise BenchError("D6 is the interrupt pin and is not writable")
            if p not in GPIO_CH:
                raise BenchError("%s is not a writable GPIO channel" % p)
            bit = GPIO_CH.index(p)
            olat = self.dg.reg_read(G_OLAT)
            olat = (olat | (1 << bit)) if val else (olat & ~(1 << bit))
            self.dg.reg_write(G_OLAT, olat & 0xFF)
            return
        if self.mode in (MODE_COUNTER, MODE_SERVO):
            self.dg.reg_write(B_SEL, self._seeed(p))
            self.dg.reg_write(B_GPO, 1 if val else 0)
            st = self.dg.reg_read(B_STAT)
            if st != BENCH_OK:
                raise BenchError("write %s: bench status %d (%s)" % (p, st, _stat_name(st)))
            return
        raise BenchError("bench not supported in mode %d yet" % self.mode)

    # -- ADC reads -----------------------------------------------------------
    def read_adc(self, pin, timeout=0.3):
        """ADC counts (0..4095). COUNTER/SERVO: 16x one-shot on an `adc`-role pad
        (request -> poll -> get). ADC mode: the instantaneous A1 sample."""
        p = self._norm(pin)
        if self.mode in (MODE_COUNTER, MODE_SERVO):
            self.dg.reg_write(B_SEL, self._seeed(p))
            self.dg.reg_write(B_ADCRQ, 1)                 # request the conversion
            t0 = time.monotonic()
            while self.dg.reg_read(B_ADCST):              # poll until ready (0)
                if time.monotonic() - t0 > timeout:
                    raise BenchError("read_adc %s: conversion timeout" % p)
                time.sleep(0.002)
            st = self.dg.reg_read(B_STAT)
            if st != BENCH_OK:
                raise BenchError("read_adc %s: bench status %d (%s)" % (p, st, _stat_name(st)))
            return self.dg.reg_read(B_ADCV) | (self.dg.reg_read(B_ADCV + 1) << 8)
        if self.mode == MODE_ADC:
            if p != 'D1':                                 # A1 == D1 == AIN4, the only ADC-mode channel
                raise BenchError("ADC mode samples only A1 (=D1), got %r" % p)
            return self.dg.reg_read(A_LATEST) | (self.dg.reg_read(A_LATEST + 1) << 8)
        raise BenchError("read_adc not supported in mode %d" % self.mode)

    def read_adc_stats(self, window='hz10'):
        """ADC mode only: {seq,min,max,avg,rms} of a downsample window
        (khz1/hz100/hz10). rms is AC-rms (DC removed)."""
        if self.mode != MODE_ADC:
            raise BenchError("read_adc_stats is ADC-mode only (mode %d)" % self.mode)
        w = ADC_WINDOWS.get(window)
        if w is None:
            raise BenchError("unknown window %r (khz1/hz100/hz10)" % window)
        self.dg.reg_write(A_WIN_SEL, w)
        time.sleep(0.02)
        def u16(r):
            return self.dg.reg_read(r) | (self.dg.reg_read(r + 1) << 8)
        return {'seq': u16(A_SEQ), 'min': u16(A_MIN), 'max': u16(A_MAX),
                'avg': u16(A_AVG), 'rms': u16(A_RMS)}


def _stat_name(st):
    return {BENCH_OK: 'OK', BENCH_BAD_PIN: 'BAD_PIN',
            BENCH_WRONG_ROLE: 'WRONG_ROLE', BENCH_UNSUPPORTED: 'UNSUPPORTED'}.get(st, '?')
