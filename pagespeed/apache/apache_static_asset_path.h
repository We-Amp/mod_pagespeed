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

// Names the static asset a request asks for in the Apache port.  Pulled out
// of instaweb_handler.cc so the decision can be unit-tested without httpd.
//
// A request has two readings of its path: the one the server works with and
// the one the module derives from the request line.  The static asset
// handler answers only when both readings are the same path and that path is
// a file directly under the static asset prefix; every other request is left
// to the server.

#ifndef PAGESPEED_APACHE_APACHE_STATIC_ASSET_PATH_H_
#define PAGESPEED_APACHE_APACHE_STATIC_ASSET_PATH_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Returns the asset file name -- the part of the path after `prefix` -- when
// `server_uri` and `module_path` (both without query) are identical and name
// a file directly under `prefix`.  Returns an empty StringPiece otherwise:
// the two readings differ, the path is not under `prefix`, nothing follows
// the prefix, what follows contains a '/', or `prefix` is empty or does not
// end in '/'.  The result points into `server_uri`.
StringPiece ApacheStaticAssetName(StringPiece server_uri,
                                  StringPiece module_path, StringPiece prefix);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_STATIC_ASSET_PATH_H_
