// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// reference-parsers.mjs — source parsers shared by the reference generator
// (scripts/generate-references.mjs) and the drift gates in test/reference/.
//
// Everything here reads program SOURCE, never a built binary: the optimizer
// worker's usage text (src/worker/main.cc), the thin nginx module's command
// table (src/nginx/ngx_pagespeed_module.cc), and the native module's option
// registration tables and filter tables in the public mod_pagespeed tree.
// The parsers are deliberately small and regular: a parse that cannot
// derive a value returns null for it; it never guesses.

// ---------------------------------------------------------------------------
// C/C++ helpers
// ---------------------------------------------------------------------------

/** Drop // and /* *\/ comments outside string literals. */
export function stripComments(src) {
  let out = '';
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === '"') {
      let j = i + 1;
      while (j < n && src[j] !== '"') {
        if (src[j] === '\\') j++;
        j++;
      }
      out += src.slice(i, j + 1);
      i = j + 1;
    } else if (c === '/' && src[i + 1] === '/') {
      while (i < n && src[i] !== '\n') i++;
    } else if (c === '/' && src[i + 1] === '*') {
      const end = src.indexOf('*/', i + 2);
      i = end === -1 ? n : end + 2;
      out += ' ';
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

/** Decode a C string literal body ("..." without the quotes). */
export function unescapeC(body) {
  return body.replace(/\\(n|t|"|\\|')/g, (_, ch) => (ch === 'n' ? '\n' : ch === 't' ? '\t' : ch));
}

/**
 * If `expr` is one or more adjacent C string literals, return the concatenated
 * decoded string; otherwise null.
 */
export function literalString(expr) {
  const trimmed = expr.trim();
  if (!trimmed.startsWith('"')) return null;
  const re = /"((?:[^"\\]|\\.)*)"/g;
  let out = '';
  let last = 0;
  let m;
  while ((m = re.exec(trimmed)) !== null) {
    if (trimmed.slice(last, m.index).trim() !== '') return null;
    out += unescapeC(m[1]);
    last = m.index + m[0].length;
  }
  if (trimmed.slice(last).trim() !== '') return null;
  return out;
}

/** Split a call's argument text on top-level commas (parens and strings aware). */
export function splitArgs(text) {
  const args = [];
  let depth = 0;
  let cur = '';
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    if (c === '"') {
      let j = i + 1;
      while (j < text.length && text[j] !== '"') {
        if (text[j] === '\\') j++;
        j++;
      }
      cur += text.slice(i, j + 1);
      i = j + 1;
      continue;
    }
    if (c === '(' || c === '{' || c === '<') depth++;
    if (c === ')' || c === '}' || c === '>') depth--;
    if (c === ',' && depth === 0) {
      args.push(cur.trim());
      cur = '';
    } else {
      cur += c;
    }
    i++;
  }
  if (cur.trim() !== '') args.push(cur.trim());
  return args;
}

/**
 * Find every call `fnName(` in `src` and return its argument list (top-level
 * split). Comments are stripped first so a `// half a second` inside an
 * argument list cannot break the split.
 */
export function findCalls(src, fnName) {
  const clean = stripComments(src);
  const calls = [];
  const re = new RegExp(`(?<![A-Za-z0-9_])${fnName}\\s*\\(`, 'g');
  let m;
  while ((m = re.exec(clean)) !== null) {
    let i = m.index + m[0].length;
    let depth = 1;
    let inStr = false;
    const start = i;
    while (i < clean.length && depth > 0) {
      const c = clean[i];
      if (inStr) {
        if (c === '\\') i++;
        else if (c === '"') inStr = false;
      } else if (c === '"') inStr = true;
      else if (c === '(') depth++;
      else if (c === ')') depth--;
      i++;
    }
    calls.push({ args: splitArgs(clean.slice(start, i - 1)), index: m.index });
  }
  return calls;
}

// ---------------------------------------------------------------------------
// Optimizer worker: usage text in src/worker/main.cc
// ---------------------------------------------------------------------------

