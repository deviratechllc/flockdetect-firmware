package dev.deviratech.flockdetect

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Bundle
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.osmdroid.config.Configuration
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import java.io.File

/**
 * Map, review queue and export.
 *
 * The review step is the point of the whole app: a detection arrives stamped with
 * where the *phone* was, which is on the road. A human drags it to the camera and
 * sets which way it looks before anything leaves the device.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var map: MapView
    private lateinit var status: TextView
    private lateinit var store: CandidateStore
    private val markers = HashMap<String, Marker>()

    private val permissions = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { startLink() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // osmdroid needs a user agent set before any tile fetch, or OSM's tile
        // servers return 403 and the map silently stays blank.
        Configuration.getInstance().load(this, getSharedPreferences("osmdroid", MODE_PRIVATE))
        Configuration.getInstance().userAgentValue = packageName

        setContentView(R.layout.activity_main)
        store = CandidateStore(File(filesDir, "candidates.json"))

        map = findViewById(R.id.map)
        map.setTileSource(TileSourceFactory.MAPNIK)
        map.setMultiTouchControls(true)
        map.controller.setZoom(16.0)

        status = findViewById(R.id.status)
        findViewById<Button>(R.id.connectBtn).setOnClickListener { requestAndStart() }
        findViewById<Button>(R.id.simBtn).setOnClickListener {
            startService(Intent(this, BleLinkService::class.java).setAction(BleLinkService.CMD_SIM))
            toast("Sent FYSIM — a synthetic detection should arrive")
        }
        findViewById<Button>(R.id.exportBtn).setOnClickListener { exportMenu() }

        redrawAll()
    }

    override fun onResume() {
        super.onResume()
        map.onResume()
        val f = IntentFilter().apply {
            addAction(BleLinkService.ACTION_STATE)
            addAction(BleLinkService.ACTION_DETECTION)
            addAction(BleLinkService.ACTION_STATUS)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(receiver, f)
        }
    }

    override fun onPause() {
        super.onPause()
        map.onPause()
        runCatching { unregisterReceiver(receiver) }
    }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, i: Intent?) {
            when (i?.action) {
                BleLinkService.ACTION_STATE ->
                    status.text = i.getStringExtra(BleLinkService.EXTRA_STATE) ?: ""
                BleLinkService.ACTION_STATUS -> {
                    val s = Protocol.parse(i.getStringExtra(BleLinkService.EXTRA_JSON) ?: "")
                    if (s is PairStatus) {
                        status.text = "${s.device} ch${s.channel} · rx ${s.rxFrames} · drops ${s.queueDrops}"
                    }
                }
                BleLinkService.ACTION_DETECTION -> {
                    val d = Protocol.parse(i.getStringExtra(BleLinkService.EXTRA_JSON) ?: "")
                    if (d is Detection) onDetection(d)
                }
            }
        }
    }

    private fun onDetection(d: Detection) {
        if (!d.hasPosition()) {
            // No fix yet means no map position. Say so rather than dropping it
            // silently, since the usual cause is GPS not having locked on.
            toast("Detection ${d.mac} ignored — no GPS fix yet")
            return
        }
        val c = store.addOrMerge(d, System.currentTimeMillis())
        if (c == null) {
            redrawAll()
            return
        }
        addMarker(c)
        map.controller.animateTo(GeoPoint(c.lat, c.lon))
        toast("${c.tier}: ${c.mac} — tap to review")
    }

    // --- map ----------------------------------------------------------------

    private fun redrawAll() {
        map.overlays.clear()
        markers.clear()
        store.all().filter { it.status != Candidate.REJECTED }.forEach { addMarker(it) }
        store.all().firstOrNull()?.let { map.controller.setCenter(GeoPoint(it.lat, it.lon)) }
        map.invalidate()
    }

    private fun addMarker(c: Candidate) {
        val m = Marker(map)
        m.position = GeoPoint(c.lat, c.lon)
        m.setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
        m.title = "${c.mac} · ${c.tier}"
        m.snippet = "${c.method} ${c.peakRssi}dBm ch${c.channel}"
        m.isDraggable = true
        m.setOnMarkerClickListener { _, _ -> reviewDialog(c); true }
        m.setOnMarkerDragListener(object : Marker.OnMarkerDragListener {
            override fun onMarkerDrag(marker: Marker) {}
            override fun onMarkerDragEnd(marker: Marker) {
                // The drag is the verification: this is the operator saying the
                // camera is *here*, not where the phone happened to be.
                c.lat = marker.position.latitude
                c.lon = marker.position.longitude
                store.update(c)
                toast("Position updated")
            }
            override fun onMarkerDragStart(marker: Marker) {}
        })
        markers[c.id] = m
        map.overlays.add(m)
        map.invalidate()
    }

    // --- review -------------------------------------------------------------

    private fun reviewDialog(c: Candidate) {
        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(40, 20, 40, 0)
        }
        val info = TextView(this).apply {
            text = buildString {
                appendLine("MAC: ${c.mac}")
                appendLine("OUI: ${c.oui}")
                appendLine("Method: ${c.method}   Tier: ${c.tier}")
                appendLine("Peak: ${c.peakRssi} dBm   ch${c.channel}")
                if (c.ssid.isNotEmpty()) appendLine("SSID: ${c.ssid}")
                appendLine()
                appendLine("Drag the marker onto the camera before approving.")
            }
            textSize = 13f
        }
        val bearingLabel = TextView(this).apply {
            text = "Camera direction: ${c.bearingDeg?.let { "${it.toInt()}°" } ?: "not set"}"
            textSize = 13f
        }
        // Direction is set by hand. Inferring it from travel heading would encode
        // which way *you* drove, not which way the camera looks.
        val bearing = SeekBar(this).apply {
            max = 359
            progress = c.bearingDeg?.toInt() ?: 0
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, v: Int, fromUser: Boolean) {
                    bearingLabel.text = "Camera direction: $v°"
                }
                override fun onStartTrackingTouch(sb: SeekBar?) {}
                override fun onStopTrackingTouch(sb: SeekBar?) {}
            })
        }
        val note = EditText(this).apply {
            hint = "Note (optional)"
            setText(c.note)
        }
        body.addView(info); body.addView(bearingLabel); body.addView(bearing); body.addView(note)

        AlertDialog.Builder(this)
            .setTitle("Review candidate")
            .setView(body)
            .setPositiveButton("Approve") { _, _ ->
                c.bearingDeg = bearing.progress.toFloat()
                c.note = note.text.toString()
                c.status = Candidate.APPROVED
                store.update(c)
                toast("Approved — include it in the next export")
            }
            .setNegativeButton("Reject") { _, _ ->
                c.status = Candidate.REJECTED
                store.update(c)
                redrawAll()
            }
            .setNeutralButton("Cancel", null)
            .show()
    }

    // --- export -------------------------------------------------------------

    private fun exportMenu() {
        val approved = store.approved()
        if (approved.isEmpty()) {
            toast("Nothing approved yet — review a candidate first")
            return
        }
        val formats = arrayOf("GeoJSON (JOSM)", "CSV", "GPX")
        AlertDialog.Builder(this)
            .setTitle("Export ${approved.size} approved")
            .setItems(formats) { _, which ->
                val (file, mime) = when (which) {
                    0 -> ExportManager.exportGeoJson(this, approved) to "application/geo+json"
                    1 -> ExportManager.exportCsv(this, approved) to "text/csv"
                    else -> ExportManager.exportGpx(this, approved) to "application/gpx+xml"
                }
                ExportManager.share(this, file, mime)
            }
            .show()
    }

    // --- permissions --------------------------------------------------------

    private fun requestAndStart() {
        val needed = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            needed += Manifest.permission.BLUETOOTH_SCAN
            needed += Manifest.permission.BLUETOOTH_CONNECT
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            needed += Manifest.permission.POST_NOTIFICATIONS
        }
        permissions.launch(needed.toTypedArray())
    }

    private fun startLink() {
        val i = Intent(this, BleLinkService::class.java).setAction(BleLinkService.CMD_START)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(i)
        else startService(i)
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
}
