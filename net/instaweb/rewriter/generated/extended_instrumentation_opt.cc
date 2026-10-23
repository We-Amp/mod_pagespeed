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
// Automatically generated from extended_instrumentation_opt.js

namespace net_instaweb {

const char* JS_extended_instrumentation_opt =
    "(()=>{window.pagespeed=window.pagespeed||{};var I=window.pag"
    "espeed;I.getResourceTimingData=function(){if(window.performa"
    "nce&&(window.performance.getEntries||window.performance.webk"
    "itGetEntries)){for(var c=0,E=0,v=0,_=0,f=0,h=0,w=0,G=0,g=0,i"
    "=0,p=0,a={},d=window.performance.getEntries?window.performan"
    "ce.getEntries():window.performance.webkitGetEntries(),r=0;r<"
    "d.length;r++){var S=d[r].duration;S>0&&(c+=S,++v,E=Math.max("
    "E,S));var j=d[r].connectEnd-d[r].connectStart;j>0&&(h+=j,++w"
    ");var z=d[r].domainLookupEnd-d[r].domainLookupStart;z>0&&(_+"
    "=z,++f);var b=d[r].initiatorType;a[b]?++a[b]:a[b]=1;var A=d["
    "r].requestStart-d[r].fetchStart;A>0&&(i+=A,++p);var H=d[r].r"
    "esponseStart-d[r].requestStart;H>0&&(G+=H,++g)}return\"&afd=\""
    "+(v?Math.round(c/v):0)+\"&nfd=\"+v+\"&mfd=\"+Math.round(E)+\"&act"
    "=\"+(w?Math.round(h/w):0)+\"&nct=\"+w+\"&adt=\"+(f?Math.round(_/f"
    "):0)+\"&ndt=\"+f+\"&abt=\"+(p?Math.round(i/p):0)+\"&nbt=\"+p+\"&att"
    "fb=\"+(g?Math.round(G/g):0)+\"&nttfb=\"+g+(a.css?\"&rit_css=\"+a."
    "css:\"\")+(a.link?\"&rit_link=\"+a.link:\"\")+(a.script?\"&rit_scri"
    "pt=\"+a.script:\"\")+(a.img?\"&rit_img=\"+a.img:\"\")}return\"\"};})("
    ");\n";

}  // namespace net_instaweb
