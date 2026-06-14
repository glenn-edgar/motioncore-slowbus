#!/usr/bin/env python3
"""slave_dsl.py — Python-embedded DSL that compiles a SAMD21 config-chip unit
definition into the raw on-chip files (`idnt`, `ilcf`).

Three layers, matching the firmware:

  * unit(addr, type)            -> emits `idnt` = raw [addr, mode] (ALL modes)
  * pins(D0='adc', D8='in:up')  -> the `cfg[...]` pin-role section (GPIO/MIXED).
                                   Input pins take a pull (up/down/none) and, in
                                   MIXED only, a debounce interval `debounce_<N>ms`
                                   (e.g. D8='in:up:debounce_50ms'); the firmware
                                   shift register runs at the ~100 Hz MIXED tick so
                                   the interval rounds to ~20-150 ms. GPIO mode has
                                   no debounce (reads the raw pin) -> rejected there.
  * interlock(name, when, drive)-> the `watch[...]`/`out_ok`/`out_err` sections,
                                   compiled from a boolean expression to the
                                   firmware's disjunctive-normal-form grammar

`idnt`/`ilcf` are RAW bytes the firmware reads directly (no CBOR) — distinct
from the Pico-consumed CBOR files (net, site). The master commission program
walks units, calls .files(), and writes each blob (offline -> write -> reboot).

Boolean expressions use C logical operators -- && (AND), || (OR), ~ (NOT) --
parentheses, and comparisons:

    interlock('safe',
        when='(A0 > 2.5V && D8) || ~D9',
        drive={'D6': 1})        # D6 = 1 while the condition holds, else 0

Comparisons:  >  <  >=  <=  ==  !=   (a bare GPIO pin means "== 1").  Pin names
are case-insensitive (d0 == D0).  ADC thresholds may be volts ('2.5V', full-scale
0..VDDANA≈3.3 V over 0..4095) or a raw count (0..4095).  ~/De-Morgan is pushed
into the comparison operator (the firmware has no NOT), then AND-over-OR is
distributed to DNF: clauses in a group are ANDed (`,`), groups are ORed (`|`).

Firmware limits (enforced here with clear errors):
  <=4 watched input pins, <=8 watch clauses total, <=2 outputs,
  ilcf string <=128 bytes, interlock name <16 chars.

Authoring style — a unit .py file just calls the module-level functions:

    unit(addr=0x55, type='MIXED')
    pins(A0='adc:oversample_16', D8='in:up', D6='out')
    interlock('safe', when='A0 > 2.5V && D8', drive={'D6': 1})

then `load('lift.py')` returns the Unit; `unit.files()` gives {'idnt':..,'ilcf':..}.
"""

import re

# ---- firmware constants (keep in sync with samd21_interlocks.h) ------------
MODES = {
    'IDLE': 0, 'GPIO': 1, 'PIO': 1, 'ADC': 2, 'MIXED': 3,
    'SERVO': 4, 'COUNTER': 5,
}
MODE_NAME = {0: 'IDLE', 1: 'GPIO', 2: 'ADC', 3: 'MIXED', 4: 'SERVO', 5: 'COUNTER'}
MODES_WITH_ILCF = {1, 2, 3}          # GPIO, ADC, MIXED carry an interlock file

IL_MAX_INPUTS = 4
IL_MAX_WATCHES = 8
IL_MAX_OUTPUTS = 2
IL_DSL_MAX = 128
IL_NAME_MAX = 16

# MIXED-mode GPIO debounce: authored in the DSL as a time (ms). The firmware shift
# register samples at the ~100 Hz MIXED tick, so depth = round(ms / tick); the
# firmware caps depth at [2,15] -> ~20-150 ms. GPIO mode has no debounce (its
# evaluator reads the raw pin), so debounce is rejected outside MIXED.
MIXED_TICK_MS = 10
DEBOUNCE_DEPTH_MIN, DEBOUNCE_DEPTH_MAX = 2, 15

# COUNTER mode: a `cntr` config file [VER=1, rate_lo, rate_hi, ch0..ch8]. Each ch
# byte: bit0 enable. If a counter (bit0=1): bits1-2 pull (0 none/1 up/2 down),
# bits3-4 edge (0 rise/1 fall/2 both). If NOT a counter (bit0=0): bits1-3 = bench
# role for that spare pad (firmware BENCH_ROLE_*: 0 none,1 gpio-in,2 gpio-out,3 adc,
# 4 dac). Pad order (bit 0..8): D0,D1,D2,D3,D7,D8,D9,D10,D6 (= servo bank).
CNTR_VERSION = 1
COUNTER_PINS = ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10', 'D6')
COUNTER_PIN_IDX = {p: i for i, p in enumerate(COUNTER_PINS)}
# A-labels alias the same physical pad (A0==D0, ...); accepted in counter()/pins().
COUNTER_PIN_ALIAS = {'A0': 'D0', 'A1': 'D1', 'A2': 'D2', 'A3': 'D3', 'A6': 'D6',
                     'A7': 'D7', 'A8': 'D8', 'A9': 'D9', 'A10': 'D10'}
COUNTER_PULLS = {'none': 0, 'up': 1, 'down': 2}
COUNTER_EDGES = {'rising': 0, 'falling': 1, 'both': 2}
# Bench roles for spare (non-counter) pads -> firmware BENCH_ROLE_* value. The byte
# stores (role << 1) with the enable bit clear. `dac` is hardware-limited to D0/A0.
# `oc` = open-collector (6); `oc:up` (internal pull-up on release) = 7.
COUNTER_BENCH_ROLES = {'in': 1, 'out': 2, 'adc': 3, 'dac': 4, 'oc': 6}
BENCH_ROLE_OC, BENCH_ROLE_OC_PU = 6, 7
COUNTER_RATE_MIN, COUNTER_RATE_MAX = 50, 10000   # Hz; max countable ~ rate/2

# SERVO mode: a `srvo` config file [VER=1, role(CH0)..role(CH7)]; one role byte per
# non-D6 bank pad. D6 is ALWAYS the e-stop interlock (input, pull-up, active-low) and
# is not declarable. Role byte = firmware BENCH_ROLE_* (1 gpio-in, 2 gpio-out, 3 adc,
# 4 dac) or SERVO=5 for a servo channel; 0 = none. Pad order: D0,D1,D2,D3,D7,D8,D9,D10.
SRVO_VERSION = 1
SERVO_PINS = ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10')
SERVO_PIN_IDX = {p: i for i, p in enumerate(SERVO_PINS)}
SERVO_PIN_ALIAS = {'A0': 'D0', 'A1': 'D1', 'A2': 'D2', 'A3': 'D3',
                   'A7': 'D7', 'A8': 'D8', 'A9': 'D9', 'A10': 'D10'}
SERVO_ROLES = {'in': 1, 'out': 2, 'adc': 3, 'dac': 4, 'servo': 5, 'oc': 6}

