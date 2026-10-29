// Loaded first in the bundled brew and settings pages (see BrowserActivity).
// Routes their requests to the MokaBot through the app (WiFi or Bluetooth),
// saves their downloads with Android's file picker and hides what the app
// cannot do.
(function () {
  // ---- Device requests: fetch("/brewHistory?...") etc. go to MokaBridge.
  var pending = {};
  var nextId = 1;
  var originalFetch = window.fetch.bind(window);

  // The pages' own requests: same origin, outside the app's /app/ files.
  function isDeviceUrl(url) {
    return url.origin === location.origin && url.pathname.indexOf("/app/") !== 0;
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
      MokaBridge.request(id, method, url.pathname + url.search, body);
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
      blob.text().then(function (text) { MokaBridge.saveFile(name, text); });
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
