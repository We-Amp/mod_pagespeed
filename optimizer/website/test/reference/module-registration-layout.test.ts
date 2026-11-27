// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// module-registration-layout.test.ts — the module source layouts the reference
// generator reads directive registrations from.
//
// From mod_pagespeed 2.10.0 the Apache command table is a
// PAGESPEED_APACHE_DIRECTIVE_TABLE(X) macro in pagespeed/apache/
// apache_directive_table.h (its names are `inline constexpr char` constants),
// and the process-scope options the system factory parses live in
// pagespeed/system/system_process_options.cc. Earlier releases keep the table
// in mod_instaweb.cc and the names in system_rewrite_driver_factory.cc. Both
// layouts must parse to the same entries, and a layout the parsers do not know
// must fail the generator loudly instead of silently dropping directives.

import { describe, it, expect } from 'vitest';
import {
  parseApacheCommandTable,
  parseProcessOptionNames,
} from '../../scripts/lib/reference-parsers.mjs';
import {
  checkDirectiveCount,
  requireEntries,
  requireNoProblems,
} from '../../scripts/generate-references.mjs';

// The 2.9.2 layout: the table and its constants in mod_instaweb.cc.
const OLD_MOD_INSTAWEB = `
const char kModPagespeedIf[] = "<ModPagespeedIf";
const char kModPagespeedAllow[] = "ModPagespeedAllow";
const char kModPagespeedFileCachePath[] = "ModPagespeedFileCachePath";
const char kModPagespeedMapOriginDomain[] = "ModPagespeedMapOriginDomain";
const char kModPagespeedCustomFetchHeader[] = "ModPagespeedCustomFetchHeader";
const char kModPagespeedLoadFromFileRule[] = "ModPagespeedLoadFromFileRule";
static const command_rec mod_pagespeed_filter_cmds[] = {
  APACHE_SCOPE_OPTION(kModPagespeedIf, "Conditionally apply some options."),
  APACHE_CONFIG_DIR_OPTION(RewriteQuery::kModPagespeed, "Enable instaweb"),
  APACHE_CONFIG_DIR_OPTION(kModPagespeedAllow, "wildcard_spec for urls"),
  APACHE_CONFIG_OPTION(kModPagespeedFileCachePath, "Directory for the cache"),
  APACHE_CONFIG_FETCH_OPTION2(kModPagespeedCustomFetchHeader, "name value"),
  APACHE_CONFIG_FETCH_OPTION23(kModPagespeedMapOriginDomain, "to from [host]"),
  APACHE_CONFIG_DIR_OPTION3(kModPagespeedLoadFromFileRule, "Allow|Disallow prefix"),
  {NULL}
};
`;

// The 2.10.0 layout: the same table as an X-macro in apache_directive_table.h.
const NEW_DIRECTIVE_TABLE_H = `
inline constexpr char kModPagespeedIf[] = "<ModPagespeedIf";
inline constexpr char kModPagespeedAllow[] = "ModPagespeedAllow";
inline constexpr char kModPagespeedFileCachePath[] =
    "ModPagespeedFileCachePath";
inline constexpr char kModPagespeedMapOriginDomain[] =
    "ModPagespeedMapOriginDomain";
inline constexpr char kModPagespeedCustomFetchHeader[] =
    "ModPagespeedCustomFetchHeader";
inline constexpr char
    kModPagespeedLoadFromFileRule[] =
        "ModPagespeedLoadFromFileRule";
#define PAGESPEED_APACHE_DIRECTIVE_TABLE(X)                                    \\
  /* Special conditional op. */                                                \\
  X(kScope, kModPagespeedIf,                                                   \\
    "Conditionally apply some "                                                \\
    "options.")                                                                \\
  X(kDir1, RewriteQuery::kModPagespeed, "Enable instaweb")                     \\
  X(kDir1, kModPagespeedAllow, "wildcard_spec for urls")                       \\
  X(kServer1, kModPagespeedFileCachePath, "Directory for the cache")           \\
  X(kFetch2, kModPagespeedCustomFetchHeader, "name value")                     \\
  X(kFetch23, kModPagespeedMapOriginDomain, "to from [host]")                  \\
  X(kDir3, kModPagespeedLoadFromFileRule, "Allow|Disallow prefix")
`;

describe('parseApacheCommandTable', () => {
  it('reads the 2.9.2 command table in mod_instaweb.cc', () => {
    // The fetch options (APACHE_CONFIG_FETCH_OPTION*) have never been part of
    // this table parse; the new layout keeps that, so the generated reference
    // does not change with the source layout alone.
    const names = parseApacheCommandTable(OLD_MOD_INSTAWEB).map((c) => c.apacheName);
    expect(names).toEqual([
      '<ModPagespeedIf',
      'ModPagespeed',
      'ModPagespeedAllow',
      'ModPagespeedFileCachePath',
      'ModPagespeedLoadFromFileRule',
    ]);
  });

  it('reads the 2.10.0 X-macro table in apache_directive_table.h', () => {
    expect(parseApacheCommandTable(NEW_DIRECTIVE_TABLE_H).length).toBeGreaterThan(0);
  });

  it('gives both layouts the same entries, field for field', () => {
    expect(parseApacheCommandTable(NEW_DIRECTIVE_TABLE_H)).toEqual(
      parseApacheCommandTable(OLD_MOD_INSTAWEB),
    );
  });
});

