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

// Include Apache's httpd.h and capture its "OK" macro as the APACHE_OK enum,
// then undefine it, so the short macro name cannot collide with third-party
// enums in translation units that also include Apache headers.

#ifndef PAGESPEED_APACHE_APACHE_HTTPD_INCLUDES_H_
#define PAGESPEED_APACHE_APACHE_HTTPD_INCLUDES_H_

#include "httpd.h"

// Apache defines "OK" as a macro, which can collide with same-named enum
// values in other libraries. Expand the macro out into APACHE_OK and then
// undefine it.
enum { APACHE_OK = OK };
#undef OK

#endif  // PAGESPEED_APACHE_APACHE_HTTPD_INCLUDES_H_
