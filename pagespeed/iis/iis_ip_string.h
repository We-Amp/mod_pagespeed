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

#ifndef PAGESPEED_IIS_IIS_IP_STRING_H_
#define PAGESPEED_IIS_IIS_IP_STRING_H_

// Keep <windows.h> (pulled in transitively by IIS translation units) from
// dragging in the legacy <winsock.h> ahead of <winsock2.h>.
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
// Order matters: <winsock2.h> must precede <ws2tcpip.h>.  Including both here
// keeps consumers (and clang-format's include sorting) from accidentally
// pulling <ws2tcpip.h> in first.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

// Renders a client/server socket address as a printable IP string, dispatching
// on sa_family (AF_INET / AF_INET6) and reinterpreting as SOCKADDR_IN6 for
// AF_INET6 so the v6 address is read from sin6_addr at the correct offset.
//
// Returns an empty string for a null pointer, for an all-zero IPv4 / unspecified
// IPv6 address (callers treat that as "connect back on 127.0.0.1"), and for an
// unrecognized address family.  Mirrors GetClientIp() in iis_admin_handler.cc.
GoogleString GetIPString(SOCKADDR* addr);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_IP_STRING_H_
