#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Job graph of a release pipeline, with each `if:` evaluated per event.

The module release lane is changed without being able to dispatch a branch
(the caller pins the lane to master), so a change to the conditions of its
jobs is checked by rendering the graph before and after. For one pipeline
file this prints, for each job id, its `needs` and its `if:` with the known
constants of an event substituted and the boolean expression simplified.
Runtime values (needs.X.result, vars.*, github.actor, ...) stay symbolic.

Events:
  dispatch-dry-run  a plain workflow_dispatch with dry_run=true
  tag-push          a push of a module-v* tag

    release_job_graph.py render FILE [--event EVENT]    (default: both)
    release_job_graph.py compare OLD NEW                 (exit 1 on any difference)
    release_job_graph.py --self-test

The reader is line-oriented (jobs at two spaces, their keys at four, `if:` on
one line) and needs only the standard library, like the other CI tools.
"""

import re
import sys

EVENTS = {
    # Constants an event gives the contexts a job condition reads. Contexts
    # not listed are runtime values and stay symbolic. A value is a string
    # (job outputs and event names are strings) or a bool (workflow inputs).
    "dispatch-dry-run": {
        "github.event_name": "workflow_dispatch",
        "inputs.dry_run": True,
        "inputs.candidate": False,
        "needs.version.outputs.dry_run": "true",
        "needs.version.outputs.candidate": "false",
        "needs.version.outputs.build_profile": "rehearsal",
    },
    "tag-push": {
        "github.event_name": "push",
        "inputs.dry_run": False,
        "inputs.candidate": False,
        "needs.version.outputs.dry_run": "false",
        "needs.version.outputs.candidate": "false",
        "needs.version.outputs.build_profile": "release",
    },
}

# --------------------------------------------------------------------------
# Expressions: ('const', v) ('ctx', dotted) ('call', name, args) ('not', x)
# ('and', [x...]) ('or', [x...]) ('cmp', op, a, b)

TOKEN_RE = re.compile(r"""\s*(?:
    (?P<str>'(?:[^']|'')*')
  | (?P<num>-?\d+(?:\.\d+)?)
  | (?P<op>&&|\|\||==|!=|<=|>=|<|>|!|\(|\)|,)
  | (?P<id>[A-Za-z_][A-Za-z0-9_\-\.\*]*(?:\[[^\]]*\][A-Za-z0-9_\-\.]*)*)
)""", re.X)


def tokenize(text):
    pos, out = 0, []
    text = text.strip()
    while pos < len(text):
        m = TOKEN_RE.match(text, pos)
        if not m or m.end() == pos:
            raise ValueError("cannot read expression at %r" % text[pos:pos + 20])
        pos = m.end()
        for kind in ("str", "num", "op", "id"):
            if m.group(kind) is not None:
                out.append((kind, m.group(kind)))
                break
    return out


class Parser:
    def __init__(self, tokens):
        self.t, self.i = tokens, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else (None, None)

    def take(self, value=None):
        tok = self.peek()
        if value is not None and tok[1] != value:
            raise ValueError("expected %r, found %r" % (value, tok[1]))
        self.i += 1
        return tok

    def parse(self):
        node = self.or_()
        if self.i != len(self.t):
            raise ValueError("trailing tokens %r" % (self.t[self.i:],))
        return node

    def or_(self):
        items = [self.and_()]
        while self.peek()[1] == "||":
            self.take()
            items.append(self.and_())
        return items[0] if len(items) == 1 else ("or", items)

    def and_(self):
        items = [self.cmp_()]
        while self.peek()[1] == "&&":
            self.take()
            items.append(self.cmp_())
        return items[0] if len(items) == 1 else ("and", items)

    def cmp_(self):
        left = self.unary()
        while self.peek()[1] in ("==", "!=", "<", ">", "<=", ">="):
            op = self.take()[1]
            left = ("cmp", op, left, self.unary())
        return left

    def unary(self):
        if self.peek()[1] == "!":
            self.take()
            return ("not", self.unary())
        return self.atom()

    def atom(self):
        kind, val = self.take()
        if kind == "str":
            return ("const", val[1:-1].replace("''", "'"))
        if kind == "num":
            return ("const", val)
        if val == "(":
            node = self.or_()
            self.take(")")
            return node
        if kind == "id":
            if val in ("true", "false"):
                return ("const", val == "true")
            if val == "null":
                return ("const", "")
            if self.peek()[1] == "(":
                self.take()
                args = []
                if self.peek()[1] != ")":
                    args.append(self.or_())
                    while self.peek()[1] == ",":
                        self.take()
                        args.append(self.or_())
                self.take(")")
                return ("call", val, tuple(args))
            return ("ctx", val)
        raise ValueError("unexpected token %r" % (val,))


def truthy(v):
    # GitHub expressions: an empty string is falsy, any other string is truthy.
    return bool(v) if not isinstance(v, str) else len(v) > 0


def norm(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v).lower()


def key(node):
    return repr(node)


def simplify(node, consts):
    k = node[0]
    if k == "const":
        return node
    if k == "ctx":
        return ("const", consts[node[1]]) if node[1] in consts else node
    if k == "call":
        return ("call", node[1], tuple(simplify(a, consts) for a in node[2]))
    if k == "not":
        x = simplify(node[1], consts)
        if x[0] == "const":
            return ("const", not truthy(x[1]))
        if x[0] == "not":
            return x[1]
        return ("not", x)
    if k == "cmp":
        a, b = simplify(node[2], consts), simplify(node[3], consts)
        if a[0] == "const" and b[0] == "const" and node[1] in ("==", "!="):
            eq = norm(a[1]) == norm(b[1])
            return ("const", eq if node[1] == "==" else not eq)
        return ("cmp", node[1], a, b)
    if k in ("and", "or"):
        absorbing = (k == "or")  # a true operand ends an or, a false one ends an and
        items, seen = [], set()
        for sub in node[1]:
            x = simplify(sub, consts)
            if x[0] == k:
                subs = x[1]
            else:
                subs = [x]
            for y in subs:
                if y[0] == "const":
                    if truthy(y[1]) == absorbing:
                        return ("const", absorbing)
                    continue
                if key(y) in seen:
                    continue
                seen.add(key(y))
                items.append(y)
        if not items:
            return ("const", not absorbing)
        return items[0] if len(items) == 1 else (k, items)
    raise ValueError("unknown node %r" % (k,))


def render(node, parent=None):
    k = node[0]
    if k == "const":
        v = node[1]
        if isinstance(v, bool):
            return "true" if v else "false"
        return "'%s'" % str(v).replace("'", "''")
    if k == "ctx":
        return node[1]
    if k == "call":
        return "%s(%s)" % (node[1], ", ".join(render(a) for a in node[2]))
    if k == "not":
        return "!" + render(node[1], "not")
    if k == "cmp":
        s = "%s %s %s" % (render(node[2], "cmp"), node[1], render(node[3], "cmp"))
        return "(%s)" % s if parent in ("not", "cmp") else s
    op = " && " if k == "and" else " || "
    s = op.join(render(x, k) for x in node[1])
    needs_paren = (k == "or" and parent in ("and", "not", "cmp")) or (k == "and" and parent in ("not", "cmp"))
    return "(%s)" % s if needs_paren else s


def evaluate(expr, consts):
    """The simplified, rendered text of a job condition under the constants."""
    inner = expr.strip()
    m = re.fullmatch(r"\$\{\{(.*)\}\}", inner, re.S)
    if m:
        inner = m.group(1)
    return render(simplify(Parser(tokenize(inner)).parse(), consts))


# --------------------------------------------------------------------------
# The file


def read_jobs(path):
    """{job id: {'needs': [...], 'if': text or None}} from a pipeline file."""
    jobs, cur, injobs, in_needs = {}, None, False, False
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if re.match(r"^\s*#", line) or not line.strip():
                continue
            if re.match(r"^jobs:\s*$", line):
                injobs = True
                continue
            if injobs and re.match(r"^[^\s#]", line):
                injobs = False
            if not injobs:
                continue
            m = re.match(r"^  ([A-Za-z0-9_\-]+):\s*$", line)
            if m:
                cur = jobs.setdefault(m.group(1), {"needs": [], "if": None})
                in_needs = False
                continue
            if cur is None:
                continue
            m = re.match(r"^    needs:\s*(.*?)\s*$", line)
            if m:
                rest = m.group(1)
                in_needs = rest == ""
                if rest.startswith("["):
                    cur["needs"] = [x.strip() for x in rest.strip("[]").split(",") if x.strip()]
                elif rest:
                    cur["needs"] = [rest]
                continue
            if in_needs:
                m = re.match(r"^      - (\S+)\s*$", line)
                if m:
                    cur["needs"].append(m.group(1))
                    continue
                in_needs = False
            m = re.match(r"^    if:\s*(.*?)\s*$", line)
            if m:
                cur["if"] = m.group(1)
    return jobs


def render_graph(path, event):
    consts = EVENTS[event]
    lines = ["event: %s" % event]
    jobs = read_jobs(path)
    for jid in sorted(jobs):
        j = jobs[jid]
        cond = "(none)" if j["if"] is None else evaluate(j["if"], consts)
        lines.append("job %s" % jid)
        lines.append("  needs: %s" % (", ".join(sorted(j["needs"])) or "(none)"))
        lines.append("  if: %s" % cond)
    return "\n".join(lines) + "\n"


def compare(old, new):
    diffs = []
    for event in sorted(EVENTS):
        a = render_graph(old, event).splitlines()
        b = render_graph(new, event).splitlines()
        if a != b:
            diffs.append("event %s differs:" % event)
            sa, sb = set(a), set(b)
            diffs += ["  - " + x for x in a if x not in sb]
            diffs += ["  + " + x for x in b if x not in sa]
    return diffs


# --------------------------------------------------------------------------


def self_test():
    fails = 0

    def eq(desc, got, want):
        nonlocal fails
        ok = got == want
        print("%s %s%s" % ("ok  " if ok else "FAIL", desc, "" if ok else " (got %r, want %r)" % (got, want)))
        if not ok:
            fails += 1

    d, t = EVENTS["dispatch-dry-run"], EVENTS["tag-push"]
    real = "needs.version.outputs.dry_run == 'false'"
    cand = "needs.version.outputs.candidate != 'true'"
    eq("constant folds true", evaluate("${{ %s }}" % cand, d), "true")
    eq("dry_run condition: dispatch", evaluate("${{ %s }}" % real, d), "false")
    eq("dry_run condition: tag push", evaluate("${{ %s }}" % real, t), "true")
    eq("symbolic part is kept, true part dropped",
       evaluate("${{ !cancelled() && needs.a.result == 'success' && %s && %s }}" % (real, cand), t),
       "!cancelled() && needs.a.result == 'success'")
    eq("a false conjunct makes the whole condition false",
       evaluate("${{ !cancelled() && needs.a.result == 'success' && %s }}" % real, d), "false")
    eq("added candidate term does not change the result",
       evaluate("${{ !cancelled() && needs.a.result == 'success' && %s && %s }}" % (real, cand), d),
       evaluate("${{ !cancelled() && needs.a.result == 'success' && %s }}" % real, d))
    eq("or with a true operand", evaluate("${{ vars.X == 'true' || needs.version.outputs.dry_run == 'true' }}", d), "true")
    eq("or keeps symbolic operands",
       evaluate("${{ vars.X == 'true' || needs.version.outputs.dry_run == 'true' }}", t), "vars.X == 'true'")
    eq("event name folds",
       evaluate("${{ github.event_name == 'pull_request' && github.actor == 'x' }}", d), "false")
    eq("negation of a symbolic atom", evaluate("${{ !cancelled() }}", d), "!cancelled()")
    eq("double negation", evaluate("${{ !!cancelled() }}", d), "cancelled()")
    eq("parentheses kept where needed",
       evaluate("${{ (vars.A == 'x' || vars.B == 'y') && !cancelled() }}", d),
       "(vars.A == 'x' || vars.B == 'y') && !cancelled()")
    eq("duplicate operands collapse", evaluate("${{ !cancelled() && !cancelled() }}", d), "!cancelled()")

    import os
    import tempfile
    body = """name: x
