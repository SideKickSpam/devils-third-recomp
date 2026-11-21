#!/usr/bin/env python3
"""Demangler for Green Hills (EDG front end, cfront-style) C++ symbol names, as found in dt.elf.

Supported constructs (all observed in Devil's Third):
  name__<class>[C|S|V]F<params>[_<ret>]  functions, members, const/static members, return types
  <len><id>, Q<n>_ / Q_<nn>_              length-prefixed and qualified names
  __tm__<len>_<args>                      template arguments (__ps__ = partial-spec pattern, __pt__ = template)
  Z<n>Z, Z<n>_<level>Z                    template parameter reference (Z<n>Z is replaced by the actual
                                          argument from the function's or enclosing class's __tm__ block)
  X<type>L_<len>_<value>, X<name>         non-type template argument: literal, or address of an entity
  T<n>, N<count><n>                       repeated parameter types
  M<class><type>                          pointer to member
  U<len><qual>                            vendor qualifier (__restrict)
  BV<n>_<type>                            n-wide vector type, rendered __vec<n><type> (inferred: paired singles)
  <len><name>__L<n>__<function>           local class inside a function
  name__tm__..__S__...                    static function-template specialisation
  __N_<len>_<file>                        anonymous namespace
  __CPR<len>__...J<off>J...JJ             GHS name compression (back-references, JJ = literal J)
  __<name>_static_in_<path>               file-static functions
  __ghs_thunk__<adj>__<name>              this-adjusting thunk
  __sti__/__std__                         static initialisation/destruction functions
  __ct, __dt, __nw, __op<type>, ...       constructors, destructors, operators

demangle() returns None for names that are not mangled (plain C) and raises DemangleError when a
name looks mangled but cannot be decoded.

Usage as a CLI: ghs_demangle.py NAME...   (prints "mangled -> demangled", exits 1 if any failed)
"""

import re
import sys

OPERATORS = {
    "nw": "operator new", "dl": "operator delete", "nwa": "operator new[]", "dla": "operator delete[]",
    "pl": "operator+", "mi": "operator-", "ml": "operator*", "dv": "operator/", "md": "operator%",
    "er": "operator^", "ad": "operator&", "or": "operator|", "co": "operator~", "nt": "operator!",
    "as": "operator=", "lt": "operator<", "gt": "operator>", "apl": "operator+=", "ami": "operator-=",
    "amu": "operator*=", "adv": "operator/=", "amd": "operator%=", "aer": "operator^=", "aad": "operator&=",
    "aor": "operator|=", "ls": "operator<<", "rs": "operator>>", "ars": "operator>>=", "als": "operator<<=",
    "eq": "operator==", "ne": "operator!=", "le": "operator<=", "ge": "operator>=", "aa": "operator&&",
    "oo": "operator||", "pp": "operator++", "mm": "operator--", "cm": "operator,", "rm": "operator->*",
    "rf": "operator->", "cl": "operator()", "vc": "operator[]",
}
# Longest codes first so "nwa" wins over "nw".
OPERATOR_RE = re.compile(r"__(" + "|".join(sorted(OPERATORS, key=len, reverse=True)) + r")(?=__|$)")

BUILTINS = {
    "v": "void", "b": "bool", "c": "char", "s": "short", "i": "int", "l": "long", "x": "long long",
    "L": "long long", "f": "float", "d": "double", "r": "long double", "w": "wchar_t", "e": "...",
}


class DemangleError(Exception):
    pass


# Template arguments that Z<n>Z refers to in the signature being decoded (innermost last).
# GHS mangles a specialisation's generic signature, so binding these makes the output readable.
_bindings = []


def decompress(name):
    """Expand a __CPR<len>__ name. J<off>J copies the length-prefixed identifier at offset <off> of the
    output produced so far; JJ is a literal J."""
    m = re.match(r"__CPR(\d+)__", name)
    if not m:
        return name
    body, out, i = name[m.end():], [], 0
    expected = int(m.group(1))
    while i < len(body):
        if body.startswith("JJ", i):
            out.append("J")
            i += 2
            continue
        r = re.match(r"J(\d+)J", body[i:])
        if r:
            text = "".join(out)
            off = int(r.group(1))
            n = re.match(r"\d+", text[off:])
            if not n:
                raise DemangleError(f"bad back-reference J{off}J")
            out.append(text[off:off + len(n.group()) + int(n.group())])
            i += len(r.group())
            continue
        out.append(body[i])
        i += 1
    result = "".join(out)
    if len(result) != expected:
        raise DemangleError(f"CPR length mismatch ({len(result)} != {expected})")
    return result


