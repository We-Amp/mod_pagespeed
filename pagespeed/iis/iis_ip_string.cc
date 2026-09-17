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

#include "pagespeed/iis/iis_ip_string.h"

namespace net_instaweb {

GoogleString GetIPString(SOCKADDR* addr) {
  GoogleString ip_address;

  if (addr == NULL) {
    return ip_address;
  }

  if (addr->sa_family == AF_INET) {
    SOCKADDR_IN* addr_in = reinterpret_cast<SOCKADDR_IN*>(addr);
    // A broadcast/all-zero address means we should be able to connect back on
    // 127.0.0.1, so leave the string empty in that case.
    if (addr_in->sin_addr.S_un.S_un_b.s_b1 == 0 &&
        addr_in->sin_addr.S_un.S_un_b.s_b2 == 0 &&
        addr_in->sin_addr.S_un.S_un_b.s_b3 == 0 &&
        addr_in->sin_addr.S_un.S_un_b.s_b4 == 0) {
    } else {
      char str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &(addr_in->sin_addr), str, INET_ADDRSTRLEN);
      ip_address = GoogleString(str);
    }
  } else if (addr->sa_family == AF_INET6) {
    SOCKADDR_IN6* addr_in6 = reinterpret_cast<SOCKADDR_IN6*>(addr);
    // An all-zero (unspecified) address means we should be able to connect back
    // on 127.0.0.1, so leave the string empty in that case.
    if (IN6_IS_ADDR_UNSPECIFIED(&addr_in6->sin6_addr)) {
    } else {
      char str[INET6_ADDRSTRLEN];
      inet_ntop(AF_INET6, &(addr_in6->sin6_addr), str, INET6_ADDRSTRLEN);
      ip_address = GoogleString(str);
    }
  }
  return ip_address;
}

}  // namespace net_instaweb
