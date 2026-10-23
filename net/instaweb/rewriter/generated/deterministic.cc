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
// Automatically generated from deterministic_dbg.js

namespace net_instaweb {

const char* JS_deterministic =
    "(() => {\n  var orig_date = Date;\n  var random_count = 0;\n  v"
    "ar date_count = 0;\n  var random_seed = 0.462;\n  var time_see"
    "d = 1204251968254;\n  var random_count_threshold = 25;\n  var "
    "date_count_threshold = 25;\n  Math.random = function() {\n    "
    "random_count++;\n    if (random_count > random_count_threshol"
    "d) {\n      random_seed += 0.1;\n      random_count = 1;\n    }"
    "\n    return random_seed % 1;\n  };\n  window.Date = function()"
    " {\n    if (this instanceof Date) {\n      date_count++;\n     "
    " if (date_count > date_count_threshold) {\n        time_seed "
    "+= 50;\n        date_count = 1;\n      }\n      switch (argumen"
    "ts.length) {\n        case 0:\n          return new orig_date("
    "time_seed);\n        case 1:\n          return new orig_date(a"
    "rguments[0]);\n        default:\n          return new orig_dat"
    "e(\n            arguments[0],\n            arguments[1],\n     "
    "       arguments.length >= 3 ? arguments[2] : 1,\n           "
    " arguments.length >= 4 ? arguments[3] : 0,\n            argum"
    "ents.length >= 5 ? arguments[4] : 0,\n            arguments.l"
    "ength >= 6 ? arguments[5] : 0,\n            arguments.length "
    ">= 7 ? arguments[6] : 0\n          );\n      }\n    }\n    retur"
    "n (new Date()).toString();\n  };\n  Date.__proto__ = orig_date"
    ";\n  Date.prototype.constructor = Date;\n  orig_date.now = fun"
    "ction() {\n    return (new Date()).getTime();\n  };\n})();\n";

}  // namespace net_instaweb