// 2.9.2: the names are constants in system_rewrite_driver_factory.cc and the
// factory's ParseAndSetOption1/2 compare them with StringCaseEqual.
// 2.10.0: SystemProcessOptions keeps them in a kOptions table (Parse1) and
// compares CreateSharedMemoryMetadataCache in Parse2.
const NEW_PROCESS_OPTIONS_CC = `
const char kStaticAssetPrefix[] = "StaticAssetPrefix";
const char kNumRewriteThreads[] = "NumRewriteThreads";
const char kForceCaching[] = "ForceCaching";
const char kCreateSharedMemoryMetadataCache[] =
    "CreateSharedMemoryMetadataCache";
const char kAutoThreadCountLiteral[] = "auto";
const NamedOption kOptions[] = {
    {kStaticAssetPrefix, SystemProcessOptions::Option::kStaticAssetPrefix,
     true},
    {kNumRewriteThreads, SystemProcessOptions::Option::kNumRewriteThreads,
     true},
    {kForceCaching, SystemProcessOptions::Option::kForceCaching, false},
};
RewriteOptions::OptionSettingResult SystemProcessOptions::Parse1(
    StringPiece option, StringPiece arg) {
  for (const NamedOption& candidate : kOptions) {
    if (StringCaseEqual(option, candidate.name)) {
    }
  }
  if (StringCaseEqual(arg, kAutoThreadCountLiteral)) {
  }
}
RewriteOptions::OptionSettingResult SystemProcessOptions::Parse2(
    StringPiece option, StringPiece arg1, StringPiece arg2) {
  if (!StringCaseEqual(option, kCreateSharedMemoryMetadataCache)) {
    return RewriteOptions::kOptionNameUnknown;
  }
}
`;

describe('parseApacheCommandTable reports rows it cannot read', () => {
  it('reports a table kind it does not know, but not the skipped fetch kinds', () => {
    const problems: string[] = [];
    const src = NEW_DIRECTIVE_TABLE_H.replace(
      'X(kServer1, kModPagespeedFileCachePath',
      'X(kServer9, kModPagespeedFileCachePath',
    );
    const names = parseApacheCommandTable(src, problems).map((c) => c.name);
    expect(names).not.toContain('FileCachePath');
    expect(problems).toEqual([
      'unknown directive table kind kServer9 (name kModPagespeedFileCachePath)',
    ]);
    const clean: string[] = [];
    parseApacheCommandTable(NEW_DIRECTIVE_TABLE_H, clean);
    expect(clean).toEqual([]);
  });

  it('reports a name it cannot resolve', () => {
    const problems: string[] = [];
    const src = NEW_DIRECTIVE_TABLE_H.replace(
      'X(kDir1, kModPagespeedAllow,',
      'X(kDir1, kModPagespeedNoSuchName,',
    );
    parseApacheCommandTable(src, problems);
    expect(problems).toEqual(['unresolved directive table name kModPagespeedNoSuchName']);
  });

  it('makes the generator refuse a table with unreadable rows, naming the file', () => {
    expect(() =>
      requireNoProblems(
        ['unknown directive table kind kServer9'],
        'pagespeed/apache/apache_directive_table.h',
      ),
    ).toThrow(/apache_directive_table\.h.*kServer9/);
    expect(() => requireNoProblems([], 'any.h')).not.toThrow();
  });
});

describe('parseProcessOptionNames', () => {
  it('reads the process-scope option names of system_process_options.cc', () => {
    expect([...parseProcessOptionNames(NEW_PROCESS_OPTIONS_CC)]).toEqual([
      'StaticAssetPrefix',
      'NumRewriteThreads',
      'ForceCaching',
      'CreateSharedMemoryMetadataCache',
    ]);
  });

  it('refuses a renamed or reformatted kOptions table instead of keeping only Parse2', () => {
    const renamed = NEW_PROCESS_OPTIONS_CC.replace('kOptions[] = {', 'kProcessOptions[] = {');
    expect(() => parseProcessOptionNames(renamed)).toThrow(/kOptions/);
  });

  it('refuses a kOptions row whose name constant does not resolve', () => {
    const broken = NEW_PROCESS_OPTIONS_CC.replace(
      '{kForceCaching, SystemProcessOptions',
      '{kNoSuchConstant, SystemProcessOptions',
    );
    expect(() => parseProcessOptionNames(broken)).toThrow(/kNoSuchConstant/);
  });

  it('does not count argument literals as option names', () => {
    expect(parseProcessOptionNames(NEW_PROCESS_OPTIONS_CC).has('auto')).toBe(false);
  });
});

describe('the generator fails loudly on a layout it cannot read', () => {
  it('refuses a registration file that yields no entries, naming the file', () => {
    expect(() =>
      requireEntries([], 'pagespeed/apache/apache_directive_table.h', 'Apache directives'),
    ).toThrow(/pagespeed\/apache\/apache_directive_table\.h/);
    expect(() => requireEntries(['x'], 'any.cc', 'anything')).not.toThrow();
  });

  it('refuses a sharp drop in the number of directives against the committed reference', () => {
    // checkDirectiveCount(generated, committed)
    expect(() => checkDirectiveCount(295, 400)).toThrow(/295.*400/);
    expect(() => checkDirectiveCount(399, 401)).not.toThrow();
    expect(() => checkDirectiveCount(410, 401)).not.toThrow();
    expect(() => checkDirectiveCount(401, null)).not.toThrow();
  });
});
