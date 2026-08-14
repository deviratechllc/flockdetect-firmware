package dev.deviratech.flockdetect

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * A detection the operator has not yet verified.
 *
 * `lat`/`lon` start at the phone's position when the detection arrived — which is
 * where *you* were, on the road, not where the camera is. Review exists to move it.
 */
data class Candidate(
    val id: String,
    val mac: String,
    val oui: String,
    val method: String,
    val tier: String,
    val rssi: Int,
    val peakRssi: Int,
    val channel: Int,
    val ssid: String,
    val firstSeenMs: Long,
    var lat: Double,
    var lon: Double,
    var accuracy: Float,
    /** Where the camera looks. Set by hand; never inferred from travel. */
    var bearingDeg: Float? = null,
    var status: String = PENDING,
    var note: String = ""
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("id", id); put("mac", mac); put("oui", oui); put("method", method)
        put("tier", tier); put("rssi", rssi); put("peak_rssi", peakRssi)
        put("channel", channel); put("ssid", ssid); put("first_seen_ms", firstSeenMs)
        put("lat", lat); put("lon", lon); put("accuracy", accuracy.toDouble())
        bearingDeg?.let { put("bearing_deg", it.toDouble()) }
        put("status", status); put("note", note)
    }

    companion object {
        const val PENDING = "pending"
        const val APPROVED = "approved"
        const val REJECTED = "rejected"

        fun fromJson(o: JSONObject) = Candidate(
            id = o.optString("id"),
            mac = o.optString("mac"),
            oui = o.optString("oui"),
            method = o.optString("method"),
            tier = o.optString("tier"),
            rssi = o.optInt("rssi"),
            peakRssi = o.optInt("peak_rssi"),
            channel = o.optInt("channel"),
            ssid = o.optString("ssid"),
            firstSeenMs = o.optLong("first_seen_ms"),
            lat = o.optDouble("lat"),
            lon = o.optDouble("lon"),
            accuracy = o.optDouble("accuracy").toFloat(),
            bearingDeg = if (o.has("bearing_deg")) o.optDouble("bearing_deg").toFloat() else null,
            status = o.optString("status", PENDING),
            note = o.optString("note")
        )
    }
}

/**
 * Review queue, persisted as a JSON file.
 *
 * A plain file rather than Room: no codegen, no schema migrations, and the whole
 * thing stays unit-testable on the JVM.
 */
class CandidateStore(private val file: File) {

    private val items = LinkedHashMap<String, Candidate>()

    /** CYD's protocol recommends 250 ft; this is that in metres. */
    val dedupeRadiusM = 76.2

    init {
        load()
    }

    fun all(): List<Candidate> = items.values.toList()
    fun pending(): List<Candidate> = items.values.filter { it.status == Candidate.PENDING }
    fun approved(): List<Candidate> = items.values.filter { it.status == Candidate.APPROVED }

    /**
     * Adds a detection to the queue.
     *
     * Returns null when it is a duplicate. Two rules, and they are different:
     * the same MAC is always the same unit, however far the fix has drifted; a
     * different MAC within [dedupeRadiusM] is treated as the same physical
     * installation, which is what stops one camera becoming five map pins as you
     * drive past it — and what MAC randomisation would otherwise cause.
     */
    fun addOrMerge(d: Detection, nowMs: Long): Candidate? {
        if (!d.hasPosition()) return null

        // Keyed by candidate id, so the same-unit lookup has to search by MAC.
        items.values.firstOrNull { it.mac == d.mac }?.let { existing ->
            if (d.peakRssi > existing.peakRssi) {
                // A stronger sighting is physically nearer the camera, so it is
                // the better position estimate.
                existing.lat = d.lat!!
                existing.lon = d.lon!!
                existing.accuracy = d.accuracy ?: existing.accuracy
            }
            save()
            return null
        }

        val near = items.values.firstOrNull {
            it.status != Candidate.REJECTED &&
                distanceM(it.lat, it.lon, d.lat!!, d.lon!!) < dedupeRadiusM
        }
        if (near != null) return null

        val c = Candidate(
            id = "${d.mac}-$nowMs",
            mac = d.mac, oui = d.oui, method = d.method, tier = d.tier,
            rssi = d.rssi, peakRssi = d.peakRssi, channel = d.channel, ssid = d.ssid,
            firstSeenMs = nowMs,
            lat = d.lat!!, lon = d.lon!!, accuracy = d.accuracy ?: 0f
        )
        items[c.id] = c
        save()
        return c
    }

    fun update(c: Candidate) {
        items[c.id] = c
        save()
    }

    fun remove(id: String) {
        items.remove(id)
        save()
    }

    private fun load() {
        if (!file.exists()) return
        runCatching {
            val arr = JSONArray(file.readText())
            for (i in 0 until arr.length()) {
                val c = Candidate.fromJson(arr.getJSONObject(i))
                if (c.id.isNotEmpty()) items[c.id] = c
            }
        }
    }

    private fun save() {
        runCatching {
            val arr = JSONArray()
            items.values.forEach { arr.put(it.toJson()) }
            file.parentFile?.mkdirs()
            file.writeText(arr.toString())
        }
    }

    companion object {
        /** Haversine. Fine at these distances and needs no projection. */
        fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
            val r = 6_371_000.0
            val p1 = Math.toRadians(lat1)
            val p2 = Math.toRadians(lat2)
            val dp = Math.toRadians(lat2 - lat1)
            val dl = Math.toRadians(lon2 - lon1)
            val a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2)
            return r * 2 * atan2(sqrt(a), sqrt(1 - a))
        }
    }
}
