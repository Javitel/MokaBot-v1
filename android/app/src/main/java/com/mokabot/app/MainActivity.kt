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
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch

/** Connection screen: Bluetooth (Waveshare board) or WiFi (any board). */
class MainActivity : AppCompatActivity() {
    private lateinit var bluetoothButton: Button
    private lateinit var wifiButton: Button
    private lateinit var addressInput: EditText
    private lateinit var progress: ProgressBar
    private lateinit var statusText: TextView

    private val bluetoothPermissions =
        if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private val permissionRequest =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
            if (granted.values.all { it }) connectBluetooth()
            else showStatus(getString(R.string.status_no_permission))
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        bluetoothButton = findViewById(R.id.bluetoothButton)
        wifiButton = findViewById(R.id.wifiButton)
        addressInput = findViewById(R.id.addressInput)
        progress = findViewById(R.id.progress)
        statusText = findViewById(R.id.statusText)

        val prefs = getPreferences(MODE_PRIVATE)
        addressInput.setText(prefs.getString(KEY_ADDRESS, "mokabot.local"))

        bluetoothButton.setOnClickListener {
            if (bluetoothPermissions.all { granted(it) }) connectBluetooth()
            else permissionRequest.launch(bluetoothPermissions)
        }
        wifiButton.setOnClickListener { connectWifi() }
    }

    private fun granted(permission: String) =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    private fun setBusy(busy: Boolean, status: String = "") {
        bluetoothButton.isEnabled = !busy
        wifiButton.isEnabled = !busy
        progress.visibility = if (busy) View.VISIBLE else View.GONE
        statusText.text = status
    }

    private fun showStatus(status: String) = setBusy(false, status)

    private fun connectBluetooth() {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null || !adapter.isEnabled) {
            showStatus(getString(R.string.status_bt_off))
            return
        }
        setBusy(true, getString(R.string.status_scanning))
        lifecycleScope.launch {
            try {
                val device = BleTransport.scan(this@MainActivity)
                if (device == null) {
                    showStatus(getString(R.string.status_not_found))
                    return@launch
                }
                setBusy(true, getString(R.string.status_connecting))
                open(BleTransport.connect(this@MainActivity, device))
            } catch (e: Exception) {
                showStatus(getString(R.string.status_failed, e.message ?: e.javaClass.simpleName))
            }
        }
    }

    private fun connectWifi() {
        val host = addressInput.text.toString().trim()
            .removePrefix("http://").removeSuffix("/")
        if (host.isEmpty()) return
        getPreferences(MODE_PRIVATE).edit().putString(KEY_ADDRESS, host).apply()
        setBusy(true, getString(R.string.status_connecting))
        lifecycleScope.launch {
            try {
                open(WifiTransport.connect(host))
            } catch (e: Exception) {
                showStatus(getString(R.string.status_failed, e.message ?: e.javaClass.simpleName))
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
    }
}
