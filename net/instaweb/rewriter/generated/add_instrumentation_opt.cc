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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/add_instrumentation_opt.js

namespace net_instaweb {

const char* JS_add_instrumentation_opt =
    "(function(){window.pagespeed=window.pagespeed||{};var f=wind"
    "ow.pagespeed;function h(c,a,e,b){this.j=c;this.g=a;this.h=e;"
    "this.l=b}f.beaconUrl=\"\";\nfunction k(c){var a=c.j,e=window.mo"
    "d_pagespeed_start,b=Number(new Date)-e;a+=a.indexOf(\"?\")==-1"
    "?\"?\":\"&\";a=a+\"ets=\"+(c.g==\"load\"?\"load:\":\"unload:\");if(c.g!="
    "\"beforeunload\"||!window.mod_pagespeed_loaded){a=a+b+(\"&r\"+c."
    "g+\"=\");if(window.performance){b=window.performance.timing;va"
    "r d=b.navigationStart,g=b.requestStart;a+=b[c.g+\"EventStart\""
    "]-d;a+=\"&nav=\"+(b.fetchStart-d);a+=\"&dns=\"+(b.domainLookupEn"
    "d-b.domainLookupStart);a+=\"&connect=\"+(b.connectEnd-b.connec"
    "tStart);a=a+(\"&req_start=\"+(g-d))+(\"&ttfb=\"+(b.responseStart"
    "-\ng));a+=\"&dwld=\"+(b.responseEnd-b.responseStart);a+=\"&dom_c"
    "=\"+(b.domContentLoadedEventStart-d);window.performance.navig"
    "ation&&(a+=\"&nt=\"+window.performance.navigation.type);d=-1;b"
    ".msFirstPaint?d=b.msFirstPaint:window.chrome&&window.chrome."
    "loadTimes&&(d=Math.floor(window.chrome.loadTimes().firstPain"
    "tTime*1E3));d-=g;d>=0&&(a+=\"&fp=\"+d)}else a+=b;f.getResource"
    "TimingData&&window.parent==window&&(a+=f.getResourceTimingDa"
    "ta());a+=window.parent!=window?\"&ifr=1\":\"&ifr=0\";c.g==\"load\""
    "&&(window.mod_pagespeed_loaded=\n!0,(b=window.mod_pagespeed_n"
    "um_resources_prefetched)&&(a+=\"&nrp=\"+b),(b=window.mod_pages"
    "peed_prefetch_start)&&(a+=\"&htmlAt=\"+(e-b)));f.criticalCss&&"
    "(e=f.criticalCss,a+=\"&ccis=\"+e.total_critical_inlined_size+\""
    "&cces=\"+e.total_original_external_size+\"&ccos=\"+e.total_over"
    "head_size+\"&ccrl=\"+e.num_replaced_links+\"&ccul=\"+e.num_unrep"
    "laced_links);a+=\"&dpr=\"+window.devicePixelRatio;c.h!=\"\"&&(a+"
    "=c.h);document.referrer&&(a+=\"&ref=\"+encodeURIComponent(docu"
    "ment.referrer));a+=\"&url=\"+encodeURIComponent(c.l);f.beaconU"
    "rl=\na;(new Image).src=a}}f.i=function(c,a,e,b){var d=new h(c"
    ",a,e,b);window.addEventListener?window.addEventListener(a,fu"
    "nction(){k(d)},!1):window.attachEvent(\"on\"+a,function(){k(d)"
    "})};f.addInstrumentationInit=f.i;})();\n";

}  // namespace net_instaweb
