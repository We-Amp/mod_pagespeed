/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/kernel/thread/pthread_shared_mem.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <map>
#include <utility>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/abstract_shared_mem.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

// This implementation relies on readonly copies of old memory and shared R/W
// mappings being kept across a fork. It simply stashes addresses of
// shared mmap segments into a map where kid processes can pick them up.

// close() a fd logging failure and dealing with EINTR.
void CheckedClose(int fd, MessageHandler* message_handler) {
  while (close(fd) != 0) {
    if (errno != EINTR) {
      message_handler->Message(kWarning, "Problem closing SHM segment fd:%d",
                               errno);
      return;
    }
  }
}

// Robust mutexes (POSIX.1-2008) let a lock operation take over a mutex whose
// holder terminated while holding it, instead of blocking forever. glibc,
// musl and FreeBSD provide them; macOS does not.
#if defined(__linux__) || defined(__FreeBSD__)
#define PAGESPEED_HAVE_ROBUST_MUTEXES 1
#endif

// Unlike PthreadMutex this doesn't own the lock, but rather refers to an
// external one.
//
// The mutex is robust where the platform supports it (see
// InitializeSharedMutex below): when a process or thread terminates while
// holding it, e.g. a server child process killed by a signal, the next lock
// operation acquires it with EOWNERDEAD. Without that, every thread of every
// process attached to the segment that needs this mutex would block forever.
class PthreadSharedMemMutex : public AbstractMutex {
 public:
  explicit PthreadSharedMemMutex(pthread_mutex_t* external_mutex,
                                 AbstractSharedMemSegment::MutexRecoveryHandler*
                                     recovery_handler = nullptr)
      : external_mutex_(external_mutex), recovery_handler_(recovery_handler) {}

  bool TryLock() override {
    int result = pthread_mutex_trylock(external_mutex_);
    if (result == EOWNERDEAD) {
      RecoverFromOwnerDeath();
      return true;
    }
    return (result == 0);
  }

  void Lock() override {
    int result = pthread_mutex_lock(external_mutex_);
    if (result == EOWNERDEAD) {
      RecoverFromOwnerDeath();
    } else if (result != 0) {
      LOG(DFATAL) << "pthread_mutex_lock failed with error " << result;
    }
  }

  void Unlock() override { pthread_mutex_unlock(external_mutex_); }

 private:
  // We hold the mutex, acquired from a holder that terminated while holding
  // it. Marks it consistent, so that it keeps working after we unlock it
  // (unlocking it without that makes it permanently unusable), and lets the
  // owner of the protected data deal with whatever the holder left behind.
  void RecoverFromOwnerDeath() {
#ifdef PAGESPEED_HAVE_ROBUST_MUTEXES
    // If this failed, unlocking would make the mutex unusable for every
    // process (ENOTRECOVERABLE), and they would all run without exclusion.
    int result = pthread_mutex_consistent(external_mutex_);
    CHECK_EQ(0, result) << "pthread_mutex_consistent failed with error "
                        << result;
#endif
    LOG(WARNING) << "Took over a shared memory mutex whose holder terminated "
                    "while holding it";
    if (recovery_handler_ != nullptr) {
      recovery_handler_->OnOwnerDied();
    }
  }

  pthread_mutex_t* external_mutex_;
  AbstractSharedMemSegment::MutexRecoveryHandler* recovery_handler_;

  PthreadSharedMemMutex(const PthreadSharedMemMutex&) = delete;
  PthreadSharedMemMutex& operator=(const PthreadSharedMemMutex&) = delete;
};

class PthreadSharedMemSegment : public AbstractSharedMemSegment {
 public:
  // We will be representing memory mapped in the [base, base + size) range.
  PthreadSharedMemSegment(char* base, size_t size, MessageHandler* handler)
      : base_(base), size_(size) {}

  ~PthreadSharedMemSegment() override {}

  volatile char* Base() override { return base_; }

  size_t SharedMutexSize() const override { return sizeof(pthread_mutex_t); }

  bool InitializeSharedMutex(size_t offset, MessageHandler* handler) override {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
      handler->Message(kError, "pthread_mutexattr_init failed with errno:%d",
                       errno);
      return false;
    }

    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
      pthread_mutexattr_destroy(&attr);
      handler->Message(
          kError, "pthread_mutexattr_setpshared failed with errno:%d", errno);
      return false;
    }

#ifdef PAGESPEED_HAVE_ROBUST_MUTEXES
    // See PthreadSharedMemMutex. Without this a mutex still works, it just
    // stays locked forever if its holder terminates, so carry on if it fails.
    int robust_result =
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    if (robust_result != 0) {
      handler->Message(kWarning,
                       "pthread_mutexattr_setrobust failed with error:%d",
                       robust_result);
    }
