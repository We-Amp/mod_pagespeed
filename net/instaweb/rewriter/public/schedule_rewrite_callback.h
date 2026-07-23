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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_SCHEDULE_REWRITE_CALLBACK_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_SCHEDULE_REWRITE_CALLBACK_H_

#include <memory>

#include "net/instaweb/rewriter/public/sequence_bound_callback.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/thread/sequence.h"

// Callback classes to support per-key rewrite scheduling via
// NamedLockScheduleRewriteController.

namespace net_instaweb {

// Passed to RunImpl for implementations of ScheduleRewriteCallback.
class ScheduleRewriteContext {
 public:
  virtual ~ScheduleRewriteContext();

  // Mark the rewrite operation as complete. MarkSucceeded will be
  // automatically invoked at destruction if neither is explicitly called.
  virtual void MarkSucceeded() = 0;
  virtual void MarkFailed() = 0;

 protected:
  ScheduleRewriteContext();

 private:
  ScheduleRewriteContext(const ScheduleRewriteContext&) = delete;
  ScheduleRewriteContext& operator=(const ScheduleRewriteContext&) = delete;
};

// Implementor interface for rewrite scheduling.
class ScheduleRewriteCallback
    : public SequenceBoundCallback<ScheduleRewriteContext> {
 public:
  explicit ScheduleRewriteCallback(const GoogleString& key, Sequence* sequence);
  ~ScheduleRewriteCallback() override;

  const GoogleString& key() { return key_; }

 private:
  // SequenceBoundCallback interface.
  void RunImpl(std::unique_ptr<ScheduleRewriteContext>* context) override = 0;
  void CancelImpl() override = 0;

  GoogleString key_;

  ScheduleRewriteCallback(const ScheduleRewriteCallback&) = delete;
  ScheduleRewriteCallback& operator=(const ScheduleRewriteCallback&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_SCHEDULE_REWRITE_CALLBACK_H_
