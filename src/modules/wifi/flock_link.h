#ifndef FLOCK_LINK_H
#define FLOCK_LINK_H

#if !defined(LITE_VERSION)

#include <Arduino.h>

// ============================================================================
// FlockLink - pushes FlockDetect detections to a paired phone over BLE.
// ============================================================================
// The phone owns GPS, the map and the review queue; the device only detects and
// reports. A phone's GNSS receiver beats anything we could solder on, and a
// human verifying a marker on a map beats a coordinate read off a 170px screen.
//
// Wire protocol is deliberately compatible with CYD-Flock-You's "DeFlock pairing
// protocol" (docs/deflock-pairing-protocol.md in that project): Nordic UART
// Service, newline-delimited JSON out, plain command lines in. Protocols are not
// copyrightable and NUS is a public Nordic standard; no code was taken.
//
// Note we advertise as "FlockDetect" rather than impersonating their device
// name, so their app's `device == "CYD-Flock-You"` pairing check will not match
// without a change on their side. Ours accepts both.
// ============================================================================

// Start/stop the GATT server. Safe to call repeatedly.
bool flock_link_start();
void flock_link_stop();
bool flock_link_active();
bool flock_link_connected();

// Mirror the scan counters in for the pair_status heartbeat.
void flock_link_set_status(
    uint8_t channel, uint32_t rxAll, uint32_t rxMgmt, uint32_t drops, uint32_t confirmed,
    uint32_t suspicious
);

// Pump: drains phone commands and emits the periodic pair_status heartbeat.
// Call from the scan loop.
void flock_link_tick();

// Emit one detection. Fields mirror FdDevice; ieSig may be empty.
void flock_link_send_detection(
    const uint8_t *mac, uint8_t tier, const char *method, const char *ssid, int8_t rssi,
    int8_t peakRssi, uint8_t channel, uint16_t seq, uint32_t hits, const char *ieSig, bool isBle
);

// Freshest phone fix, or false when there is none inside the freshness window.
// Lets the on-device CSV carry real coordinates without a GPS module.
bool flock_link_gps(double &lat, double &lon, float &accuracy, uint32_t &ageMs);

#endif // !LITE_VERSION
#endif // FLOCK_LINK_H