#endif

    if (pthread_mutex_init(MutexPtr(offset), &attr) != 0) {
      pthread_mutexattr_destroy(&attr);
      handler->Message(kError, "pthread_mutex_init failed with errno:%d",
                       errno);
      return false;
    }

    pthread_mutexattr_destroy(&attr);
    return true;
  }

  AbstractMutex* AttachToSharedMutex(size_t offset) override {
    return new PthreadSharedMemMutex(MutexPtr(offset));
  }

  AbstractMutex* AttachToSharedMutexWithRecovery(
      size_t offset, MutexRecoveryHandler* handler) override {
    return new PthreadSharedMemMutex(MutexPtr(offset), handler);
  }

 private:
  pthread_mutex_t* MutexPtr(size_t offset) {
    return reinterpret_cast<pthread_mutex_t*>(base_ + offset);
  }

  char* const base_;
  const size_t size_;

  PthreadSharedMemSegment(const PthreadSharedMemSegment&) = delete;
  PthreadSharedMemSegment& operator=(const PthreadSharedMemSegment&) = delete;
};

pthread_mutex_t segment_bases_lock = PTHREAD_MUTEX_INITIALIZER;

}  // namespace

size_t PthreadSharedMem::s_instance_count_ = 0;

PthreadSharedMem::SegmentBaseMap* PthreadSharedMem::segment_bases_ = nullptr;

PthreadSharedMem::PthreadSharedMem() { instance_number_ = ++s_instance_count_; }

PthreadSharedMem::~PthreadSharedMem() {}

bool PthreadSharedMem::RecoversAbandonedMutexes() {
#ifdef PAGESPEED_HAVE_ROBUST_MUTEXES
  return true;
#else
  return false;
#endif
}

size_t PthreadSharedMem::SharedMutexSize() const {
  return sizeof(pthread_mutex_t);
}

AbstractSharedMemSegment* PthreadSharedMem::CreateSegment(
    const GoogleString& name, size_t size, MessageHandler* handler) {
  GoogleString prefixed_name = PrefixSegmentName(name);
  // Create the memory
  int fd = open("/dev/zero", O_RDWR);
  if (fd == -1) {
    handler->Message(
        kError,
        "Unable to create SHM segment %s, open of /dev/zero failed "
        "with errno=%d.",
        prefixed_name.c_str(), errno);
    return nullptr;
  }

  // map it
  char* base = reinterpret_cast<char*>(
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  int mmap_errno = errno;
  CheckedClose(fd, handler);
  if (base == MAP_FAILED) {
    handler->Message(
        kError, "Unable to create SHM segment %s, mmap failed with errno=%d.",
        prefixed_name.c_str(), mmap_errno);
    return nullptr;
  }

  SegmentBaseMap* bases = AcquireSegmentBases();
  (*bases)[prefixed_name] = std::make_pair(base, size);
  UnlockSegmentBases();
  return new PthreadSharedMemSegment(base, size, handler);
}

AbstractSharedMemSegment* PthreadSharedMem::AttachToSegment(
    const GoogleString& name, size_t size, MessageHandler* handler) {
  GoogleString prefixed_name = PrefixSegmentName(name);
  SegmentBaseMap* bases = AcquireSegmentBases();
  SegmentBaseMap::const_iterator i = bases->find(prefixed_name);
  if (i == bases->end()) {
    handler->Message(kError, "Unable to find SHM segment %s to attach to.",
                     prefixed_name.c_str());
    UnlockSegmentBases();
    return nullptr;
  }
  char* base = i->second.first;
  DCHECK_EQ(size, i->second.second);
  UnlockSegmentBases();
  return new PthreadSharedMemSegment(base, size, handler);
}

void PthreadSharedMem::DestroySegment(const GoogleString& name,
                                      MessageHandler* handler) {
  GoogleString prefixed_name = PrefixSegmentName(name);
  // Note that in the process state children will not see any mutations
  // we make here, so it acts mostly for checking in that case.
  SegmentBaseMap* bases = AcquireSegmentBases();
  SegmentBaseMap::iterator i = bases->find(prefixed_name);
  if (i != bases->end()) {
    // Note that we must munmap the segment here in order to not leak like crazy
    // for things like apache2ctrl graceful (and similar nginx configuration).
    munmap(i->second.first, i->second.second);
    bases->erase(i);
    if (bases->empty()) {
      delete segment_bases_;
      segment_bases_ = nullptr;
    }
  } else {
    handler->Message(kError, "Attempt to destroy unknown SHM segment %s.",
                     prefixed_name.c_str());
  }
  UnlockSegmentBases();
}

PthreadSharedMem::SegmentBaseMap* PthreadSharedMem::AcquireSegmentBases() {
  PthreadSharedMemMutex lock(&segment_bases_lock);
  lock.Lock();

  if (segment_bases_ == nullptr) {
    segment_bases_ = new SegmentBaseMap();
  }

  return segment_bases_;
}

void PthreadSharedMem::UnlockSegmentBases() {
  PthreadSharedMemMutex lock(&segment_bases_lock);
  lock.Unlock();
}

GoogleString PthreadSharedMem::PrefixSegmentName(const GoogleString& name) {
  GoogleString res;
  StrAppend(&res, "[", IntegerToString(instance_number_), "]", name);
  return res;
}

void PthreadSharedMem::Terminate() {
  // Clean up the local memory associated with the maps to shared memory
  // storage.
  PthreadSharedMemMutex lock(&segment_bases_lock);
  lock.Lock();
  if (segment_bases_ != nullptr) {
    delete segment_bases_;
    segment_bases_ = nullptr;
  }
  lock.Unlock();
}

}  // namespace net_instaweb
