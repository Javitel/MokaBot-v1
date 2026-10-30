// Loaded first in the bundled brew and settings pages of the Android app
// (BrowserActivity) and the iOS app (BrowserView). Routes their requests to
// the MokaBot through the app (WiFi or Bluetooth), saves their downloads with
// the system's file dialog and hides what the apps cannot do.
(function () {
  // The app side: Android's MokaBridge object or iOS's message handler.
  var native = window.MokaBridge ? {
    request: function (id, method, target, body) { MokaBridge.request(id, method, target, body); },
    saveFile: function (name, text) { MokaBridge.saveFile(name, text); }
  } : {
    request: function (id, method, target, body) {
      window.webkit.messageHandlers.mokaBridge.postMessage(
        { type: "request", id: id, method: method, target: target, body: body });
    },
    saveFile: function (name, text) {
      window.webkit.messageHandlers.mokaBridge.postMessage({ type: "saveFile", name: name, text: text });
    }
  };

  // ---- Device requests: fetch("/brewHistory?...") etc. go to MokaBridge.
  var pending = {};
  var nextId = 1;
  var originalFetch = window.fetch.bind(window);

  // The pages' own requests: same host, outside the app's /app/ files. (Not
  // compared by origin: the iOS app's custom scheme has an opaque origin.)
  function isDeviceUrl(url) {
    return url.protocol === location.protocol && url.host === location.host &&
      url.pathname.indexOf("/app/") !== 0;
  }

  // Called by the app with the device's answer (body in base64). Status 0 is
  // a connection error: fetch() rejects, as it does on a network error.
  window.__mokaBridgeDone = function (id, status, type, base64) {
    var request = pending[id];
    if (!request) return;
    delete pending[id];
    var binary = atob(base64);
    if (status === 0) {
      request.reject(new TypeError(binary || "Connection error"));
      return;
    }
    var bytes = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    request.resolve(new Response(bytes, { status: status, headers: { "Content-Type": type } }));
  };

  window.fetch = function (input, init) {
    var url = new URL(typeof input === "string" ? input : input.url, location.href);
    if (!isDeviceUrl(url)) return originalFetch(input, init);
    init = init || {};
    var method = (init.method || "GET").toUpperCase();
    var body = init.body === undefined || init.body === null ? null : String(init.body);
    return new Promise(function (resolve, reject) {
      var id = nextId++;
      pending[id] = { resolve: resolve, reject: reject };
      native.request(id, method, url.pathname + url.search, body);
    });
  };

  // ---- Downloads: WebView ignores <a download href="blob:...">; the blob is
  // kept when its URL is created and handed to the app on click.
  var blobs = {};
  var originalCreate = URL.createObjectURL;
  var originalRevoke = URL.revokeObjectURL;
  URL.createObjectURL = function (object) {
    var url = originalCreate.call(URL, object);
    blobs[url] = object;
    return url;
  };
  URL.revokeObjectURL = function (url) {
    delete blobs[url];
    return originalRevoke.call(URL, url);
  };
  var originalClick = HTMLAnchorElement.prototype.click;
  HTMLAnchorElement.prototype.click = function () {
    var blob = blobs[this.href];
    if (this.download && blob) {
      var name = this.download;
      blob.text().then(function (text) { native.saveFile(name, text); });
      return;
    }
    return originalClick.call(this);
  };

  // ---- Firmware upload (settings page): a 2 MB upload is not supported over
  // the app; OTA or the browser page do it.
  document.addEventListener("DOMContentLoaded", function () {
    var upload = document.getElementById("firmwareUpload");
    var fieldset = upload && upload.closest("fieldset");
    if (fieldset) {
      fieldset.innerHTML = "<legend>Upload firmware</legend>" +
        "Not available in the app: update over WiFi with OTA (long press the colour " +
        "button on the screen) or from the settings page in a browser.";
    }
  });
})();
