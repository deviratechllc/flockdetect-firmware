package dev.deviratech.flockdetect

import org.junit.Assert.*
import org.junit.Test

class ProtocolTest {

    // A real line as the firmware emits it (flock_link.cpp), GPS block included.
    private val detectionLine = """
        {"event":"detection","detection_method":"ie_sig","protocol":"wifi_2_4ghz",
        "mac_address":"70:c9:4e:aa:bb:cc","oui":"70:c9:4e","device_name":"","rssi":-63,
        "channel":6,"frequency":2437,"ssid":"","tier":"CONFIRMED","seq":1234,
        "peak_rssi":-58,"hits":7,"ie_sig":"2,12,127",
        "gps":{"latitude":45.171234,"longitude":-93.225678,"accuracy":6.5,"age_ms":250,"source":"phone"}}
    """.trimIndent().replace("\n", "")

    @Test
    fun parsesDetection() {
        val d = Protocol.parse(detectionLine) as Detection
        assertEquals("70:c9:4e:aa:bb:cc", d.mac)
        assertEquals("ie_sig", d.method)
        assertEquals(-63, d.rssi)
        assertEquals(6, d.channel)
        assertEquals("CONFIRMED", d.tier)
        assertEquals(1234, d.seq)
        assertEquals(-58, d.peakRssi)
        assertEquals(45.171234, d.lat!!, 1e-6)
        assertEquals(-93.225678, d.lon!!, 1e-6)
        assertTrue(d.hasPosition())
    }

    /** A CYD board sends none of our extra keys; parsing must still succeed. */
    @Test
    fun parsesCydDetectionWithoutOurExtensions() {
        val line = """{"event":"detection","detection_method":"wifi_wildcard_probe",""" +
            """"protocol":"wifi_2_4ghz","mac_address":"70:c9:4e:11:22:33","oui":"70:c9:4e",""" +
            """"device_name":"","rssi":-70,"channel":1,"frequency":2412,"ssid":""}"""
        val d = Protocol.parse(line) as Detection
        assertEquals("70:c9:4e:11:22:33", d.mac)
        assertEquals("SUSPICIOUS", d.tier)  // default when absent
        assertEquals(-1, d.seq)             // sentinel when absent
        assertEquals(-70, d.peakRssi)       // falls back to rssi
        assertFalse(d.hasPosition())        // no gps block
    }

    @Test
    fun parsesPairStatusAndAcceptsBothDevices() {
        val line = """{"event":"pair_status","device":"FlockDetect","protocol_version":1,""" +
            """"features":["wifi_promisc","phone_gps"],"gps":true,"sd":false,"detections":3,""" +
            """"channel":6,"rx_frames":1000,"rx_mgmt":900,"queue_drops":0}"""
        val s = Protocol.parse(line) as PairStatus
        assertEquals("FlockDetect", s.device)
        assertEquals(1000L, s.rxFrames)
        assertEquals(2, s.features.size)
        assertTrue(Protocol.isPairable(s))
        assertTrue(Protocol.isPairable(s.copy(device = "CYD-Flock-You")))
        assertFalse(Protocol.isPairable(s.copy(device = "SomeRandomBeacon")))
        assertFalse(Protocol.isPairable(s.copy(protocolVersion = 2)))
    }

    /**
     * The regression that matters: a GATT notification is capped at MTU-3, so a
     * detection with a GPS block routinely straddles two of them.
     */
    @Test
    fun reassemblesObjectSplitAcrossNotifications() {
        val a = Protocol.LineAssembler()
        val mid = detectionLine.length / 2
        assertTrue(a.offer(detectionLine.substring(0, mid)).isEmpty())
        val lines = a.offer(detectionLine.substring(mid) + "\n")
        assertEquals(1, lines.size)
        assertTrue(Protocol.parse(lines[0]) is Detection)
    }

    @Test
    fun splitsMultipleLinesInOneChunkAndIgnoresDebugText() {
        val a = Protocol.LineAssembler()
        val lines = a.offer("boot: wifi ready\n$detectionLine\nch6 hopping\n")
        assertEquals(3, lines.size)
        val parsed = lines.mapNotNull { Protocol.parse(it) }
        assertEquals(1, parsed.size)          // the two debug lines are dropped
        assertTrue(parsed[0] is Detection)
    }

    @Test
    fun assemblerDoesNotGrowWithoutBoundOnANewlinelessDevice() {
        val a = Protocol.LineAssembler(limit = 64)
        repeat(100) { assertTrue(a.offer("x".repeat(32)).isEmpty()) }
        val lines = a.offer("\n")
        assertEquals(1, lines.size)
        assertTrue(lines[0].length <= 64)
    }

    @Test
    fun gpsLineIsCommaSafeAndMatchesTheExtendedForm() {
        val s = Protocol.gpsLine(45.171234, -93.225678, 6.5f, 38.2f, 184.0f, 12, 0.9f, 1760000000L, -300)
        assertEquals("FYGPS,45.171234,-93.225678,6.5,38.2,184.0,12,0.9,1760000000,-300", s)
        assertEquals(10, s.split(",").size)  // a comma decimal separator would break this
    }

    @Test
    fun rejectsNonJsonAndMalformedLines() {
        assertNull(Protocol.parse("ch6 scanning"))
        assertNull(Protocol.parse(""))
        assertNull(Protocol.parse("{ not json at all"))
        assertNull(Protocol.parse("""{"event":"something_else"}"""))
    }
}
