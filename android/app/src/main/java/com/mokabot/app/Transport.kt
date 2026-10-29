package com.mokabot.app

/** Answer of the MokaBot to one request (same as its web server's). */
class DeviceResponse(val status: Int, val contentType: String, val body: ByteArray)

/**
 * A connection to the MokaBot. The pages' data requests (/brewHistory,
 * /settingsInfo, /set...) go through it, over WiFi (HTTP) or Bluetooth LE.
 */
interface Transport {
    /** Short description for the title bar, e.g. "WiFi 192.168.1.20". */
    val label: String

    /** Called when the connection drops (Bluetooth only). */
    var onDisconnected: (() -> Unit)?

    /** [target] is the path with its query, e.g. "/brewHistory?session=1&from=0". */
    suspend fun request(method: String, target: String, body: ByteArray?): DeviceResponse

    fun close()
}

/** The connection in use, shared by the connection screen and the pages. */
object Connection {
    var transport: Transport? = null
}
