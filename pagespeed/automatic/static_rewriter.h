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

#ifndef PAGESPEED_AUTOMATIC_STATIC_REWRITER_H_
#define PAGESPEED_AUTOMATIC_STATIC_REWRITER_H_

#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_gflags.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/writer.h"
#include "pagespeed/kernel/util/simple_stats.h"

namespace net_instaweb {

class FileSystem;
class Hasher;
class MessageHandler;
class NamedLockManager;
class ProcessContext;
class RewriteOptions;
class ServerContext;
class UrlAsyncFetcher;

// Implements a baseline RewriteDriverFactory with the simplest possible
// options for cache, fetchers, & system interface.
//
// TODO(jmarantz): fill out enough functionality so that this will be
// a functional static rewriter that could optimize an HTML file
// passed as a command-line parameter or via stdin.
class FileRewriter : public RewriteDriverFactory {
 public:
  FileRewriter(const ProcessContext& process_context,
               const RewriteGflags* gflags, bool echo_errors_to_stdout);
  ~FileRewriter() override;
  NamedLockManager* DefaultLockManager() override;
  RewriteOptions* NewRewriteOptions() override;
  Hasher* NewHasher() override;
  UrlAsyncFetcher* DefaultAsyncUrlFetcher() override;
  MessageHandler* DefaultHtmlParseMessageHandler() override;
  MessageHandler* DefaultMessageHandler() override;
  FileSystem* DefaultFileSystem() override;
  void SetupCaches(ServerContext* server_context) override;
  virtual Statistics* statistics();
  ServerContext* NewServerContext() override;
  ServerContext* NewDecodingServerContext() override;
  bool UseBeaconResultsInFilters() const override { return false; }

 private:
  const RewriteGflags* gflags_;
  SimpleStats simple_stats_;
  bool echo_errors_to_stdout_;

  FileRewriter(const FileRewriter&) = delete;
  FileRewriter& operator=(const FileRewriter&) = delete;
};

// Encapsulates the instantiation of a FileRewriter & a simple one-shot
// interface to rewrite some HTML text.
class StaticRewriter {
 public:
  StaticRewriter(const ProcessContext& process_context, int* argc,
                 char*** argv);
  explicit StaticRewriter(const ProcessContext& process_context);
  ~StaticRewriter();

  bool ParseText(const StringPiece& url, const StringPiece& id,
                 const StringPiece& text, const StringPiece& output_dir,
                 Writer* writer);

  FileSystem* file_system();
  MessageHandler* message_handler();

 private:
  RewriteGflags gflags_;
  FileRewriter file_rewriter_;
  ServerContext* server_context_;

  StaticRewriter(const StaticRewriter&) = delete;
  StaticRewriter& operator=(const StaticRewriter&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_AUTOMATIC_STATIC_REWRITER_H_
