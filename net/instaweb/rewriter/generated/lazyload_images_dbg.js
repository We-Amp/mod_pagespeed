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
  pagespeed.LazyloadImages = function(blankImageSrc) {
    this.deferred_ = [];
    this.buffer_ = 0;
    this.force_load_ = false;
    this.blank_image_src_ = blankImageSrc;
    this.scroll_timer_ = null;
    this.last_scroll_time_ = 0;
    this.min_scroll_time_ = 200;
    this.onload_done_ = false;
    this.imgs_to_load_before_beaconing_ = 0;
  };
  pagespeed.LazyloadImages.prototype.countDeferredImgs_ = function() {
    var deferredImgCount = 0;
    var imgs = document.getElementsByTagName("img");
    for (var i = 0, img; img = imgs[i]; i++) {
      if (img.src.indexOf(this.blank_image_src_) != -1 && this.hasAttribute_(img, "data-pagespeed-lazy-src")) {
        deferredImgCount++;
      }
    }
    return deferredImgCount;
  };
  pagespeed.LazyloadImages.prototype.viewport_ = function() {
    var scrollY = 0;
    if (typeof window.pageYOffset == "number") {
      scrollY = window.pageYOffset;
    } else if (document.body && document.body.scrollTop) {
      scrollY = document.body.scrollTop;
    } else if (document.documentElement && document.documentElement.scrollTop) {
      scrollY = document.documentElement.scrollTop;
    }
    var height = window.innerHeight || document.documentElement.clientHeight || document.body.clientHeight;
    return {
      top: scrollY,
      bottom: scrollY + height,
      height
    };
  };
  pagespeed.LazyloadImages.prototype.compute_top_ = function(element) {
    var position = element.getAttribute("data-pagespeed-lazy-position");
    if (position) {
      return parseInt(position, 0);
    }
    position = element.offsetTop;
    var parent = element.offsetParent;
    if (parent) {
      position += this.compute_top_(parent);
    }
    position = Math.max(position, 0);
    element.setAttribute("data-pagespeed-lazy-position", position);
    return position;
  };
  pagespeed.LazyloadImages.prototype.offset_ = function(element) {
    var top_position = this.compute_top_(element);
    return {
      top: top_position,
      bottom: top_position + element.offsetHeight
    };
  };
  pagespeed.LazyloadImages.prototype.isVisible_ = function(element) {
    if (!this.onload_done_ && (element.offsetHeight == 0 || element.offsetWidth == 0)) {
      return false;
    }
    var viewport = this.viewport_();
    var rect = element.getBoundingClientRect();
    var top_diff, bottom_diff;
    if (rect) {
      top_diff = rect.top - viewport.height;
      bottom_diff = rect.bottom;
    } else {
      var position = this.offset_(element);
      top_diff = position.top - viewport.bottom;
      bottom_diff = position.bottom - viewport.top;
    }
    return top_diff <= this.buffer_ && bottom_diff + this.buffer_ >= 0;
  };
  pagespeed.LazyloadImages.prototype.loadIfVisibleAndMaybeBeacon = function(element) {
    this.overrideAttributeFunctionsInternal_(element);
    var context = this;
    window.setTimeout(function() {
      var data_src = element.getAttribute("data-pagespeed-lazy-src");
      if (data_src) {
        if ((context.force_load_ || context.isVisible_(element)) && element.src.indexOf(context.blank_image_src_) != -1) {
          var parent_node = element.parentNode;
          var next_sibling = element.nextSibling;
          if (parent_node) {
            parent_node.removeChild(element);
          }
          if (element._getAttribute) {
            element.getAttribute = element._getAttribute;
          }
          element.removeAttribute("onload");
          if (element.tagName && element.tagName == "IMG") {
            if (pagespeed.CriticalImages) {
              var counted = false;
              var decrementAndMaybeBeacon = function() {
                if (counted) {
                  return;
                }
                counted = true;
                if (context.onload_done_) {
                  context.imgs_to_load_before_beaconing_--;
                  if (context.imgs_to_load_before_beaconing_ == 0) {
                    pagespeed.CriticalImages.checkCriticalImages();
                  }
                }
              };
              addHandler(element, "load", function(e) {
                pagespeed.CriticalImages.checkImageForCriticality(this);
                decrementAndMaybeBeacon();
              });
              addHandler(element, "error", function(e) {
                decrementAndMaybeBeacon();
              });
            }
          }
          element.removeAttribute("data-pagespeed-lazy-src");
          element.removeAttribute("data-pagespeed-lazy-replaced-functions");
          if (parent_node) {
            parent_node.insertBefore(element, next_sibling);
          }
          var srcset = element.getAttribute("data-pagespeed-lazy-srcset");
          if (srcset) {
            element.srcset = srcset;
            element.removeAttribute("data-pagespeed-lazy-srcset");
          }
          element.src = data_src;
        } else {
          context.deferred_.push(element);
        }
      }
    }, 0);
  };
  pagespeed.LazyloadImages.prototype.loadAllImages = function() {
    this.force_load_ = true;
    this.loadVisible_();
    var images = document.getElementsByTagName("img");
    for (var i = 0, element; element = images[i]; i++) {
      if (this.hasAttribute_(element, "data-pagespeed-lazy-src")) {
        this.loadIfVisibleAndMaybeBeacon(element);
      }
    }
  };
  pagespeed.LazyloadImages.prototype.loadVisible_ = function() {
    var old_deferred = this.deferred_;
    var len = old_deferred.length;
    this.deferred_ = [];
    for (var i = 0; i < len; ++i) {
      this.loadIfVisibleAndMaybeBeacon(old_deferred[i]);
    }
  };
  pagespeed.LazyloadImages.prototype.hasAttribute_ = function(element, attribute) {
    if (element.getAttribute_) {
      return element.getAttribute_(attribute) != null;
    }
    return element.getAttribute(attribute) != null;
  };
  pagespeed.LazyloadImages.prototype.overrideAttributeFunctions = function() {
    var images = document.getElementsByTagName("img");
    for (var i = 0, element; element = images[i]; i++) {
      if (this.hasAttribute_(element, "data-pagespeed-lazy-src")) {
        this.overrideAttributeFunctionsInternal_(element);
      }
    }
  };
  pagespeed.LazyloadImages.prototype.overrideAttributeFunctionsInternal_ = function(element) {
    var context = this;
    if (!this.hasAttribute_(element, "data-pagespeed-lazy-replaced-functions")) {
      element._getAttribute = element.getAttribute;
      element.getAttribute = function(name) {
        if (name.toLowerCase() == "src" && context.hasAttribute_(this, "data-pagespeed-lazy-src")) {
          name = "data-pagespeed-lazy-src";
        }
        return this._getAttribute(name);
      };
      element.setAttribute("data-pagespeed-lazy-replaced-functions", "1");
    }
  };
  pagespeed.lazyLoadInit = function(loadAfterOnload, blankImageSrc) {
    var context = new pagespeed.LazyloadImages(blankImageSrc);
    pagespeed.lazyLoadImages = context;
    var lazy_onload = function() {
      context.onload_done_ = true;
      context.force_load_ = loadAfterOnload;
      context.buffer_ = 200;
      if (pagespeed.CriticalImages) {
        context.imgs_to_load_before_beaconing_ = context.countDeferredImgs_();
        if (context.imgs_to_load_before_beaconing_ == 0) {
          pagespeed.CriticalImages.checkCriticalImages();
        }
      }
      context.loadVisible_();
    };
    addHandler(window, "load", lazy_onload);
    if (blankImageSrc.indexOf("data:") != 0) {
      new Image().src = blankImageSrc;
    }
    var lazy_onscroll = function() {
      if (context.onload_done_ && loadAfterOnload) {
        return;
      }
      if (!context.scroll_timer_) {
        var now2 = (new Date()).getTime();
        var timeout_ms = context.min_scroll_time_;
        if (now2 - context.last_scroll_time_ > context.min_scroll_time_) {
          timeout_ms = 0;
        }
        context.scroll_timer_ = window.setTimeout(function() {
          context.last_scroll_time_ = (new Date()).getTime();
          context.loadVisible_();
          context.scroll_timer_ = null;
        }, timeout_ms);
      }
    };
    addHandler(window, "scroll", lazy_onscroll);
    addHandler(window, "resize", lazy_onscroll);
  };
})();
