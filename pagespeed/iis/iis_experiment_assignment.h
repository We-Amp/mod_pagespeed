// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#ifndef IIS_EXPERIMENT_ASSIGNMENT_H_
#define IIS_EXPERIMENT_ASSIGNMENT_H_

#include "net/instaweb/rewriter/public/experiment_util.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"

namespace net_instaweb {

// The experiment arm a request was assigned to.
//
// The module builds the request's options more than once: once when it first
// looks at the request, and again when the response is rewritten. The visitor
// is assigned an arm (and given the experiment cookie) only once, on the
// first pass, so every later options clone has to be handed the same arm.
// Without that, the clone that rewrites the HTML has no arm: the arm's
// filters are not enabled and the experiment reports nothing.
class IisExperimentAssignment {
 public:
  IisExperimentAssignment()
      : assigned_(false), state_(experiment::kExperimentNotSet) {}

  // True once an arm has been assigned for this request.
  bool assigned() const { return assigned_; }
  int state() const { return state_; }

  // Remembers the arm that |options| was just assigned.
  void Record(const RewriteOptions& options) {
    assigned_ = true;
    state_ = options.experiment_id();
  }

  // Gives |options| the arm recorded earlier. Does nothing, and returns
  // false, when no arm was recorded.
  bool ApplyTo(RewriteOptions* options) const {
    if (!assigned_) {
      return false;
    }
    options->SetExperimentState(state_);
    return true;
  }

 private:
  bool assigned_;
  int state_;
};

// Merges options fetched from a remote configuration source into the
// request's options. The merge resets the experiment id; the request's arm is
// given back unless the remote configuration turned the experiment off.
inline void MergeRemoteOptions(const RewriteOptions& remote,
                               const IisExperimentAssignment& assignment,
                               RewriteOptions* options) {
  options->Merge(remote);
  if (options->running_experiment()) {
    assignment.ApplyTo(options);
  }
}

}  // namespace net_instaweb

#endif  // IIS_EXPERIMENT_ASSIGNMENT_H_