/** Collect the string literals of a `<<`-chained stream statement body. */
function streamLiterals(body) {
  const re = /"((?:[^"\\]|\\.)*)"|\b(kServiceUsage)\b|\bprogram\b/g;
  const parts = [];
  let m;
  while ((m = re.exec(body)) !== null) {
    if (m[1] !== undefined) parts.push({ text: unescapeC(m[1]) });
    else if (m[2]) parts.push({ marker: 'service' });
    else parts.push({ text: 'factory_worker' });
  }
  return parts;
}

const ALL_CAPS_ARG = /^\[?[A-Z][A-Z0-9_=/\-[\]]*\]?$/;

function parseUsageLines(lines, platform, flags, startSection) {
  let section = startSection;
  let current = null;
  let sawBlank = false;
  for (const raw of lines) {
    const line = raw.replace(/\s+$/, '');
    if (line === '') {
      sawBlank = true;
      continue;
    }
    const header = /^(\S.*):$/.exec(line);
    if (header) {
      section = header[1];
      current = null;
      sawBlank = false;
      continue;
    }
    const flag = /^ {2}(-{1,2}[^\s]+)(.*)$/.exec(line);
    if (flag) {
      // A flag that follows a blank line but no new header belongs to the
      // first section again (the usage text closes with --version/--help
      // after the last named group).
      if (sawBlank && section !== startSection && !header) {
        // Only reset when the previous section was a named group that has
        // ended; a blank line inside a group never occurs in the source.
        section = startSection;
      }
      sawBlank = false;
      let rest = flag[2];
      let arg = null;
      const argMatch = /^\s+(\S+)(.*)$/.exec(rest);
      if (argMatch && ALL_CAPS_ARG.test(argMatch[1]) && /[A-Z]/.test(argMatch[1])) {
        arg = argMatch[1];
        rest = argMatch[2];
      }
      current = {
        name: flag[1],
        arg,
        description: rest.trim(),
        section,
        platform,
      };
      flags.push(current);
      continue;
    }
    if (/^ {4,}\S/.test(line) && current) {
      current.description = `${current.description} ${line.trim()}`.trim();
      continue;
    }
    // "Usage: factory_worker [options]" and anything else is not a flag.
    current = null;
  }
}

/** Extract "(default: X)" from a usage description, or null. */
export function usageDefault(description) {
  const m = /\(default(?: path)?:\s*([^)]*)\)/.exec(description);
  if (!m) return null;
  let value = m[1].split(/[;,]/)[0].trim();
  if (value === '') return null;
  const numEq = /^(-?[0-9.]+)=(.+)$/.exec(value);
  if (numEq) value = `${numEq[1]} (${numEq[2]})`;
  return value;
}

/**
 * Parse the worker's PrintUsage() text into flags.
 * @param {string} mainCc contents of src/worker/main.cc
 * @returns {{ name: string, arg: string|null, default: string|null,
 *            description: string, section: string, platform: string|null }[]}
 */
export function parseWorkerUsage(mainCc) {
  const fnStart = mainCc.indexOf('void PrintUsage(');
  if (fnStart === -1) throw new Error('PrintUsage() not found in main.cc');
  const fnEnd = mainCc.indexOf('\n}', fnStart);
  const body = mainCc.slice(fnStart, fnEnd);

  // Windows-only options live in a separate literal under #ifdef _WIN32.
  const win = /#ifdef _WIN32\s*constexpr const char\* kServiceUsage =([\s\S]*?);/.exec(mainCc);
  const winText = win
    ? streamLiterals(win[1])
        .map((p) => p.text ?? '')
        .join('')
    : '';

  const flags = [];
  const parts = streamLiterals(body);
  let text = '';
  for (const p of parts) {
    if (p.marker === 'service') {
      // Flush what we have, parse the Windows block on its own, continue.
      parseUsageLines(text.split('\n'), null, flags, 'Options');
      text = '';
      parseUsageLines(winText.split('\n'), 'windows', flags, 'Windows service');
    } else {
      text += p.text;
    }
  }
  // The remainder continues the section that was open before the Windows
  // block; re-derive it by replaying headers seen so far.
  const seen = flags.filter((f) => f.platform === null).map((f) => f.section);
  const resume = seen.length ? seen[seen.length - 1] : 'Options';
  parseUsageLines(text.split('\n'), null, flags, resume);

  return flags.map((f) => ({
    name: f.name,
    arg: f.arg,
    default: usageDefault(f.description),
    description: f.description.replace(/\s+/g, ' ').trim(),
    section: f.section,
    platform: f.platform,
  }));
}

