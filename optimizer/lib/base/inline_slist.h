// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// A simple intrusive linked list optimized for memory usage,
// cheap appends and traversals (including removals). Links
// are stored within elements rather than externally.

#ifndef PAGESPEED_LIB_BASE_INLINE_SLIST_H_
#define PAGESPEED_LIB_BASE_INLINE_SLIST_H_

#include <cassert>
#include <cstddef>

// For DISALLOW_COPY_AND_ASSIGN - use C++11 delete syntax directly
// instead of depending on basictypes.h to avoid circular dependencies

namespace net_instaweb {

// Forward declaration
template <class T>
class InlineSList;

// A helper base class for things that would get stored in the list.
// You don't have to inherit this, and can implement next() and set_next()
// directly.
template <class T>
class InlineSListElement {
 private:
  // CRTP guard: only the derived class T (and the list) may construct
  // this base, so inheriting with a mismatched T fails to compile.
  friend T;
  InlineSListElement() : next_(nullptr) {}

  friend class InlineSList<T>;
  T* next() { return next_; }
  void set_next(T* new_next) { next_ = new_next; }

  T* next_;
  InlineSListElement(const InlineSListElement&) = delete;
  void operator=(const InlineSListElement&) = delete;
};

// A simple linked list that's optimized for memory usage,
// cheap appends and traversals (including removals). Links
// are stored within elements rather than externally.
//
// To permit that, the type T must provide next() and set_next() methods,
// accessible to InlineSList<T>. Easy way to do that is by inheriting off
// InlineSListElement<T>.
//
// Note that while this results in a list object that's just one pointer wide,
// iterators are two pointers wide.
//
// Representation: circular linked list with a pointer to tail. Iterators
// store pointers to nodes before the one they're conceptually targeting.
template <class T>
class InlineSList {
 private:
  // This private class has to be above the public: section
  // since public classes inherit off it.
  //
  // We represent an iterator by keeping a pointer to the node before
  // the one it represents, which makes it easy to delete things.
  //
  // We also keep a pointer to the containing list, so we can
  // detect when we walk past the end of the list, at which point we
  // turn the node_ pointer into nullptr. (Which also means the begin
  // iterator for an empty list is a one-past-end iterator, as expected).
  class IterBase {
   protected:
    IterBase(const InlineSList<T>* list, T* node) : list_(list), node_(node) {}

    bool AtEnd() const { return (node_ == nullptr); }

    void Advance() {
      assert(!AtEnd());
      node_ = node_->next();
      // If we travel to the tail node (as opposed to start pointing to it),
      // we have reached the end, and became one-past-the-end iterator.
      if (node_ == list_->tail_) {
        node_ = nullptr;
      }
    }

    T* Data() { return node_->next(); }

    bool Equals(const IterBase& other) const {
      return (node_ == other.node_) && (list_ == other.list_);
    }

   private:
    friend class InlineSList<T>;
    const InlineSList<T>* list_;
    T* node_;
  };

 public:
  // Iterator interface to the list contents. You may use this both for simple
  // enumeration and for deletion. Iteration works the same as with any STL
  // container.
  //
  // If you want to remove things, make sure not to call the operator ++
  // when you do, as after deletion the iterator will be pointing at the next
  // element already (or past the end!). An example of doing it right:
  //
  // InlineSList<Type>::iterator iter(list.begin());
  // while (iter != list.end()) {
  //   if (ShouldErase(*iter)) {
  //     list.Erase(&iter);
  //   } else {
  //     ++iter;
  //   }
  // }
  //
  // This type does not make general guarantees of iterators staying valid on
  // operations --- only the iterator passed to Erase() will be fixed, not
  // any others; and Append operations should not be done concurrent with
  // iteration.
  class Iterator : public IterBase {
   public:
    Iterator& operator++() {
      this->Advance();
      return *this;
    }

