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
    "(function(){var a=Date,b=0,c=0,d=.462,e=1204251968254;Math.r"
    "andom=function(){b++;b>25&&(d+=.1,b=1);return d%1};Date=func"
    "tion(){if(this instanceof Date)switch(c++,c>25&&(e+=50,c=1),"
    "arguments.length){case 0:return new a(e);case 1:return new a"
    "(arguments[0]);default:return new a(arguments[0],arguments[1"
    "],arguments.length>=3?arguments[2]:1,arguments.length>=4?arg"
    "uments[3]:0,arguments.length>=5?arguments[4]:0,arguments.len"
    "gth>=6?arguments[5]:0,arguments.length>=7?arguments[6]:0)}re"
    "turn(new Date).toString()};\nDate.__proto__=a;Date.prototype."
    "constructor=Date;a.now=function(){return(new Date).getTime()"
    "};})();\n";

}  // namespace net_instaweb
