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

#ifndef PAGESPEED_APACHE_LOG_MESSAGE_HANDLER_H_
#define PAGESPEED_APACHE_LOG_MESSAGE_HANDLER_H_

#include <algorithm>  // for std::min

#include "apr_pools.h"
#include "pagespeed/kernel/base/string_util.h"

struct server_rec;

namespace net_instaweb {

namespace log_message_handler {

// Install a log message handler that routes LOG() messages to the
// apache error log. Should be called once at startup.
void Install(apr_pool_t* pool);

// The log_message_handler is not attached to a specific server_rec, so the
// LogLevel is not automatically set for it. Every server_rec instance
// should call AddServerConfig and let us decide what level to log at.
// Currently we set it to the min LogLevel.
void AddServerConfig(const server_rec* server, const StringPiece& version);

// Maps a base/logging.h severity to the Apache log level it is emitted at.
// LOG(INFO/WARNING/ERROR/FATAL) keep their established levels. Severities
// below LOG_INFO are the verbose VLOG(n) ones (glog convention: VLOG(n)
// carries severity -n); they map to APLOG_DEBUG so that verbose
// diagnostics only surface when the server's LogLevel is debug. Exposed
// for unit tests.
int GetApacheLogLevel(int severity);

// Free the memory from the log message handler
void ShutDown();

}  // namespace log_message_handler

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_LOG_MESSAGE_HANDLER_H_
