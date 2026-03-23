/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/deterministic_dbg.js

namespace net_instaweb {

const char* JS_deterministic =
    "(function(){var orig_date = Date, random_count = 0, date_cou"
    "nt = 0, random_seed = 0.462, time_seed = 1204251968254, rand"
    "om_count_threshold = 25, date_count_threshold = 25;\nMath.ran"
    "dom = function() {\n  random_count++;\n  random_count > random"
    "_count_threshold && (random_seed += 0.1, random_count = 1);\n"
    "  return random_seed % 1;\n};\nDate = function() {\n  if (this "
    "instanceof Date) {\n    switch(date_count++, date_count > dat"
    "e_count_threshold && (time_seed += 50, date_count = 1), argu"
    "ments.length) {\n      case 0:\n        return new orig_date(t"
    "ime_seed);\n      case 1:\n        return new orig_date(argume"
    "nts[0]);\n      default:\n        return new orig_date(argumen"
    "ts[0], arguments[1], arguments.length >= 3 ? arguments[2] : "
    "1, arguments.length >= 4 ? arguments[3] : 0, arguments.lengt"
    "h >= 5 ? arguments[4] : 0, arguments.length >= 6 ? arguments"
    "[5] : 0, arguments.length >= 7 ? arguments[6] : 0);\n    }\n  "
    "}\n  return (new Date()).toString();\n};\nDate.__proto__ = orig"
    "_date;\nDate.prototype.constructor = Date;\norig_date.now = fu"
    "nction() {\n  return (new Date()).getTime();\n};\n})();\n";

}  // namespace net_instaweb
