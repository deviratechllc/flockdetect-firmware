package dev.deviratech.flockdetect

import org.json.JSONObject

/**
 * Wire protocol for the FlockDetect BLE link.
 *
 * Compatible with CYD-Flock-You's DeFlock pairing protocol: Nordic UART Service,
 * newline-delimited JSON from the device, plain command lines to it.
 *
 * Deliberately free of Android dependencies (bar org.json, which is stubbed on the
 * JVM by the unit-test config) so the framing and parsing can be tested without a
 * device — that is the only part of this app that can be verified off-hardware.
 */
object Protocol {

    /** Nordic UART Service. The device advertises this; the app scans for it. */
    const val SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
    const val RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // app writes here
    const val TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // device notifies here

    /**
     * Device names we will pair with. Ours plus CYD's, so this app also works with
     * a CYD-Flock-You board.
     */
    val KNOWN_DEVICES = setOf("FlockDetect", "CYD-Flock-You")

    const val CMD_HELLO = "FYHELLO"
    const val CMD_SIM = "FYSIM"

    /**
     * Reassembles newline-delimited lines from BLE notifications.
     *
     * A GATT notification is capped at MTU-3 bytes, so a detection JSON object
     * routinely arrives split across two or more of them. Anything that parses a
     * notification payload as a whole message will silently drop long detections —
     * which is exactly the interesting ones, since they carry the GPS block.
     */
    class LineAssembler(private val limit: Int = 8192) {
        private val buf = StringBuilder()

        fun offer(chunk: String): List<String> {
            buf.append(chunk)
            // A device that never sends a newline must not grow this without bound.
            if (buf.length > limit) buf.delete(0, buf.length - limit)

            val out = ArrayList<String>()
            while (true) {
                val nl = buf.indexOf("\n")
                if (nl < 0) break
                val line = buf.substring(0, nl).trim()
                buf.delete(0, nl + 1)
                if (line.isNotEmpty()) out.add(line)
            }
            return out
        }

        fun reset() = buf.setLength(0)
    }

    /**
     * The device interleaves human-readable debug lines with JSON. The protocol
     * says to ignore anything that isn't JSON unless showing a raw console.
     */
    fun parse(line: String): Message? {
        val t = line.trim()
        if (!t.startsWith("{") || !t.endsWith("}")) return null
        val o = try {
            JSONObject(t)
        } catch (_: Exception) {
            return null
        }
        return when (o.optString("event")) {
            "detection" -> parseDetection(o)
            "pair_status" -> parsePairStatus(o)
            else -> null
        }
    }

    private fun parseDetection(o: JSONObject): Detection {
        val g = o.optJSONObject("gps")
        return Detection(
            method = o.optString("detection_method"),
            protocol = o.optString("protocol"),
            mac = o.optString("mac_address"),
            oui = o.optString("oui"),
            deviceName = o.optString("device_name"),
            rssi = o.optInt("rssi"),
            channel = o.optInt("channel"),
            frequency = o.optInt("frequency"),
            ssid = o.optString("ssid"),
            // Our extensions. A CYD board omits these, hence the defaults.
            tier = o.optString("tier", "SUSPICIOUS"),
            seq = o.optInt("seq", -1),
            peakRssi = o.optInt("peak_rssi", o.optInt("rssi")),
            hits = o.optInt("hits", 1),
            ieSig = o.optString("ie_sig"),
            lat = g?.optDouble("latitude"),
            lon = g?.optDouble("longitude"),
            accuracy = g?.optDouble("accuracy")?.toFloat(),
            gpsAgeMs = g?.optLong("age_ms") ?: 0L
        )
    }

    private fun parsePairStatus(o: JSONObject): PairStatus {
        val feats = o.optJSONArray("features")
        val list = ArrayList<String>()
        if (feats != null) for (i in 0 until feats.length()) list.add(feats.optString(i))
        return PairStatus(
            device = o.optString("device"),
            protocolVersion = o.optInt("protocol_version", 0),
            features = list,
            hasGps = o.optBoolean("gps"),
            hasSd = o.optBoolean("sd"),
            detections = o.optInt("detections"),
            channel = o.optInt("channel"),
            rxFrames = o.optLong("rx_frames"),
            rxMgmt = o.optLong("rx_mgmt"),
            queueDrops = o.optLong("queue_drops")
        )
    }

    /** True when this status is from a device we understand. */
    fun isPairable(s: PairStatus): Boolean =
        s.device in KNOWN_DEVICES && s.protocolVersion == 1

    /**
     * `FYGPS,<lat>,<lon>,<accuracy_m>,<speed_kmph>,<course_deg>,<sats>,<hdop>,<unix>,<utc_offset_min>`
     *
     * The extended form. The device accepts the short one too, but sending the
     * extra fields lets it show local time. Locale.ROOT so a comma decimal
     * separator can never corrupt a comma-delimited line.
     */
    fun gpsLine(
        lat: Double, lon: Double, accuracyM: Float, speedKmph: Float, courseDeg: Float,
        sats: Int, hdop: Float, unixTime: Long, utcOffsetMin: Int
    ): String = String.format(
        java.util.Locale.ROOT,
        "FYGPS,%.6f,%.6f,%.1f,%.1f,%.1f,%d,%.1f,%d,%d",
        lat, lon, accuracyM, speedKmph, courseDeg, sats, hdop, unixTime, utcOffsetMin
    )
}

data class Detection(
    val method: String,
    val protocol: String,
    val mac: String,
    val oui: String,
    val deviceName: String,
    val rssi: Int,
    val channel: Int,
    val frequency: Int,
    val ssid: String,
    val tier: String,
    val seq: Int,
    val peakRssi: Int,
    val hits: Int,
    val ieSig: String,
    val lat: Double?,
    val lon: Double?,
    val accuracy: Float?,
    val gpsAgeMs: Long
) : Message {
    /** Without a fix there is nothing to put on a map. */
    fun hasPosition(): Boolean =
        lat != null && lon != null && !(lat == 0.0 && lon == 0.0) && !lat.isNaN() && !lon.isNaN()
}

data class PairStatus(
    val device: String,
    val protocolVersion: Int,
    val features: List<String>,
    val hasGps: Boolean,
    val hasSd: Boolean,
    val detections: Int,
    val channel: Int,
    val rxFrames: Long,
    val rxMgmt: Long,
    val queueDrops: Long
) : Message

sealed interface Message
