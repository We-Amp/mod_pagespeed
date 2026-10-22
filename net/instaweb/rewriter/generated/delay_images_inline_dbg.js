(() => {
  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.DelayImagesInline = function() {
    this.inlineMap_ = {};
  };
  pagespeed.DelayImagesInline.prototype.addLowResImages = function(url, lowResImage) {
    this.inlineMap_[url] = lowResImage;
  };
  pagespeed.DelayImagesInline.prototype.replaceElementSrc = function(elements) {
    for (var i = 0; i < elements.length; ++i) {
      var high_res_src = elements[i].getAttribute("data-pagespeed-high-res-src");
      var src = elements[i].getAttribute("src");
      if (high_res_src && !src) {
        var low_res = this.inlineMap_[high_res_src];
        if (low_res) {
          elements[i].setAttribute("src", low_res);
        }
      }
    }
  };
  pagespeed.DelayImagesInline.prototype.replaceWithLowRes = function() {
    this.replaceElementSrc(document.getElementsByTagName("img"));
    this.replaceElementSrc(document.getElementsByTagName("input"));
  };
  pagespeed.delayImagesInlineInit = function() {
    var temp = new pagespeed.DelayImagesInline();
    pagespeed.delayImagesInline = temp;
  };
})();