// ---------------------------------------------------------------------------
// Thin nginx module: command table in src/nginx/ngx_pagespeed_module.cc
// ---------------------------------------------------------------------------

const NGX_CONTEXT = [
  ['NGX_HTTP_MAIN_CONF', 'http'],
  ['NGX_HTTP_SRV_CONF', 'server'],
  ['NGX_HTTP_LOC_CONF', 'location'],
  ['NGX_HTTP_LIF_CONF', 'if in location'],
];

/**
 * Parse the thin module's ngx_command_t table and the defaults its
 * merge_loc_conf() applies.
 * @param {string} moduleCc contents of src/nginx/ngx_pagespeed_module.cc
 */
export function parseThinModuleDirectives(moduleCc) {
  const clean = stripComments(moduleCc);
  const tableStart = clean.indexOf('ngx_http_pagespeed_commands[] = {');
  if (tableStart === -1) throw new Error('ngx_http_pagespeed_commands[] not found');
  const tableEnd = clean.indexOf('ngx_null_command', tableStart);
  const table = clean.slice(tableStart, tableEnd);

  // Enum tables: static ngx_conf_enum_t NAME[] = { {ngx_string("a"), 0}, ... }
  const enums = new Map();
  const enumRe = /static ngx_conf_enum_t (\w+)\[\] = \{([\s\S]*?)\};/g;
  let em;
  while ((em = enumRe.exec(clean)) !== null) {
    const values = [...em[2].matchAll(/ngx_string\("([^"]+)"\),\s*(\d+)/g)].map((v) => ({
      name: v[1],
      value: Number(v[2]),
    }));
    enums.set(em[1], values);
  }

  // Defaults: ngx_conf_merge_*value(conf->field, prev->field, DEFAULT)
  const defaults = new Map();
  const mergeRe =
    /ngx_conf_merge_(str_|uint_)?value\(conf->(\w+),\s*prev->\2,\s*((?:[^()]|\([^()]*\))*)\)/g;
  let mm;
  while ((mm = mergeRe.exec(clean)) !== null) {
    defaults.set(mm[2], { kind: mm[1] ?? '', expr: mm[3].trim() });
  }
  const constInt = (name) => {
    const m = new RegExp(`static constexpr int ${name} = (\\d+);`).exec(clean);
    return m ? m[1] : null;
  };
  const modeDefault = (varName) => {
    const m = new RegExp(
      `ngx_int_t ${varName} = \\(resolved_mode == 1\\) \\? (\\d+) : (\\d+);`,
    ).exec(clean);
    return m ? { aggressive: m[1], safe: m[2] } : null;
  };

  const entryRe = /\{ngx_string\("([^"]+)"\),([\s\S]*?)\},\s*(?=\{ngx_string|$)/g;
  const out = [];
  let m;
  while ((m = entryRe.exec(table)) !== null) {
    const name = m[1];
    const rest = m[2];
    const context = NGX_CONTEXT.filter(([flag]) => rest.includes(flag)).map(([, ctx]) => ctx);
    const field = /offsetof\(ngx_http_pagespeed_loc_conf_t,\s*(\w+)\)/.exec(rest)?.[1] ?? null;
    let type = 'value';
    let values = null;
    if (rest.includes('NGX_CONF_FLAG')) type = 'flag';
    else if (rest.includes('ngx_conf_set_num_slot')) type = 'number';
    else if (rest.includes('ngx_conf_set_str_slot')) type = 'string';
    else if (rest.includes('ngx_conf_set_enum_slot')) {
      type = 'enum';
      const enumName = /ngx_conf_set_enum_slot[\s\S]*?,\s*(\w+)\s*$/.exec(rest.trim())?.[1];
      values = enums.get(enumName)?.map((v) => v.name) ?? null;
    } else if (name === 'pagespeed_disallow') type = 'pattern';

    let def = null;
    let defaultNote = null;
    const d = field ? defaults.get(field) : null;
    if (d) {
      const expr = d.expr;
      if (type === 'flag') def = expr === '1' ? 'on' : expr === '0' ? 'off' : null;
      else if (/^\d+$/.test(expr)) def = expr;
      else if (/^".*"$/.test(expr)) def = expr.slice(1, -1);
      else if (/^k[A-Z]\w*$/.test(expr)) def = constInt(expr);
      else if (/_default$/.test(expr)) {
        const md = modeDefault(expr);
        if (md) {
          def = `${md.safe} (safe) / ${md.aggressive} (aggressive)`;
          defaultNote = 'depends on the resolved cache mode';
        }
      } else if (expr === 'NGX_CONF_UNSET_UINT' && field === 'cache_mode') {
        // merge_loc_conf resolves an unset mode as: explicit directive >
        // the worker's shared configuration > safe.
        def = 'safe';
        defaultNote = 'unless the worker shared configuration sets aggressive';
      }
    }
    out.push({ name, context, type, values, field, default: def, defaultNote });
  }
  return out;
}

