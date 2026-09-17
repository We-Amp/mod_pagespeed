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

// Port-parity hygiene for the cross-port init-time filesystem-prep
// virtuals on RewriteDriverFactory. The Apache, nginx, and Envoy ports
// inherit the base-class defaults; the IIS port overrides them. These pin the
// base defaults so a future POSIX-port override that breaks the no-op
// contract is caught at test time, even though Apache/nginx/Envoy do not
// (yet) have factory-level unit-test targets in-tree.

#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"

#include "pagespeed/kernel/base/string.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

// Uses RewriteTestBase purely for its TestRewriteDriverFactory plumbing.
// TestRewriteDriverFactory does not override those virtuals, so calling
// them through factory() exercises the base-class defaults that
// Apache/nginx/Envoy inherit.
class RewriteDriverFactoryTest : public RewriteTestBase {};

TEST_F(RewriteDriverFactoryTest, EnsureDirectoryWritableDefaultIsNoOp) {
  // Apache/nginx/Envoy factories inherit the base default
  // (return true, error_message untouched). The IIS factory overrides
  // with mkdir+ACL semantics. This test pins the base default so a
  // future override that breaks the POSIX-port no-op contract is
  // caught at test time.
  GoogleString err = "untouched";
  EXPECT_TRUE(
      factory()->EnsureDirectoryWritable("/tmp/does-not-need-to-exist", &err));
  EXPECT_EQ("untouched", err);
}

TEST_F(RewriteDriverFactoryTest, EnsureDirectoryWritableNullErrorMessage) {
  // The hook contract permits a null error_message pointer (callers that
  // don't care about the diagnostic, e.g. unconditional success paths).
  // The base default must not dereference it.
  EXPECT_TRUE(factory()->EnsureDirectoryWritable("/tmp/does-not-need-to-exist",
                                                 /*error_message=*/nullptr));
}

TEST_F(RewriteDriverFactoryTest, IsPathInAutoCreatePrefixDefaultIsTrue) {
  // POSIX ports' directive-parser mkdir handles the full
  // FileCachePath at config-parse time, so the prefix gate at
  // server-context init returns true (auto-create skipped, but the
  // path was already prepared). Pin the default for any path shape.
  EXPECT_TRUE(factory()->IsPathInAutoCreatePrefix("/tmp/whatever"));
  EXPECT_TRUE(factory()->IsPathInAutoCreatePrefix("/var/cache/anything"));
  EXPECT_TRUE(factory()->IsPathInAutoCreatePrefix(""));
}

// TODO: IsLogDirInAutoCreatePrefix has since landed; add the parallel
// default-true assertion for it here.

}  // namespace

}  // namespace net_instaweb
