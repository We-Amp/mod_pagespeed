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
// Automatically generated from extended_instrumentation_dbg.js

namespace net_instaweb {

const char* JS_extended_instrumentation =
    "(() => {\n  window.pagespeed = window.pagespeed || {};\n  var "
    "pagespeed = window.pagespeed;\n  pagespeed.getResourceTimingD"
    "ata = function() {\n    if (window[\"performance\"] && (window["
    "\"performance\"][\"getEntries\"] || window[\"performance\"][\"webki"
    "tGetEntries\"])) {\n      var totalFetchDuration = 0;\n      va"
    "r maxFetchDuration = 0;\n      var numFetches = 0;\n      var "
    "totalDnsDuration = 0;\n      var numDnsLookups = 0;\n      var"
    " totalConnectionTime = 0;\n      var numConnections = 0;\n    "
    "  var totalTTFB = 0;\n      var numTTFBRequests = 0;\n      va"
    "r totalBlockingTime = 0;\n      var numRequestsBlocked = 0;\n "
    "     var entryCountMap = {};\n      var entries = window[\"per"
    "formance\"][\"getEntries\"] ? window[\"performance\"].getEntries("
    ") : window[\"performance\"].webkitGetEntries();\n      for (var"
    " i = 0; i < entries.length; i++) {\n        var duration = en"
    "tries[i][\"duration\"];\n        if (duration > 0) {\n          "
    "totalFetchDuration += duration;\n          ++numFetches;\n    "
    "      maxFetchDuration = Math.max(maxFetchDuration, duration"
    ");\n        }\n        var connectTime = entries[i][\"connectEn"
    "d\"] - entries[i][\"connectStart\"];\n        if (connectTime > "
    "0) {\n          totalConnectionTime += connectTime;\n         "
    " ++numConnections;\n        }\n        var dnsTime = entries[i"
    "][\"domainLookupEnd\"] - entries[i][\"domainLookupStart\"];\n    "
    "    if (dnsTime > 0) {\n          totalDnsDuration += dnsTime"
    ";\n          ++numDnsLookups;\n        }\n        var initiator"
    " = entries[i][\"initiatorType\"];\n        if (entryCountMap[in"
    "itiator]) {\n          ++entryCountMap[initiator];\n        } "
    "else {\n          entryCountMap[initiator] = 1;\n        }\n   "
    "     var blockingTime = entries[i][\"requestStart\"] - entries"
    "[i][\"fetchStart\"];\n        if (blockingTime > 0) {\n         "
    " totalBlockingTime += blockingTime;\n          ++numRequestsB"
    "locked;\n        }\n        var ttfb = entries[i][\"responseSta"
    "rt\"] - entries[i][\"requestStart\"];\n        if (ttfb > 0) {\n "
    "         totalTTFB += ttfb;\n          ++numTTFBRequests;\n   "
    "     }\n      }\n      return \"&afd=\" + (numFetches ? Math.rou"
    "nd(\n        totalFetchDuration / numFetches\n      ) : 0) + \""
    "&nfd=\" + numFetches + \"&mfd=\" + Math.round(maxFetchDuration)"
    " + \"&act=\" + (numConnections ? Math.round(\n        totalConn"
    "ectionTime / numConnections\n      ) : 0) + \"&nct=\" + numConn"
    "ections + \"&adt=\" + (numDnsLookups ? Math.round(\n        tot"
    "alDnsDuration / numDnsLookups\n      ) : 0) + \"&ndt=\" + numDn"
    "sLookups + \"&abt=\" + (numRequestsBlocked ? Math.round(\n     "
    "   totalBlockingTime / numRequestsBlocked\n      ) : 0) + \"&n"
    "bt=\" + numRequestsBlocked + \"&attfb=\" + (numTTFBRequests ? M"
    "ath.round(\n        totalTTFB / numTTFBRequests\n      ) : 0) "
    "+ \"&nttfb=\" + numTTFBRequests + (entryCountMap[\"css\"] ? \"&ri"
    "t_css=\" + entryCountMap[\"css\"] : \"\") + (entryCountMap[\"link\""
    "] ? \"&rit_link=\" + entryCountMap[\"link\"] : \"\") + (entryCount"
    "Map[\"script\"] ? \"&rit_script=\" + entryCountMap[\"script\"] : \""
    "\") + (entryCountMap[\"img\"] ? \"&rit_img=\" + entryCountMap[\"im"
    "g\"] : \"\");\n    }\n    return \"\";\n  };\n})();\n";

}  // namespace net_instaweb
