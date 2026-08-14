package dev.deviratech.flockdetect

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.*

/**
 * Turns approved candidates into files for DeFlock / JOSM / OSM.
 *
 * Export rather than in-app upload, deliberately: an OAuth flow plus changeset
 * writes from a phone in a moving car is a good way to put bad geometry into a
 * public dataset. A file the operator inspects first is not.
 *
 * Only APPROVED candidates are ever written — pending ones are unverified by
 * definition and rejected ones were judged wrong.
 */
object ExportManager {

    fun exportGeoJson(ctx: Context, items: List<Candidate>): File {
        val features = JSONArray()
        items.forEach { c ->
            val props = JSONObject().apply {
                // OSM surveillance tagging, so the file drops into JOSM ready to
                // review rather than needing every tag typed by hand.
                put("man_made", "surveillance")
                put("surveillance", "public")
                put("surveillance:type", "ALPR")
                put("surveillance:zone", "traffic")
                put("camera:mount", "pole")
                c.bearingDeg?.let { put("camera:direction", Math.round(it)) }
                put("operator", "Flock Safety")
                // Provenance, so a reviewer can tell where this came from.
                put("fd:mac", c.mac)
                put("fd:oui", c.oui)
                put("fd:method", c.method)
                put("fd:tier", c.tier)
                put("fd:peak_rssi", c.peakRssi)
                put("fd:channel", c.channel)
                if (c.note.isNotEmpty()) put("fd:note", c.note)
            }
            features.put(JSONObject().apply {
                put("type", "Feature")
                put("geometry", JSONObject().apply {
                    put("type", "Point")
                    // GeoJSON is lon,lat — the reverse of how everything else here
                    // orders it, and a classic way to land points in the ocean.
                    put("coordinates", JSONArray().put(c.lon).put(c.lat))
                })
                put("properties", props)
            })
        }
        val root = JSONObject().apply {
            put("type", "FeatureCollection")
            put("features", features)
        }
        return write(ctx, "flockdetect-${stamp()}.geojson", root.toString(2))
    }

    fun exportCsv(ctx: Context, items: List<Candidate>): File {
        val sb = StringBuilder("lat,lon,bearing_deg,mac,oui,method,tier,peak_rssi,channel,ssid,first_seen,note\n")
        items.forEach { c ->
            sb.append(
                String.format(
                    Locale.ROOT, "%.6f,%.6f,%s,%s,%s,%s,%s,%d,%d,\"%s\",%s,\"%s\"\n",
                    c.lat, c.lon, c.bearingDeg?.let { Math.round(it).toString() } ?: "",
                    c.mac, c.oui, c.method, c.tier, c.peakRssi, c.channel,
                    c.ssid.replace("\"", "'"), iso(c.firstSeenMs), c.note.replace("\"", "'")
                )
            )
        }
        return write(ctx, "flockdetect-${stamp()}.csv", sb.toString())
    }

    fun exportGpx(ctx: Context, items: List<Candidate>): File {
        val sb = StringBuilder()
        sb.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        sb.append("<gpx version=\"1.1\" creator=\"FlockDetect\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n")
        items.forEach { c ->
            sb.append(String.format(Locale.ROOT, "  <wpt lat=\"%.6f\" lon=\"%.6f\">\n", c.lat, c.lon))
            sb.append("    <name>${xml(c.mac)}</name>\n")
            sb.append("    <desc>${xml("${c.method} ${c.tier} ${c.peakRssi}dBm ch${c.channel}")}</desc>\n")
            sb.append("    <sym>Flag</sym>\n  </wpt>\n")
        }
        sb.append("</gpx>\n")
        return write(ctx, "flockdetect-${stamp()}.gpx", sb.toString())
    }

    /** Hands the file to any app that can take it, via the FileProvider. */
    fun share(ctx: Context, file: File, mime: String) {
        val uri = FileProvider.getUriForFile(ctx, "${ctx.packageName}.fileprovider", file)
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = mime
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        ctx.startActivity(Intent.createChooser(intent, "Export candidates"))
    }

    private fun write(ctx: Context, name: String, body: String): File {
        val dir = File(ctx.filesDir, "exports").apply { mkdirs() }
        return File(dir, name).apply { writeText(body) }
    }

    private fun stamp(): String =
        SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date())

    private fun iso(ms: Long): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", Locale.ROOT).format(Date(ms))

    private fun xml(s: String): String = s
        .replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        .replace("\"", "&quot;")
}
