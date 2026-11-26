// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Removes elements from an nginx list (ngx_list_t) in place.  nginx has no
// such call, and its request-header consumers (proxy, FastCGI, uwsgi, SCGI,
// gRPC) send every element of r->headers_in.headers, whatever its hash, so a
// request header is gone only when it is out of the list.  The cached
// pointers in r->headers_in (host, user_agent, if_none_match, ...) point at
// list elements, so no kept element may move: the list is relinked as runs
// of the elements it keeps, each run a part that points into the original
// arrays.
//
// A template over the list, part and element types, so it can be unit-tested
// without nginx (//test/pagespeed/nginx:ngx_list_filter_test); the module
// instantiates it with ngx_list_t, ngx_list_part_t and ngx_table_elt_t.

#ifndef PAGESPEED_NGINX_NGX_LIST_FILTER_H_
#define PAGESPEED_NGINX_NGX_LIST_FILTER_H_

#include <cstddef>
#include <vector>

namespace net_instaweb {

// Removes every element of `list` for which `drop(element)` is true.  `alloc`
// (size in bytes -> void*, nullptr on failure) allocates from the list's
// pool.  When nothing is kept, the list gets a fresh first part with room for
// `empty_capacity` elements.  On a failed allocation the list is left as it
// was and false is returned.  Afterwards the list is valid for ngx_list_push:
// its last part reads as full, so a push starts a new part.
template <typename Elt, typename Part, typename List, typename Drop,
          typename Alloc>
bool NgxListRemoveIf(List* list, Drop drop, Alloc alloc,
                     size_t empty_capacity) {
  struct Run {
    Elt* elts;
    size_t nelts;
  };
  std::vector<Run> runs;
  bool dropped = false;
  for (Part* part = &list->part; part != nullptr; part = part->next) {
    Elt* e = static_cast<Elt*>(part->elts);
    size_t start = 0;
    const size_t n = part->nelts;
    for (size_t i = 0; i < n; ++i) {
      if (drop(e[i])) {
        dropped = true;
        if (i > start) {
          runs.push_back(Run{e + start, i - start});
        }
        start = i + 1;
      }
    }
    if (n > start) {
      runs.push_back(Run{e + start, n - start});
    }
  }
  if (!dropped) {
    return true;
  }

  // Everything is allocated before the list is touched.
  std::vector<Part*> parts;
  void* fresh = nullptr;
  if (runs.empty()) {
    if (empty_capacity == 0) {
      return false;
    }
    fresh = alloc(empty_capacity * list->size);
    if (fresh == nullptr) {
      return false;
    }
  } else {
    parts.reserve(runs.size() - 1);
    for (size_t k = 1; k < runs.size(); ++k) {
      Part* p = static_cast<Part*>(alloc(sizeof(Part)));
      if (p == nullptr) {
        return false;
      }
      parts.push_back(p);
    }
  }

  if (runs.empty()) {
    list->part.elts = fresh;
    list->part.nelts = 0;
    list->part.next = nullptr;
    list->last = &list->part;
    list->nalloc = empty_capacity;
    return true;
  }
  list->part.elts = runs[0].elts;
  list->part.nelts = runs[0].nelts;
  Part* tail = &list->part;
  for (size_t k = 1; k < runs.size(); ++k) {
    Part* p = parts[k - 1];
    p->elts = runs[k].elts;
    p->nelts = runs[k].nelts;
    p->next = nullptr;
    tail->next = p;
    tail = p;
  }
  tail->next = nullptr;
  list->last = tail;
  // A run cut from the middle of an array has no room after it: the last
  // part reads as full, so the next push allocates a part of its own.
  list->nalloc = tail->nelts;
  return true;
}

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_LIST_FILTER_H_