// ---------------------------------------------------------------------------
// Native module (mod_pagespeed): option registrations and filter tables
// ---------------------------------------------------------------------------

/**
 * Collect `k<Name>` constant definitions from C++ sources:
 *   const char RewriteOptions::kX[] = "...";   const char kX[] = "...";
 *   inline constexpr char kX[] = "...";         const int64 RewriteOptions::kX = expr;
 *   static const int64 kX = expr;               const double RewriteOptions::kX[] = {a, b};
 * Returns Map<name, { kind: 'string'|'expr'|'array', value }>.
 */
export function parseConstants(sources) {
  const map = new Map();
  for (const src of sources) {
    const clean = stripComments(src);
    const re =
      /(?:static\s+)?(?:inline\s+)?(?:constexpr|const)\s+(?:char|int64|int|double|float|int32|size_t|unsigned|long|bool)\s+(?:\w+::)?(k[A-Z]\w*)(\[\])?\s*=\s*([^;]*);/g;
    let m;
    while ((m = re.exec(clean)) !== null) {
      const [, name, isArray, rhs] = m;
      if (map.has(name)) continue;
      const str = literalString(rhs);
      if (str !== null) map.set(name, { kind: 'string', value: str });
      else if (isArray && rhs.trim().startsWith('{'))
        map.set(name, {
          kind: 'array',
          value: splitArgs(rhs.trim().replace(/^\{|\}$/g, '')).map((s) => s.trim()),
        });
      else map.set(name, { kind: 'expr', value: rhs.trim() });
    }
  }
  return map;
}

/** Evaluate an integer/float arithmetic expression over resolved constants. */
export function evalExpr(expr, lookup, depth = 0) {
  if (depth > 8) return null;
  let e = expr.trim();
  e = e.replace(/static_cast<[^>]+>\(([^)]*)\)/g, '$1');
  // Brace-initialized integer casts: int64{5}, uint32_t{1}, size_t{8}.
  e = e.replace(/\b(?:u?int(?:8|16|32|64)?(?:_t)?|size_t)\s*\{([^{}]*)\}/g, '($1)');
  e = e.replace(/\b(\d+)(?:LL|L|UL|U|ULL)\b/g, '$1');
  e = e.replace(/\b(?:\w+::)*(k[A-Z]\w*)\b/g, (_, k) => {
    const v = lookup(k, depth + 1);
    return v === null || v === undefined ? 'NaN' : `(${v})`;
  });
  if (e.includes('<<')) return evalShiftExpr(e);
  if (!/^[\d\s+\-*/().eE]+$/.test(e) || e.includes('NaN')) return null;
  try {
    // The expression is now digits and arithmetic only (validated above).
    const value = Function(`"use strict"; return (${e});`)();
    if (typeof value !== 'number' || !Number.isFinite(value)) return null;
    return value;
  } catch {
    return null;
  }
}

