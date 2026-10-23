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
  pagespeed.CriticalCssLoader = pagespeed.CriticalCssLoader || {};
  pagespeed.CriticalCssLoader.DEFERRED_ATTRIBUTE_ = "data-pagespeed-deferred-css";
  pagespeed.CriticalCssLoader.started_ = false;
  pagespeed.CriticalCssLoader.apply_ = function(link) {
    if (link.getAttribute("rel") == "preload") {
      link.setAttribute("rel", "stylesheet");
    }
  };
  pagespeed.CriticalCssLoader.onLinkEvent_ = function(event) {
    var target = event.target;
    if (target && target.nodeName == "LINK" && target.hasAttribute(pagespeed.CriticalCssLoader.DEFERRED_ATTRIBUTE_)) {
      pagespeed.CriticalCssLoader.apply_(target);
    }
  };
  pagespeed.CriticalCssLoader.applyAll = function() {
    var links = document.querySelectorAll(
      "link[" + pagespeed.CriticalCssLoader.DEFERRED_ATTRIBUTE_ + "]"
    );
    for (var i = 0; i < links.length; ++i) {
      pagespeed.CriticalCssLoader.apply_(links[i]);
    }
  };
  pagespeed.CriticalCssLoader.Run = function() {
    if (pagespeed.CriticalCssLoader.started_) {
      return;
    }
    pagespeed.CriticalCssLoader.started_ = true;
    document.addEventListener(
      "load",
      pagespeed.CriticalCssLoader.onLinkEvent_,
      true
    );
    document.addEventListener(
      "error",
      pagespeed.CriticalCssLoader.onLinkEvent_,
      true
    );
    var probe = document.createElement("link");
    var supportsPreload = !!(probe.relList && probe.relList.supports && probe.relList.supports("preload"));
    if (!supportsPreload) {
      addHandler(
        document,
        "DOMContentLoaded",
        pagespeed.CriticalCssLoader.applyAll
      );
    }
    addHandler(
      window,
      "load",
      pagespeed.CriticalCssLoader.applyAll
    );
    pagespeed.CriticalCssLoader.applyAll();
  };
})();
