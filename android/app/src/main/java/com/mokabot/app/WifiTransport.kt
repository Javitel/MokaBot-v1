package com.mokabot.app

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL

/** Plain HTTP to the MokaBot's web server on the local network. */
class WifiTransport(private val host: String) : Transport {
    override val label = "WiFi $host"
    override var onDisconnected: (() -> Unit)? = null

    override suspend fun request(method: String, target: String, body: ByteArray?): DeviceResponse =
        withContext(Dispatchers.IO) {
            val connection = URL("http://$host$target").openConnection() as HttpURLConnection
            try {
                connection.requestMethod = method
                connection.connectTimeout = 5000
                connection.readTimeout = 15000
                if (body != null) {
                    connection.doOutput = true
                    connection.setRequestProperty("Content-Type", "text/plain")
                    connection.outputStream.use { it.write(body) }
                }
                val status = connection.responseCode
                val stream = if (status >= 400) connection.errorStream else connection.inputStream
                val data = stream?.use { it.readBytes() } ?: ByteArray(0)
                val type = connection.contentType?.substringBefore(';') ?: "text/plain"
                DeviceResponse(status, type, data)
            } finally {
                connection.disconnect()
            }
        }

    override fun close() {}

    companion object {
        /** Checks that a MokaBot answers at [host]. */
        suspend fun connect(host: String): WifiTransport {
            val transport = WifiTransport(host)
            val response = transport.request("GET", "/brewSlots", null)
            if (response.status != 200) throw IllegalStateException("HTTP ${response.status}")
            return transport
        }
    }
}