class Parser:
    def __init__(self, s):
        self.s = s
        self.i = 0
        self.params = []  # parameter types seen in the current function signature, for T<n>/N<c><n>
        self.last_ident = ""  # raw text of the last <len><id> parsed, to recover a scope's template args

    def peek(self, n=1):
        return self.s[self.i:self.i + n]

    def eat(self, text):
        if self.s.startswith(text, self.i):
            self.i += len(text)
            return True
        return False

    def expect(self, text):
        if not self.eat(text):
            raise DemangleError(f"expected {text!r} at {self.i}")

    def number(self):
        m = re.compile(r"\d+").match(self.s, self.i)
        if not m:
            raise DemangleError(f"expected number at {self.i}")
        self.i = m.end()
        return int(m.group())

    def done(self):
        return self.i >= len(self.s)

    # --- names ---
    def source_name(self):
        """<len><id>, where <id> may itself carry template arguments or be an anonymous namespace."""
        n = self.number()
        if self.i + n > len(self.s):
            raise DemangleError("name runs past end")
        ident = self.s[self.i:self.i + n]
        self.i += n
        self.last_ident = ident
        return render_identifier(ident)

    def qualified_name(self):
        if self.eat("Q"):
            if self.eat("_"):
                count = self.number()
            else:
                count = int(self.s[self.i])
                self.i += 1
            self.expect("_")
            return "::".join(self.name_component() for _ in range(count))
        return self.source_name()

    def name_component(self):
        """One component of a Q-qualified name: <len><id> or a template parameter Z<n>Z."""
        if self.peek() == "Z":
            return self.template_param()
        return self.source_name()

    def template_param(self):
        m = TPARAM_RE.match(self.s, self.i)
        if not m:
            raise DemangleError(f"bad template parameter at {self.i}")
        self.i = m.end()
        if m.group(2):
            return f"T{m.group(1)}_{m.group(2)}"  # level-qualified: binding is ambiguous, leave symbolic
        n = int(m.group(1))
        if _bindings and _bindings[-1] and n <= len(_bindings[-1]):
            return _bindings[-1][n - 1]
        return f"T{n}"

    # --- types ---
    def type(self):
        quals = []
        while True:
            c = self.peek()
            if c == "C":
                quals.append("const")
            elif c == "V":
                quals.append("volatile")
            elif c in "US" and self.peek(2)[1:] in BUILTINS:
                quals.append("unsigned" if c == "U" else "signed")
            else:
                break
            self.i += 1
        c = self.peek()
        if not c:
            raise DemangleError("unexpected end in type")
        if c == "P" or c == "R":
            self.i += 1
            inner = self.type()
            base = wrap_declarator(inner, "*" if c == "P" else "&")
        elif c == "A":
            self.i += 1
            dim = self.number()
            self.expect("_")
            inner = self.type()
            base = wrap_declarator(inner, f"[{dim}]", suffix=True)
        elif c == "F":
            self.i += 1
            args = self.param_list(nested=True)
            ret = "void"
            if self.eat("_"):
                ret = self.type()
            base = f"{ret} (%s)({args})"
        elif c == "B" and self.peek(2) == "BV":
            # B V<n>_<type>: only seen in Wwise DSP code; inferred to be GHS's n-wide vector type
            # (paired singles for V2_f). Rendered as __vec<n><type>.
            self.i += 2
            n = self.number()
            self.expect("_")
            base = f"__vec{n}<{finish(self.type())}>"
        elif c == "U" and self.peek(2)[1:].isdigit():
            # U<len><qualifier>: vendor-extended qualifier such as __restrict, applied to the following type.
            self.i += 1
            n = self.number()
            qual = self.s[self.i:self.i + n]
            self.i += n
            base = wrap_declarator(self.type(), f" {qual}", suffix=True)
        elif c == "M":
            self.i += 1
            cls = self.template_param() if self.peek() == "Z" else self.qualified_name()
            inner = self.type()
            base = wrap_declarator(inner, f"{cls}::*")
        elif c == "Z":
            base = self.template_param()
        elif c.isdigit() or c == "Q":
            base = self.qualified_name()
        elif c in BUILTINS:
            self.i += 1
            base = BUILTINS[c]
        else:
            raise DemangleError(f"unknown type code {c!r} at {self.i}")
        if quals:
            base = apply_quals(quals, base)
        return base

    def param_list(self, nested=False):
        saved = self.params
        out = []
        self.params = out  # T<n>/N<c><n> index into the list being built
        while not self.done() and self.peek() != "_":
            if self.eat("T"):
                idx = self.repeat_index()
                out.append(self.repeat(idx))
            elif self.peek() == "N" and self.peek(2)[1:].isdigit():
                self.i += 1
                count = int(self.s[self.i])
                self.i += 1
                idx = self.repeat_index()
                out.extend([self.repeat(idx)] * count)
            else:
                t = self.type()
                out.append(t)
        if nested:
            self.params = saved
        if out == ["void"]:
            return ""
        return ", ".join(finish(t) for t in out)

    def repeat_index(self):
        if self.eat("_"):
            idx = self.number()
            self.expect("_")
            return idx
        idx = int(self.s[self.i])
        self.i += 1
        return idx

    def repeat(self, idx):
        if not 1 <= idx <= len(self.params):
            raise DemangleError(f"bad repeat index {idx}")
        return self.params[idx - 1]

    # --- template arguments ---
    def template_args(self, text):
        p = Parser(text)
        args = []
        while not p.done():
            if p.eat("X"):
                t = p.type()
                if not p.eat("L_"):
                    # X<name> without a literal: the address of a global object/function.
                    # (or, for X<Z..Z>, a non-type template parameter).
                    args.append(t if re.fullmatch(r"T\d+(_\d+)?", t) else "&" + finish(t))
                    continue
                n = p.number()
                p.expect("_")
                value = p.s[p.i:p.i + n]
                p.i += n
                args.append(render_literal(finish(t), value))
            else:
                args.append(finish(p.type()))
        return args