/**
 * Evaluate an integer expression that uses `<<` (2 << 20). JavaScript's `<<`
 * works on 32-bit integers, so the expression is evaluated in BigInt, where
 * `<<`, `+`, `-` and `*` have C's integer meaning and precedence; division
 * and non-integer literals are refused rather than approximated.
 */
function evalShiftExpr(e) {
  if (!/^[\d\s+\-*()<]+$/.test(e) || /</.test(e.replace(/<</g, ''))) return null;
  try {
    const big = e.replace(/\d+/g, '$&n');
    const value = Function(`"use strict"; return (${big});`)();
    if (typeof value !== 'bigint') return null;
    if (value > BigInt(Number.MAX_SAFE_INTEGER) || value < -BigInt(Number.MAX_SAFE_INTEGER))
      return null;
    return Number(value);
  } catch {
    return null;
  }
}

/**
 * Resolve a default-value token from an option registration to a display
 * string. Returns { value: string|null, type: 'bool'|'int'|'float'|'string'|'enum'|'empty'|null }.
 */
export function resolveDefault(token, constants, enumMap) {
  const t = token.trim();
  if (t === 'true') return { value: 'on', type: 'bool' };
  if (t === 'false') return { value: 'off', type: 'bool' };
  const str = literalString(t);
  if (str !== null) return { value: str, type: 'string' };
  if (/^-?\d+(?:LL|L|UL|U)?$/.test(t))
    return { value: String(Number(t.replace(/[LU]+$/, ''))), type: 'int' };
  if (/^-?\d*\.\d+f?$/.test(t)) return { value: t.replace(/f$/, ''), type: 'float' };
  if (/^\w+\(\)$/.test(t)) return { value: '', type: 'empty' };
  const bare = t.replace(/^(?:\w+::)+/, '');
  if (enumMap.has(bare)) return { value: enumMap.get(bare), type: 'enum' };
  const lookup = (k, depth) => {
    const c = constants.get(k);
    if (!c) return null;
    if (c.kind === 'expr') return evalExpr(c.value, lookup, depth);
    return null;
  };
  const c = constants.get(bare);
  if (c) {
    if (c.kind === 'string') return { value: c.value, type: 'string' };
    if (c.kind === 'array') {
      const vals = c.value.map((v) => evalExpr(v, lookup));
      if (vals.every((v) => v !== null)) return { value: vals.join(','), type: 'string' };
      return { value: null, type: null };
    }
    const n = evalExpr(c.value, lookup);
    if (n !== null) return { value: String(n), type: Number.isInteger(n) ? 'int' : 'float' };
  }
  const n = evalExpr(t, lookup);
  if (n !== null) return { value: String(n), type: Number.isInteger(n) ? 'int' : 'float' };
  return { value: null, type: null };
}

/**
 * Parse X(default, member, id, nameTok, scopeTok, help, safe) entries of
 * rewrite_options_properties.inc.
 */
export function parsePropertiesInc(incSrc) {
  // Join macro continuation lines, then strip comments.
  const joined = stripComments(incSrc.replace(/\\\n/g, '\n'));
  return findCalls(joined, 'X')
    .filter((c) => c.args.length === 7)
    .map((c) => entryFromArgs(c.args, 'rewrite_options'));
}

function entryFromArgs(args, origin, scopeDefault = null) {
  // Shapes:
  //  7 args: default, member, id, name, scope, help, safe
  //  6 args: default, member, id, name, help, safe        (scope implied)
  let [def, , id, name, scope, help, safe] = args;
  if (args.length === 6) {
    [def, , id, name, help, safe] = args;
    scope = scopeDefault;
  }
  const helpStr = literalString(help);
  return {
    origin,
    defaultTok: def,
    idTok: id,
    nameTok: name,
    scopeTok: scope,
    help: helpStr === null && /^(nullptr|NULL)$/.test(help.trim()) ? null : helpStr,
    safeToPrint: safe.trim() === 'true',
  };
}

/** Parse explicit AddBaseProperty/AddSystemProperty/AddApacheProperty/add_ngx_option calls. */
export function parsePropertyCalls(src, fnName, origin, scopeDefault = null) {
  return findCalls(src, fnName)
    .filter((c) => c.args.length === 7 || c.args.length === 6)
    .map((c) => entryFromArgs(c.args, origin, scopeDefault));
}

