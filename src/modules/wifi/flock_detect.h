#ifndef FLOCK_DETECT_H
#define FLOCK_DETECT_H

#if !defined(LITE_VERSION)

// FlockDetect - passive Flock Safety / surveillance-device detector.
//
// Identifies Flock Safety ALPR gear and related surveillance devices from
// passively captured 802.11 management frames, using four detection vectors
// (wildcard-probe IE signature, SSID text patterns, hidden-SSID flagging, and
// OUI tables) presented through a tiered CONFIRMED/SUSPICIOUS/INFO UI.
//
// Ported from three upstream projects (see THIRD_PARTY.md for full credits):
//   - flock-you           (MIT, colonelpanichacks)  - the IE-signature builder
//   - flock-you-wifi-recon (MIT, JakeSwiz)           - SSID/hidden/OUI recon
//   - FlockSquawk          (GPL-3.0, f1yaw4y)        - tiered-alert UI pattern
void flock_detect_setup();

// BLE variant: detects Raven acoustic sensors (service UUIDs), Flock-family BLE
// device names, and the XUNTONG manufacturer ID. Shares the tiered UI/tracker.
void flock_detect_ble_setup();

#endif // !LITE_VERSION
#endif // FLOCK_DETECT_H