jobs:
  version:
    runs-on: ubuntu-latest
  build:
    needs: [version]
    if: ${{ !cancelled() && needs.version.result == 'success' }}
  sign:
    needs:
      - version
      - build
    if: ${{ needs.build.result == 'success' && needs.version.outputs.dry_run == 'false' }}
    environment: release
"""
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "p.yml")
        with open(p, "w") as fh:
            fh.write(body)
        jobs = read_jobs(p)
        eq("job ids", sorted(jobs), ["build", "sign", "version"])
        eq("flow needs", jobs["build"]["needs"], ["version"])
        eq("block needs", jobs["sign"]["needs"], ["version", "build"])
        eq("no if", jobs["version"]["if"], None)
        out = render_graph(p, "dispatch-dry-run")
        eq("render is deterministic", out, render_graph(p, "dispatch-dry-run"))
        eq("render shows the dispatch result for sign", "job sign\n  needs: build, version\n  if: false\n" in out, True)
        eq("render shows the tag push result for sign",
           "job sign\n  needs: build, version\n  if: needs.build.result == 'success'\n" in render_graph(p, "tag-push"), True)
        q = os.path.join(td, "q.yml")
        with open(q, "w") as fh:
            fh.write(body.replace("dry_run == 'false' }}", "dry_run == 'false' && needs.version.outputs.candidate != 'true' }}"))
        eq("compare: an added always-true term is no difference", compare(p, q), [])
        r = os.path.join(td, "r.yml")
        with open(r, "w") as fh:
            fh.write(body.replace("needs.build.result == 'success' && ", ""))
        eq("compare: a changed condition is reported", bool(compare(p, r)), True)

    if fails:
        print("release_job_graph self-test: %d failure(s)" % fails)
        return 1
    print("release_job_graph self-test: all passed")
    return 0


def main(argv):
    if argv[:1] == ["--self-test"]:
        return self_test()
    if argv[:1] == ["render"] and len(argv) in (2, 4):
        path = argv[1]
        events = sorted(EVENTS)
        if len(argv) == 4:
            if argv[2] != "--event" or argv[3] not in EVENTS:
                print("usage: release_job_graph.py render FILE [--event %s]" % "|".join(sorted(EVENTS)), file=sys.stderr)
                return 2
            events = [argv[3]]
        sys.stdout.write("\n".join(render_graph(path, e) for e in events))
        return 0
    if argv[:1] == ["compare"] and len(argv) == 3:
        diffs = compare(argv[1], argv[2])
        print("\n".join(diffs) if diffs else "job graphs are identical for every event")
        return 1 if diffs else 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