/** AddDeprecatedProperty("Name", kScope) calls. */
export function parseDeprecatedProperties(src, origin) {
  return findCalls(src, 'AddDeprecatedProperty')
    .filter((c) => c.args.length === 2)
    .map((c) => ({ origin, name: literalString(c.args[0]), scopeTok: c.args[1] }));
}

// Kinds of the PAGESPEED_APACHE_DIRECTIVE_TABLE(X) macro (apache_directive_table.h,
// mod_pagespeed 2.10.0 and later), as the APACHE_* macro each replaced in
// mod_instaweb.cc. The fetch kinds (APACHE_CONFIG_FETCH_OPTION2/23) were never
// part of the mod_instaweb.cc parse below, so they are left out here too.
// The fetch kinds, deliberately not parsed (see above).
const DIRECTIVE_TABLE_SKIPPED_KINDS = new Set(['kFetch2', 'kFetch23']);
const DIRECTIVE_TABLE_KINDS = {
  kScope: { directoryOk: false, arity: 1 },
  kDir1: { directoryOk: true, arity: 1 },
  kServer1: { directoryOk: false, arity: 1 },
  kDir2: { directoryOk: true, arity: 2 },
  kServer2: { directoryOk: false, arity: 2 },
  kDir3: { directoryOk: true, arity: 3 },
};

/** `const char kModPagespeedX[] = "..."` and `inline constexpr char` names. */
function apacheNameConstants(clean) {
  const consts = new Map();
  const cre = /(?:const|constexpr)\s+char\s+(kModPagespeed\w+)\[\]\s*=\s*"([^"]+)";/g;
  let cm;
  while ((cm = cre.exec(clean)) !== null) consts.set(cm[1], cm[2]);
  return consts;
}

function apacheCommandName(nameTok, consts) {
  return (
    literalString(nameTok) ??
    consts.get(nameTok.trim()) ??
    (nameTok.trim() === 'RewriteQuery::kModPagespeed' ? 'ModPagespeed' : null)
  );
}

/**
 * The Apache command table, in either source layout:
 * - mod_pagespeed 2.10.0 and later: the PAGESPEED_APACHE_DIRECTIVE_TABLE(X)
 *   macro of X(kKind, kModPagespeedX, "help") rows in apache_directive_table.h;
 * - earlier: the static mod_pagespeed_filter_cmds table in mod_instaweb.cc,
 *   APACHE_CONFIG_(DIR_)OPTION[2|3|23](kModPagespeedX | "literal", "help").
 * A row of the X-macro table with a kind this parser does not know (other than
 * the skipped fetch kinds), or a name it cannot resolve, is reported in
 * `problems` instead of being dropped silently; the generator refuses those.
 */
