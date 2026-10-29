package com.mokabot.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeout
import java.io.ByteArrayOutputStream
import java.util.UUID
import kotlin.coroutines.resume

/**
 * Bluetooth LE link to the MokaBot (Waveshare board), see the firmware's
 * src/logic/bleLink.hpp. Messages are split into packets of at most
 * (MTU - 3) bytes; the first byte of each packet flags the last one.
 *   request:  "<METHOD> <path>[?query]\n<body>"
 *   response: "<status> <content type>\n<body>"
 * One request at a time.
 */
@SuppressLint("MissingPermission")   // callers check the Bluetooth permissions first
class BleTransport private constructor(private val context: Context) : Transport {
    override val label = "Bluetooth"
    override var onDisconnected: (() -> Unit)? = null

    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var mtu = 23
    private val requestLock = Mutex()

    // Completed by the GATT callback.
    private var connectStep: CompletableDeferred<Unit>? = null
    private var writeDone: CompletableDeferred<Int>? = null
    private var responseDone: CompletableDeferred<ByteArray>? = null
    private val responseBuffer = ByteArrayOutputStream()
    @Volatile private var closed = false

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                // Larger packets: the board accepts up to 517.
                if (!g.requestMtu(517)) g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                val error = IllegalStateException("disconnected")
                connectStep?.completeExceptionally(error)
                writeDone?.completeExceptionally(error)
                responseDone?.completeExceptionally(error)
                g.close()
                if (!closed) {
                    closed = true
                    onDisconnected?.invoke()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(SERVICE_UUID)
            val tx = service?.getCharacteristic(TX_UUID)
            rx = service?.getCharacteristic(RX_UUID)
            if (tx == null || rx == null) {
                connectStep?.completeExceptionally(IllegalStateException("not a MokaBot"))
                return
            }
            // Responses arrive as notifications.
            g.setCharacteristicNotification(tx, true)
            val cccd = tx.getDescriptor(CCCD_UUID)
            val enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(cccd, enable)
            } else {
                @Suppress("DEPRECATION")
                cccd.value = enable
                @Suppress("DEPRECATION")
                g.writeDescriptor(cccd)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) connectStep?.complete(Unit)
            else connectStep?.completeExceptionally(IllegalStateException("notifications refused ($status)"))
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            writeDone?.complete(status)
        }

        // Android 13+
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            onPacket(value)
        }

        // Android 12 and older
        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) {
                @Suppress("DEPRECATION")
                onPacket(c.value ?: return)
            }
        }
    }

    private fun onPacket(packet: ByteArray) {
        if (packet.isEmpty()) return
        synchronized(responseBuffer) {
            responseBuffer.write(packet, 1, packet.size - 1)
            if (packet[0].toInt() and LAST_PACKET != 0) {
                val message = responseBuffer.toByteArray()
                responseBuffer.reset()
                responseDone?.complete(message)
            }
        }
    }

    private suspend fun open(device: BluetoothDevice) {
        val step = CompletableDeferred<Unit>()
        connectStep = step
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
        withTimeout(15_000) { step.await() }
    }

    private suspend fun writePacket(packet: ByteArray) {
        val g = gatt ?: throw IllegalStateException("not connected")
        val characteristic = rx ?: throw IllegalStateException("not connected")
        val done = CompletableDeferred<Int>()
        writeDone = done
        val type = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        val started = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(characteristic, packet, type) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            characteristic.writeType = type
            @Suppress("DEPRECATION")
            characteristic.value = packet
            @Suppress("DEPRECATION")
            g.writeCharacteristic(characteristic)
        }
        if (!started) throw IllegalStateException("write failed")
        val status = withTimeout(5_000) { done.await() }
        if (status != BluetoothGatt.GATT_SUCCESS) throw IllegalStateException("write failed ($status)")
    }

    override suspend fun request(method: String, target: String, body: ByteArray?): DeviceResponse =
        requestLock.withLock {
            val message = ByteArrayOutputStream().apply {
                write("$method $target\n".toByteArray())
                if (body != null) write(body)
            }.toByteArray()

            val response = CompletableDeferred<ByteArray>()
            synchronized(responseBuffer) {
                responseBuffer.reset()
                responseDone = response
            }
            val chunk = maxOf(mtu - 3 - 1, 19)
            var offset = 0
            do {
                val length = minOf(chunk, message.size - offset)
                val last = offset + length >= message.size
                val packet = ByteArray(length + 1)
                packet[0] = if (last) LAST_PACKET.toByte() else 0
                System.arraycopy(message, offset, packet, 1, length)
                writePacket(packet)
                offset += length
            } while (offset < message.size)

            // Large answers (a whole brew history) take a few seconds.
            val raw = withTimeout(30_000) { response.await() }
            parseResponse(raw)
        }

    private fun parseResponse(raw: ByteArray): DeviceResponse {
        val lineEnd = raw.indexOf('\n'.code.toByte()).let { if (it < 0) raw.size else it }
        val line = String(raw, 0, lineEnd)
        val status = line.substringBefore(' ').toIntOrNull() ?: 500
        val type = line.substringAfter(' ', "text/plain")
        val body = if (lineEnd < raw.size) raw.copyOfRange(lineEnd + 1, raw.size) else ByteArray(0)
        return DeviceResponse(status, type, body)
    }

    override fun close() {
        closed = true
        gatt?.disconnect()
        gatt?.close()
        gatt = null
    }

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("6d6f6b61-626f-7400-0000-000000000001")
        val RX_UUID: UUID = UUID.fromString("6d6f6b61-626f-7400-0000-000000000002")
        val TX_UUID: UUID = UUID.fromString("6d6f6b61-626f-7400-0000-000000000003")
        private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val LAST_PACKET = 0x01

        /** Scans for a MokaBot (its service UUID) for up to [timeoutMs]. */
        suspend fun scan(context: Context, timeoutMs: Long = 10_000): BluetoothDevice? {
            val manager = context.getSystemService(BluetoothManager::class.java)
            val scanner = manager?.adapter?.bluetoothLeScanner ?: return null
            val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
            var scanCallback: ScanCallback? = null
            return try {
                kotlinx.coroutines.withTimeoutOrNull(timeoutMs) {
                    suspendCancellableCoroutine<BluetoothDevice?> { continuation ->
                        scanCallback = object : ScanCallback() {
                            override fun onScanResult(callbackType: Int, result: ScanResult) {
                                if (continuation.isActive) continuation.resume(result.device)
                            }
                            override fun onScanFailed(errorCode: Int) {
                                if (continuation.isActive) continuation.resume(null)
                            }
                        }
                        scanner.startScan(listOf(filter), settings, scanCallback)
                    }
                }
            } finally {
                scanCallback?.let { scanner.stopScan(it) }
            }
        }

        suspend fun connect(context: Context, device: BluetoothDevice): BleTransport {
            val transport = BleTransport(context.applicationContext)
            try {
                transport.open(device)
            } catch (e: Exception) {
                transport.close()
                throw e
            }
            return transport
        }
    }
}