# MIXED mode: a `mxmp` fixed pin-map file [VER=1, padbyte x8] over the 8 usable pads
# (D0,D1,D2,D3,D7,D8,D9,D10; D6 = INT line, reserved). padbyte = (debounce<<4)|role;
# role low-nibble: 0 safe, 1 in, 2 in,up, 3 in,down, 4 out, 5 oc, 6 oc,up, 7 adc,
# 8 dac (D0/A0 only). The firmware applies + enforces this at MIXED mode-entry; the
# interlock operates WITHIN this pin environment (the I2C master is read-only).
MXMP_VERSION = 1
MIXED_PINS = ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10')   # D6 = INT (reserved)
MIXED_PIN_IDX = {p: i for i, p in enumerate(MIXED_PINS)}

ADC_FULLSCALE = 4095
VREF_DEFAULT = 3.3                    # full-scale volts (INTVCC1 ref + GAIN=DIV2)

# GPIO power-on pin map ("gpmp"): the 8 usable GPIO channels, in gpmp bit order
# 0..7 (D4/D5 = I2C, D6 = INT). Applied at startup before the interlock arms.
GPIO_PINS = ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10')
GPMP_VERSION = 2

# The configurable GP pads per mode. D4/D5 = I2C (always reserved); D6's role is
# mode-specific (interlock output in GPIO/ADC/MIXED, e-stop input in SERVO, a free
# counter/bench pad in COUNTER); ADC also reserves A0/D0 (DAC) + A1/D1 (sample).
# SAFETY RULE: every pad listed here must be EXPLICITLY assigned a role (a real role
# or `safe` = held Hi-Z) -- enforced by Unit._check_coverage. The I2C master is
# read-only, so a commissioned pin state can never be changed on the bus.
CONFIG_PADS = {
    MODES['GPIO']:    ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10'),        # D6 = INT output
    MODES['MIXED']:   ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10'),        # D6 = INT output
    MODES['ADC']:     ('D2', 'D3', 'D7', 'D8', 'D9', 'D10'),                    # D0=DAC, D1=sample, D6=INT
    MODES['SERVO']:   ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10'),        # D6 = e-stop input
    MODES['COUNTER']: ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10', 'D6'),  # D6 = free pad
}
# Why a pad is reserved, per mode -- for a clear DSL error if the user declares it.
RESERVED_WHY = {
    MODES['GPIO']:    {'D6': 'the interlock/INT output'},
    MODES['MIXED']:   {'D6': 'the interlock/INT output'},
    MODES['ADC']:     {'D0': 'the A0 DAC output', 'D1': 'the A1 ADC sample channel',
                       'D6': 'the interlock/INT output'},
    MODES['SERVO']:   {'D6': 'the e-stop input'},
    MODES['COUNTER']: {},
}
PIN_ALIAS = {'A0': 'D0', 'A1': 'D1', 'A2': 'D2', 'A3': 'D3', 'A6': 'D6',
             'A7': 'D7', 'A8': 'D8', 'A9': 'D9', 'A10': 'D10'}

_OP_FROM_SYM = {'>': 'gt', '<': 'lt', '>=': 'ge', '<=': 'le', '==': 'eq', '!=': 'ne'}
_OP_INVERT = {'gt': 'le', 'le': 'gt', 'lt': 'ge', 'ge': 'lt', 'eq': 'ne', 'ne': 'eq'}

_PULL_MODS = {'up', 'down'}
_ROLE_BASES = {'in', 'out', 'adc', 'dac', 'count', 'servo', 'oc', 'safe'}


class DSLError(Exception):
    """Raised on any malformed unit definition or boolean expression."""


_PAD_RANK_RE = re.compile(r'^[ADad](\d+)$')


def _pad_rank(label):
    """Canonical pad order: numeric (A-alias folds onto D); unknown labels last.
    Mirrors the Lua port's pad_rank/sorted_keys so emission is deterministic."""
    m = _PAD_RANK_RE.match(label)
    return (int(m.group(1)) if m else 999, label)


def _bench_role_value(label, base, mods):
    """Resolve a bench role base(+mods) to its firmware role byte. Only `oc` takes a
    modifier -- `oc:up` (internal pull-up on release) = 7, plain `oc` = 6; every
    other bench role takes no modifiers."""
    if base == 'oc':
        if not mods:
            return BENCH_ROLE_OC
        if mods == ['up']:
            return BENCH_ROLE_OC_PU
        raise DSLError("oc on %s: only the :up modifier is allowed, got %r" % (label, mods))
    if mods:
        raise DSLError("bench pin %s: role %r takes no modifiers, got %r" % (label, base, mods))
    return COUNTER_BENCH_ROLES[base]


# ---------------------------------------------------------------------------
# Boolean-expression tokenizer + recursive-descent parser
# ---------------------------------------------------------------------------

_TOKEN = re.compile(r'''
    \s*(?:
        (?P<and_op>&&) |
        (?P<or_op>\|\|) |
        (?P<not_op>~) |
        (?P<op>>=|<=|==|!=|>|<) |
        (?P<lp>\() |
        (?P<rp>\)) |
        (?P<num>\d+(?:\.\d+)?[Vv]?) |
        (?P<dot>\.) |
        (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
    )
''', re.VERBOSE)

# ADC-mode interlock streams. ADC mode is a single-channel (A1) signal-processing
# mode sampled at 16 kHz; the three downsample windows produce min/max/avg/AC-rms at
# 1 kHz / 100 Hz / 10 Hz output rates.
ADC_STATS   = ('avg', 'min', 'max', 'rms')          # windowed stats (rms = AC-rms)
ADC_WINDOWS = {'khz1': 0, 'hz100': 1, 'hz10': 2}    # downsample windows: 1 kHz / 100 Hz / 10 Hz
# The single sampled channel is A1 (= AIN4 = D1). A0 = DAC, D6 = interrupt.
ADC_WATCH_PINS = ('A1', 'D1')


def _tokenize(text):
    toks, pos = [], 0
    while pos < len(text):
        if text[pos].isspace():
            pos += 1
            continue
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise DSLError("cannot tokenize at %r" % text[pos:pos + 12])
        pos = m.end()
        kind = m.lastgroup
        toks.append((kind, m.group(kind)))
    return toks


def _volts_to_count(volts, vref):
    n = int(round(volts / vref * ADC_FULLSCALE))
    return max(0, min(ADC_FULLSCALE, n))


