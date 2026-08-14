package dev.deviratech.flockdetect

import android.Manifest
import android.annotation.SuppressLint
import android.app.*
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.*
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationCompat
import java.util.*

/**
 * Holds the BLE link to the device and streams the phone's GPS to it.
 *
 * A foreground service with `location` type because the useful case is the phone
 * in a pocket with the screen off while driving — a bound-to-Activity connection
 * would be killed exactly when it matters.
 */
class BleLinkService : Service() {

    companion object {
        const val ACTION_STATE = "dev.deviratech.flockdetect.STATE"
        const val ACTION_DETECTION = "dev.deviratech.flockdetect.DETECTION"
        const val ACTION_STATUS = "dev.deviratech.flockdetect.STATUS"
        const val EXTRA_STATE = "state"
        const val EXTRA_JSON = "json"

        const val CMD_START = "start"
        const val CMD_STOP = "stop"
        const val CMD_SIM = "sim"

        private const val CHANNEL_ID = "flocklink"
        private const val NOTIF_ID = 1
        private const val GPS_INTERVAL_MS = 1000L
        private const val SCAN_TIMEOUT_MS = 20000L
    }

    enum class State { IDLE, SCANNING, CONNECTING, CONNECTED }

    private val binder = LocalBinder()
    inner class LocalBinder : Binder() {
        val service: BleLinkService get() = this@BleLinkService
    }

    private var gatt: BluetoothGatt? = null
    private var txChar: BluetoothGattCharacteristic? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private val assembler = Protocol.LineAssembler()
    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE
    private var lastLocation: Location? = null

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            CMD_START -> {
                startForeground(NOTIF_ID, buildNotification("Looking for FlockDetect…"))
                startScan()
                startGpsStream()
            }
            CMD_SIM -> send(Protocol.CMD_SIM)
            CMD_STOP -> {
                teardown()
                stopSelf()
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        teardown()
        super.onDestroy()
    }

    // --- scanning -----------------------------------------------------------

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            stopScan()
            connect(result.device)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        if (!hasBlePermission()) {
            setState(State.IDLE)
            return
        }
        val scanner = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager)
            .adapter?.bluetoothLeScanner ?: return
        setState(State.SCANNING)

        // Filter on the service UUID rather than the name: the name lives in the
        // scan response, and filtering on it would miss devices whose scan
        // response has not been received yet.
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(UUID.fromString(Protocol.SERVICE_UUID)))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(listOf(filter), settings, scanCallback)
        main.postDelayed({
            if (state == State.SCANNING) {
                stopScan()
                setState(State.IDLE)
            }
        }, SCAN_TIMEOUT_MS)
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        if (!hasBlePermission()) return
        runCatching {
            (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager)
                .adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        }
    }

    // --- connection ---------------------------------------------------------

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        if (!hasBlePermission()) return
        setState(State.CONNECTING)
        assembler.reset()
        gatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                // Ask for a bigger MTU before discovery: the default 23 bytes
                // splits every detection across several notifications.
                g.requestMtu(247)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                setState(State.IDLE)
                runCatching { g.close() }
                gatt = null
                // The device drops the link whenever the operator leaves the
                // module; retry so it reconnects on its own.
                main.postDelayed({ if (state == State.IDLE) startScan() }, 3000)
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            g.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(UUID.fromString(Protocol.SERVICE_UUID)) ?: run {
                g.disconnect(); return
            }
            txChar = svc.getCharacteristic(UUID.fromString(Protocol.TX_UUID))
            rxChar = svc.getCharacteristic(UUID.fromString(Protocol.RX_UUID))
            val tx = txChar ?: run { g.disconnect(); return }

            g.setCharacteristicNotification(tx, true)
            // The CCCD write is what actually enables notifications; without it
            // setCharacteristicNotification only arms the local side.
            tx.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"))?.let { d ->
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(d)
                }
            }
            setState(State.CONNECTED)
            main.postDelayed({ send(Protocol.CMD_HELLO) }, 300)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray
        ) = onPayload(value)

        @Deprecated("Pre-API 33 delivery path")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            onPayload(c.value ?: return)
        }
    }

    private fun onPayload(value: ByteArray) {
        for (line in assembler.offer(String(value, Charsets.UTF_8))) {
            when (val msg = Protocol.parse(line)) {
                is Detection -> broadcast(ACTION_DETECTION, line)
                is PairStatus -> {
                    if (Protocol.isPairable(msg)) broadcast(ACTION_STATUS, line)
                }
                else -> { /* debug text, per the protocol */ }
            }
        }
    }

    // --- transmit -----------------------------------------------------------

    @SuppressLint("MissingPermission")
    fun send(cmd: String) {
        val g = gatt ?: return
        val c = rxChar ?: return
        if (!hasBlePermission()) return
        val bytes = (cmd + "\n").toByteArray(Charsets.UTF_8)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(c, bytes, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
        } else {
            @Suppress("DEPRECATION")
            c.value = bytes
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }
    }

    // --- GPS ----------------------------------------------------------------

    private val locListener = LocationListener { loc -> lastLocation = loc }

    @SuppressLint("MissingPermission")
    private fun startGpsStream() {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED
        ) return
        val lm = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        runCatching {
            lm.requestLocationUpdates(
                LocationManager.GPS_PROVIDER, GPS_INTERVAL_MS, 0f, locListener
            )
        }
        main.post(gpsPump)
    }

    private val gpsPump = object : Runnable {
        override fun run() {
            val loc = lastLocation
            if (state == State.CONNECTED && loc != null) {
                val offsetMin = TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60000
                send(
                    Protocol.gpsLine(
                        loc.latitude, loc.longitude, loc.accuracy,
                        loc.speed * 3.6f, loc.bearing,
                        loc.extras?.getInt("satellites") ?: 0, 0f,
                        loc.time / 1000, offsetMin
                    )
                )
            }
            main.postDelayed(this, GPS_INTERVAL_MS)
        }
    }

    // --- plumbing -----------------------------------------------------------

    @SuppressLint("MissingPermission")
    private fun teardown() {
        main.removeCallbacksAndMessages(null)
        stopScan()
        runCatching {
            (getSystemService(Context.LOCATION_SERVICE) as LocationManager)
                .removeUpdates(locListener)
        }
        runCatching { gatt?.disconnect(); gatt?.close() }
        gatt = null
        setState(State.IDLE)
    }

    private fun hasBlePermission(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_GRANTED &&
                ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED
        } else true

    private fun setState(s: State) {
        state = s
        broadcast(ACTION_STATE, s.name)
        notify(
            when (s) {
                State.IDLE -> "Disconnected"
                State.SCANNING -> "Looking for FlockDetect…"
                State.CONNECTING -> "Connecting…"
                State.CONNECTED -> "Linked — streaming GPS"
            }
        )
    }

    private fun broadcast(action: String, payload: String) {
        sendBroadcast(Intent(action).setPackage(packageName).apply {
            putExtra(if (action == ACTION_STATE) EXTRA_STATE else EXTRA_JSON, payload)
        })
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                CHANNEL_ID, "FlockDetect link", NotificationManager.IMPORTANCE_LOW
            )
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .createNotificationChannel(ch)
        }
    }

    private fun buildNotification(text: String): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("FlockDetect")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setOngoing(true)
            .setContentIntent(
                PendingIntent.getActivity(
                    this, 0, Intent(this, MainActivity::class.java),
                    PendingIntent.FLAG_IMMUTABLE
                )
            )
            .build()

    private fun notify(text: String) {
        runCatching {
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .notify(NOTIF_ID, buildNotification(text))
        }
    }
}
