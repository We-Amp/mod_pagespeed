(() => {
  var ENTER_KEY_CODE = 13;
  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.ClientDomainRewriter = function(mappedDomainNames) {
    this.mappedDomainNames_ = mappedDomainNames;
  };
  pagespeed.ClientDomainRewriter.prototype.anchorListener = function(event) {
    event = event || window.event;
    if (event.type == "keypress" && event.keyCode != ENTER_KEY_CODE) {
      return;
    }
    for (var target = event.target; target != null; target = target.parentNode) {
      if (target.tagName == "A") {
        this.processEvent(target.href, event);
        return;
      }
    }
  };
  pagespeed.ClientDomainRewriter.prototype.addEventListeners = function() {
    var me = this;
    document.body.onclick = function(event) {
      me.anchorListener(event);
    };
    document.body.onkeypress = function(event) {
      me.anchorListener(event);
    };
  };
  pagespeed.ClientDomainRewriter.prototype.processEvent = function(url, event) {
    for (var i = 0; i < this.mappedDomainNames_.length; i++) {
      if (url.indexOf(this.mappedDomainNames_[i]) == 0) {
        window.location = window.location.protocol + "//" + window.location.hostname + "/" + url.substr(this.mappedDomainNames_[i].length);
        event.preventDefault();
        return;
      }
    }
  };
  pagespeed.clientDomainRewriterInit = function(mappedDomainNames) {
    var clientDomainRewriter = new pagespeed.ClientDomainRewriter(
      mappedDomainNames
    );
    pagespeed.clientDomainRewriter = clientDomainRewriter;
    clientDomainRewriter.addEventListeners();
  };
})();
