// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The experiment arm assigned to a request is remembered and given to every
// later options clone of the same request.

#include "pagespeed/iis/iis_experiment_assignment.h"

#include <memory>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/experiment_util.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/kernel/base/null_message_handler.h"

namespace net_instaweb {
namespace {

class IisExperimentAssignmentTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { IisRewriteOptions::Initialize(); }
  static void TearDownTestSuite() { IisRewriteOptions::Terminate(); }

  // An options object running a two-arm experiment: arm 1 enables
  // extend_cache, arm 2 enables combine_css.
  std::unique_ptr<IisRewriteOptions> NewExperimentOptions() {
    auto options = std::make_unique<IisRewriteOptions>(nullptr);
    options->SetRewriteLevel(RewriteOptions::kPassThrough);
    options->set_running_experiment(true);
    NullMessageHandler handler;
    EXPECT_TRUE(options->AddExperimentSpec(
        "id=1;percent=50;enable=extend_cache", &handler));
    EXPECT_TRUE(options->AddExperimentSpec(
        "id=2;percent=50;enable=combine_css", &handler));
    return options;
  }
};

TEST_F(IisExperimentAssignmentTest, NothingAssignedYet) {
  IisExperimentAssignment assignment;
  EXPECT_FALSE(assignment.assigned());
  EXPECT_EQ(experiment::kExperimentNotSet, assignment.state());
  auto options = NewExperimentOptions();
  EXPECT_FALSE(assignment.ApplyTo(options.get()));
  EXPECT_EQ(experiment::kExperimentNotSet, options->experiment_id());
}

// The clone that rewrites the HTML gets the arm of the first pass, with the
// arm's filters, instead of no arm at all.
TEST_F(IisExperimentAssignmentTest, LaterCloneGetsTheAssignedArm) {
  auto first = NewExperimentOptions();
  first->SetExperimentState(2);
  IisExperimentAssignment assignment;
  assignment.Record(*first);
  EXPECT_TRUE(assignment.assigned());
  EXPECT_EQ(2, assignment.state());

  auto clone = NewExperimentOptions();
  EXPECT_EQ(experiment::kExperimentNotSet, clone->experiment_id());
  EXPECT_FALSE(clone->Enabled(RewriteOptions::kCombineCss));

  EXPECT_TRUE(assignment.ApplyTo(clone.get()));
  EXPECT_EQ(2, clone->experiment_id());
  EXPECT_TRUE(clone->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(clone->Enabled(RewriteOptions::kExtendCacheCss));
}

// A visitor assigned to no experiment stays out of it on every clone.
TEST_F(IisExperimentAssignmentTest, NoExperimentArmIsKept) {
  auto first = NewExperimentOptions();
  first->SetExperimentState(experiment::kNoExperiment);
  IisExperimentAssignment assignment;
  assignment.Record(*first);

  auto clone = NewExperimentOptions();
  EXPECT_TRUE(assignment.ApplyTo(clone.get()));
  EXPECT_EQ(experiment::kNoExperiment, clone->experiment_id());
  EXPECT_FALSE(clone->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(clone->Enabled(RewriteOptions::kExtendCacheCss));
}

// Options fetched from a remote configuration source are merged into the
// request's options after the arm was assigned. The merge resets the arm; the
// shared helper puts it back, with the arm's filters winning over the remote
// ones (arm 2 is not the default arm), as on the nginx port.
TEST_F(IisExperimentAssignmentTest, ArmSurvivesMergeOfRemoteOptions) {
  auto request_options = NewExperimentOptions();
  request_options->SetExperimentState(2);
  IisExperimentAssignment assignment;
  assignment.Record(*request_options);

  auto remote_options = NewExperimentOptions();
  remote_options->EnableFilter(RewriteOptions::kExtendCacheCss);
  MergeRemoteOptions(*remote_options, assignment, request_options.get());

  EXPECT_EQ(2, request_options->experiment_id());
  EXPECT_EQ("b", request_options->GetExperimentStateStr());
  EXPECT_TRUE(request_options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(request_options->Enabled(RewriteOptions::kExtendCacheCss));
}

// A remote configuration that turns the experiment off wins: the arm is not
// imposed again.
TEST_F(IisExperimentAssignmentTest, RemoteRunExperimentOffGetsNoArmBack) {
  auto request_options = NewExperimentOptions();
  request_options->SetExperimentState(2);
  IisExperimentAssignment assignment;
  assignment.Record(*request_options);

  auto remote_options = NewExperimentOptions();
  remote_options->set_running_experiment(false);
  MergeRemoteOptions(*remote_options, assignment, request_options.get());

  EXPECT_FALSE(request_options->running_experiment());
  EXPECT_NE(2, request_options->experiment_id());
  EXPECT_EQ("", request_options->GetExperimentStateStr());
}

}  // namespace
}  // namespace net_instaweb
