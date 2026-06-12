#!/usr/bin/env python3
"""slave_dsl.py — Python-embedded DSL that compiles a SAMD21 config-chip unit
definition into the raw on-chip files (`idnt`, `ilcf`).

Three layers, matching the firmware:

  * unit(addr, type)            -> emits `idnt` = raw [addr, mode] (ALL modes)
  * pins(D0='adc', D8='in:up')  -> the `cfg[...]` pin-role section (GPIO/MIXED)
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

ADC_FULLSCALE = 4095
VREF_DEFAULT = 3.3                    # full-scale volts (INTVCC1 ref + GAIN=DIV2)

# GPIO power-on pin map ("gpmp"): the 8 usable GPIO channels, in gpmp bit order
# 0..7 (D4/D5 = I2C, D6 = INT). Applied at startup before the interlock arms.
GPIO_PINS = ('D0', 'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10')
GPMP_VERSION = 2

_OP_FROM_SYM = {'>': 'gt', '<': 'lt', '>=': 'ge', '<=': 'le', '==': 'eq', '!=': 'ne'}
_OP_INVERT = {'gt': 'le', 'le': 'gt', 'lt': 'ge', 'ge': 'lt', 'eq': 'ne', 'ne': 'eq'}

_PULL_MODS = {'up', 'down'}
_ROLE_BASES = {'in', 'out', 'adc'}


class DSLError(Exception):
    """Raised on any malformed unit definition or boolean expression."""


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

# ADC-mode interlock streams: a watch reads one of these per channel.
ADC_STATS   = ('avg', 'min', 'max', 'rms')         # windowed stats
ADC_WINDOWS = {'fast': 0, 'mid': 1, 'slow': 2}     # 10 Hz / 1 Hz / 0.1 Hz tumbling windows
# Channels the ADC sweep samples (D6/A6 = interlock output, A0 = DAC -> not watchable).
ADC_WATCH_PINS = ('A1', 'A2', 'A3', 'A7', 'A8', 'A9', 'A10',
                  'D1', 'D2', 'D3', 'D7', 'D8', 'D9', 'D10')


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
            raise DSLError("ADC channel %r not watchable (A1-A3,A7-A10; A0=DAC, A6/D6=output)" % chan)
        operand = chan
        if self._peek()[0] == 'dot':
            self._next()
            k, stat = self._next()
            if k != 'ident' or stat.lower() not in ADC_STATS:
                raise DSLError("expected stat avg/min/max/rms after '.', got %r" % (stat,))
            if self._next()[0] != 'dot':
                raise DSLError("expected .window after .%s (fast/mid/slow)" % stat)
            k, win = self._next()
            if k != 'ident' or win.lower() not in ADC_WINDOWS:
                raise DSLError("expected window fast/mid/slow, got %r" % (win,))
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

    # -- authoring -------------------------------------------------------
    def pins(self, **roles):
        for label, spec in roles.items():
            parts = str(spec).split(':')
            base, mods = parts[0], parts[1:]
            if base not in _ROLE_BASES:
                raise DSLError("pin %s: bad role base %r" % (label, base))
            self.roles[label.upper()] = (base, mods)   # pin names case-insensitive
        return self

    def interlock(self, name, when, drive=None):
        if len(name) >= IL_NAME_MAX:
            raise DSLError("interlock name %r >= %d chars" % (name, IL_NAME_MAX))
        drive = {k.upper(): v for k, v in (drive or {}).items()}
        self.il = (name, when, drive)
        return self

    # -- emission --------------------------------------------------------
    def idnt(self):
        return bytes([self.addr, self.mode])

    def _cfg_token(self, pin):
        """il_parse cfg token for one pin, derived from its declared role."""
        base, mods = self.roles[pin]
        if base == 'adc':
            return "(%s):adc%s" % (pin, "".join("," + m for m in mods))
        if base == 'in':
            pull = mods[0] if mods else 'none'
            sfx = {'up': ',up', 'down': ',down', 'none': ''}.get(pull)
            if sfx is None:
                raise DSLError("input %s pull must be up/down/none, got %r" % (pin, pull))
            return "(%s):in%s" % (pin, sfx)
        if base == 'out':
            return "(%s):out" % pin          # interlock drives it; init lives in gpmp
        raise DSLError("pin %s: bad role base %r" % (pin, base))

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
            if p not in self.roles or self.roles[p][0] == 'out':
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
            if self.roles.get(label, (None,))[0] != 'out':
                raise DSLError("drive pin %r not declared as 'out'" % label)

        # MIXED's cfg declares every channel (the mode samples them all); GPIO's
        # cfg declares only the interlock's pins (all 8 live in gpmp, can't fit).
        if self.mode == MODES['MIXED']:
            cfg_pins = list(self.roles.keys())
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
        cfg = ",".join("(%s):out" % d.upper() for d in drive)
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
                if kind == 'od':                       # open-drain, powers up released (Hi-Z)
                    od_bm |= bit
                    out_bm |= bit
                elif kind in ('0', '1'):               # push-pull, drive level
                    if kind == '1':
                        out_bm |= bit
                else:
                    raise DSLError("output %s must be 0, 1 or od, got %r" % (pin, kind))
            elif base == 'in':
                pull = mods[0] if mods else 'none'
                if pull not in ('up', 'down', 'none'):
                    raise DSLError("input %s pull must be up/down/none, got %r" % (pin, pull))
                if pull != 'none':
                    pullen_bm |= bit
                    if pull == 'up':
                        out_bm |= bit
            else:
                raise DSLError("GPIO pin %s: role %r not allowed (in/out only)" % (pin, base))
        intcfg = 0x01 if self.il else 0x00   # bit0: open-drain active-low INT enabled
        return bytes([GPMP_VERSION, dir_bm, pullen_bm, out_bm, od_bm, intcfg])

    def files(self):
        """Return {name: bytes} ready for the commission tool to write."""
        out = {'idnt': self.idnt()}
        gpmp = self.gpmp()
        if gpmp is not None:
            out['gpmp'] = gpmp
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
    # 1. MIXED: AND condition + drive, volts -> count (2.5/3.3*4095 = 3102)
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc:oversample_16', D8='in:up', D6='out')
    u.interlock('safe', when='A0 > 2.5V && D8', drive={'D6': 1})
    f = u.files()
    assert f['idnt'] == bytes([0x55, 3]), f['idnt']
    exp = (b"safe;cfg[(A0):adc,oversample_16,(D8):in,up,(D6):out];"
           b"watch[A0:gt:3102,D8:1];out_ok[D6:1];out_err[D6:0]")
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

    # 4. SERVO/COUNTER/IDLE: idnt only. ADC (interlock-capable) with no interlock
    #    emits the null ilcf, like GPIO/MIXED.
    for ty, m in (('SERVO', 4), ('COUNTER', 5), ('IDLE', 0)):
        u = Unit(0x60, ty)
        assert u.files() == {'idnt': bytes([0x60, m])}, (ty, u.files())
    assert Unit(0x60, 'ADC').files() == {'idnt': bytes([0x60, 2]), 'ilcf': b'off'}

    # 5. error cases
    for fn in (
        lambda: Unit(0x55, 'MIXED').pins(D8='in').interlock('s', when='Z9 && D8').ilcf(),
        lambda: Unit(0x55, 'MIXED').pins(A0='adc').interlock('s', when='A0').ilcf(),
        lambda: Unit(0x55, 'MIXED').pins(D6='out').interlock('s', when='D6 > 1').ilcf(),
        lambda: Unit(0x55, 'BOGUS'),
        lambda: Unit(0x99, 'MIXED'),
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

    # 10. open-drain output (out:od): OD bit set, OUT=1 (released/Hi-Z) at boot
    u = Unit(0x20, 'GPIO')
    u.pins(D0='in:up', D1='in:none', D2='in:none', D3='in:none',
           D7='out:0', D8='out:od', D9='out:1', D10='out:od')
    g = u.gpmp()
    #  DIR  outputs D7,D8,D9,D10 = 0xF0
    #  PULLEN D0 up = 0x01
    #  OUT  D0(up) + D8,D10(od released) + D9(out:1) = bits 0,5,6,7 = 0xE1
    #  OD   D8,D10 = bits 5,7 = 0xA0
    assert g == bytes([2, 0xF0, 0x01, 0xE1, 0xA0, 0x00]), list(g)

    # 11. ADC mode interlock: stream selectors (.stat.window) + default instantaneous
    u = Unit(0x30, 'ADC')
    u.interlock('ov', 'A1.avg.fast > 1.5V || A2.rms.mid > 0.5V', drive={'D6': 1})
    f = u.files()
    assert f['idnt'] == bytes([0x30, 2]), f['idnt']
    assert set(f) == {'idnt', 'ilcf'}, set(f)               # no gpmp in ADC mode
    exp = ("ov;cfg[(D6):out];watch[A1_avg_fast:gt:1861|A2_rms_mid:gt:620];"
           "out_ok[D6:1];out_err[D6:0]")           # 1.5V->1861, 0.5V->620
    assert f['ilcf'].decode() == exp, f['ilcf'].decode()
    u2 = Unit(0x30, 'ADC'); u2.interlock('x', 'A1 > 2.0V', drive={'D6': 1})
    assert u2.ilcf().split(';')[2] == "watch[A1:gt:2482]", u2.ilcf()   # default instantaneous
    for bad in ('A0 > 1V', 'A1.foo.fast > 1V', 'A1.avg.turbo > 1V', 'A1 > 1V'):
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