export function parseApacheCommandTable(src, problems = []) {
  const clean = stripComments(src);
  const consts = apacheNameConstants(clean);
  const macro = clean.indexOf('#define PAGESPEED_APACHE_DIRECTIVE_TABLE(X)');
  if (macro !== -1) {
    // The macro body is one logical line: drop the line continuations.
    const bodyStart = clean.indexOf('\n', macro) + 1;
    let body = '';
    for (const line of clean.slice(bodyStart).split('\n')) {
      const cont = /\\\s*$/.test(line);
      body += `${line.replace(/\\\s*$/, '')}\n`;
      if (!cont) break;
    }
    const out = [];
    for (const call of findCalls(body, 'X')) {
      const [kindTok, nameTok, ...helpParts] = call.args;
      const kindName = kindTok.trim();
      if (DIRECTIVE_TABLE_SKIPPED_KINDS.has(kindName)) continue;
      const kind = DIRECTIVE_TABLE_KINDS[kindName];
      if (!kind) {
        problems.push(`unknown directive table kind ${kindName} (name ${nameTok.trim()})`);
        continue;
      }
      const full = apacheCommandName(nameTok, consts);
      if (!full) {
        problems.push(`unresolved directive table name ${nameTok.trim()}`);
        continue;
      }
      out.push({
        name: full.replace(/^<?ModPagespeed/, ''),
        apacheName: full,
        directoryOk: kind.directoryOk,
        arity: kind.arity,
        help: literalString(helpParts.join(',')),
      });
    }
    return out;
  }
  const start = clean.indexOf('mod_pagespeed_filter_cmds[] = {');
  const end = clean.indexOf('};', start);
  const table = clean.slice(start, end);
  const re = /APACHE_(CONFIG_DIR_OPTION|CONFIG_OPTION|SCOPE_OPTION)(\d*)\s*\(/g;
  const out = [];
  let m;
  while ((m = re.exec(table)) !== null) {
    const callStart = m.index;
    const call = findCalls(table.slice(callStart), `APACHE_${m[1]}${m[2]}`)[0];
    if (!call) continue;
    const [nameTok, ...helpParts] = call.args;
    const full = apacheCommandName(nameTok, consts);
    if (!full) {
      problems.push(`unresolved command table name ${nameTok.trim()}`);
      continue;
    }
    out.push({
      name: full.replace(/^<?ModPagespeed/, ''),
      apacheName: full,
      directoryOk: m[1] === 'CONFIG_DIR_OPTION',
      arity: m[2] === '' ? 1 : m[2] === '23' ? '2-3' : Number(m[2]),
      help: literalString(helpParts.join(',')),
    });
  }
  return out;
}

/** Names listed in a `const char* const NAME[] = { "a", "b" }` array. */
export function parseStringArray(src, arrayName) {
  const clean = stripComments(src);
  const m = new RegExp(`${arrayName}\\[\\]\\s*=\\s*\\{([\\s\\S]*?)\\};`).exec(clean);
  if (!m) return [];
  return [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]);
}

/** `const char kX[] = "Y";` definitions in a file, as Map<constName, string>. */
export function parseNamedStrings(src) {
  const clean = stripComments(src);
  const map = new Map();
  const re = /const char (k\w+)\[\]\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);/g;
  let m;
  while ((m = re.exec(clean)) !== null) map.set(m[1], literalString(m[2]));
  return map;
}

/**
 * The process-scope option names of SystemProcessOptions
 * (pagespeed/system/system_process_options.cc, mod_pagespeed 2.10.0 and later):
 * the names of its kOptions table, which Parse1 looks up, plus the names Parse2
 * compares with `StringCaseEqual(option, kX)`. Returns a Set in source order.
 * Throws when the kOptions table is missing or empty, or when a row's name
 * constant does not resolve: Parse1 finds most of the names only through that
 * table, so a partial parse would silently move them out of process scope.
 */
