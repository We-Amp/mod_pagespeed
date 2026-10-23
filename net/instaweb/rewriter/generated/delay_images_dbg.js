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
  pagespeed.DelayImages = function() {
    this.lazyLoadHighResHandlersRegistered_ = false;
    this.highResReplaced_ = false;
  };
  pagespeed.DelayImages.prototype.replaceElementSrc = function(elements) {
    for (var i = 0; i < elements.length; ++i) {
      var src = elements[i].getAttribute("data-pagespeed-high-res-src");
      if (src) {
        elements[i].setAttribute("src", src);
      }
    }
  };
  pagespeed.DelayImages.prototype.registerLazyLoadHighRes = function() {
    if (this.lazyLoadHighResHandlersRegistered_) {
      this.highResReplaced_ = false;
      return;
    }
    var elem = document.body;
    var interval = 500;
    var tapStart, tapEnd = 0;
    var me = this;
    this.highResReplaced = false;
    if ("ontouchstart" in elem) {
      addHandler(elem, "touchstart", function(e) {
        tapStart = now();
      });
      addHandler(elem, "touchend", function(e) {
        tapEnd = now();
        if (e.changedTouches != null && e.changedTouches.length == 2 || e.touches != null && e.touches.length == 2 || tapEnd - tapStart < interval) {
          me.loadHighRes();
        }
      });
    } else {
      addHandler(window, "click", function(e) {
        me.loadHighRes();
      });
    }
    addHandler(window, "load", function(e) {
      me.loadHighRes();
    });
    this.lazyLoadHighResHandlersRegistered_ = true;
  };
  pagespeed.DelayImages.prototype.loadHighRes = function() {
    if (!this.highResReplaced_) {
      this.replaceWithHighRes();
      this.highResReplaced_ = true;
    }
  };
  pagespeed.DelayImages.prototype.replaceWithHighRes = function() {
    this.replaceElementSrc(document.getElementsByTagName("img"));
    this.replaceElementSrc(document.getElementsByTagName("input"));
  };
  pagespeed.delayImagesInit = function() {
    var temp = new pagespeed.DelayImages();
    pagespeed.delayImages = temp;
  };
})();