    T* Get() { return this->Data(); }
    T* operator->() { return this->Data(); }
    T& operator*() { return *this->Data(); }
    bool operator==(const Iterator& other) const { return this->Equals(other); }
    bool operator!=(const Iterator& other) const {
      return !this->Equals(other);
    }
    // default copy op, dtor are OK.

   private:
    friend class InlineSList<T>;
    Iterator(const InlineSList<T>* list, T* prev) : IterBase(list, prev) {}
  };

  typedef Iterator iterator;

  // Read-only iterator type; cannot be used for deletion or to modify
  // the contained items.
  class ConstIterator : public IterBase {
   public:
    ConstIterator& operator++() {
      this->Advance();
      return *this;
    }

    const T* Get() { return this->Data(); }
    const T* operator->() { return this->Data(); }
    const T& operator*() { return *this->Data(); }
    bool operator==(const ConstIterator& other) const {
      return this->Equals(other);
    }
    bool operator!=(const ConstIterator& other) const {
      return !this->Equals(other);
    }
    // default copy op, dtor are OK.

   private:
    friend class InlineSList<T>;
    ConstIterator(const InlineSList<T>* list, T* prev) : IterBase(list, prev) {}
  };

  typedef ConstIterator const_iterator;

  InlineSList() : tail_(nullptr) {}

  // The destructor deletes all the nodes in the list.
  ~InlineSList();

  bool IsEmpty() const { return (tail_ == nullptr); }

  void Append(T* node);

  // Removes the item pointed to by the iterator, and updates the iterator
  // to point after it. Note that this means that it is now effectively
  // advanced (potentially past the end of the list) and that you should not
  // call ++ if you just want to consume one item.
  // See the iterator docs for example of proper use.
  void Erase(Iterator* iter);

  // Returns last item.
  T* Last() {
    assert(!IsEmpty());
    return tail_;
  }

  const T* Last() const {
    assert(!IsEmpty());
    return tail_;
  }

  // Iterator interface.

  // Note that all of these pass tail_ since iterator implementation internally
  // keeps track of the /previous/ node to the one pointed at.
  iterator begin() { return Iterator(this, tail_); }
  const_iterator begin() const { return ConstIterator(this, tail_); }

  // End iterators have their position at nullptr.
  iterator end() { return Iterator(this, nullptr); }
  const_iterator end() const { return ConstIterator(this, nullptr); }

 private:
  // The representation we chose here is a circular linked list where we point
  // at the tail. This is because that permits us to append in O(1) to the end,
  // yet still have easy front-to-end traversal.
  T* tail_;

  InlineSList(const InlineSList&) = delete;
  void operator=(const InlineSList&) = delete;
};

template <class T>
inline InlineSList<T>::~InlineSList() {
  if (tail_ != nullptr) {
    T* node = tail_->next();  // start at head node.
    while (true) {
      T* next = node->next();
      delete node;
      if (node == tail_) {  // stop when we deleted tail.
        break;
      } else {
        node = next;
      }
    }
  }
  tail_ = nullptr;
}

template <class T>
inline void InlineSList<T>::Append(T* node) {
  if (tail_ == nullptr) {
    tail_ = node;
    node->set_next(node);
  } else {
    node->set_next(tail_->next());
    tail_->set_next(node);
    tail_ = node;
  }
}

template <class T>
inline void InlineSList<T>::Erase(Iterator* iter) {
  assert(!iter->AtEnd());

  T* iter_node = iter->node_;
  T* target_node = iter_node->next();

  if (iter_node == target_node) {
    // Only 1 element before the call, 0 now.
    tail_ = nullptr;
    iter->node_ = nullptr;
  } else {
    iter_node->set_next(target_node->next());
    if (target_node == tail_) {
      // Removed tail.. need to point it earlier.
      tail_ = iter_node;
      // Iterator is now one-past-end
      iter->node_ = nullptr;
    }
  }
  delete target_node;
}

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_INLINE_SLIST_H_