export function parseProcessOptionNames(src) {
  const clean = stripComments(src);
  const consts = parseNamedStrings(src);
  const names = new Set();
  const table = /\bkOptions\[\]\s*=\s*\{([\s\S]*?)\n\};/.exec(clean);
  const rows = table ? [...table[1].matchAll(/\{\s*(k[A-Z]\w*)\s*,/g)] : [];
  if (rows.length === 0) {
    throw new Error(
      'no rows parsed from the kOptions table of SystemProcessOptions; teach parseProcessOptionNames the new layout',
    );
  }
  for (const m of rows) {
    const v = consts.get(m[1]);
    if (!v) {
      throw new Error(`kOptions row ${m[1]} does not resolve to a name constant`);
    }
    names.add(v);
  }
  for (const sig of ['SystemProcessOptions::Parse1(', 'SystemProcessOptions::Parse2(']) {
    const start = clean.indexOf(sig);
    if (start === -1) continue;
    const body = clean.slice(start, clean.indexOf('\n}', start));
    for (const m of body.matchAll(/StringCaseEqual\(\s*option\s*,\s*(k[A-Z]\w*)\s*\)/g)) {
      const v = consts.get(m[1]);
      if (v) names.add(v);
    }
  }
  return names;
}

/** The string literals a `StringCaseEqual(in, "X")` parser function accepts. */
export function parseAcceptedValues(src, fnSignature) {
  const clean = stripComments(src);
  const start = clean.indexOf(fnSignature);
  if (start === -1) return null;
  const end = clean.indexOf('\n}', start);
  const body = clean.slice(start, end);
  return [...body.matchAll(/StringCaseEqual\(\w+,\s*"([^"]+)"\)/g)].map((m) => m[1]);
}

// --- filters ---------------------------------------------------------------

/** gperf keyword table: `"name", RewriteOptions::kEnum` rows between %% markers. */
export function parseFilterNames(gperfSrc) {
  const parts = gperfSrc.split(/^%%$/m);
  if (parts.length < 3) throw new Error('gperf table markers not found');
  const rows = [];
  for (const line of parts[1].split('\n')) {
    const m = /^"([a-z0-9_]+)",\s*RewriteOptions::(k\w+)/.exec(line.trim());
    if (m) rows.push({ name: m[1], enumTok: m[2] });
  }
  return rows;
}

/** kFilterVectorStaticInitializer: {RewriteOptions::kEnum, "id"|kIdConst, "Label"}. */
export function parseFilterVector(rewriteOptionsCc) {
  const clean = stripComments(rewriteOptionsCc);
  const start = clean.indexOf('kFilterVectorStaticInitializer[] = {');
  const end = clean.indexOf('};', start);
  const body = clean.slice(start, end);
  const out = [];
  const re = /\{\s*RewriteOptions::(k\w+),\s*([^,]+),\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}/g;
  let m;
  while ((m = re.exec(body)) !== null) {
    out.push({ enumTok: m[1], idTok: m[2].trim(), label: literalString(m[3]) });
  }
  return out;
}

/** `const RewriteOptions::Filter NAME[] = { RewriteOptions::kX, ... };` */
export function parseFilterSet(rewriteOptionsCc, setName) {
  const clean = stripComments(rewriteOptionsCc);
  const m = new RegExp(`${setName}\\[\\] = \\{([\\s\\S]*?)\\};`).exec(clean);
  if (!m) return [];
  return [...m[1].matchAll(/RewriteOptions::(k\w+)/g)].map((x) => x[1]);
}

/**
 * Compound filter names expanded in AddByNameToFilterSet():
 *   if (option == "rewrite_images") { set->Insert(kX); ... } else if ...
 */
export function parseFilterAliases(rewriteOptionsCc) {
  const clean = stripComments(rewriteOptionsCc);
  const start = clean.indexOf('bool RewriteOptions::AddByNameToFilterSet(');
  const end = clean.indexOf('\n}', start);
  const body = clean.slice(start, end);
  const aliases = {};
  const re = /option == "([a-z_]+)"\)\s*\{([\s\S]*?)(?=\n\s*\} else|\n\s*\}\s*$)/g;
  let m;
  while ((m = re.exec(body)) !== null) {
    const members = [...m[2].matchAll(/set->Insert\((k\w+)\)/g)].map((x) => x[1]);
    const sets = [...m[2].matchAll(/arraysize\((k\w+FilterSet)\)/g)].map((x) => x[1]);
    aliases[m[1]] = { members, sets };
  }
  return aliases;
}

/**
 * `kName = 403,` style enumerator values (HttpStatus::Code, experiment ids),
 * as Map<name, { kind: 'expr', value }>, for resolving option defaults that
 * are enumerators rather than defined constants.
 */
export function parseEnumConstants(src) {
  const map = new Map();
  for (const m of stripComments(src).matchAll(/\b(k[A-Z]\w*)\s*=\s*(-?\d+)\s*[,}]/g)) {
    if (!map.has(m[1])) map.set(m[1], { kind: 'expr', value: m[2] });
  }
  return map;
}

/**
 * Resolve a local variable used as a default value to the constant it was
 * initialized from: `type name = {kX, ...}` or `name.assign(kX, ...)`.
 */
export function localInitializer(src, varName) {
  const clean = stripComments(src);
  const m =
    new RegExp(`\\b${varName}\\s*=\\s*\\{\\s*(k[A-Z]\\w*)`).exec(clean) ??
    new RegExp(`\\b${varName}\\.assign\\(\\s*(k[A-Z]\\w*)`).exec(clean);
  return m ? m[1] : null;
}
