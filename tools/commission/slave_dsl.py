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

Boolean expressions support and / or / not / parentheses and comparisons:

    interlock('safe',
        when='(A0 > 2.5V and D8) or not D9',
        drive={'D6': 1})        # D6 = 1 while the condition holds, else 0

Comparisons:  >  <  >=  <=  ==  !=   (a bare GPIO pin means "== 1").
ADC thresholds may be volts ('2.5V', full-scale 0..VDDANA≈3.3 V over 0..4095)
or a raw count (0..4095).  not/De-Morgan is pushed into the comparison operator
(the firmware has no NOT), then AND-over-OR is distributed to DNF: clauses in a
group are ANDed (`,`), groups are ORed (`|`).

Firmware limits (enforced here with clear errors):
  <=4 watched input pins, <=8 watch clauses total, <=2 outputs,
  ilcf string <=128 bytes, interlock name <16 chars.

Authoring style — a unit .py file just calls the module-level functions:

    unit(addr=0x55, type='MIXED')
    pins(A0='adc:oversample_16', D8='in:up', D6='out')
    interlock('safe', when='A0 > 2.5V and D8', drive={'D6': 1})

then `load('lift.py')` returns the Unit; `unit.files()` gives {'idnt':..,'ilcf':..}.
"""

import re

# ---- firmware constants (keep in sync with samd21_interlocks.h) ------------
MODES = {
    'IDLE': 0, 'GPIO': 1, 'PIO': 1, 'ADC': 2, 'MIXED': 3,
    'SERVO': 4, 'COUNTER': 5,
}
MODE_NAME = {0: 'IDLE', 1: 'GPIO', 2: 'ADC', 3: 'MIXED', 4: 'SERVO', 5: 'COUNTER'}
MODES_WITH_ILCF = {1, 3}              # GPIO + MIXED carry a pin/interlock file

IL_MAX_INPUTS = 4
IL_MAX_WATCHES = 8
IL_MAX_OUTPUTS = 2
IL_DSL_MAX = 128
IL_NAME_MAX = 16

ADC_FULLSCALE = 4095
VREF_DEFAULT = 3.3                    # full-scale volts (INTVCC1 ref + GAIN=DIV2)

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
        (?P<op>>=|<=|==|!=|>|<) |
        (?P<lp>\() |
        (?P<rp>\)) |
        (?P<num>\d+(?:\.\d+)?V?) |
        (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
    )
''', re.VERBOSE)


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

    def __init__(self, toks, roles, vref):
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
        while self._peek() == ('ident', 'or'):
            self._next()
            children.append(self._and())
        return children[0] if len(children) == 1 else ('or', children)

    def _and(self):
        children = [self._not()]
        while self._peek() == ('ident', 'and'):
            self._next()
            children.append(self._not())
        return children[0] if len(children) == 1 else ('and', children)

    def _not(self):
        if self._peek() == ('ident', 'not'):
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
            if val in ('and', 'or', 'not'):
                raise DSLError("unexpected keyword %r" % val)
            return self._comparison(val)
        raise DSLError("expected a pin or '(', got %r" % (val,))

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
        if tok.endswith('V'):
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


def _compile_expr(when, roles, vref):
    """Boolean expression -> list of groups, each a list of (pin, op, thr)."""
    ast = _ExprParser(_tokenize(when), roles, vref).parse()
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
            self.roles[label] = (base, mods)
        return self

    def interlock(self, name, when, drive=None):
        if len(name) >= IL_NAME_MAX:
            raise DSLError("interlock name %r >= %d chars" % (name, IL_NAME_MAX))
        self.il = (name, when, dict(drive or {}))
        return self

    # -- emission --------------------------------------------------------
    def idnt(self):
        return bytes([self.addr, self.mode])

    def _cfg_section(self):
        items = []
        for label, (base, mods) in self.roles.items():
            items.append("(%s):%s" % (label, ",".join([base] + mods)))
        return "cfg[%s]" % ",".join(items)

    def ilcf(self):
        if self.mode not in MODES_WITH_ILCF:
            return None
        if not self.roles:
            raise DSLError("%s mode needs pins()" % MODE_NAME[self.mode])
        name, when, drive = (self.il or ("il", None, {}))
        sections = [name, self._cfg_section()]

        if when:
            groups = _compile_expr(when, self.roles, self.vref)
            inputs = {p for g in groups for (p, _, _) in g}
            for p in inputs:
                if p not in self.roles or self.roles[p][0] == 'out':
                    raise DSLError("watched pin %r is not a declared input" % p)
            if len(inputs) > IL_MAX_INPUTS:
                raise DSLError("%d watched inputs > IL_MAX_INPUTS=%d"
                               % (len(inputs), IL_MAX_INPUTS))
            total = sum(len(g) for g in groups)
            if total > IL_MAX_WATCHES:
                raise DSLError("%d watch clauses > IL_MAX_WATCHES=%d (after DNF "
                               "expansion); simplify the expression" % (total, IL_MAX_WATCHES))
            sections.append("watch[%s]" % "|".join(
                ",".join(self._clause(p, op, thr) for (p, op, thr) in g) for g in groups))

        if drive:
            if len(drive) > IL_MAX_OUTPUTS:
                raise DSLError("%d outputs > IL_MAX_OUTPUTS=%d" % (len(drive), IL_MAX_OUTPUTS))
            ok, err = [], []
            for label, v in drive.items():
                if self.roles.get(label, (None,))[0] != 'out':
                    raise DSLError("drive pin %r not declared as 'out'" % label)
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

    def files(self):
        """Return {name: bytes} ready for the commission tool to write."""
        out = {'idnt': self.idnt()}
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
    u.interlock('safe', when='A0 > 2.5V and D8', drive={'D6': 1})
    f = u.files()
    assert f['idnt'] == bytes([0x55, 3]), f['idnt']
    exp = (b"safe;cfg[(A0):adc,oversample_16,(D8):in,up,(D6):out];"
           b"watch[A0:gt:3102,D8:1];out_ok[D6:1];out_err[D6:0]")
    assert f['ilcf'] == exp, f['ilcf']

    # 2. OR + NOT -> two DNF groups; not D8 -> ne 1
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc', D8='in', D6='out')
    u.interlock('s', when='A0 > 2.5V and D8 or not D8', drive={'D6': 1})
    assert u.ilcf().split(';')[2] == "watch[A0:gt:3102,D8:1|D8:ne:1]", u.ilcf()

    # 3. De-Morgan: not (A0 > 1.0V or D8) -> (A0<=372) and (not D8) -> one group
    u = Unit(0x55, 'MIXED')
    u.pins(A0='adc', D8='in', D6='out')
    u.interlock('s', when='not (A0 > 1.0V or D8)', drive={'D6': 0})
    assert u.ilcf().split(';')[2] == "watch[A0:le:1241,D8:ne:1]", u.ilcf()
    assert u.ilcf().split(';')[3] == "out_ok[D6:0]"      # ok=0
    assert u.ilcf().split(';')[4] == "out_err[D6:1]"     # err=1-0

    # 4. SERVO/COUNTER/ADC/IDLE: idnt only, no ilcf
    for ty, m in (('SERVO', 4), ('COUNTER', 5), ('ADC', 2), ('IDLE', 0)):
        u = Unit(0x60, ty)
        assert u.files() == {'idnt': bytes([0x60, m])}, (ty, u.files())

    # 5. error cases
    for fn in (
        lambda: Unit(0x55, 'MIXED').pins(D8='in').interlock('s', when='Z9 and D8').ilcf(),
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
    u.interlock('big', when='(A0>1V or A1>1V) and (D8 or D9) and (A0<3V or A1<3V)')
    try:
        u.ilcf()
    except DSLError as e:
        assert 'IL_MAX_WATCHES' in str(e), e
    else:
        raise AssertionError("expected clause-count overflow")

    print("slave_dsl self-test: PASS")


if __name__ == '__main__':
    _selftest()