def wrap_declarator(inner, op, suffix=False):
    """Insert a declarator into a type that may contain a %s placeholder (function/array types)."""
    if "%s" in inner:
        return inner.replace("%s", f"%s{op}" if suffix else f"{op}%s", 1)
    return inner + op


def apply_quals(quals, base):
    if "%s" in base:
        return base.replace("%s", " ".join(quals) + " %s", 1)
    return " ".join(quals) + " " + base


def finish(t):
    """Drop the declarator placeholder: 'void (*%s)(int)' -> 'void (*)(int)'."""
    return t.replace("%s", "").replace("( ", "(").replace(" )", ")")


def render_literal(type_name, value):
    type_name = re.sub(r"^(const |volatile )+", "", type_name)
    if type_name == "bool":
        return {"0": "false", "1": "true"}.get(value, value)
    if type_name.startswith(("unsigned", "int", "long", "short", "char", "signed")):
        return value
    return f"({type_name}){value}"


TEMPLATE_RE = re.compile(r"__(tm|ps|pt)__(\d+)_")
TPARAM_RE = re.compile(r"Z(\d+)(?:_(\d+))?Z")


def render_identifier(ident):
    """Render an identifier that may carry __tm__/__ps__/__pt__ template parts or be an anonymous namespace."""
    m = re.fullmatch(r"__N_\d+_(.*)", ident)
    if m:
        return "(anonymous namespace)"
    m = re.fullmatch(r"([A-Za-z_]\w*?)__L\d+__(.+)", ident)
    if m:
        return f"{demangle_core(m.group(2))}::{m.group(1)}"
    m = TEMPLATE_RE.search(ident)
    if not m:
        return ident
    base = ident[:m.start()]
    rest = ident[m.start():]
    tm_args = None
    while rest:
        m = TEMPLATE_RE.match(rest)
        if not m:
            raise DemangleError(f"junk after template args: {rest!r}")
        n = int(m.group(2))
        # The length counts the leading "_" that follows the number.
        args_text = rest[m.end():m.end() + n - 1]
        if len(args_text) != n - 1:
            raise DemangleError("template args run past end")
        rest = rest[m.end() + n - 1:]
        if m.group(1) in ("tm", "pt"):
            tm_args = ", ".join(Parser("").template_args(args_text))
        else:
            Parser("").template_args(args_text)  # validate the partial-specialisation pattern
    if tm_args is None:
        return base
    return f"{base}<{tm_args}>"


def identifier_template_args(ident):
    """Template arguments (as a list) of an identifier with a __tm__/__pt__ block, else []."""
    if re.fullmatch(r"[A-Za-z_]\w*?__L\d+__.+", ident):
        return []
    m = TEMPLATE_RE.search(ident)
    while m:
        n = int(m.group(2))
        if m.group(1) in ("tm", "pt"):
            return Parser("").template_args(ident[m.end():m.end() + n - 1])
        m = TEMPLATE_RE.match(ident, m.end() + n - 1)
    return []


def demangle_function_name(fname):
    """Map the part before the first top-level '__' separator to a readable name."""
    if fname.startswith("__op") and len(fname) > 4 and fname[2:] not in OPERATORS:
        # Conversion operator. Parse the type first: it may itself contain __tm__ blocks.
        p = Parser(fname[4:])
        t = finish(p.type())
        rest = fname[4 + p.i:]
        return f"operator {t}" + (render_identifier("x" + rest)[1:] if rest else "")
    tm = ""
    m = TEMPLATE_RE.search(fname)
    if m and m.start() > 0:
        tm_text = fname[m.start():]
        fname = fname[:m.start()]
        tm = render_identifier("x" + tm_text)[1:]
    if fname == "__ct":
        return "<ctor>" + tm
    if fname == "__dt":
        return "<dtor>" + tm
    if fname.startswith("__") and fname[2:] in OPERATORS:
        return OPERATORS[fname[2:]] + tm
    return fname + tm


