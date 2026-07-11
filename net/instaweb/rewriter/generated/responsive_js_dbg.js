(function(){var COMPILED = !0, goog = goog || {};
goog.global = this || self;
goog.exportPath_ = function(a, b, c, d) {
  a = a.split(".");
  d = d || goog.global;
  a[0] in d || typeof d.execScript == "undefined" || d.execScript("var " + a[0]);
  for (var e; a.length && (e = a.shift());) {
    if (a.length || b === void 0) {
      d = d[e] && d[e] !== Object.prototype[e] ? d[e] : d[e] = {};
    } else {
      if (!c && goog.isObject(b) && goog.isObject(d[e])) {
        for (var f in b) {
          b.hasOwnProperty(f) && (d[e][f] = b[f]);
        }
      } else {
        d[e] = b;
      }
    }
  }
};
goog.define = function(a, b) {
  if (!COMPILED) {
    var c = goog.global.CLOSURE_UNCOMPILED_DEFINES, d = goog.global.CLOSURE_DEFINES;
    c && c.nodeType === void 0 && Object.prototype.hasOwnProperty.call(c, a) ? b = c[a] : d && d.nodeType === void 0 && Object.prototype.hasOwnProperty.call(d, a) && (b = d[a]);
  }
  return b;
};
goog.FEATURESET_YEAR = 2012;
goog.DEBUG = !0;
goog.LOCALE = "en";
goog.TRUSTED_SITE = !0;
goog.DISALLOW_TEST_ONLY_CODE = COMPILED && !goog.DEBUG;
goog.ENABLE_CHROME_APP_SAFE_SCRIPT_LOADING = !1;
goog.provide = function(a) {
  if (goog.isInModuleLoader_()) {
    throw Error("goog.provide cannot be used within a module.");
  }
  if (!COMPILED && goog.isProvided_(a)) {
    throw Error('Namespace "' + a + '" already declared.');
  }
  goog.constructNamespace_(a);
};
goog.constructNamespace_ = function(a, b, c) {
  if (!COMPILED) {
    delete goog.implicitNamespaces_[a];
    for (var d = a; (d = d.substring(0, d.lastIndexOf("."))) && !goog.getObjectByName(d);) {
      goog.implicitNamespaces_[d] = !0;
    }
  }
  goog.exportPath_(a, b, c);
};
goog.NONCE_PATTERN_ = /^[\w+/_-]+[=]{0,2}$/;
goog.getScriptNonce_ = function(a) {
  a = (a || goog.global).document;
  return (a = a.querySelector && a.querySelector("script[nonce]")) && (a = a.nonce || a.getAttribute("nonce")) && goog.NONCE_PATTERN_.test(a) ? a : "";
};
goog.VALID_MODULE_RE_ = /^[a-zA-Z_$][a-zA-Z0-9._$]*$/;
goog.module = function(a) {
  if (typeof a !== "string" || !a || a.search(goog.VALID_MODULE_RE_) == -1) {
    throw Error("Invalid module identifier");
  }
  if (!goog.isInGoogModuleLoader_()) {
    throw Error("Module " + a + " has been loaded incorrectly. Note, modules cannot be loaded as normal scripts. They require some kind of pre-processing step. You're likely trying to load a module via a script tag or as a part of a concatenated bundle without rewriting the module. For more info see: https://github.com/google/closure-library/wiki/goog.module:-an-ES6-module-like-alternative-to-goog.provide.");
  }
  if (goog.moduleLoaderState_.moduleName) {
    throw Error("goog.module may only be called once per module.");
  }
  goog.moduleLoaderState_.moduleName = a;
  if (!COMPILED) {
    if (goog.isProvided_(a)) {
      throw Error('Namespace "' + a + '" already declared.');
    }
    delete goog.implicitNamespaces_[a];
  }
};
goog.module.get = function(a) {
  return goog.module.getInternal_(a);
};
goog.module.getInternal_ = function(a) {
  if (!COMPILED) {
    if (a in goog.loadedModules_) {
      return goog.loadedModules_[a].exports;
    }
    if (!goog.implicitNamespaces_[a]) {
      return a = goog.getObjectByName(a), a != null ? a : null;
    }
  }
  return null;
};
goog.ModuleType = {ES6:"es6", GOOG:"goog"};
goog.moduleLoaderState_ = null;
goog.isInModuleLoader_ = function() {
  return goog.isInGoogModuleLoader_() || goog.isInEs6ModuleLoader_();
};
goog.isInGoogModuleLoader_ = function() {
  return !!goog.moduleLoaderState_ && goog.moduleLoaderState_.type == goog.ModuleType.GOOG;
};
goog.isInEs6ModuleLoader_ = function() {
  if (goog.moduleLoaderState_ && goog.moduleLoaderState_.type == goog.ModuleType.ES6) {
    return !0;
  }
  var a = goog.global.$jscomp;
  return a ? typeof a.getCurrentModulePath != "function" ? !1 : !!a.getCurrentModulePath() : !1;
};
goog.module.declareLegacyNamespace = function() {
  if (!COMPILED && !goog.isInGoogModuleLoader_()) {
    throw Error("goog.module.declareLegacyNamespace must be called from within a goog.module");
  }
  if (!COMPILED && !goog.moduleLoaderState_.moduleName) {
    throw Error("goog.module must be called prior to goog.module.declareLegacyNamespace.");
  }
  goog.moduleLoaderState_.declareLegacyNamespace = !0;
};
goog.declareModuleId = function(a) {
  if (!COMPILED) {
    if (!goog.isInEs6ModuleLoader_()) {
      throw Error("goog.declareModuleId may only be called from within an ES6 module");
    }
    if (goog.moduleLoaderState_ && goog.moduleLoaderState_.moduleName) {
      throw Error("goog.declareModuleId may only be called once per module.");
    }
    if (a in goog.loadedModules_) {
      throw Error('Module with namespace "' + a + '" already exists.');
    }
  }
  if (goog.moduleLoaderState_) {
    goog.moduleLoaderState_.moduleName = a;
  } else {
    var b = goog.global.$jscomp;
    if (!b || typeof b.getCurrentModulePath != "function") {
      throw Error('Module with namespace "' + a + '" has been loaded incorrectly.');
    }
    b = b.require(b.getCurrentModulePath());
    goog.loadedModules_[a] = {exports:b, type:goog.ModuleType.ES6, moduleId:a};
  }
};
goog.setTestOnly = function(a) {
  if (goog.DISALLOW_TEST_ONLY_CODE) {
    throw a = a || "", Error("Importing test-only code into non-debug environment" + (a ? ": " + a : "."));
  }
};
goog.forwardDeclare = function(a) {
};
COMPILED || (goog.isProvided_ = function(a) {
  return a in goog.loadedModules_ || !goog.implicitNamespaces_[a] && goog.getObjectByName(a) != null;
}, goog.implicitNamespaces_ = {"goog.module":!0});
goog.getObjectByName = function(a, b) {
  a = a.split(".");
  b = b || goog.global;
  for (var c = 0; c < a.length; c++) {
    if (b = b[a[c]], b == null) {
      return null;
    }
  }
  return b;
};
goog.addDependency = function(a, b, c, d) {
  !COMPILED && goog.DEPENDENCIES_ENABLED && goog.debugLoader_.addDependency(a, b, c, d);
};
goog.ENABLE_DEBUG_LOADER = !1;
goog.logToConsole_ = function(a) {
  goog.global.console && goog.global.console.error(a);
};
goog.require = function(a) {
  if (!COMPILED) {
    goog.ENABLE_DEBUG_LOADER && goog.debugLoader_.requested(a);
    if (goog.isProvided_(a)) {
      if (goog.isInModuleLoader_()) {
        return goog.module.getInternal_(a);
      }
    } else if (goog.ENABLE_DEBUG_LOADER) {
      var b = goog.moduleLoaderState_;
      goog.moduleLoaderState_ = null;
      try {
        goog.debugLoader_.load_(a);
      } finally {
        goog.moduleLoaderState_ = b;
      }
    }
    return null;
  }
};
goog.requireType = function(a) {
  return {};
};
goog.basePath = "";
goog.abstractMethod = function() {
  throw Error("unimplemented abstract method");
};
goog.addSingletonGetter = function(a) {
  a.instance_ = void 0;
  a.getInstance = function() {
    if (a.instance_) {
      return a.instance_;
    }
    goog.DEBUG && (goog.instantiatedSingletons_[goog.instantiatedSingletons_.length] = a);
    return a.instance_ = new a();
  };
};
goog.instantiatedSingletons_ = [];
goog.LOAD_MODULE_USING_EVAL = !0;
goog.SEAL_MODULE_EXPORTS = goog.DEBUG;
goog.loadedModules_ = {};
goog.DEPENDENCIES_ENABLED = !COMPILED && goog.ENABLE_DEBUG_LOADER;
goog.TRANSPILE = "detect";
goog.ASSUME_ES_MODULES_TRANSPILED = !1;
goog.TRUSTED_TYPES_POLICY_NAME = "goog";
goog.hasBadLetScoping = null;
goog.loadModule = function(a) {
  var b = goog.moduleLoaderState_;
  try {
    goog.moduleLoaderState_ = {moduleName:"", declareLegacyNamespace:!1, type:goog.ModuleType.GOOG};
    var c = {}, d = c;
    if (typeof a === "function") {
      d = a.call(void 0, d);
    } else if (typeof a === "string") {
      d = goog.loadModuleFromSource_.call(void 0, d, a);
    } else {
      throw Error("Invalid module definition");
    }
    var e = goog.moduleLoaderState_.moduleName;
    if (typeof e === "string" && e) {
      goog.moduleLoaderState_.declareLegacyNamespace ? goog.constructNamespace_(e, d, c !== d) : goog.SEAL_MODULE_EXPORTS && Object.seal && typeof d == "object" && d != null && Object.seal(d), goog.loadedModules_[e] = {exports:d, type:goog.ModuleType.GOOG, moduleId:goog.moduleLoaderState_.moduleName};
    } else {
      throw Error('Invalid module name "' + e + '"');
    }
  } finally {
    goog.moduleLoaderState_ = b;
  }
};
goog.loadModuleFromSource_ = function(a) {
  eval(goog.CLOSURE_EVAL_PREFILTER_.createScript(arguments[1]));
  return a;
};
goog.normalizePath_ = function(a) {
  a = a.split("/");
  for (var b = 0; b < a.length;) {
    a[b] == "." ? a.splice(b, 1) : b && a[b] == ".." && a[b - 1] && a[b - 1] != ".." ? a.splice(--b, 2) : b++;
  }
  return a.join("/");
};
goog.loadFileSync_ = function(a) {
  if (goog.global.CLOSURE_LOAD_FILE_SYNC) {
    return goog.global.CLOSURE_LOAD_FILE_SYNC(a);
  }
  try {
    var b = new goog.global.XMLHttpRequest();
    b.open("get", a, !1);
    b.send();
    return b.status == 0 || b.status == 200 ? b.responseText : null;
  } catch (c) {
    return null;
  }
};
goog.typeOf = function(a) {
  var b = typeof a;
  return b != "object" ? b : a ? Array.isArray(a) ? "array" : b : "null";
};
goog.isArrayLike = function(a) {
  var b = goog.typeOf(a);
  return b == "array" || b == "object" && typeof a.length == "number";
};
goog.isDateLike = function(a) {
  return goog.isObject(a) && typeof a.getFullYear == "function";
};
goog.isObject = function(a) {
  var b = typeof a;
  return b == "object" && a != null || b == "function";
};
goog.getUid = function(a) {
  return Object.prototype.hasOwnProperty.call(a, goog.UID_PROPERTY_) && a[goog.UID_PROPERTY_] || (a[goog.UID_PROPERTY_] = ++goog.uidCounter_);
};
goog.hasUid = function(a) {
  return !!a[goog.UID_PROPERTY_];
};
goog.removeUid = function(a) {
  a !== null && "removeAttribute" in a && a.removeAttribute(goog.UID_PROPERTY_);
  try {
    delete a[goog.UID_PROPERTY_];
  } catch (b) {
  }
};
goog.UID_PROPERTY_ = "closure_uid_" + (Math.random() * 1e9 >>> 0);
goog.uidCounter_ = 0;
goog.cloneObject = function(a) {
  var b = goog.typeOf(a);
  if (b == "object" || b == "array") {
    if (typeof a.clone === "function") {
      return a.clone();
    }
    if (typeof Map !== "undefined" && a instanceof Map) {
      return new Map(a);
    }
    if (typeof Set !== "undefined" && a instanceof Set) {
      return new Set(a);
    }
    b = b == "array" ? [] : {};
    for (var c in a) {
      b[c] = goog.cloneObject(a[c]);
    }
    return b;
  }
  return a;
};
goog.bindNative_ = function(a, b, c) {
  return a.call.apply(a.bind, arguments);
};
goog.bindJs_ = function(a, b, c) {
  if (!a) {
    throw Error();
  }
  if (arguments.length > 2) {
    var d = Array.prototype.slice.call(arguments, 2);
    return function() {
      var e = Array.prototype.slice.call(arguments);
      Array.prototype.unshift.apply(e, d);
      return a.apply(b, e);
    };
  }
  return function() {
    return a.apply(b, arguments);
  };
};
goog.bind = function(a, b, c) {
  Function.prototype.bind && Function.prototype.bind.toString().indexOf("native code") != -1 ? goog.bind = goog.bindNative_ : goog.bind = goog.bindJs_;
  return goog.bind.apply(null, arguments);
};
goog.partial = function(a, b) {
  var c = Array.prototype.slice.call(arguments, 1);
  return function() {
    var d = c.slice();
    d.push.apply(d, arguments);
    return a.apply(this, d);
  };
};
goog.now = function() {
  return Date.now();
};
goog.globalEval = function(a) {
  (0,eval)(a);
};
goog.getCssName = function(a, b) {
  if (String(a).charAt(0) == ".") {
    throw Error('className passed in goog.getCssName must not start with ".". You passed: ' + a);
  }
  var c = function(e) {
    return goog.cssNameMapping_[e] || e;
  }, d = function(e) {
    e = e.split("-");
    for (var f = [], g = 0; g < e.length; g++) {
      f.push(c(e[g]));
    }
    return f.join("-");
  };
  d = goog.cssNameMapping_ ? goog.cssNameMappingStyle_ == "BY_WHOLE" ? c : d : function(e) {
    return e;
  };
  a = b ? a + "-" + d(b) : d(a);
  return goog.global.CLOSURE_CSS_NAME_MAP_FN ? goog.global.CLOSURE_CSS_NAME_MAP_FN(a) : a;
};
goog.setCssNameMapping = function(a, b) {
  goog.cssNameMapping_ = a;
  goog.cssNameMappingStyle_ = b;
};
!COMPILED && goog.global.CLOSURE_CSS_NAME_MAPPING && (goog.cssNameMapping_ = goog.global.CLOSURE_CSS_NAME_MAPPING);
goog.GetMsgOptions = function() {
};
goog.getMsg = function(a, b, c) {
  c && c.html && (a = a.replace(/</g, "&lt;"));
  c && c.unescapeHtmlEntities && (a = a.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&apos;/g, "'").replace(/&quot;/g, '"').replace(/&amp;/g, "&"));
  b && (a = a.replace(/\{\$([^}]+)}/g, function(d, e) {
    return b != null && e in b ? b[e] : d;
  }));
  return a;
};
goog.getMsgWithFallback = function(a, b) {
  return a;
};
goog.exportSymbol = function(a, b, c) {
  goog.exportPath_(a, b, !0, c);
};
goog.exportProperty = function(a, b, c) {
  a[b] = c;
};
goog.inherits = function(a, b) {
  function c() {
  }
  c.prototype = b.prototype;
  a.superClass_ = b.prototype;
  a.prototype = new c();
  a.prototype.constructor = a;
  a.base = function(d, e, f) {
    for (var g = Array(arguments.length - 2), h = 2; h < arguments.length; h++) {
      g[h - 2] = arguments[h];
    }
    return b.prototype[e].apply(d, g);
  };
};
goog.scope = function(a) {
  if (goog.isInModuleLoader_()) {
    throw Error("goog.scope is not supported within a module.");
  }
  a.call(goog.global);
};
COMPILED || (goog.global.COMPILED = COMPILED);
goog.defineClass = function(a, b) {
  var c = b.constructor, d = b.statics;
  c && c != Object.prototype.constructor || (c = function() {
    throw Error("cannot instantiate an interface (no constructor defined).");
  });
  c = goog.defineClass.createSealingConstructor_(c, a);
  a && goog.inherits(c, a);
  delete b.constructor;
  delete b.statics;
  goog.defineClass.applyProperties_(c.prototype, b);
  d != null && (d instanceof Function ? d(c) : goog.defineClass.applyProperties_(c, d));
  return c;
};
goog.defineClass.SEAL_CLASS_INSTANCES = goog.DEBUG;
goog.defineClass.createSealingConstructor_ = function(a, b) {
  return goog.defineClass.SEAL_CLASS_INSTANCES ? function() {
    var c = a.apply(this, arguments) || this;
    c[goog.UID_PROPERTY_] = c[goog.UID_PROPERTY_];
    return c;
  } : a;
};
goog.defineClass.OBJECT_PROTOTYPE_FIELDS_ = "constructor hasOwnProperty isPrototypeOf propertyIsEnumerable toLocaleString toString valueOf".split(" ");
goog.defineClass.applyProperties_ = function(a, b) {
  for (var c in b) {
    Object.prototype.hasOwnProperty.call(b, c) && (a[c] = b[c]);
  }
  for (var d = 0; d < goog.defineClass.OBJECT_PROTOTYPE_FIELDS_.length; d++) {
    c = goog.defineClass.OBJECT_PROTOTYPE_FIELDS_[d], Object.prototype.hasOwnProperty.call(b, c) && (a[c] = b[c]);
  }
};
goog.identity_ = function(a) {
  return a;
};
goog.createTrustedTypesPolicy = function(a) {
  var b = null, c = goog.global.trustedTypes;
  if (!c || !c.createPolicy) {
    return b;
  }
  try {
    b = c.createPolicy(a, {createHTML:goog.identity_, createScript:goog.identity_, createScriptURL:goog.identity_});
  } catch (d) {
    goog.logToConsole_(d.message);
  }
  return b;
};
!COMPILED && goog.DEPENDENCIES_ENABLED && (goog.isEdge_ = function() {
  return !!(goog.global.navigator && goog.global.navigator.userAgent ? goog.global.navigator.userAgent : "").match(/Edge\/(\d+)(\.\d)*/i);
}, goog.inHtmlDocument_ = function() {
  var a = goog.global.document;
  return a != null && "write" in a;
}, goog.isDocumentLoading_ = function() {
  var a = goog.global.document;
  return a.attachEvent ? a.readyState != "complete" : a.readyState == "loading";
}, goog.findBasePath_ = function() {
  if (goog.global.CLOSURE_BASE_PATH != void 0 && typeof goog.global.CLOSURE_BASE_PATH === "string") {
    goog.basePath = goog.global.CLOSURE_BASE_PATH;
  } else if (goog.inHtmlDocument_()) {
    var a = goog.global.document, b = a.currentScript;
    a = b ? [b] : a.getElementsByTagName("SCRIPT");
    for (b = a.length - 1; b >= 0; --b) {
      var c = a[b].src, d = c.lastIndexOf("?");
      d = d == -1 ? c.length : d;
      if (c.slice(d - 7, d) == "base.js") {
        goog.basePath = c.slice(0, d - 7);
        break;
      }
    }
  }
}, goog.findBasePath_(), goog.protectScriptTag_ = function(a) {
  return a.replace(/<\/(SCRIPT)/ig, "\\x3c/$1");
}, goog.DebugLoader_ = function() {
  this.dependencies_ = {};
  this.idToPath_ = {};
  this.written_ = {};
  this.loadingDeps_ = [];
  this.depsToLoad_ = [];
  this.paused_ = !1;
  this.factory_ = new goog.DependencyFactory();
  this.deferredCallbacks_ = {};
  this.deferredQueue_ = [];
}, goog.DebugLoader_.prototype.bootstrap = function(a, b) {
  function c() {
    d && (goog.global.setTimeout(d, 0), d = null);
  }
  var d = b;
  if (a.length) {
    b = [];
    for (var e = 0; e < a.length; e++) {
      var f = this.getPathFromDeps_(a[e]);
      if (!f) {
        throw Error("Unregonized namespace: " + a[e]);
      }
      b.push(this.dependencies_[f]);
    }
    f = goog.require;
    var g = 0;
    for (e = 0; e < a.length; e++) {
      f(a[e]), b[e].onLoad(function() {
        ++g == a.length && c();
      });
    }
  } else {
    c();
  }
}, goog.DebugLoader_.prototype.loadClosureDeps = function() {
  this.depsToLoad_.push(this.factory_.createDependency(goog.normalizePath_(goog.basePath + "deps.js"), "deps.js", [], [], {}));
  this.loadDeps_();
}, goog.DebugLoader_.prototype.requested = function(a, b) {
  (a = this.getPathFromDeps_(a)) && (b || this.areDepsLoaded_(this.dependencies_[a].requires)) && (b = this.deferredCallbacks_[a]) && (delete this.deferredCallbacks_[a], b());
}, goog.DebugLoader_.prototype.setDependencyFactory = function(a) {
  this.factory_ = a;
}, goog.DebugLoader_.prototype.load_ = function(a) {
  if (this.getPathFromDeps_(a)) {
    var b = this, c = [], d = function(e) {
      var f = b.getPathFromDeps_(e);
      if (!f) {
        throw Error("Bad dependency path or symbol: " + e);
      }
      if (!b.written_[f]) {
        b.written_[f] = !0;
        e = b.dependencies_[f];
        for (f = 0; f < e.requires.length; f++) {
          goog.isProvided_(e.requires[f]) || d(e.requires[f]);
        }
        c.push(e);
      }
    };
    d(a);
    a = !!this.depsToLoad_.length;
    this.depsToLoad_ = this.depsToLoad_.concat(c);
    this.paused_ || a || this.loadDeps_();
  } else {
    goog.logToConsole_("goog.require could not find: " + a);
  }
}, goog.DebugLoader_.prototype.loadDeps_ = function() {
  for (var a = this, b = this.paused_; this.depsToLoad_.length && !b;) {
    (function() {
      var c = !1, d = a.depsToLoad_.shift(), e = !1;
      a.loading_(d);
      var f = {pause:function() {
        if (c) {
          throw Error("Cannot call pause after the call to load.");
        }
        b = !0;
      }, resume:function() {
        c ? a.resume_() : b = !1;
      }, loaded:function() {
        if (e) {
          throw Error("Double call to loaded.");
        }
        e = !0;
        a.loaded_(d);
      }, pending:function() {
        for (var g = [], h = 0; h < a.loadingDeps_.length; h++) {
          g.push(a.loadingDeps_[h]);
        }
        return g;
      }, setModuleState:function(g) {
        goog.moduleLoaderState_ = {type:g, moduleName:"", declareLegacyNamespace:!1};
      }, registerEs6ModuleExports:function(g, h, k) {
        k && (goog.loadedModules_[k] = {exports:h, type:goog.ModuleType.ES6, moduleId:k || ""});
      }, registerGoogModuleExports:function(g, h) {
        goog.loadedModules_[g] = {exports:h, type:goog.ModuleType.GOOG, moduleId:g};
      }, clearModuleState:function() {
        goog.moduleLoaderState_ = null;
      }, defer:function(g) {
        if (c) {
          throw Error("Cannot register with defer after the call to load.");
        }
        a.defer_(d, g);
      }, areDepsLoaded:function() {
        return a.areDepsLoaded_(d.requires);
      }};
      try {
        d.load(f);
      } finally {
        c = !0;
      }
    })();
  }
  b && this.pause_();
}, goog.DebugLoader_.prototype.pause_ = function() {
  this.paused_ = !0;
}, goog.DebugLoader_.prototype.resume_ = function() {
  this.paused_ && (this.paused_ = !1, this.loadDeps_());
}, goog.DebugLoader_.prototype.loading_ = function(a) {
  this.loadingDeps_.push(a);
}, goog.DebugLoader_.prototype.loaded_ = function(a) {
  for (var b = 0; b < this.loadingDeps_.length; b++) {
    if (this.loadingDeps_[b] == a) {
      this.loadingDeps_.splice(b, 1);
      break;
    }
  }
  for (b = 0; b < this.deferredQueue_.length; b++) {
    if (this.deferredQueue_[b] == a.path) {
      this.deferredQueue_.splice(b, 1);
      break;
    }
  }
  if (this.loadingDeps_.length == this.deferredQueue_.length && !this.depsToLoad_.length) {
    for (; this.deferredQueue_.length;) {
      this.requested(this.deferredQueue_.shift(), !0);
    }
  }
  a.loaded();
}, goog.DebugLoader_.prototype.areDepsLoaded_ = function(a) {
  for (var b = 0; b < a.length; b++) {
    var c = this.getPathFromDeps_(a[b]);
    if (!c || !(c in this.deferredCallbacks_ || goog.isProvided_(a[b]))) {
      return !1;
    }
  }
  return !0;
}, goog.DebugLoader_.prototype.getPathFromDeps_ = function(a) {
  return a in this.idToPath_ ? this.idToPath_[a] : a in this.dependencies_ ? a : null;
}, goog.DebugLoader_.prototype.defer_ = function(a, b) {
  this.deferredCallbacks_[a.path] = b;
  this.deferredQueue_.push(a.path);
}, goog.LoadController = function() {
}, goog.LoadController.prototype.pause = function() {
}, goog.LoadController.prototype.resume = function() {
}, goog.LoadController.prototype.loaded = function() {
}, goog.LoadController.prototype.pending = function() {
}, goog.LoadController.prototype.registerEs6ModuleExports = function(a, b, c) {
}, goog.LoadController.prototype.setModuleState = function(a) {
}, goog.LoadController.prototype.clearModuleState = function() {
}, goog.LoadController.prototype.defer = function(a) {
}, goog.LoadController.prototype.areDepsLoaded = function() {
}, goog.Dependency = function(a, b, c, d, e) {
  this.path = a;
  this.relativePath = b;
  this.provides = c;
  this.requires = d;
  this.loadFlags = e;
  this.loaded_ = !1;
  this.loadCallbacks_ = [];
}, goog.Dependency.prototype.getPathName = function() {
  var a = this.path, b = a.indexOf("://");
  b >= 0 && (a = a.substring(b + 3), b = a.indexOf("/"), b >= 0 && (a = a.substring(b + 1)));
  return a;
}, goog.Dependency.prototype.onLoad = function(a) {
  this.loaded_ ? a() : this.loadCallbacks_.push(a);
}, goog.Dependency.prototype.loaded = function() {
  this.loaded_ = !0;
  var a = this.loadCallbacks_;
  this.loadCallbacks_ = [];
  for (var b = 0; b < a.length; b++) {
    a[b]();
  }
}, goog.Dependency.defer_ = !1, goog.Dependency.callbackMap_ = {}, goog.Dependency.registerCallback_ = function(a) {
  var b = Math.random().toString(32);
  goog.Dependency.callbackMap_[b] = a;
  return b;
}, goog.Dependency.unregisterCallback_ = function(a) {
  delete goog.Dependency.callbackMap_[a];
}, goog.Dependency.callback_ = function(a, b) {
  if (a in goog.Dependency.callbackMap_) {
    for (var c = goog.Dependency.callbackMap_[a], d = [], e = 1; e < arguments.length; e++) {
      d.push(arguments[e]);
    }
    c.apply(void 0, d);
  } else {
    throw Error("Callback key " + a + " does not exist (was base.js loaded more than once?).");
  }
}, goog.Dependency.prototype.load = function(a) {
  if (goog.global.CLOSURE_IMPORT_SCRIPT) {
    goog.global.CLOSURE_IMPORT_SCRIPT(this.path) ? a.loaded() : a.pause();
  } else {
    if (goog.inHtmlDocument_()) {
      var b = goog.global.document;
      if (b.readyState == "complete" && !goog.ENABLE_CHROME_APP_SAFE_SCRIPT_LOADING) {
        if (/\bdeps.js$/.test(this.path)) {
          a.loaded();
          return;
        }
        throw Error('Cannot write "' + this.path + '" after document load');
      }
      var c = goog.getScriptNonce_();
      if (!goog.ENABLE_CHROME_APP_SAFE_SCRIPT_LOADING && goog.isDocumentLoading_()) {
        var d = function(h) {
          h.readyState && h.readyState != "complete" ? h.onload = d : (goog.Dependency.unregisterCallback_(e), a.loaded());
        };
        var e = goog.Dependency.registerCallback_(d);
        c = c ? ' nonce="' + c + '"' : "";
        var f = '<script src="' + this.path + '"' + c + (goog.Dependency.defer_ ? " defer" : "") + ' id="script-' + e + '">\x3c/script>';
        f += "<script" + c + ">";
        f = goog.Dependency.defer_ ? f + ("document.getElementById('script-" + e + "').onload = function() {\n  goog.Dependency.callback_('" + e + "', this);\n};\n") : f + ("goog.Dependency.callback_('" + e + "', document.getElementById('script-" + e + "'));");
        f += "\x3c/script>";
        b.write(goog.TRUSTED_TYPES_POLICY_ ? goog.TRUSTED_TYPES_POLICY_.createHTML(f) : f);
      } else {
        var g = b.createElement("script");
        g.defer = goog.Dependency.defer_;
        g.async = !1;
        c && (g.nonce = c);
        g.onload = function() {
          g.onload = null;
          a.loaded();
        };
        g.src = goog.TRUSTED_TYPES_POLICY_ ? goog.TRUSTED_TYPES_POLICY_.createScriptURL(this.path) : this.path;
        b.head.appendChild(g);
      }
    } else {
      goog.logToConsole_("Cannot use default debug loader outside of HTML documents."), this.relativePath == "deps.js" ? (goog.logToConsole_("Consider setting CLOSURE_IMPORT_SCRIPT before loading base.js, or setting CLOSURE_NO_DEPS to true."), a.loaded()) : a.pause();
    }
  }
}, goog.Es6ModuleDependency = function(a, b, c, d, e) {
  goog.Dependency.call(this, a, b, c, d, e);
}, goog.inherits(goog.Es6ModuleDependency, goog.Dependency), goog.Es6ModuleDependency.prototype.load = function(a) {
  function b(l, n) {
    var m = "", p = goog.getScriptNonce_();
    p && (m = ' nonce="' + p + '"');
    l = n ? '<script type="module" crossorigin' + m + ">" + n + "\x3c/script>" : '<script type="module" crossorigin src="' + l + '"' + m + ">\x3c/script>";
    d.write(goog.TRUSTED_TYPES_POLICY_ ? goog.TRUSTED_TYPES_POLICY_.createHTML(l) : l);
  }
  function c(l, n) {
    var m = d.createElement("script");
    m.defer = !0;
    m.async = !1;
    m.type = "module";
    m.setAttribute("crossorigin", !0);
    var p = goog.getScriptNonce_();
    p && (m.nonce = p);
    n ? m.text = goog.TRUSTED_TYPES_POLICY_ ? goog.TRUSTED_TYPES_POLICY_.createScript(n) : n : m.src = goog.TRUSTED_TYPES_POLICY_ ? goog.TRUSTED_TYPES_POLICY_.createScriptURL(l) : l;
    d.head.appendChild(m);
  }
  if (goog.global.CLOSURE_IMPORT_SCRIPT) {
    goog.global.CLOSURE_IMPORT_SCRIPT(this.path) ? a.loaded() : a.pause();
  } else {
    if (goog.inHtmlDocument_()) {
      var d = goog.global.document, e = this;
      if (goog.isDocumentLoading_()) {
        var f = b;
        goog.Dependency.defer_ = !0;
      } else {
        f = c;
      }
      var g = goog.Dependency.registerCallback_(function() {
        goog.Dependency.unregisterCallback_(g);
        a.setModuleState(goog.ModuleType.ES6);
      });
      f(void 0, 'goog.Dependency.callback_("' + g + '")');
      f(this.path, void 0);
      var h = goog.Dependency.registerCallback_(function(l) {
        goog.Dependency.unregisterCallback_(h);
        a.registerEs6ModuleExports(e.path, l, goog.moduleLoaderState_.moduleName);
      });
      f(void 0, 'import * as m from "' + this.path + '"; goog.Dependency.callback_("' + h + '", m)');
      var k = goog.Dependency.registerCallback_(function() {
        goog.Dependency.unregisterCallback_(k);
        a.clearModuleState();
        a.loaded();
      });
      f(void 0, 'goog.Dependency.callback_("' + k + '")');
    } else {
      goog.logToConsole_("Cannot use default debug loader outside of HTML documents."), a.pause();
    }
  }
}, goog.TransformedDependency = function(a, b, c, d, e) {
  goog.Dependency.call(this, a, b, c, d, e);
  this.contents_ = null;
  this.lazyFetch_ = !goog.inHtmlDocument_() || !("noModule" in goog.global.document.createElement("script"));
}, goog.inherits(goog.TransformedDependency, goog.Dependency), goog.TransformedDependency.prototype.load = function(a) {
  function b() {
    e.contents_ = goog.loadFileSync_(e.path);
    e.contents_ && (e.contents_ = e.transform(e.contents_), e.contents_ && (e.contents_ += "\n//# sourceURL=" + e.path));
  }
  function c() {
    e.lazyFetch_ && b();
    if (e.contents_) {
      f && a.setModuleState(goog.ModuleType.ES6);
      try {
        var l = e.contents_;
        e.contents_ = null;
        goog.globalEval(goog.CLOSURE_EVAL_PREFILTER_.createScript(l));
        if (f) {
          var n = goog.moduleLoaderState_.moduleName;
        }
      } finally {
        f && a.clearModuleState();
      }
      f && goog.global.$jscomp.require.ensure([e.getPathName()], function() {
        a.registerEs6ModuleExports(e.path, goog.global.$jscomp.require(e.getPathName()), n);
      });
      a.loaded();
    }
  }
  function d() {
    var l = goog.global.document, n = goog.Dependency.registerCallback_(function() {
      goog.Dependency.unregisterCallback_(n);
      c();
    }), m = goog.getScriptNonce_();
    m = "<script" + (m ? ' nonce="' + m + '"' : "") + ">" + goog.protectScriptTag_('goog.Dependency.callback_("' + n + '");') + "\x3c/script>";
    l.write(goog.TRUSTED_TYPES_POLICY_ ? goog.TRUSTED_TYPES_POLICY_.createHTML(m) : m);
  }
  var e = this;
  if (goog.global.CLOSURE_IMPORT_SCRIPT) {
    b(), this.contents_ && goog.global.CLOSURE_IMPORT_SCRIPT("", this.contents_) ? (this.contents_ = null, a.loaded()) : a.pause();
  } else {
    var f = this.loadFlags.module == goog.ModuleType.ES6;
    this.lazyFetch_ || b();
    var g = a.pending().length > 1;
    if (goog.Dependency.defer_ && (g || goog.isDocumentLoading_())) {
      a.defer(function() {
        c();
      });
    } else {
      var h = goog.global.document;
      g = goog.inHtmlDocument_() && ("ActiveXObject" in goog.global || goog.isEdge_());
      if (f && goog.inHtmlDocument_() && goog.isDocumentLoading_() && !g) {
        goog.Dependency.defer_ = !0;
        a.pause();
        var k = h.onreadystatechange;
        h.onreadystatechange = function() {
          h.readyState == "interactive" && (h.onreadystatechange = k, c(), a.resume());
          typeof k === "function" && k.apply(void 0, arguments);
        };
      } else {
        goog.inHtmlDocument_() && goog.isDocumentLoading_() ? d() : c();
      }
    }
  }
}, goog.TransformedDependency.prototype.transform = function(a) {
}, goog.PreTranspiledEs6ModuleDependency = function(a, b, c, d, e) {
  goog.TransformedDependency.call(this, a, b, c, d, e);
}, goog.inherits(goog.PreTranspiledEs6ModuleDependency, goog.TransformedDependency), goog.PreTranspiledEs6ModuleDependency.prototype.transform = function(a) {
  return a;
}, goog.GoogModuleDependency = function(a, b, c, d, e) {
  goog.TransformedDependency.call(this, a, b, c, d, e);
}, goog.inherits(goog.GoogModuleDependency, goog.TransformedDependency), goog.GoogModuleDependency.prototype.transform = function(a) {
  return goog.LOAD_MODULE_USING_EVAL && goog.global.JSON !== void 0 ? "goog.loadModule(" + goog.global.JSON.stringify(a + "\n//# sourceURL=" + this.path + "\n") + ");" : 'goog.loadModule(function(exports) {"use strict";' + a + "\n;return exports});\n//# sourceURL=" + this.path + "\n";
}, goog.DebugLoader_.prototype.addDependency = function(a, b, c, d) {
  b = b || [];
  a = a.replace(/\\/g, "/");
  var e = goog.normalizePath_(goog.basePath + a);
  d && typeof d !== "boolean" || (d = d ? {module:goog.ModuleType.GOOG} : {});
  c = this.factory_.createDependency(e, a, b, c, d);
  this.dependencies_[e] = c;
  for (c = 0; c < b.length; c++) {
    this.idToPath_[b[c]] = e;
  }
  this.idToPath_[a] = e;
}, goog.DependencyFactory = function() {
}, goog.DependencyFactory.prototype.createDependency = function(a, b, c, d, e) {
  return e.module == goog.ModuleType.GOOG ? new goog.GoogModuleDependency(a, b, c, d, e) : e.module == goog.ModuleType.ES6 ? goog.ASSUME_ES_MODULES_TRANSPILED ? new goog.PreTranspiledEs6ModuleDependency(a, b, c, d, e) : new goog.Es6ModuleDependency(a, b, c, d, e) : new goog.Dependency(a, b, c, d, e);
}, goog.debugLoader_ = new goog.DebugLoader_(), goog.loadClosureDeps = function() {
  goog.debugLoader_.loadClosureDeps();
}, goog.setDependencyFactory = function(a) {
  goog.debugLoader_.setDependencyFactory(a);
}, goog.TRUSTED_TYPES_POLICY_ = goog.TRUSTED_TYPES_POLICY_NAME ? goog.createTrustedTypesPolicy(goog.TRUSTED_TYPES_POLICY_NAME + "#base") : null, goog.global.CLOSURE_NO_DEPS || goog.debugLoader_.loadClosureDeps(), goog.bootstrap = function(a, b) {
  goog.debugLoader_.bootstrap(a, b);
});
if (!COMPILED) {
  var isChrome87 = !1;
  try {
    isChrome87 = eval(goog.global.trustedTypes.emptyScript) !== goog.global.trustedTypes.emptyScript;
  } catch (a) {
  }
  goog.CLOSURE_EVAL_PREFILTER_ = goog.global.trustedTypes && isChrome87 && goog.createTrustedTypesPolicy("goog#base#devonly#eval") || {createScript:goog.identity_};
}
;goog.debug = {};
function module$contents$goog$debug$Error_DebugError(a, b) {
  if (Error.captureStackTrace) {
    Error.captureStackTrace(this, module$contents$goog$debug$Error_DebugError);
  } else {
    const c = Error().stack;
    c && (this.stack = c);
  }
  a && (this.message = String(a));
  b !== void 0 && (this.cause = b);
  this.reportErrorToServer = !0;
}
goog.inherits(module$contents$goog$debug$Error_DebugError, Error);
module$contents$goog$debug$Error_DebugError.prototype.name = "CustomError";
goog.debug.Error = module$contents$goog$debug$Error_DebugError;
goog.dom = {};
goog.dom.NodeType = {ELEMENT:1, ATTRIBUTE:2, TEXT:3, CDATA_SECTION:4, ENTITY_REFERENCE:5, ENTITY:6, PROCESSING_INSTRUCTION:7, COMMENT:8, DOCUMENT:9, DOCUMENT_TYPE:10, DOCUMENT_FRAGMENT:11, NOTATION:12};
goog.asserts = {};
goog.asserts.ENABLE_ASSERTS = goog.DEBUG;
function module$contents$goog$asserts_AssertionError(a, b) {
  module$contents$goog$debug$Error_DebugError.call(this, module$contents$goog$asserts_subs(a, b));
  this.messagePattern = a;
}
goog.inherits(module$contents$goog$asserts_AssertionError, module$contents$goog$debug$Error_DebugError);
goog.asserts.AssertionError = module$contents$goog$asserts_AssertionError;
module$contents$goog$asserts_AssertionError.prototype.name = "AssertionError";
goog.asserts.DEFAULT_ERROR_HANDLER = function(a) {
  throw a;
};
let module$contents$goog$asserts_errorHandler_ = goog.asserts.DEFAULT_ERROR_HANDLER;
function module$contents$goog$asserts_subs(a, b) {
  a = a.split("%s");
  let c = "";
  const d = a.length - 1;
  for (let e = 0; e < d; e++) {
    c += a[e] + (e < b.length ? b[e] : "%s");
  }
  return c + a[d];
}
function module$contents$goog$asserts_doAssertFailure(a, b, c, d) {
  let e = "Assertion failed", f;
  c ? (e += ": " + c, f = d) : a && (e += ": " + a, f = b);
  a = new module$contents$goog$asserts_AssertionError("" + e, f || []);
  module$contents$goog$asserts_errorHandler_(a);
}
goog.asserts.setErrorHandler = function(a) {
  goog.asserts.ENABLE_ASSERTS && (module$contents$goog$asserts_errorHandler_ = a);
};
goog.asserts.assert = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && !a && module$contents$goog$asserts_doAssertFailure("", null, b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertExists = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && a == null && module$contents$goog$asserts_doAssertFailure("Expected to exist: %s.", [a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.fail = function(a, b) {
  goog.asserts.ENABLE_ASSERTS && module$contents$goog$asserts_errorHandler_(new module$contents$goog$asserts_AssertionError("Failure" + (a ? ": " + a : ""), Array.prototype.slice.call(arguments, 1)));
};
goog.asserts.assertNumber = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && typeof a !== "number" && module$contents$goog$asserts_doAssertFailure("Expected number but got %s: %s.", [goog.typeOf(a), a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertString = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && typeof a !== "string" && module$contents$goog$asserts_doAssertFailure("Expected string but got %s: %s.", [goog.typeOf(a), a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertFunction = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && typeof a !== "function" && module$contents$goog$asserts_doAssertFailure("Expected function but got %s: %s.", [goog.typeOf(a), a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertObject = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && !goog.isObject(a) && module$contents$goog$asserts_doAssertFailure("Expected object but got %s: %s.", [goog.typeOf(a), a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertArray = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && !Array.isArray(a) && module$contents$goog$asserts_doAssertFailure("Expected array but got %s: %s.", [goog.typeOf(a), a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertBoolean = function(a, b, c) {
  goog.asserts.ENABLE_ASSERTS && typeof a !== "boolean" && module$contents$goog$asserts_doAssertFailure("Expected boolean but got %s: %s.", [goog.typeOf(a), a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertElement = function(a, b, c) {
  !goog.asserts.ENABLE_ASSERTS || goog.isObject(a) && a.nodeType == goog.dom.NodeType.ELEMENT || module$contents$goog$asserts_doAssertFailure("Expected Element but got %s: %s.", [goog.typeOf(a), a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
goog.asserts.assertInstanceof = function(a, b, c, d) {
  !goog.asserts.ENABLE_ASSERTS || a instanceof b || module$contents$goog$asserts_doAssertFailure("Expected instanceof %s but got %s.", [module$contents$goog$asserts_getType(b), module$contents$goog$asserts_getType(a)], c, Array.prototype.slice.call(arguments, 3));
  return a;
};
goog.asserts.assertFinite = function(a, b, c) {
  !goog.asserts.ENABLE_ASSERTS || typeof a == "number" && isFinite(a) || module$contents$goog$asserts_doAssertFailure("Expected %s to be a finite number but it is not.", [a], b, Array.prototype.slice.call(arguments, 2));
  return a;
};
function module$contents$goog$asserts_getType(a) {
  return a instanceof Function ? a.displayName || a.name || "unknown type name" : a instanceof Object ? a.constructor.displayName || a.constructor.name || Object.prototype.toString.call(a) : a === null ? "null" : typeof a;
}
;goog.array = {};
goog.NATIVE_ARRAY_PROTOTYPES = goog.TRUSTED_SITE;
const module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS = goog.FEATURESET_YEAR > 2012;
goog.array.ASSUME_NATIVE_FUNCTIONS = module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS;
function module$contents$goog$array_peek(a) {
  return a[a.length - 1];
}
goog.array.peek = module$contents$goog$array_peek;
goog.array.last = module$contents$goog$array_peek;
const module$contents$goog$array_indexOf = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.indexOf) ? function(a, b, c) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.indexOf.call(a, b, c);
} : function(a, b, c) {
  c = c == null ? 0 : c < 0 ? Math.max(0, a.length + c) : c;
  if (typeof a === "string") {
    return typeof b !== "string" || b.length != 1 ? -1 : a.indexOf(b, c);
  }
  for (; c < a.length; c++) {
    if (c in a && a[c] === b) {
      return c;
    }
  }
  return -1;
};
goog.array.indexOf = module$contents$goog$array_indexOf;
const module$contents$goog$array_lastIndexOf = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.lastIndexOf) ? function(a, b, c) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.lastIndexOf.call(a, b, c == null ? a.length - 1 : c);
} : function(a, b, c) {
  c = c == null ? a.length - 1 : c;
  c < 0 && (c = Math.max(0, a.length + c));
  if (typeof a === "string") {
    return typeof b !== "string" || b.length != 1 ? -1 : a.lastIndexOf(b, c);
  }
  for (; c >= 0; c--) {
    if (c in a && a[c] === b) {
      return c;
    }
  }
  return -1;
};
goog.array.lastIndexOf = module$contents$goog$array_lastIndexOf;
const module$contents$goog$array_forEach = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.forEach) ? function(a, b, c) {
  goog.asserts.assert(a.length != null);
  Array.prototype.forEach.call(a, b, c);
} : function(a, b, c) {
  const d = a.length, e = typeof a === "string" ? a.split("") : a;
  for (let f = 0; f < d; f++) {
    f in e && b.call(c, e[f], f, a);
  }
};
goog.array.forEach = module$contents$goog$array_forEach;
function module$contents$goog$array_forEachRight(a, b, c) {
  var d = a.length;
  const e = typeof a === "string" ? a.split("") : a;
  for (--d; d >= 0; --d) {
    d in e && b.call(c, e[d], d, a);
  }
}
goog.array.forEachRight = module$contents$goog$array_forEachRight;
const module$contents$goog$array_filter = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.filter) ? function(a, b, c) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.filter.call(a, b, c);
} : function(a, b, c) {
  const d = a.length, e = [];
  let f = 0;
  const g = typeof a === "string" ? a.split("") : a;
  for (let h = 0; h < d; h++) {
    if (h in g) {
      const k = g[h];
      b.call(c, k, h, a) && (e[f++] = k);
    }
  }
  return e;
};
goog.array.filter = module$contents$goog$array_filter;
const module$contents$goog$array_map = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.map) ? function(a, b, c) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.map.call(a, b, c);
} : function(a, b, c) {
  const d = a.length, e = Array(d), f = typeof a === "string" ? a.split("") : a;
  for (let g = 0; g < d; g++) {
    g in f && (e[g] = b.call(c, f[g], g, a));
  }
  return e;
};
goog.array.map = module$contents$goog$array_map;
const module$contents$goog$array_reduce = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.reduce) ? function(a, b, c, d) {
  goog.asserts.assert(a.length != null);
  d && (b = goog.bind(b, d));
  return Array.prototype.reduce.call(a, b, c);
} : function(a, b, c, d) {
  let e = c;
  module$contents$goog$array_forEach(a, function(f, g) {
    e = b.call(d, e, f, g, a);
  });
  return e;
};
goog.array.reduce = module$contents$goog$array_reduce;
const module$contents$goog$array_reduceRight = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.reduceRight) ? function(a, b, c, d) {
  goog.asserts.assert(a.length != null);
  goog.asserts.assert(b != null);
  d && (b = goog.bind(b, d));
  return Array.prototype.reduceRight.call(a, b, c);
} : function(a, b, c, d) {
  let e = c;
  module$contents$goog$array_forEachRight(a, function(f, g) {
    e = b.call(d, e, f, g, a);
  });
  return e;
};
goog.array.reduceRight = module$contents$goog$array_reduceRight;
const module$contents$goog$array_some = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.some) ? function(a, b, c) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.some.call(a, b, c);
} : function(a, b, c) {
  const d = a.length, e = typeof a === "string" ? a.split("") : a;
  for (let f = 0; f < d; f++) {
    if (f in e && b.call(c, e[f], f, a)) {
      return !0;
    }
  }
  return !1;
};
goog.array.some = module$contents$goog$array_some;
const module$contents$goog$array_every = goog.NATIVE_ARRAY_PROTOTYPES && (module$contents$goog$array_ASSUME_NATIVE_FUNCTIONS || Array.prototype.every) ? function(a, b, c) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.every.call(a, b, c);
} : function(a, b, c) {
  const d = a.length, e = typeof a === "string" ? a.split("") : a;
  for (let f = 0; f < d; f++) {
    if (f in e && !b.call(c, e[f], f, a)) {
      return !1;
    }
  }
  return !0;
};
goog.array.every = module$contents$goog$array_every;
function module$contents$goog$array_count(a, b, c) {
  let d = 0;
  module$contents$goog$array_forEach(a, function(e, f, g) {
    b.call(c, e, f, g) && ++d;
  }, c);
  return d;
}
goog.array.count = module$contents$goog$array_count;
function module$contents$goog$array_find(a, b, c) {
  b = module$contents$goog$array_findIndex(a, b, c);
  return b < 0 ? null : typeof a === "string" ? a.charAt(b) : a[b];
}
goog.array.find = module$contents$goog$array_find;
function module$contents$goog$array_findIndex(a, b, c) {
  const d = a.length, e = typeof a === "string" ? a.split("") : a;
  for (let f = 0; f < d; f++) {
    if (f in e && b.call(c, e[f], f, a)) {
      return f;
    }
  }
  return -1;
}
goog.array.findIndex = module$contents$goog$array_findIndex;
function module$contents$goog$array_findRight(a, b, c) {
  b = module$contents$goog$array_findIndexRight(a, b, c);
  return b < 0 ? null : typeof a === "string" ? a.charAt(b) : a[b];
}
goog.array.findRight = module$contents$goog$array_findRight;
function module$contents$goog$array_findIndexRight(a, b, c) {
  var d = a.length;
  const e = typeof a === "string" ? a.split("") : a;
  for (--d; d >= 0; d--) {
    if (d in e && b.call(c, e[d], d, a)) {
      return d;
    }
  }
  return -1;
}
goog.array.findIndexRight = module$contents$goog$array_findIndexRight;
function module$contents$goog$array_contains(a, b) {
  return module$contents$goog$array_indexOf(a, b) >= 0;
}
goog.array.contains = module$contents$goog$array_contains;
function module$contents$goog$array_isEmpty(a) {
  return a.length == 0;
}
goog.array.isEmpty = module$contents$goog$array_isEmpty;
function module$contents$goog$array_clear(a) {
  if (!Array.isArray(a)) {
    for (let b = a.length - 1; b >= 0; b--) {
      delete a[b];
    }
  }
  a.length = 0;
}
goog.array.clear = module$contents$goog$array_clear;
function module$contents$goog$array_insert(a, b) {
  module$contents$goog$array_contains(a, b) || a.push(b);
}
goog.array.insert = module$contents$goog$array_insert;
function module$contents$goog$array_insertAt(a, b, c) {
  module$contents$goog$array_splice(a, c, 0, b);
}
goog.array.insertAt = module$contents$goog$array_insertAt;
function module$contents$goog$array_insertArrayAt(a, b, c) {
  goog.partial(module$contents$goog$array_splice, a, c, 0).apply(null, b);
}
goog.array.insertArrayAt = module$contents$goog$array_insertArrayAt;
function module$contents$goog$array_insertBefore(a, b, c) {
  let d;
  arguments.length == 2 || (d = module$contents$goog$array_indexOf(a, c)) < 0 ? a.push(b) : module$contents$goog$array_insertAt(a, b, d);
}
goog.array.insertBefore = module$contents$goog$array_insertBefore;
function module$contents$goog$array_remove(a, b) {
  b = module$contents$goog$array_indexOf(a, b);
  let c;
  (c = b >= 0) && module$contents$goog$array_removeAt(a, b);
  return c;
}
goog.array.remove = module$contents$goog$array_remove;
function module$contents$goog$array_removeLast(a, b) {
  b = module$contents$goog$array_lastIndexOf(a, b);
  return b >= 0 ? (module$contents$goog$array_removeAt(a, b), !0) : !1;
}
goog.array.removeLast = module$contents$goog$array_removeLast;
function module$contents$goog$array_removeAt(a, b) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.splice.call(a, b, 1).length == 1;
}
goog.array.removeAt = module$contents$goog$array_removeAt;
function module$contents$goog$array_removeIf(a, b, c) {
  b = module$contents$goog$array_findIndex(a, b, c);
  return b >= 0 ? (module$contents$goog$array_removeAt(a, b), !0) : !1;
}
goog.array.removeIf = module$contents$goog$array_removeIf;
function module$contents$goog$array_removeAllIf(a, b, c) {
  let d = 0;
  module$contents$goog$array_forEachRight(a, function(e, f) {
    b.call(c, e, f, a) && module$contents$goog$array_removeAt(a, f) && d++;
  });
  return d;
}
goog.array.removeAllIf = module$contents$goog$array_removeAllIf;
function module$contents$goog$array_concat(a) {
  return Array.prototype.concat.apply([], arguments);
}
goog.array.concat = module$contents$goog$array_concat;
function module$contents$goog$array_join(a) {
  return Array.prototype.concat.apply([], arguments);
}
goog.array.join = module$contents$goog$array_join;
function module$contents$goog$array_toArray(a) {
  const b = a.length;
  if (b > 0) {
    const c = Array(b);
    for (let d = 0; d < b; d++) {
      c[d] = a[d];
    }
    return c;
  }
  return [];
}
const module$contents$goog$array_clone = goog.array.toArray = module$contents$goog$array_toArray;
goog.array.clone = module$contents$goog$array_toArray;
function module$contents$goog$array_extend(a, b) {
  for (let c = 1; c < arguments.length; c++) {
    const d = arguments[c];
    if (goog.isArrayLike(d)) {
      const e = a.length || 0, f = d.length || 0;
      a.length = e + f;
      for (let g = 0; g < f; g++) {
        a[e + g] = d[g];
      }
    } else {
      a.push(d);
    }
  }
}
goog.array.extend = module$contents$goog$array_extend;
function module$contents$goog$array_splice(a, b, c, d) {
  goog.asserts.assert(a.length != null);
  return Array.prototype.splice.apply(a, module$contents$goog$array_slice(arguments, 1));
}
goog.array.splice = module$contents$goog$array_splice;
function module$contents$goog$array_slice(a, b, c) {
  goog.asserts.assert(a.length != null);
  return arguments.length <= 2 ? Array.prototype.slice.call(a, b) : Array.prototype.slice.call(a, b, c);
}
goog.array.slice = module$contents$goog$array_slice;
function module$contents$goog$array_removeDuplicates(a, b, c) {
  b = b || a;
  var d = function(g) {
    return goog.isObject(g) ? "o" + goog.getUid(g) : (typeof g).charAt(0) + g;
  };
  c = c || d;
  let e = d = 0;
  const f = {};
  for (; e < a.length;) {
    const g = a[e++], h = c(g);
    Object.prototype.hasOwnProperty.call(f, h) || (f[h] = !0, b[d++] = g);
  }
  b.length = d;
}
goog.array.removeDuplicates = module$contents$goog$array_removeDuplicates;
function module$contents$goog$array_binarySearch(a, b, c) {
  return module$contents$goog$array_binarySearch_(a, c || module$contents$goog$array_defaultCompare, !1, b);
}
goog.array.binarySearch = module$contents$goog$array_binarySearch;
function module$contents$goog$array_binarySelect(a, b, c) {
  return module$contents$goog$array_binarySearch_(a, b, !0, void 0, c);
}
goog.array.binarySelect = module$contents$goog$array_binarySelect;
function module$contents$goog$array_binarySearch_(a, b, c, d, e) {
  let f = 0, g = a.length, h;
  for (; f < g;) {
    const k = f + (g - f >>> 1);
    let l;
    l = c ? b.call(e, a[k], k, a) : b(d, a[k]);
    l > 0 ? f = k + 1 : (g = k, h = !l);
  }
  return h ? f : -f - 1;
}
function module$contents$goog$array_sort(a, b) {
  a.sort(b || module$contents$goog$array_defaultCompare);
}
goog.array.sort = module$contents$goog$array_sort;
function module$contents$goog$array_stableSort(a, b) {
  const c = Array(a.length);
  for (let e = 0; e < a.length; e++) {
    c[e] = {index:e, value:a[e]};
  }
  const d = b || module$contents$goog$array_defaultCompare;
  module$contents$goog$array_sort(c, function(e, f) {
    return d(e.value, f.value) || e.index - f.index;
  });
  for (b = 0; b < a.length; b++) {
    a[b] = c[b].value;
  }
}
goog.array.stableSort = module$contents$goog$array_stableSort;
function module$contents$goog$array_sortByKey(a, b, c) {
  const d = c || module$contents$goog$array_defaultCompare;
  module$contents$goog$array_sort(a, function(e, f) {
    return d(b(e), b(f));
  });
}
goog.array.sortByKey = module$contents$goog$array_sortByKey;
function module$contents$goog$array_sortObjectsByKey(a, b, c) {
  module$contents$goog$array_sortByKey(a, function(d) {
    return d[b];
  }, c);
}
goog.array.sortObjectsByKey = module$contents$goog$array_sortObjectsByKey;
function module$contents$goog$array_isSorted(a, b, c) {
  b = b || module$contents$goog$array_defaultCompare;
  for (let d = 1; d < a.length; d++) {
    const e = b(a[d - 1], a[d]);
    if (e > 0 || e == 0 && c) {
      return !1;
    }
  }
  return !0;
}
goog.array.isSorted = module$contents$goog$array_isSorted;
function module$contents$goog$array_equals(a, b, c) {
  if (!goog.isArrayLike(a) || !goog.isArrayLike(b) || a.length != b.length) {
    return !1;
  }
  const d = a.length;
  c = c || module$contents$goog$array_defaultCompareEquality;
  for (let e = 0; e < d; e++) {
    if (!c(a[e], b[e])) {
      return !1;
    }
  }
  return !0;
}
goog.array.equals = module$contents$goog$array_equals;
function module$contents$goog$array_compare3(a, b, c) {
  c = c || module$contents$goog$array_defaultCompare;
  const d = Math.min(a.length, b.length);
  for (let e = 0; e < d; e++) {
    const f = c(a[e], b[e]);
    if (f != 0) {
      return f;
    }
  }
  return module$contents$goog$array_defaultCompare(a.length, b.length);
}
goog.array.compare3 = module$contents$goog$array_compare3;
function module$contents$goog$array_defaultCompare(a, b) {
  return a > b ? 1 : a < b ? -1 : 0;
}
goog.array.defaultCompare = module$contents$goog$array_defaultCompare;
function module$contents$goog$array_inverseDefaultCompare(a, b) {
  return -module$contents$goog$array_defaultCompare(a, b);
}
goog.array.inverseDefaultCompare = module$contents$goog$array_inverseDefaultCompare;
function module$contents$goog$array_defaultCompareEquality(a, b) {
  return a === b;
}
goog.array.defaultCompareEquality = module$contents$goog$array_defaultCompareEquality;
function module$contents$goog$array_binaryInsert(a, b, c) {
  c = module$contents$goog$array_binarySearch(a, b, c);
  return c < 0 ? (module$contents$goog$array_insertAt(a, b, -(c + 1)), !0) : !1;
}
goog.array.binaryInsert = module$contents$goog$array_binaryInsert;
function module$contents$goog$array_binaryRemove(a, b, c) {
  b = module$contents$goog$array_binarySearch(a, b, c);
  return b >= 0 ? module$contents$goog$array_removeAt(a, b) : !1;
}
goog.array.binaryRemove = module$contents$goog$array_binaryRemove;
function module$contents$goog$array_bucket(a, b, c) {
  const d = {};
  for (let e = 0; e < a.length; e++) {
    const f = a[e], g = b.call(c, f, e, a);
    g !== void 0 && (d[g] || (d[g] = [])).push(f);
  }
  return d;
}
goog.array.bucket = module$contents$goog$array_bucket;
function module$contents$goog$array_bucketToMap(a, b) {
  const c = new Map();
  for (let d = 0; d < a.length; d++) {
    const e = a[d], f = b(e, d, a);
    if (f !== void 0) {
      let g = c.get(f);
      g || (g = [], c.set(f, g));
      g.push(e);
    }
  }
  return c;
}
goog.array.bucketToMap = module$contents$goog$array_bucketToMap;
function module$contents$goog$array_toObject(a, b, c) {
  const d = {};
  module$contents$goog$array_forEach(a, function(e, f) {
    d[b.call(c, e, f, a)] = e;
  });
  return d;
}
goog.array.toObject = module$contents$goog$array_toObject;
function module$contents$goog$array_toMap(a, b) {
  const c = new Map();
  for (let d = 0; d < a.length; d++) {
    const e = a[d];
    c.set(b(e, d, a), e);
  }
  return c;
}
goog.array.toMap = module$contents$goog$array_toMap;
function module$contents$goog$array_range(a, b, c) {
  const d = [];
  let e = 0, f = a;
  c = c || 1;
  b !== void 0 && (e = a, f = b);
  if (c * (f - e) < 0) {
    return [];
  }
  if (c > 0) {
    for (a = e; a < f; a += c) {
      d.push(a);
    }
  } else {
    for (a = e; a > f; a += c) {
      d.push(a);
    }
  }
  return d;
}
goog.array.range = module$contents$goog$array_range;
function module$contents$goog$array_repeat(a, b) {
  const c = [];
  for (let d = 0; d < b; d++) {
    c[d] = a;
  }
  return c;
}
goog.array.repeat = module$contents$goog$array_repeat;
function module$contents$goog$array_flatten(a) {
  const b = [];
  for (let d = 0; d < arguments.length; d++) {
    const e = arguments[d];
    if (Array.isArray(e)) {
      for (let f = 0; f < e.length; f += 8192) {
        var c = module$contents$goog$array_slice(e, f, f + 8192);
        c = module$contents$goog$array_flatten.apply(null, c);
        for (let g = 0; g < c.length; g++) {
          b.push(c[g]);
        }
      }
    } else {
      b.push(e);
    }
  }
  return b;
}
goog.array.flatten = module$contents$goog$array_flatten;
function module$contents$goog$array_rotate(a, b) {
  goog.asserts.assert(a.length != null);
  a.length && (b %= a.length, b > 0 ? Array.prototype.unshift.apply(a, a.splice(-b, b)) : b < 0 && Array.prototype.push.apply(a, a.splice(0, -b)));
  return a;
}
goog.array.rotate = module$contents$goog$array_rotate;
function module$contents$goog$array_moveItem(a, b, c) {
  goog.asserts.assert(b >= 0 && b < a.length);
  goog.asserts.assert(c >= 0 && c < a.length);
  b = Array.prototype.splice.call(a, b, 1);
  Array.prototype.splice.call(a, c, 0, b[0]);
}
goog.array.moveItem = module$contents$goog$array_moveItem;
function module$contents$goog$array_zip(a) {
  if (!arguments.length) {
    return [];
  }
  const b = [];
  let c = arguments[0].length;
  for (var d = 1; d < arguments.length; d++) {
    arguments[d].length < c && (c = arguments[d].length);
  }
  for (d = 0; d < c; d++) {
    const e = [];
    for (let f = 0; f < arguments.length; f++) {
      e.push(arguments[f][d]);
    }
    b.push(e);
  }
  return b;
}
goog.array.zip = module$contents$goog$array_zip;
function module$contents$goog$array_shuffle(a, b) {
  b = b || Math.random;
  for (let c = a.length - 1; c > 0; c--) {
    const d = Math.floor(b() * (c + 1)), e = a[c];
    a[c] = a[d];
    a[d] = e;
  }
}
goog.array.shuffle = module$contents$goog$array_shuffle;
function module$contents$goog$array_copyByIndex(a, b) {
  const c = [];
  module$contents$goog$array_forEach(b, function(d) {
    c.push(a[d]);
  });
  return c;
}
goog.array.copyByIndex = module$contents$goog$array_copyByIndex;
function module$contents$goog$array_concatMap(a, b, c) {
  return module$contents$goog$array_concat.apply([], module$contents$goog$array_map(a, b, c));
}
goog.array.concatMap = module$contents$goog$array_concatMap;
goog.dom.HtmlElement = function() {
};
goog.dom.TagName = class {
  static cast(a, b) {
    return a;
  }
  constructor() {
  }
  toString() {
  }
};
goog.dom.TagName.A = "A";
goog.dom.TagName.ABBR = "ABBR";
goog.dom.TagName.ACRONYM = "ACRONYM";
goog.dom.TagName.ADDRESS = "ADDRESS";
goog.dom.TagName.APPLET = "APPLET";
goog.dom.TagName.AREA = "AREA";
goog.dom.TagName.ARTICLE = "ARTICLE";
goog.dom.TagName.ASIDE = "ASIDE";
goog.dom.TagName.AUDIO = "AUDIO";
goog.dom.TagName.B = "B";
goog.dom.TagName.BASE = "BASE";
goog.dom.TagName.BASEFONT = "BASEFONT";
goog.dom.TagName.BDI = "BDI";
goog.dom.TagName.BDO = "BDO";
goog.dom.TagName.BIG = "BIG";
goog.dom.TagName.BLOCKQUOTE = "BLOCKQUOTE";
goog.dom.TagName.BODY = "BODY";
goog.dom.TagName.BR = "BR";
goog.dom.TagName.BUTTON = "BUTTON";
goog.dom.TagName.CANVAS = "CANVAS";
goog.dom.TagName.CAPTION = "CAPTION";
goog.dom.TagName.CENTER = "CENTER";
goog.dom.TagName.CITE = "CITE";
goog.dom.TagName.CODE = "CODE";
goog.dom.TagName.COL = "COL";
goog.dom.TagName.COLGROUP = "COLGROUP";
goog.dom.TagName.COMMAND = "COMMAND";
goog.dom.TagName.DATA = "DATA";
goog.dom.TagName.DATALIST = "DATALIST";
goog.dom.TagName.DD = "DD";
goog.dom.TagName.DEL = "DEL";
goog.dom.TagName.DETAILS = "DETAILS";
goog.dom.TagName.DFN = "DFN";
goog.dom.TagName.DIALOG = "DIALOG";
goog.dom.TagName.DIR = "DIR";
goog.dom.TagName.DIV = "DIV";
goog.dom.TagName.DL = "DL";
goog.dom.TagName.DT = "DT";
goog.dom.TagName.EM = "EM";
goog.dom.TagName.EMBED = "EMBED";
goog.dom.TagName.FIELDSET = "FIELDSET";
goog.dom.TagName.FIGCAPTION = "FIGCAPTION";
goog.dom.TagName.FIGURE = "FIGURE";
goog.dom.TagName.FONT = "FONT";
goog.dom.TagName.FOOTER = "FOOTER";
goog.dom.TagName.FORM = "FORM";
goog.dom.TagName.FRAME = "FRAME";
goog.dom.TagName.FRAMESET = "FRAMESET";
goog.dom.TagName.H1 = "H1";
goog.dom.TagName.H2 = "H2";
goog.dom.TagName.H3 = "H3";
goog.dom.TagName.H4 = "H4";
goog.dom.TagName.H5 = "H5";
goog.dom.TagName.H6 = "H6";
goog.dom.TagName.HEAD = "HEAD";
goog.dom.TagName.HEADER = "HEADER";
goog.dom.TagName.HGROUP = "HGROUP";
goog.dom.TagName.HR = "HR";
goog.dom.TagName.HTML = "HTML";
goog.dom.TagName.I = "I";
goog.dom.TagName.IFRAME = "IFRAME";
goog.dom.TagName.IMG = "IMG";
goog.dom.TagName.INPUT = "INPUT";
goog.dom.TagName.INS = "INS";
goog.dom.TagName.ISINDEX = "ISINDEX";
goog.dom.TagName.KBD = "KBD";
goog.dom.TagName.KEYGEN = "KEYGEN";
goog.dom.TagName.LABEL = "LABEL";
goog.dom.TagName.LEGEND = "LEGEND";
goog.dom.TagName.LI = "LI";
goog.dom.TagName.LINK = "LINK";
goog.dom.TagName.MAIN = "MAIN";
goog.dom.TagName.MAP = "MAP";
goog.dom.TagName.MARK = "MARK";
goog.dom.TagName.MATH = "MATH";
goog.dom.TagName.MENU = "MENU";
goog.dom.TagName.MENUITEM = "MENUITEM";
goog.dom.TagName.META = "META";
goog.dom.TagName.METER = "METER";
goog.dom.TagName.NAV = "NAV";
goog.dom.TagName.NOFRAMES = "NOFRAMES";
goog.dom.TagName.NOSCRIPT = "NOSCRIPT";
goog.dom.TagName.OBJECT = "OBJECT";
goog.dom.TagName.OL = "OL";
goog.dom.TagName.OPTGROUP = "OPTGROUP";
goog.dom.TagName.OPTION = "OPTION";
goog.dom.TagName.OUTPUT = "OUTPUT";
goog.dom.TagName.P = "P";
goog.dom.TagName.PARAM = "PARAM";
goog.dom.TagName.PICTURE = "PICTURE";
goog.dom.TagName.PRE = "PRE";
goog.dom.TagName.PROGRESS = "PROGRESS";
goog.dom.TagName.Q = "Q";
goog.dom.TagName.RP = "RP";
goog.dom.TagName.RT = "RT";
goog.dom.TagName.RTC = "RTC";
goog.dom.TagName.RUBY = "RUBY";
goog.dom.TagName.S = "S";
goog.dom.TagName.SAMP = "SAMP";
goog.dom.TagName.SCRIPT = "SCRIPT";
goog.dom.TagName.SECTION = "SECTION";
goog.dom.TagName.SELECT = "SELECT";
goog.dom.TagName.SMALL = "SMALL";
goog.dom.TagName.SOURCE = "SOURCE";
goog.dom.TagName.SPAN = "SPAN";
goog.dom.TagName.STRIKE = "STRIKE";
goog.dom.TagName.STRONG = "STRONG";
goog.dom.TagName.STYLE = "STYLE";
goog.dom.TagName.SUB = "SUB";
goog.dom.TagName.SUMMARY = "SUMMARY";
goog.dom.TagName.SUP = "SUP";
goog.dom.TagName.SVG = "SVG";
goog.dom.TagName.TABLE = "TABLE";
goog.dom.TagName.TBODY = "TBODY";
goog.dom.TagName.TD = "TD";
goog.dom.TagName.TEMPLATE = "TEMPLATE";
goog.dom.TagName.TEXTAREA = "TEXTAREA";
goog.dom.TagName.TFOOT = "TFOOT";
goog.dom.TagName.TH = "TH";
goog.dom.TagName.THEAD = "THEAD";
goog.dom.TagName.TIME = "TIME";
goog.dom.TagName.TITLE = "TITLE";
goog.dom.TagName.TR = "TR";
goog.dom.TagName.TRACK = "TRACK";
goog.dom.TagName.TT = "TT";
goog.dom.TagName.U = "U";
goog.dom.TagName.UL = "UL";
goog.dom.TagName.VAR = "VAR";
goog.dom.TagName.VIDEO = "VIDEO";
goog.dom.TagName.WBR = "WBR";
goog.dom.element = {};
const module$contents$goog$dom$element_HTML_NAMESPACE = "http://www.w3.org/1999/xhtml", module$contents$goog$dom$element_isElement = a => goog.isObject(a) && a.nodeType === goog.dom.NodeType.ELEMENT, module$contents$goog$dom$element_isHtmlElement = a => goog.isObject(a) && module$contents$goog$dom$element_isElement(a) && (!a.namespaceURI || a.namespaceURI === module$contents$goog$dom$element_HTML_NAMESPACE), module$contents$goog$dom$element_isHtmlElementOfType = (a, b) => goog.isObject(a) && module$contents$goog$dom$element_isHtmlElement(a) && 
a.tagName.toUpperCase() === b.toString(), module$contents$goog$dom$element_isHtmlAnchorElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.A), module$contents$goog$dom$element_isHtmlButtonElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.BUTTON), module$contents$goog$dom$element_isHtmlLinkElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.LINK), module$contents$goog$dom$element_isHtmlImageElement = 
a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.IMG), module$contents$goog$dom$element_isHtmlAudioElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.AUDIO), module$contents$goog$dom$element_isHtmlVideoElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.VIDEO), module$contents$goog$dom$element_isHtmlInputElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.INPUT), 
module$contents$goog$dom$element_isHtmlTextAreaElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.TEXTAREA), module$contents$goog$dom$element_isHtmlCanvasElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.CANVAS), module$contents$goog$dom$element_isHtmlEmbedElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.EMBED), module$contents$goog$dom$element_isHtmlFormElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, 
goog.dom.TagName.FORM), module$contents$goog$dom$element_isHtmlFrameElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.FRAME), module$contents$goog$dom$element_isHtmlIFrameElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.IFRAME), module$contents$goog$dom$element_isHtmlObjectElement = a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.OBJECT), module$contents$goog$dom$element_isHtmlScriptElement = 
a => module$contents$goog$dom$element_isHtmlElementOfType(a, goog.dom.TagName.SCRIPT);
goog.dom.element.isElement = module$contents$goog$dom$element_isElement;
goog.dom.element.isHtmlElement = module$contents$goog$dom$element_isHtmlElement;
goog.dom.element.isHtmlElementOfType = module$contents$goog$dom$element_isHtmlElementOfType;
goog.dom.element.isHtmlAnchorElement = module$contents$goog$dom$element_isHtmlAnchorElement;
goog.dom.element.isHtmlButtonElement = module$contents$goog$dom$element_isHtmlButtonElement;
goog.dom.element.isHtmlLinkElement = module$contents$goog$dom$element_isHtmlLinkElement;
goog.dom.element.isHtmlImageElement = module$contents$goog$dom$element_isHtmlImageElement;
goog.dom.element.isHtmlAudioElement = module$contents$goog$dom$element_isHtmlAudioElement;
goog.dom.element.isHtmlVideoElement = module$contents$goog$dom$element_isHtmlVideoElement;
goog.dom.element.isHtmlInputElement = module$contents$goog$dom$element_isHtmlInputElement;
goog.dom.element.isHtmlTextAreaElement = module$contents$goog$dom$element_isHtmlTextAreaElement;
goog.dom.element.isHtmlCanvasElement = module$contents$goog$dom$element_isHtmlCanvasElement;
goog.dom.element.isHtmlEmbedElement = module$contents$goog$dom$element_isHtmlEmbedElement;
goog.dom.element.isHtmlFormElement = module$contents$goog$dom$element_isHtmlFormElement;
goog.dom.element.isHtmlFrameElement = module$contents$goog$dom$element_isHtmlFrameElement;
goog.dom.element.isHtmlIFrameElement = module$contents$goog$dom$element_isHtmlIFrameElement;
goog.dom.element.isHtmlObjectElement = module$contents$goog$dom$element_isHtmlObjectElement;
goog.dom.element.isHtmlScriptElement = module$contents$goog$dom$element_isHtmlScriptElement;
goog.asserts.dom = {};
const module$contents$goog$asserts$dom_assertIsElement = a => {
  goog.asserts.ENABLE_ASSERTS && !module$contents$goog$dom$element_isElement(a) && goog.asserts.fail(`Argument is not an Element; got: ${module$contents$goog$asserts$dom_debugStringForType(a)}`);
  return a;
}, module$contents$goog$asserts$dom_assertIsHtmlElement = a => {
  goog.asserts.ENABLE_ASSERTS && !module$contents$goog$dom$element_isHtmlElement(a) && goog.asserts.fail(`Argument is not an HTML Element; got: ${module$contents$goog$asserts$dom_debugStringForType(a)}`);
  return a;
}, module$contents$goog$asserts$dom_assertIsHtmlElementOfType = (a, b) => {
  goog.asserts.ENABLE_ASSERTS && !module$contents$goog$dom$element_isHtmlElementOfType(a, b) && goog.asserts.fail("Argument is not an HTML Element with tag name " + `${b.toString()}; got: ${module$contents$goog$asserts$dom_debugStringForType(a)}`);
  return a;
}, module$contents$goog$asserts$dom_assertIsHtmlAnchorElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.A), module$contents$goog$asserts$dom_assertIsHtmlButtonElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.BUTTON), module$contents$goog$asserts$dom_assertIsHtmlLinkElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.LINK), module$contents$goog$asserts$dom_assertIsHtmlImageElement = 
a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.IMG), module$contents$goog$asserts$dom_assertIsHtmlAudioElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.AUDIO), module$contents$goog$asserts$dom_assertIsHtmlVideoElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.VIDEO), module$contents$goog$asserts$dom_assertIsHtmlInputElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, 
goog.dom.TagName.INPUT), module$contents$goog$asserts$dom_assertIsHtmlTextAreaElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.TEXTAREA), module$contents$goog$asserts$dom_assertIsHtmlCanvasElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.CANVAS), module$contents$goog$asserts$dom_assertIsHtmlEmbedElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.EMBED), module$contents$goog$asserts$dom_assertIsHtmlFormElement = 
a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.FORM), module$contents$goog$asserts$dom_assertIsHtmlFrameElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.FRAME), module$contents$goog$asserts$dom_assertIsHtmlIFrameElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.IFRAME), module$contents$goog$asserts$dom_assertIsHtmlObjectElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, 
goog.dom.TagName.OBJECT), module$contents$goog$asserts$dom_assertIsHtmlScriptElement = a => module$contents$goog$asserts$dom_assertIsHtmlElementOfType(a, goog.dom.TagName.SCRIPT), module$contents$goog$asserts$dom_debugStringForType = a => {
  if (goog.isObject(a)) {
    try {
      return a.constructor.displayName || a.constructor.name || Object.prototype.toString.call(a);
    } catch (b) {
      return "<object could not be stringified>";
    }
  } else {
    return a === void 0 ? "undefined" : a === null ? "null" : typeof a;
  }
};
goog.asserts.dom.assertIsElement = module$contents$goog$asserts$dom_assertIsElement;
goog.asserts.dom.assertIsHtmlElement = module$contents$goog$asserts$dom_assertIsHtmlElement;
goog.asserts.dom.assertIsHtmlElementOfType = module$contents$goog$asserts$dom_assertIsHtmlElementOfType;
goog.asserts.dom.assertIsHtmlAnchorElement = module$contents$goog$asserts$dom_assertIsHtmlAnchorElement;
goog.asserts.dom.assertIsHtmlButtonElement = module$contents$goog$asserts$dom_assertIsHtmlButtonElement;
goog.asserts.dom.assertIsHtmlLinkElement = module$contents$goog$asserts$dom_assertIsHtmlLinkElement;
goog.asserts.dom.assertIsHtmlImageElement = module$contents$goog$asserts$dom_assertIsHtmlImageElement;
goog.asserts.dom.assertIsHtmlAudioElement = module$contents$goog$asserts$dom_assertIsHtmlAudioElement;
goog.asserts.dom.assertIsHtmlVideoElement = module$contents$goog$asserts$dom_assertIsHtmlVideoElement;
goog.asserts.dom.assertIsHtmlInputElement = module$contents$goog$asserts$dom_assertIsHtmlInputElement;
goog.asserts.dom.assertIsHtmlTextAreaElement = module$contents$goog$asserts$dom_assertIsHtmlTextAreaElement;
goog.asserts.dom.assertIsHtmlCanvasElement = module$contents$goog$asserts$dom_assertIsHtmlCanvasElement;
goog.asserts.dom.assertIsHtmlEmbedElement = module$contents$goog$asserts$dom_assertIsHtmlEmbedElement;
goog.asserts.dom.assertIsHtmlFormElement = module$contents$goog$asserts$dom_assertIsHtmlFormElement;
goog.asserts.dom.assertIsHtmlFrameElement = module$contents$goog$asserts$dom_assertIsHtmlFrameElement;
goog.asserts.dom.assertIsHtmlIFrameElement = module$contents$goog$asserts$dom_assertIsHtmlIFrameElement;
goog.asserts.dom.assertIsHtmlObjectElement = module$contents$goog$asserts$dom_assertIsHtmlObjectElement;
goog.asserts.dom.assertIsHtmlScriptElement = module$contents$goog$asserts$dom_assertIsHtmlScriptElement;
goog.string = {};
goog.string.internal = {};
goog.string.internal.startsWith = function(a, b) {
  return a.lastIndexOf(b, 0) == 0;
};
goog.string.internal.endsWith = function(a, b) {
  const c = a.length - b.length;
  return c >= 0 && a.indexOf(b, c) == c;
};
goog.string.internal.caseInsensitiveStartsWith = function(a, b) {
  return goog.string.internal.caseInsensitiveCompare(b, a.slice(0, b.length)) == 0;
};
goog.string.internal.caseInsensitiveEndsWith = function(a, b) {
  return goog.string.internal.caseInsensitiveCompare(b, a.slice(a.length - b.length)) == 0;
};
goog.string.internal.caseInsensitiveEquals = function(a, b) {
  return a.toLowerCase() == b.toLowerCase();
};
goog.string.internal.isEmptyOrWhitespace = function(a) {
  return /^[\s\xa0]*$/.test(a);
};
goog.string.internal.trim = goog.TRUSTED_SITE && String.prototype.trim ? function(a) {
  return a.trim();
} : function(a) {
  return /^[\s\xa0]*([\s\S]*?)[\s\xa0]*$/.exec(a)[1];
};
goog.string.internal.caseInsensitiveCompare = function(a, b) {
  a = String(a).toLowerCase();
  b = String(b).toLowerCase();
  return a < b ? -1 : a == b ? 0 : 1;
};
goog.string.internal.newLineToBr = function(a, b) {
  return a.replace(/(\r\n|\r|\n)/g, b ? "<br />" : "<br>");
};
goog.string.internal.htmlEscape = function(a, b) {
  if (b) {
    a = a.replace(goog.string.internal.AMP_RE_, "&amp;").replace(goog.string.internal.LT_RE_, "&lt;").replace(goog.string.internal.GT_RE_, "&gt;").replace(goog.string.internal.QUOT_RE_, "&quot;").replace(goog.string.internal.SINGLE_QUOTE_RE_, "&#39;").replace(goog.string.internal.NULL_RE_, "&#0;");
  } else {
    if (!goog.string.internal.ALL_RE_.test(a)) {
      return a;
    }
    a.indexOf("&") != -1 && (a = a.replace(goog.string.internal.AMP_RE_, "&amp;"));
    a.indexOf("<") != -1 && (a = a.replace(goog.string.internal.LT_RE_, "&lt;"));
    a.indexOf(">") != -1 && (a = a.replace(goog.string.internal.GT_RE_, "&gt;"));
    a.indexOf('"') != -1 && (a = a.replace(goog.string.internal.QUOT_RE_, "&quot;"));
    a.indexOf("'") != -1 && (a = a.replace(goog.string.internal.SINGLE_QUOTE_RE_, "&#39;"));
    a.indexOf("\x00") != -1 && (a = a.replace(goog.string.internal.NULL_RE_, "&#0;"));
  }
  return a;
};
goog.string.internal.AMP_RE_ = /&/g;
goog.string.internal.LT_RE_ = /</g;
goog.string.internal.GT_RE_ = />/g;
goog.string.internal.QUOT_RE_ = /"/g;
goog.string.internal.SINGLE_QUOTE_RE_ = /'/g;
goog.string.internal.NULL_RE_ = /\x00/g;
goog.string.internal.ALL_RE_ = /[\x00&<>"']/;
goog.string.internal.whitespaceEscape = function(a, b) {
  return goog.string.internal.newLineToBr(a.replace(/  /g, " &#160;"), b);
};
goog.string.internal.contains = function(a, b) {
  return a.indexOf(b) != -1;
};
goog.string.internal.caseInsensitiveContains = function(a, b) {
  return goog.string.internal.contains(a.toLowerCase(), b.toLowerCase());
};
goog.string.internal.compareVersions = function(a, b) {
  var c = 0;
  a = goog.string.internal.trim(String(a)).split(".");
  b = goog.string.internal.trim(String(b)).split(".");
  const d = Math.max(a.length, b.length);
  for (let g = 0; c == 0 && g < d; g++) {
    var e = a[g] || "", f = b[g] || "";
    do {
      e = /(\d*)(\D*)(.*)/.exec(e) || ["", "", "", ""];
      f = /(\d*)(\D*)(.*)/.exec(f) || ["", "", "", ""];
      if (e[0].length == 0 && f[0].length == 0) {
        break;
      }
      c = e[1].length == 0 ? 0 : parseInt(e[1], 10);
      const h = f[1].length == 0 ? 0 : parseInt(f[1], 10);
      c = goog.string.internal.compareElements_(c, h) || goog.string.internal.compareElements_(e[2].length == 0, f[2].length == 0) || goog.string.internal.compareElements_(e[2], f[2]);
      e = e[3];
      f = f[3];
    } while (c == 0);
  }
  return c;
};
goog.string.internal.compareElements_ = function(a, b) {
  return a < b ? -1 : a > b ? 1 : 0;
};
goog.flags = {};
goog.flags.USE_USER_AGENT_CLIENT_HINTS = !1;
goog.flags.ASYNC_THROW_ON_UNICODE_TO_BYTE = !1;
goog.labs = {};
goog.labs.userAgent = {};
const module$contents$goog$labs$userAgent_USE_CLIENT_HINTS_OVERRIDE = "", module$contents$goog$labs$userAgent_USE_CLIENT_HINTS = !1;
let module$contents$goog$labs$userAgent_forceClientHintsInTests = !1;
goog.labs.userAgent.setUseClientHintsForTesting = a => {
  module$contents$goog$labs$userAgent_forceClientHintsInTests = a;
};
const module$contents$goog$labs$userAgent_useClientHintsRuntimeOverride = module$contents$goog$labs$userAgent_USE_CLIENT_HINTS_OVERRIDE ? !!goog.getObjectByName(module$contents$goog$labs$userAgent_USE_CLIENT_HINTS_OVERRIDE) : !1;
goog.labs.userAgent.useClientHints = () => goog.flags.USE_USER_AGENT_CLIENT_HINTS || module$contents$goog$labs$userAgent_USE_CLIENT_HINTS || module$contents$goog$labs$userAgent_useClientHintsRuntimeOverride || module$contents$goog$labs$userAgent_forceClientHintsInTests;
goog.labs.userAgent.util = {};
const module$contents$goog$labs$userAgent$util_ASSUME_CLIENT_HINTS_SUPPORT = !1;
function module$contents$goog$labs$userAgent$util_getNativeUserAgentString() {
  var a = module$contents$goog$labs$userAgent$util_getNavigator();
  return a && (a = a.userAgent) ? a : "";
}
function module$contents$goog$labs$userAgent$util_getNativeUserAgentData() {
  const a = module$contents$goog$labs$userAgent$util_getNavigator();
  return a ? a.userAgentData || null : null;
}
function module$contents$goog$labs$userAgent$util_getNavigator() {
  return goog.global.navigator;
}
let module$contents$goog$labs$userAgent$util_userAgentInternal = null, module$contents$goog$labs$userAgent$util_userAgentDataInternal = module$contents$goog$labs$userAgent$util_getNativeUserAgentData();
function module$contents$goog$labs$userAgent$util_setUserAgent(a) {
  module$contents$goog$labs$userAgent$util_userAgentInternal = typeof a === "string" ? a : module$contents$goog$labs$userAgent$util_getNativeUserAgentString();
}
function module$contents$goog$labs$userAgent$util_getUserAgent() {
  return module$contents$goog$labs$userAgent$util_userAgentInternal == null ? module$contents$goog$labs$userAgent$util_getNativeUserAgentString() : module$contents$goog$labs$userAgent$util_userAgentInternal;
}
function module$contents$goog$labs$userAgent$util_setUserAgentData(a) {
  module$contents$goog$labs$userAgent$util_userAgentDataInternal = a;
}
function module$contents$goog$labs$userAgent$util_resetUserAgentData() {
  module$contents$goog$labs$userAgent$util_userAgentDataInternal = module$contents$goog$labs$userAgent$util_getNativeUserAgentData();
}
function module$contents$goog$labs$userAgent$util_getUserAgentData() {
  return module$contents$goog$labs$userAgent$util_userAgentDataInternal;
}
function module$contents$goog$labs$userAgent$util_matchUserAgentDataBrand(a) {
  if (!(0,goog.labs.userAgent.useClientHints)()) {
    return !1;
  }
  const b = module$contents$goog$labs$userAgent$util_getUserAgentData();
  return b ? b.brands.some(({brand:c}) => c && (0,goog.string.internal.contains)(c, a)) : !1;
}
function module$contents$goog$labs$userAgent$util_matchUserAgent(a) {
  const b = module$contents$goog$labs$userAgent$util_getUserAgent();
  return (0,goog.string.internal.contains)(b, a);
}
function module$contents$goog$labs$userAgent$util_matchUserAgentIgnoreCase(a) {
  const b = module$contents$goog$labs$userAgent$util_getUserAgent();
  return (0,goog.string.internal.caseInsensitiveContains)(b, a);
}
function module$contents$goog$labs$userAgent$util_extractVersionTuples(a) {
  const b = RegExp("([A-Z][\\w ]+)/([^\\s]+)\\s*(?:\\((.*?)\\))?", "g"), c = [];
  let d;
  for (; d = b.exec(a);) {
    c.push([d[1], d[2], d[3] || void 0]);
  }
  return c;
}
goog.labs.userAgent.util.ASSUME_CLIENT_HINTS_SUPPORT = module$contents$goog$labs$userAgent$util_ASSUME_CLIENT_HINTS_SUPPORT;
goog.labs.userAgent.util.extractVersionTuples = module$contents$goog$labs$userAgent$util_extractVersionTuples;
goog.labs.userAgent.util.getNativeUserAgentString = module$contents$goog$labs$userAgent$util_getNativeUserAgentString;
goog.labs.userAgent.util.getUserAgent = module$contents$goog$labs$userAgent$util_getUserAgent;
goog.labs.userAgent.util.getUserAgentData = module$contents$goog$labs$userAgent$util_getUserAgentData;
goog.labs.userAgent.util.matchUserAgent = module$contents$goog$labs$userAgent$util_matchUserAgent;
goog.labs.userAgent.util.matchUserAgentDataBrand = module$contents$goog$labs$userAgent$util_matchUserAgentDataBrand;
goog.labs.userAgent.util.matchUserAgentIgnoreCase = module$contents$goog$labs$userAgent$util_matchUserAgentIgnoreCase;
goog.labs.userAgent.util.resetUserAgentData = module$contents$goog$labs$userAgent$util_resetUserAgentData;
goog.labs.userAgent.util.setUserAgent = module$contents$goog$labs$userAgent$util_setUserAgent;
goog.labs.userAgent.util.setUserAgentData = module$contents$goog$labs$userAgent$util_setUserAgentData;
var module$exports$goog$labs$userAgent$highEntropy$highEntropyValue = {AsyncValue:class {
  getIfLoaded() {
  }
  load() {
  }
}, HighEntropyValue:class {
  constructor(a) {
    this.key_ = a;
    this.promise_ = this.value_ = void 0;
    this.pending_ = !1;
  }
  getIfLoaded() {
    if (module$contents$goog$labs$userAgent$util_getUserAgentData()) {
      return this.value_;
    }
  }
  async load() {
    const a = module$contents$goog$labs$userAgent$util_getUserAgentData();
    if (a) {
      return this.promise_ || (this.pending_ = !0, this.promise_ = (async() => {
        try {
          return this.value_ = (await a.getHighEntropyValues([this.key_]))[this.key_];
        } finally {
          this.pending_ = !1;
        }
      })()), await this.promise_;
    }
  }
  resetForTesting() {
    if (this.pending_) {
      throw Error("Unsafe call to resetForTesting");
    }
    this.value_ = this.promise_ = void 0;
    this.pending_ = !1;
  }
}, Version:class {
  constructor(a) {
    this.versionString_ = a;
  }
  toVersionStringForLogging() {
    return this.versionString_;
  }
  isAtLeast(a) {
    return (0,goog.string.internal.compareVersions)(this.versionString_, a) >= 0;
  }
}};
var module$exports$goog$labs$userAgent$highEntropy$highEntropyData = {};
module$exports$goog$labs$userAgent$highEntropy$highEntropyData.fullVersionList = new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.HighEntropyValue("fullVersionList");
module$exports$goog$labs$userAgent$highEntropy$highEntropyData.platformVersion = new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.HighEntropyValue("platformVersion");
goog.labs.userAgent.browser = {};
const module$contents$goog$labs$userAgent$browser_Brand = {ANDROID_BROWSER:"Android Browser", CHROMIUM:"Chromium", EDGE:"Microsoft Edge", FIREFOX:"Firefox", IE:"Internet Explorer", OPERA:"Opera", SAFARI:"Safari", SILK:"Silk"};
goog.labs.userAgent.browser.Brand = module$contents$goog$labs$userAgent$browser_Brand;
function module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand(a = !1) {
  if (module$contents$goog$labs$userAgent$util_ASSUME_CLIENT_HINTS_SUPPORT) {
    return !0;
  }
  if (!a && !(0,goog.labs.userAgent.useClientHints)()) {
    return !1;
  }
  a = module$contents$goog$labs$userAgent$util_getUserAgentData();
  return !!a && a.brands.length > 0;
}
function module$contents$goog$labs$userAgent$browser_hasFullVersionList() {
  return module$contents$goog$labs$userAgent$browser_isAtLeast(module$contents$goog$labs$userAgent$browser_Brand.CHROMIUM, 98);
}
function module$contents$goog$labs$userAgent$browser_matchOpera() {
  return module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() ? !1 : module$contents$goog$labs$userAgent$util_matchUserAgent("Opera");
}
function module$contents$goog$labs$userAgent$browser_matchIE() {
  return module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() ? !1 : module$contents$goog$labs$userAgent$util_matchUserAgent("Trident") || module$contents$goog$labs$userAgent$util_matchUserAgent("MSIE");
}
function module$contents$goog$labs$userAgent$browser_matchEdgeHtml() {
  return module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() ? !1 : module$contents$goog$labs$userAgent$util_matchUserAgent("Edge");
}
function module$contents$goog$labs$userAgent$browser_matchEdgeChromium() {
  return module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() ? module$contents$goog$labs$userAgent$util_matchUserAgentDataBrand(module$contents$goog$labs$userAgent$browser_Brand.EDGE) : module$contents$goog$labs$userAgent$util_matchUserAgent("Edg/");
}
function module$contents$goog$labs$userAgent$browser_matchOperaChromium() {
  return module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() ? module$contents$goog$labs$userAgent$util_matchUserAgentDataBrand(module$contents$goog$labs$userAgent$browser_Brand.OPERA) : module$contents$goog$labs$userAgent$util_matchUserAgent("OPR");
}
function module$contents$goog$labs$userAgent$browser_matchFirefox() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Firefox") || module$contents$goog$labs$userAgent$util_matchUserAgent("FxiOS");
}
function module$contents$goog$labs$userAgent$browser_matchSafari() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Safari") && !(module$contents$goog$labs$userAgent$browser_matchChrome() || module$contents$goog$labs$userAgent$browser_matchCoast() || module$contents$goog$labs$userAgent$browser_matchOpera() || module$contents$goog$labs$userAgent$browser_matchEdgeHtml() || module$contents$goog$labs$userAgent$browser_matchEdgeChromium() || module$contents$goog$labs$userAgent$browser_matchOperaChromium() || module$contents$goog$labs$userAgent$browser_matchFirefox() || 
  module$contents$goog$labs$userAgent$browser_isSilk() || module$contents$goog$labs$userAgent$util_matchUserAgent("Android"));
}
function module$contents$goog$labs$userAgent$browser_matchCoast() {
  return module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() ? !1 : module$contents$goog$labs$userAgent$util_matchUserAgent("Coast");
}
function module$contents$goog$labs$userAgent$browser_matchIosWebview() {
  return (module$contents$goog$labs$userAgent$util_matchUserAgent("iPad") || module$contents$goog$labs$userAgent$util_matchUserAgent("iPhone")) && !module$contents$goog$labs$userAgent$browser_matchSafari() && !module$contents$goog$labs$userAgent$browser_matchChrome() && !module$contents$goog$labs$userAgent$browser_matchCoast() && !module$contents$goog$labs$userAgent$browser_matchFirefox() && module$contents$goog$labs$userAgent$util_matchUserAgent("AppleWebKit");
}
function module$contents$goog$labs$userAgent$browser_matchChrome() {
  return module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() ? module$contents$goog$labs$userAgent$util_matchUserAgentDataBrand(module$contents$goog$labs$userAgent$browser_Brand.CHROMIUM) : (module$contents$goog$labs$userAgent$util_matchUserAgent("Chrome") || module$contents$goog$labs$userAgent$util_matchUserAgent("CriOS")) && !module$contents$goog$labs$userAgent$browser_matchEdgeHtml() || module$contents$goog$labs$userAgent$browser_isSilk();
}
function module$contents$goog$labs$userAgent$browser_matchAndroidBrowser() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Android") && !(module$contents$goog$labs$userAgent$browser_matchChrome() || module$contents$goog$labs$userAgent$browser_matchFirefox() || module$contents$goog$labs$userAgent$browser_matchOpera() || module$contents$goog$labs$userAgent$browser_isSilk());
}
const module$contents$goog$labs$userAgent$browser_isOpera = module$contents$goog$labs$userAgent$browser_matchOpera;
goog.labs.userAgent.browser.isOpera = module$contents$goog$labs$userAgent$browser_matchOpera;
const module$contents$goog$labs$userAgent$browser_isIE = module$contents$goog$labs$userAgent$browser_matchIE;
goog.labs.userAgent.browser.isIE = module$contents$goog$labs$userAgent$browser_matchIE;
const module$contents$goog$labs$userAgent$browser_isEdge = module$contents$goog$labs$userAgent$browser_matchEdgeHtml;
goog.labs.userAgent.browser.isEdge = module$contents$goog$labs$userAgent$browser_matchEdgeHtml;
const module$contents$goog$labs$userAgent$browser_isEdgeChromium = module$contents$goog$labs$userAgent$browser_matchEdgeChromium;
goog.labs.userAgent.browser.isEdgeChromium = module$contents$goog$labs$userAgent$browser_matchEdgeChromium;
const module$contents$goog$labs$userAgent$browser_isOperaChromium = module$contents$goog$labs$userAgent$browser_matchOperaChromium;
goog.labs.userAgent.browser.isOperaChromium = module$contents$goog$labs$userAgent$browser_matchOperaChromium;
const module$contents$goog$labs$userAgent$browser_isFirefox = module$contents$goog$labs$userAgent$browser_matchFirefox;
goog.labs.userAgent.browser.isFirefox = module$contents$goog$labs$userAgent$browser_matchFirefox;
const module$contents$goog$labs$userAgent$browser_isSafari = module$contents$goog$labs$userAgent$browser_matchSafari;
goog.labs.userAgent.browser.isSafari = module$contents$goog$labs$userAgent$browser_matchSafari;
const module$contents$goog$labs$userAgent$browser_isCoast = module$contents$goog$labs$userAgent$browser_matchCoast;
goog.labs.userAgent.browser.isCoast = module$contents$goog$labs$userAgent$browser_matchCoast;
const module$contents$goog$labs$userAgent$browser_isIosWebview = module$contents$goog$labs$userAgent$browser_matchIosWebview;
goog.labs.userAgent.browser.isIosWebview = module$contents$goog$labs$userAgent$browser_matchIosWebview;
const module$contents$goog$labs$userAgent$browser_isChrome = module$contents$goog$labs$userAgent$browser_matchChrome;
goog.labs.userAgent.browser.isChrome = module$contents$goog$labs$userAgent$browser_matchChrome;
const module$contents$goog$labs$userAgent$browser_isAndroidBrowser = module$contents$goog$labs$userAgent$browser_matchAndroidBrowser;
goog.labs.userAgent.browser.isAndroidBrowser = module$contents$goog$labs$userAgent$browser_matchAndroidBrowser;
function module$contents$goog$labs$userAgent$browser_isSilk() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Silk");
}
goog.labs.userAgent.browser.isSilk = module$contents$goog$labs$userAgent$browser_isSilk;
function module$contents$goog$labs$userAgent$browser_createVersionMap(a) {
  const b = {};
  a.forEach(c => {
    b[c[0]] = c[1];
  });
  return c => b[c.find(d => d in b)] || "";
}
function module$contents$goog$labs$userAgent$browser_getVersion() {
  var a = module$contents$goog$labs$userAgent$util_getUserAgent();
  if (module$contents$goog$labs$userAgent$browser_matchIE()) {
    return module$contents$goog$labs$userAgent$browser_getIEVersion(a);
  }
  a = module$contents$goog$labs$userAgent$util_extractVersionTuples(a);
  const b = module$contents$goog$labs$userAgent$browser_createVersionMap(a);
  return module$contents$goog$labs$userAgent$browser_matchOpera() ? b(["Version", "Opera"]) : module$contents$goog$labs$userAgent$browser_matchEdgeHtml() ? b(["Edge"]) : module$contents$goog$labs$userAgent$browser_matchEdgeChromium() ? b(["Edg"]) : module$contents$goog$labs$userAgent$browser_isSilk() ? b(["Silk"]) : module$contents$goog$labs$userAgent$browser_matchChrome() ? b(["Chrome", "CriOS", "HeadlessChrome"]) : (a = a[2]) && a[1] || "";
}
goog.labs.userAgent.browser.getVersion = module$contents$goog$labs$userAgent$browser_getVersion;
function module$contents$goog$labs$userAgent$browser_isVersionOrHigher(a) {
  return (0,goog.string.internal.compareVersions)(module$contents$goog$labs$userAgent$browser_getVersion(), a) >= 0;
}
goog.labs.userAgent.browser.isVersionOrHigher = module$contents$goog$labs$userAgent$browser_isVersionOrHigher;
function module$contents$goog$labs$userAgent$browser_getIEVersion(a) {
  var b = /rv: *([\d\.]*)/.exec(a);
  if (b && b[1]) {
    return b[1];
  }
  b = "";
  const c = /MSIE +([\d\.]+)/.exec(a);
  if (c && c[1]) {
    if (a = /Trident\/(\d.\d)/.exec(a), c[1] == "7.0") {
      if (a && a[1]) {
        switch(a[1]) {
          case "4.0":
            b = "8.0";
            break;
          case "5.0":
            b = "9.0";
            break;
          case "6.0":
            b = "10.0";
            break;
          case "7.0":
            b = "11.0";
        }
      } else {
        b = "7.0";
      }
    } else {
      b = c[1];
    }
  }
  return b;
}
function module$contents$goog$labs$userAgent$browser_getFullVersionFromUserAgentString(a) {
  var b = module$contents$goog$labs$userAgent$util_getUserAgent();
  if (a === module$contents$goog$labs$userAgent$browser_Brand.IE) {
    return module$contents$goog$labs$userAgent$browser_matchIE() ? module$contents$goog$labs$userAgent$browser_getIEVersion(b) : "";
  }
  b = module$contents$goog$labs$userAgent$util_extractVersionTuples(b);
  const c = module$contents$goog$labs$userAgent$browser_createVersionMap(b);
  switch(a) {
    case module$contents$goog$labs$userAgent$browser_Brand.OPERA:
      if (module$contents$goog$labs$userAgent$browser_matchOpera()) {
        return c(["Version", "Opera"]);
      }
      if (module$contents$goog$labs$userAgent$browser_matchOperaChromium()) {
        return c(["OPR"]);
      }
      break;
    case module$contents$goog$labs$userAgent$browser_Brand.EDGE:
      if (module$contents$goog$labs$userAgent$browser_matchEdgeHtml()) {
        return c(["Edge"]);
      }
      if (module$contents$goog$labs$userAgent$browser_matchEdgeChromium()) {
        return c(["Edg"]);
      }
      break;
    case module$contents$goog$labs$userAgent$browser_Brand.CHROMIUM:
      if (module$contents$goog$labs$userAgent$browser_matchChrome()) {
        return c(["Chrome", "CriOS", "HeadlessChrome"]);
      }
  }
  return a === module$contents$goog$labs$userAgent$browser_Brand.FIREFOX && module$contents$goog$labs$userAgent$browser_matchFirefox() || a === module$contents$goog$labs$userAgent$browser_Brand.SAFARI && module$contents$goog$labs$userAgent$browser_matchSafari() || a === module$contents$goog$labs$userAgent$browser_Brand.ANDROID_BROWSER && module$contents$goog$labs$userAgent$browser_matchAndroidBrowser() || a === module$contents$goog$labs$userAgent$browser_Brand.SILK && module$contents$goog$labs$userAgent$browser_isSilk() ? 
  (a = b[2]) && a[1] || "" : "";
}
function module$contents$goog$labs$userAgent$browser_versionOf_(a) {
  if (module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand() && a !== module$contents$goog$labs$userAgent$browser_Brand.SILK) {
    var b = module$contents$goog$labs$userAgent$util_getUserAgentData().brands.find(({brand:c}) => c === a);
    if (!b || !b.version) {
      return NaN;
    }
    b = b.version.split(".");
  } else {
    b = module$contents$goog$labs$userAgent$browser_getFullVersionFromUserAgentString(a);
    if (b === "") {
      return NaN;
    }
    b = b.split(".");
  }
  return b.length === 0 ? NaN : Number(b[0]);
}
function module$contents$goog$labs$userAgent$browser_isAtLeast(a, b) {
  (0,goog.asserts.assert)(Math.floor(b) === b, "Major version must be an integer");
  return module$contents$goog$labs$userAgent$browser_versionOf_(a) >= b;
}
goog.labs.userAgent.browser.isAtLeast = module$contents$goog$labs$userAgent$browser_isAtLeast;
function module$contents$goog$labs$userAgent$browser_isAtMost(a, b) {
  (0,goog.asserts.assert)(Math.floor(b) === b, "Major version must be an integer");
  return module$contents$goog$labs$userAgent$browser_versionOf_(a) <= b;
}
goog.labs.userAgent.browser.isAtMost = module$contents$goog$labs$userAgent$browser_isAtMost;
class module$contents$goog$labs$userAgent$browser_HighEntropyBrandVersion {
  constructor(a, b, c) {
    this.brand_ = a;
    this.version_ = new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.Version(c);
    this.useUach_ = b;
  }
  getIfLoaded() {
    if (this.useUach_) {
      var a = module$exports$goog$labs$userAgent$highEntropy$highEntropyData.fullVersionList.getIfLoaded();
      if (a !== void 0) {
        return a = a.find(({brand:b}) => this.brand_ === b), (0,goog.asserts.assertExists)(a), new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.Version(a.version);
      }
    }
    if (module$contents$goog$labs$userAgent$browser_preUachHasLoaded) {
      return this.version_;
    }
  }
  async load() {
    if (this.useUach_) {
      var a = await module$exports$goog$labs$userAgent$highEntropy$highEntropyData.fullVersionList.load();
      if (a !== void 0) {
        return a = a.find(({brand:b}) => this.brand_ === b), (0,goog.asserts.assertExists)(a), new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.Version(a.version);
      }
    } else {
      await 0;
    }
    module$contents$goog$labs$userAgent$browser_preUachHasLoaded = !0;
    return this.version_;
  }
}
let module$contents$goog$labs$userAgent$browser_preUachHasLoaded = !1;
async function module$contents$goog$labs$userAgent$browser_loadFullVersions() {
  module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand(!0) && await module$exports$goog$labs$userAgent$highEntropy$highEntropyData.fullVersionList.load();
  module$contents$goog$labs$userAgent$browser_preUachHasLoaded = !0;
}
goog.labs.userAgent.browser.loadFullVersions = module$contents$goog$labs$userAgent$browser_loadFullVersions;
goog.labs.userAgent.browser.resetForTesting = () => {
  module$contents$goog$labs$userAgent$browser_preUachHasLoaded = !1;
  module$exports$goog$labs$userAgent$highEntropy$highEntropyData.fullVersionList.resetForTesting();
};
function module$contents$goog$labs$userAgent$browser_fullVersionOf(a) {
  let b = "";
  module$contents$goog$labs$userAgent$browser_hasFullVersionList() || (b = module$contents$goog$labs$userAgent$browser_getFullVersionFromUserAgentString(a));
  const c = a !== module$contents$goog$labs$userAgent$browser_Brand.SILK && module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand(!0);
  if (c) {
    if (!module$contents$goog$labs$userAgent$util_getUserAgentData().brands.find(({brand:d}) => d === a)) {
      return;
    }
  } else if (b === "") {
    return;
  }
  return new module$contents$goog$labs$userAgent$browser_HighEntropyBrandVersion(a, c, b);
}
goog.labs.userAgent.browser.fullVersionOf = module$contents$goog$labs$userAgent$browser_fullVersionOf;
function module$contents$goog$labs$userAgent$browser_getVersionStringForLogging(a) {
  if (module$contents$goog$labs$userAgent$browser_useUserAgentDataBrand(!0)) {
    var b = module$contents$goog$labs$userAgent$browser_fullVersionOf(a);
    if (b) {
      if (b = b.getIfLoaded()) {
        return b.toVersionStringForLogging();
      }
      b = module$contents$goog$labs$userAgent$util_getUserAgentData().brands.find(({brand:c}) => c === a);
      (0,goog.asserts.assertExists)(b);
      return b.version;
    }
    return "";
  }
  return module$contents$goog$labs$userAgent$browser_getFullVersionFromUserAgentString(a);
}
goog.labs.userAgent.browser.getVersionStringForLogging = module$contents$goog$labs$userAgent$browser_getVersionStringForLogging;
goog.labs.userAgent.engine = {};
function module$contents$goog$labs$userAgent$engine_isPresto() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Presto");
}
function module$contents$goog$labs$userAgent$engine_isTrident() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Trident") || module$contents$goog$labs$userAgent$util_matchUserAgent("MSIE");
}
function module$contents$goog$labs$userAgent$engine_isEdge() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Edge");
}
function module$contents$goog$labs$userAgent$engine_isWebKit() {
  return module$contents$goog$labs$userAgent$util_matchUserAgentIgnoreCase("WebKit") && !module$contents$goog$labs$userAgent$engine_isEdge();
}
function module$contents$goog$labs$userAgent$engine_isGecko() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("Gecko") && !module$contents$goog$labs$userAgent$engine_isWebKit() && !module$contents$goog$labs$userAgent$engine_isTrident() && !module$contents$goog$labs$userAgent$engine_isEdge();
}
function module$contents$goog$labs$userAgent$engine_getVersion() {
  var a = module$contents$goog$labs$userAgent$util_getUserAgent();
  if (a) {
    a = module$contents$goog$labs$userAgent$util_extractVersionTuples(a);
    const c = module$contents$goog$labs$userAgent$engine_getEngineTuple(a);
    if (c) {
      return c[0] == "Gecko" ? module$contents$goog$labs$userAgent$engine_getVersionForKey(a, "Firefox") : c[1];
    }
    a = a[0];
    var b;
    if (a && (b = a[2]) && (b = /Trident\/([^\s;]+)/.exec(b))) {
      return b[1];
    }
  }
  return "";
}
function module$contents$goog$labs$userAgent$engine_getEngineTuple(a) {
  if (!module$contents$goog$labs$userAgent$engine_isEdge()) {
    return a[1];
  }
  for (let b = 0; b < a.length; b++) {
    const c = a[b];
    if (c[0] == "Edge") {
      return c;
    }
  }
}
function module$contents$goog$labs$userAgent$engine_isVersionOrHigher(a) {
  return goog.string.internal.compareVersions(module$contents$goog$labs$userAgent$engine_getVersion(), a) >= 0;
}
function module$contents$goog$labs$userAgent$engine_getVersionForKey(a, b) {
  return (a = module$contents$goog$array_find(a, function(c) {
    return b == c[0];
  })) && a[1] || "";
}
goog.labs.userAgent.engine.getVersion = module$contents$goog$labs$userAgent$engine_getVersion;
goog.labs.userAgent.engine.isEdge = module$contents$goog$labs$userAgent$engine_isEdge;
goog.labs.userAgent.engine.isGecko = module$contents$goog$labs$userAgent$engine_isGecko;
goog.labs.userAgent.engine.isPresto = module$contents$goog$labs$userAgent$engine_isPresto;
goog.labs.userAgent.engine.isTrident = module$contents$goog$labs$userAgent$engine_isTrident;
goog.labs.userAgent.engine.isVersionOrHigher = module$contents$goog$labs$userAgent$engine_isVersionOrHigher;
goog.labs.userAgent.engine.isWebKit = module$contents$goog$labs$userAgent$engine_isWebKit;
goog.labs.userAgent.platform = {};
function module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform(a = !1) {
  if (module$contents$goog$labs$userAgent$util_ASSUME_CLIENT_HINTS_SUPPORT) {
    return !0;
  }
  if (!a && !(0,goog.labs.userAgent.useClientHints)()) {
    return !1;
  }
  a = module$contents$goog$labs$userAgent$util_getUserAgentData();
  return !!a && !!a.platform;
}
function module$contents$goog$labs$userAgent$platform_isAndroid() {
  return module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform() ? module$contents$goog$labs$userAgent$util_getUserAgentData().platform === "Android" : module$contents$goog$labs$userAgent$util_matchUserAgent("Android");
}
function module$contents$goog$labs$userAgent$platform_isIpod() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("iPod");
}
function module$contents$goog$labs$userAgent$platform_isIphone() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("iPhone") && !module$contents$goog$labs$userAgent$util_matchUserAgent("iPod") && !module$contents$goog$labs$userAgent$util_matchUserAgent("iPad");
}
function module$contents$goog$labs$userAgent$platform_isIpad() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("iPad");
}
function module$contents$goog$labs$userAgent$platform_isIos() {
  return module$contents$goog$labs$userAgent$platform_isIphone() || module$contents$goog$labs$userAgent$platform_isIpad() || module$contents$goog$labs$userAgent$platform_isIpod();
}
function module$contents$goog$labs$userAgent$platform_isMacintosh() {
  return module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform() ? module$contents$goog$labs$userAgent$util_getUserAgentData().platform === "macOS" : module$contents$goog$labs$userAgent$util_matchUserAgent("Macintosh");
}
function module$contents$goog$labs$userAgent$platform_isLinux() {
  return module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform() ? module$contents$goog$labs$userAgent$util_getUserAgentData().platform === "Linux" : module$contents$goog$labs$userAgent$util_matchUserAgent("Linux");
}
function module$contents$goog$labs$userAgent$platform_isWindows() {
  return module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform() ? module$contents$goog$labs$userAgent$util_getUserAgentData().platform === "Windows" : module$contents$goog$labs$userAgent$util_matchUserAgent("Windows");
}
function module$contents$goog$labs$userAgent$platform_isChromeOS() {
  return module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform() ? module$contents$goog$labs$userAgent$util_getUserAgentData().platform === "Chrome OS" : module$contents$goog$labs$userAgent$util_matchUserAgent("CrOS");
}
function module$contents$goog$labs$userAgent$platform_isChromecast() {
  return module$contents$goog$labs$userAgent$util_matchUserAgent("CrKey");
}
function module$contents$goog$labs$userAgent$platform_isKaiOS() {
  return module$contents$goog$labs$userAgent$util_matchUserAgentIgnoreCase("KaiOS");
}
function module$contents$goog$labs$userAgent$platform_getVersion() {
  var a = module$contents$goog$labs$userAgent$util_getUserAgent(), b = "";
  module$contents$goog$labs$userAgent$platform_isWindows() ? (b = /Windows (?:NT|Phone) ([0-9.]+)/, b = (a = b.exec(a)) ? a[1] : "0.0") : module$contents$goog$labs$userAgent$platform_isIos() ? (b = /(?:iPhone|iPod|iPad|CPU)\s+OS\s+(\S+)/, b = (a = b.exec(a)) && a[1].replace(/_/g, ".")) : module$contents$goog$labs$userAgent$platform_isMacintosh() ? (b = /Mac OS X ([0-9_.]+)/, b = (a = b.exec(a)) ? a[1].replace(/_/g, ".") : "10") : module$contents$goog$labs$userAgent$platform_isKaiOS() ? (b = /(?:KaiOS)\/(\S+)/i, 
  b = (a = b.exec(a)) && a[1]) : module$contents$goog$labs$userAgent$platform_isAndroid() ? (b = /Android\s+([^\);]+)(\)|;)/, b = (a = b.exec(a)) && a[1]) : module$contents$goog$labs$userAgent$platform_isChromeOS() && (b = /(?:CrOS\s+(?:i686|x86_64)\s+([0-9.]+))/, b = (a = b.exec(a)) && a[1]);
  return b || "";
}
function module$contents$goog$labs$userAgent$platform_isVersionOrHigher(a) {
  return goog.string.internal.compareVersions(module$contents$goog$labs$userAgent$platform_getVersion(), a) >= 0;
}
class module$contents$goog$labs$userAgent$platform_PlatformVersion {
  constructor() {
    this.preUachHasLoaded_ = !1;
  }
  getIfLoaded() {
    if (module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform(!0)) {
      const a = module$exports$goog$labs$userAgent$highEntropy$highEntropyData.platformVersion.getIfLoaded();
      return a === void 0 ? void 0 : new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.Version(a);
    }
    if (this.preUachHasLoaded_) {
      return new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.Version(module$contents$goog$labs$userAgent$platform_getVersion());
    }
  }
  async load() {
    if (module$contents$goog$labs$userAgent$platform_useUserAgentDataPlatform(!0)) {
      return new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.Version(await module$exports$goog$labs$userAgent$highEntropy$highEntropyData.platformVersion.load());
    }
    this.preUachHasLoaded_ = !0;
    return new module$exports$goog$labs$userAgent$highEntropy$highEntropyValue.Version(module$contents$goog$labs$userAgent$platform_getVersion());
  }
  resetForTesting() {
    module$exports$goog$labs$userAgent$highEntropy$highEntropyData.platformVersion.resetForTesting();
    this.preUachHasLoaded_ = !1;
  }
}
const module$contents$goog$labs$userAgent$platform_version = new module$contents$goog$labs$userAgent$platform_PlatformVersion();
goog.labs.userAgent.platform.getVersion = module$contents$goog$labs$userAgent$platform_getVersion;
goog.labs.userAgent.platform.isAndroid = module$contents$goog$labs$userAgent$platform_isAndroid;
goog.labs.userAgent.platform.isChromeOS = module$contents$goog$labs$userAgent$platform_isChromeOS;
goog.labs.userAgent.platform.isChromecast = module$contents$goog$labs$userAgent$platform_isChromecast;
goog.labs.userAgent.platform.isIos = module$contents$goog$labs$userAgent$platform_isIos;
goog.labs.userAgent.platform.isIpad = module$contents$goog$labs$userAgent$platform_isIpad;
goog.labs.userAgent.platform.isIphone = module$contents$goog$labs$userAgent$platform_isIphone;
goog.labs.userAgent.platform.isIpod = module$contents$goog$labs$userAgent$platform_isIpod;
goog.labs.userAgent.platform.isKaiOS = module$contents$goog$labs$userAgent$platform_isKaiOS;
goog.labs.userAgent.platform.isLinux = module$contents$goog$labs$userAgent$platform_isLinux;
goog.labs.userAgent.platform.isMacintosh = module$contents$goog$labs$userAgent$platform_isMacintosh;
goog.labs.userAgent.platform.isVersionOrHigher = module$contents$goog$labs$userAgent$platform_isVersionOrHigher;
goog.labs.userAgent.platform.isWindows = module$contents$goog$labs$userAgent$platform_isWindows;
goog.labs.userAgent.platform.version = module$contents$goog$labs$userAgent$platform_version;
goog.reflect = {};
goog.reflect.object = function(a, b) {
  return b;
};
goog.reflect.objectProperty = function(a, b) {
  return a;
};
goog.reflect.sinkValue = function(a) {
  goog.reflect.sinkValue[" "](a);
  return a;
};
goog.reflect.sinkValue[" "] = function() {
};
goog.reflect.canAccessProperty = function(a, b) {
  try {
    return goog.reflect.sinkValue(a[b]), !0;
  } catch (c) {
  }
  return !1;
};
goog.reflect.cache = function(a, b, c, d) {
  d = d ? d(b) : b;
  return Object.prototype.hasOwnProperty.call(a, d) ? a[d] : a[d] = c(b);
};
goog.userAgent = {};
goog.userAgent.ASSUME_IE = !1;
goog.userAgent.ASSUME_EDGE = !1;
goog.userAgent.ASSUME_GECKO = !1;
goog.userAgent.ASSUME_WEBKIT = !1;
goog.userAgent.ASSUME_MOBILE_WEBKIT = !1;
goog.userAgent.ASSUME_OPERA = !1;
goog.userAgent.ASSUME_ANY_VERSION = !1;
goog.userAgent.BROWSER_KNOWN_ = goog.userAgent.ASSUME_IE || goog.userAgent.ASSUME_EDGE || goog.userAgent.ASSUME_GECKO || goog.userAgent.ASSUME_MOBILE_WEBKIT || goog.userAgent.ASSUME_WEBKIT || goog.userAgent.ASSUME_OPERA;
goog.userAgent.getUserAgentString = function() {
  return module$contents$goog$labs$userAgent$util_getUserAgent();
};
goog.userAgent.getNavigatorTyped = function() {
  return goog.global.navigator || null;
};
goog.userAgent.getNavigator = function() {
  return goog.userAgent.getNavigatorTyped();
};
goog.userAgent.OPERA = goog.userAgent.BROWSER_KNOWN_ ? goog.userAgent.ASSUME_OPERA : module$contents$goog$labs$userAgent$browser_matchOpera();
goog.userAgent.IE = goog.userAgent.BROWSER_KNOWN_ ? goog.userAgent.ASSUME_IE : module$contents$goog$labs$userAgent$browser_matchIE();
goog.userAgent.EDGE = goog.userAgent.BROWSER_KNOWN_ ? goog.userAgent.ASSUME_EDGE : module$contents$goog$labs$userAgent$engine_isEdge();
goog.userAgent.EDGE_OR_IE = goog.userAgent.EDGE || goog.userAgent.IE;
goog.userAgent.GECKO = goog.userAgent.BROWSER_KNOWN_ ? goog.userAgent.ASSUME_GECKO : module$contents$goog$labs$userAgent$engine_isGecko();
goog.userAgent.WEBKIT = goog.userAgent.BROWSER_KNOWN_ ? goog.userAgent.ASSUME_WEBKIT || goog.userAgent.ASSUME_MOBILE_WEBKIT : module$contents$goog$labs$userAgent$engine_isWebKit();
goog.userAgent.isMobile_ = function() {
  return goog.userAgent.WEBKIT && module$contents$goog$labs$userAgent$util_matchUserAgent("Mobile");
};
goog.userAgent.MOBILE = goog.userAgent.ASSUME_MOBILE_WEBKIT || goog.userAgent.isMobile_();
goog.userAgent.SAFARI = goog.userAgent.WEBKIT;
goog.userAgent.determinePlatform_ = function() {
  var a = goog.userAgent.getNavigatorTyped();
  return a && a.platform || "";
};
goog.userAgent.PLATFORM = goog.userAgent.determinePlatform_();
goog.userAgent.ASSUME_MAC = !1;
goog.userAgent.ASSUME_WINDOWS = !1;
goog.userAgent.ASSUME_LINUX = !1;
goog.userAgent.ASSUME_X11 = !1;
goog.userAgent.ASSUME_ANDROID = !1;
goog.userAgent.ASSUME_IPHONE = !1;
goog.userAgent.ASSUME_IPAD = !1;
goog.userAgent.ASSUME_IPOD = !1;
goog.userAgent.ASSUME_KAIOS = !1;
goog.userAgent.PLATFORM_KNOWN_ = goog.userAgent.ASSUME_MAC || goog.userAgent.ASSUME_WINDOWS || goog.userAgent.ASSUME_LINUX || goog.userAgent.ASSUME_X11 || goog.userAgent.ASSUME_ANDROID || goog.userAgent.ASSUME_IPHONE || goog.userAgent.ASSUME_IPAD || goog.userAgent.ASSUME_IPOD;
goog.userAgent.MAC = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_MAC : module$contents$goog$labs$userAgent$platform_isMacintosh();
goog.userAgent.WINDOWS = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_WINDOWS : module$contents$goog$labs$userAgent$platform_isWindows();
goog.userAgent.isLegacyLinux_ = function() {
  return module$contents$goog$labs$userAgent$platform_isLinux() || module$contents$goog$labs$userAgent$platform_isChromeOS();
};
goog.userAgent.LINUX = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_LINUX : goog.userAgent.isLegacyLinux_();
goog.userAgent.isX11_ = function() {
  var a = goog.userAgent.getNavigatorTyped();
  return !!a && goog.string.internal.contains(a.appVersion || "", "X11");
};
goog.userAgent.X11 = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_X11 : goog.userAgent.isX11_();
goog.userAgent.ANDROID = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_ANDROID : module$contents$goog$labs$userAgent$platform_isAndroid();
goog.userAgent.IPHONE = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_IPHONE : module$contents$goog$labs$userAgent$platform_isIphone();
goog.userAgent.IPAD = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_IPAD : module$contents$goog$labs$userAgent$platform_isIpad();
goog.userAgent.IPOD = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_IPOD : module$contents$goog$labs$userAgent$platform_isIpod();
goog.userAgent.IOS = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_IPHONE || goog.userAgent.ASSUME_IPAD || goog.userAgent.ASSUME_IPOD : module$contents$goog$labs$userAgent$platform_isIos();
goog.userAgent.KAIOS = goog.userAgent.PLATFORM_KNOWN_ ? goog.userAgent.ASSUME_KAIOS : module$contents$goog$labs$userAgent$platform_isKaiOS();
goog.userAgent.determineVersion_ = function() {
  var a = "", b = goog.userAgent.getVersionRegexResult_();
  b && (a = b ? b[1] : "");
  return goog.userAgent.IE && (b = goog.userAgent.getDocumentMode_(), b != null && b > parseFloat(a)) ? String(b) : a;
};
goog.userAgent.getVersionRegexResult_ = function() {
  var a = goog.userAgent.getUserAgentString();
  if (goog.userAgent.GECKO) {
    return /rv:([^\);]+)(\)|;)/.exec(a);
  }
  if (goog.userAgent.EDGE) {
    return /Edge\/([\d\.]+)/.exec(a);
  }
  if (goog.userAgent.IE) {
    return /\b(?:MSIE|rv)[: ]([^\);]+)(\)|;)/.exec(a);
  }
  if (goog.userAgent.WEBKIT) {
    return /WebKit\/(\S+)/.exec(a);
  }
  if (goog.userAgent.OPERA) {
    return /(?:Version)[ \/]?(\S+)/.exec(a);
  }
};
goog.userAgent.getDocumentMode_ = function() {
  var a = goog.global.document;
  return a ? a.documentMode : void 0;
};
goog.userAgent.VERSION = goog.userAgent.determineVersion_();
goog.userAgent.compare = function(a, b) {
  return goog.string.internal.compareVersions(a, b);
};
goog.userAgent.isVersionOrHigherCache_ = {};
goog.userAgent.isVersionOrHigher = function(a) {
  return goog.userAgent.ASSUME_ANY_VERSION || goog.reflect.cache(goog.userAgent.isVersionOrHigherCache_, a, function() {
    return goog.string.internal.compareVersions(goog.userAgent.VERSION, a) >= 0;
  });
};
goog.userAgent.isDocumentModeOrHigher = function(a) {
  return Number(goog.userAgent.DOCUMENT_MODE) >= a;
};
goog.userAgent.isDocumentMode = goog.userAgent.isDocumentModeOrHigher;
goog.userAgent.DOCUMENT_MODE = function() {
  if (goog.global.document && goog.userAgent.IE) {
    var a = goog.userAgent.getDocumentMode_();
    return a ? a : parseInt(goog.userAgent.VERSION, 10) || void 0;
  }
}();
goog.dom.BrowserFeature = {};
goog.dom.BrowserFeature.ASSUME_NO_OFFSCREEN_CANVAS = !1;
goog.dom.BrowserFeature.ASSUME_OFFSCREEN_CANVAS = !1;
goog.dom.BrowserFeature.detectOffscreenCanvas_ = function(a) {
  try {
    return !!(new self.OffscreenCanvas(0, 0)).getContext(a);
  } catch (b) {
  }
  return !1;
};
goog.dom.BrowserFeature.OFFSCREEN_CANVAS_2D = !goog.dom.BrowserFeature.ASSUME_NO_OFFSCREEN_CANVAS && (goog.dom.BrowserFeature.ASSUME_OFFSCREEN_CANVAS || goog.dom.BrowserFeature.detectOffscreenCanvas_("2d"));
goog.dom.BrowserFeature.CAN_ADD_NAME_OR_TYPE_ATTRIBUTES = !0;
goog.dom.BrowserFeature.CAN_USE_CHILDREN_ATTRIBUTE = !0;
goog.dom.BrowserFeature.CAN_USE_INNER_TEXT = !1;
goog.dom.BrowserFeature.CAN_USE_PARENT_ELEMENT_PROPERTY = goog.userAgent.IE || goog.userAgent.WEBKIT;
goog.dom.BrowserFeature.INNER_HTML_NEEDS_SCOPED_ELEMENT = goog.userAgent.IE;
goog.dom.asserts = {};
goog.dom.asserts.assertIsLocation = function(a) {
  if (goog.asserts.ENABLE_ASSERTS) {
    var b = goog.dom.asserts.getWindow_(a);
    b && (!a || !(a instanceof b.Location) && a instanceof b.Element) && goog.asserts.fail("Argument is not a Location (or a non-Element mock); got: %s", goog.dom.asserts.debugStringForType_(a));
  }
  return a;
};
goog.dom.asserts.debugStringForType_ = function(a) {
  if (goog.isObject(a)) {
    try {
      return a.constructor.displayName || a.constructor.name || Object.prototype.toString.call(a);
    } catch (b) {
      return "<object could not be stringified>";
    }
  } else {
    return a === void 0 ? "undefined" : a === null ? "null" : typeof a;
  }
};
goog.dom.asserts.getWindow_ = function(a) {
  try {
    var b = a && a.ownerDocument, c = b && (b.defaultView || b.parentWindow);
    c = c || goog.global;
    if (c.Element && c.Location) {
      return c;
    }
  } catch (d) {
  }
  return null;
};
goog.functions = {};
goog.functions.constant = function(a) {
  return function() {
    return a;
  };
};
goog.functions.FALSE = function() {
  return !1;
};
goog.functions.TRUE = function() {
  return !0;
};
goog.functions.NULL = function() {
  return null;
};
goog.functions.UNDEFINED = function() {
};
goog.functions.EMPTY = goog.functions.UNDEFINED;
goog.functions.identity = function(a, b) {
  return a;
};
goog.functions.error = function(a) {
  return function() {
    throw Error(a);
  };
};
goog.functions.fail = function(a) {
  return function() {
    throw a;
  };
};
goog.functions.lock = function(a, b) {
  b = b || 0;
  return function() {
    return a.apply(this, Array.prototype.slice.call(arguments, 0, b));
  };
};
goog.functions.nth = function(a) {
  return function() {
    return arguments[a];
  };
};
goog.functions.partialRight = function(a, b) {
  const c = Array.prototype.slice.call(arguments, 1);
  return function() {
    let d = this;
    d === goog.global && (d = void 0);
    const e = Array.prototype.slice.call(arguments);
    e.push.apply(e, c);
    return a.apply(d, e);
  };
};
goog.functions.withReturnValue = function(a, b) {
  return goog.functions.sequence(a, goog.functions.constant(b));
};
goog.functions.equalTo = function(a, b) {
  return function(c) {
    return b ? a == c : a === c;
  };
};
goog.functions.compose = function(a, b) {
  const c = arguments, d = c.length;
  return function() {
    let e;
    d && (e = c[d - 1].apply(this, arguments));
    for (let f = d - 2; f >= 0; f--) {
      e = c[f].call(this, e);
    }
    return e;
  };
};
goog.functions.sequence = function(a) {
  const b = arguments, c = b.length;
  return function() {
    let d;
    for (let e = 0; e < c; e++) {
      d = b[e].apply(this, arguments);
    }
    return d;
  };
};
goog.functions.and = function(a) {
  const b = arguments, c = b.length;
  return function() {
    for (let d = 0; d < c; d++) {
      if (!b[d].apply(this, arguments)) {
        return !1;
      }
    }
    return !0;
  };
};
goog.functions.or = function(a) {
  const b = arguments, c = b.length;
  return function() {
    for (let d = 0; d < c; d++) {
      if (b[d].apply(this, arguments)) {
        return !0;
      }
    }
    return !1;
  };
};
goog.functions.not = function(a) {
  return function() {
    return !a.apply(this, arguments);
  };
};
goog.functions.create = function(a, b) {
  var c = function() {
  };
  c.prototype = a.prototype;
  c = new c();
  a.apply(c, Array.prototype.slice.call(arguments, 1));
  return c;
};
goog.functions.CACHE_RETURN_VALUE = !0;
goog.functions.cacheReturnValue = function(a) {
  let b = !1, c;
  return function() {
    if (!goog.functions.CACHE_RETURN_VALUE) {
      return a();
    }
    b || (c = a(), b = !0);
    return c;
  };
};
goog.functions.once = function(a) {
  let b = a;
  return function() {
    if (b) {
      const c = b;
      b = null;
      c();
    }
  };
};
goog.functions.debounce = function(a, b, c) {
  let d = 0;
  return function(e) {
    goog.global.clearTimeout(d);
    const f = arguments;
    d = goog.global.setTimeout(function() {
      a.apply(c, f);
    }, b);
  };
};
goog.functions.throttle = function(a, b, c) {
  let d = 0, e = !1, f = [];
  const g = function() {
    d = 0;
    e && (e = !1, h());
  }, h = function() {
    d = goog.global.setTimeout(g, b);
    let k = f;
    f = [];
    a.apply(c, k);
  };
  return function(k) {
    f = arguments;
    d ? e = !0 : h();
  };
};
goog.functions.rateLimit = function(a, b, c) {
  let d = 0;
  const e = function() {
    d = 0;
  };
  return function(f) {
    d || (d = goog.global.setTimeout(e, b), a.apply(c, arguments));
  };
};
goog.functions.isFunction = a => typeof a === "function";
goog.string.TypedString = function() {
};
goog.string.Const = function(a, b) {
  this.stringConstValueWithSecurityContract__googStringSecurityPrivate_ = a === goog.string.Const.GOOG_STRING_CONSTRUCTOR_TOKEN_PRIVATE_ && b || "";
  this.STRING_CONST_TYPE_MARKER__GOOG_STRING_SECURITY_PRIVATE_ = goog.string.Const.TYPE_MARKER_;
};
goog.string.Const.prototype.implementsGoogStringTypedString = !0;
goog.string.Const.prototype.getTypedStringValue = function() {
  return this.stringConstValueWithSecurityContract__googStringSecurityPrivate_;
};
goog.DEBUG && (goog.string.Const.prototype.toString = function() {
  return "Const{" + this.stringConstValueWithSecurityContract__googStringSecurityPrivate_ + "}";
});
goog.string.Const.unwrap = function(a) {
  if (a instanceof goog.string.Const && a.constructor === goog.string.Const && a.STRING_CONST_TYPE_MARKER__GOOG_STRING_SECURITY_PRIVATE_ === goog.string.Const.TYPE_MARKER_) {
    return a.stringConstValueWithSecurityContract__googStringSecurityPrivate_;
  }
  goog.asserts.fail("expected object of type Const, got '" + a + "'");
  return "type_error:Const";
};
goog.string.Const.from = function(a) {
  return new goog.string.Const(goog.string.Const.GOOG_STRING_CONSTRUCTOR_TOKEN_PRIVATE_, a);
};
goog.string.Const.TYPE_MARKER_ = {};
goog.string.Const.GOOG_STRING_CONSTRUCTOR_TOKEN_PRIVATE_ = {};
goog.string.Const.EMPTY = goog.string.Const.from("");
goog.html = {};
goog.html.trustedtypes = {};
goog.html.trustedtypes.POLICY_NAME = goog.TRUSTED_TYPES_POLICY_NAME ? goog.TRUSTED_TYPES_POLICY_NAME + "#html" : "";
goog.html.trustedtypes.getPolicyPrivateDoNotAccessOrElse = function() {
  if (!goog.html.trustedtypes.POLICY_NAME) {
    return null;
  }
  goog.html.trustedtypes.cachedPolicy_ === void 0 && (goog.html.trustedtypes.cachedPolicy_ = goog.createTrustedTypesPolicy(goog.html.trustedtypes.POLICY_NAME));
  return goog.html.trustedtypes.cachedPolicy_;
};
const module$contents$goog$html$SafeScript_CONSTRUCTOR_TOKEN_PRIVATE = {};
class module$contents$goog$html$SafeScript_SafeScript {
  constructor(a, b) {
    if (goog.DEBUG && b !== module$contents$goog$html$SafeScript_CONSTRUCTOR_TOKEN_PRIVATE) {
      throw Error("SafeScript is not meant to be built directly");
    }
    this.privateDoNotAccessOrElseSafeScriptWrappedValue_ = a;
    this.implementsGoogStringTypedString = !0;
  }
  toString() {
    return this.privateDoNotAccessOrElseSafeScriptWrappedValue_.toString();
  }
  static fromConstant(a) {
    a = goog.string.Const.unwrap(a);
    return a.length === 0 ? module$contents$goog$html$SafeScript_SafeScript.EMPTY : module$contents$goog$html$SafeScript_SafeScript.createSafeScriptSecurityPrivateDoNotAccessOrElse(a);
  }
  static fromJson(a) {
    return module$contents$goog$html$SafeScript_SafeScript.createSafeScriptSecurityPrivateDoNotAccessOrElse(module$contents$goog$html$SafeScript_SafeScript.stringify_(a));
  }
  getTypedStringValue() {
    return this.privateDoNotAccessOrElseSafeScriptWrappedValue_.toString();
  }
  static unwrap(a) {
    return module$contents$goog$html$SafeScript_SafeScript.unwrapTrustedScript(a).toString();
  }
  static unwrapTrustedScript(a) {
    if (a instanceof module$contents$goog$html$SafeScript_SafeScript && a.constructor === module$contents$goog$html$SafeScript_SafeScript) {
      return a.privateDoNotAccessOrElseSafeScriptWrappedValue_;
    }
    (0,goog.asserts.fail)("expected object of type SafeScript, got '" + a + "' of type " + goog.typeOf(a));
    return "type_error:SafeScript";
  }
  static stringify_(a) {
    return JSON.stringify(a).replace(/</g, "\\x3c");
  }
  static createSafeScriptSecurityPrivateDoNotAccessOrElse(a) {
    const b = goog.html.trustedtypes.getPolicyPrivateDoNotAccessOrElse();
    a = b ? b.createScript(a) : a;
    return new module$contents$goog$html$SafeScript_SafeScript(a, module$contents$goog$html$SafeScript_CONSTRUCTOR_TOKEN_PRIVATE);
  }
}
module$contents$goog$html$SafeScript_SafeScript.EMPTY = function() {
  return module$contents$goog$html$SafeScript_SafeScript.createSafeScriptSecurityPrivateDoNotAccessOrElse("");
}();
goog.html.SafeScript = module$contents$goog$html$SafeScript_SafeScript;
goog.fs = {};
goog.fs.url = {};
goog.fs.url.createObjectUrl = function(a) {
  return goog.fs.url.getUrlObject_().createObjectURL(a);
};
goog.fs.url.revokeObjectUrl = function(a) {
  goog.fs.url.getUrlObject_().revokeObjectURL(a);
};
goog.fs.url.UrlObject_ = function() {
};
goog.fs.url.UrlObject_.prototype.createObjectURL = function(a) {
};
goog.fs.url.UrlObject_.prototype.revokeObjectURL = function(a) {
};
goog.fs.url.getUrlObject_ = function() {
  const a = goog.fs.url.findUrlObject_();
  if (a != null) {
    return a;
  }
  throw Error("This browser doesn't seem to support blob URLs");
};
goog.fs.url.findUrlObject_ = function() {
  return goog.global.URL !== void 0 && goog.global.URL.createObjectURL !== void 0 ? goog.global.URL : goog.global.createObjectURL !== void 0 ? goog.global : null;
};
goog.fs.url.browserSupportsObjectUrls = function() {
  return goog.fs.url.findUrlObject_() != null;
};
goog.fs.blob = {};
goog.fs.blob.getBlob = function(a) {
  var b = goog.global.BlobBuilder || goog.global.WebKitBlobBuilder;
  if (b !== void 0) {
    b = new b();
    for (let c = 0; c < arguments.length; c++) {
      b.append(arguments[c]);
    }
    return b.getBlob();
  }
  return goog.fs.blob.getBlobWithProperties(Array.prototype.slice.call(arguments));
};
goog.fs.blob.getBlobWithProperties = function(a, b, c) {
  var d = goog.global.BlobBuilder || goog.global.WebKitBlobBuilder;
  if (d !== void 0) {
    d = new d();
    for (let e = 0; e < a.length; e++) {
      d.append(a[e], c);
    }
    return d.getBlob(b);
  }
  if (goog.global.Blob !== void 0) {
    return d = {}, b && (d.type = b), c && (d.endings = c), new Blob(a, d);
  }
  throw Error("This browser doesn't seem to support creating Blobs");
};
goog.html.TrustedResourceUrl = class {
  constructor(a, b) {
    if (goog.DEBUG && b !== goog.html.TrustedResourceUrl.CONSTRUCTOR_TOKEN_PRIVATE_) {
      throw Error("TrustedResourceUrl is not meant to be built directly");
    }
    this.privateDoNotAccessOrElseTrustedResourceUrlWrappedValue_ = a;
  }
  toString() {
    return this.privateDoNotAccessOrElseTrustedResourceUrlWrappedValue_ + "";
  }
};
goog.html.TrustedResourceUrl.prototype.implementsGoogStringTypedString = !0;
goog.html.TrustedResourceUrl.prototype.getTypedStringValue = function() {
  return this.privateDoNotAccessOrElseTrustedResourceUrlWrappedValue_.toString();
};
goog.html.TrustedResourceUrl.prototype.cloneWithParams = function(a, b) {
  var c = goog.html.TrustedResourceUrl.unwrap(this);
  c = goog.html.TrustedResourceUrl.URL_PARAM_PARSER_.exec(c);
  var d = c[3] || "";
  return goog.html.TrustedResourceUrl.createTrustedResourceUrlSecurityPrivateDoNotAccessOrElse(c[1] + goog.html.TrustedResourceUrl.stringifyParams_("?", c[2] || "", a) + goog.html.TrustedResourceUrl.stringifyParams_("#", d, b));
};
goog.html.TrustedResourceUrl.unwrap = function(a) {
  return goog.html.TrustedResourceUrl.unwrapTrustedScriptURL(a).toString();
};
goog.html.TrustedResourceUrl.unwrapTrustedScriptURL = function(a) {
  if (a instanceof goog.html.TrustedResourceUrl && a.constructor === goog.html.TrustedResourceUrl) {
    return a.privateDoNotAccessOrElseTrustedResourceUrlWrappedValue_;
  }
  goog.asserts.fail("expected object of type TrustedResourceUrl, got '" + a + "' of type " + goog.typeOf(a));
  return "type_error:TrustedResourceUrl";
};
goog.html.TrustedResourceUrl.format = function(a, b) {
  var c = goog.string.Const.unwrap(a);
  if (!goog.html.TrustedResourceUrl.BASE_URL_.test(c)) {
    throw Error("Invalid TrustedResourceUrl format: " + c);
  }
  a = c.replace(goog.html.TrustedResourceUrl.FORMAT_MARKER_, function(d, e) {
    if (!Object.prototype.hasOwnProperty.call(b, e)) {
      throw Error('Found marker, "' + e + '", in format string, "' + c + '", but no valid label mapping found in args: ' + JSON.stringify(b));
    }
    d = b[e];
    return d instanceof goog.string.Const ? goog.string.Const.unwrap(d) : encodeURIComponent(String(d));
  });
  return goog.html.TrustedResourceUrl.createTrustedResourceUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.TrustedResourceUrl.FORMAT_MARKER_ = /%{(\w+)}/g;
goog.html.TrustedResourceUrl.BASE_URL_ = RegExp("^((https:)?//[0-9a-z.:[\\]-]+/|/[^/\\\\]|[^:/\\\\%]+/|[^:/\\\\%]*[?#]|about:blank#)", "i");
goog.html.TrustedResourceUrl.URL_PARAM_PARSER_ = /^([^?#]*)(\?[^#]*)?(#[\s\S]*)?/;
goog.html.TrustedResourceUrl.formatWithParams = function(a, b, c, d) {
  return goog.html.TrustedResourceUrl.format(a, b).cloneWithParams(c, d);
};
goog.html.TrustedResourceUrl.fromConstant = function(a) {
  return goog.html.TrustedResourceUrl.createTrustedResourceUrlSecurityPrivateDoNotAccessOrElse(goog.string.Const.unwrap(a));
};
goog.html.TrustedResourceUrl.fromConstants = function(a) {
  for (var b = "", c = 0; c < a.length; c++) {
    b += goog.string.Const.unwrap(a[c]);
  }
  return goog.html.TrustedResourceUrl.createTrustedResourceUrlSecurityPrivateDoNotAccessOrElse(b);
};
goog.html.TrustedResourceUrl.fromSafeScript = function(a) {
  a = goog.fs.blob.getBlobWithProperties([module$contents$goog$html$SafeScript_SafeScript.unwrap(a)], "text/javascript");
  a = goog.fs.url.createObjectUrl(a);
  return goog.html.TrustedResourceUrl.createTrustedResourceUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.TrustedResourceUrl.CONSTRUCTOR_TOKEN_PRIVATE_ = {};
goog.html.TrustedResourceUrl.createTrustedResourceUrlSecurityPrivateDoNotAccessOrElse = function(a) {
  const b = goog.html.trustedtypes.getPolicyPrivateDoNotAccessOrElse();
  a = b ? b.createScriptURL(a) : a;
  return new goog.html.TrustedResourceUrl(a, goog.html.TrustedResourceUrl.CONSTRUCTOR_TOKEN_PRIVATE_);
};
goog.html.TrustedResourceUrl.stringifyParams_ = function(a, b, c) {
  if (c == null) {
    return b;
  }
  if (typeof c === "string") {
    return c ? a + encodeURIComponent(c) : "";
  }
  for (var d in c) {
    if (Object.prototype.hasOwnProperty.call(c, d)) {
      var e = c[d];
      e = Array.isArray(e) ? e : [e];
      for (var f = 0; f < e.length; f++) {
        var g = e[f];
        g != null && (b ||= a, b += (b.length > a.length ? "&" : "") + encodeURIComponent(d) + "=" + encodeURIComponent(String(g)));
      }
    }
  }
  return b;
};
goog.html.SafeUrl = class {
  constructor(a, b) {
    if (goog.DEBUG && b !== goog.html.SafeUrl.CONSTRUCTOR_TOKEN_PRIVATE_) {
      throw Error("SafeUrl is not meant to be built directly");
    }
    this.privateDoNotAccessOrElseSafeUrlWrappedValue_ = a;
  }
  toString() {
    return this.privateDoNotAccessOrElseSafeUrlWrappedValue_.toString();
  }
};
goog.html.SafeUrl.INNOCUOUS_STRING = "about:invalid#zClosurez";
goog.html.SafeUrl.prototype.implementsGoogStringTypedString = !0;
goog.html.SafeUrl.prototype.getTypedStringValue = function() {
  return this.privateDoNotAccessOrElseSafeUrlWrappedValue_.toString();
};
goog.html.SafeUrl.unwrap = function(a) {
  if (a instanceof goog.html.SafeUrl && a.constructor === goog.html.SafeUrl) {
    return a.privateDoNotAccessOrElseSafeUrlWrappedValue_;
  }
  goog.asserts.fail("expected object of type SafeUrl, got '" + a + "' of type " + goog.typeOf(a));
  return "type_error:SafeUrl";
};
goog.html.SafeUrl.fromConstant = function(a) {
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(goog.string.Const.unwrap(a));
};
goog.html.SAFE_MIME_TYPE_PATTERN_ = RegExp('^(?:audio/(?:3gpp2|3gpp|aac|L16|midi|mp3|mp4|mpeg|oga|ogg|opus|x-m4a|x-matroska|x-wav|wav|webm)|font/\\w+|image/(?:bmp|gif|jpeg|jpg|png|tiff|webp|x-icon|heic|heif)|video/(?:mpeg|mp4|ogg|webm|quicktime|x-matroska))(?:;\\w+=(?:\\w+|"[\\w;,= ]+"))*$', "i");
goog.html.SafeUrl.isSafeMimeType = function(a) {
  return goog.html.SAFE_MIME_TYPE_PATTERN_.test(a);
};
goog.html.SafeUrl.fromBlob = function(a) {
  a = goog.html.SafeUrl.isSafeMimeType(a.type) ? goog.fs.url.createObjectUrl(a) : goog.html.SafeUrl.INNOCUOUS_STRING;
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.revokeObjectUrl = function(a) {
  a = a.getTypedStringValue();
  a !== goog.html.SafeUrl.INNOCUOUS_STRING && goog.fs.url.revokeObjectUrl(a);
};
goog.html.SafeUrl.fromMediaSource = function(a) {
  goog.asserts.assert("MediaSource" in goog.global, "No support for MediaSource");
  a = a instanceof MediaSource ? goog.fs.url.createObjectUrl(a) : goog.html.SafeUrl.INNOCUOUS_STRING;
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.DATA_URL_PATTERN_ = /^data:(.*);base64,[a-z0-9+\/]+=*$/i;
goog.html.SafeUrl.tryFromDataUrl = function(a) {
  a = String(a);
  a = a.replace(/(%0A|%0D)/g, "");
  return a.match(goog.html.DATA_URL_PATTERN_) ? goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a) : null;
};
goog.html.SafeUrl.fromDataUrl = function(a) {
  return goog.html.SafeUrl.tryFromDataUrl(a) || goog.html.SafeUrl.INNOCUOUS_URL;
};
goog.html.SafeUrl.fromTelUrl = function(a) {
  goog.string.internal.caseInsensitiveStartsWith(a, "tel:") || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SIP_URL_PATTERN_ = RegExp("^sip[s]?:[+a-z0-9_.!$%&'*\\/=^`{|}~-]+@([a-z0-9-]+\\.)+[a-z0-9]{2,63}$", "i");
goog.html.SafeUrl.fromSipUrl = function(a) {
  goog.html.SIP_URL_PATTERN_.test(decodeURIComponent(a)) || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.fromFacebookMessengerUrl = function(a) {
  goog.string.internal.caseInsensitiveStartsWith(a, "fb-messenger://share") || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.fromWhatsAppUrl = function(a) {
  goog.string.internal.caseInsensitiveStartsWith(a, "whatsapp://send") || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.fromSmsUrl = function(a) {
  goog.string.internal.caseInsensitiveStartsWith(a, "sms:") && goog.html.SafeUrl.isSmsUrlBodyValid_(a) || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.isSmsUrlBodyValid_ = function(a) {
  var b = a.indexOf("#");
  b > 0 && (a = a.substring(0, b));
  b = a.match(/[?&]body=/gi);
  if (!b) {
    return !0;
  }
  if (b.length > 1) {
    return !1;
  }
  a = a.match(/[?&]body=([^&]*)/)[1];
  if (!a) {
    return !0;
  }
  try {
    decodeURIComponent(a);
  } catch (c) {
    return !1;
  }
  return /^(?:[a-z0-9\-_.~]|%[0-9a-f]{2})+$/i.test(a);
};
goog.html.SafeUrl.fromSshUrl = function(a) {
  goog.string.internal.caseInsensitiveStartsWith(a, "ssh://") || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.sanitizeChromeExtensionUrl = function(a, b) {
  return goog.html.SafeUrl.sanitizeExtensionUrl_(/^chrome-extension:\/\/([^\/]+)\//, a, b);
};
goog.html.SafeUrl.sanitizeFirefoxExtensionUrl = function(a, b) {
  return goog.html.SafeUrl.sanitizeExtensionUrl_(/^moz-extension:\/\/([^\/]+)\//, a, b);
};
goog.html.SafeUrl.sanitizeEdgeExtensionUrl = function(a, b) {
  return goog.html.SafeUrl.sanitizeExtensionUrl_(/^ms-browser-extension:\/\/([^\/]+)\//, a, b);
};
goog.html.SafeUrl.sanitizeExtensionUrl_ = function(a, b, c) {
  (a = a.exec(b)) ? (a = a[1], (c instanceof goog.string.Const ? [goog.string.Const.unwrap(c)] : c.map(function(d) {
    return goog.string.Const.unwrap(d);
  })).indexOf(a) == -1 && (b = goog.html.SafeUrl.INNOCUOUS_STRING)) : b = goog.html.SafeUrl.INNOCUOUS_STRING;
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(b);
};
goog.html.SafeUrl.fromTrustedResourceUrl = function(a) {
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(goog.html.TrustedResourceUrl.unwrap(a));
};
goog.html.SAFE_URL_PATTERN_ = /^(?:(?:https?|mailto|ftp):|[^:/?#]*(?:[/?#]|$))/i;
goog.html.SafeUrl.SAFE_URL_PATTERN = goog.html.SAFE_URL_PATTERN_;
goog.html.SafeUrl.trySanitize = function(a) {
  if (a instanceof goog.html.SafeUrl) {
    return a;
  }
  a = typeof a == "object" && a.implementsGoogStringTypedString ? a.getTypedStringValue() : String(a);
  return goog.html.SAFE_URL_PATTERN_.test(a) ? goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a) : goog.html.SafeUrl.tryFromDataUrl(a);
};
goog.html.SafeUrl.sanitize = function(a) {
  return goog.html.SafeUrl.trySanitize(a) || goog.html.SafeUrl.INNOCUOUS_URL;
};
goog.html.SafeUrl.sanitizeAssertUnchanged = function(a, b) {
  if (a instanceof goog.html.SafeUrl) {
    return a;
  }
  a = typeof a == "object" && a.implementsGoogStringTypedString ? a.getTypedStringValue() : String(a);
  if (b && /^data:/i.test(a) && (b = goog.html.SafeUrl.fromDataUrl(a), b.getTypedStringValue() == a)) {
    return b;
  }
  goog.asserts.assert(goog.html.SAFE_URL_PATTERN_.test(a), "%s does not match the safe URL pattern", a) || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.extractScheme = function(a) {
  let b;
  try {
    b = new URL(a);
  } catch (c) {
    return "https:";
  }
  return b.protocol;
};
goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged = function(a) {
  if (a instanceof goog.html.SafeUrl) {
    return a;
  }
  a = typeof a == "object" && a.implementsGoogStringTypedString ? a.getTypedStringValue() : String(a);
  const b = goog.html.SafeUrl.extractScheme(a);
  goog.asserts.assert(b !== "javascript:", "%s is a javascript: URL", a) || (a = goog.html.SafeUrl.INNOCUOUS_STRING);
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(a);
};
goog.html.SafeUrl.CONSTRUCTOR_TOKEN_PRIVATE_ = {};
goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse = function(a) {
  return new goog.html.SafeUrl(a, goog.html.SafeUrl.CONSTRUCTOR_TOKEN_PRIVATE_);
};
goog.html.SafeUrl.INNOCUOUS_URL = goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(goog.html.SafeUrl.INNOCUOUS_STRING);
goog.html.SafeUrl.ABOUT_BLANK = goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse("about:blank");
const module$contents$goog$html$SafeStyle_CONSTRUCTOR_TOKEN_PRIVATE = {};
class module$contents$goog$html$SafeStyle_SafeStyle {
  constructor(a, b) {
    if (goog.DEBUG && b !== module$contents$goog$html$SafeStyle_CONSTRUCTOR_TOKEN_PRIVATE) {
      throw Error("SafeStyle is not meant to be built directly");
    }
    this.privateDoNotAccessOrElseSafeStyleWrappedValue_ = a;
    this.implementsGoogStringTypedString = !0;
  }
  static fromConstant(a) {
    a = goog.string.Const.unwrap(a);
    if (a.length === 0) {
      return module$contents$goog$html$SafeStyle_SafeStyle.EMPTY;
    }
    (0,goog.asserts.assert)((0,goog.string.internal.endsWith)(a, ";"), `Last character of style string is not ';': ${a}`);
    (0,goog.asserts.assert)((0,goog.string.internal.contains)(a, ":"), "Style string must contain at least one ':', to specify a \"name: value\" pair: " + a);
    return module$contents$goog$html$SafeStyle_SafeStyle.createSafeStyleSecurityPrivateDoNotAccessOrElse(a);
  }
  getTypedStringValue() {
    return this.privateDoNotAccessOrElseSafeStyleWrappedValue_;
  }
  toString() {
    return this.privateDoNotAccessOrElseSafeStyleWrappedValue_.toString();
  }
  static unwrap(a) {
    if (a instanceof module$contents$goog$html$SafeStyle_SafeStyle && a.constructor === module$contents$goog$html$SafeStyle_SafeStyle) {
      return a.privateDoNotAccessOrElseSafeStyleWrappedValue_;
    }
    (0,goog.asserts.fail)(`expected object of type SafeStyle, got '${a}` + "' of type " + goog.typeOf(a));
    return "type_error:SafeStyle";
  }
  static createSafeStyleSecurityPrivateDoNotAccessOrElse(a) {
    return new module$contents$goog$html$SafeStyle_SafeStyle(a, module$contents$goog$html$SafeStyle_CONSTRUCTOR_TOKEN_PRIVATE);
  }
  static create(a) {
    let b = "";
    for (let c in a) {
      if (Object.prototype.hasOwnProperty.call(a, c)) {
        if (!/^[-_a-zA-Z0-9]+$/.test(c)) {
          throw Error(`Name allows only [-_a-zA-Z0-9], got: ${c}`);
        }
        let d = a[c];
        d != null && (d = Array.isArray(d) ? d.map(module$contents$goog$html$SafeStyle_sanitizePropertyValue).join(" ") : module$contents$goog$html$SafeStyle_sanitizePropertyValue(d), b += `${c}:${d};`);
      }
    }
    return b ? module$contents$goog$html$SafeStyle_SafeStyle.createSafeStyleSecurityPrivateDoNotAccessOrElse(b) : module$contents$goog$html$SafeStyle_SafeStyle.EMPTY;
  }
  static concat(a) {
    let b = "";
    const c = d => {
      Array.isArray(d) ? d.forEach(c) : b += module$contents$goog$html$SafeStyle_SafeStyle.unwrap(d);
    };
    Array.prototype.forEach.call(arguments, c);
    return b ? module$contents$goog$html$SafeStyle_SafeStyle.createSafeStyleSecurityPrivateDoNotAccessOrElse(b) : module$contents$goog$html$SafeStyle_SafeStyle.EMPTY;
  }
}
module$contents$goog$html$SafeStyle_SafeStyle.EMPTY = module$contents$goog$html$SafeStyle_SafeStyle.createSafeStyleSecurityPrivateDoNotAccessOrElse("");
module$contents$goog$html$SafeStyle_SafeStyle.INNOCUOUS_STRING = "zClosurez";
function module$contents$goog$html$SafeStyle_sanitizePropertyValue(a) {
  if (a instanceof goog.html.SafeUrl) {
    return 'url("' + goog.html.SafeUrl.unwrap(a).replace(/</g, "%3c").replace(/[\\"]/g, "\\$&") + '")';
  }
  a = a instanceof goog.string.Const ? goog.string.Const.unwrap(a) : module$contents$goog$html$SafeStyle_sanitizePropertyValueString(String(a));
  if (/[{;}]/.test(a)) {
    throw new module$contents$goog$asserts_AssertionError("Value does not allow [{;}], got: %s.", [a]);
  }
  return a;
}
function module$contents$goog$html$SafeStyle_sanitizePropertyValueString(a) {
  const b = a.replace(module$contents$goog$html$SafeStyle_FUNCTIONS_RE, "$1").replace(module$contents$goog$html$SafeStyle_FUNCTIONS_RE, "$1").replace(module$contents$goog$html$SafeStyle_URL_RE, "url");
  if (module$contents$goog$html$SafeStyle_VALUE_RE.test(b)) {
    if (module$contents$goog$html$SafeStyle_COMMENT_RE.test(a)) {
      return (0,goog.asserts.fail)(`String value disallows comments, got: ${a}`), module$contents$goog$html$SafeStyle_SafeStyle.INNOCUOUS_STRING;
    }
    if (!module$contents$goog$html$SafeStyle_hasBalancedQuotes(a)) {
      return (0,goog.asserts.fail)(`String value requires balanced quotes, got: ${a}`), module$contents$goog$html$SafeStyle_SafeStyle.INNOCUOUS_STRING;
    }
    if (!module$contents$goog$html$SafeStyle_hasBalancedSquareBrackets(a)) {
      return (0,goog.asserts.fail)("String value requires balanced square brackets and one identifier per pair of brackets, got: " + a), module$contents$goog$html$SafeStyle_SafeStyle.INNOCUOUS_STRING;
    }
  } else {
    return (0,goog.asserts.fail)(`String value allows only ${module$contents$goog$html$SafeStyle_VALUE_ALLOWED_CHARS}` + " and simple functions, got: " + a), module$contents$goog$html$SafeStyle_SafeStyle.INNOCUOUS_STRING;
  }
  return module$contents$goog$html$SafeStyle_sanitizeUrl(a);
}
function module$contents$goog$html$SafeStyle_hasBalancedQuotes(a) {
  let b = !0, c = !0;
  for (let d = 0; d < a.length; d++) {
    const e = a.charAt(d);
    e == "'" && c ? b = !b : e == '"' && b && (c = !c);
  }
  return b && c;
}
function module$contents$goog$html$SafeStyle_hasBalancedSquareBrackets(a) {
  let b = !0;
  const c = /^[-_a-zA-Z0-9]$/;
  for (let d = 0; d < a.length; d++) {
    const e = a.charAt(d);
    if (e == "]") {
      if (b) {
        return !1;
      }
      b = !0;
    } else if (e == "[") {
      if (!b) {
        return !1;
      }
      b = !1;
    } else if (!b && !c.test(e)) {
      return !1;
    }
  }
  return b;
}
const module$contents$goog$html$SafeStyle_VALUE_ALLOWED_CHARS = "[-+,.\"'%_!#/ a-zA-Z0-9\\[\\]]", module$contents$goog$html$SafeStyle_VALUE_RE = new RegExp(`^${module$contents$goog$html$SafeStyle_VALUE_ALLOWED_CHARS}+\$`), module$contents$goog$html$SafeStyle_URL_RE = RegExp("\\b(url\\([ \t\n]*)('[ -&(-\\[\\]-~]*'|\"[ !#-\\[\\]-~]*\"|[!#-&*-\\[\\]-~]*)([ \t\n]*\\))", "g"), module$contents$goog$html$SafeStyle_ALLOWED_FUNCTIONS = "calc cubic-bezier fit-content hsl hsla linear-gradient matrix minmax radial-gradient repeat rgb rgba (rotate|scale|translate)(X|Y|Z|3d)? steps var".split(" "), 
module$contents$goog$html$SafeStyle_FUNCTIONS_RE = new RegExp("\\b(" + module$contents$goog$html$SafeStyle_ALLOWED_FUNCTIONS.join("|") + ")\\([-+*/0-9a-zA-Z.%#\\[\\], ]+\\)", "g"), module$contents$goog$html$SafeStyle_COMMENT_RE = /\/\*/;
function module$contents$goog$html$SafeStyle_sanitizeUrl(a) {
  return a.replace(module$contents$goog$html$SafeStyle_URL_RE, (b, c, d, e) => {
    let f = "";
    d = d.replace(/^(['"])(.*)\1$/, (g, h, k) => {
      f = h;
      return k;
    });
    b = goog.html.SafeUrl.sanitize(d).getTypedStringValue();
    return c + f + b + f + e;
  });
}
goog.html.SafeStyle = module$contents$goog$html$SafeStyle_SafeStyle;
goog.object = {};
function module$contents$goog$object_forEach(a, b, c) {
  for (const d in a) {
    b.call(c, a[d], d, a);
  }
}
function module$contents$goog$object_filter(a, b, c) {
  const d = {};
  for (const e in a) {
    b.call(c, a[e], e, a) && (d[e] = a[e]);
  }
  return d;
}
function module$contents$goog$object_map(a, b, c) {
  const d = {};
  for (const e in a) {
    d[e] = b.call(c, a[e], e, a);
  }
  return d;
}
function module$contents$goog$object_some(a, b, c) {
  for (const d in a) {
    if (b.call(c, a[d], d, a)) {
      return !0;
    }
  }
  return !1;
}
function module$contents$goog$object_every(a, b, c) {
  for (const d in a) {
    if (!b.call(c, a[d], d, a)) {
      return !1;
    }
  }
  return !0;
}
function module$contents$goog$object_getCount(a) {
  let b = 0;
  for (const c in a) {
    b++;
  }
  return b;
}
function module$contents$goog$object_getAnyKey(a) {
  for (const b in a) {
    return b;
  }
}
function module$contents$goog$object_getAnyValue(a) {
  for (const b in a) {
    return a[b];
  }
}
function module$contents$goog$object_contains(a, b) {
  return module$contents$goog$object_containsValue(a, b);
}
function module$contents$goog$object_getValues(a) {
  const b = [];
  let c = 0;
  for (const d in a) {
    b[c++] = a[d];
  }
  return b;
}
function module$contents$goog$object_getKeys(a) {
  const b = [];
  let c = 0;
  for (const d in a) {
    b[c++] = d;
  }
  return b;
}
function module$contents$goog$object_getValueByKeys(a, b) {
  var c = goog.isArrayLike(b);
  const d = c ? b : arguments;
  for (c = c ? 0 : 1; c < d.length; c++) {
    if (a == null) {
      return;
    }
    a = a[d[c]];
  }
  return a;
}
function module$contents$goog$object_containsKey(a, b) {
  return a !== null && b in a;
}
function module$contents$goog$object_containsValue(a, b) {
  for (const c in a) {
    if (a[c] == b) {
      return !0;
    }
  }
  return !1;
}
function module$contents$goog$object_findKey(a, b, c) {
  for (const d in a) {
    if (b.call(c, a[d], d, a)) {
      return d;
    }
  }
}
function module$contents$goog$object_findValue(a, b, c) {
  return (b = module$contents$goog$object_findKey(a, b, c)) && a[b];
}
function module$contents$goog$object_isEmpty(a) {
  for (const b in a) {
    return !1;
  }
  return !0;
}
function module$contents$goog$object_clear(a) {
  for (const b in a) {
    delete a[b];
  }
}
function module$contents$goog$object_remove(a, b) {
  let c;
  (c = b in a) && delete a[b];
  return c;
}
function module$contents$goog$object_add(a, b, c) {
  if (a !== null && b in a) {
    throw Error(`The object already contains the key "${b}"`);
  }
  module$contents$goog$object_set(a, b, c);
}
function module$contents$goog$object_get(a, b, c) {
  return a !== null && b in a ? a[b] : c;
}
function module$contents$goog$object_set(a, b, c) {
  a[b] = c;
}
function module$contents$goog$object_setIfUndefined(a, b, c) {
  return b in a ? a[b] : a[b] = c;
}
function module$contents$goog$object_setWithReturnValueIfNotSet(a, b, c) {
  if (b in a) {
    return a[b];
  }
  c = c();
  return a[b] = c;
}
function module$contents$goog$object_equals(a, b) {
  for (const c in a) {
    if (!(c in b) || a[c] !== b[c]) {
      return !1;
    }
  }
  for (const c in b) {
    if (!(c in a)) {
      return !1;
    }
  }
  return !0;
}
function module$contents$goog$object_clone(a) {
  const b = {};
  for (const c in a) {
    b[c] = a[c];
  }
  return b;
}
function module$contents$goog$object_unsafeClone(a) {
  if (!a || typeof a !== "object") {
    return a;
  }
  if (typeof a.clone === "function") {
    return a.clone();
  }
  if (typeof Map !== "undefined" && a instanceof Map) {
    return new Map(a);
  }
  if (typeof Set !== "undefined" && a instanceof Set) {
    return new Set(a);
  }
  if (a instanceof Date) {
    return new Date(a.getTime());
  }
  const b = Array.isArray(a) ? [] : typeof ArrayBuffer !== "function" || typeof ArrayBuffer.isView !== "function" || !ArrayBuffer.isView(a) || a instanceof DataView ? {} : new a.constructor(a.length);
  for (const c in a) {
    b[c] = module$contents$goog$object_unsafeClone(a[c]);
  }
  return b;
}
function module$contents$goog$object_transpose(a) {
  const b = {};
  for (const c in a) {
    b[a[c]] = c;
  }
  return b;
}
const module$contents$goog$object_PROTOTYPE_FIELDS = "constructor hasOwnProperty isPrototypeOf propertyIsEnumerable toLocaleString toString valueOf".split(" ");
function module$contents$goog$object_extend(a, b) {
  let c, d;
  for (let e = 1; e < arguments.length; e++) {
    d = arguments[e];
    for (c in d) {
      a[c] = d[c];
    }
    for (let f = 0; f < module$contents$goog$object_PROTOTYPE_FIELDS.length; f++) {
      c = module$contents$goog$object_PROTOTYPE_FIELDS[f], Object.prototype.hasOwnProperty.call(d, c) && (a[c] = d[c]);
    }
  }
}
function module$contents$goog$object_create(a) {
  const b = arguments.length;
  if (b == 1 && Array.isArray(arguments[0])) {
    return module$contents$goog$object_create.apply(null, arguments[0]);
  }
  if (b % 2) {
    throw Error("Uneven number of arguments");
  }
  const c = {};
  for (let d = 0; d < b; d += 2) {
    c[arguments[d]] = arguments[d + 1];
  }
  return c;
}
function module$contents$goog$object_createSet(a) {
  const b = arguments.length;
  if (b == 1 && Array.isArray(arguments[0])) {
    return module$contents$goog$object_createSet.apply(null, arguments[0]);
  }
  const c = {};
  for (let d = 0; d < b; d++) {
    c[arguments[d]] = !0;
  }
  return c;
}
function module$contents$goog$object_createImmutableView(a) {
  let b = a;
  Object.isFrozen && !Object.isFrozen(a) && (b = Object.create(a), Object.freeze(b));
  return b;
}
function module$contents$goog$object_isImmutableView(a) {
  return !!Object.isFrozen && Object.isFrozen(a);
}
function module$contents$goog$object_getAllPropertyNames(a, b, c) {
  if (!a) {
    return [];
  }
  if (!Object.getOwnPropertyNames || !Object.getPrototypeOf) {
    return module$contents$goog$object_getKeys(a);
  }
  const d = {};
  for (; a && (a !== Object.prototype || b) && (a !== Function.prototype || c);) {
    const e = Object.getOwnPropertyNames(a);
    for (let f = 0; f < e.length; f++) {
      d[e[f]] = !0;
    }
    a = Object.getPrototypeOf(a);
  }
  return module$contents$goog$object_getKeys(d);
}
function module$contents$goog$object_getSuperClass(a) {
  return (a = Object.getPrototypeOf(a.prototype)) && a.constructor;
}
goog.object.add = module$contents$goog$object_add;
goog.object.clear = module$contents$goog$object_clear;
goog.object.clone = module$contents$goog$object_clone;
goog.object.contains = module$contents$goog$object_contains;
goog.object.containsKey = module$contents$goog$object_containsKey;
goog.object.containsValue = module$contents$goog$object_containsValue;
goog.object.create = module$contents$goog$object_create;
goog.object.createImmutableView = module$contents$goog$object_createImmutableView;
goog.object.createSet = module$contents$goog$object_createSet;
goog.object.equals = module$contents$goog$object_equals;
goog.object.every = module$contents$goog$object_every;
goog.object.extend = module$contents$goog$object_extend;
goog.object.filter = module$contents$goog$object_filter;
goog.object.findKey = module$contents$goog$object_findKey;
goog.object.findValue = module$contents$goog$object_findValue;
goog.object.forEach = module$contents$goog$object_forEach;
goog.object.get = module$contents$goog$object_get;
goog.object.getAllPropertyNames = module$contents$goog$object_getAllPropertyNames;
goog.object.getAnyKey = module$contents$goog$object_getAnyKey;
goog.object.getAnyValue = module$contents$goog$object_getAnyValue;
goog.object.getCount = module$contents$goog$object_getCount;
goog.object.getKeys = module$contents$goog$object_getKeys;
goog.object.getSuperClass = module$contents$goog$object_getSuperClass;
goog.object.getValueByKeys = module$contents$goog$object_getValueByKeys;
goog.object.getValues = module$contents$goog$object_getValues;
goog.object.isEmpty = module$contents$goog$object_isEmpty;
goog.object.isImmutableView = module$contents$goog$object_isImmutableView;
goog.object.map = module$contents$goog$object_map;
goog.object.remove = module$contents$goog$object_remove;
goog.object.set = module$contents$goog$object_set;
goog.object.setIfUndefined = module$contents$goog$object_setIfUndefined;
goog.object.setWithReturnValueIfNotSet = module$contents$goog$object_setWithReturnValueIfNotSet;
goog.object.some = module$contents$goog$object_some;
goog.object.transpose = module$contents$goog$object_transpose;
goog.object.unsafeClone = module$contents$goog$object_unsafeClone;
const module$contents$goog$html$SafeStyleSheet_CONSTRUCTOR_TOKEN_PRIVATE = {};
class module$contents$goog$html$SafeStyleSheet_SafeStyleSheet {
  constructor(a, b) {
    if (goog.DEBUG && b !== module$contents$goog$html$SafeStyleSheet_CONSTRUCTOR_TOKEN_PRIVATE) {
      throw Error("SafeStyleSheet is not meant to be built directly");
    }
    this.privateDoNotAccessOrElseSafeStyleSheetWrappedValue_ = a;
    this.implementsGoogStringTypedString = !0;
  }
  toString() {
    return this.privateDoNotAccessOrElseSafeStyleSheetWrappedValue_.toString();
  }
  static createRule(a, b) {
    if ((0,goog.string.internal.contains)(a, "<")) {
      throw Error(`Selector does not allow '<', got: ${a}`);
    }
    const c = a.replace(/('|")((?!\1)[^\r\n\f\\]|\\[\s\S])*\1/g, "");
    if (!/^[-_a-zA-Z0-9#.:* ,>+~[\]()=\\^$|]+$/.test(c)) {
      throw Error("Selector allows only [-_a-zA-Z0-9#.:* ,>+~[\\]()=\\^$|] and strings, got: " + a);
    }
    if (!module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.hasBalancedBrackets_(c)) {
      throw Error("() and [] in selector must be balanced, got: " + a);
    }
    b instanceof module$contents$goog$html$SafeStyle_SafeStyle || (b = module$contents$goog$html$SafeStyle_SafeStyle.create(b));
    a = `${a}{` + module$contents$goog$html$SafeStyle_SafeStyle.unwrap(b).replace(/</g, "\\3C ") + "}";
    return module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.createSafeStyleSheetSecurityPrivateDoNotAccessOrElse(a);
  }
  static hasBalancedBrackets_(a) {
    const b = {"(":")", "[":"]"}, c = [];
    for (let d = 0; d < a.length; d++) {
      const e = a[d];
      if (b[e]) {
        c.push(b[e]);
      } else if (module$contents$goog$object_contains(b, e) && c.pop() != e) {
        return !1;
      }
    }
    return c.length == 0;
  }
  static concat(a) {
    let b = "";
    const c = d => {
      Array.isArray(d) ? d.forEach(c) : b += module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.unwrap(d);
    };
    Array.prototype.forEach.call(arguments, c);
    return module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.createSafeStyleSheetSecurityPrivateDoNotAccessOrElse(b);
  }
  static fromConstant(a) {
    a = goog.string.Const.unwrap(a);
    if (a.length === 0) {
      return module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.EMPTY;
    }
    (0,goog.asserts.assert)(!(0,goog.string.internal.contains)(a, "<"), `Forbidden '<' character in style sheet string: ${a}`);
    return module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.createSafeStyleSheetSecurityPrivateDoNotAccessOrElse(a);
  }
  getTypedStringValue() {
    return this.privateDoNotAccessOrElseSafeStyleSheetWrappedValue_;
  }
  static unwrap(a) {
    if (a instanceof module$contents$goog$html$SafeStyleSheet_SafeStyleSheet && a.constructor === module$contents$goog$html$SafeStyleSheet_SafeStyleSheet) {
      return a.privateDoNotAccessOrElseSafeStyleSheetWrappedValue_;
    }
    (0,goog.asserts.fail)("expected object of type SafeStyleSheet, got '" + a + "' of type " + goog.typeOf(a));
    return "type_error:SafeStyleSheet";
  }
  static createSafeStyleSheetSecurityPrivateDoNotAccessOrElse(a) {
    return new module$contents$goog$html$SafeStyleSheet_SafeStyleSheet(a, module$contents$goog$html$SafeStyleSheet_CONSTRUCTOR_TOKEN_PRIVATE);
  }
}
module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.EMPTY = module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.createSafeStyleSheetSecurityPrivateDoNotAccessOrElse("");
goog.html.SafeStyleSheet = module$contents$goog$html$SafeStyleSheet_SafeStyleSheet;
goog.dom.tags = {};
goog.dom.tags.VOID_TAGS_ = {area:!0, base:!0, br:!0, col:!0, command:!0, embed:!0, hr:!0, img:!0, input:!0, keygen:!0, link:!0, meta:!0, param:!0, source:!0, track:!0, wbr:!0};
goog.dom.tags.isVoidTag = function(a) {
  return goog.dom.tags.VOID_TAGS_[a] === !0;
};
const module$contents$goog$html$SafeHtml_CONSTRUCTOR_TOKEN_PRIVATE = {};
class module$contents$goog$html$SafeHtml_SafeHtml {
  constructor(a, b) {
    if (goog.DEBUG && b !== module$contents$goog$html$SafeHtml_CONSTRUCTOR_TOKEN_PRIVATE) {
      throw Error("SafeHtml is not meant to be built directly");
    }
    this.privateDoNotAccessOrElseSafeHtmlWrappedValue_ = a;
    this.implementsGoogStringTypedString = !0;
  }
  getTypedStringValue() {
    return this.privateDoNotAccessOrElseSafeHtmlWrappedValue_.toString();
  }
  toString() {
    return this.privateDoNotAccessOrElseSafeHtmlWrappedValue_.toString();
  }
  static unwrap(a) {
    return module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(a).toString();
  }
  static unwrapTrustedHTML(a) {
    if (a instanceof module$contents$goog$html$SafeHtml_SafeHtml && a.constructor === module$contents$goog$html$SafeHtml_SafeHtml) {
      return a.privateDoNotAccessOrElseSafeHtmlWrappedValue_;
    }
    goog.asserts.fail(`expected object of type SafeHtml, got '${a}' of type ` + goog.typeOf(a));
    return "type_error:SafeHtml";
  }
  static htmlEscape(a) {
    if (a instanceof module$contents$goog$html$SafeHtml_SafeHtml) {
      return a;
    }
    a = typeof a == "object" && a.implementsGoogStringTypedString ? a.getTypedStringValue() : String(a);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(goog.string.internal.htmlEscape(a));
  }
  static htmlEscapePreservingNewlines(a) {
    if (a instanceof module$contents$goog$html$SafeHtml_SafeHtml) {
      return a;
    }
    a = module$contents$goog$html$SafeHtml_SafeHtml.htmlEscape(a);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(goog.string.internal.newLineToBr(module$contents$goog$html$SafeHtml_SafeHtml.unwrap(a)));
  }
  static htmlEscapePreservingNewlinesAndSpaces(a) {
    if (a instanceof module$contents$goog$html$SafeHtml_SafeHtml) {
      return a;
    }
    a = module$contents$goog$html$SafeHtml_SafeHtml.htmlEscape(a);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(goog.string.internal.whitespaceEscape(module$contents$goog$html$SafeHtml_SafeHtml.unwrap(a)));
  }
  static comment(a) {
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse("\x3c!--" + goog.string.internal.htmlEscape(a) + "--\x3e");
  }
  static create(a, b, c) {
    module$contents$goog$html$SafeHtml_SafeHtml.verifyTagName(String(a));
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlTagSecurityPrivateDoNotAccessOrElse(String(a), b, c);
  }
  static verifyTagName(a) {
    if (!module$contents$goog$html$SafeHtml_VALID_NAMES_IN_TAG.test(a)) {
      throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? `Invalid tag name <${a}>.` : "");
    }
    if (a.toUpperCase() in module$contents$goog$html$SafeHtml_NOT_ALLOWED_TAG_NAMES) {
      throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? `Tag name <${a}> is not allowed for SafeHtml.` : "");
    }
  }
  static createIframe(a, b, c, d) {
    a && goog.html.TrustedResourceUrl.unwrap(a);
    const e = {};
    e.src = a || null;
    e.srcdoc = b && module$contents$goog$html$SafeHtml_SafeHtml.unwrap(b);
    a = module$contents$goog$html$SafeHtml_SafeHtml.combineAttributes(e, {sandbox:""}, c);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlTagSecurityPrivateDoNotAccessOrElse("iframe", a, d);
  }
  static createSandboxIframe(a, b, c, d) {
    if (!module$contents$goog$html$SafeHtml_SafeHtml.canUseSandboxIframe()) {
      throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? "The browser does not support sandboxed iframes." : "");
    }
    const e = {};
    e.src = a ? goog.html.SafeUrl.unwrap(goog.html.SafeUrl.sanitize(a)) : null;
    e.srcdoc = b || null;
    e.sandbox = "";
    a = module$contents$goog$html$SafeHtml_SafeHtml.combineAttributes(e, {}, c);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlTagSecurityPrivateDoNotAccessOrElse("iframe", a, d);
  }
  static canUseSandboxIframe() {
    return goog.global.HTMLIFrameElement && "sandbox" in goog.global.HTMLIFrameElement.prototype;
  }
  static createScriptSrc(a, b) {
    goog.html.TrustedResourceUrl.unwrap(a);
    a = module$contents$goog$html$SafeHtml_SafeHtml.combineAttributes({src:a}, {}, b);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlTagSecurityPrivateDoNotAccessOrElse("script", a);
  }
  static createScript(a, b) {
    for (var c in b) {
      if (Object.prototype.hasOwnProperty.call(b, c)) {
        var d = c.toLowerCase();
        if (d == "language" || d == "src" || d == "text") {
          throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? `Cannot set "${d}" attribute` : "");
        }
      }
    }
    c = "";
    a = module$contents$goog$array_concat(a);
    for (d = 0; d < a.length; d++) {
      c += module$contents$goog$html$SafeScript_SafeScript.unwrap(a[d]);
    }
    a = module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(c);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlTagSecurityPrivateDoNotAccessOrElse("script", b, a);
  }
  static createStyle(a, b) {
    b = module$contents$goog$html$SafeHtml_SafeHtml.combineAttributes({type:"text/css"}, {}, b);
    let c = "";
    a = module$contents$goog$array_concat(a);
    for (let d = 0; d < a.length; d++) {
      c += module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.unwrap(a[d]);
    }
    a = module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(c);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlTagSecurityPrivateDoNotAccessOrElse("style", b, a);
  }
  static createMetaRefresh(a, b) {
    a = goog.html.SafeUrl.unwrap(goog.html.SafeUrl.sanitize(a));
    (module$contents$goog$labs$userAgent$browser_matchIE() || module$contents$goog$labs$userAgent$browser_matchEdgeHtml()) && goog.string.internal.contains(a, ";") && (a = "'" + a.replace(/'/g, "%27") + "'");
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlTagSecurityPrivateDoNotAccessOrElse("meta", {"http-equiv":"refresh", content:(b || 0) + "; url=" + a});
  }
  static join(a, b) {
    a = module$contents$goog$html$SafeHtml_SafeHtml.htmlEscape(a);
    const c = [], d = e => {
      Array.isArray(e) ? e.forEach(d) : (e = module$contents$goog$html$SafeHtml_SafeHtml.htmlEscape(e), c.push(module$contents$goog$html$SafeHtml_SafeHtml.unwrap(e)));
    };
    b.forEach(d);
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(c.join(module$contents$goog$html$SafeHtml_SafeHtml.unwrap(a)));
  }
  static concat(a) {
    return module$contents$goog$html$SafeHtml_SafeHtml.join(module$contents$goog$html$SafeHtml_SafeHtml.EMPTY, Array.prototype.slice.call(arguments));
  }
  static createSafeHtmlSecurityPrivateDoNotAccessOrElse(a) {
    const b = goog.html.trustedtypes.getPolicyPrivateDoNotAccessOrElse();
    a = b ? b.createHTML(a) : a;
    return new module$contents$goog$html$SafeHtml_SafeHtml(a, module$contents$goog$html$SafeHtml_CONSTRUCTOR_TOKEN_PRIVATE);
  }
  static createSafeHtmlTagSecurityPrivateDoNotAccessOrElse(a, b, c) {
    b = `<${a}` + module$contents$goog$html$SafeHtml_SafeHtml.stringifyAttributes(a, b);
    c == null ? c = [] : Array.isArray(c) || (c = [c]);
    goog.dom.tags.isVoidTag(a.toLowerCase()) ? (goog.asserts.assert(!c.length, `Void tag <${a}> does not allow content.`), b += ">") : (c = module$contents$goog$html$SafeHtml_SafeHtml.concat(c), b += ">" + module$contents$goog$html$SafeHtml_SafeHtml.unwrap(c) + "</" + a + ">");
    return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(b);
  }
  static stringifyAttributes(a, b) {
    let c = "";
    if (b) {
      for (let d in b) {
        if (Object.prototype.hasOwnProperty.call(b, d)) {
          if (!module$contents$goog$html$SafeHtml_VALID_NAMES_IN_TAG.test(d)) {
            throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? `Invalid attribute name "${d}".` : "");
          }
          const e = b[d];
          e != null && (c += " " + module$contents$goog$html$SafeHtml_getAttrNameAndValue(a, d, e));
        }
      }
    }
    return c;
  }
  static combineAttributes(a, b, c) {
    const d = {};
    for (var e in a) {
      Object.prototype.hasOwnProperty.call(a, e) && (goog.asserts.assert(e.toLowerCase() == e, "Must be lower case"), d[e] = a[e]);
    }
    for (const f in b) {
      Object.prototype.hasOwnProperty.call(b, f) && (goog.asserts.assert(f.toLowerCase() == f, "Must be lower case"), d[f] = b[f]);
    }
    if (c) {
      for (const f in c) {
        if (Object.prototype.hasOwnProperty.call(c, f)) {
          e = f.toLowerCase();
          if (e in a) {
            throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? `Cannot override "${e}" attribute, got "` + f + '" with value "' + c[f] + '"' : "");
          }
          e in b && delete d[e];
          d[f] = c[f];
        }
      }
    }
    return d;
  }
}
module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES = goog.DEBUG;
module$contents$goog$html$SafeHtml_SafeHtml.SUPPORT_STYLE_ATTRIBUTE = !0;
module$contents$goog$html$SafeHtml_SafeHtml.from = module$contents$goog$html$SafeHtml_SafeHtml.htmlEscape;
const module$contents$goog$html$SafeHtml_VALID_NAMES_IN_TAG = /^[a-zA-Z0-9-]+$/, module$contents$goog$html$SafeHtml_URL_ATTRIBUTES = {action:!0, cite:!0, data:!0, formaction:!0, href:!0, manifest:!0, poster:!0, src:!0}, module$contents$goog$html$SafeHtml_NOT_ALLOWED_TAG_NAMES = {[goog.dom.TagName.APPLET]:!0, [goog.dom.TagName.BASE]:!0, [goog.dom.TagName.EMBED]:!0, [goog.dom.TagName.IFRAME]:!0, [goog.dom.TagName.LINK]:!0, [goog.dom.TagName.MATH]:!0, [goog.dom.TagName.META]:!0, [goog.dom.TagName.OBJECT]:!0, 
[goog.dom.TagName.SCRIPT]:!0, [goog.dom.TagName.STYLE]:!0, [goog.dom.TagName.SVG]:!0, [goog.dom.TagName.TEMPLATE]:!0};
function module$contents$goog$html$SafeHtml_getAttrNameAndValue(a, b, c) {
  if (c instanceof goog.string.Const) {
    c = goog.string.Const.unwrap(c);
  } else if (b.toLowerCase() == "style") {
    if (module$contents$goog$html$SafeHtml_SafeHtml.SUPPORT_STYLE_ATTRIBUTE) {
      c = module$contents$goog$html$SafeHtml_getStyleValue(c);
    } else {
      throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? 'Attribute "style" not supported.' : "");
    }
  } else {
    if (/^on/i.test(b)) {
      throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? `Attribute "${b}` + '" requires goog.string.Const value, "' + c + '" given.' : "");
    }
    if (b.toLowerCase() in module$contents$goog$html$SafeHtml_URL_ATTRIBUTES) {
      if (c instanceof goog.html.TrustedResourceUrl) {
        c = goog.html.TrustedResourceUrl.unwrap(c);
      } else if (c instanceof goog.html.SafeUrl) {
        c = goog.html.SafeUrl.unwrap(c);
      } else if (typeof c === "string") {
        c = goog.html.SafeUrl.sanitize(c).getTypedStringValue();
      } else {
        throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? `Attribute "${b}" on tag "${a}` + '" requires goog.html.SafeUrl, goog.string.Const, or string, value "' + c + '" given.' : "");
      }
    }
  }
  c.implementsGoogStringTypedString && (c = c.getTypedStringValue());
  goog.asserts.assert(typeof c === "string" || typeof c === "number", "String or number value expected, got " + typeof c + " with value: " + c);
  return `${b}="` + goog.string.internal.htmlEscape(String(c)) + '"';
}
function module$contents$goog$html$SafeHtml_getStyleValue(a) {
  if (!goog.isObject(a)) {
    throw Error(module$contents$goog$html$SafeHtml_SafeHtml.ENABLE_ERROR_MESSAGES ? 'The "style" attribute requires goog.html.SafeStyle or map of style properties, ' + typeof a + " given: " + a : "");
  }
  a instanceof module$contents$goog$html$SafeStyle_SafeStyle || (a = module$contents$goog$html$SafeStyle_SafeStyle.create(a));
  return module$contents$goog$html$SafeStyle_SafeStyle.unwrap(a);
}
module$contents$goog$html$SafeHtml_SafeHtml.DOCTYPE_HTML = function() {
  return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse("<!DOCTYPE html>");
}();
module$contents$goog$html$SafeHtml_SafeHtml.EMPTY = new module$contents$goog$html$SafeHtml_SafeHtml(goog.global.trustedTypes && goog.global.trustedTypes.emptyHTML || "", module$contents$goog$html$SafeHtml_CONSTRUCTOR_TOKEN_PRIVATE);
module$contents$goog$html$SafeHtml_SafeHtml.BR = function() {
  return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse("<br>");
}();
goog.html.SafeHtml = module$contents$goog$html$SafeHtml_SafeHtml;
goog.html.uncheckedconversions = {};
goog.html.uncheckedconversions.safeHtmlFromStringKnownToSatisfyTypeContract = function(a, b) {
  goog.asserts.assertString(goog.string.Const.unwrap(a), "must provide justification");
  goog.asserts.assert(!goog.string.internal.isEmptyOrWhitespace(goog.string.Const.unwrap(a)), "must provide non-empty justification");
  return module$contents$goog$html$SafeHtml_SafeHtml.createSafeHtmlSecurityPrivateDoNotAccessOrElse(b);
};
goog.html.uncheckedconversions.safeScriptFromStringKnownToSatisfyTypeContract = function(a, b) {
  goog.asserts.assertString(goog.string.Const.unwrap(a), "must provide justification");
  goog.asserts.assert(!goog.string.internal.isEmptyOrWhitespace(goog.string.Const.unwrap(a)), "must provide non-empty justification");
  return module$contents$goog$html$SafeScript_SafeScript.createSafeScriptSecurityPrivateDoNotAccessOrElse(b);
};
goog.html.uncheckedconversions.safeStyleFromStringKnownToSatisfyTypeContract = function(a, b) {
  goog.asserts.assertString(goog.string.Const.unwrap(a), "must provide justification");
  goog.asserts.assert(!goog.string.internal.isEmptyOrWhitespace(goog.string.Const.unwrap(a)), "must provide non-empty justification");
  return module$contents$goog$html$SafeStyle_SafeStyle.createSafeStyleSecurityPrivateDoNotAccessOrElse(b);
};
goog.html.uncheckedconversions.safeStyleSheetFromStringKnownToSatisfyTypeContract = function(a, b) {
  goog.asserts.assertString(goog.string.Const.unwrap(a), "must provide justification");
  goog.asserts.assert(!goog.string.internal.isEmptyOrWhitespace(goog.string.Const.unwrap(a)), "must provide non-empty justification");
  return module$contents$goog$html$SafeStyleSheet_SafeStyleSheet.createSafeStyleSheetSecurityPrivateDoNotAccessOrElse(b);
};
goog.html.uncheckedconversions.safeUrlFromStringKnownToSatisfyTypeContract = function(a, b) {
  goog.asserts.assertString(goog.string.Const.unwrap(a), "must provide justification");
  goog.asserts.assert(!goog.string.internal.isEmptyOrWhitespace(goog.string.Const.unwrap(a)), "must provide non-empty justification");
  return goog.html.SafeUrl.createSafeUrlSecurityPrivateDoNotAccessOrElse(b);
};
goog.html.uncheckedconversions.trustedResourceUrlFromStringKnownToSatisfyTypeContract = function(a, b) {
  goog.asserts.assertString(goog.string.Const.unwrap(a), "must provide justification");
  goog.asserts.assert(!goog.string.internal.isEmptyOrWhitespace(goog.string.Const.unwrap(a)), "must provide non-empty justification");
  return goog.html.TrustedResourceUrl.createTrustedResourceUrlSecurityPrivateDoNotAccessOrElse(b);
};
goog.dom.safe = {};
goog.dom.safe.InsertAdjacentHtmlPosition = {AFTERBEGIN:"afterbegin", AFTEREND:"afterend", BEFOREBEGIN:"beforebegin", BEFOREEND:"beforeend"};
goog.dom.safe.insertAdjacentHtml = function(a, b, c) {
  a.insertAdjacentHTML(b, module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(c));
};
goog.dom.safe.SET_INNER_HTML_DISALLOWED_TAGS_ = {MATH:!0, SCRIPT:!0, STYLE:!0, SVG:!0, TEMPLATE:!0};
goog.dom.safe.isInnerHtmlCleanupRecursive_ = goog.functions.cacheReturnValue(function() {
  if (goog.DEBUG && typeof document === "undefined") {
    return !1;
  }
  var a = document.createElement("div"), b = document.createElement("div");
  b.appendChild(document.createElement("div"));
  a.appendChild(b);
  if (goog.DEBUG && !a.firstChild) {
    return !1;
  }
  b = a.firstChild.firstChild;
  a.innerHTML = module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(module$contents$goog$html$SafeHtml_SafeHtml.EMPTY);
  return !b.parentElement;
});
goog.dom.safe.unsafeSetInnerHtmlDoNotUseOrElse = function(a, b) {
  if (goog.dom.safe.isInnerHtmlCleanupRecursive_()) {
    for (; a.lastChild;) {
      a.removeChild(a.lastChild);
    }
  }
  a.innerHTML = module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(b);
};
goog.dom.safe.setInnerHtml = function(a, b) {
  if (goog.asserts.ENABLE_ASSERTS && a.tagName) {
    var c = a.tagName.toUpperCase();
    if (goog.dom.safe.SET_INNER_HTML_DISALLOWED_TAGS_[c]) {
      throw Error("goog.dom.safe.setInnerHtml cannot be used to set content of " + a.tagName + ".");
    }
  }
  goog.dom.safe.unsafeSetInnerHtmlDoNotUseOrElse(a, b);
};
goog.dom.safe.setInnerHtmlFromConstant = function(a, b) {
  goog.dom.safe.setInnerHtml(a, goog.html.uncheckedconversions.safeHtmlFromStringKnownToSatisfyTypeContract(goog.string.Const.from("Constant HTML to be immediatelly used."), goog.string.Const.unwrap(b)));
};
goog.dom.safe.setOuterHtml = function(a, b) {
  a.outerHTML = module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(b);
};
goog.dom.safe.setFormElementAction = function(a, b) {
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  module$contents$goog$asserts$dom_assertIsHtmlFormElement(a).action = goog.html.SafeUrl.unwrap(b);
};
goog.dom.safe.setButtonFormAction = function(a, b) {
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  module$contents$goog$asserts$dom_assertIsHtmlButtonElement(a).formAction = goog.html.SafeUrl.unwrap(b);
};
goog.dom.safe.setInputFormAction = function(a, b) {
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  module$contents$goog$asserts$dom_assertIsHtmlInputElement(a).formAction = goog.html.SafeUrl.unwrap(b);
};
goog.dom.safe.setStyle = function(a, b) {
  a.style.cssText = module$contents$goog$html$SafeStyle_SafeStyle.unwrap(b);
};
goog.dom.safe.documentWrite = function(a, b) {
  a.write(module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(b));
};
goog.dom.safe.setAnchorHref = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlAnchorElement(a);
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  a.href = goog.html.SafeUrl.unwrap(b);
};
goog.dom.safe.setAudioSrc = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlAudioElement(a);
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  a.src = goog.html.SafeUrl.unwrap(b);
};
goog.dom.safe.setVideoSrc = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlVideoElement(a);
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  a.src = goog.html.SafeUrl.unwrap(b);
};
goog.dom.safe.setEmbedSrc = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlEmbedElement(a);
  a.src = goog.html.TrustedResourceUrl.unwrapTrustedScriptURL(b);
};
goog.dom.safe.setFrameSrc = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlFrameElement(a);
  a.src = goog.html.TrustedResourceUrl.unwrap(b);
};
goog.dom.safe.setIframeSrc = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlIFrameElement(a);
  a.src = goog.html.TrustedResourceUrl.unwrap(b);
};
goog.dom.safe.setIframeSrcdoc = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlIFrameElement(a);
  a.srcdoc = module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(b);
};
goog.dom.safe.setLinkHrefAndRel = function(a, b, c) {
  module$contents$goog$asserts$dom_assertIsHtmlLinkElement(a);
  a.rel = c;
  goog.string.internal.caseInsensitiveContains(c, "stylesheet") ? (goog.asserts.assert(b instanceof goog.html.TrustedResourceUrl, 'URL must be TrustedResourceUrl because "rel" contains "stylesheet"'), a.href = goog.html.TrustedResourceUrl.unwrap(b), (b = goog.dom.safe.getStyleNonce(a.ownerDocument && a.ownerDocument.defaultView)) && a.setAttribute("nonce", b)) : a.href = b instanceof goog.html.TrustedResourceUrl ? goog.html.TrustedResourceUrl.unwrap(b) : b instanceof goog.html.SafeUrl ? goog.html.SafeUrl.unwrap(b) : 
  goog.html.SafeUrl.unwrap(goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b));
};
goog.dom.safe.setObjectData = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlObjectElement(a);
  a.data = goog.html.TrustedResourceUrl.unwrapTrustedScriptURL(b);
};
goog.dom.safe.setScriptSrc = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlScriptElement(a);
  goog.dom.safe.setNonceForScriptElement_(a);
  a.src = goog.html.TrustedResourceUrl.unwrapTrustedScriptURL(b);
};
goog.dom.safe.setScriptContent = function(a, b) {
  module$contents$goog$asserts$dom_assertIsHtmlScriptElement(a);
  goog.dom.safe.setNonceForScriptElement_(a);
  a.textContent = module$contents$goog$html$SafeScript_SafeScript.unwrapTrustedScript(b);
};
goog.dom.safe.setNonceForScriptElement_ = function(a) {
  const b = goog.dom.safe.getScriptNonce(a.ownerDocument && a.ownerDocument.defaultView);
  b && a.setAttribute("nonce", b);
};
goog.dom.safe.setLocationHref = function(a, b) {
  goog.dom.asserts.assertIsLocation(a);
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  a.href = goog.html.SafeUrl.unwrap(b);
};
goog.dom.safe.assignLocation = function(a, b) {
  goog.dom.asserts.assertIsLocation(a);
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  a.assign(goog.html.SafeUrl.unwrap(b));
};
goog.dom.safe.replaceLocation = function(a, b) {
  b = b instanceof goog.html.SafeUrl ? b : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(b);
  a.replace(goog.html.SafeUrl.unwrap(b));
};
goog.dom.safe.openInWindow = function(a, b, c, d) {
  a = a instanceof goog.html.SafeUrl ? a : goog.html.SafeUrl.sanitizeJavascriptUrlAssertUnchanged(a);
  b = b || goog.global;
  c = c instanceof goog.string.Const ? goog.string.Const.unwrap(c) : c || "";
  return d !== void 0 ? b.open(goog.html.SafeUrl.unwrap(a), c, d) : b.open(goog.html.SafeUrl.unwrap(a), c);
};
goog.dom.safe.parseFromStringHtml = function(a, b) {
  return goog.dom.safe.parseFromString(a, b, "text/html");
};
goog.dom.safe.parseFromString = function(a, b, c) {
  return a.parseFromString(module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(b), c);
};
goog.dom.safe.createImageFromBlob = function(a) {
  if (!/^image\/.*/g.test(a.type)) {
    throw Error("goog.dom.safe.createImageFromBlob only accepts MIME type image/.*.");
  }
  var b = goog.global.URL.createObjectURL(a);
  a = new goog.global.Image();
  a.onload = function() {
    goog.global.URL.revokeObjectURL(b);
  };
  a.src = b;
  return a;
};
goog.dom.safe.createContextualFragment = function(a, b) {
  return a.createContextualFragment(module$contents$goog$html$SafeHtml_SafeHtml.unwrapTrustedHTML(b));
};
goog.dom.safe.getScriptNonce = function(a) {
  return goog.dom.safe.getNonce_("script[nonce]", a);
};
goog.dom.safe.getStyleNonce = function(a) {
  return goog.dom.safe.getNonce_('style[nonce],link[rel="stylesheet"][nonce]', a);
};
goog.dom.safe.NONCE_PATTERN_ = /^[\w+/_-]+[=]{0,2}$/;
goog.dom.safe.getNonce_ = function(a, b) {
  b = (b || goog.global).document;
  return b.querySelector ? (a = b.querySelector(a)) && (a = a.nonce || a.getAttribute("nonce")) && goog.dom.safe.NONCE_PATTERN_.test(a) ? a : "" : "";
};
goog.math = {};
goog.math.randomInt = function(a) {
  return Math.floor(Math.random() * a);
};
goog.math.uniformRandom = function(a, b) {
  return a + Math.random() * (b - a);
};
goog.math.clamp = function(a, b, c) {
  return Math.min(Math.max(a, b), c);
};
goog.math.modulo = function(a, b) {
  a %= b;
  return a * b < 0 ? a + b : a;
};
goog.math.lerp = function(a, b, c) {
  return a + c * (b - a);
};
goog.math.nearlyEquals = function(a, b, c) {
  return Math.abs(a - b) <= (c || 0.000001);
};
goog.math.standardAngle = function(a) {
  return goog.math.modulo(a, 360);
};
goog.math.standardAngleInRadians = function(a) {
  return goog.math.modulo(a, 2 * Math.PI);
};
goog.math.toRadians = function(a) {
  return a * Math.PI / 180;
};
goog.math.toDegrees = function(a) {
  return a * 180 / Math.PI;
};
goog.math.angleDx = function(a, b) {
  return b * Math.cos(goog.math.toRadians(a));
};
goog.math.angleDy = function(a, b) {
  return b * Math.sin(goog.math.toRadians(a));
};
goog.math.angle = function(a, b, c, d) {
  return goog.math.standardAngle(goog.math.toDegrees(Math.atan2(d - b, c - a)));
};
goog.math.angleDifference = function(a, b) {
  a = goog.math.standardAngle(b) - goog.math.standardAngle(a);
  a > 180 ? a -= 360 : a <= -180 && (a = 360 + a);
  return a;
};
goog.math.sign = function(a) {
  return a > 0 ? 1 : a < 0 ? -1 : a;
};
goog.math.longestCommonSubsequence = function(a, b, c, d) {
  c = c || function(n, m) {
    return n == m;
  };
  d = d || function(n, m) {
    return a[n];
  };
  for (var e = a.length, f = b.length, g = [], h = 0; h < e + 1; h++) {
    g[h] = [], g[h][0] = 0;
  }
  for (var k = 0; k < f + 1; k++) {
    g[0][k] = 0;
  }
  for (h = 1; h <= e; h++) {
    for (k = 1; k <= f; k++) {
      c(a[h - 1], b[k - 1]) ? g[h][k] = g[h - 1][k - 1] + 1 : g[h][k] = Math.max(g[h - 1][k], g[h][k - 1]);
    }
  }
  var l = [];
  h = e;
  for (k = f; h > 0 && k > 0;) {
    c(a[h - 1], b[k - 1]) ? (l.unshift(d(h - 1, k - 1)), h--, k--) : g[h - 1][k] > g[h][k - 1] ? h-- : k--;
  }
  return l;
};
goog.math.sum = function(a) {
  return Array.prototype.reduce.call(arguments, function(b, c) {
    return b + c;
  }, 0);
};
goog.math.average = function(a) {
  return goog.math.sum.apply(null, arguments) / arguments.length;
};
goog.math.sampleVariance = function(a) {
  var b = arguments.length;
  if (b < 2) {
    return 0;
  }
  var c = goog.math.average.apply(null, arguments);
  return goog.math.sum.apply(null, Array.prototype.map.call(arguments, function(d) {
    return Math.pow(d - c, 2);
  })) / (b - 1);
};
goog.math.standardDeviation = function(a) {
  return Math.sqrt(goog.math.sampleVariance.apply(null, arguments));
};
goog.math.isInt = function(a) {
  return isFinite(a) && a % 1 == 0;
};
goog.math.isFiniteNumber = function(a) {
  return isFinite(a);
};
goog.math.isNegativeZero = function(a) {
  return a == 0 && 1 / a < 0;
};
goog.math.log10Floor = function(a) {
  if (a > 0) {
    var b = Math.round(Math.log(a) * Math.LOG10E);
    return b - (parseFloat("1e" + b) > a ? 1 : 0);
  }
  return a == 0 ? -Infinity : NaN;
};
goog.math.safeFloor = function(a, b) {
  goog.asserts.assert(b === void 0 || b > 0);
  return Math.floor(a + (b || 2e-15));
};
goog.math.safeCeil = function(a, b) {
  goog.asserts.assert(b === void 0 || b > 0);
  return Math.ceil(a - (b || 2e-15));
};
goog.math.Coordinate = function(a, b) {
  this.x = a !== void 0 ? a : 0;
  this.y = b !== void 0 ? b : 0;
};
goog.math.Coordinate.prototype.clone = function() {
  return new goog.math.Coordinate(this.x, this.y);
};
goog.DEBUG && (goog.math.Coordinate.prototype.toString = function() {
  return "(" + this.x + ", " + this.y + ")";
});
goog.math.Coordinate.prototype.equals = function(a) {
  return a instanceof goog.math.Coordinate && goog.math.Coordinate.equals(this, a);
};
goog.math.Coordinate.equals = function(a, b) {
  return a == b ? !0 : a && b ? a.x == b.x && a.y == b.y : !1;
};
goog.math.Coordinate.distance = function(a, b) {
  var c = a.x - b.x;
  a = a.y - b.y;
  return Math.sqrt(c * c + a * a);
};
goog.math.Coordinate.magnitude = function(a) {
  return Math.sqrt(a.x * a.x + a.y * a.y);
};
goog.math.Coordinate.azimuth = function(a) {
  return goog.math.angle(0, 0, a.x, a.y);
};
goog.math.Coordinate.squaredDistance = function(a, b) {
  var c = a.x - b.x;
  a = a.y - b.y;
  return c * c + a * a;
};
goog.math.Coordinate.difference = function(a, b) {
  return new goog.math.Coordinate(a.x - b.x, a.y - b.y);
};
goog.math.Coordinate.sum = function(a, b) {
  return new goog.math.Coordinate(a.x + b.x, a.y + b.y);
};
goog.math.Coordinate.prototype.ceil = function() {
  this.x = Math.ceil(this.x);
  this.y = Math.ceil(this.y);
  return this;
};
goog.math.Coordinate.prototype.floor = function() {
  this.x = Math.floor(this.x);
  this.y = Math.floor(this.y);
  return this;
};
goog.math.Coordinate.prototype.round = function() {
  this.x = Math.round(this.x);
  this.y = Math.round(this.y);
  return this;
};
goog.math.Coordinate.prototype.translate = function(a, b) {
  a instanceof goog.math.Coordinate ? (this.x += a.x, this.y += a.y) : (this.x += Number(a), typeof b === "number" && (this.y += b));
  return this;
};
goog.math.Coordinate.prototype.scale = function(a, b) {
  this.x *= a;
  this.y *= typeof b === "number" ? b : a;
  return this;
};
goog.math.Coordinate.prototype.rotateRadians = function(a, b) {
  b = b || new goog.math.Coordinate(0, 0);
  var c = this.x, d = this.y, e = Math.cos(a);
  a = Math.sin(a);
  this.x = (c - b.x) * e - (d - b.y) * a + b.x;
  this.y = (c - b.x) * a + (d - b.y) * e + b.y;
};
goog.math.Coordinate.prototype.rotateDegrees = function(a, b) {
  this.rotateRadians(goog.math.toRadians(a), b);
};
goog.math.Size = function(a, b) {
  this.width = a;
  this.height = b;
};
goog.math.Size.equals = function(a, b) {
  return a == b ? !0 : a && b ? a.width == b.width && a.height == b.height : !1;
};
goog.math.Size.prototype.clone = function() {
  return new goog.math.Size(this.width, this.height);
};
goog.DEBUG && (goog.math.Size.prototype.toString = function() {
  return "(" + this.width + " x " + this.height + ")";
});
goog.math.Size.prototype.getLongest = function() {
  return Math.max(this.width, this.height);
};
goog.math.Size.prototype.getShortest = function() {
  return Math.min(this.width, this.height);
};
goog.math.Size.prototype.area = function() {
  return this.width * this.height;
};
goog.math.Size.prototype.perimeter = function() {
  return (this.width + this.height) * 2;
};
goog.math.Size.prototype.aspectRatio = function() {
  return this.width / this.height;
};
goog.math.Size.prototype.isEmpty = function() {
  return !this.area();
};
goog.math.Size.prototype.ceil = function() {
  this.width = Math.ceil(this.width);
  this.height = Math.ceil(this.height);
  return this;
};
goog.math.Size.prototype.fitsInside = function(a) {
  return this.width <= a.width && this.height <= a.height;
};
goog.math.Size.prototype.floor = function() {
  this.width = Math.floor(this.width);
  this.height = Math.floor(this.height);
  return this;
};
goog.math.Size.prototype.round = function() {
  this.width = Math.round(this.width);
  this.height = Math.round(this.height);
  return this;
};
goog.math.Size.prototype.scale = function(a, b) {
  this.width *= a;
  this.height *= typeof b === "number" ? b : a;
  return this;
};
goog.math.Size.prototype.scaleToCover = function(a) {
  a = this.aspectRatio() <= a.aspectRatio() ? a.width / this.width : a.height / this.height;
  return this.scale(a);
};
goog.math.Size.prototype.scaleToFit = function(a) {
  a = this.aspectRatio() > a.aspectRatio() ? a.width / this.width : a.height / this.height;
  return this.scale(a);
};
goog.string.DETECT_DOUBLE_ESCAPING = !1;
goog.string.FORCE_NON_DOM_HTML_UNESCAPING = !1;
goog.string.Unicode = {NBSP:"\u00a0", ZERO_WIDTH_SPACE:"\u200b"};
goog.string.startsWith = goog.string.internal.startsWith;
goog.string.endsWith = goog.string.internal.endsWith;
goog.string.caseInsensitiveStartsWith = goog.string.internal.caseInsensitiveStartsWith;
goog.string.caseInsensitiveEndsWith = goog.string.internal.caseInsensitiveEndsWith;
goog.string.caseInsensitiveEquals = goog.string.internal.caseInsensitiveEquals;
goog.string.subs = function(a, b) {
  const c = a.split("%s");
  let d = "";
  const e = Array.prototype.slice.call(arguments, 1);
  for (; e.length && c.length > 1;) {
    d += c.shift() + e.shift();
  }
  return d + c.join("%s");
};
goog.string.collapseWhitespace = function(a) {
  return a.replace(/[\s\xa0]+/g, " ").replace(/^\s+|\s+$/g, "");
};
goog.string.isEmptyOrWhitespace = goog.string.internal.isEmptyOrWhitespace;
goog.string.isEmptyString = function(a) {
  return a.length == 0;
};
goog.string.isEmpty = goog.string.isEmptyOrWhitespace;
goog.string.isEmptyOrWhitespaceSafe = function(a) {
  return goog.string.isEmptyOrWhitespace(goog.string.makeSafe(a));
};
goog.string.isEmptySafe = goog.string.isEmptyOrWhitespaceSafe;
goog.string.isBreakingWhitespace = function(a) {
  return !/[^\t\n\r ]/.test(a);
};
goog.string.isAlpha = function(a) {
  return !/[^a-zA-Z]/.test(a);
};
goog.string.isNumeric = function(a) {
  return !/[^0-9]/.test(a);
};
goog.string.isAlphaNumeric = function(a) {
  return !/[^a-zA-Z0-9]/.test(a);
};
goog.string.isSpace = function(a) {
  return a == " ";
};
goog.string.isUnicodeChar = function(a) {
  return a.length == 1 && a >= " " && a <= "~" || a >= "\u0080" && a <= "\ufffd";
};
goog.string.stripNewlines = function(a) {
  return a.replace(/(\r\n|\r|\n)+/g, " ");
};
goog.string.canonicalizeNewlines = function(a) {
  return a.replace(/(\r\n|\r|\n)/g, "\n");
};
goog.string.normalizeWhitespace = function(a) {
  return a.replace(/\xa0|\s/g, " ");
};
goog.string.normalizeSpaces = function(a) {
  return a.replace(/\xa0|[ \t]+/g, " ");
};
goog.string.collapseBreakingSpaces = function(a) {
  return a.replace(/[\t\r\n ]+/g, " ").replace(/^[\t\r\n ]+|[\t\r\n ]+$/g, "");
};
goog.string.trim = goog.string.internal.trim;
goog.string.trimLeft = function(a) {
  return a.replace(/^[\s\xa0]+/, "");
};
goog.string.trimRight = function(a) {
  return a.replace(/[\s\xa0]+$/, "");
};
goog.string.caseInsensitiveCompare = goog.string.internal.caseInsensitiveCompare;
goog.string.numberAwareCompare_ = function(a, b, c) {
  if (a == b) {
    return 0;
  }
  if (!a) {
    return -1;
  }
  if (!b) {
    return 1;
  }
  const d = a.toLowerCase().match(c), e = b.toLowerCase().match(c), f = Math.min(d.length, e.length);
  for (let g = 0; g < f; g++) {
    c = d[g];
    const h = e[g];
    if (c != h) {
      return a = parseInt(c, 10), !isNaN(a) && (b = parseInt(h, 10), !isNaN(b) && a - b) ? a - b : c < h ? -1 : 1;
    }
  }
  return d.length != e.length ? d.length - e.length : a < b ? -1 : 1;
};
goog.string.intAwareCompare = function(a, b) {
  return goog.string.numberAwareCompare_(a, b, /\d+|\D+/g);
};
goog.string.floatAwareCompare = function(a, b) {
  return goog.string.numberAwareCompare_(a, b, /\d+|\.\d+|\D+/g);
};
goog.string.numerateCompare = goog.string.floatAwareCompare;
goog.string.urlEncode = function(a) {
  return encodeURIComponent(String(a));
};
goog.string.urlDecode = function(a) {
  return decodeURIComponent(a.replace(/\+/g, " "));
};
goog.string.newLineToBr = goog.string.internal.newLineToBr;
goog.string.htmlEscape = function(a, b) {
  a = goog.string.internal.htmlEscape(a, b);
  goog.string.DETECT_DOUBLE_ESCAPING && (a = a.replace(goog.string.E_RE_, "&#101;"));
  return a;
};
goog.string.E_RE_ = /e/g;
goog.string.unescapeEntities = function(a) {
  return goog.string.contains(a, "&") ? !goog.string.FORCE_NON_DOM_HTML_UNESCAPING && "document" in goog.global ? goog.string.unescapeEntitiesUsingDom_(a) : goog.string.unescapePureXmlEntities_(a) : a;
};
goog.string.unescapeEntitiesWithDocument = function(a, b) {
  return goog.string.contains(a, "&") ? goog.string.unescapeEntitiesUsingDom_(a, b) : a;
};
goog.string.unescapeEntitiesUsingDom_ = function(a, b) {
  const c = {"&amp;":"&", "&lt;":"<", "&gt;":">", "&quot;":'"'};
  let d;
  d = b ? b.createElement("div") : goog.global.document.createElement("div");
  return a.replace(goog.string.HTML_ENTITY_PATTERN_, function(e, f) {
    let g = c[e];
    if (g) {
      return g;
    }
    f.charAt(0) == "#" && (f = Number("0" + f.slice(1)), isNaN(f) || (g = String.fromCharCode(f)));
    g || (goog.dom.safe.setInnerHtml(d, goog.html.uncheckedconversions.safeHtmlFromStringKnownToSatisfyTypeContract(goog.string.Const.from("Single HTML entity."), e + " ")), g = d.firstChild.nodeValue.slice(0, -1));
    return c[e] = g;
  });
};
goog.string.unescapePureXmlEntities_ = function(a) {
  return a.replace(/&([^;]+);/g, function(b, c) {
    switch(c) {
      case "amp":
        return "&";
      case "lt":
        return "<";
      case "gt":
        return ">";
      case "quot":
        return '"';
      default:
        return c.charAt(0) != "#" || (c = Number("0" + c.slice(1)), isNaN(c)) ? b : String.fromCharCode(c);
    }
  });
};
goog.string.HTML_ENTITY_PATTERN_ = /&([^;\s<&]+);?/g;
goog.string.whitespaceEscape = function(a, b) {
  return goog.string.newLineToBr(a.replace(/  /g, " &#160;"), b);
};
goog.string.preserveSpaces = function(a) {
  return a.replace(/(^|[\n ]) /g, "$1" + goog.string.Unicode.NBSP);
};
goog.string.stripQuotes = function(a, b) {
  const c = b.length;
  for (let d = 0; d < c; d++) {
    const e = c == 1 ? b : b.charAt(d);
    if (a.charAt(0) == e && a.charAt(a.length - 1) == e) {
      return a.substring(1, a.length - 1);
    }
  }
  return a;
};
goog.string.truncate = function(a, b, c) {
  c && (a = goog.string.unescapeEntities(a));
  a.length > b && (a = a.substring(0, b - 3) + "...");
  c && (a = goog.string.htmlEscape(a));
  return a;
};
goog.string.truncateMiddle = function(a, b, c, d) {
  c && (a = goog.string.unescapeEntities(a));
  if (d && a.length > b) {
    d > b && (d = b);
    var e = a.length - d;
    a = a.substring(0, b - d) + "..." + a.substring(e);
  } else {
    a.length > b && (d = Math.floor(b / 2), e = a.length - d, a = a.substring(0, d + b % 2) + "..." + a.substring(e));
  }
  c && (a = goog.string.htmlEscape(a));
  return a;
};
goog.string.specialEscapeChars_ = {"\x00":"\\0", "\b":"\\b", "\f":"\\f", "\n":"\\n", "\r":"\\r", "\t":"\\t", "\v":"\\x0B", '"':'\\"', "\\":"\\\\", "<":"\\u003C"};
goog.string.jsEscapeCache_ = {"'":"\\'"};
goog.string.quote = function(a) {
  a = String(a);
  const b = ['"'];
  for (let c = 0; c < a.length; c++) {
    const d = a.charAt(c), e = d.charCodeAt(0);
    b[c + 1] = goog.string.specialEscapeChars_[d] || (e > 31 && e < 127 ? d : goog.string.escapeChar(d));
  }
  b.push('"');
  return b.join("");
};
goog.string.escapeString = function(a) {
  const b = [];
  for (let c = 0; c < a.length; c++) {
    b[c] = goog.string.escapeChar(a.charAt(c));
  }
  return b.join("");
};
goog.string.escapeChar = function(a) {
  if (a in goog.string.jsEscapeCache_) {
    return goog.string.jsEscapeCache_[a];
  }
  if (a in goog.string.specialEscapeChars_) {
    return goog.string.jsEscapeCache_[a] = goog.string.specialEscapeChars_[a];
  }
  let b;
  const c = a.charCodeAt(0);
  if (c > 31 && c < 127) {
    b = a;
  } else {
    if (c < 256) {
      if (b = "\\x", c < 16 || c > 256) {
        b += "0";
      }
    } else {
      b = "\\u", c < 4096 && (b += "0");
    }
    b += c.toString(16).toUpperCase();
  }
  return goog.string.jsEscapeCache_[a] = b;
};
goog.string.contains = goog.string.internal.contains;
goog.string.caseInsensitiveContains = goog.string.internal.caseInsensitiveContains;
goog.string.countOf = function(a, b) {
  return a && b ? a.split(b).length - 1 : 0;
};
goog.string.removeAt = function(a, b, c) {
  let d = a;
  b >= 0 && b < a.length && c > 0 && (d = a.slice(0, b) + a.slice(b + c));
  return d;
};
goog.string.remove = function(a, b) {
  return a.replace(b, "");
};
goog.string.removeAll = function(a, b) {
  b = new RegExp(goog.string.regExpEscape(b), "g");
  return a.replace(b, "");
};
goog.string.replaceAll = function(a, b, c) {
  b = new RegExp(goog.string.regExpEscape(b), "g");
  return a.replace(b, c.replace(/\$/g, "$$$$"));
};
goog.string.regExpEscape = function(a) {
  return String(a).replace(/([-()\[\]{}+?*.$\^|,:#<!\\])/g, "\\$1").replace(/\x08/g, "\\x08");
};
goog.string.repeat = String.prototype.repeat ? function(a, b) {
  return a.repeat(b);
} : function(a, b) {
  return Array(b + 1).join(a);
};
goog.string.padNumber = function(a, b, c) {
  if (!Number.isFinite(a)) {
    return String(a);
  }
  a = c !== void 0 ? a.toFixed(c) : String(a);
  c = a.indexOf(".");
  c === -1 && (c = a.length);
  const d = a[0] === "-" ? "-" : "";
  d && (a = a.substring(1));
  return d + goog.string.repeat("0", Math.max(0, b - c)) + a;
};
goog.string.makeSafe = function(a) {
  return a == null ? "" : String(a);
};
goog.string.getRandomString = function() {
  return Math.floor(Math.random() * 2147483648).toString(36) + Math.abs(Math.floor(Math.random() * 2147483648) ^ goog.now()).toString(36);
};
goog.string.compareVersions = goog.string.internal.compareVersions;
goog.string.hashCode = function(a) {
  let b = 0;
  for (let c = 0; c < a.length; ++c) {
    b = 31 * b + a.charCodeAt(c) >>> 0;
  }
  return b;
};
goog.string.uniqueStringCounter_ = Math.random() * 2147483648 | 0;
goog.string.createUniqueString = function() {
  return "goog_" + goog.string.uniqueStringCounter_++;
};
goog.string.toNumber = function(a) {
  const b = Number(a);
  return b == 0 && goog.string.isEmptyOrWhitespace(a) ? NaN : b;
};
goog.string.isLowerCamelCase = function(a) {
  return /^[a-z]+([A-Z][a-z]*)*$/.test(a);
};
goog.string.isUpperCamelCase = function(a) {
  return /^([A-Z][a-z]*)+$/.test(a);
};
goog.string.toCamelCase = function(a) {
  return String(a).replace(/\-([a-z])/g, function(b, c) {
    return c.toUpperCase();
  });
};
goog.string.toSelectorCase = function(a) {
  return String(a).replace(/([A-Z])/g, "-$1").toLowerCase();
};
goog.string.toTitleCase = function(a, b) {
  b = typeof b === "string" ? goog.string.regExpEscape(b) : "\\s";
  return a.replace(new RegExp("(^" + (b ? "|[" + b + "]+" : "") + ")([a-z])", "g"), function(c, d, e) {
    return d + e.toUpperCase();
  });
};
goog.string.capitalize = function(a) {
  return String(a.charAt(0)).toUpperCase() + String(a.slice(1)).toLowerCase();
};
goog.string.parseInt = function(a) {
  isFinite(a) && (a = String(a));
  return typeof a === "string" ? /^\s*-?0x/i.test(a) ? parseInt(a, 16) : parseInt(a, 10) : NaN;
};
goog.string.splitLimit = function(a, b, c) {
  a = a.split(b);
  const d = [];
  for (; c > 0 && a.length;) {
    d.push(a.shift()), c--;
  }
  a.length && d.push(a.join(b));
  return d;
};
goog.string.lastComponent = function(a, b) {
  if (b) {
    typeof b == "string" && (b = [b]);
  } else {
    return a;
  }
  let c = -1;
  for (let d = 0; d < b.length; d++) {
    if (b[d] == "") {
      continue;
    }
    const e = a.lastIndexOf(b[d]);
    e > c && (c = e);
  }
  return c == -1 ? a : a.slice(c + 1);
};
goog.string.editDistance = function(a, b) {
  const c = [], d = [];
  if (a == b) {
    return 0;
  }
  if (!a.length || !b.length) {
    return Math.max(a.length, b.length);
  }
  for (var e = 0; e < b.length + 1; e++) {
    c[e] = e;
  }
  for (e = 0; e < a.length; e++) {
    d[0] = e + 1;
    for (var f = 0; f < b.length; f++) {
      d[f + 1] = Math.min(d[f] + 1, c[f + 1] + 1, c[f] + Number(a[e] != b[f]));
    }
    for (f = 0; f < c.length; f++) {
      c[f] = d[f];
    }
  }
  return d[b.length];
};
goog.dom.Appendable = {};
goog.dom.ASSUME_QUIRKS_MODE = !1;
goog.dom.ASSUME_STANDARDS_MODE = !1;
goog.dom.COMPAT_MODE_KNOWN_ = goog.dom.ASSUME_QUIRKS_MODE || goog.dom.ASSUME_STANDARDS_MODE;
goog.dom.getDomHelper = function(a) {
  return a ? new goog.dom.DomHelper(goog.dom.getOwnerDocument(a)) : goog.dom.defaultDomHelper_ || (goog.dom.defaultDomHelper_ = new goog.dom.DomHelper());
};
goog.dom.getDocument = function() {
  return document;
};
goog.dom.getElement = function(a) {
  return goog.dom.getElementHelper_(document, a);
};
goog.dom.getHTMLElement = function(a) {
  return (a = goog.dom.getElement(a)) ? module$contents$goog$asserts$dom_assertIsHtmlElement(a) : null;
};
goog.dom.getElementHelper_ = function(a, b) {
  return typeof b === "string" ? a.getElementById(b) : b;
};
goog.dom.getRequiredElement = function(a) {
  return goog.dom.getRequiredElementHelper_(document, a);
};
goog.dom.getRequiredHTMLElement = function(a) {
  return module$contents$goog$asserts$dom_assertIsHtmlElement(goog.dom.getRequiredElementHelper_(document, a));
};
goog.dom.getRequiredElementHelper_ = function(a, b) {
  goog.asserts.assertString(b);
  a = goog.dom.getElementHelper_(a, b);
  return goog.asserts.assert(a, "No element found with id: " + b);
};
goog.dom.$ = goog.dom.getElement;
goog.dom.getElementsByTagName = function(a, b) {
  return (b || document).getElementsByTagName(String(a));
};
goog.dom.getElementsByTagNameAndClass = function(a, b, c) {
  return goog.dom.getElementsByTagNameAndClass_(document, a, b, c);
};
goog.dom.getElementByTagNameAndClass = function(a, b, c) {
  return goog.dom.getElementByTagNameAndClass_(document, a, b, c);
};
goog.dom.getElementsByClass = function(a, b) {
  var c = b || document;
  return goog.dom.canUseQuerySelector_(c) ? c.querySelectorAll("." + a) : goog.dom.getElementsByTagNameAndClass_(document, "*", a, b);
};
goog.dom.getElementByClass = function(a, b) {
  var c = b || document;
  return (c.getElementsByClassName ? c.getElementsByClassName(a)[0] : goog.dom.getElementByTagNameAndClass_(document, "*", a, b)) || null;
};
goog.dom.getHTMLElementByClass = function(a, b) {
  return (a = goog.dom.getElementByClass(a, b)) ? module$contents$goog$asserts$dom_assertIsHtmlElement(a) : null;
};
goog.dom.getRequiredElementByClass = function(a, b) {
  b = goog.dom.getElementByClass(a, b);
  return goog.asserts.assert(b, "No element found with className: " + a);
};
goog.dom.getRequiredHTMLElementByClass = function(a, b) {
  b = goog.dom.getElementByClass(a, b);
  goog.asserts.assert(b, "No HTMLElement found with className: " + a);
  return module$contents$goog$asserts$dom_assertIsHtmlElement(b);
};
goog.dom.canUseQuerySelector_ = function(a) {
  return !(!a.querySelectorAll || !a.querySelector);
};
goog.dom.getElementsByTagNameAndClass_ = function(a, b, c, d) {
  a = d || a;
  b = b && b != "*" ? String(b).toUpperCase() : "";
  if (goog.dom.canUseQuerySelector_(a) && (b || c)) {
    return a.querySelectorAll(b + (c ? "." + c : ""));
  }
  if (c && a.getElementsByClassName) {
    a = a.getElementsByClassName(c);
    if (b) {
      d = {};
      for (var e = 0, f = 0, g; g = a[f]; f++) {
        b == g.nodeName && (d[e++] = g);
      }
      d.length = e;
      return d;
    }
    return a;
  }
  a = a.getElementsByTagName(b || "*");
  if (c) {
    d = {};
    for (f = e = 0; g = a[f]; f++) {
      b = g.className, typeof b.split == "function" && module$contents$goog$array_contains(b.split(/\s+/), c) && (d[e++] = g);
    }
    d.length = e;
    return d;
  }
  return a;
};
goog.dom.getElementByTagNameAndClass_ = function(a, b, c, d) {
  var e = d || a, f = b && b != "*" ? String(b).toUpperCase() : "";
  return goog.dom.canUseQuerySelector_(e) && (f || c) ? e.querySelector(f + (c ? "." + c : "")) : goog.dom.getElementsByTagNameAndClass_(a, b, c, d)[0] || null;
};
goog.dom.$$ = goog.dom.getElementsByTagNameAndClass;
goog.dom.setProperties = function(a, b) {
  module$contents$goog$object_forEach(b, function(c, d) {
    c && typeof c == "object" && c.implementsGoogStringTypedString && (c = c.getTypedStringValue());
    d == "style" ? a.style.cssText = c : d == "class" ? a.className = c : d == "for" ? a.htmlFor = c : goog.dom.DIRECT_ATTRIBUTE_MAP_.hasOwnProperty(d) ? a.setAttribute(goog.dom.DIRECT_ATTRIBUTE_MAP_[d], c) : goog.string.startsWith(d, "aria-") || goog.string.startsWith(d, "data-") ? a.setAttribute(d, c) : a[d] = c;
  });
};
goog.dom.DIRECT_ATTRIBUTE_MAP_ = {cellpadding:"cellPadding", cellspacing:"cellSpacing", colspan:"colSpan", frameborder:"frameBorder", height:"height", maxlength:"maxLength", nonce:"nonce", role:"role", rowspan:"rowSpan", type:"type", usemap:"useMap", valign:"vAlign", width:"width"};
goog.dom.getViewportSize = function(a) {
  return goog.dom.getViewportSize_(a || window);
};
goog.dom.getViewportSize_ = function(a) {
  a = a.document;
  a = goog.dom.isCss1CompatMode_(a) ? a.documentElement : a.body;
  return new goog.math.Size(a.clientWidth, a.clientHeight);
};
goog.dom.getDocumentHeight = function() {
  return goog.dom.getDocumentHeight_(window);
};
goog.dom.getDocumentHeightForWindow = function(a) {
  return goog.dom.getDocumentHeight_(a);
};
goog.dom.getDocumentHeight_ = function(a) {
  var b = a.document, c = 0;
  if (b) {
    c = b.body;
    var d = b.documentElement;
    if (!d || !c) {
      return 0;
    }
    a = goog.dom.getViewportSize_(a).height;
    if (goog.dom.isCss1CompatMode_(b) && d.scrollHeight) {
      c = d.scrollHeight != a ? d.scrollHeight : d.offsetHeight;
    } else {
      b = d.scrollHeight;
      var e = d.offsetHeight;
      d.clientHeight != e && (b = c.scrollHeight, e = c.offsetHeight);
      c = b > a ? b > e ? b : e : b < e ? b : e;
    }
  }
  return c;
};
goog.dom.getPageScroll = function(a) {
  return goog.dom.getDomHelper((a || goog.global || window).document).getDocumentScroll();
};
goog.dom.getDocumentScroll = function() {
  return goog.dom.getDocumentScroll_(document);
};
goog.dom.getDocumentScroll_ = function(a) {
  var b = goog.dom.getDocumentScrollElement_(a);
  a = goog.dom.getWindow_(a);
  return goog.userAgent.IE && a.pageYOffset != b.scrollTop ? new goog.math.Coordinate(b.scrollLeft, b.scrollTop) : new goog.math.Coordinate(a.pageXOffset || b.scrollLeft, a.pageYOffset || b.scrollTop);
};
goog.dom.getDocumentScrollElement = function() {
  return goog.dom.getDocumentScrollElement_(document);
};
goog.dom.getDocumentScrollElement_ = function(a) {
  return a.scrollingElement ? a.scrollingElement : !goog.userAgent.WEBKIT && goog.dom.isCss1CompatMode_(a) ? a.documentElement : a.body || a.documentElement;
};
goog.dom.getWindow = function(a) {
  return a ? goog.dom.getWindow_(a) : window;
};
goog.dom.getWindow_ = function(a) {
  return a.parentWindow || a.defaultView;
};
goog.dom.createDom = function(a, b, c) {
  return goog.dom.createDom_(document, arguments);
};
goog.dom.createDom_ = function(a, b) {
  var c = b[1], d = goog.dom.createElement_(a, String(b[0]));
  c && (typeof c === "string" ? d.className = c : Array.isArray(c) ? d.className = c.join(" ") : goog.dom.setProperties(d, c));
  b.length > 2 && goog.dom.append_(a, d, b, 2);
  return d;
};
goog.dom.append_ = function(a, b, c, d) {
  function e(g) {
    g && b.appendChild(typeof g === "string" ? a.createTextNode(g) : g);
  }
  for (; d < c.length; d++) {
    var f = c[d];
    goog.isArrayLike(f) && !goog.dom.isNodeLike(f) ? module$contents$goog$array_forEach(goog.dom.isNodeList(f) ? module$contents$goog$array_toArray(f) : f, e) : e(f);
  }
};
goog.dom.$dom = goog.dom.createDom;
goog.dom.createElement = function(a) {
  return goog.dom.createElement_(document, a);
};
goog.dom.createElement_ = function(a, b) {
  b = String(b);
  a.contentType === "application/xhtml+xml" && (b = b.toLowerCase());
  return a.createElement(b);
};
goog.dom.createTextNode = function(a) {
  return document.createTextNode(String(a));
};
goog.dom.createTable = function(a, b, c) {
  return goog.dom.createTable_(document, a, b, !!c);
};
goog.dom.createTable_ = function(a, b, c, d) {
  for (var e = goog.dom.createElement_(a, goog.dom.TagName.TABLE), f = e.appendChild(goog.dom.createElement_(a, goog.dom.TagName.TBODY)), g = 0; g < b; g++) {
    for (var h = goog.dom.createElement_(a, goog.dom.TagName.TR), k = 0; k < c; k++) {
      var l = goog.dom.createElement_(a, goog.dom.TagName.TD);
      d && goog.dom.setTextContent(l, goog.string.Unicode.NBSP);
      h.appendChild(l);
    }
    f.appendChild(h);
  }
  return e;
};
goog.dom.constHtmlToNode = function(a) {
  var b = Array.prototype.map.call(arguments, goog.string.Const.unwrap);
  b = goog.html.uncheckedconversions.safeHtmlFromStringKnownToSatisfyTypeContract(goog.string.Const.from("Constant HTML string, that gets turned into a Node later, so it will be automatically balanced."), b.join(""));
  return goog.dom.safeHtmlToNode(b);
};
goog.dom.safeHtmlToNode = function(a) {
  return goog.dom.safeHtmlToNode_(document, a);
};
goog.dom.safeHtmlToNode_ = function(a, b) {
  var c = goog.dom.createElement_(a, goog.dom.TagName.DIV);
  goog.userAgent.IE ? (goog.dom.safe.setInnerHtml(c, module$contents$goog$html$SafeHtml_SafeHtml.concat(module$contents$goog$html$SafeHtml_SafeHtml.BR, b)), c.removeChild(goog.asserts.assert(c.firstChild))) : goog.dom.safe.setInnerHtml(c, b);
  return goog.dom.childrenToNode_(a, c);
};
goog.dom.childrenToNode_ = function(a, b) {
  if (b.childNodes.length == 1) {
    return b.removeChild(goog.asserts.assert(b.firstChild));
  }
  for (a = a.createDocumentFragment(); b.firstChild;) {
    a.appendChild(b.firstChild);
  }
  return a;
};
goog.dom.isCss1CompatMode = function() {
  return goog.dom.isCss1CompatMode_(document);
};
goog.dom.isCss1CompatMode_ = function(a) {
  return goog.dom.COMPAT_MODE_KNOWN_ ? goog.dom.ASSUME_STANDARDS_MODE : a.compatMode == "CSS1Compat";
};
goog.dom.canHaveChildren = function(a) {
  if (a.nodeType != goog.dom.NodeType.ELEMENT) {
    return !1;
  }
  switch(a.tagName) {
    case String(goog.dom.TagName.APPLET):
    case String(goog.dom.TagName.AREA):
    case String(goog.dom.TagName.BASE):
    case String(goog.dom.TagName.BR):
    case String(goog.dom.TagName.COL):
    case String(goog.dom.TagName.COMMAND):
    case String(goog.dom.TagName.EMBED):
    case String(goog.dom.TagName.FRAME):
    case String(goog.dom.TagName.HR):
    case String(goog.dom.TagName.IMG):
    case String(goog.dom.TagName.INPUT):
    case String(goog.dom.TagName.IFRAME):
    case String(goog.dom.TagName.ISINDEX):
    case String(goog.dom.TagName.KEYGEN):
    case String(goog.dom.TagName.LINK):
    case String(goog.dom.TagName.NOFRAMES):
    case String(goog.dom.TagName.NOSCRIPT):
    case String(goog.dom.TagName.META):
    case String(goog.dom.TagName.OBJECT):
    case String(goog.dom.TagName.PARAM):
    case String(goog.dom.TagName.SCRIPT):
    case String(goog.dom.TagName.SOURCE):
    case String(goog.dom.TagName.STYLE):
    case String(goog.dom.TagName.TRACK):
    case String(goog.dom.TagName.WBR):
      return !1;
  }
  return !0;
};
goog.dom.appendChild = function(a, b) {
  goog.asserts.assert(a != null && b != null, "goog.dom.appendChild expects non-null arguments");
  a.appendChild(b);
};
goog.dom.append = function(a, b) {
  goog.dom.append_(goog.dom.getOwnerDocument(a), a, arguments, 1);
};
goog.dom.removeChildren = function(a) {
  for (var b; b = a.firstChild;) {
    a.removeChild(b);
  }
};
goog.dom.insertSiblingBefore = function(a, b) {
  goog.asserts.assert(a != null && b != null, "goog.dom.insertSiblingBefore expects non-null arguments");
  b.parentNode && b.parentNode.insertBefore(a, b);
};
goog.dom.insertSiblingAfter = function(a, b) {
  goog.asserts.assert(a != null && b != null, "goog.dom.insertSiblingAfter expects non-null arguments");
  b.parentNode && b.parentNode.insertBefore(a, b.nextSibling);
};
goog.dom.insertChildAt = function(a, b, c) {
  goog.asserts.assert(a != null, "goog.dom.insertChildAt expects a non-null parent");
  a.insertBefore(b, a.childNodes[c] || null);
};
goog.dom.removeNode = function(a) {
  return a && a.parentNode ? a.parentNode.removeChild(a) : null;
};
goog.dom.replaceNode = function(a, b) {
  goog.asserts.assert(a != null && b != null, "goog.dom.replaceNode expects non-null arguments");
  var c = b.parentNode;
  c && c.replaceChild(a, b);
};
goog.dom.copyContents = function(a, b) {
  goog.asserts.assert(a != null && b != null, "goog.dom.copyContents expects non-null arguments");
  b = b.cloneNode(!0).childNodes;
  for (goog.dom.removeChildren(a); b.length;) {
    a.appendChild(b[0]);
  }
};
goog.dom.flattenElement = function(a) {
  var b, c = a.parentNode;
  if (c && c.nodeType != goog.dom.NodeType.DOCUMENT_FRAGMENT) {
    if (a.removeNode) {
      return a.removeNode(!1);
    }
    for (; b = a.firstChild;) {
      c.insertBefore(b, a);
    }
    return goog.dom.removeNode(a);
  }
};
goog.dom.getChildren = function(a) {
  return a.children != void 0 ? a.children : Array.prototype.filter.call(a.childNodes, function(b) {
    return b.nodeType == goog.dom.NodeType.ELEMENT;
  });
};
goog.dom.getFirstElementChild = function(a) {
  return a.firstElementChild !== void 0 ? a.firstElementChild : goog.dom.getNextElementNode_(a.firstChild, !0);
};
goog.dom.getLastElementChild = function(a) {
  return a.lastElementChild !== void 0 ? a.lastElementChild : goog.dom.getNextElementNode_(a.lastChild, !1);
};
goog.dom.getNextElementSibling = function(a) {
  return a.nextElementSibling !== void 0 ? a.nextElementSibling : goog.dom.getNextElementNode_(a.nextSibling, !0);
};
goog.dom.getPreviousElementSibling = function(a) {
  return a.previousElementSibling !== void 0 ? a.previousElementSibling : goog.dom.getNextElementNode_(a.previousSibling, !1);
};
goog.dom.getNextElementNode_ = function(a, b) {
  for (; a && a.nodeType != goog.dom.NodeType.ELEMENT;) {
    a = b ? a.nextSibling : a.previousSibling;
  }
  return a;
};
goog.dom.getNextNode = function(a) {
  if (!a) {
    return null;
  }
  if (a.firstChild) {
    return a.firstChild;
  }
  for (; a && !a.nextSibling;) {
    a = a.parentNode;
  }
  return a ? a.nextSibling : null;
};
goog.dom.getPreviousNode = function(a) {
  if (!a) {
    return null;
  }
  if (!a.previousSibling) {
    return a.parentNode;
  }
  for (a = a.previousSibling; a && a.lastChild;) {
    a = a.lastChild;
  }
  return a;
};
goog.dom.isNodeLike = function(a) {
  return goog.isObject(a) && a.nodeType > 0;
};
goog.dom.isElement = function(a) {
  return goog.isObject(a) && a.nodeType == goog.dom.NodeType.ELEMENT;
};
goog.dom.isWindow = function(a) {
  return goog.isObject(a) && a.window == a;
};
goog.dom.getParentElement = function(a) {
  var b;
  if (goog.dom.BrowserFeature.CAN_USE_PARENT_ELEMENT_PROPERTY && (b = a.parentElement)) {
    return b;
  }
  b = a.parentNode;
  return goog.dom.isElement(b) ? b : null;
};
goog.dom.contains = function(a, b) {
  if (!a || !b) {
    return !1;
  }
  if (a.contains && b.nodeType == goog.dom.NodeType.ELEMENT) {
    return a == b || a.contains(b);
  }
  if (typeof a.compareDocumentPosition != "undefined") {
    return a == b || !!(a.compareDocumentPosition(b) & 16);
  }
  for (; b && a != b;) {
    b = b.parentNode;
  }
  return b == a;
};
goog.dom.compareNodeOrder = function(a, b) {
  if (a == b) {
    return 0;
  }
  if (a.compareDocumentPosition) {
    return a.compareDocumentPosition(b) & 2 ? 1 : -1;
  }
  if (goog.userAgent.IE && !goog.userAgent.isDocumentModeOrHigher(9)) {
    if (a.nodeType == goog.dom.NodeType.DOCUMENT) {
      return -1;
    }
    if (b.nodeType == goog.dom.NodeType.DOCUMENT) {
      return 1;
    }
  }
  if ("sourceIndex" in a || a.parentNode && "sourceIndex" in a.parentNode) {
    var c = a.nodeType == goog.dom.NodeType.ELEMENT, d = b.nodeType == goog.dom.NodeType.ELEMENT;
    if (c && d) {
      return a.sourceIndex - b.sourceIndex;
    }
    var e = a.parentNode, f = b.parentNode;
    return e == f ? goog.dom.compareSiblingOrder_(a, b) : !c && goog.dom.contains(e, b) ? -1 * goog.dom.compareParentsDescendantNodeIe_(a, b) : !d && goog.dom.contains(f, a) ? goog.dom.compareParentsDescendantNodeIe_(b, a) : (c ? a.sourceIndex : e.sourceIndex) - (d ? b.sourceIndex : f.sourceIndex);
  }
  d = goog.dom.getOwnerDocument(a);
  c = d.createRange();
  c.selectNode(a);
  c.collapse(!0);
  a = d.createRange();
  a.selectNode(b);
  a.collapse(!0);
  return c.compareBoundaryPoints(goog.global.Range.START_TO_END, a);
};
goog.dom.compareParentsDescendantNodeIe_ = function(a, b) {
  var c = a.parentNode;
  if (c == b) {
    return -1;
  }
  for (; b.parentNode != c;) {
    b = b.parentNode;
  }
  return goog.dom.compareSiblingOrder_(b, a);
};
goog.dom.compareSiblingOrder_ = function(a, b) {
  for (; b = b.previousSibling;) {
    if (b == a) {
      return -1;
    }
  }
  return 1;
};
goog.dom.findCommonAncestor = function(a) {
  var b, c = arguments.length;
  if (!c) {
    return null;
  }
  if (c == 1) {
    return arguments[0];
  }
  var d = [], e = Infinity;
  for (b = 0; b < c; b++) {
    for (var f = [], g = arguments[b]; g;) {
      f.unshift(g), g = g.parentNode;
    }
    d.push(f);
    e = Math.min(e, f.length);
  }
  f = null;
  for (b = 0; b < e; b++) {
    g = d[0][b];
    for (var h = 1; h < c; h++) {
      if (g != d[h][b]) {
        return f;
      }
    }
    f = g;
  }
  return f;
};
goog.dom.isInDocument = function(a) {
  return (a.ownerDocument.compareDocumentPosition(a) & 16) == 16;
};
goog.dom.getOwnerDocument = function(a) {
  goog.asserts.assert(a, "Node cannot be null or undefined.");
  return a.nodeType == goog.dom.NodeType.DOCUMENT ? a : a.ownerDocument || a.document;
};
goog.dom.getFrameContentDocument = function(a) {
  return a.contentDocument || a.contentWindow.document;
};
goog.dom.getFrameContentWindow = function(a) {
  try {
    return a.contentWindow || (a.contentDocument ? goog.dom.getWindow(a.contentDocument) : null);
  } catch (b) {
  }
  return null;
};
goog.dom.setTextContent = function(a, b) {
  goog.asserts.assert(a != null, "goog.dom.setTextContent expects a non-null value for node");
  if ("textContent" in a) {
    a.textContent = b;
  } else if (a.nodeType == goog.dom.NodeType.TEXT) {
    a.data = String(b);
  } else if (a.firstChild && a.firstChild.nodeType == goog.dom.NodeType.TEXT) {
    for (; a.lastChild != a.firstChild;) {
      a.removeChild(goog.asserts.assert(a.lastChild));
    }
    a.firstChild.data = String(b);
  } else {
    goog.dom.removeChildren(a);
    var c = goog.dom.getOwnerDocument(a);
    a.appendChild(c.createTextNode(String(b)));
  }
};
goog.dom.getOuterHtml = function(a) {
  goog.asserts.assert(a !== null, "goog.dom.getOuterHtml expects a non-null value for element");
  if ("outerHTML" in a) {
    return a.outerHTML;
  }
  var b = goog.dom.getOwnerDocument(a);
  b = goog.dom.createElement_(b, goog.dom.TagName.DIV);
  b.appendChild(a.cloneNode(!0));
  return b.innerHTML;
};
goog.dom.findNode = function(a, b) {
  var c = [];
  return goog.dom.findNodes_(a, b, c, !0) ? c[0] : void 0;
};
goog.dom.findNodes = function(a, b) {
  var c = [];
  goog.dom.findNodes_(a, b, c, !1);
  return c;
};
goog.dom.findNodes_ = function(a, b, c, d) {
  if (a != null) {
    for (a = a.firstChild; a;) {
      if (b(a) && (c.push(a), d) || goog.dom.findNodes_(a, b, c, d)) {
        return !0;
      }
      a = a.nextSibling;
    }
  }
  return !1;
};
goog.dom.findElement = function(a, b) {
  for (a = goog.dom.getChildrenReverse_(a); a.length > 0;) {
    var c = a.pop();
    if (b(c)) {
      return c;
    }
    for (c = c.lastElementChild; c; c = c.previousElementSibling) {
      a.push(c);
    }
  }
  return null;
};
goog.dom.findElements = function(a, b) {
  var c = [];
  for (a = goog.dom.getChildrenReverse_(a); a.length > 0;) {
    var d = a.pop();
    b(d) && c.push(d);
    for (d = d.lastElementChild; d; d = d.previousElementSibling) {
      a.push(d);
    }
  }
  return c;
};
goog.dom.getChildrenReverse_ = function(a) {
  if (a.nodeType == goog.dom.NodeType.DOCUMENT) {
    return [a.documentElement];
  }
  var b = [];
  for (a = a.lastElementChild; a; a = a.previousElementSibling) {
    b.push(a);
  }
  return b;
};
goog.dom.TAGS_TO_IGNORE_ = {SCRIPT:1, STYLE:1, HEAD:1, IFRAME:1, OBJECT:1};
goog.dom.PREDEFINED_TAG_VALUES_ = {IMG:" ", BR:"\n"};
goog.dom.isFocusableTabIndex = function(a) {
  return goog.dom.hasSpecifiedTabIndex_(a) && goog.dom.isTabIndexFocusable_(a);
};
goog.dom.setFocusableTabIndex = function(a, b) {
  b ? a.tabIndex = 0 : (a.tabIndex = -1, a.removeAttribute("tabIndex"));
};
goog.dom.isFocusable = function(a) {
  var b;
  return (b = goog.dom.nativelySupportsFocus_(a) ? !a.disabled && (!goog.dom.hasSpecifiedTabIndex_(a) || goog.dom.isTabIndexFocusable_(a)) : goog.dom.isFocusableTabIndex(a)) && goog.userAgent.IE ? goog.dom.hasNonZeroBoundingRect_(a) : b;
};
goog.dom.hasSpecifiedTabIndex_ = function(a) {
  return a.hasAttribute("tabindex");
};
goog.dom.isTabIndexFocusable_ = function(a) {
  a = a.tabIndex;
  return typeof a === "number" && a >= 0 && a < 32768;
};
goog.dom.nativelySupportsFocus_ = function(a) {
  return a.tagName == goog.dom.TagName.A && a.hasAttribute("href") || a.tagName == goog.dom.TagName.INPUT || a.tagName == goog.dom.TagName.TEXTAREA || a.tagName == goog.dom.TagName.SELECT || a.tagName == goog.dom.TagName.BUTTON;
};
goog.dom.hasNonZeroBoundingRect_ = function(a) {
  a = typeof a.getBoundingClientRect !== "function" || goog.userAgent.IE && a.parentElement == null ? {height:a.offsetHeight, width:a.offsetWidth} : a.getBoundingClientRect();
  return a != null && a.height > 0 && a.width > 0;
};
goog.dom.getTextContent = function(a) {
  var b = [];
  goog.dom.getTextContent_(a, b, !0);
  a = b.join("");
  a = a.replace(/ \xAD /g, " ").replace(/\xAD/g, "");
  a = a.replace(/\u200B/g, "");
  a = a.replace(/ +/g, " ");
  a != " " && (a = a.replace(/^\s*/, ""));
  return a;
};
goog.dom.getRawTextContent = function(a) {
  var b = [];
  goog.dom.getTextContent_(a, b, !1);
  return b.join("");
};
goog.dom.getTextContent_ = function(a, b, c) {
  if (!(a.nodeName in goog.dom.TAGS_TO_IGNORE_)) {
    if (a.nodeType == goog.dom.NodeType.TEXT) {
      c ? b.push(String(a.nodeValue).replace(/(\r\n|\r|\n)/g, "")) : b.push(a.nodeValue);
    } else if (a.nodeName in goog.dom.PREDEFINED_TAG_VALUES_) {
      b.push(goog.dom.PREDEFINED_TAG_VALUES_[a.nodeName]);
    } else {
      for (a = a.firstChild; a;) {
        goog.dom.getTextContent_(a, b, c), a = a.nextSibling;
      }
    }
  }
};
goog.dom.getNodeTextLength = function(a) {
  return goog.dom.getTextContent(a).length;
};
goog.dom.getNodeTextOffset = function(a, b) {
  b = b || goog.dom.getOwnerDocument(a).body;
  for (var c = []; a && a != b;) {
    for (var d = a; d = d.previousSibling;) {
      c.unshift(goog.dom.getTextContent(d));
    }
    a = a.parentNode;
  }
  return goog.string.trimLeft(c.join("")).replace(/ +/g, " ").length;
};
goog.dom.getNodeAtOffset = function(a, b, c) {
  a = [a];
  for (var d = 0, e = null; a.length > 0 && d < b;) {
    if (e = a.pop(), !(e.nodeName in goog.dom.TAGS_TO_IGNORE_)) {
      if (e.nodeType == goog.dom.NodeType.TEXT) {
        var f = e.nodeValue.replace(/(\r\n|\r|\n)/g, "").replace(/ +/g, " ");
        d += f.length;
      } else if (e.nodeName in goog.dom.PREDEFINED_TAG_VALUES_) {
        d += goog.dom.PREDEFINED_TAG_VALUES_[e.nodeName].length;
      } else {
        for (f = e.childNodes.length - 1; f >= 0; f--) {
          a.push(e.childNodes[f]);
        }
      }
    }
  }
  goog.isObject(c) && (c.remainder = e ? e.nodeValue.length + b - d - 1 : 0, c.node = e);
  return e;
};
goog.dom.isNodeList = function(a) {
  if (a && typeof a.length == "number") {
    if (goog.isObject(a)) {
      return typeof a.item == "function" || typeof a.item == "string";
    }
    if (typeof a === "function") {
      return typeof a.item == "function";
    }
  }
  return !1;
};
goog.dom.getAncestorByTagNameAndClass = function(a, b, c, d) {
  if (!b && !c) {
    return null;
  }
  var e = b ? String(b).toUpperCase() : null;
  return goog.dom.getAncestor(a, function(f) {
    return (!e || f.nodeName == e) && (!c || typeof f.className === "string" && module$contents$goog$array_contains(f.className.split(/\s+/), c));
  }, !0, d);
};
goog.dom.getAncestorByClass = function(a, b, c) {
  return goog.dom.getAncestorByTagNameAndClass(a, null, b, c);
};
goog.dom.getAncestor = function(a, b, c, d) {
  a && !c && (a = a.parentNode);
  for (c = 0; a && (d == null || c <= d);) {
    goog.asserts.assert(a.name != "parentNode");
    if (b(a)) {
      return a;
    }
    a = a.parentNode;
    c++;
  }
  return null;
};
goog.dom.getActiveElement = function(a) {
  try {
    var b = a && a.activeElement;
    return b && b.nodeName ? b : null;
  } catch (c) {
    return null;
  }
};
goog.dom.getPixelRatio = function() {
  var a = goog.dom.getWindow();
  return a.devicePixelRatio !== void 0 ? a.devicePixelRatio : a.matchMedia ? goog.dom.matchesPixelRatio_(3) || goog.dom.matchesPixelRatio_(2) || goog.dom.matchesPixelRatio_(1.5) || goog.dom.matchesPixelRatio_(1) || .75 : 1;
};
goog.dom.matchesPixelRatio_ = function(a) {
  return goog.dom.getWindow().matchMedia("(min-resolution: " + a + "dppx),(min--moz-device-pixel-ratio: " + a + "),(min-resolution: " + a * 96 + "dpi)").matches ? a : 0;
};
goog.dom.getCanvasContext2D = function(a) {
  return a.getContext("2d");
};
goog.dom.DomHelper = function(a) {
  this.document_ = a || goog.global.document || document;
};
goog.dom.DomHelper.prototype.getDomHelper = goog.dom.getDomHelper;
goog.dom.DomHelper.prototype.setDocument = function(a) {
  this.document_ = a;
};
goog.dom.DomHelper.prototype.getDocument = function() {
  return this.document_;
};
goog.dom.DomHelper.prototype.getElement = function(a) {
  return goog.dom.getElementHelper_(this.document_, a);
};
goog.dom.DomHelper.prototype.getRequiredElement = function(a) {
  return goog.dom.getRequiredElementHelper_(this.document_, a);
};
goog.dom.DomHelper.prototype.$ = goog.dom.DomHelper.prototype.getElement;
goog.dom.DomHelper.prototype.getElementsByTagName = function(a, b) {
  return (b || this.document_).getElementsByTagName(String(a));
};
goog.dom.DomHelper.prototype.getElementsByTagNameAndClass = function(a, b, c) {
  return goog.dom.getElementsByTagNameAndClass_(this.document_, a, b, c);
};
goog.dom.DomHelper.prototype.getElementByTagNameAndClass = function(a, b, c) {
  return goog.dom.getElementByTagNameAndClass_(this.document_, a, b, c);
};
goog.dom.DomHelper.prototype.getElementsByClass = function(a, b) {
  return goog.dom.getElementsByClass(a, b || this.document_);
};
goog.dom.DomHelper.prototype.getElementByClass = function(a, b) {
  return goog.dom.getElementByClass(a, b || this.document_);
};
goog.dom.DomHelper.prototype.getRequiredElementByClass = function(a, b) {
  return goog.dom.getRequiredElementByClass(a, b || this.document_);
};
goog.dom.DomHelper.prototype.$$ = goog.dom.DomHelper.prototype.getElementsByTagNameAndClass;
goog.dom.DomHelper.prototype.setProperties = goog.dom.setProperties;
goog.dom.DomHelper.prototype.getViewportSize = function(a) {
  return goog.dom.getViewportSize(a || this.getWindow());
};
goog.dom.DomHelper.prototype.getDocumentHeight = function() {
  return goog.dom.getDocumentHeight_(this.getWindow());
};
goog.dom.DomHelper.prototype.createDom = function(a, b, c) {
  return goog.dom.createDom_(this.document_, arguments);
};
goog.dom.DomHelper.prototype.$dom = goog.dom.DomHelper.prototype.createDom;
goog.dom.DomHelper.prototype.createElement = function(a) {
  return goog.dom.createElement_(this.document_, a);
};
goog.dom.DomHelper.prototype.createTextNode = function(a) {
  return this.document_.createTextNode(String(a));
};
goog.dom.DomHelper.prototype.createTable = function(a, b, c) {
  return goog.dom.createTable_(this.document_, a, b, !!c);
};
goog.dom.DomHelper.prototype.safeHtmlToNode = function(a) {
  return goog.dom.safeHtmlToNode_(this.document_, a);
};
goog.dom.DomHelper.prototype.isCss1CompatMode = function() {
  return goog.dom.isCss1CompatMode_(this.document_);
};
goog.dom.DomHelper.prototype.getWindow = function() {
  return goog.dom.getWindow_(this.document_);
};
goog.dom.DomHelper.prototype.getDocumentScrollElement = function() {
  return goog.dom.getDocumentScrollElement_(this.document_);
};
goog.dom.DomHelper.prototype.getDocumentScroll = function() {
  return goog.dom.getDocumentScroll_(this.document_);
};
goog.dom.DomHelper.prototype.getActiveElement = function(a) {
  return goog.dom.getActiveElement(a || this.document_);
};
goog.dom.DomHelper.prototype.appendChild = goog.dom.appendChild;
goog.dom.DomHelper.prototype.append = goog.dom.append;
goog.dom.DomHelper.prototype.canHaveChildren = goog.dom.canHaveChildren;
goog.dom.DomHelper.prototype.removeChildren = goog.dom.removeChildren;
goog.dom.DomHelper.prototype.insertSiblingBefore = goog.dom.insertSiblingBefore;
goog.dom.DomHelper.prototype.insertSiblingAfter = goog.dom.insertSiblingAfter;
goog.dom.DomHelper.prototype.insertChildAt = goog.dom.insertChildAt;
goog.dom.DomHelper.prototype.removeNode = goog.dom.removeNode;
goog.dom.DomHelper.prototype.replaceNode = goog.dom.replaceNode;
goog.dom.DomHelper.prototype.copyContents = goog.dom.copyContents;
goog.dom.DomHelper.prototype.flattenElement = goog.dom.flattenElement;
goog.dom.DomHelper.prototype.getChildren = goog.dom.getChildren;
goog.dom.DomHelper.prototype.getFirstElementChild = goog.dom.getFirstElementChild;
goog.dom.DomHelper.prototype.getLastElementChild = goog.dom.getLastElementChild;
goog.dom.DomHelper.prototype.getNextElementSibling = goog.dom.getNextElementSibling;
goog.dom.DomHelper.prototype.getPreviousElementSibling = goog.dom.getPreviousElementSibling;
goog.dom.DomHelper.prototype.getNextNode = goog.dom.getNextNode;
goog.dom.DomHelper.prototype.getPreviousNode = goog.dom.getPreviousNode;
goog.dom.DomHelper.prototype.isNodeLike = goog.dom.isNodeLike;
goog.dom.DomHelper.prototype.isElement = goog.dom.isElement;
goog.dom.DomHelper.prototype.isWindow = goog.dom.isWindow;
goog.dom.DomHelper.prototype.getParentElement = goog.dom.getParentElement;
goog.dom.DomHelper.prototype.contains = goog.dom.contains;
goog.dom.DomHelper.prototype.compareNodeOrder = goog.dom.compareNodeOrder;
goog.dom.DomHelper.prototype.findCommonAncestor = goog.dom.findCommonAncestor;
goog.dom.DomHelper.prototype.getOwnerDocument = goog.dom.getOwnerDocument;
goog.dom.DomHelper.prototype.getFrameContentDocument = goog.dom.getFrameContentDocument;
goog.dom.DomHelper.prototype.getFrameContentWindow = goog.dom.getFrameContentWindow;
goog.dom.DomHelper.prototype.setTextContent = goog.dom.setTextContent;
goog.dom.DomHelper.prototype.getOuterHtml = goog.dom.getOuterHtml;
goog.dom.DomHelper.prototype.findNode = goog.dom.findNode;
goog.dom.DomHelper.prototype.findNodes = goog.dom.findNodes;
goog.dom.DomHelper.prototype.isFocusableTabIndex = goog.dom.isFocusableTabIndex;
goog.dom.DomHelper.prototype.setFocusableTabIndex = goog.dom.setFocusableTabIndex;
goog.dom.DomHelper.prototype.isFocusable = goog.dom.isFocusable;
goog.dom.DomHelper.prototype.getTextContent = goog.dom.getTextContent;
goog.dom.DomHelper.prototype.getNodeTextLength = goog.dom.getNodeTextLength;
goog.dom.DomHelper.prototype.getNodeTextOffset = goog.dom.getNodeTextOffset;
goog.dom.DomHelper.prototype.getNodeAtOffset = goog.dom.getNodeAtOffset;
goog.dom.DomHelper.prototype.isNodeList = goog.dom.isNodeList;
goog.dom.DomHelper.prototype.getAncestorByTagNameAndClass = goog.dom.getAncestorByTagNameAndClass;
goog.dom.DomHelper.prototype.getAncestorByClass = goog.dom.getAncestorByClass;
goog.dom.DomHelper.prototype.getAncestor = goog.dom.getAncestor;
goog.dom.DomHelper.prototype.getCanvasContext2D = goog.dom.getCanvasContext2D;
goog.events = {};
const module$contents$goog$events$BrowserFeature_purify = a => ({valueOf:a}).valueOf();
goog.events.BrowserFeature = {TOUCH_ENABLED:"ontouchstart" in goog.global || !!(goog.global.document && document.documentElement && "ontouchstart" in document.documentElement) || !(!goog.global.navigator || !goog.global.navigator.maxTouchPoints && !goog.global.navigator.msMaxTouchPoints), POINTER_EVENTS:"PointerEvent" in goog.global, MSPOINTER_EVENTS:!1, PASSIVE_EVENTS:module$contents$goog$events$BrowserFeature_purify(function() {
  if (!goog.global.addEventListener || !Object.defineProperty) {
    return !1;
  }
  var a = !1, b = Object.defineProperty({}, "passive", {get:function() {
    a = !0;
  }});
  try {
    const c = () => {
    };
    goog.global.addEventListener("test", c, b);
    goog.global.removeEventListener("test", c, b);
  } catch (c) {
  }
  return a;
})};
goog.events.eventTypeHelpers = {};
goog.events.eventTypeHelpers.getVendorPrefixedName = function(a) {
  return goog.userAgent.WEBKIT ? "webkit" + a : a.toLowerCase();
};
goog.events.eventTypeHelpers.getPointerFallbackEventName = function(a, b, c) {
  return goog.events.BrowserFeature.POINTER_EVENTS ? a : goog.events.BrowserFeature.MSPOINTER_EVENTS ? b : c;
};
goog.events.EventType = {CLICK:"click", RIGHTCLICK:"rightclick", DBLCLICK:"dblclick", AUXCLICK:"auxclick", MOUSEDOWN:"mousedown", MOUSEUP:"mouseup", MOUSEOVER:"mouseover", MOUSEOUT:"mouseout", MOUSEMOVE:"mousemove", MOUSEENTER:"mouseenter", MOUSELEAVE:"mouseleave", MOUSECANCEL:"mousecancel", SELECTIONCHANGE:"selectionchange", SELECTSTART:"selectstart", WHEEL:"wheel", KEYPRESS:"keypress", KEYDOWN:"keydown", KEYUP:"keyup", BLUR:"blur", FOCUS:"focus", DEACTIVATE:"deactivate", FOCUSIN:"focusin", FOCUSOUT:"focusout", 
CHANGE:"change", RESET:"reset", SELECT:"select", SUBMIT:"submit", INPUT:"input", PROPERTYCHANGE:"propertychange", DRAGSTART:"dragstart", DRAG:"drag", DRAGENTER:"dragenter", DRAGOVER:"dragover", DRAGLEAVE:"dragleave", DROP:"drop", DRAGEND:"dragend", TOUCHSTART:"touchstart", TOUCHMOVE:"touchmove", TOUCHEND:"touchend", TOUCHCANCEL:"touchcancel", BEFOREUNLOAD:"beforeunload", CONSOLEMESSAGE:"consolemessage", CONTEXTMENU:"contextmenu", DEVICECHANGE:"devicechange", DEVICEMOTION:"devicemotion", DEVICEORIENTATION:"deviceorientation", 
DOMCONTENTLOADED:"DOMContentLoaded", ERROR:"error", HELP:"help", LOAD:"load", LOSECAPTURE:"losecapture", ORIENTATIONCHANGE:"orientationchange", READYSTATECHANGE:"readystatechange", RESIZE:"resize", SCROLL:"scroll", UNLOAD:"unload", CANPLAY:"canplay", CANPLAYTHROUGH:"canplaythrough", DURATIONCHANGE:"durationchange", EMPTIED:"emptied", ENDED:"ended", LOADEDDATA:"loadeddata", LOADEDMETADATA:"loadedmetadata", PAUSE:"pause", PLAY:"play", PLAYING:"playing", PROGRESS:"progress", RATECHANGE:"ratechange", 
SEEKED:"seeked", SEEKING:"seeking", STALLED:"stalled", SUSPEND:"suspend", TIMEUPDATE:"timeupdate", VOLUMECHANGE:"volumechange", WAITING:"waiting", SOURCEOPEN:"sourceopen", SOURCEENDED:"sourceended", SOURCECLOSED:"sourceclosed", ABORT:"abort", UPDATE:"update", UPDATESTART:"updatestart", UPDATEEND:"updateend", HASHCHANGE:"hashchange", PAGEHIDE:"pagehide", PAGESHOW:"pageshow", POPSTATE:"popstate", COPY:"copy", PASTE:"paste", CUT:"cut", BEFORECOPY:"beforecopy", BEFORECUT:"beforecut", BEFOREPASTE:"beforepaste", 
ONLINE:"online", OFFLINE:"offline", MESSAGE:"message", CONNECT:"connect", INSTALL:"install", ACTIVATE:"activate", FETCH:"fetch", FOREIGNFETCH:"foreignfetch", MESSAGEERROR:"messageerror", STATECHANGE:"statechange", UPDATEFOUND:"updatefound", CONTROLLERCHANGE:"controllerchange", ANIMATIONSTART:goog.events.eventTypeHelpers.getVendorPrefixedName("AnimationStart"), ANIMATIONEND:goog.events.eventTypeHelpers.getVendorPrefixedName("AnimationEnd"), ANIMATIONITERATION:goog.events.eventTypeHelpers.getVendorPrefixedName("AnimationIteration"), 
TRANSITIONEND:goog.events.eventTypeHelpers.getVendorPrefixedName("TransitionEnd"), POINTERDOWN:"pointerdown", POINTERUP:"pointerup", POINTERCANCEL:"pointercancel", POINTERMOVE:"pointermove", POINTEROVER:"pointerover", POINTEROUT:"pointerout", POINTERENTER:"pointerenter", POINTERLEAVE:"pointerleave", GOTPOINTERCAPTURE:"gotpointercapture", LOSTPOINTERCAPTURE:"lostpointercapture", MSGESTURECHANGE:"MSGestureChange", MSGESTUREEND:"MSGestureEnd", MSGESTUREHOLD:"MSGestureHold", MSGESTURESTART:"MSGestureStart", 
MSGESTURETAP:"MSGestureTap", MSGOTPOINTERCAPTURE:"MSGotPointerCapture", MSINERTIASTART:"MSInertiaStart", MSLOSTPOINTERCAPTURE:"MSLostPointerCapture", MSPOINTERCANCEL:"MSPointerCancel", MSPOINTERDOWN:"MSPointerDown", MSPOINTERENTER:"MSPointerEnter", MSPOINTERHOVER:"MSPointerHover", MSPOINTERLEAVE:"MSPointerLeave", MSPOINTERMOVE:"MSPointerMove", MSPOINTEROUT:"MSPointerOut", MSPOINTEROVER:"MSPointerOver", MSPOINTERUP:"MSPointerUp", TEXT:"text", TEXTINPUT:goog.userAgent.IE ? "textinput" : "textInput", 
COMPOSITIONSTART:"compositionstart", COMPOSITIONUPDATE:"compositionupdate", COMPOSITIONEND:"compositionend", BEFOREINPUT:"beforeinput", FULLSCREENCHANGE:"fullscreenchange", WEBKITBEGINFULLSCREEN:"webkitbeginfullscreen", WEBKITENDFULLSCREEN:"webkitendfullscreen", EXIT:"exit", LOADABORT:"loadabort", LOADCOMMIT:"loadcommit", LOADREDIRECT:"loadredirect", LOADSTART:"loadstart", LOADSTOP:"loadstop", RESPONSIVE:"responsive", SIZECHANGED:"sizechanged", UNRESPONSIVE:"unresponsive", VISIBILITYCHANGE:"visibilitychange", 
STORAGE:"storage", DOMSUBTREEMODIFIED:"DOMSubtreeModified", DOMNODEINSERTED:"DOMNodeInserted", DOMNODEREMOVED:"DOMNodeRemoved", DOMNODEREMOVEDFROMDOCUMENT:"DOMNodeRemovedFromDocument", DOMNODEINSERTEDINTODOCUMENT:"DOMNodeInsertedIntoDocument", DOMATTRMODIFIED:"DOMAttrModified", DOMCHARACTERDATAMODIFIED:"DOMCharacterDataModified", BEFOREPRINT:"beforeprint", AFTERPRINT:"afterprint", BEFOREINSTALLPROMPT:"beforeinstallprompt", APPINSTALLED:"appinstalled", CANCEL:"cancel", FINISH:"finish", REMOVE:"remove"};
var pagespeed = {ResponsiveImageCandidate:function(a, b) {
  this.resolution = a;
  this.url = b;
}, ResponsiveImage:function(a) {
  this.img = a;
  this.currentResolution = 0;
  this.availableResolutions = [];
}, Responsive:function() {
  this.allImages_ = [];
}};
pagespeed.Responsive.updateImgSrc_ = function(a, b) {
  var c = new Image();
  c.onload = function() {
    a.src = b;
  };
  c.src = b;
};
pagespeed.ResponsiveImage.prototype.responsiveResize = function(a) {
  if (a > this.currentResolution) {
    for (var b = this.availableResolutions.length, c = 0; c < b; ++c) {
      if (a <= this.availableResolutions[c].resolution) {
        this.currentResolution = this.availableResolutions[c].resolution;
        pagespeed.Responsive.updateImgSrc_(this.img, this.availableResolutions[c].url);
        break;
      }
    }
  }
};
pagespeed.Responsive.prototype.computeDevicePixelRatioWithZoom = function() {
  var a = document.documentElement.clientWidth / window.innerWidth;
  return goog.dom.getPixelRatio() * a;
};
pagespeed.Responsive.prototype.responsiveResize = function() {
  for (var a = this.computeDevicePixelRatioWithZoom(), b = this.allImages_.length, c = 0; c < b; ++c) {
    this.allImages_[c].responsiveResize(a);
  }
};
pagespeed.Responsive.search_ = function(a, b) {
  b = a.search(b);
  return b == -1 ? a.length : b;
};
var WHITESPACE = /[ \t\n\f\r]/, NOT_WHITESPACE = /[^ \t\n\f\r]/, WHITESPACE_OR_COMMA = /[ \t\n\f\r,]/, NOT_WHITESPACE_OR_COMMA = /[^ \t\n\f\r,]/;
pagespeed.Responsive.prototype.parseSrcset = function(a, b, c) {
  a = new pagespeed.ResponsiveImage(a);
  var d = !1, e = pagespeed.Responsive.search_(c, NOT_WHITESPACE_OR_COMMA);
  for (c = c.slice(e); c.length > 0;) {
    e = pagespeed.Responsive.search_(c, WHITESPACE);
    var f = c.slice(0, e);
    c = c.slice(e);
    if (f[f.length - 1] == ",") {
      return null;
    }
    e = pagespeed.Responsive.search_(c, NOT_WHITESPACE);
    c = c.slice(e);
    e = pagespeed.Responsive.search_(c, WHITESPACE_OR_COMMA);
    var g = c.slice(0, e);
    c = c.slice(e);
    if (g.length > 1 && g[g.length - 1] == "x") {
      e = goog.string.toNumber(g.slice(0, -1));
      if (isNaN(e)) {
        return null;
      }
      a.availableResolutions.push(new pagespeed.ResponsiveImageCandidate(e, f));
      e == 1 && (d = !0);
    } else {
      return null;
    }
    e = pagespeed.Responsive.search_(c, NOT_WHITESPACE);
    c = c.slice(e);
    if (c.length > 0 && c[0] != ",") {
      return null;
    }
    c = c.slice(1);
    e = pagespeed.Responsive.search_(c, NOT_WHITESPACE_OR_COMMA);
    c = c.slice(e);
  }
  !d && b && a.availableResolutions.push(new pagespeed.ResponsiveImageCandidate(1, b));
  a.availableResolutions.sort(function(h, k) {
    return h.resolution - k.resolution;
  });
  return a;
};
pagespeed.Responsive.prototype.init = function() {
  for (var a = goog.dom.getElementsByTagName(goog.dom.TagName.IMG), b = 0, c; c = a[b]; ++b) {
    var d = c.getAttribute("src"), e = c.getAttribute("srcset");
    e && (c = this.parseSrcset(c, d, e), c != null && this.allImages_.push(c));
  }
  window.addEventListener(goog.events.EventType.RESIZE, goog.bind(this.responsiveResize, this));
  window.addEventListener(goog.events.EventType.TOUCHMOVE, goog.bind(function(f) {
    f.touches.length > 1 && this.responsiveResize();
  }, this));
  this.responsiveResize();
};
pagespeed.responsiveInstance = new pagespeed.Responsive();
pagespeed.responsiveInstance.init();
})();
