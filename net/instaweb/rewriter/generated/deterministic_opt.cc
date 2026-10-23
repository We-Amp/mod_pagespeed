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
// Automatically generated from deterministic_opt.js

namespace net_instaweb {

const char* JS_deterministic_opt =
    "(()=>{var t=Date,e=0,n=0,r=.462,a=1204251968254,u=25,o=25;Ma"
    "th.random=function(){return e++,e>u&&(r+=.1,e=1),r%1};window"
    ".Date=function(){if(this instanceof Date)switch(n++,n>o&&(a+"
    "=50,n=1),arguments.length){case 0:return new t(a);case 1:ret"
    "urn new t(arguments[0]);default:return new t(arguments[0],ar"
    "guments[1],arguments.length>=3?arguments[2]:1,arguments.leng"
    "th>=4?arguments[3]:0,arguments.length>=5?arguments[4]:0,argu"
    "ments.length>=6?arguments[5]:0,arguments.length>=7?arguments"
    "[6]:0)}return new Date().toString()};Date.__proto__=t;Date.p"
    "rototype.constructor=Date;t.now=function(){return new Date()"
    ".getTime()};})();\n";

}  // namespace net_instaweb
