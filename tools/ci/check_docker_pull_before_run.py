#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Every `docker run` of a registry image in a shell script must be preceded
by a docker_pull_retry of exactly the same image with the same --platform.

Usage: check_docker_pull_before_run.py FILE...   (prints one line per
violation, exit 1 if any)

Rules:
- A `docker run` / `docker container run` (any whitespace) at the start of a
  command is checked; its image is the first argument that is not an option or
  an option value.
- A docker_pull_retry of the same image (compared exactly, after dropping
  quotes and braces of plain variables) must come earlier in the file: within
  the 12 lines before the run, or at the top level of the script (no
  indentation).
- The pull's --platform must equal the run's.
- A pull ending in `|| true` is only allowed when the run is itself best
  effort (its statement also ends in `|| true`).
- A run is exempt when the line right above it is exactly a `# local-image`
  comment AND its image is a tag made by a `docker build -t` in the same file.
- A file that has a checked run must source docker_pull_retry.sh.
"""
import re
import shlex
import sys

VALUE_OPTS = {
    '--name', '-v', '--volume', '-e', '--env', '--env-file', '--platform', '--network', '--net',
    '--network-alias', '--user', '-u', '-w', '--workdir', '--cpus', '--memory', '-m',
    '--memory-swap', '--add-host', '--entrypoint', '--cpu-shares', '-p', '--publish', '--mount',
    '--label', '-l', '--tmpfs', '--pull', '--hostname', '-h', '--ulimit', '--shm-size', '--cap-add',
    '--cap-drop', '--security-opt', '--device', '--group-add', '--pid', '--ipc', '--restart',
    '--cidfile', '--log-driver', '--dns', '--sysctl', '--stop-timeout', '--cpuset-cpus',
}
RUN_RE = re.compile(r'(?:^|[;&|(]\s*|\bif\s+|\bthen\s+|\belif\s+|!\s*|\bwhile\s+)docker\s+(?:container\s+)?run\b')


def mask(text, table):
    """Replace complex ${...} and $(...) with placeholders; plain ${name} -> $name."""
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == '$' and i + 1 < len(text) and text[i + 1] in '{(':
            open_c = text[i + 1]
            close_c = '}' if open_c == '{' else ')'
            depth = 0
            j = i + 1
            while j < len(text):
                if text[j] == open_c:
                    depth += 1
                elif text[j] == close_c:
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            expr = text[i:j + 1]
            m = re.fullmatch(r'\$\{([A-Za-z_]\w*)\}', expr)
            if m:
                out.append('$' + m.group(1))
            else:
                table.append(expr)
                out.append('\x00%d\x00' % (len(table) - 1))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def restore(tok, table):
    return re.sub('\x00(\\d+)\x00', lambda m: table[int(m.group(1))], tok)


def statements(lines):
    """Yield (index, joined statement) for each logical command start."""
    i = 0
    while i < len(lines):
        stmt = lines[i]
        j = i
        while stmt.rstrip().endswith('\\') and j + 1 < len(lines):
            j += 1
            stmt = stmt.rstrip()[:-1] + ' ' + lines[j]
        yield i, j, stmt
        i = j + 1


def tokens(stmt, table):
    s = mask(stmt, table)
    # A multi-line quoted body (bash -c '...) leaves a quote open: cut it off.
    while True:
        try:
            return shlex.split(s, comments=False, posix=True)
        except ValueError:
            cut = max(s.rfind("'"), s.rfind('"'))
            if cut < 0:
                return s.split()
            s = s[:cut]


def after(toks, words):
    for k in range(len(toks) - len(words) + 1):
        if toks[k:k + len(words)] == words:
            return toks[k + len(words):]
    return None


def run_image(toks):
    rest = after(toks, ['docker', 'container', 'run']) or after(toks, ['docker', 'run'])
    if rest is None:
        return None, []
    k = 0
    plat = []
    while k < len(rest):
        t = rest[k]
        if t.startswith('\x00'):
            k += 1
        elif t in VALUE_OPTS:
            if t == '--platform' and k + 1 < len(rest):
                plat.append(rest[k + 1])
            k += 2
        elif t.startswith('--platform='):
            plat.append(t.split('=', 1)[1]); k += 1
        elif t.startswith('-'):
            k += 1
        else:
            return t, plat
    return None, plat


def platform_of_stmt(stmt):
    """Normalised platform arguments, including those inside expansions."""
    found = re.findall(r'--platform[ =]\s*("(?:[^"\\]|\\.)*"|\S+)', stmt)
    return sorted(re.sub(r'[\'"{}]', '', f) for f in found)


def norm(tok, table):
    return re.sub(r'[\'"]', '', restore(tok, table))


def check(path):
    lines = open(path).read().split('\n')
    bad = []
    builds = set()
    for _, _, stmt in statements(lines):
        if re.search(r'\bdocker\s+build\b', stmt) and not stmt.lstrip().startswith('#'):
            t = []
            toks = tokens(stmt, t)
            for k, w in enumerate(toks):
                if w in ('-t', '--tag') and k + 1 < len(toks):
                    builds.add(norm(toks[k + 1], t))
    pulls = []  # (line index, indented, image, platform, best_effort)
    checked = 0
    for i, j, stmt in statements(lines):
        s = stmt.strip()
        if s.startswith('#'):
            continue
        m = re.match(r'docker_pull_retry\b(.*)', s)
        if m:
            t = []
            toks = tokens(s, t)[1:]
            toks = [x for x in toks if x != '--quiet']
            if toks:
                pulls.append((i, lines[i][:1] in ' \t', norm(toks[0], t),
                              platform_of_stmt(s), bool(re.search(r'\|\|\s*(true|:)\s*$', s))))
            continue
        rm = RUN_RE.search(s)
        if not rm:
            continue
        t = []
        toks = tokens(s[s.index('docker', rm.start()):], t)
        image, _ = run_image(toks)
        if image is None:
            bad.append('%s:%d: cannot find the image of this docker run' % (path, i + 1))
            continue
        image = norm(image, t)
        if i > 0 and re.match(r'^\s*# local-image\b', lines[i - 1]):
            if image not in builds:
                bad.append('%s:%d: marked local-image, but %s is not built by a docker build -t in this file'
                           % (path, i + 1, image))
            continue
        checked += 1
        plat = platform_of_stmt(s)
        best_effort = bool(re.search(r'\|\|\s*(true|:)\b', s))
        cands = [p for p in pulls if p[0] < i and p[2] == image and (i - p[0] <= 12 or not p[1])]
        if not cands:
            bad.append('%s:%d: docker run of %s without a docker_pull_retry of exactly that image before it'
                       % (path, i + 1, image))
            continue
        p = cands[-1]
        if p[3] != plat:
            bad.append('%s:%d: --platform of the docker run %s differs from its pull %s' % (path, i + 1, plat, p[3]))
        if p[4] and not best_effort:
            bad.append('%s:%d: the pull of %s ends in || true but the run is not best effort' % (path, i + 1, image))
    if checked and 'docker_pull_retry.sh' not in '\n'.join(lines):
        bad.append('%s: has a docker run but does not source docker_pull_retry.sh' % path)
    return bad


if __name__ == '__main__':
    problems = []
    for f in sys.argv[1:]:
        problems += check(f)
    print('\n'.join(problems))
    sys.exit(1 if problems else 0)
