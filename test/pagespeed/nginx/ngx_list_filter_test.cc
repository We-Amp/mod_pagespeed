// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// NgxListRemoveIf on structs laid out like ngx_list_t (nginx itself cannot be
// linked into a test), with a push that does what ngx_list_push does.

#include "pagespeed/nginx/ngx_list_filter.h"

#include <cstdlib>
#include <memory>
#include <vector>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

struct Elt {
  int id;
  bool drop;
};

struct Part {
  void* elts;
  size_t nelts;
  Part* next;
};

struct List {
  Part* last;
  Part part;
  size_t size;
  size_t nalloc;
};

class NgxListFilterTest : public testing::Test {
 protected:
  void* Alloc(size_t bytes) {
    if (fail_allocs_) {
      return nullptr;
    }
    blocks_.emplace_back(new char[bytes]);
    return blocks_.back().get();
  }

  void Init(size_t nalloc) {
    list_.part.elts = Alloc(nalloc * sizeof(Elt));
    list_.part.nelts = 0;
    list_.part.next = nullptr;
    list_.last = &list_.part;
    list_.size = sizeof(Elt);
    list_.nalloc = nalloc;
  }

  // ngx_list_push.
  Elt* Push(int id, bool drop = false) {
    Part* last = list_.last;
    if (last->nelts == list_.nalloc) {
      last = static_cast<Part*>(Alloc(sizeof(Part)));
      last->elts = Alloc(list_.nalloc * list_.size);
      last->nelts = 0;
      last->next = nullptr;
      list_.last->next = last;
      list_.last = last;
    }
    Elt* e = static_cast<Elt*>(last->elts) + last->nelts;
    ++last->nelts;
    e->id = id;
    e->drop = drop;
    return e;
  }

  bool RemoveDropped(size_t empty_capacity = 4) {
    return NgxListRemoveIf<Elt, Part>(
        &list_, [](const Elt& e) { return e.drop; },
        [this](size_t bytes) { return Alloc(bytes); }, empty_capacity);
  }

  // The ids in iteration order, walked the way nginx walks a list (which
  // reads element 0 of every part after the first without a check).
  std::vector<int> Ids() {
    std::vector<int> ids;
    Part* part = &list_.part;
    Elt* e = static_cast<Elt*>(part->elts);
    for (size_t i = 0;; ++i) {
      if (i >= part->nelts) {
        if (part->next == nullptr) {
          break;
        }
        part = part->next;
        e = static_cast<Elt*>(part->elts);
        i = 0;
        EXPECT_GT(part->nelts, 0u) << "an empty part after the first";
      }
      ids.push_back(e[i].id);
    }
    return ids;
  }

  List list_;
  bool fail_allocs_ = false;
  std::vector<std::unique_ptr<char[]>> blocks_;
};

TEST_F(NgxListFilterTest, NothingDroppedLeavesTheList) {
  Init(4);
  Push(1);
  Push(2);
  Part* last = list_.last;
  EXPECT_TRUE(RemoveDropped());
  EXPECT_EQ(last, list_.last);
  EXPECT_EQ(4u, list_.nalloc);
  EXPECT_EQ((std::vector<int>{1, 2}), Ids());
}

TEST_F(NgxListFilterTest, KeptElementsDoNotMove) {
  Init(3);
  Elt* a = Push(1);
  Push(2, true);
  Elt* c = Push(3);
  Elt* d = Push(4);  // second part
  Push(5, true);
  Elt* f = Push(6);
  Push(7, true);  // third part, alone
  EXPECT_TRUE(RemoveDropped());
  EXPECT_EQ((std::vector<int>{1, 3, 4, 6}), Ids());
  EXPECT_EQ(1, a->id);
  EXPECT_EQ(3, c->id);
  EXPECT_EQ(4, d->id);
  EXPECT_EQ(6, f->id);
}

TEST_F(NgxListFilterTest, FirstAndLastElements) {
  Init(8);
  Push(1, true);
  Push(2);
  Push(3);
  Push(4, true);
  EXPECT_TRUE(RemoveDropped());
  EXPECT_EQ((std::vector<int>{2, 3}), Ids());
}

TEST_F(NgxListFilterTest, AWholeFirstPartDropped) {
  Init(2);
  Push(1, true);
  Push(2, true);
  Push(3);
  EXPECT_TRUE(RemoveDropped());
  EXPECT_EQ((std::vector<int>{3}), Ids());
}

TEST_F(NgxListFilterTest, EverythingDropped) {
  Init(2);
  Push(1, true);
  Push(2, true);
  Push(3, true);
  EXPECT_TRUE(RemoveDropped(5));
  EXPECT_EQ(std::vector<int>{}, Ids());
  EXPECT_EQ(&list_.part, list_.last);
  EXPECT_EQ(5u, list_.nalloc);
  for (int i = 10; i < 17; ++i) {
    Push(i);
  }
  EXPECT_EQ((std::vector<int>{10, 11, 12, 13, 14, 15, 16}), Ids());
}

// Pushing after a removal never writes past a part, nor over a kept element.
TEST_F(NgxListFilterTest, PushAfterRemoval) {
  Init(4);
  Push(1);
  Push(2, true);
  Push(3);
  Elt* kept = Push(4);
  EXPECT_TRUE(RemoveDropped());
  for (int i = 10; i < 20; ++i) {
    Push(i);
  }
  EXPECT_EQ(4, kept->id);
  EXPECT_EQ((std::vector<int>{1, 3, 4, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19}),
            Ids());
  // And a second removal on the relinked list.
  for (Part* p = &list_.part; p != nullptr; p = p->next) {
    Elt* e = static_cast<Elt*>(p->elts);
    for (size_t i = 0; i < p->nelts; ++i) {
      e[i].drop = e[i].id % 2 == 1;
    }
  }
  EXPECT_TRUE(RemoveDropped());
  EXPECT_EQ((std::vector<int>{4, 10, 12, 14, 16, 18}), Ids());
  Push(30);
  EXPECT_EQ((std::vector<int>{4, 10, 12, 14, 16, 18, 30}), Ids());
}

TEST_F(NgxListFilterTest, FailedAllocationLeavesTheList) {
  Init(4);
  Push(1);
  Push(2, true);
  Push(3);
  fail_allocs_ = true;
  EXPECT_FALSE(RemoveDropped());
  fail_allocs_ = false;
  EXPECT_EQ((std::vector<int>{1, 2, 3}), Ids());

  Init(4);
  Push(1, true);
  fail_allocs_ = true;
  EXPECT_FALSE(RemoveDropped());
  fail_allocs_ = false;
  EXPECT_EQ((std::vector<int>{1}), Ids());
}

}  // namespace
}  // namespace net_instaweb
