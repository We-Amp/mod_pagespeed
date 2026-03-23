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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/extended_instrumentation_dbg.js

namespace net_instaweb {

const char* JS_extended_instrumentation =
    "(function(){window.pagespeed = window.pagespeed || {};\nvar p"
    "agespeed = window.pagespeed;\npagespeed.getResourceTimingData"
    " = function() {\n  if (window.performance && (window.performa"
    "nce.getEntries || window.performance.webkitGetEntries)) {\n  "
    "  for (var m = 0, l = 0, e = 0, n = 0, f = 0, p = 0, g = 0, "
    "q = 0, h = 0, r = 0, k = 0, c = {}, d = window.performance.g"
    "etEntries ? window.performance.getEntries() : window.perform"
    "ance.webkitGetEntries(), b = 0; b < d.length; b++) {\n      v"
    "ar a = d[b].duration;\n      a > 0 && (m += a, ++e, l = Math."
    "max(l, a));\n      a = d[b].connectEnd - d[b].connectStart;\n "
    "     a > 0 && (p += a, ++g);\n      a = d[b].domainLookupEnd "
    "- d[b].domainLookupStart;\n      a > 0 && (n += a, ++f);\n    "
    "  a = d[b].initiatorType;\n      c[a] ? ++c[a] : c[a] = 1;\n  "
    "    a = d[b].requestStart - d[b].fetchStart;\n      a > 0 && "
    "(r += a, ++k);\n      a = d[b].responseStart - d[b].requestSt"
    "art;\n      a > 0 && (q += a, ++h);\n    }\n    return \"&afd=\" "
    "+ (e ? Math.round(m / e) : 0) + \"&nfd=\" + e + \"&mfd=\" + Math"
    ".round(l) + \"&act=\" + (g ? Math.round(p / g) : 0) + \"&nct=\" "
    "+ g + \"&adt=\" + (f ? Math.round(n / f) : 0) + \"&ndt=\" + f + "
    "\"&abt=\" + (k ? Math.round(r / k) : 0) + \"&nbt=\" + k + \"&attf"
    "b=\" + (h ? Math.round(q / h) : 0) + \"&nttfb=\" + h + (c.css ?"
    " \"&rit_css=\" + c.css : \"\") + (c.link ? \"&rit_link=\" + c.link"
    " : \"\") + (c.script ? \"&rit_script=\" + c.script : \"\") + (c.im"
    "g ? \"&rit_img=\" + c.img : \"\");\n  }\n  return \"\";\n};\npagespeed"
    ".getResourceTimingData = pagespeed.getResourceTimingData;\n})"
    "();\n";

}  // namespace net_instaweb
