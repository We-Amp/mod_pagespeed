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
// Automatically generated from bazel-out/darwin_arm64-fastbuild/bin/net/instaweb/rewriter/client_domain_rewriter_opt.js

namespace net_instaweb {

const char* JS_client_domain_rewriter_opt =
    "(function(){window.pagespeed=window.pagespeed||{};var d=wind"
    "ow.pagespeed;function f(a){this.g=a}function g(a,b){b=b||win"
    "dow.event;if(b.type!=\"keypress\"||b.keyCode==13)for(var c=b.t"
    "arget;c!=null;c=c.parentNode)if(c.tagName==\"A\"){c=c.href;for"
    "(var e=0;e<a.g.length;e++)if(c.indexOf(a.g[e])==0){window.lo"
    "cation=window.location.protocol+\"//\"+window.location.hostnam"
    "e+\"/\"+c.substr(a.g[e].length);b.preventDefault();break}break"
    "}}\nfunction h(a){document.body.onclick=function(b){g(a,b)};d"
    "ocument.body.onkeypress=function(b){g(a,b)}}d.h=function(a){"
    "a=new f(a);d.clientDomainRewriter=a;h(a)};d.clientDomainRewr"
    "iterInit=d.h;})();\n";

}  // namespace net_instaweb
