package com.mokabot.app

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch

/**
 * Connection screen: Bluetooth or WiFi. With several MokaBots in reach, the
 * user picks one from a list (by device name and signal); the app remembers
 * the last Bluetooth device and connects straight to it next time.
 */
class MainActivity : AppCompatActivity() {
    private lateinit var bluetoothButton: Button
    private lateinit var chooseButton: Button
    private lateinit var wifiButton: Button
    private lateinit var searchButton: Button
    private lateinit var addressInput: EditText
    private lateinit var progress: ProgressBar
    private lateinit var statusText: TextView

    private val bluetoothPermissions =
        if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    // Whether the Bluetooth connection waiting for the permissions should
    // always show the device list.
    private var chooseAfterPermission = false
    private val permissionRequest =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
            if (granted.values.all { it }) connectBluetooth(chooseAfterPermission)
            else showStatus(getString(R.string.status_no_permission))
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        bluetoothButton = findViewById(R.id.bluetoothButton)
        chooseButton = findViewById(R.id.chooseButton)
        wifiButton = findViewById(R.id.wifiButton)
        searchButton = findViewById(R.id.searchButton)
        addressInput = findViewById(R.id.addressInput)
        progress = findViewById(R.id.progress)
        statusText = findViewById(R.id.statusText)

        addressInput.setText(prefs().getString(KEY_ADDRESS, "mokabot.local"))

        bluetoothButton.setOnClickListener { requestBluetooth(choose = false) }
        chooseButton.setOnClickListener { requestBluetooth(choose = true) }
        wifiButton.setOnClickListener { connectWifi(addressInput.text.toString()) }
        searchButton.setOnClickListener { searchWifi() }
    }

    private fun prefs() = getPreferences(MODE_PRIVATE)

    private fun granted(permission: String) =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    private fun setBusy(busy: Boolean, status: String = "") {
        for (button in listOf(bluetoothButton, chooseButton, wifiButton, searchButton)) button.isEnabled = !busy
        progress.visibility = if (busy) View.VISIBLE else View.GONE
        statusText.text = status
    }

    private fun showStatus(status: String) = setBusy(false, status)

    private fun failed(e: Exception) =
        showStatus(getString(R.string.status_failed, e.message ?: e.javaClass.simpleName))

    // ---- Bluetooth ----

    private fun requestBluetooth(choose: Boolean) {
        if (bluetoothPermissions.all { granted(it) }) {
            connectBluetooth(choose)
        } else {
            chooseAfterPermission = choose
            permissionRequest.launch(bluetoothPermissions)
        }
    }

    /**
     * Scans for MokaBots. Without [choose]: connects to the last device used
     * as soon as it is seen, or to the only one found; with several, or with
     * [choose], shows the list.
     */
    private fun connectBluetooth(choose: Boolean) {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null || !adapter.isEnabled) {
            showStatus(getString(R.string.status_bt_off))
            return
        }
        val lastDevice = prefs().getString(KEY_DEVICE, null)
        setBusy(true, getString(R.string.status_scanning))
        lifecycleScope.launch {
            try {
                val found = BleTransport.scan(this@MainActivity, stopOn = if (choose) null else lastDevice)
                val last = found.firstOrNull { it.device.address == lastDevice }
                when {
                    found.isEmpty() -> showStatus(getString(R.string.status_not_found))
                    !choose && last != null -> openBluetooth(last)
                    !choose && found.size == 1 -> openBluetooth(found[0])
                    else -> chooseBluetooth(found, lastDevice)
                }
            } catch (e: Exception) {
                failed(e)
            }
        }
    }

    private fun chooseBluetooth(found: List<FoundDevice>, lastDevice: String?) {
        setBusy(false)
        val labels = found.map { device ->
            var label = getString(R.string.device_entry, device.name, signalLabel(device.rssi), device.rssi)
            if (device.device.address == lastDevice) label += getString(R.string.device_last_used)
            label
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.choose_device_title)
            .setItems(labels.toTypedArray()) { _, which -> openBluetooth(found[which]) }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun signalLabel(rssi: Int) = getString(
        when {
            rssi >= -60 -> R.string.signal_near
            rssi >= -75 -> R.string.signal_medium
            else -> R.string.signal_far
        })

    private fun openBluetooth(found: FoundDevice) {
        setBusy(true, getString(R.string.status_connecting_to, found.name))
        lifecycleScope.launch {
            try {
                val transport = BleTransport.connect(this@MainActivity, found.device)
                prefs().edit().putString(KEY_DEVICE, found.device.address).apply()
                open(transport)
            } catch (e: Exception) {
                failed(e)
            }
        }
    }

    // ---- WiFi ----

    private fun connectWifi(address: String) {
        val host = address.trim().removePrefix("http://").removeSuffix("/")
        if (host.isEmpty()) return
        addressInput.setText(host)
        prefs().edit().putString(KEY_ADDRESS, host).apply()
        setBusy(true, getString(R.string.status_connecting))
        lifecycleScope.launch {
            try {
                open(WifiTransport.connect(host))
            } catch (e: Exception) {
                failed(e)
            }
        }
    }

    /** Finds the MokaBots on the WiFi network; with several, shows the list. */
    private fun searchWifi() {
        setBusy(true, getString(R.string.status_searching_wifi))
        lifecycleScope.launch {
            try {
                val found = Discovery.search()
                when (found.size) {
                    0 -> showStatus(getString(R.string.status_wifi_not_found))
                    1 -> connectWifi(found[0].ip)
                    else -> {
                        setBusy(false)
                        AlertDialog.Builder(this@MainActivity)
                            .setTitle(R.string.choose_device_title)
                            .setItems(found.map { "${it.name}  (${it.ip})" }.toTypedArray()) { _, which ->
                                connectWifi(found[which].ip)
                            }
                            .setNegativeButton(android.R.string.cancel, null)
                            .show()
                    }
                }
            } catch (e: Exception) {
                failed(e)
            }
        }
    }

    private fun open(transport: Transport) {
        Connection.transport?.close()
        Connection.transport = transport
        setBusy(false)
        startActivity(Intent(this, BrowserActivity::class.java))
    }

    companion object {
        private const val KEY_ADDRESS = "address"
        private const val KEY_DEVICE = "bluetoothDevice"
    }
}
