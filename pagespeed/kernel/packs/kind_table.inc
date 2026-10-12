// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The one list of rule kinds, in the order they are applied to a page: a
// later kind sees the final values of the earlier ones. Included with
// RULE_KIND defined; there is no include guard on purpose.
//
//          name         class            header
// (The header column is documentation: kind_registry.cc includes each header
// by hand, and a missing include fails to compile.)
RULE_KIND(canonical,   CanonicalKind,   "pagespeed/kernel/packs/kinds/canonical_kind.h")
RULE_KIND(title,       TitleKind,       "pagespeed/kernel/packs/kinds/title_kind.h")
RULE_KIND(description, DescriptionKind, "pagespeed/kernel/packs/kinds/description_kind.h")
RULE_KIND(hreflang,    HreflangKind,    "pagespeed/kernel/packs/kinds/hreflang_kind.h")
RULE_KIND(jsonld,      JsonLdKind,      "pagespeed/kernel/packs/kinds/jsonld_kind.h")
