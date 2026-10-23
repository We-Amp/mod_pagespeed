(() => {
  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.DeferIframe = function() {
  };
  pagespeed.DeferIframe.prototype.convertToIframe = function() {
    var nodes = document.getElementsByTagName("pagespeed_iframe");
    if (nodes.length > 0) {
      var oldElement = nodes[0];
      var newElement = document.createElement("iframe");
      for (var i = 0, a = oldElement.attributes, n = a.length; i < n; ++i) {
        newElement.setAttribute(a[i].name, a[i].value);
      }
      oldElement.parentNode.replaceChild(newElement, oldElement);
    }
  };
  pagespeed.deferIframeInit = function() {
    var deferIframe = new pagespeed.DeferIframe();
    pagespeed.deferIframe = deferIframe;
  };
})();
