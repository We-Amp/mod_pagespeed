(() => {
  var addHandler = function(elem, eventName, func) {
    if (elem.addEventListener) {
      elem.addEventListener(eventName, func, false);
    } else if (elem.attachEvent) {
      elem.attachEvent("on" + eventName, func);
    } else {
      var oldHandler = elem["on" + eventName];
      elem["on" + eventName] = function() {
        func.call(this);
        if (oldHandler) {
          oldHandler.call(this);
        }
      };
    }
  };
  var now = Date.now || (function() {
    return +new Date();
  });

  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.LocalStorageCache = function() {
    this.regenerate_cookie_ = true;
  };
  pagespeed.LocalStorageCache.prototype.hasExpired = function(obj) {
    var expiry = parseInt(obj.substring(0, obj.indexOf(" ")), 10);
    return !isNaN(expiry) && expiry <= now();
  };
  pagespeed.LocalStorageCache.prototype.getData = function(obj) {
    var pos1 = obj.indexOf(" ");
    var pos2 = obj.indexOf(" ", pos1 + 1);
    return obj.substring(pos2 + 1);
  };
  pagespeed.LocalStorageCache.prototype.replaceLastScript = function(newElement) {
    var scripts = document.getElementsByTagName("script");
    var lastElement = scripts[scripts.length - 1];
    lastElement.parentNode.replaceChild(newElement, lastElement);
  };
  pagespeed.LocalStorageCache.prototype.inlineCss = function(url) {
    var obj = null;
    try {
      obj = window.localStorage.getItem("pagespeed_lsc_url:" + url);
    } catch (e) {
    }
    var newNode = document.createElement(obj ? "style" : "link");
    if (obj && !this.hasExpired(obj)) {
      newNode.type = "text/css";
      newNode.appendChild(document.createTextNode(this.getData(obj)));
    } else {
      newNode.rel = "stylesheet";
      newNode.href = url;
      this.regenerate_cookie_ = true;
    }
    this.replaceLastScript(newNode);
  };
  pagespeed.LocalStorageCache.prototype.inlineImg = function(url, hash) {
    var obj = null;
    try {
      obj = window.localStorage.getItem("pagespeed_lsc_url:" + url + " pagespeed_lsc_hash:" + hash);
    } catch (e) {
    }
    var newNode = document.createElement("img");
    if (obj && !this.hasExpired(obj)) {
      newNode.src = this.getData(obj);
    } else {
      newNode.src = url;
      this.regenerate_cookie_ = true;
    }
    for (var i = 2, n = arguments.length; i < n; ++i) {
      var pos = arguments[i].indexOf("=");
      newNode.setAttribute(
        arguments[i].substring(0, pos),
        arguments[i].substring(pos + 1)
      );
    }
    this.replaceLastScript(newNode);
  };
  pagespeed.LocalStorageCache.prototype.processTags_ = function(tagName, isHashInKey, dataFunc) {
    var elements = document.getElementsByTagName(tagName);
    for (var i = 0, n = elements.length; i < n; ++i) {
      var element = elements[i];
      var hash = element.getAttribute("data-pagespeed-lsc-hash");
      var url = element.getAttribute("data-pagespeed-lsc-url");
      if (hash && url) {
        var urlkey = "pagespeed_lsc_url:" + url;
        if (isHashInKey) {
          urlkey += " pagespeed_lsc_hash:" + hash;
        }
        var expiry = element.getAttribute("data-pagespeed-lsc-expiry");
        var millis = expiry ? new Date(expiry).getTime() : "";
        var data = dataFunc(element);
        if (!data) {
          var obj = null;
          try {
            obj = window.localStorage.getItem(urlkey);
          } catch (e) {
          }
          if (obj) {
            data = this.getData(obj);
          }
        }
        if (data) {
          try {
            window.localStorage.setItem(
              urlkey,
              millis + " " + hash + " " + data
            );
            this.regenerate_cookie_ = true;
          } catch (e) {
          }
        }
      }
    }
  };
  pagespeed.LocalStorageCache.prototype.saveInlinedData_ = function() {
    this.processTags_(
      "img",
      true,
      function(e) {
        return e.src;
      }
    );
    this.processTags_(
      "style",
      false,
      function(e) {
        return e.firstChild ? e.firstChild.nodeValue : null;
      }
    );
  };
  pagespeed.LocalStorageCache.prototype.generateCookie_ = function() {
    if (this.regenerate_cookie_) {
      var deadUns = [];
      var goodUns = [];
      var minExpiry = 0;
      var currentTime = now();
      var numItems = 0;
      try {
        numItems = window.localStorage.length;
      } catch (e) {
      }
      for (var i = 0, n = numItems; i < n; ++i) {
        try {
          var key = window.localStorage.key(i);
          if (key.indexOf("pagespeed_lsc_url:")) continue;
          var obj = window.localStorage.getItem(key);
          var pos1 = obj.indexOf(" ");
          var expiry = parseInt(obj.substring(0, pos1), 10);
          if (!isNaN(expiry)) {
            if (expiry <= currentTime) {
              deadUns.push(key);
              continue;
            } else if (expiry < minExpiry || minExpiry == 0) {
              minExpiry = expiry;
            }
          }
          var pos2 = obj.indexOf(" ", pos1 + 1);
          var hash = obj.substring(pos1 + 1, pos2);
          goodUns.push(hash);
        } catch (e) {
          continue;
        }
      }
      var expires = "";
      if (minExpiry) expires = "; expires=" + new Date(minExpiry).toUTCString();
      document.cookie = "_GPSLSC=" + goodUns.join("!") + ";path=/" + expires;
      for (var i = 0, n = deadUns.length; i < n; ++i) {
        try {
          window.localStorage.removeItem(deadUns[i]);
        } catch (e) {
        }
      }
      this.regenerate_cookie_ = false;
    }
  };
  pagespeed.localStorageCacheInit = function() {
    var temp = new pagespeed.LocalStorageCache();
    pagespeed.localStorageCache = temp;
    addHandler(
      window,
      "load",
      function() {
        temp.saveInlinedData_();
      }
    );
    addHandler(
      window,
      "load",
      function() {
        temp.generateCookie_();
      }
    );
  };
})();
