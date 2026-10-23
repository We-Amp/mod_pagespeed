(() => {
  window.pagespeed = window.pagespeed || {};
  var pagespeed = window.pagespeed;
  pagespeed.getResourceTimingData = function() {
    if (window["performance"] && (window["performance"]["getEntries"] || window["performance"]["webkitGetEntries"])) {
      var totalFetchDuration = 0;
      var maxFetchDuration = 0;
      var numFetches = 0;
      var totalDnsDuration = 0;
      var numDnsLookups = 0;
      var totalConnectionTime = 0;
      var numConnections = 0;
      var totalTTFB = 0;
      var numTTFBRequests = 0;
      var totalBlockingTime = 0;
      var numRequestsBlocked = 0;
      var entryCountMap = {};
      var entries = window["performance"]["getEntries"] ? window["performance"].getEntries() : window["performance"].webkitGetEntries();
      for (var i = 0; i < entries.length; i++) {
        var duration = entries[i]["duration"];
        if (duration > 0) {
          totalFetchDuration += duration;
          ++numFetches;
          maxFetchDuration = Math.max(maxFetchDuration, duration);
        }
        var connectTime = entries[i]["connectEnd"] - entries[i]["connectStart"];
        if (connectTime > 0) {
          totalConnectionTime += connectTime;
          ++numConnections;
        }
        var dnsTime = entries[i]["domainLookupEnd"] - entries[i]["domainLookupStart"];
        if (dnsTime > 0) {
          totalDnsDuration += dnsTime;
          ++numDnsLookups;
        }
        var initiator = entries[i]["initiatorType"];
        if (entryCountMap[initiator]) {
          ++entryCountMap[initiator];
        } else {
          entryCountMap[initiator] = 1;
        }
        var blockingTime = entries[i]["requestStart"] - entries[i]["fetchStart"];
        if (blockingTime > 0) {
          totalBlockingTime += blockingTime;
          ++numRequestsBlocked;
        }
        var ttfb = entries[i]["responseStart"] - entries[i]["requestStart"];
        if (ttfb > 0) {
          totalTTFB += ttfb;
          ++numTTFBRequests;
        }
      }
      return "&afd=" + (numFetches ? Math.round(
        totalFetchDuration / numFetches
      ) : 0) + "&nfd=" + numFetches + "&mfd=" + Math.round(maxFetchDuration) + "&act=" + (numConnections ? Math.round(
        totalConnectionTime / numConnections
      ) : 0) + "&nct=" + numConnections + "&adt=" + (numDnsLookups ? Math.round(
        totalDnsDuration / numDnsLookups
      ) : 0) + "&ndt=" + numDnsLookups + "&abt=" + (numRequestsBlocked ? Math.round(
        totalBlockingTime / numRequestsBlocked
      ) : 0) + "&nbt=" + numRequestsBlocked + "&attfb=" + (numTTFBRequests ? Math.round(
        totalTTFB / numTTFBRequests
      ) : 0) + "&nttfb=" + numTTFBRequests + (entryCountMap["css"] ? "&rit_css=" + entryCountMap["css"] : "") + (entryCountMap["link"] ? "&rit_link=" + entryCountMap["link"] : "") + (entryCountMap["script"] ? "&rit_script=" + entryCountMap["script"] : "") + (entryCountMap["img"] ? "&rit_img=" + entryCountMap["img"] : "");
    }
    return "";
  };
})();