class _ExprParser:
    """Parses a boolean expression against a unit's declared pin roles into an
    AST of ('or'|'and', [children]) / ('not', child) / ('lit', pin, op, thr)."""

    def __init__(self, toks, roles, vref, adc=False):
        self.adc = adc                # ADC mode: atoms are channel.stat.window streams
        self.toks = toks
        self.i = 0
        self.roles = roles            # label -> (base, mods)
        self.vref = vref

    def _peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def _next(self):
        t = self._peek()
        self.i += 1
        return t

    def parse(self):
        node = self._or()
        if self.i != len(self.toks):
            raise DSLError("trailing tokens in expression: %r" % (self.toks[self.i:],))
        return node

    def _or(self):
        children = [self._and()]
        while self._peek()[0] == 'or_op':          # ||
            self._next()
            children.append(self._and())
        return children[0] if len(children) == 1 else ('or', children)

    def _and(self):
        children = [self._not()]
        while self._peek()[0] == 'and_op':         # &&
            self._next()
            children.append(self._not())
        return children[0] if len(children) == 1 else ('and', children)

    def _not(self):
        if self._peek()[0] == 'not_op':            # ~
            self._next()
            return ('not', self._not())
        return self._atom()

    def _atom(self):
        kind, val = self._peek()
        if kind == 'lp':
            self._next()
            node = self._or()
            if self._next()[0] != 'rp':
                raise DSLError("missing ')'")
            return node
        if kind == 'ident':
            self._next()
            if self.adc:
                return self._adc_comparison(val.upper())
            # pin names are case-insensitive (d0 == D0); virtuals keep their case
            pin = val if val.startswith('_') else val.upper()
            return self._comparison(pin)
        raise DSLError("expected a pin or '(', got %r" % (val,))

    def _adc_comparison(self, chan):
        # ADC stream: <channel>[.<stat>.<window>] <op> <threshold>. Default stat =
        # instantaneous. Emits the ilcf operand <PIN> or <PIN>_<stat>_<win>.
        if chan not in ADC_WATCH_PINS:
            raise DSLError("ADC channel %r not watchable (only A1/D1; A0=DAC, D6=interrupt)" % chan)
        operand = chan
        if self._peek()[0] == 'dot':
            self._next()
            k, stat = self._next()
            if k != 'ident' or stat.lower() not in ADC_STATS:
                raise DSLError("expected stat avg/min/max/rms after '.', got %r" % (stat,))
            if self._next()[0] != 'dot':
                raise DSLError("expected .window after .%s (khz1/hz100/hz10)" % stat)
            k, win = self._next()
            if k != 'ident' or win.lower() not in ADC_WINDOWS:
                raise DSLError("expected window khz1/hz100/hz10, got %r" % (win,))
            operand = "%s_%s_%s" % (chan, stat.lower(), win.lower())
        k, sym = self._peek()
        if k != 'op':
            raise DSLError("ADC stream %r needs a comparison (e.g. %s > 1.5V)" % (operand, chan))
        self._next()
        op = _OP_FROM_SYM[sym]
        nk, nval = self._next()
        if nk != 'num':
            raise DSLError("expected a number after %r" % sym)
        if nval[-1:] in ('V', 'v'):
            thr = _volts_to_count(float(nval[:-1]), self.vref)
        else:
            thr = int(nval)
            if not 0 <= thr <= ADC_FULLSCALE:
                raise DSLError("ADC count %d out of range 0..%d" % (thr, ADC_FULLSCALE))
        return ('lit', operand, op, thr)

    def _comparison(self, pin):
        if pin not in self.roles:
            raise DSLError("pin %r used in expression but not declared in pins()" % pin)
        base, _ = self.roles[pin]
        if base == 'out':
            raise DSLError("output pin %r cannot be watched in an expression" % pin)
        kind, val = self._peek()
        if kind == 'op':                              # pin OP value
            self._next()
            op = _OP_FROM_SYM[val]
            nkind, nval = self._next()
            if nkind != 'num':
                raise DSLError("expected a number after %r" % val)
            thr = self._value(pin, base, nval)
            return ('lit', pin, op, thr)
        # bare pin -> GPIO "== 1"
        if base == 'adc':
            raise DSLError("ADC pin %r needs a comparison (e.g. %s > 1.0V)" % (pin, pin))
        return ('lit', pin, 'eq', 1)

    def _value(self, pin, base, tok):
        if tok[-1:] in ('V', 'v'):
            if base != 'adc':
                raise DSLError("voltage threshold on non-ADC pin %r" % pin)
            return _volts_to_count(float(tok[:-1]), self.vref)
        n = int(tok)
        if base == 'adc':
            if not 0 <= n <= ADC_FULLSCALE:
                raise DSLError("ADC count %d out of range 0..%d" % (n, ADC_FULLSCALE))
        elif n not in (0, 1):
            raise DSLError("GPIO threshold on %r must be 0 or 1, got %d" % (pin, n))
        return n


def _push_not(node, neg=False):
    """De-Morgan: drive every NOT down into the comparison operators."""
    t = node[0]
    if t == 'lit':
        _, pin, op, thr = node
        return ('lit', pin, _OP_INVERT[op] if neg else op, thr)
    if t == 'not':
        return _push_not(node[1], not neg)
    if t in ('and', 'or'):
        flipped = ('or' if t == 'and' else 'and') if neg else t
        return (flipped, [_push_not(c, neg) for c in node[1]])
    raise DSLError("bad AST node %r" % (node,))


def _to_dnf(node):
    """Return a list of product-terms (each a list of literal tuples)."""
    t = node[0]
    if t == 'lit':
        return [[node]]
    if t == 'or':
        terms = []
        for c in node[1]:
            terms.extend(_to_dnf(c))
        return terms
    if t == 'and':
        terms = [[]]
        for c in node[1]:
            ct = _to_dnf(c)
            terms = [pre + term for pre in terms for term in ct]
        return terms
    raise DSLError("bad AST node %r" % (node,))


def _compile_expr(when, roles, vref, adc=False):
    """Boolean expression -> list of groups, each a list of (operand, op, thr)."""
    ast = _ExprParser(_tokenize(when), roles, vref, adc=adc).parse()
    groups = []
    for term in _to_dnf(_push_not(ast)):
        seen, lits = set(), []
        for (_, pin, op, thr) in term:                # dedup identical clauses
            key = (pin, op, thr)
            if key not in seen:
                seen.add(key)
                lits.append((pin, op, thr))
        groups.append(lits)
    return groups


# ---------------------------------------------------------------------------
# Unit builder
# ---------------------------------------------------------------------------

