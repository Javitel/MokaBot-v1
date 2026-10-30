package com.mokabot.app

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketTimeoutException

/** A MokaBot found on the WiFi network. */
class NetworkDevice(val name: String, val ip: String)

/**
 * WiFi "Search", see the firmware's src/logic/discovery.hpp: broadcasts
 * "MOKABOT?" to UDP port 41820 and collects the replies
 * ({"name":"...","ip":"..."}) for [durationMs].
 */
object Discovery {
    private const val PORT = 41820
    private const val REQUEST = "MOKABOT?"

    suspend fun search(durationMs: Long = 1_500): List<NetworkDevice> = withContext(Dispatchers.IO) {
        val found = LinkedHashMap<String, NetworkDevice>()
        DatagramSocket().use { socket ->
            socket.broadcast = true
            val request = REQUEST.toByteArray()
            socket.send(DatagramPacket(request, request.size, InetAddress.getByName("255.255.255.255"), PORT))
            val deadline = System.currentTimeMillis() + durationMs
            val buffer = ByteArray(256)
            while (true) {
                val remaining = deadline - System.currentTimeMillis()
                if (remaining <= 0) break
                socket.soTimeout = remaining.toInt()
                val packet = DatagramPacket(buffer, buffer.size)
                try {
                    socket.receive(packet)
                } catch (e: SocketTimeoutException) {
                    break
                }
                try {
                    val reply = JSONObject(String(packet.data, 0, packet.length))
                    val ip = packet.address.hostAddress ?: reply.optString("ip")
                    found[ip] = NetworkDevice(reply.optString("name", "mokabot"), ip)
                } catch (e: Exception) {
                    // Not a MokaBot reply.
                }
            }
        }
        found.values.toList()
    }
}