def split_candidates(s):
    """Positions of '__' that could separate the function name from its signature, left to right.
    A run of underscores yields one candidate per position so names like '__x' and 'x___' both work."""
    start = 2 if s.startswith("__") else 1
    pos = []
    i = s.find("__", start)
    while i != -1:
        pos.append(i)
        i = s.find("__", i + 1)
    return pos


def demangle_core(s):
    """Demangle a decompressed, de-static'd name. Raises DemangleError when it is not valid mangling."""
    errors = []
    for cut in split_candidates(s):
        fname = s[:cut]
        # Template arguments on a function name belong to the name, not the signature.
        if TEMPLATE_RE.match(s, cut):
            continue
        try:
            return demangle_signature(fname, s[cut + 2:])
        except DemangleError as e:
            errors.append(e)
    raise DemangleError(str(errors[-1]) if errors else "no '__' separator")


def demangle_signature(fname, sig):
    name = demangle_function_name(fname)
    # A conversion operator's template blocks belong to its target type, not to the function.
    conversion = fname.startswith("__op") and fname[2:] not in OPERATORS
    m = None if conversion else TEMPLATE_RE.search(fname, 1)
    own_args = identifier_template_args("x" + fname[m.start():]) if m else []
    static_tmpl = sig.startswith("S__")
    if static_tmpl:
        sig = sig[3:]
    p = Parser(sig)
    scope = ""
    if p.peek() and (p.peek().isdigit() or p.peek() == "Q"):
        scope = p.qualified_name()
    quals = []
    static = False
    while p.peek() in ("C", "V", "S") and p.peek(2)[1:] in ("F", "C", "V", "S"):
        c = p.peek()
        p.i += 1
        if c == "S":
            static = True
        else:
            quals.append("const" if c == "C" else "volatile")
    if p.done():
        if not scope or quals or static:
            raise DemangleError("no signature")
        return f"{scope}::{fixup_special(name, scope)}"
    p.expect("F")
    _bindings.append(own_args or (identifier_template_args(p.last_ident) if scope else []))
    try:
        args = p.param_list()
        ret = ""
        if p.eat("_"):
            ret = finish(p.type())
        if not p.done():
            raise DemangleError(f"junk at end: {p.s[p.i:]!r}")
    finally:
        _bindings.pop()
    full = f"{scope}::{fixup_special(name, scope)}" if scope else name
    out = f"{full}({args})"
    if quals:
        out += " " + " ".join(quals)
    if ret:
        out = f"{ret} {out}"
    if static or static_tmpl:
        out = "static " + out
    return out


def fixup_special(name, scope):
    last = re.sub(r"<.*$", "", scope.split("::")[-1]) if scope else ""
    if name.startswith("<ctor>"):
        return last + name[len("<ctor>"):]
    if name.startswith("<dtor>"):
        return "~" + last + name[len("<dtor>"):]
    return name


def demangle(name):
    """Return the demangled form of name, or None if it is not mangled.
    Raises DemangleError if it looks mangled but cannot be decoded."""
    suffix = ""
    m = re.search(r"_static_in_(.+)$", name)
    if m and name.startswith("__"):
        suffix = f" [static in {m.group(1)}]"
        name = name[2:m.start()]
    m = re.fullmatch(r"__ghs_thunk__(0x[0-9a-fA-F]+)__(.+)", name)
    if m:
        adj = int(m.group(1), 16)
        adj = adj - (1 << 32) if adj & 0x80000000 else adj
        inner = demangle(m.group(2))
        return f"[thunk this{adj:+#x}] {inner if inner is not None else m.group(2)}{suffix}"
    m = re.fullmatch(r"__(sti|std)___\d+_(.+)", name)
    if m:
        kind = "static initializer" if m.group(1) == "sti" else "static destructor"
        return f"[{kind} for {m.group(2)}]{suffix}"
    if name.startswith("__CPR"):
        name = decompress(name)
    if not looks_mangled(name):
        return name + suffix if suffix else None
    return demangle_core(name) + suffix


def looks_mangled(name):
    """Heuristic: a '__' separator followed by something that can start a signature or scope,
    or an operator/constructor-style name."""
    if OPERATOR_RE.match(name) or name.startswith(("__ct__", "__dt__", "__op")):
        return True
    return re.search(r"[^_]_*__(?:[0-9QFCS]|tm__|ps__|pt__)", name) is not None


def main(argv):
    status = 0
    for name in argv[1:]:
        try:
            out = demangle(name)
            print(f"{name} -> {out if out is not None else '(not mangled)'}")
        except DemangleError as e:
            print(f"{name} -> FAILED: {e}")
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
