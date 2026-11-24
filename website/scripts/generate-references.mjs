// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// generate-references.mjs — derives the configuration and filter reference
// data from the product's SOURCE, so the reference pages cannot drift from
// what the binaries accept.
//
//   mod_pagespeed tree (--module-src, checked out at the tag in
//   src/data/reference/source-pin.json)
//     net/instaweb/rewriter/rewrite_options_properties.inc  option table
//     net/instaweb/rewriter/rewrite_options.cc              explicit registrations,
//                                                           name constants, filter
//                                                           tables and sets
//     pagespeed/system/system_rewrite_options.cc            server-level options
//     pagespeed/system/system_rewrite_driver_factory.cc     process-level directives
//     pagespeed/system/system_process_options.cc            (2.10.0 and later)
//     pagespeed/apache/apache_config.cc, mod_instaweb.cc    Apache-only options,
//     pagespeed/apache/apache_directive_table.h             (the command table, 2.10.0 and later)
//                                                           the static command table
//     pagespeed/nginx/ngx_rewrite_options.cc                nginx-only options
//     pagespeed/iis/iis_rewrite_options.cpp                 IIS-only options
//     net/instaweb/rewriter/rewrite_filter_names.gperf      filter names
//   this tree
//     src/worker/main.cc                                    worker usage text
//     src/nginx/ngx_pagespeed_module.cc                     thin-module directives
//     website/src/data/examples.ts                          demo per filter
//     website/src/data/reference/filters-overlay.json       category and risk
//     website/src/data/reference/directives-overlay.json    prose for directives
//                                                           whose source text is
//                                                           only a syntax hint
//     => website/src/data/reference/{module-directives,worker-flags,
//        thin-module-directives,filters}.json + source-pin.json
//
// A field the source does not state is emitted as null, never guessed.
//
// Usage:
//   node scripts/generate-references.mjs --module-src ../../mod_pagespeed
//   node scripts/generate-references.mjs --module-src DIR --out /tmp/ref   # CI diff
//   node scripts/generate-references.mjs --module-src DIR --check          # exit 1 on drift

import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, join } from 'node:path';
import {
  parseWorkerUsage,
  parseThinModuleDirectives,
  parseConstants,
  parseEnumConstants,
  localInitializer,
  resolveDefault,
  parsePropertiesInc,
  parsePropertyCalls,
  parseDeprecatedProperties,
  parseApacheCommandTable,
  parseStringArray,
  parseNamedStrings,
  parseProcessOptionNames,
  parseAcceptedValues,
  parseFilterNames,
  parseFilterVector,
  parseFilterSet,
  parseFilterAliases,
  stripComments,
  literalString,
} from './lib/reference-parsers.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const WEBSITE_ROOT = resolve(__dirname, '..');
export const REPO_ROOT = resolve(WEBSITE_ROOT, '..');
export const REFERENCE_DIR = resolve(WEBSITE_ROOT, 'src/data/reference');
export const MODULE_REPO_URL = 'https://github.com/We-Amp/mod_pagespeed';
export const THIS_REPO_URL = 'https://github.com/We-Amp/pagespeed-optimizer';

export const OUTPUTS = [
  'source-pin.json',
  'module-directives.json',
  'worker-flags.json',
  'thin-module-directives.json',
  'filters.json',
];

// ---------------------------------------------------------------------------
// Scope → context tables (RewriteOptions::OptionScope, mod_instaweb.cc
// FillInApacheCommand, NgxRewriteOptions::GetOptionScope)
// ---------------------------------------------------------------------------

const SCOPE_BY_TOKEN = {
  kQueryScope: 'query',
  kDirectoryScope: 'directory',
  kServerScope: 'server',
  kLegacyProcessScope: 'legacy-process',
  kProcessScopeStrict: 'process',
};

export const CONTEXT_BY_SCOPE = {
  query: {
    apache: ['server config', 'virtual host', 'directory', '.htaccess'],
    nginx: ['http', 'server', 'location'],
    query: true,
  },
  directory: {
    apache: ['server config', 'virtual host', 'directory', '.htaccess'],
    nginx: ['http', 'server', 'location'],
    query: false,
  },
  server: { apache: ['server config', 'virtual host'], nginx: ['http', 'server'], query: false },
  'legacy-process': {
    apache: ['server config', 'virtual host (tolerated)'],
    nginx: ['http'],
    query: false,
  },
  process: { apache: ['server config'], nginx: ['http'], query: false },
};

// IIS reads every directive from pagespeed.config; IisConfiguration refuses a
// directive whose IisRewriteOptions::GetOptionScope is above directory scope
// inside a match rule (iis_configuration.cpp).
const IIS_CONTEXT_BY_SCOPE = {
  query: ['pagespeed.config', 'match rules'],
  directory: ['pagespeed.config', 'match rules'],
  server: ['pagespeed.config, outside match rules'],
  'legacy-process': ['pagespeed.config, outside match rules'],
  process: ['pagespeed.config, outside match rules'],
};

// Enum-valued defaults that are enumerator tokens without a numeric definition.
const ENUM_DEFAULTS = new Map([
  ['kPassThrough', 'PassThrough'],
  ['kCoreFilters', 'CoreFilters'],
  ['kOptimizeForBandwidth', 'OptimizeForBandwidth'],
  ['kAllFilters', 'AllFilters'],
  ['kEnabledOn', 'on'],
  ['kEnabledOff', 'off'],
  ['kEnabledStandby', 'standby'],
  ['kEnabledUnplugged', 'unplugged'],
  ['kLazyloadImagesModeAuto', 'auto'],
  ['kLazyloadImagesModeNative', 'native'],
  ['kLazyloadImagesModeJs', 'js'],
  ['kRulePackModeReport', 'report'],
  ['kRulePackModeEnforce', 'enforce'],
  ['kDaemonHtmlCacheOff', 'off'],
  ['kDaemonHtmlCacheReportOnly', 'report-only'],
]);

// Options whose accepted values are parsed by a dedicated function; the
// generator reads the literals that function compares against.
const ENUM_PARSERS = [
  ['RewriteLevel', 'bool RewriteOptions::ParseRewriteLevel('],
  ['EnableRewriting', 'EnabledEnum* value)'],
  ['LazyloadImagesMode', 'LazyloadImagesMode* value)'],
  ['RulePackMode', 'RulePackMode* value)'],
  ['DaemonHtmlCache', 'DaemonHtmlCacheMode* value)'],
];

