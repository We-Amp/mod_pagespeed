// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/base/stl_util.h header, mirroring its
// implementation exactly.
//
// Consumed surface (verified at pin 61618a0c): canonical html_parse.cc calls
// STLDeleteElements(&queue_) in ~HtmlParse and STLDeleteElements(events) in
// ClearElements — the HtmlEvent queue is RAW-OWNED on both sides (mpp #670
// modernized event_listeners_/deferred_nodes_ to unique_ptr but deliberately
// left the event queue; any change is deferred to a coordinated
// both-repos conversion). STLDeleteContainerPointers is included because
// canonical factors STLDeleteElements through it. STLFind/STLDeleteValues
// are NOT provided (nothing in the vendored set consumes them); a future
// re-sync that uses them fails the build here — extend deliberately, with
// the same semantics audit as the other compat headers.
//
// Global namespace, exactly like canonical (these are Chromium-era global
// template helpers). Deleting through this header is type-complete-checked at
// the call site, as canonical.

#ifndef PAGESPEED_LIB_HTML_COMPAT_STL_UTIL_H_
#define PAGESPEED_LIB_HTML_COMPAT_STL_UTIL_H_

template <class ForwardIterator>
void STLDeleteContainerPointers(ForwardIterator begin, ForwardIterator end) {
  while (begin != end) {
    ForwardIterator temp = begin;
    ++begin;
    delete *temp;
  }
}

template <class T>
void STLDeleteElements(T* container) {
  if (!container) return;
  STLDeleteContainerPointers(container->begin(), container->end());
  container->clear();
}

#endif  // PAGESPEED_LIB_HTML_COMPAT_STL_UTIL_H_