class Unit:
    def __init__(self, addr, type, vref=VREF_DEFAULT):
        if not 0 <= addr <= 0x7F:
            raise DSLError("i2c address 0x%X out of 7-bit range" % addr)
        key = str(type).upper()
        if key not in MODES:
            raise DSLError("unknown type %r (one of %s)" % (type, sorted(MODES)))
        self.addr = addr
        self.mode = MODES[key]
        self.vref = vref
        self.roles = {}               # label -> (base, [mods]) in declaration order
        self.il = None                # (name, when, drive)
        self.cntr_rate = 1000         # COUNTER bank-global update rate (Hz)

    # -- authoring -------------------------------------------------------
    def counter(self, rate=1000):
        """Set the COUNTER bank-global update rate (Hz). Pins are declared via
        pins(D1='count:up:rising'); see cntr()."""
        if self.mode != MODES['COUNTER']:
            raise DSLError("counter() is COUNTER-mode only")
        if not (COUNTER_RATE_MIN <= rate <= COUNTER_RATE_MAX):
            raise DSLError("counter rate %d out of [%d,%d] Hz"
                           % (rate, COUNTER_RATE_MIN, COUNTER_RATE_MAX))
        self.cntr_rate = rate
        return self

    def servo(self):
        """SERVO mode marker; pads are declared via pins(D0='servo', D8='adc', ...).
        D6 is always the e-stop interlock and cannot be declared. See srvo()."""
        if self.mode != MODES['SERVO']:
            raise DSLError("servo() is SERVO-mode only")
        return self

    def pins(self, **roles):
        for label, spec in roles.items():
            parts = str(spec).split(':')
            base, mods = parts[0], parts[1:]
            if base not in _ROLE_BASES:
                raise DSLError("pin %s: bad role base %r" % (label, base))
            if base == 'safe' and mods:
                raise DSLError("pin %s: 'safe' takes no modifiers" % label)
            self.roles[label.upper()] = (base, mods)   # pin names case-insensitive
        return self

    def interlock(self, name, when, drive=None):
        if len(name) >= IL_NAME_MAX:
            raise DSLError("interlock name %r >= %d chars" % (name, IL_NAME_MAX))
        drive = {k.upper(): v for k, v in (drive or {}).items()}
        self.il = (name, when, drive)
        return self

    # SAFETY GATE: every configurable pad for this mode must be explicitly assigned
    # (a real role or `safe`), no reserved pad may be declared, and no pad declared
    # twice. A hardware environment must never be left with a pad in an unspecified
    # state, and the I2C master cannot change a commissioned pin -- so completeness is
    # proven here, at config time. Run first in files().
    def _check_coverage(self):
        pads = CONFIG_PADS.get(self.mode)
        if pads is None:                              # IDLE: idnt-only, no pins
            if self.roles:
                raise DSLError("%s mode takes no pins()" % MODE_NAME[self.mode])
            return
        want = set(pads)
        why = RESERVED_WHY.get(self.mode, {})
        have = set()
        for label in self.roles:                      # roles keys are already upper
            p = PIN_ALIAS.get(label, label)
            if p in ('D4', 'D5'):
                raise DSLError("%s: %s is the I2C bus, not user I/O"
                               % (MODE_NAME[self.mode], label))
            if p in why:
                raise DSLError("%s: %s is %s, not user I/O"
                               % (MODE_NAME[self.mode], label, why[p]))
            if p not in want:
                raise DSLError("%s: %s is not a configurable pad"
                               % (MODE_NAME[self.mode], label))
            if p in have:
                raise DSLError("%s: pad %s declared twice (alias?)"
                               % (MODE_NAME[self.mode], p))
            have.add(p)
        for p in pads:
            if p not in have:
                raise DSLError("%s: pad %s is unassigned -- every configurable pad "
                               "must be set (use 'safe' to hold it Hi-Z)"
                               % (MODE_NAME[self.mode], p))

    # -- emission --------------------------------------------------------
    def idnt(self):
        return bytes([self.addr, self.mode])

    def _debounce_depth(self, pin, mod):
        """Parse a `debounce_<N>ms` input modifier -> firmware shift depth.
        MIXED-only (GPIO mode reads the raw pin); ms rounds to the ~100 Hz tick."""
        if self.mode != MODES['MIXED']:
            raise DSLError("debounce on %s is MIXED-only (GPIO mode has no debounce)" % pin)
        body = mod[len('debounce_'):]
        if not body.endswith('ms'):
            raise DSLError("debounce on %s must be in ms, e.g. debounce_50ms (got %r)" % (pin, mod))
        try:
            ms = int(body[:-2])
        except ValueError:
            raise DSLError("debounce on %s: bad ms value in %r" % (pin, mod))
        depth = (ms + MIXED_TICK_MS // 2) // MIXED_TICK_MS         # round to nearest tick
        if not (DEBOUNCE_DEPTH_MIN <= depth <= DEBOUNCE_DEPTH_MAX):
            raise DSLError("debounce %dms on %s -> depth %d out of range "
                           "(allowed ~%d-%d ms)" % (ms, pin, depth,
                           DEBOUNCE_DEPTH_MIN * MIXED_TICK_MS, DEBOUNCE_DEPTH_MAX * MIXED_TICK_MS))
        return depth

    def _cfg_token(self, pin):
        """il_parse cfg token for one pin, derived from its declared role."""
        base, mods = self.roles[pin]
        if base == 'adc':
            return "(%s):adc%s" % (pin, "".join("," + m for m in mods))
        if base == 'in':
            pull, deb = 'none', None
            for m in mods:
                if m in ('up', 'down', 'none'):
                    pull = m
                elif m.startswith('debounce_'):
                    deb = self._debounce_depth(pin, m)
                else:
                    raise DSLError("input %s: bad modifier %r "
                                   "(pull up/down/none or debounce_<N>ms)" % (pin, m))
            sfx = {'up': ',up', 'down': ',down', 'none': ''}[pull]
            if deb is not None:
                sfx += ",debounce_%d" % deb
            return "(%s):in%s" % (pin, sfx)
        if base == 'out':
            return "(%s):out" % pin          # interlock drives it; init lives in gpmp
        if base == 'oc':                     # open-collector output: oc / oc:up
            return self._out_cfg_token(pin)
        raise DSLError("pin %s: bad role base %r" % (pin, base))

    def _out_cfg_token(self, pin):
        """cfg token for an interlock OUTPUT pin: `oc`/`oc,up` if the pin is declared
        open-collector, else push-pull `out` (the default when undeclared)."""
        base, mods = self.roles.get(pin, ('out', []))
        if base == 'oc':
            if mods == ['up']:
                return "(%s):oc,up" % pin
            if mods:
                raise DSLError("oc on %s: only the :up modifier is allowed, got %r" % (pin, mods))
            return "(%s):oc" % pin
        if base != 'out':
            raise DSLError("pin %s drives an interlock output but is declared %r; "
                           "must be out or oc" % (pin, base))
        return "(%s):out" % pin

    # An inert ilcf: a name with no watch section -> il_parse OK but the firmware
    # won't arm it (needs >=1 watch). Emitted when a GPIO/MIXED unit has no
    # interlock, so commissioning OVERWRITES any stale interlock left in flash
    # (there is no file-delete yet). Without this, a no-interlock re-commission
    # leaves the previous interlock armed (it can hold an output at its safe value).
    ILCF_OFF = "off"

    def ilcf(self):
        # ilcf carries ONLY the interlock; the GPIO power-on pin state is in gpmp.
        if self.mode not in MODES_WITH_ILCF:
            return None
        if self.il is None or not self.il[1]:
            return self.ILCF_OFF
        name, when, drive = self.il
        drive = drive or {}
        if self.mode == MODES['ADC']:
            return self._ilcf_adc(name, when, drive)
        if not self.roles:
            raise DSLError("%s mode needs pins()" % MODE_NAME[self.mode])

        groups = _compile_expr(when, self.roles, self.vref)
        inputs = []                           # watched input pins, first-seen order
        for g in groups:
            for (p, _, _) in g:
                if p not in inputs:
                    inputs.append(p)
        for p in inputs:
            if p not in self.roles or self.roles[p][0] in ('out', 'oc'):
                raise DSLError("watched pin %r is not a declared input" % p)
        if len(inputs) > IL_MAX_INPUTS:
            raise DSLError("%d watched inputs > IL_MAX_INPUTS=%d" % (len(inputs), IL_MAX_INPUTS))
        total = sum(len(g) for g in groups)
        if total > IL_MAX_WATCHES:
            raise DSLError("%d watch clauses > IL_MAX_WATCHES=%d (after DNF expansion); "
                           "simplify the expression" % (total, IL_MAX_WATCHES))
        if len(drive) > IL_MAX_OUTPUTS:
            raise DSLError("%d outputs > IL_MAX_OUTPUTS=%d" % (len(drive), IL_MAX_OUTPUTS))
        for label in drive:
            if self.roles.get(label, (None,))[0] not in ('out', 'oc'):
                raise DSLError("drive pin %r not declared as 'out' or 'oc'" % label)

        # MIXED's interlock cfg lists the in/out/oc/adc pins; pin HW config lives in
        # mxmp, so skip safe (Hi-Z) and dac (DAC bank). GPIO's cfg declares only the
        # interlock's pins (all 8 live in gpmp, can't fit). Emit in canonical PAD
        # order so insertion order matches the Lua port's deterministic output.
        if self.mode == MODES['MIXED']:
            cfg_pins = [p for p in sorted(self.roles, key=_pad_rank)
                        if self.roles[p][0] not in ('safe', 'dac')]
        else:
            cfg_pins = inputs + [p for p in drive if p not in inputs]

        sections = [name, "cfg[%s]" % ",".join(self._cfg_token(p) for p in cfg_pins)]
        sections.append("watch[%s]" % "|".join(
            ",".join(self._clause(p, op, thr) for (p, op, thr) in g) for g in groups))
        if drive:
            ok, err = [], []
            for label, v in drive.items():
                if v not in (0, 1):
                    raise DSLError("drive value for %r must be 0 or 1" % label)
                ok.append("%s:%d" % (label, v))
                err.append("%s:%d" % (label, 1 - v))
            sections.append("out_ok[%s]" % ",".join(ok))
            sections.append("out_err[%s]" % ",".join(err))

        text = sections[0] + "".join(";" + s for s in sections[1:])
        if len(text) > IL_DSL_MAX:
            raise DSLError("ilcf is %d bytes > IL_DSL_MAX=%d" % (len(text), IL_DSL_MAX))
        return text

    @staticmethod
    def _clause(pin, op, thr):
        return "%s:%d" % (pin, thr) if op == 'eq' else "%s:%s:%d" % (pin, op, thr)

    def _ilcf_adc(self, name, when, drive):
        """ADC-mode interlock: watches are channel streams (no cfg pin claim --
        the sweep samples every channel), driving the output(s). Emits
        name;cfg[(D6):out];watch[<stream>:op:thr|...];out_ok[..];out_err[..],
        where <stream> is `A1` (instantaneous) or `A1_avg_fast` (stat_window)."""
        if not drive:
            raise DSLError("ADC interlock needs a drive output (e.g. drive={'D6':1})")
        if len(drive) > IL_MAX_OUTPUTS:
            raise DSLError("%d outputs > IL_MAX_OUTPUTS=%d" % (len(drive), IL_MAX_OUTPUTS))
        groups = _compile_expr(when, {}, self.vref, adc=True)
        streams = []
        for g in groups:
            for (operand, _, _) in g:
                if operand not in streams:
                    streams.append(operand)
        if len(streams) > IL_MAX_INPUTS:
            raise DSLError("%d watched ADC streams > IL_MAX_INPUTS=%d" % (len(streams), IL_MAX_INPUTS))
        total = sum(len(g) for g in groups)
        if total > IL_MAX_WATCHES:
            raise DSLError("%d watch clauses > IL_MAX_WATCHES=%d" % (total, IL_MAX_WATCHES))
        cfg = ",".join(self._out_cfg_token(d.upper()) for d in drive)
        watch = "|".join(",".join(self._clause(s, op, thr) for (s, op, thr) in g) for g in groups)
        ok, err = [], []
        for label, v in drive.items():
            if v not in (0, 1):
                raise DSLError("drive value for %r must be 0 or 1" % label)
            ok.append("%s:%d" % (label.upper(), v))
            err.append("%s:%d" % (label.upper(), 1 - v))
        sections = [name, "cfg[%s]" % cfg, "watch[%s]" % watch,
                    "out_ok[%s]" % ",".join(ok), "out_err[%s]" % ",".join(err)]
        text = sections[0] + "".join(";" + s for s in sections[1:])
        if len(text) > IL_DSL_MAX:
            raise DSLError("ilcf is %d bytes > IL_DSL_MAX=%d" % (len(text), IL_DSL_MAX))
        return text

    def gpmp(self):
        """GPIO power-on pin map: 6 bytes [VER=2, DIR, PULLEN, OUT, OD, INTCFG].

        Bitmaps over GPIO_PINS (bit 0..7). All 8 usable pins MUST be defined.
        OUT does double duty (SAMD21 PORT.OUT): output drive level, or pull
        direction (1=up,0=down) for a pulled input. OD marks open-drain outputs
        (drive low / Hi-Z); for an OD output OUT=1 means released (Hi-Z) at boot.
        """
        if self.mode != MODES['GPIO']:
            return None
        for p in self.roles:
            if p not in GPIO_PINS:
                raise DSLError("GPIO: pin %s is not a usable channel %s (D6=INT)"
                               % (p, ",".join(GPIO_PINS)))
        dir_bm = pullen_bm = out_bm = od_bm = 0
        for i, pin in enumerate(GPIO_PINS):
            if pin not in self.roles:
                raise DSLError("GPIO: pin %s undefined -- all 8 of %s must be set"
                               % (pin, ",".join(GPIO_PINS)))
            base, mods = self.roles[pin]
            bit = 1 << i
            if base == 'out':
                dir_bm |= bit
                kind = mods[0] if mods else '0'
                if kind in ('0', '1'):                 # push-pull, drive level
                    if kind == '1':
                        out_bm |= bit
                else:
                    raise DSLError("output %s must be 0 or 1 (open-collector is now 'oc'), got %r"
                                   % (pin, kind))
            elif base == 'oc':                         # open-collector: output, open-drain,
                dir_bm |= bit                          # powers up released (Hi-Z, OUT=1)
                od_bm  |= bit
                out_bm |= bit
                if mods == ['up']:                     # oc:up -> internal pull-up on release
                    pullen_bm |= bit
                elif mods:
                    raise DSLError("oc on %s: only the :up modifier is allowed, got %r" % (pin, mods))
            elif base == 'in':
                pull = 'none'                          # iterate all mods (don't drop extras)
                for m in mods:
                    if m in ('up', 'down', 'none'):
                        pull = m
                    elif m.startswith('debounce_'):
                        raise DSLError("debounce on %s is MIXED-only" % pin)
                    else:
                        raise DSLError("GPIO input %s: bad modifier %r" % (pin, m))
                if pull != 'none':
                    pullen_bm |= bit
                    if pull == 'up':
                        out_bm |= bit
            elif base == 'safe':
                pass                                   # held safe: input, no pull (all bits 0)
            else:
                raise DSLError("GPIO pin %s: role %r not allowed (in/out/oc/safe)" % (pin, base))
        intcfg = 0x01 if self.il else 0x00   # bit0: open-drain active-low INT enabled
        return bytes([GPMP_VERSION, dir_bm, pullen_bm, out_bm, od_bm, intcfg])

    def cntr(self):
        """COUNTER config file: [VER, rate_lo, rate_hi, ch0..ch8]. Each declared
        `count` pad becomes a counter (pull up/down/none, edge rising/falling/both).
        A spare pad may instead carry a bench role for the COUNTER bench tools:
        `in` (gpio read), `out` (gpio write), `adc` (16x oneshot), or `dac` (D0/A0
        only). Undeclared pads default to no role (every bench op on them errors)."""
        if self.mode != MODES['COUNTER']:
            return None
        chbytes = [0] * len(COUNTER_PINS)
        for label, (base, mods) in self.roles.items():
            pin = COUNTER_PIN_ALIAS.get(label, label)        # A0->D0, ...
            if pin not in COUNTER_PIN_IDX:
                raise DSLError("COUNTER pin %s is not a usable pad %s" % (label, COUNTER_PINS))
            idx = COUNTER_PIN_IDX[pin]
            if base == 'count':
                pull, edge = 'none', 'rising'
                for m in mods:
                    if m in COUNTER_PULLS:
                        pull = m
                    elif m in COUNTER_EDGES:
                        edge = m
                    else:
                        raise DSLError("counter %s: bad modifier %r (pull up/down/none, "
                                       "edge rising/falling/both)" % (label, m))
                chbytes[idx] = (1 | (COUNTER_PULLS[pull] << 1) | (COUNTER_EDGES[edge] << 3))
            elif base in COUNTER_BENCH_ROLES:
                role = _bench_role_value(label, base, mods)      # handles oc / oc:up
                if base == 'dac' and pin != 'D0':
                    raise DSLError("COUNTER pin %s: dac role is only on D0/A0 (the DAC pad)" % label)
                chbytes[idx] = (role << 1)                        # enable bit 0 left clear
            elif base == 'safe':
                chbytes[idx] = 0                                  # NONE: held safe (Hi-Z)
            else:
                raise DSLError("COUNTER pin %s: role %r must be count, a bench role, or safe"
                               % (label, base))
        r = self.cntr_rate
        return bytes([CNTR_VERSION, r & 0xFF, (r >> 8) & 0xFF] + chbytes)

    def srvo(self):
        """SERVO config file: [VER, role(CH0)..role(CH7)]. Each declared pad is a
        `servo` channel or a bench role (in/out/adc/dac; dac on D0/A0 only). D6 is
        always the e-stop interlock and may not be declared. Undeclared pads = none."""
        if self.mode != MODES['SERVO']:
            return None
        rolebytes = [0] * len(SERVO_PINS)
        for label, (base, mods) in self.roles.items():
            pin = SERVO_PIN_ALIAS.get(label, label)          # A0->D0, ...
            if pin in ('D6', 'A6'):
                raise DSLError("SERVO pin %s is the e-stop interlock; it can't be declared" % label)
            if pin not in SERVO_PIN_IDX:
                raise DSLError("SERVO pin %s is not a usable pad %s" % (label, SERVO_PINS))
            if base == 'safe':
                role = 0                                         # NONE: held safe (Hi-Z)
            elif base not in SERVO_ROLES:
                raise DSLError("SERVO pin %s: role %r must be one of %s"
                               % (label, base, sorted(SERVO_ROLES)))
            elif base == 'dac' and pin != 'D0':
                raise DSLError("SERVO pin %s: dac role is only on D0/A0 (the DAC pad)" % label)
            elif base == 'oc':
                role = _bench_role_value(label, base, mods)      # oc / oc:up
            elif mods:
                raise DSLError("SERVO pin %s: role %r takes no modifiers, got %r" % (label, base, mods))
            else:
                role = SERVO_ROLES[base]
            rolebytes[SERVO_PIN_IDX[pin]] = role
        return bytes([SRVO_VERSION] + rolebytes)

    def mxmp(self):
        """MIXED fixed pin-map: [VER=1, padbyte x8] in MIXED_PINS order
        (D0,D1,D2,D3,D7,D8,D9,D10; D6 = INT, reserved). padbyte = (debounce<<4)|role,
        role low-nibble: 0 safe 1 in 2 in,up 3 in,down 4 out 5 oc 6 oc,up 7 adc 8 dac.
        debounce (in-roles only): 0 none, else 2..15 ticks (x10ms) = 20..150 ms."""
        if self.mode != MODES['MIXED']:
            return None
        padbyte = [0] * len(MIXED_PINS)                       # default safe
        for label, (base, mods) in self.roles.items():
            pin = PIN_ALIAS.get(label, label)
            idx = MIXED_PIN_IDX[pin]                          # coverage guarantees valid+present
            code, depth = 0, 0
            if base == 'safe':
                code = 0
            elif base == 'in':
                pull = 'none'
                for m in mods:
                    if m in ('up', 'down', 'none'):
                        pull = m
                    elif m.startswith('debounce_'):
                        depth = self._debounce_depth(pin, m)
                    else:
                        raise DSLError("MIXED input %s: bad modifier %r" % (label, m))
                code = 2 if pull == 'up' else 3 if pull == 'down' else 1
            elif base == 'out':
                if mods:
                    raise DSLError("MIXED out %s: takes no modifiers" % label)
                code = 4
            elif base == 'oc':
                if mods == ['up']:
                    code = 6
                elif mods:
                    raise DSLError("oc on %s: only :up allowed" % label)
                else:
                    code = 5
            elif base == 'adc':
                if mods:
                    raise DSLError("MIXED adc %s: takes no modifiers" % label)
                code = 7
            elif base == 'dac':
                if pin != 'D0':
                    raise DSLError("MIXED: dac only on D0/A0")
                if mods:
                    raise DSLError("MIXED dac: takes no modifiers")
                code = 8
            else:
                raise DSLError("MIXED pin %s: role %r not allowed (in/out/oc/adc/dac/safe)"
                               % (label, base))
            padbyte[idx] = (depth << 4) | code
        return bytes([MXMP_VERSION] + padbyte)

    def files(self):
        """Return {name: bytes} ready for the commission tool to write."""
        self._check_coverage()        # SAFETY GATE: all pads explicitly assigned
        out = {'idnt': self.idnt()}
        gpmp = self.gpmp()
        if gpmp is not None:
            out['gpmp'] = gpmp
        mxmp = self.mxmp()
        if mxmp is not None:
            out['mxmp'] = mxmp
        cntr = self.cntr()
        if cntr is not None:
            out['cntr'] = cntr
        srvo = self.srvo()
        if srvo is not None:
            out['srvo'] = srvo
        ilcf = self.ilcf()
        if ilcf is not None:
            out['ilcf'] = ilcf.encode('ascii')
        return out


# ---------------------------------------------------------------------------
# Module-level authoring API (for unit .py files) + loader
# ---------------------------------------------------------------------------

_CURRENT = []


def unit(addr, type, vref=VREF_DEFAULT):
    u = Unit(addr, type, vref=vref)
    _CURRENT.append(u)
    return u


def pins(**roles):
    if not _CURRENT:
        raise DSLError("pins() called before unit()")
    return _CURRENT[-1].pins(**roles)


def interlock(name, when=None, drive=None):
    if not _CURRENT:
        raise DSLError("interlock() called before unit()")
    return _CURRENT[-1].interlock(name, when, drive)


def load(path):
    """Exec a unit-definition .py and return the Unit it defined."""
    _CURRENT.clear()
    ns = {'unit': unit, 'pins': pins, 'interlock': interlock,
          'Unit': Unit, 'DSLError': DSLError}
    with open(path) as f:
        exec(compile(f.read(), path, 'exec'), ns)
    if not _CURRENT:
        raise DSLError("%s defined no unit()" % path)
    if len(_CURRENT) > 1:
        raise DSLError("%s defined %d units; one per file" % (path, len(_CURRENT)))
    return _CURRENT[-1]


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _selftest():
    # 1. MIXED: AND condition + drive, volts -> count (2.5/3.3*4095 = 3102).
    #    Coverage requires every pad set; unused pads held safe. D8 watched -> drive D7.
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc', D8='in:up', D7='out',
           D1='safe', D2='safe', D3='safe', D9='safe', D10='safe')
    u.interlock('safe', when='A0 > 2.5V && D8', drive={'D7': 1})
    f = u.files()
    assert f['idnt'] == bytes([0x55, 3]), f['idnt']
    exp = (b"safe;cfg[(A0):adc,(D7):out,(D8):in,up];"
           b"watch[A0:gt:3102,D8:1];out_ok[D7:1];out_err[D7:0]")
    assert f['ilcf'] == exp, f['ilcf']

    # 2. OR + NOT -> two DNF groups; ~D8 -> ne 1
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc', D8='in', D6='out')
    u.interlock('s', when='A0 > 2.5V && D8 || ~D8', drive={'D6': 1})
    assert u.ilcf().split(';')[2] == "watch[A0:gt:3102,D8:1|D8:ne:1]", u.ilcf()

    # 3. De-Morgan: ~(A0 > 1.0V || D8) -> (A0<=1241) && (~D8) -> one group
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc', D8='in', D6='out')
    u.interlock('s', when='~(A0 > 1.0V || D8)', drive={'D6': 0})
    assert u.ilcf().split(';')[2] == "watch[A0:le:1241,D8:ne:1]", u.ilcf()
    assert u.ilcf().split(';')[3] == "out_ok[D6:0]"      # ok=0
    assert u.ilcf().split(';')[4] == "out_err[D6:1]"     # err=1-0

    # 3b. MIXED GPIO debounce: ms -> shift depth at the ~100 Hz tick (50ms -> 5)
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc', D8='in:up:debounce_50ms', D6='out')
    u.interlock('s', when='A0 > 2.5V && D8', drive={'D6': 1})
    assert u.ilcf().split(';')[1] == \
        "cfg[(A0):adc,(D6):out,(D8):in,up,debounce_5]", u.ilcf()   # canonical PAD order
    # rounds to nearest tick: 24ms -> depth 2
    u = Unit(0x55, 'MIXED'); u.pins(D8='in:debounce_24ms', D6='out')
    u.interlock('s', when='D8', drive={'D6': 1})
    assert u.ilcf().split(';')[1] == "cfg[(D6):out,(D8):in,debounce_2]", u.ilcf()

    # 4. IDLE: idnt only (no pins allowed). ADC/SERVO/COUNTER now require every
    #    configurable pad to be set (use 'safe'); ADC-with-no-interlock emits the
    #    null ilcf, COUNTER/SERVO carry a config file (`cntr`/`srvo`).
    assert Unit(0x60, 'IDLE').files() == {'idnt': bytes([0x60, 0])}, Unit(0x60, 'IDLE').files()
    adc_safe = {'D2': 'safe', 'D3': 'safe', 'D7': 'safe', 'D8': 'safe', 'D9': 'safe', 'D10': 'safe'}
    assert Unit(0x60, 'ADC').pins(**adc_safe).files() == {'idnt': bytes([0x60, 2]), 'ilcf': b'off'}
    # all-safe SERVO -> srvo with all 8 pads role 0 (D6 = e-stop, implicit)
    servo_safe = {p: 'safe' for p in SERVO_PINS}
    assert Unit(0x60, 'SERVO').servo().pins(**servo_safe).files() == {
        'idnt': bytes([0x60, 4]), 'srvo': bytes([1] + [0] * 8)}
    # SERVO with declared pads: D0/D1 servo, D8 adc, D9 out, rest safe; role byte per
    #   pad. order D0,D1,D2,D3,D7,D8,D9,D10 -> [5,5,0,0,0,3,2,0]
    u = Unit(0x55, 'SERVO').servo()
    u.pins(D0='servo', D1='servo', D8='adc', D9='out',
           D2='safe', D3='safe', D7='safe', D10='safe')
    assert u.srvo() == bytes([1, 5, 5, 0, 0, 0, 3, 2, 0]), list(u.srvo())

    # 4b. COUNTER cntr: D1 up/rising + D2 down/both at 2 kHz; D0 free (DAC pad).
    #     ch byte = bit0 enable | pull<<1 | edge<<3. D1=idx1: 1|1<<1=0x03;
    #     D2=idx2: 1|2<<1|2<<3 = 1|4|16 = 0x15. rate 2000 = 0x07D0.
    u = Unit(0x55, 'COUNTER').counter(rate=2000)
    u.pins(D1='count:up:rising', D2='count:down:both',
           D0='safe', D3='safe', D7='safe', D8='safe', D9='safe', D10='safe', D6='safe')
    exp = bytes([1, 0xD0, 0x07, 0x00, 0x03, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
    assert u.files()['cntr'] == exp, list(u.files()['cntr'])
    assert u.files()['idnt'] == bytes([0x55, 5])
    # all-safe COUNTER -> cntr with all channels disabled (role 0), default 1 kHz
    cnt_safe = {p: 'safe' for p in COUNTER_PINS}
    assert Unit(0x60, 'COUNTER').pins(**cnt_safe).cntr() == bytes([1, 0xE8, 0x03] + [0] * 9)

    # 5. error cases
    for fn in (
        lambda: Unit(0x55, 'MIXED').pins(D8='in').interlock('s', when='Z9 && D8').ilcf(),
        lambda: Unit(0x55, 'MIXED').pins(A0='adc').interlock('s', when='A0').ilcf(),
        lambda: Unit(0x55, 'MIXED').pins(D6='out').interlock('s', when='D6 > 1').ilcf(),
        lambda: Unit(0x55, 'BOGUS'),
        lambda: Unit(0x99, 'MIXED'),
        # debounce is MIXED-only -> rejected on GPIO
        lambda: Unit(0x55, 'GPIO').pins(D8='in:up:debounce_50ms', D6='out')
                    .interlock('s', when='D8', drive={'D6': 1}).ilcf(),
        # debounce out of range: 200ms -> depth 20 > 15
        lambda: Unit(0x55, 'MIXED').pins(D8='in:debounce_200ms', D6='out')
                    .interlock('s', when='D8', drive={'D6': 1}).ilcf(),
        # debounce must carry ms units
        lambda: Unit(0x55, 'MIXED').pins(D8='in:debounce_5', D6='out')
                    .interlock('s', when='D8', drive={'D6': 1}).ilcf(),
        # counter rate out of range
        lambda: Unit(0x55, 'COUNTER').counter(rate=50000),
        # counter() is COUNTER-only
        lambda: Unit(0x55, 'GPIO').counter(rate=1000),
        # bad counter modifier
        lambda: Unit(0x55, 'COUNTER').pins(D1='count:up:bogus').cntr(),
        # D4 is not a counter pad (I2C pin)
        lambda: Unit(0x55, 'COUNTER').pins(D4='count:up').cntr(),
    ):
        try:
            fn()
        except DSLError:
            pass
        else:
            raise AssertionError("expected DSLError from %r" % fn)

    # 6. clause-count guard: 3-deep OR-of-ANDs exceeding 8 clauses
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc', A1='adc', D8='in', D9='in')
    u.interlock('big', when='(A0>1V || A1>1V) && (D8 || D9) && (A0<3V || A1<3V)')
    try:
        u.ilcf()
    except DSLError as e:
        assert 'IL_MAX_WATCHES' in str(e), e
    else:
        raise AssertionError("expected clause-count overflow")

    # 7. pin names are case-insensitive: lowercase pins/expr == uppercase
    up = Unit(0x55, 'MIXED'); up.pins(A0='adc', D8='in', D6='out')
    up.interlock('s', when='A0 > 2.5V && D8', drive={'D6': 1})
    lo = Unit(0x55, 'MIXED'); lo.pins(a0='adc', d8='in', d6='out')
    lo.interlock('s', when='a0 > 2.5v && d8', drive={'d6': 1})
    assert up.ilcf() == lo.ilcf(), (up.ilcf(), lo.ilcf())

    # 8. GPIO: full pin map (gpmp) + interlock cfg restricted to its own pins
    u = Unit(0x20, 'GPIO')
    u.pins(D0='in:up', D1='in:up', D2='in:none', D3='in:down',
           D7='out:0', D8='out:0', D9='out:1', D10='out:0')
    u.interlock('safe', '(D0 && D1) && (D2 || D3)', drive={'D8': 1})
    f = u.files()
    assert f['idnt'] == bytes([0x20, 1]), f['idnt']
    # bits 0..7 = D0,D1,D2,D3,D7,D8,D9,D10
    #  DIR    outputs D7,D8,D9,D10 = 0xF0
    #  PULLEN pulled inputs D0,D1,D3 = 0x0B   (D2 none)
    #  OUT    up-inputs D0,D1 + out:1 D9 = 0x43
    #  OD     none -> 0x00 ; INTCFG interlock present -> 0x01
    assert f['gpmp'] == bytes([2, 0xF0, 0x0B, 0x43, 0x00, 0x01]), list(f['gpmp'])
    exp = ("safe;cfg[(D0):in,up,(D1):in,up,(D2):in,(D3):in,down,(D8):out];"
           "watch[D0:1,D1:1,D2:1|D0:1,D1:1,D3:1];out_ok[D8:1];out_err[D8:0]")
    assert f['ilcf'].decode() == exp, f['ilcf'].decode()

    # 9. GPIO: all 8 channels required; reject non-channel pins; GPIO w/o
    #    interlock -> gpmp only (no ilcf), INTCFG disabled.
    for fn in (
        lambda: Unit(0x20, 'GPIO').pins(D0='in:up').gpmp(),                  # missing pins
        lambda: Unit(0x20, 'GPIO').pins(D6='out:0').gpmp(),                  # D6=INT, not a channel
    ):
        try:
            fn()
        except DSLError:
            pass
        else:
            raise AssertionError("expected DSLError from %r" % fn)
    g = Unit(0x20, 'GPIO')
    g.pins(**{p: 'out:0' for p in GPIO_PINS})
    fg = g.files()
    assert set(fg) == {'idnt', 'gpmp', 'ilcf'}, set(fg)    # inert ilcf overwrites stale
    assert fg['ilcf'] == b'off', fg['ilcf']
    assert fg['gpmp'] == bytes([2, 0xFF, 0x00, 0x00, 0x00, 0x00]), list(fg['gpmp'])

    # 10. open-collector output (oc): OD bit set, OUT=1 (released/Hi-Z) at boot.
    #     oc:up adds the internal pull-up (PULLEN bit) for that pad.
    u = Unit(0x20, 'GPIO')
    u.pins(D0='in:up', D1='in:none', D2='in:none', D3='in:none',
           D7='out:0', D8='oc', D9='out:1', D10='oc:up')
    g = u.gpmp()
    #  DIR  outputs D7,D8,D9,D10 = 0xF0
    #  PULLEN D0 up + D10 oc:up = bits 0,7 = 0x81
    #  OUT  D0(up) + D8,D10(oc released) + D9(out:1) = bits 0,5,6,7 = 0xE1
    #  OD   D8,D10 = bits 5,7 = 0xA0
    assert g == bytes([2, 0xF0, 0x81, 0xE1, 0xA0, 0x00]), list(g)

    # 11. ADC mode interlock: single-channel (A1) stream selectors (.stat.window)
    #     with the khz1/hz100/hz10 downsample windows + default instantaneous.
    u = Unit(0x30, 'ADC')
    u.pins(**{p: 'safe' for p in ('D2', 'D3', 'D7', 'D8', 'D9', 'D10')})  # coverage
    u.interlock('ov', 'A1.avg.khz1 > 1.5V || A1.rms.hz100 > 0.5V', drive={'D6': 1})
    f = u.files()
    assert f['idnt'] == bytes([0x30, 2]), f['idnt']
    assert set(f) == {'idnt', 'ilcf'}, set(f)               # no gpmp in ADC mode
    exp = ("ov;cfg[(D6):out];watch[A1_avg_khz1:gt:1861|A1_rms_hz100:gt:620];"
           "out_ok[D6:1];out_err[D6:0]")           # 1.5V->1861, 0.5V->620
    assert f['ilcf'].decode() == exp, f['ilcf'].decode()
    u2 = Unit(0x30, 'ADC'); u2.interlock('x', 'A1 > 2.0V', drive={'D6': 1})
    assert u2.ilcf().split(';')[2] == "watch[A1:gt:2482]", u2.ilcf()   # default instantaneous
    for bad in ('A0 > 1V', 'A1.foo.khz1 > 1V', 'A1.avg.turbo > 1V', 'A1 > 1V'):
        try:
            Unit(0x30, 'ADC').interlock('x', bad,
                                        drive=None if bad == 'A1 > 1V' else {'D6': 1}).ilcf()
        except DSLError:
            pass
        else:
            raise AssertionError("expected DSLError: %s" % bad)

    print("slave_dsl self-test: PASS")


if __name__ == '__main__':
    _selftest()