// Argument shapes of the directives that are not registered options. The
// handlers in RewriteOptions::ParseAndSetOptionFromName{1,2,3},
// SystemRewriteOptions::ParseAndSetOptionFromName2 and
// SystemRewriteDriverFactory::ParseAndSetOption{1,2} take positional
// arguments; the Apache command table states their arity, this map states
// what each position is (the Apache help text is the source for most).
const DIRECTIVE_ARGS = {
  Allow: 'wildcard',
  Disallow: 'wildcard',
  Domain: 'domain',
  EnableFilters: 'filter[,filter...]',
  DisableFilters: 'filter[,filter...]',
  ForbidFilters: 'filter[,filter...]',
  ExperimentVariable: 'slot',
  ExperimentSpec: 'spec',
  RetainComment: 'wildcard',
  BlockingRewriteRefererUrls: 'wildcard',
  PermitIdsForCssCombining: 'wildcard',
  ProxySuffix: 'suffix',
  DownstreamCachePurgeLocationPrefix: 'host:port/path',
  AddResourceHeader: 'name value',
  CustomFetchHeader: 'name value',
  LoadFromFile: 'url_prefix filename_prefix',
  LoadFromFileMatch: 'url_regexp filename_prefix',
  LoadFromFileRule: 'Allow|Disallow filename_prefix',
  LoadFromFileRuleMatch: 'Allow|Disallow filename_regexp',
  MapOriginDomain: 'to_domain from_domain[,from_domain...] [host_header]',
  MapProxyDomain: 'proxy_domain origin_domain [to_domain]',
  MapRewriteDomain: 'to_domain from_domain[,from_domain...]',
  ShardDomain: 'from_domain shard_domain[,shard_domain...]',
  UrlValuedAttribute: 'element attribute category',
  Library: 'bytes md5 canonical_url',
  CreateSharedMemoryMetadataCache: 'name size_kb',
  StatisticsDomains: 'Allow|Disallow domain_wildcard',
  GlobalStatisticsDomains: 'Allow|Disallow domain_wildcard',
  MessagesDomains: 'Allow|Disallow domain_wildcard',
  ConsoleDomains: 'Allow|Disallow domain_wildcard',
  AdminDomains: 'Allow|Disallow domain_wildcard',
  GlobalAdminDomains: 'Allow|Disallow domain_wildcard',
  StaticAssetPrefix: 'url_prefix',
  UsePerVHostStatistics: 'on|off',
  InstallCrashHandler: 'on|off',
  NumRewriteThreads: 'auto|number',
  NumExpensiveRewriteThreads: 'auto|number',
  ForceCaching: 'on|off',
  ListOutstandingUrlsOnError: 'on|off',
  MessageBufferSize: 'bytes',
  TrackOriginalContentLength: 'on|off',
  ExperimentalMeasurementProxy: 'https://root.domain password',
  UseNativeFetcher: 'on|off',
  NativeFetcherMaxKeepaliveRequests: 'number',
  ProcessScriptVariables: 'on|off|all',
  ClearInheritedScripts: '',
  RulePackTrustKey: 'key_id base64_public_key',
};

// nginx-only directives handled directly in NgxRewriteOptions::ParseAndSetOptions
// (they configure the driver factory, not a RewriteOptions property, so the
// source carries no help string for them; these descriptions summarize the
// handler code).
const NGINX_ONLY_DIRECTIVES = [
  {
    name: 'UseNativeFetcher',
    scope: 'process',
    description:
      "Use nginx's own event-driven fetcher for the resource fetches the module makes, instead of the built-in serf fetcher.",
  },
  {
    name: 'NativeFetcherMaxKeepaliveRequests',
    scope: 'process',
    description:
      'Maximum number of requests the native fetcher sends over one keep-alive connection before it opens a new one (a positive integer).',
  },
  {
    name: 'ProcessScriptVariables',
    scope: 'process',
    description:
      'Evaluate nginx script variables ($var) in pagespeed directive arguments at request time: off, on (the LoadFromFile*, EnableFilters, DisableFilters, DownstreamCache* and ShardDomain directives) or all (every query- and directory-scoped option too). Settable once, at the top level.',
  },
  {
    name: 'ClearInheritedScripts',
    scope: 'directory',
    description: null,
  },
];

// IIS-only directives handled by name in IisRewriteOptions::ParseAndSetOptions
// (iis_rewrite_options.cpp). They set a process global, not a RewriteOptions
// property, so the source carries no help string; the description summarizes
// the handler and IisAdminAccessAllowed (iis_local_request.h). `global` names
// the bool the handler sets, whose initializer is the default. The scope is
// what GetOptionScope returns for a name on none of its lists.
const IIS_ONLY_DIRECTIVES = [
  {
    name: 'InfoUrlsLocalOnly',
    global: 'INFO_URLS_LOCAL_ONLY',
    scope: 'directory',
    // The handler sets a process global when the line is read, so the match
    // rule the line sits under does not limit it.
    contextIis: ['pagespeed.config (a match rule does not limit it)'],
    description:
      "Serve the admin (including cache purge), statistics, console and message pages to local requests only. With off, they are served to every request, unless an allowlist (AdminDomains, StatisticsDomains, ConsoleDomains, MessagesDomains, GlobalAdminDomains and GlobalStatisticsDomains) is set for that page; the request's host name (its Host header) must then match it. A request whose host name is on a configured allowlist is served even when it is not local. The value applies to the whole IIS process, not to one site.",
  },
];

