(function(){var pagespeedutils = {MAX_POST_SIZE:131072, sendBeacon:function(a, b, c) {
  if (window.XMLHttpRequest) {
    var d = new XMLHttpRequest();
  } else if (window.ActiveXObject) {
    try {
      d = new ActiveXObject("Msxml2.XMLHTTP");
    } catch (f) {
      try {
        d = new ActiveXObject("Microsoft.XMLHTTP");
      } catch (g) {
      }
    }
  }
  if (!d) {
    return !1;
  }
  var e = a.indexOf("?") == -1 ? "?" : "&";
  a = a + e + "url=" + encodeURIComponent(b);
  d.open("POST", a);
  d.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
  d.send(c);
  return !0;
}, addHandler:function(a, b, c) {
  if (a.addEventListener) {
    a.addEventListener(b, c, !1);
  } else if (a.attachEvent) {
    a.attachEvent("on" + b, c);
  } else {
    var d = a["on" + b];
    a["on" + b] = function() {
      c.call(this);
      d && d.call(this);
    };
  }
}, getPosition:function(a) {
  for (var b = a.offsetTop, c = a.offsetLeft; a.offsetParent;) {
    a = a.offsetParent, b += a.offsetTop, c += a.offsetLeft;
  }
  return {top:b, left:c};
}, getWindowSize:function() {
  return {height:window.innerHeight || document.documentElement.clientHeight || document.body.clientHeight, width:window.innerWidth || document.documentElement.clientWidth || document.body.clientWidth};
}, inViewport:function(a, b) {
  a = pagespeedutils.getPosition(a);
  return pagespeedutils.positionInViewport(a, b);
}, positionInViewport:function(a, b) {
  return a.top < b.height && a.left < b.width;
}, getRequestAnimationFrame:function() {
  return window.requestAnimationFrame || window.webkitRequestAnimationFrame || window.mozRequestAnimationFrame || window.oRequestAnimationFrame || window.msRequestAnimationFrame || null;
}};
pagespeedutils.now = Date.now || function() {
  return +new Date();
};
window.pagespeed = window.pagespeed || {};
var pagespeed = window.pagespeed;
pagespeed.LazyloadImages = function(a) {
  this.deferred_ = [];
  this.buffer_ = 0;
  this.force_load_ = !1;
  this.blank_image_src_ = a;
  this.scroll_timer_ = null;
  this.last_scroll_time_ = 0;
  this.min_scroll_time_ = 200;
  this.onload_done_ = !1;
  this.imgs_to_load_before_beaconing_ = 0;
};
pagespeed.LazyloadImages.prototype.countDeferredImgs_ = function() {
  for (var a = 0, b = document.getElementsByTagName("img"), c = 0, d; d = b[c]; c++) {
    d.src.indexOf(this.blank_image_src_) != -1 && this.hasAttribute_(d, "data-pagespeed-lazy-src") && a++;
  }
  return a;
};
pagespeed.LazyloadImages.prototype.viewport_ = function() {
  var a = 0;
  typeof window.pageYOffset == "number" ? a = window.pageYOffset : document.body && document.body.scrollTop ? a = document.body.scrollTop : document.documentElement && document.documentElement.scrollTop && (a = document.documentElement.scrollTop);
  var b = window.innerHeight || document.documentElement.clientHeight || document.body.clientHeight;
  return {top:a, bottom:a + b, height:b};
};
pagespeed.LazyloadImages.prototype.compute_top_ = function(a) {
  var b = a.getAttribute("data-pagespeed-lazy-position");
  if (b) {
    return parseInt(b, 0);
  }
  b = a.offsetTop;
  var c = a.offsetParent;
  c && (b += this.compute_top_(c));
  b = Math.max(b, 0);
  a.setAttribute("data-pagespeed-lazy-position", b);
  return b;
};
pagespeed.LazyloadImages.prototype.offset_ = function(a) {
  var b = this.compute_top_(a);
  return {top:b, bottom:b + a.offsetHeight};
};
pagespeed.LazyloadImages.prototype.isVisible_ = function(a) {
  if (!this.onload_done_ && (a.offsetHeight == 0 || a.offsetWidth == 0)) {
    return !1;
  }
  var b = this.viewport_(), c = a.getBoundingClientRect();
  c ? (a = c.top - b.height, b = c.bottom) : (c = this.offset_(a), a = c.top - b.bottom, b = c.bottom - b.top);
  return a <= this.buffer_ && b + this.buffer_ >= 0;
};
pagespeed.LazyloadImages.prototype.loadIfVisibleAndMaybeBeacon = function(a) {
  this.overrideAttributeFunctionsInternal_(a);
  var b = this;
  window.setTimeout(function() {
    var c = a.getAttribute("data-pagespeed-lazy-src");
    if (c) {
      if ((b.force_load_ || b.isVisible_(a)) && a.src.indexOf(b.blank_image_src_) != -1) {
        var d = a.parentNode, e = a.nextSibling;
        d && d.removeChild(a);
        a._getAttribute && (a.getAttribute = a._getAttribute);
        a.removeAttribute("onload");
        if (a.tagName && a.tagName == "IMG" && pagespeed.CriticalImages) {
          var f = !1, g = function() {
            f || (f = !0, b.onload_done_ && (b.imgs_to_load_before_beaconing_--, b.imgs_to_load_before_beaconing_ == 0 && pagespeed.CriticalImages.checkCriticalImages()));
          };
          pagespeedutils.addHandler(a, "load", function(h) {
            pagespeed.CriticalImages.checkImageForCriticality(this);
            g();
          });
          pagespeedutils.addHandler(a, "error", function(h) {
            g();
          });
        }
        a.removeAttribute("data-pagespeed-lazy-src");
        a.removeAttribute("data-pagespeed-lazy-replaced-functions");
        d && d.insertBefore(a, e);
        if (d = a.getAttribute("data-pagespeed-lazy-srcset")) {
          a.srcset = d, a.removeAttribute("data-pagespeed-lazy-srcset");
        }
        a.src = c;
      } else {
        b.deferred_.push(a);
      }
    }
  }, 0);
};
pagespeed.LazyloadImages.prototype.loadIfVisibleAndMaybeBeacon = pagespeed.LazyloadImages.prototype.loadIfVisibleAndMaybeBeacon;
pagespeed.LazyloadImages.prototype.loadAllImages = function() {
  this.force_load_ = !0;
  this.loadVisible_();
  for (var a = document.getElementsByTagName("img"), b = 0, c; c = a[b]; b++) {
    this.hasAttribute_(c, "data-pagespeed-lazy-src") && this.loadIfVisibleAndMaybeBeacon(c);
  }
};
pagespeed.LazyloadImages.prototype.loadAllImages = pagespeed.LazyloadImages.prototype.loadAllImages;
pagespeed.LazyloadImages.prototype.loadVisible_ = function() {
  var a = this.deferred_, b = a.length;
  this.deferred_ = [];
  for (var c = 0; c < b; ++c) {
    this.loadIfVisibleAndMaybeBeacon(a[c]);
  }
};
pagespeed.LazyloadImages.prototype.hasAttribute_ = function(a, b) {
  return a.getAttribute_ ? a.getAttribute_(b) != null : a.getAttribute(b) != null;
};
pagespeed.LazyloadImages.prototype.overrideAttributeFunctions = function() {
  for (var a = document.getElementsByTagName("img"), b = 0, c; c = a[b]; b++) {
    this.hasAttribute_(c, "data-pagespeed-lazy-src") && this.overrideAttributeFunctionsInternal_(c);
  }
};
pagespeed.LazyloadImages.prototype.overrideAttributeFunctions = pagespeed.LazyloadImages.prototype.overrideAttributeFunctions;
pagespeed.LazyloadImages.prototype.overrideAttributeFunctionsInternal_ = function(a) {
  var b = this;
  this.hasAttribute_(a, "data-pagespeed-lazy-replaced-functions") || (a._getAttribute = a.getAttribute, a.getAttribute = function(c) {
    c.toLowerCase() == "src" && b.hasAttribute_(this, "data-pagespeed-lazy-src") && (c = "data-pagespeed-lazy-src");
    return this._getAttribute(c);
  }, a.setAttribute("data-pagespeed-lazy-replaced-functions", "1"));
};
pagespeed.lazyLoadInit = function(a, b) {
  var c = new pagespeed.LazyloadImages(b);
  pagespeed.lazyLoadImages = c;
  pagespeedutils.addHandler(window, "load", function() {
    c.onload_done_ = !0;
    c.force_load_ = a;
    c.buffer_ = 200;
    pagespeed.CriticalImages && (c.imgs_to_load_before_beaconing_ = c.countDeferredImgs_(), c.imgs_to_load_before_beaconing_ == 0 && pagespeed.CriticalImages.checkCriticalImages());
    c.loadVisible_();
  });
  b.indexOf("data:") != 0 && ((new Image()).src = b);
  b = function() {
    if (!(c.onload_done_ && a || c.scroll_timer_)) {
      var d = (new Date()).getTime(), e = c.min_scroll_time_;
      d - c.last_scroll_time_ > c.min_scroll_time_ && (e = 0);
      c.scroll_timer_ = window.setTimeout(function() {
        c.last_scroll_time_ = (new Date()).getTime();
        c.loadVisible_();
        c.scroll_timer_ = null;
      }, e);
    }
  };
  pagespeedutils.addHandler(window, "scroll", b);
  pagespeedutils.addHandler(window, "resize", b);
};
pagespeed.lazyLoadInit = pagespeed.lazyLoadInit;
})();
