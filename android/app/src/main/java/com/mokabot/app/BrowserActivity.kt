package com.mokabot.app

import android.annotation.SuppressLint
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.util.Base64
import android.view.Menu
import android.view.MenuItem
import android.webkit.JavascriptInterface
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.io.ByteArrayInputStream

/**
 * Shows the firmware's brew and settings pages (bundled in the APK) in a
 * WebView. Their data requests are routed through [Connection.transport] by
 * assets/app/bridge.js, so they work over WiFi and Bluetooth alike.
 */
class BrowserActivity : AppCompatActivity() {
    private lateinit var webView: WebView
    private var transport: Transport? = null

    // File picker of the settings page's brew import.
    private var fileCallback: ValueCallback<Array<Uri>>? = null
    private val openFile =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            fileCallback?.onReceiveValue(if (uri != null) arrayOf(uri) else null)
            fileCallback = null
        }

    // Brew download from the settings page.
    private var pendingSave: String? = null
    private val saveFile =
        registerForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
            val text = pendingSave
            pendingSave = null
            if (uri != null && text != null) {
                contentResolver.openOutputStream(uri)?.use { it.write(text.toByteArray()) }
                Toast.makeText(this, R.string.saved, Toast.LENGTH_SHORT).show()
            }
        }

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val current = Connection.transport
        if (current == null) {
            finish()
            return
        }
        transport = current
        setContentView(R.layout.activity_browser)
        supportActionBar?.subtitle = current.label
        current.onDisconnected = { runOnUiThread { connectionLost() } }

        webView = findViewById(R.id.webView)
        webView.settings.javaScriptEnabled = true
        webView.settings.domStorageEnabled = true   // the brew page keeps line colours there
        webView.addJavascriptInterface(Bridge(), "MokaBridge")
        webView.webViewClient = PageClient()
        webView.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(
                view: WebView,
                callback: ValueCallback<Array<Uri>>,
                params: FileChooserParams
            ): Boolean {
                fileCallback?.onReceiveValue(null)
                fileCallback = callback
                openFile.launch(arrayOf("application/json", "text/plain", "*/*"))
                return true
            }
        }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (webView.canGoBack()) webView.goBack() else finish()
            }
        })

        if (savedInstanceState == null) webView.loadUrl(PAGE_BREW)
        else webView.restoreState(savedInstanceState)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        if (::webView.isInitialized) webView.saveState(outState)
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.browser, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean = when (item.itemId) {
        R.id.menuBrew -> { webView.loadUrl(PAGE_BREW); true }
        R.id.menuSettings -> { webView.loadUrl(PAGE_SETTINGS); true }
        R.id.menuDisconnect -> { finish(); true }
        else -> super.onOptionsItemSelected(item)
    }

    override fun onDestroy() {
        super.onDestroy()
        // Leaving the pages (Disconnect, back) ends the connection.
        if (isFinishing && Connection.transport === transport) {
            transport?.onDisconnected = null
            transport?.close()
            Connection.transport = null
        }
    }

    private fun connectionLost() {
        if (isFinishing) return
        Toast.makeText(this, R.string.disconnected, Toast.LENGTH_LONG).show()
        finish()
    }

    /** Called by bridge.js (on a WebView thread). */
    private inner class Bridge {
        @JavascriptInterface
        fun request(id: Int, method: String, target: String, body: String?) {
            lifecycleScope.launch {
                val current = transport
                try {
                    if (current == null) throw IllegalStateException("not connected")
                    val response = current.request(method, target, body?.toByteArray())
                    deliver(id, response.status, response.contentType, response.body)
                } catch (e: Exception) {
                    // Status 0: the page's fetch() rejects, as on a network error.
                    deliver(id, 0, "text/plain", (e.message ?: "connection error").toByteArray())
                }
            }
        }

        @JavascriptInterface
        fun saveFile(name: String, text: String) {
            runOnUiThread {
                pendingSave = text
                saveFile.launch(name)
            }
        }
    }

    private fun deliver(id: Int, status: Int, type: String, body: ByteArray) {
        val base64 = Base64.encodeToString(body, Base64.NO_WRAP)
        val script = "window.__mokaBridgeDone($id,$status,${JSONObject.quote(type)},\"$base64\")"
        if (!isDestroyed) webView.evaluateJavascript(script, null)
    }

    /** Serves the bundled pages, the bridge and the pages' CDN libraries. */
    private inner class PageClient : WebViewClient() {
        override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? {
            val url = request.url
            if (url.host == APP_HOST) {
                return when (url.path) {
                    "/app/", "/app/brew", "/app/brew.html" -> page("pages/brew.html")
                    "/app/settings", "/app/settings.html" -> page("pages/settings.html")
                    "/app/bridge.js" -> asset("app/bridge.js", "text/javascript")
                    else -> WebResourceResponse("text/plain", "utf-8", 404, "Not found", null,
                        ByteArrayInputStream(ByteArray(0)))
                }
            }
            // The pages load these from a CDN; bundled so they work offline
            // (e.g. connected over Bluetooth without internet).
            if (url.host == "cdn.jsdelivr.net") {
                val path = url.path ?: ""
                if (path.contains("/chart.js@")) return asset("vendor/chart.umd.min.js", "text/javascript")
                if (path.contains("/@jaames/iro@")) return asset("vendor/iro.min.js", "text/javascript")
            }
            return null
        }

        private fun asset(name: String, type: String) =
            WebResourceResponse(type, "utf-8", assets.open(name))

        // The page with the bridge loaded first, before the page's own scripts.
        private fun page(name: String): WebResourceResponse {
            val html = assets.open(name).bufferedReader().use { it.readText() }
                .replaceFirst("<head>", "<head><script src=\"/app/bridge.js\"></script>")
            return WebResourceResponse("text/html", "utf-8", ByteArrayInputStream(html.toByteArray()))
        }
    }

    companion object {
        private const val APP_HOST = "appassets.androidplatform.net"
        private const val PAGE_BREW = "https://$APP_HOST/app/brew"
        private const val PAGE_SETTINGS = "https://$APP_HOST/app/settings"
    }
}