// An Apache help string that is only an argument-shape hint ("name size_kb",
// "<Allow|Disallow> domain_wildcard"), not a sentence.
function isSyntaxHint(help) {
  if (help === null) return false;
  const tokens = help.trim().split(/\s+/);
  return (
    tokens.length <= 6 &&
    tokens.every((t) => /^[<[]?[a-z_|:/.*[\]<>,-]+[>\]]?\*?$/.test(t)) &&
    /[_<[|]/.test(help)
  );
}

// ---------------------------------------------------------------------------
// Area classification (presentation only; deterministic, so the CI diff is
// stable). First matching rule wins; AREA_ORDER is the page order.
// ---------------------------------------------------------------------------

const AREA_RULES = [
  [
    'Enabling and filter selection',
    /^(ModPagespeed|EnableRewriting|RewriteLevel|EnableFilters|DisableFilters|ForbidFilters|ForbidAllDisabledFilters|Allow|Disallow|RewriteUncacheableResources|RewriteRandomDropPercentage|EnableAggressiveRewritersForMobile|RewriteDeadlinePerFlushMs)$/,
  ],
  [
    'Request handling and policy',
    /^(AddOptionsToUrls|AllowOptionsToBeSetByCookies|OptionCookiesDurationMs|RequestOptionOverride|UrlSigningKey|AcceptInvalidSignatures|HonorCsp|RespectVary|DisableRewriteOnNoTransform|ModifyCachingHeaders|XHeaderValue|SupportNoScriptEnabled|RespectXForwardedProto|ObliviousPagespeedUrls|AgentOptimize|AccessControlAllowOrigins|ServeXhrAccessControlHeaders|RejectBlacklisted|RejectBlacklistedStatusCode|StickyQueryParameters|PreserveSubresourceHints|InlineResourcesWithoutExplicitAuthorization|CombineAcrossPaths|TrackOriginalContentLength|DisableBackgroundFetchesForBots|HideRefererUsingMeta|ServeStaleIfFetchError|ServeStaleWhileRevalidateThresholdSec|AllowLoggingUrlsInLogRecord|ProxyAuth|ForceBuffering|ExperimentalProxyAllRequests|AddResourceHeader)$/,
  ],
  [
    'In-place resource optimization',
    /^(InPlace|Ipro|ProactiveResourceFreshening|ProactivelyFreshenUserFacingRequest|CacheSmallImagesUnrewritten|NoTransformOptimizedImages)/,
  ],
  [
    'Domains and URLs',
    /^(Domain|MapOriginDomain|MapProxyDomain|MapRewriteDomain|ShardDomain|DomainShardCount|DomainRewriteCookies|DomainRewriteHyperlinks|ClientDomainRewrite|ProxySuffix|LoadFromFile|UrlValuedAttribute|MaxSegmentLength|MaxUrlSize|PreserveUrlRelativity|CssPreserveURLs|ImagePreserveURLs|JsPreserveURLs|StaticAssetPrefix|StaticAssetCDN|RemoteConfiguration)/,
  ],
  ['Optimizer worker', /^Daemon/],
  ['Rule packs', /^RulePack/],
  ['Bot authentication and licensing', /^(WebBotAuth|RslCap)/],
  [
    'Statistics, console and logging',
    /(Statistics|AdminDomains|AdminPath|ConsoleDomains|ConsolePath|MessagesDomains|MessagesPath|MessageBufferSize|^Log|ListOutstandingUrlsOnError|InstallCrashHandler|ReportUnloadTime|SlowFileLatencyUs|StrictAdminAccess|UsePerVHostStatistics|MaxRewriteInfoLogSize|InfoUrlsLocalOnly|TreatProxiedRequestsAsLocal|AutoCreateLogDir)/,
  ],
  [
    'Images',
    /(Image|Jpeg|Webp|Avif|Png|Gif|Progressive|Responsive|Lazyload|Resize|Sprite|Srcset|Provenance)/i,
  ],
  ['CSS', /(Css|Font|Flatten|StyleAttribute)/],
  ['JavaScript', /(Js(?=[A-Z]|$)|Javascript|Script|Library|Noscript|SourceMap)/],
  [
    'Caching',
    /(Cache|Lru|Memcached|Redis|Shm|SharedMemory|Ttl|Expir|Purge|Downstream|Cyclone|Stale|Revalidat|ForceCaching|Freshen|MetadataL2)/i,
  ],
  [
    'Fetching and origins',
    /(Fetch|Ssl|Https|Slurp|Proxy|Timeout|Blocking|KeepAlive|NativeFetcher|Loopback|UnknownHosts)/i,
  ],
  [
    'HTML rewriting and page hints',
    /(Html|Comment|Whitespace|Quote|Elide|Head\b|Meta\b|Flush|Pedantic|Amp|Speculation|Preload|Prefetch|Preconnect|Lowercase|Beacon|Instrumentation|Critical|AboveTheFold|Mobil)/i,
  ],
  ['Threads and limits', /(Thread|AtOnce|Deadline|Limit|Buffer|Memory|Max|Min)/i],
  ['Experiments and analytics', /(Experiment|Analytics|SpeedTracking|Noop|Cookie)/i],
  ['Other', /./],
];

export const AREA_ORDER = ['Deprecated and ignored', ...AREA_RULES.map((r) => r[0])].filter(
  (a, i, arr) => arr.indexOf(a) === i,
);
// Deprecated entries render last.
AREA_ORDER.push(AREA_ORDER.shift());

function classify(d) {
  if (d.kind === 'deprecated' || d.kind === 'renamed' || d.name === 'If')
    return 'Deprecated and ignored';
  if (/^Deprecated and ignored/i.test(d.description ?? '')) return 'Deprecated and ignored';
  return AREA_RULES.find(([, re]) => re.test(d.name))[0];
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function read(path) {
  return readFileSync(path, 'utf8');
}

function normalizeHelp(help) {
  if (help === null || help === undefined) return null;
  const text = help.replace(/\s+/g, ' ').trim();
  return text === '' ? null : text;
}

function argsFor(type, enumValues) {
  switch (type) {
    case 'bool':
      return 'on|off';
    case 'int':
    case 'float':
      return 'number';
    case 'enum':
      return enumValues ? enumValues.join('|') : 'value';
    default:
      return 'value';
  }
}

function syntaxFor(name, args, platforms, master = false) {
  const apache = master ? 'ModPagespeed' : `ModPagespeed${name}`;
  const nginx = master ? 'pagespeed' : `pagespeed ${name}`;
  const tail = args ? ` ${args}` : '';
  const syntax = {
    apache: platforms.includes('apache') ? `${apache}${tail}` : null,
    nginx: platforms.includes('nginx') ? `${nginx}${tail};` : null,
  };
  // IIS spells a directive like nginx, without the semicolon. Only the
  // IIS-only entries carry the key (see buildModuleDirectives).
  if (platforms.includes('iis')) syntax.iis = `${nginx}${tail}`;
  return syntax;
}

function gitInfo(dir) {
  const run = (args) => {
    try {
      return execFileSync('git', ['-C', dir, ...args], { encoding: 'utf8' }).trim();
    } catch {
      return null;
    }
  };
  return { tag: run(['describe', '--tags', '--exact-match']), commit: run(['rev-parse', 'HEAD']) };
}

function loadOverlay(name, key) {
  const path = resolve(REFERENCE_DIR, name);
  return existsSync(path) ? (JSON.parse(read(path))[key] ?? {}) : {};
}

// ---------------------------------------------------------------------------
// Module directives
// ---------------------------------------------------------------------------

export function buildModuleDirectives(moduleSrc, warnings) {
  const p = (rel) => read(join(moduleSrc, rel));
  const rewriteOptionsCc = p('net/instaweb/rewriter/rewrite_options.cc');
  const rewriteOptionsH = p('net/instaweb/rewriter/public/rewrite_options.h');
  const propertiesInc = p('net/instaweb/rewriter/rewrite_options_properties.inc');
  const systemCc = p('pagespeed/system/system_rewrite_options.cc');
  const systemH = p('pagespeed/system/system_rewrite_options.h');
  const factoryCc = p('pagespeed/system/system_rewrite_driver_factory.cc');
  const apacheCc = p('pagespeed/apache/apache_config.cc');
  const modInstawebCc = p('pagespeed/apache/mod_instaweb.cc');
  // From mod_pagespeed 2.10.0 the Apache command table and the process-scope
  // option names live in their own files; earlier trees have neither.
  const pIfPresent = (rel) => (existsSync(join(moduleSrc, rel)) ? { rel, src: p(rel) } : null);
  const directiveTable = pIfPresent('pagespeed/apache/apache_directive_table.h') ?? {
    rel: 'pagespeed/apache/mod_instaweb.cc',
    src: modInstawebCc,
  };
  const processOptions = pIfPresent('pagespeed/system/system_process_options.cc');
  const ngxCc = p('pagespeed/nginx/ngx_rewrite_options.cc');
  const iisCc = p('pagespeed/iis/iis_rewrite_options.cpp');
  const overlay = loadOverlay('directives-overlay.json', 'directives');

  const constants = parseConstants([
    rewriteOptionsCc,
    rewriteOptionsH,
    systemCc,
    systemH,
    factoryCc,
    ...(processOptions ? [processOptions.src] : []),
    apacheCc,
    ngxCc,
    p('pagespeed/kernel/base/timer.h'),
    p('pagespeed/system/daemon_reader.h'),
    iisCc,
  ]);
  for (const [k, v] of [
    ...parseEnumConstants(p('pagespeed/kernel/http/http_names.h')),
    ...parseEnumConstants(p('net/instaweb/rewriter/public/experiment_util.h')),
  ]) {
    if (!constants.has(k)) constants.set(k, v);
  }

  const resolveName = (tok) => {
    const lit = literalString(tok);
    if (lit !== null) return lit;
    const bare = tok.trim().replace(/^(?:\w+::)+/, '');
    const c = constants.get(bare);
    if (c && c.kind === 'string') return c.value;
    if (/^k[A-Z]/.test(bare)) warnings.push(`unresolved option name constant: ${tok}`);
    return null;
  };

  const enumValuesByName = new Map(
    ENUM_PARSERS.map(([name, sig]) => [name, parseAcceptedValues(rewriteOptionsCc, sig)]),
  );
  // The EnabledEnum parser accepts a boolean first (on/off, true/false), then
  // the two named states.
  const enabledValues = ['on', 'off', ...(enumValuesByName.get('EnableRewriting') ?? [])];
  enumValuesByName.set('EnableRewriting', enabledValues);

  const resolveDefaultTok = (tok, src) => {
    let r = resolveDefault(tok, constants, ENUM_DEFAULTS);
    if (r.value === null && /^[a-z_]\w*$/.test(tok.trim())) {
      // A local variable (default_densities, kDefaultBeaconUrls is a struct):
      // follow its initializer to the constant.
      const init = localInitializer(src, tok.trim());
      if (init) r = resolveDefault(init, constants, ENUM_DEFAULTS);
    }
    if (r.value === null && /^k[A-Z]\w*s$/.test(tok.trim())) {
      const init = localInitializer(src, tok.trim());
      if (init) r = resolveDefault(init, constants, ENUM_DEFAULTS);
    }
    return r;
  };

  const registrations = [
    ...parsePropertiesInc(propertiesInc).map((r) => ({ ...r, src: propertiesInc })),
    ...parsePropertyCalls(rewriteOptionsCc, 'AddBaseProperty', 'rewrite_options').map((r) => ({
      ...r,
      src: rewriteOptionsCc,
    })),
    ...parsePropertyCalls(systemCc, 'AddSystemProperty', 'system', 'kServerScope').map((r) => ({
      ...r,
      src: systemCc,
    })),
    ...parsePropertyCalls(apacheCc, 'AddApacheProperty', 'apache', 'kServerScope').map((r) => ({
      ...r,
      src: apacheCc,
    })),
    ...parsePropertyCalls(ngxCc, 'add_ngx_option', 'nginx').map((r) => ({ ...r, src: ngxCc })),
  ];

  const byName = new Map();
  for (const r of registrations) {
    const name = resolveName(r.nameTok);
    if (!name) continue;
    const scope = SCOPE_BY_TOKEN[r.scopeTok?.trim().replace(/^(?:\w+::)+/, '')] ?? null;
    if (!scope) warnings.push(`unresolved scope ${r.scopeTok} for ${name}`);
    const { value: def, type } = resolveDefaultTok(r.defaultTok, r.src);
    if (def === null) warnings.push(`unresolved default for ${name}: ${r.defaultTok}`);
    const help = normalizeHelp(r.help);
    // mod_instaweb.cc registers an Apache directive only for options that
    // carry help text; nginx accepts every registered option by name.
    const platforms =
      r.origin === 'apache'
        ? ['apache']
        : r.origin === 'nginx'
          ? ['nginx']
          : help === null
            ? ['nginx']
            : ['apache', 'nginx'];
    const enumValues = enumValuesByName.get(name) ?? null;
    const valueType = enumValues ? 'enum' : type;
    const entry = byName.get(name);
    if (entry) {
      // Same name registered by two ports (DaemonSocketPath and friends).
      for (const pf of platforms) if (!entry.platforms.includes(pf)) entry.platforms.push(pf);
      if (help && help !== entry.description) entry.descriptionByPlatform[r.origin] = help;
      continue;
    }
    byName.set(name, {
      name,
      kind: 'option',
      origin: r.origin,
      id:
        literalString(r.idTok) ??
        constants.get(r.idTok?.trim().replace(/^(?:\w+::)+/, ''))?.value ??
        null,
      scope,
      valueType,
      args: argsFor(valueType, enumValues),
      default: def,
      description: help,
      descriptionSource: help === null ? null : 'source',
      descriptionByPlatform: {},
      platforms,
      safeToPrint: r.safeToPrint,
      deprecated: /^Deprecated and ignored/i.test(help ?? ''),
    });
  }

  // Options a port still accepts but ignores (AddDeprecatedProperty).
  for (const d of [
    ...parseDeprecatedProperties(rewriteOptionsCc, 'rewrite_options'),
    ...parseDeprecatedProperties(apacheCc, 'apache'),
  ]) {
    if (!d.name || byName.has(d.name)) continue;
    byName.set(d.name, {
      name: d.name,
      kind: 'deprecated',
      origin: d.origin,
      id: null,
      scope: SCOPE_BY_TOKEN[d.scopeTok.trim().replace(/^(?:\w+::)+/, '')] ?? null,
      valueType: null,
      args: 'value',
      default: null,
      description:
        d.origin === 'apache'
          ? 'Removed option; the name is still accepted and ignored.'
          : 'Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.',
      descriptionSource: 'generator',
      descriptionByPlatform: {},
      platforms: d.origin === 'apache' ? ['apache'] : ['apache', 'nginx'],
      safeToPrint: null,
      deprecated: true,
    });
  }

  // Renamed options (kRenamedOptionNameData): the old name maps to the new one.
  const renamedBlock = /kRenamedOptionNameData\[\] = \{([\s\S]*?)\};/.exec(
    stripComments(rewriteOptionsCc),
  );
  const addRenamed = (oldName, newName, origin, platforms, scopeFallback) => {
    const target = byName.get(newName);
    byName.set(oldName, {
      name: oldName,
      kind: 'renamed',
      origin,
      id: null,
      scope: target?.scope ?? scopeFallback,
      valueType: null,
      args: target?.args ?? 'value',
      default: null,
      description: `Former name of ${newName}; accepted and mapped to it.`,
      descriptionSource: 'generator',
      descriptionByPlatform: {},
      platforms,
      safeToPrint: null,
      deprecated: true,
      renamedTo: newName,
    });
  };
  if (renamedBlock) {
    for (const m of renamedBlock[1].matchAll(/\{\s*"([^"]+)",\s*"([^"]+)"\s*\}/g)) {
      addRenamed(m[1], m[2], 'rewrite_options', ['apache', 'nginx'], 'directory');
    }
  }

  // Directives that are not registered options are handled by name in the
  // parsers. Which names the shared code handles decides whether a directive
  // from the Apache command table also exists on nginx.
  const handledShared = new Set();
  const collectHandled = (src, signatures, varName) => {
    const clean = stripComments(src);
    for (const sig of signatures) {
      const start = clean.indexOf(sig);
      if (start === -1) {
        warnings.push(`handler not found: ${sig}`);
        continue;
      }
      const body = clean.slice(start, clean.indexOf('\n}', start));
      for (const m of body.matchAll(
        new RegExp(`StringCaseEqual\\(${varName},\\s*((?:\\w+::)*k[A-Z]\\w*|"[^"]+")\\)`, 'g'),
      )) {
        const name = resolveName(m[1]);
        if (name) handledShared.add(name);
      }
      for (const m of body.matchAll(
        new RegExp(`StringCaseEqual\\(\\s*"([^"]+)",\\s*${varName}\\)`, 'g'),
      )) {
        handledShared.add(m[1]);
      }
    }
  };
  collectHandled(
    rewriteOptionsCc,
    [
      'RewriteOptions::ParseAndSetOptionFromNameWithScope(',
      'RewriteOptions::OptionSettingResult RewriteOptions::ParseAndSetOptionFromName2(',
      'RewriteOptions::OptionSettingResult RewriteOptions::ParseAndSetOptionFromName3(',
    ],
    'name',
  );
  collectHandled(systemCc, ['SystemRewriteOptions::ParseAndSetOptionFromName2('], 'name');
  collectHandled(
    factoryCc,
    [
      'SystemRewriteDriverFactory::ParseAndSetOption1(',
      'SystemRewriteDriverFactory::ParseAndSetOption2(',
    ],
    'option',
  );

  // From 2.10.0 the factory hands these names to SystemProcessOptions.
  if (processOptions) {
    for (const name of parseProcessOptionNames(processOptions.src)) handledShared.add(name);
  }

  // Process-level factory directives: the factory parser refuses (or warns
  // and ignores) them outside the top-level configuration.
  const factoryNames = new Set(
    [
      ...parseNamedStrings(factoryCc).values(),
      ...(processOptions ? parseNamedStrings(processOptions.src).values() : []),
    ].filter((v) => handledShared.has(v)),
  );
  requireEntries(
    [...factoryNames],
    processOptions?.rel ?? 'pagespeed/system/system_rewrite_driver_factory.cc',
    'process-scope directive names',
  );

  const tableProblems = [];
  const apacheCommands = parseApacheCommandTable(directiveTable.src, tableProblems);
  requireNoProblems(tableProblems, directiveTable.rel);
  requireEntries(apacheCommands, directiveTable.rel, 'Apache command-table entries');

  // GetOptionScope compares these lists case-insensitively (the list spells
  // FetcherTimeoutMs, the option is FetcherTimeOutMs).
  const lower = (names) => new Set(names.map((n) => n.toLowerCase()));
  const serverOnly = lower(parseStringArray(ngxCc, 'server_only_options'));
  const mainOnly = lower(parseStringArray(ngxCc, 'main_only_options'));
  const inList = (set, name) => set.has(name.toLowerCase());
  const nginxScopeFor = (name) =>
    inList(mainOnly, name) ? 'process' : inList(serverOnly, name) ? 'server' : 'directory';

  for (const cmd of apacheCommands) {
    const name = cmd.name;
    if (name === 'If') {
      byName.set('If', {
        name: 'If',
        kind: 'directive',
        origin: 'apache',
        id: null,
        scope: 'server',
        valueType: null,
        args: 'spdy|!spdy',
        apacheName: '<ModPagespeedIf',
        default: null,
        description: cmd.help,
        descriptionSource: 'source',
        descriptionByPlatform: {},
        platforms: ['apache'],
        safeToPrint: null,
        deprecated: true,
      });
      continue;
    }
    if (cmd.apacheName === 'ModPagespeed') {
      byName.set('ModPagespeed', {
        name: 'ModPagespeed',
        kind: 'directive',
        master: true,
        origin: 'rewrite_options',
        id: null,
        scope: 'directory',
        valueType: 'enum',
        args: (enabledValues ?? ['on', 'off']).join('|'),
        default: null,
        description:
          'The module switch. on optimizes; standby serves already-optimized .pagespeed. resources and answers query-parameter requests but optimizes no new traffic; unplugged intercepts nothing (on nginx, off is an alias for unplugged). unplugged can only be set at the top level or in a virtual host. nginx needs an explicit pagespeed on; in the server block.',
        descriptionSource: 'generator',
        descriptionByPlatform: {},
        platforms: ['apache', 'nginx'],
        safeToPrint: null,
        deprecated: false,
      });
      continue;
    }
    const existing = byName.get(name);
    if (existing) {
      // The static table makes an option an Apache directive even when its
      // registration carries no help text (FetcherTimeOutMs).
      if (!existing.platforms.includes('apache')) {
        existing.platforms.push('apache');
        existing.apacheScope = cmd.directoryOk ? 'directory' : 'server';
      }
      if (!existing.description && cmd.help && !isSyntaxHint(cmd.help)) {
        existing.description = cmd.help;
        existing.descriptionSource = 'source';
      }
      continue;
    }
    const renamed =
      /^DEPRECATED, use ModPagespeed(\w+)\.|^Deprecated\.\s+Use ModPagespeed(\w+)/i.exec(
        cmd.help ?? '',
      );
    if (renamed) {
      addRenamed(
        name,
        renamed[1] ?? renamed[2],
        'apache',
        ['apache'],
        cmd.directoryOk ? 'directory' : 'server',
      );
      continue;
    }
    const shared = handledShared.has(name);
    const factory = factoryNames.has(name);
    const platforms = shared || factory ? ['apache', 'nginx'] : ['apache'];
    const help = isSyntaxHint(cmd.help) ? null : cmd.help;
    byName.set(name, {
      name,
      kind: 'directive',
      origin: factory ? 'factory' : shared ? 'rewrite_options' : 'apache',
      id: null,
      scope: factory ? 'process' : cmd.directoryOk ? 'directory' : 'server',
      scopeNginx: factory ? 'process' : nginxScopeFor(name),
      valueType: null,
      args: DIRECTIVE_ARGS[name] ?? (cmd.arity === 1 ? 'value' : null),
      arity: cmd.arity,
      default: null,
      description: help,
      descriptionSource: help ? 'source' : null,
      descriptionByPlatform: {},
      platforms,
      safeToPrint: null,
      deprecated: false,
    });
    if (!(name in DIRECTIVE_ARGS)) warnings.push(`no argument shape for directive ${name}`);
  }

  for (const d of NGINX_ONLY_DIRECTIVES) {
    if (byName.has(d.name)) continue;
    byName.set(d.name, {
      name: d.name,
      kind: 'directive',
      origin: 'nginx',
      id: null,
      scope: d.scope,
      valueType: null,
      args: DIRECTIVE_ARGS[d.name] ?? 'value',
      default: null,
      description: d.description,
      descriptionSource: d.description ? 'generator' : null,
      descriptionByPlatform: {},
      platforms: ['nginx'],
      safeToPrint: null,
      deprecated: false,
    });
  }

  // IIS-only options and directives. A name no other port accepts is added;
  // a name that IisRewriteOptions registers and another port registers too
  // gets IIS marked on it. Options shared through RewriteOptions and
  // SystemRewriteOptions are marked further down.
  for (const r of parsePropertyCalls(iisCc, 'add_iis_option', 'iis')) {
    const name = resolveName(r.nameTok);
    if (!name) continue;
    const scope = SCOPE_BY_TOKEN[r.scopeTok?.trim().replace(/^(?:\w+::)+/, '')] ?? null;
    if (!scope) warnings.push(`unresolved scope ${r.scopeTok} for ${name}`);
    // A name another port also registers (AdminPath, StatisticsPath,
    // DaemonSocketPath ...) is the same directive: mark IIS on it, with the
    // scope IisRewriteOptions registers it under.
    const shared = byName.get(name);
    if (shared) {
      if (!shared.platforms.includes('iis')) shared.platforms.push('iis');
      shared.scopeIis = scope;
      const { value: iisDef } = resolveDefaultTok(r.defaultTok, iisCc);
      if (iisDef !== null && JSON.stringify(iisDef) !== JSON.stringify(shared.default)) {
        shared.defaultIis = iisDef;
      }
      continue;
    }
    const { value: def, type } = resolveDefaultTok(r.defaultTok, iisCc);
    if (def === null) warnings.push(`unresolved default for ${name}: ${r.defaultTok}`);
    const help = normalizeHelp(r.help);
    byName.set(name, {
      name,
      kind: 'option',
      origin: 'iis',
      id: literalString(r.idTok),
      scope,
      valueType: type,
      args: argsFor(type, null),
      default: def,
      description: help,
      descriptionSource: help === null ? null : 'source',
      descriptionByPlatform: {},
      platforms: ['iis'],
      safeToPrint: r.safeToPrint,
      deprecated: false,
    });
  }
  const iisClean = stripComments(iisCc);
  for (const d of IIS_ONLY_DIRECTIVES) {
    if (byName.has(d.name)) continue;
    if (!iisClean.includes(`IsDirective(directive, "${d.name}")`)) {
      warnings.push(`IIS directive ${d.name} is no longer handled in iis_rewrite_options.cpp`);
      continue;
    }
    const init = new RegExp(`\\bbool ${d.global} = (true|false);`).exec(iisClean);
    if (!init) warnings.push(`unresolved default for ${d.name}: ${d.global}`);
    byName.set(d.name, {
      name: d.name,
      kind: 'directive',
      origin: 'iis',
      id: null,
      scope: d.scope,
      valueType: 'bool',
      args: 'on|off',
      default: init ? (init[1] === 'true' ? 'on' : 'off') : null,
      description: d.description,
      descriptionSource: 'generator',
      descriptionByPlatform: {},
      platforms: ['iis'],
      contextIis: d.contextIis,
      safeToPrint: null,
      deprecated: false,
    });
  }

  // The registered default of RewriteLevel is the base class's PassThrough,
  // but neither server runs with it: Apache's create_dir_config calls
  // SetDefaultRewriteLevel and nginx's NewRewriteOptions calls SetRewriteLevel
  // on every new configuration. The effective default is what both set; when
  // the two ports disagree, or the call is gone, the registered value stays
  // and the run warns.
  const rewriteLevel = byName.get('RewriteLevel');
  if (rewriteLevel) {
    const ngxFactoryCc = p('pagespeed/nginx/ngx_rewrite_driver_factory.cc');
    const apacheLevel =
      /SetDefaultRewriteLevel\(\s*RewriteOptions::(k\w+)\s*\)/.exec(modInstawebCc)?.[1] ?? null;
    // The body of NgxRewriteDriverFactory::NewRewriteOptions, or null when
    // the definition or its closing brace is not found.
    const ngxStart = ngxFactoryCc.indexOf('NgxRewriteDriverFactory::NewRewriteOptions()');
    const ngxEnd = ngxStart === -1 ? -1 : ngxFactoryCc.indexOf('\n}', ngxStart);
    const ngxBody = ngxEnd === -1 ? null : ngxFactoryCc.slice(ngxStart, ngxEnd);
    const ngxLevel =
      ngxBody === null
        ? null
        : (/SetRewriteLevel\(\s*RewriteOptions::(k\w+)\s*\)/.exec(ngxBody)?.[1] ?? null);
    const portDefault =
      apacheLevel && apacheLevel === ngxLevel ? ENUM_DEFAULTS.get(apacheLevel) : null;
    if (portDefault) rewriteLevel.default = portDefault;
    else
      warnings.push(
        `RewriteLevel: no common port default (Apache ${apacheLevel}, nginx ${ngxLevel}); kept ${rewriteLevel.default}`,
      );
  }

  // IIS reads its configuration through IisRewriteOptions, a
  // SystemRewriteOptions subclass, and passes every directive it does not
  // handle itself to ParseAndSetOptionFromName{1,2,3}
  // (pagespeed/iis/iis_rewrite_options.cpp). So every option and shared
  // directive that RewriteOptions or SystemRewriteOptions registers works on
  // IIS as well. Factory-level and port-specific names do not.
  for (const d of byName.values()) {
    if (
      (d.kind === 'option' || d.kind === 'directive') &&
      (d.origin === 'rewrite_options' || d.origin === 'system') &&
      !d.platforms.includes('iis')
    ) {
      d.platforms.push('iis');
    }
  }

  // Prose from the docs overlay fills in where the source has only a syntax
  // hint or nothing at all.
  for (const d of byName.values()) {
    const prose = overlay[d.name];
    if (prose && (d.description === null || d.kind === 'directive')) {
      d.description = prose;
      d.descriptionSource = 'docs';
    }
  }

  const directives = [...byName.values()]
    .map((d) => {
      const base = CONTEXT_BY_SCOPE[d.scope] ?? { apache: null, nginx: null, query: false };
      const apacheScope = d.apacheScope ?? d.scope;
      // NgxRewriteOptions::GetOptionScope consults its server-only and
      // main-only name lists before the option's own scope.
      const nginxScope =
        d.scopeNginx ??
        (inList(mainOnly, d.name) ? 'process' : inList(serverOnly, d.name) ? 'server' : d.scope);
      const context = {
        apache: d.platforms.includes('apache')
          ? (CONTEXT_BY_SCOPE[apacheScope]?.apache ?? null)
          : null,
        nginx: d.platforms.includes('nginx') ? (CONTEXT_BY_SCOPE[nginxScope]?.nginx ?? null) : null,
        query: base.query,
      };
      if (d.platforms.includes('iis')) {
        context.iis = d.contextIis ?? IIS_CONTEXT_BY_SCOPE[d.scopeIis ?? d.scope] ?? null;
      }
      const syntax = syntaxFor(d.name, d.args, d.platforms, d.master === true);
      if (d.apacheName) syntax.apache = `${d.apacheName} ${d.args}>`;
      return {
        name: d.name,
        kind: d.kind,
        area: classify(d),
        platforms: [...d.platforms].sort(),
        syntax,
        default: d.default,
        defaultIis: d.defaultIis,
        context,
        since: null,
        description: d.description,
        descriptionSource: d.descriptionSource,
        descriptionByPlatform: Object.keys(d.descriptionByPlatform).length
          ? d.descriptionByPlatform
          : undefined,
        scope: d.scope,
        scopeNginx: d.scopeNginx,
        valueType: d.valueType,
        id: d.id,
        origin: d.origin,
        deprecated: d.deprecated,
        renamedTo: d.renamedTo,
        safeToPrint: d.safeToPrint,
      };
    })
    .sort((a, b) => a.name.localeCompare(b.name, 'en'));

  return { directives, enumValues: Object.fromEntries(enumValuesByName) };
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

const CANONICAL_SPELLING = ['trim_urls', 'insert_image_dimensions'];

export function buildFilters(moduleSrc, warnings) {
  const p = (rel) => read(join(moduleSrc, rel));
  const rewriteOptionsCc = p('net/instaweb/rewriter/rewrite_options.cc');
  const rewriteOptionsH = p('net/instaweb/rewriter/public/rewrite_options.h');
  const gperf = p('net/instaweb/rewriter/rewrite_filter_names.gperf');
  const constants = parseConstants([rewriteOptionsCc, rewriteOptionsH]);

  const names = parseFilterNames(gperf);
  const vector = new Map(parseFilterVector(rewriteOptionsCc).map((v) => [v.enumTok, v]));
  const sets = {
    core: new Set(parseFilterSet(rewriteOptionsCc, 'kCoreFilterSet')),
    optimizeForBandwidth: new Set(
      parseFilterSet(rewriteOptionsCc, 'kOptimizeForBandwidthFilterSet'),
    ),
    dangerous: new Set(parseFilterSet(rewriteOptionsCc, 'kDangerousFilterSet')),
  };
  const aliasesRaw = parseFilterAliases(rewriteOptionsCc);
  const overlay = loadOverlay('filters-overlay.json', 'filters');
  const examples = parseExamples(read(resolve(WEBSITE_ROOT, 'src/data/examples.ts')));

  // Alternate spellings: two gperf names for one enum.
  const namesByEnum = new Map();
  for (const n of names) {
    const list = namesByEnum.get(n.enumTok) ?? [];
    list.push(n.name);
    namesByEnum.set(n.enumTok, list);
  }
  const canonicalFor = (enumTok) => {
    const list = namesByEnum.get(enumTok) ?? [];
    return list.find((n) => CANONICAL_SPELLING.includes(n)) ?? list[0] ?? null;
  };

  const idOf = (tok) => {
    if (!tok) return null;
    const lit = literalString(tok);
    if (lit !== null) return lit;
    const c = constants.get(tok.replace(/^(?:\w+::)+/, ''));
    return c?.kind === 'string' ? c.value : null;
  };

  const filters = names.map((n) => {
    const v = vector.get(n.enumTok);
    if (!v)
      warnings.push(`filter ${n.name} (${n.enumTok}) has no kFilterVectorStaticInitializer entry`);
    const canonical = canonicalFor(n.enumTok);
    const o = overlay[n.name] ?? {};
    if (!o.category) warnings.push(`filters-overlay.json has no category for ${n.name}`);
    const dangerous = sets.dangerous.has(n.enumTok);
    const deprecated = /Deprecated$/.test(n.enumTok) || v?.label === 'Deprecated.';
    return {
      name: n.name,
      enum: n.enumTok,
      id: idOf(v?.idTok),
      label: v?.label ?? null,
      core: sets.core.has(n.enumTok),
      optimizeForBandwidth: sets.optimizeForBandwidth.has(n.enumTok),
      dangerous,
      deprecated,
      alternateSpellingOf: canonical !== n.name ? canonical : null,
      category: o.category ?? null,
      summary: o.summary ?? null,
      risk: dangerous ? 'Dangerous' : deprecated ? 'Deprecated' : (o.risk ?? null),
      riskSource: dangerous || deprecated ? 'source' : o.risk ? 'docs' : null,
      note: o.note ?? null,
      example: examples.get(n.name) ?? null,
    };
  });

  const membership = (enumToks, set) => {
    const inSet = enumToks.filter((e) => set.has(e)).length;
    return inSet === enumToks.length ? true : inSet === 0 ? false : 'partial';
  };
  const aliases = [];
  const setKeywords = {};
  for (const [name, a] of Object.entries(aliasesRaw)) {
    if (a.members.length === 0) {
      setKeywords[name] = a.sets.map((s) =>
        s.replace(/^k|FilterSet$/g, '').replace(/^./, (c) => c.toLowerCase()),
      );
      continue;
    }
    const o = overlay[name] ?? {};
    if (!o.category) warnings.push(`filters-overlay.json has no category for alias ${name}`);
    aliases.push({
      name,
      members: a.members.map(canonicalFor),
      core: membership(a.members, sets.core),
      optimizeForBandwidth: membership(a.members, sets.optimizeForBandwidth),
      category: o.category ?? null,
      summary: o.summary ?? null,
      risk: o.risk ?? null,
      riskSource: o.risk ? 'docs' : null,
      note: o.note ?? null,
      example: examples.get(name) ?? null,
    });
  }

  const toNames = (set) =>
    [...set]
      .map(canonicalFor)
      .filter(Boolean)
      .sort((a, b) => a.localeCompare(b, 'en'));
  return {
    filters: filters.sort((a, b) => a.name.localeCompare(b.name, 'en')),
    aliases: aliases.sort((a, b) => a.name.localeCompare(b.name, 'en')),
    sets: {
      core: toNames(sets.core),
      optimizeForBandwidth: toNames(sets.optimizeForBandwidth),
      dangerous: toNames(sets.dangerous),
    },
    setKeywords,
  };
}

/** slug + filters of every example in examples.ts, as Map<filterName, examplePath>. */
export function parseExamples(examplesTs) {
  const direct = new Map();
  const primary = new Map();
  const re = /slug:\s*'([^']+)'[\s\S]*?filters:\s*'([^']+)'/g;
  let m;
  while ((m = re.exec(examplesTs)) !== null) {
    const slug = m[1];
    const filters = m[2].split(',').map((f) => f.trim().replace(/^[+-]/, ''));
    if (filters.includes(slug)) direct.set(slug, `/examples/${slug}/`);
    if (!primary.has(filters[0])) primary.set(filters[0], `/examples/${slug}/`);
  }
  const out = new Map(primary);
  for (const [k, v] of direct) out.set(k, v);
  return out;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

export function generate({ moduleSrc, tag }) {
  const warnings = [];
  const git = gitInfo(moduleSrc);
  const pinTag = tag ?? git.tag;
  if (!pinTag) {
    throw new Error('cannot determine the mod_pagespeed tag: pass --tag or check out a tag');
  }
  const moduleSource = { repo: MODULE_REPO_URL, tag: pinTag, commit: git.commit };
  const treeSource = (path) => ({ repo: THIS_REPO_URL, path });

  const { directives, enumValues } = buildModuleDirectives(moduleSrc, warnings);
  const filters = buildFilters(moduleSrc, warnings);
  const workerFlags = parseWorkerUsage(read(resolve(REPO_ROOT, 'src/worker/main.cc')));
  const thin = parseThinModuleDirectives(
    read(resolve(REPO_ROOT, 'src/nginx/ngx_pagespeed_module.cc')),
  );

  const generator = 'website/scripts/generate-references.mjs';
  const files = {
    'source-pin.json': { mod_pagespeed: pinTag },
    'module-directives.json': {
      source: moduleSource,
      generator,
      areas: AREA_ORDER,
      enumValues,
      directives,
    },
    'worker-flags.json': {
      source: treeSource('src/worker/main.cc'),
      generator,
      flags: workerFlags,
    },
    'thin-module-directives.json': {
      source: treeSource('src/nginx/ngx_pagespeed_module.cc'),
      generator,
      directives: thin,
    },
    'filters.json': { source: moduleSource, generator, ...filters },
  };
  return { files, warnings };
}

/**
 * Throws when a registration source yields nothing: the module moved its
 * registrations to a layout the parsers do not read, and the reference would
 * silently lose every entry that file used to provide.
 */
export function requireEntries(entries, file, what) {
  if (entries.length > 0) return;
  throw new Error(
    `no ${what} parsed from ${file}: the module source layout changed; ` +
      'teach scripts/lib/reference-parsers.mjs to read it',
  );
}

/**
 * Throws when a registration table had rows the parser could not read (an
 * unknown kind or a name that does not resolve): each one would otherwise drop
 * a directive from the reference without a word.
 */
export function requireNoProblems(problems, file) {
  if (problems.length === 0) return;
  throw new Error(
    `${problems.length} unreadable row(s) in ${file}: ${problems.join('; ')}; ` +
      'teach scripts/lib/reference-parsers.mjs to read them',
  );
}

/** Generating fewer than this share of the committed directives is refused. */
export const MIN_DIRECTIVE_SHARE = 0.9;

/**
 * Throws when a generation yields sharply fewer module directives than the
 * committed reference. `committed` is null when there is no committed file.
 */
export function checkDirectiveCount(generated, committed) {
  if (committed === null || committed === undefined) return;
  if (generated >= committed * MIN_DIRECTIVE_SHARE) return;
  throw new Error(
    `generated ${generated} module directives, the committed reference has ${committed}: ` +
      'a drop this large means registrations moved to a source file the generator does not ' +
      'read (see the file list at the top of scripts/generate-references.mjs). ' +
      'If the drop is real, rerun with --allow-shrink.',
  );
}

export function serialize(obj) {
  return `${JSON.stringify(obj, null, 2)}\n`;
}

function main(argv) {
  const args = new Map();
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) {
      const next = argv[i + 1];
      if (next && !next.startsWith('--')) {
        args.set(argv[i], next);
        i++;
      } else args.set(argv[i], true);
    }
  }
  const moduleSrc = args.get('--module-src');
  if (typeof moduleSrc !== 'string') {
    console.error(
      'usage: generate-references.mjs --module-src DIR [--out DIR] [--tag vX.Y.Z] [--check] [--allow-shrink]',
    );
    process.exit(2);
  }
  const outDir = resolve(typeof args.get('--out') === 'string' ? args.get('--out') : REFERENCE_DIR);
  // Every guard reports the same way: one `error:` line and exit 1.
  let generated;
  try {
    generated = generate({
      moduleSrc: resolve(moduleSrc),
      tag: typeof args.get('--tag') === 'string' ? args.get('--tag') : undefined,
    });
  } catch (e) {
    console.error(`error: ${e.message}`);
    process.exit(1);
  }
  const { files, warnings } = generated;
  for (const w of warnings) console.error(`warning: ${w}`);

  if (!args.get('--allow-shrink')) {
    const committedPath = join(REFERENCE_DIR, 'module-directives.json');
    if (!existsSync(committedPath)) {
      console.error(`warning: no committed ${committedPath}; the directive count is not checked`);
    }
    const committed = existsSync(committedPath)
      ? JSON.parse(read(committedPath)).directives.length
      : null;
    try {
      checkDirectiveCount(files['module-directives.json'].directives.length, committed);
    } catch (e) {
      console.error(`error: ${e.message}`);
      process.exit(1);
    }
  }

  if (args.get('--check')) {
    let stale = 0;
    for (const [name, obj] of Object.entries(files)) {
      const path = join(outDir, name);
      if (!existsSync(path) || read(path) !== serialize(obj)) {
        console.error(`stale: ${name}`);
        stale++;
      }
    }
    if (stale) process.exit(1);
    console.log('reference data is current');
    return;
  }

  mkdirSync(outDir, { recursive: true });
  for (const [name, obj] of Object.entries(files))
    writeFileSync(join(outDir, name), serialize(obj));
  const counts = {
    directives: files['module-directives.json'].directives.length,
    workerFlags: files['worker-flags.json'].flags.length,
    thinDirectives: files['thin-module-directives.json'].directives.length,
    filters: files['filters.json'].filters.length,
    aliases: files['filters.json'].aliases.length,
  };
  console.log(`wrote ${Object.keys(files).length} files to ${outDir}: ${JSON.stringify(counts)}`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
