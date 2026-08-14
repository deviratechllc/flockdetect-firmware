#if !defined(LITE_VERSION)
// ============================================================================
// FlockDetect - passive Flock Safety / surveillance-device detector for Bruce.
// ============================================================================
// Detection ported from upstream projects (full credits in THIRD_PARTY.md):
//
//   flock-you (MIT, Copyright (c) 2026 colonelpanichacks)
//     - the wildcard-probe Information-Element signature builder + matcher.
//   flock-you-wifi-recon (MIT, Copyright (c) 2026 colonelpanichacks; JakeSwiz)
//     - SSID text-pattern matching, hidden-SSID flagging + per-BSSID dedup,
//       and the curated Flock / SoundThinking OUI tables.
//   FlockSquawk (GPL-3.0, f1yaw4y)
//     - the tiered CONFIRMED/SUSPICIOUS/INFO alert model and the surveillance-
//       camera vendor OUI table (curated from the FlockOff database).
//
// SSID signatures credited to GainSec's 2025 Flock Safety research; IE-field
// probe fingerprinting per Pintor & Atzori (2022).
// ============================================================================

#include "flock_detect.h"
#include "flock_link.h"

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "core/display.h"
#include "core/led_control.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/wifi_common.h"
#include "modules/ble/ble_common.h"
#include "modules/others/audio.h"
#include <Arduino.h>
#include <TinyGPS++.h>
#include <globals.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

// Shown as a badge on the scan banner and on the diagnostic pages, so a photo
// or a bug report identifies which build it came from without guesswork.
#define FD_VERSION "v4"

// Scan channel set, chosen from the options menu. Deliberately not reset by
// fdResetSession() -- it is a user preference, not session state.
//
// The 1/6/11 preset is the non-overlapping trio (idea from flock-back's
// wardriver BAND_2_4): a full sweep takes 1.2s instead of 4.4s, which matters
// when driving past a camera that only probes intermittently.
enum FdChPreset { FD_CH_FAST = 0, FD_CH_US, FD_CH_WORLD, FD_CH_JP, FD_CH_PRESETS };

static const uint8_t FD_CH_FAST_LIST[] = {1, 6, 11};
static const uint8_t FD_CH_US_LIST[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t FD_CH_WORLD_LIST[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
static const uint8_t FD_CH_JP_LIST[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

struct FdChannelSet {
    const char *label;
    const uint8_t *list;
    uint8_t count;
};
static const FdChannelSet FD_CH_SETS[FD_CH_PRESETS] = {
    {"Fast 1/6/11", FD_CH_FAST_LIST,  sizeof(FD_CH_FAST_LIST)                    },
    {"US 1-11",     FD_CH_US_LIST,    sizeof(FD_CH_US_LIST)                      },
    {"World 1-13",  FD_CH_WORLD_LIST, sizeof(FD_CH_WORLD_LIST)                   },
    {"Japan 1-14",  FD_CH_JP_LIST,    sizeof(FD_CH_JP_LIST)                      },
};

static uint8_t fdChannelPreset = FD_CH_US; // current behaviour stays the default
static inline const uint8_t *fdChannels() { return FD_CH_SETS[fdChannelPreset].list; }
static inline int fdChannelCount() { return FD_CH_SETS[fdChannelPreset].count; }

static const uint16_t FD_DWELL_MS = 400;   // per-channel dwell (recon uses 400)
static const int8_t FD_RSSI_MIN_DEFAULT = -95;
static const uint32_t FD_ALARM_COOLDOWN_MS = 5000; // per-MAC re-alarm cooldown
static const uint32_t FD_ALERT_SHOW_MS = 4000;     // ALERT takeover duration
static const size_t FD_MAX_TRACKED = 48;

// Redraw pacing. The animation runs at ~15fps for smooth motion, but the
// device list itself is only rebuilt/re-sorted at 2Hz -- otherwise rows
// reshuffle under the cursor on every frame, which reads as "refreshing too
// fast" even when the frame rate is fine.
static const uint16_t FD_FRAME_MS = 66;  // ~15fps animation tick
static const uint16_t FD_DATA_MS = 500;  // 2Hz list rebuild / re-sort
static const uint32_t FD_FLASH_MS = 500; // new-device row highlight duration

// Tier ordering (higher = more severe).
enum FdTier { FD_NONE = 0, FD_INFO = 1, FD_SUSPICIOUS = 2, FD_CONFIRMED = 3 };

// ============================================================================
// DETECTION SIGNATURES
// ============================================================================

// --- Wildcard-probe IE signature (flock-you, MIT) --------------------------
// Canonicalized IE-order string of a Flock camera's wildcard probe request:
// tags 2,12,127, the Wi-Fi-Alliance vendor IE 50:6f:9a + payload 16 03 01 03,
// HT(45), VHT(191), and the Microsoft vendor IE 00:50:f2 + payload 08 00 00 00.
static const char FLOCK_PROBE_IE_SIG_PRIMARY[] =
    "2,12,127,221:506f9a16030103,45,191,221:0050f208000000";
static const char FLOCK_LITEON_IE_SIG_PREFIX[] = "221:506f9a16030103";

// --- Flock high-confidence OUIs (union of flock-you + recon lists) ---------
static const char *FD_OUI_FLOCK_HIGH[] = {
    // recon flock_mac_prefixes + flock-you target_ouis (deduplicated union)
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84", "f0:82:c0",
    "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9", "70:c9:4e", "3c:91:80",
    "d8:f3:bc", "80:30:49", "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d",
    "94:08:53", "e4:aa:ea", "b8:35:32", "24:b2:b9", "00:f4:8d", "d0:39:57",
    "e8:d0:fc", "e0:4f:43", "b8:1e:a4", "70:08:94", "3c:71:bf", "58:00:e3",
    "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12", "f4:6a:dd", "f8:a2:d6",
    // Flock Safety's own direct IEEE registration
    "b4:1e:52",
};

// --- Community-observed Flock OUIs (recon, medium confidence) ---------------
static const char *FD_OUI_FLOCK_COMMUNITY[] = {
    "c0:35:32", "82:6b:f2",
};

// --- Contract-manufacturer OUIs (recon flock_mfr, low confidence alone) -----
static const char *FD_OUI_FLOCK_MFR[] = {
    "e0:0a:f6",
};

// --- SoundThinking / ShotSpotter acoustic sensors (recon) ------------------
static const char *FD_OUI_SOUNDTHINKING[] = {
    "d4:11:d6",
};

// --- Surveillance-camera vendors (FlockSquawk, curated from FlockOff) -------
struct FdCameraOui {
    const char *prefix;
    const char *vendor;
};
static const FdCameraOui FD_OUI_CAMERA[] = {
    {"70:1a:d5", "Avigilon Alta"}, {"00:40:8c", "Axis"},
    {"ac:cc:8e", "Axis"},          {"b8:a4:4f", "Axis"},
    {"e8:27:25", "Axis"},          {"00:13:56", "FLIR"},
    {"00:40:7f", "FLIR"},          {"00:1b:d8", "FLIR"},
    {"00:13:e2", "GeoVision"},     {"44:b4:23", "Hanwha"},
    {"8c:1d:55", "Hanwha"},        {"e4:30:22", "Hanwha"},
    {"00:10:be", "March Networks"},{"00:12:81", "March Networks"},
    {"00:03:c5", "Mobotix"},       {"00:1c:27", "Sunell"},
};

// ============================================================================
// RUNTIME STATE
// ============================================================================

// Detected device tracked across sightings.
struct FdDevice {
    uint8_t mac[6];
    int8_t rssi;
    int8_t peakRssi;
    uint8_t channel;
    uint8_t tier;
    char method[20];
    char ssid[24];
    uint32_t hits;
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint32_t lastAlarmMs;
    uint16_t lastSeq; // 802.11 sequence number, see fdProcessFrame
    // --- presentation-only state (never affects detection) ---
    int16_t dispRssi;    // eased RSSI in 1/8 dBm, chases `rssi` for smooth bars
    uint32_t flashUntil; // row highlight expiry for a newly seen device
    uint8_t blipAngle;   // stable radar bearing derived from the MAC
};

static FdDevice fdDevices[FD_MAX_TRACKED];
static size_t fdDeviceCount = 0;

// Hidden-SSID BSSID dedup ring (recon fyWifiSeenHidden), so a hidden AP is
// flagged once per session rather than on every beacon.
#define FD_HIDDEN_DEDUP 64
static uint8_t fdHiddenBssids[FD_HIDDEN_DEDUP][6];
static uint8_t fdHiddenCount = 0;
static uint8_t fdHiddenNext = 0;

// Counters shown on the scan banner.
static volatile uint32_t fdFramesSeen = 0;
static uint32_t fdConfirmedCount = 0;
static uint32_t fdSuspiciousCount = 0;

// ----------------------------------------------------------------------------
// DIAGNOSTICS
// ----------------------------------------------------------------------------
// One counter per boundary in the capture -> ring -> parse -> classify chain,
// so a "nothing is detected" report can be narrowed to a single stage instead
// of guessed at. Surfaced by the DIAG pages (SEL -> Diagnostics).
// ----------------------------------------------------------------------------

static volatile uint32_t fdFramesAll = 0;    // every frame entering the RX callback
static volatile uint32_t fdRssiRejected = 0; // dropped by the fdRssiMin floor
static volatile uint32_t fdRingHigh = 0;     // deepest ring occupancy observed
static uint32_t fdFramesProcessed = 0;       // frames the drain loop parsed
static uint32_t fdParseFailed = 0;           // too short / TLV walk ran off the end
static uint32_t fdCntBeacon = 0;
static uint32_t fdCntProbeReq = 0;
static uint32_t fdCntProbeResp = 0;
static uint32_t fdCntWildcard = 0; // probe-requests with an empty SSID element
static uint32_t fdCandOui = 0;     // frames whose OUI hit any table
static uint32_t fdCandSsid = 0;    // frames whose SSID hit any pattern
static uint32_t fdCandIe = 0;      // wildcard probes matching the Flock IE signature
static uint32_t fdRejLocalMac = 0; // OUI lookups skipped: locally-administered MAC

// Last management frame seen, for the "is anything arriving at all" line.
static uint8_t fdLastMac[6] = {0};
static int8_t fdLastRssi = 0;
static uint8_t fdLastSubtype = 0xFF;
static uint16_t fdLastSeq = 0;
static char fdLastSsid[20] = {0};

// Distinct IE signatures computed from wildcard probe-requests. Recorded
// BEFORE the match test, so a near-miss against FLOCK_PROBE_IE_SIG_PRIMARY is
// visible rather than silently discarded.
struct FdSigLog {
    char sig[96];
    uint8_t oui[3];
    uint16_t hits;
};
#define FD_SIGLOG_SLOTS 8
static FdSigLog fdSigLog[FD_SIGLOG_SLOTS];
static uint8_t fdSigLogCount = 0;
static uint8_t fdSigLogNext = 0;

// Every management-frame source seen this session, regardless of tier. Kept
// apart from fdDevices[] so the tiered list stays clean; this is what shows a
// device's real MAC/OUI when it never reaches a tier at all.
struct FdSeen {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t ouiClass;
    uint8_t tier;
    char ssid[18];
    uint16_t hits;
    uint16_t seq;
};
#define FD_MAX_SEEN 64
static FdSeen fdSeen[FD_MAX_SEEN];
static size_t fdSeenCount = 0;

// 0 = normal list, 1..FD_DIAG_PAGES = diagnostic pages.
#define FD_DIAG_PAGES 3
static uint8_t fdDiagPage = 0;

// User-adjustable settings.
static int8_t fdRssiMin = FD_RSSI_MIN_DEFAULT;
static bool fdSoundEnabled = true;
static bool fdLedEnabled = true;
static bool fdLogEnabled = false;
static bool fdGpsEnabled = false;

// UI state.
static bool fdModeIsBle = false; // false = WiFi promiscuous, true = BLE scan
static uint8_t fdCurChannel = 1;
static int fdScrollTop = 0;      // first list row shown
static int fdSelected = 0;       // highlighted list row
static bool fdAlertActive = false;
static uint32_t fdAlertUntil = 0;
static uint32_t fdAlertStart = 0;  // for the expanding-ring animation
static bool fdAlertPainted = false; // background+text laid down for this alert
static FdDevice fdAlertDev;

// SD logging state.
static FS *fdFs = nullptr;
static String fdLogPath = "";
static String fdDiagLogPath = ""; // raw candidate-frame log for offline analysis

// GPS (external, optional).
static TinyGPSPlus fdGps;
static HardwareSerial fdGpsSerial(2);
static bool fdGpsStarted = false;

// ----------------------------------------------------------------------------
// Raw-frame ring buffer. The promiscuous callback runs on the WiFi task and
// must stay tiny (no heap / Serial / TFT), so it only copies the frame bytes
// into this ring; all parsing/detection happens in the drain loop.
// ----------------------------------------------------------------------------
#define FD_RING_SLOTS 64
#define FD_FRAME_MAX 256
struct FdRawFrame {
    uint8_t buf[FD_FRAME_MAX];
    uint16_t len;
    int8_t rssi;
    uint8_t channel;
};
static FdRawFrame fdRing[FD_RING_SLOTS];
static volatile uint8_t fdRingHead = 0; // written by callback
static volatile uint8_t fdRingTail = 0; // read by loop
static volatile uint32_t fdRingDropped = 0;
static portMUX_TYPE fdRingMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// PROMISCUOUS CAPTURE
// ============================================================================

static void IRAM_ATTR fd_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (!pkt) return;

    // Counted before every filter below: if this stays at 0 the promiscuous
    // callback is not firing and nothing downstream can possibly detect.
    fdFramesAll = fdFramesAll + 1;

    int rssi = pkt->rx_ctrl.rssi;
    if (rssi < fdRssiMin) {
        fdRssiRejected = fdRssiRejected + 1;
        return;
    }

    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 24) return; // shorter than a mgmt MAC header

    // Cheap pre-filter: only management beacon / probe-req / probe-resp carry
    // the SSID/IE data our detectors need. Drop everything else here so the
    // ring only ever holds candidate frames.
    uint8_t fc = pkt->payload[0];
    if (((fc >> 2) & 0x3) != 0) return; // must be management
    uint8_t subtype = (fc >> 4) & 0xF;
    if (subtype != 0x4 && subtype != 0x5 && subtype != 0x8) return;

    fdFramesSeen = fdFramesSeen + 1; // volatile: avoid deprecated compound ++

    if (len > FD_FRAME_MAX) len = FD_FRAME_MAX;

    portENTER_CRITICAL_ISR(&fdRingMux);
    uint8_t next = (fdRingHead + 1) % FD_RING_SLOTS;
    if (next == fdRingTail) {
        fdRingDropped = fdRingDropped + 1; // ring full, drop this frame
    } else {
        FdRawFrame &slot = fdRing[fdRingHead];
        memcpy(slot.buf, pkt->payload, len);
        slot.len = len;
        slot.rssi = (int8_t)rssi;
        slot.channel = pkt->rx_ctrl.channel;
        fdRingHead = next;
        // High-water mark: if this approaches FD_RING_SLOTS the drain loop is
        // being starved (typically by a slow redraw) and frames are at risk.
        // + FD_RING_SLOTS before the modulo: the operands promote to int, and
        // C's % keeps the sign of a negative dividend, so the wrapped case
        // would otherwise report garbage.
        uint32_t used = (uint32_t)(((int)fdRingHead - (int)fdRingTail + FD_RING_SLOTS) % FD_RING_SLOTS);
        if (used > fdRingHigh) fdRingHigh = used;
    }
    portEXIT_CRITICAL_ISR(&fdRingMux);
}

static void fd_start_wifi() {
    ensureWifiPlatform();
    nvs_flash_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE)
        Serial.printf("[FlockDetect] wifi_init: %s\n", esp_err_to_name(e));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE)
        Serial.printf("[FlockDetect] wifi_start: %s\n", esp_err_to_name(e));
    esp_wifi_disconnect(); // drop any STA link so channel hopping isn't pinned

    // main.cpp sets this at boot, but esp_wifi_init() above resets country to the
    // driver default whenever WiFi had been deinitialised -- and the default policy
    // caps the channel list, so esp_wifi_set_channel(12..14) would fail silently.
    const wifi_country_t fdCountry = {
        .cc = "US", .schan = 1, .nchan = 14, .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    esp_wifi_set_country(&fdCountry);

    esp_wifi_set_promiscuous(false); // reset in case the framework left it on
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(fd_rx_cb);
    esp_wifi_set_promiscuous(true);
}

static void fd_stop_wifi() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    wifiDisconnect();
    vTaskDelay(1 / portTICK_RATE_MS);
}

// ============================================================================
// OUI CLASSIFICATION
// ============================================================================

enum FdOuiClass {
    FD_OUI_NONE = 0,
    FD_OUI_HIGH,
    FD_OUI_COMMUNITY,
    FD_OUI_MFR,
    FD_OUI_SOUND,
    FD_OUI_CAM,
};

// Returns the OUI class of a source MAC and, for camera vendors, the vendor
// name via *vendorOut. Locally-administered (randomized) MACs are rejected.
static FdOuiClass fdClassifyOui(const uint8_t *mac, const char **vendorOut) {
    if (vendorOut) *vendorOut = nullptr;
    if (mac[0] & 0x02) {
        // Randomized / locally administered: the OUI carries no vendor
        // information. If this counter dominates while fdCandOui stays at 0,
        // the target is MAC-randomizing and only the IE/SSID vectors can see it.
        fdRejLocalMac++;
        return FD_OUI_NONE;
    }

    char pfx[9];
    snprintf(pfx, sizeof(pfx), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);

    for (size_t i = 0; i < sizeof(FD_OUI_FLOCK_HIGH) / sizeof(FD_OUI_FLOCK_HIGH[0]); i++)
        if (strncasecmp(pfx, FD_OUI_FLOCK_HIGH[i], 8) == 0) return FD_OUI_HIGH;
    for (size_t i = 0; i < sizeof(FD_OUI_FLOCK_COMMUNITY) / sizeof(FD_OUI_FLOCK_COMMUNITY[0]); i++)
        if (strncasecmp(pfx, FD_OUI_FLOCK_COMMUNITY[i], 8) == 0) return FD_OUI_COMMUNITY;
    for (size_t i = 0; i < sizeof(FD_OUI_FLOCK_MFR) / sizeof(FD_OUI_FLOCK_MFR[0]); i++)
        if (strncasecmp(pfx, FD_OUI_FLOCK_MFR[i], 8) == 0) return FD_OUI_MFR;
    for (size_t i = 0; i < sizeof(FD_OUI_SOUNDTHINKING) / sizeof(FD_OUI_SOUNDTHINKING[0]); i++)
        if (strncasecmp(pfx, FD_OUI_SOUNDTHINKING[i], 8) == 0) return FD_OUI_SOUND;
    for (size_t i = 0; i < sizeof(FD_OUI_CAMERA) / sizeof(FD_OUI_CAMERA[0]); i++) {
        if (strncasecmp(pfx, FD_OUI_CAMERA[i].prefix, 8) == 0) {
            if (vendorOut) *vendorOut = FD_OUI_CAMERA[i].vendor;
            return FD_OUI_CAM;
        }
    }
    return FD_OUI_NONE;
}

// ============================================================================
// SSID MATCHING  (ported from recon fyWifiMatchSSID + FlockSquawk formats)
// ============================================================================

enum FdSsidClass { FD_SSID_NONE = 0, FD_SSID_WEAK, FD_SSID_STRONG };

static bool fdContainsCI(const char *s, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return false;
    for (size_t i = 0; i + nlen <= len; i++)
        if (strncasecmp(s + i, needle, nlen) == 0) return true;
    return false;
}

static bool fdAllHex(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

static bool fdAllDigits(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// Classifies an SSID and, on a match, copies the rule name into ruleOut.
static FdSsidClass fdMatchSsid(const char *ssid, size_t len, char *ruleOut, size_t ruleCap) {
    // Strong: exact dev SSID test_flck (CVE-2025-59409)
    if (len == 9 && strncasecmp(ssid, "test_flck", 9) == 0) {
        snprintf(ruleOut, ruleCap, "ssid_test_flck");
        return FD_SSID_STRONG;
    }
    // Strong: Flock-XXXXXX (6 hex) per GainSec research
    if (len == 12 && strncasecmp(ssid, "Flock-", 6) == 0 && fdAllHex(ssid + 6, 6)) {
        snprintf(ruleOut, ruleCap, "ssid_flock_hex");
        return FD_SSID_STRONG;
    }
    // Strong: Penguin-<10 decimals> (FlockSquawk)
    if (len == 18 && strncasecmp(ssid, "Penguin-", 8) == 0 && fdAllDigits(ssid + 8, 10)) {
        snprintf(ruleOut, ruleCap, "ssid_penguin");
        return FD_SSID_STRONG;
    }
    // Strong: exact "FS Ext Battery" (FlockSquawk)
    if (len == 14 && strncmp(ssid, "FS Ext Battery", 14) == 0) {
        snprintf(ruleOut, ruleCap, "ssid_fs_battery");
        return FD_SSID_STRONG;
    }
    // Weak: any flock / flck substring (catch-all)
    if (fdContainsCI(ssid, len, "flock")) {
        snprintf(ruleOut, ruleCap, "ssid_flock_substr");
        return FD_SSID_WEAK;
    }
    if (fdContainsCI(ssid, len, "flck")) {
        snprintf(ruleOut, ruleCap, "ssid_flck_substr");
        return FD_SSID_WEAK;
    }
    return FD_SSID_NONE;
}

// Hidden SSID: element present but empty, or padded with null bytes (recon).
static bool fdIsHiddenSsid(const uint8_t *data, uint8_t len) {
    if (len == 0) return true;
    for (uint8_t i = 0; i < len; i++)
        if (data[i] != 0) return false;
    return true;
}

static bool fdSeenHidden(const uint8_t *mac) {
    uint8_t n = fdHiddenCount < FD_HIDDEN_DEDUP ? fdHiddenCount : FD_HIDDEN_DEDUP;
    for (uint8_t i = 0; i < n; i++)
        if (memcmp(fdHiddenBssids[i], mac, 6) == 0) return true;
    return false;
}

static void fdAddHidden(const uint8_t *mac) {
    if (fdHiddenCount < FD_HIDDEN_DEDUP) {
        memcpy(fdHiddenBssids[fdHiddenCount], mac, 6);
        fdHiddenCount++;
    } else {
        memcpy(fdHiddenBssids[fdHiddenNext], mac, 6);
        fdHiddenNext = (fdHiddenNext + 1) % FD_HIDDEN_DEDUP;
    }
}

// ============================================================================
// IE-SIGNATURE BUILDER  (ported from flock-you main.cpp, MIT)
// ============================================================================

static void fdHexNibbles(char *dst, const uint8_t *b, int n) {
    static const char hd[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        dst[i * 2] = hd[b[i] >> 4];
        dst[i * 2 + 1] = hd[b[i] & 0x0f];
    }
}

static bool fdSigAppend(char *out, size_t cap, size_t *pos, const char *part) {
    size_t plen = strlen(part);
    if (*pos != 0) {
        if (*pos + 1 >= cap) return false;
        out[(*pos)++] = ',';
    }
    if (*pos + plen >= cap) return false;
    memcpy(out + *pos, part, plen);
    *pos += plen;
    out[*pos] = '\0';
    return true;
}

static bool fdSigAppendVendor(char *out, size_t cap, size_t *pos, const uint8_t *body, int elen) {
    char buf[24];
    int take = elen < 8 ? elen : 8; // vendor payload truncated to 8 bytes
    buf[0] = '2';
    buf[1] = '2';
    buf[2] = '1';
    buf[3] = ':';
    fdHexNibbles(buf + 4, body, take);
    buf[4 + take * 2] = '\0';
    return fdSigAppend(out, cap, pos, buf);
}

static bool fdSigAppendTag(char *out, size_t cap, size_t *pos, uint8_t id) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)id);
    return fdSigAppend(out, cap, pos, buf);
}

static bool fdLiteonVendorAt(const uint8_t *ies, int len, int pos) {
    return pos + 9 <= len && ies[pos] == 221 && ies[pos + 1] == 7 && ies[pos + 2] == 0x50 &&
           ies[pos + 3] == 0x6f && ies[pos + 4] == 0x9a;
}

static bool fdPhantomLiteonAhead(const uint8_t *ies, int len, int pos) {
    int end = pos + 2 + 32;
    if (end > len - 1) end = len - 1;
    for (int j = pos + 2; j < end; j++)
        if (fdLiteonVendorAt(ies, len, j)) return true;
    return false;
}

static bool fdIsPhantomOverflow(const uint8_t *ies, int len, uint8_t id, int elen, int i) {
    if (i + 2 + elen <= len) return false;
    if (elen > 200) return true;
    return id == 64 && elen == 128 && fdPhantomLiteonAhead(ies, len, i);
}

static int fdTlvResync(const uint8_t *ies, int len, int start) {
    int end = start + 64;
    if (end > len - 1) end = len - 1;
    for (int j = start; j < end; j++) {
        int elen = (int)ies[j + 1];
        if (elen <= 200 && j + 2 + elen <= len) return j;
    }
    return -1;
}

static bool fdBuildIeSig(const uint8_t *ies, int len, char *out, size_t cap, bool *complete) {
    if (!ies || len < 2 || !out || cap < 2) return false;
    size_t pos = 0;
    out[0] = '\0';
    int i = 0;
    uint8_t phantomSkips = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i];
        int elen = (int)ies[i + 1];
        if (i + 2 + elen > len) {
            if (phantomSkips < 16 && fdIsPhantomOverflow(ies, len, id, elen, i)) {
                phantomSkips++;
                i += 2;
                continue;
            }
            int j = fdTlvResync(ies, len, i);
            if (j > i) {
                i = j;
                continue;
            }
            return false;
        }
        i += 2;
        if (id == 0) { // SSID never enters the signature
            if (elen == 0) {
                while (i + 2 <= len && ies[i] == 0 && ies[i + 1] == 0) i += 2;
            } else {
                i += elen;
            }
            continue;
        }
        if (id == 221 && elen >= 4) {
            if (!fdSigAppendVendor(out, cap, &pos, ies + i, elen)) return false;
        } else {
            if (!fdSigAppendTag(out, cap, &pos, id)) return false;
        }
        i += elen;
    }
    if (complete) *complete = (i == len);
    return pos > 0;
}

static void fdCanonicalizeIeSig(char *sig, size_t cap) {
    if (!sig || cap < 8) return;
    if (strncmp(sig, "2,12,127,", 9) == 0 && strstr(sig, FLOCK_LITEON_IE_SIG_PREFIX) != nullptr)
        return;
    const char *anchor = strstr(sig, FLOCK_LITEON_IE_SIG_PREFIX);
    if (!anchor) return;
    char tmp[128];
    int n = snprintf(tmp, sizeof(tmp), "2,12,127,%s", anchor);
    if (n > 0 && (size_t)n < cap) memcpy(sig, tmp, (size_t)n + 1);
}

static bool fdPickBetterSig(
    const char *a, bool aComplete, const char *b, bool bComplete, char *out, size_t cap
) {
    if (!a[0] && !b[0]) return false;
    if (a[0] && !b[0]) {
        strncpy(out, a, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
    if (!a[0] && b[0]) {
        strncpy(out, b, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
    const char *pick = a;
    if (aComplete && !bComplete) pick = a;
    else if (!aComplete && bComplete) pick = b;
    else if (strlen(b) > strlen(a)) pick = b;
    strncpy(out, pick, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

static bool fdBuildIeSigFromBody(const uint8_t *body, int bodyLen, char *out, size_t cap) {
    if (!body || bodyLen < 2 || !out || cap < 16) return false;
    char sigA[128] = {0};
    char sigB[128] = {0};
    bool completeA = false, completeB = false;
    bool okA = fdBuildIeSig(body, bodyLen, sigA, sizeof(sigA), &completeA);
    bool okB = false;
    if (bodyLen >= 2 && body[0] == 0 && body[1] == 0)
        okB = fdBuildIeSig(body + 2, bodyLen - 2, sigB, sizeof(sigB), &completeB);
    char merged[128] = {0};
    if (!fdPickBetterSig(okA ? sigA : "", completeA, okB ? sigB : "", completeB, merged, sizeof(merged)))
        return false;
    fdCanonicalizeIeSig(merged, sizeof(merged));
    strncpy(out, merged, cap - 1);
    out[cap - 1] = '\0';
    return out[0] != '\0';
}

// True if a wildcard probe-request body matches the primary Flock IE signature.
// Also hands back the signature string it computed, matching or not, so the
// diagnostic pages can show how a near-miss differs from the expected constant
// (the match is an exact strcmp, and fdCanonicalizeIeSig additionally pins the
// tail after the LiteOn vendor IE, so a single extra trailing IE defeats it).
static bool fdProbeSigAndMatch(const uint8_t *body, int bodyLen, char *sigOut, size_t sigCap) {
    if (sigOut && sigCap) sigOut[0] = '\0';
    char sig[128];
    bool matched = false;
    if (fdBuildIeSigFromBody(body, bodyLen, sig, sizeof(sig))) {
        if (sigOut && sigCap) {
            strncpy(sigOut, sig, sigCap - 1);
            sigOut[sigCap - 1] = '\0';
        }
        if (strcmp(sig, FLOCK_PROBE_IE_SIG_PRIMARY) == 0) matched = true;
    }
    if (!matched && bodyLen > 4 && fdBuildIeSigFromBody(body, bodyLen - 4, sig, sizeof(sig))) {
        if (sigOut && sigCap && !sigOut[0]) {
            strncpy(sigOut, sig, sigCap - 1);
            sigOut[sigCap - 1] = '\0';
        }
        if (strcmp(sig, FLOCK_PROBE_IE_SIG_PRIMARY) == 0) matched = true;
    }
    return matched;
}

// ============================================================================
// SD LOGGING
// ============================================================================

static void fdLogInit() {
    if (!fdLogEnabled) return;
    if (!sdcardMounted) setupSdCard();
    if (!getFsStorage(fdFs) || fdFs == nullptr) {
        fdFs = nullptr;
        return;
    }
    if (!fdFs->exists("/FlockDetect")) fdFs->mkdir("/FlockDetect");
    // Pick the next free filename index so sessions don't clobber each other.
    for (int idx = 0; idx < 1000; idx++) {
        String p = "/FlockDetect/flockdetect_" + String(idx) + ".csv";
        if (!fdFs->exists(p)) {
            fdLogPath = p;
            break;
        }
    }
    if (fdLogPath == "") return;
    File f = fdFs->open(fdLogPath, FILE_WRITE);
    if (!f) {
        fdFs = nullptr;
        return;
    }
    f.println("mac,tier,method,ssid,rssi,peak_rssi,channel,seq,hits,lat,lon,ts");
    f.close();

    // Companion diagnostic log: one row per *candidate* frame (not per
    // detection), so a session walked past known hardware can be analysed off
    // the card even when nothing ever reached a tier.
    fdDiagLogPath = fdLogPath;
    fdDiagLogPath.replace("flockdetect_", "flockdiag_");
    File d = fdFs->open(fdDiagLogPath, FILE_WRITE);
    if (!d) {
        fdDiagLogPath = "";
        return;
    }
    d.println("ts,src_mac,subtype,rssi,channel,seq,oui_class,ssid,ie_sig");
    d.close();
}

// Appends a candidate frame to the diagnostic CSV. Called only on genuinely
// new observations (first sighting of a MAC, first sighting of an IE
// signature) so a busy channel cannot turn this into a write storm.
static void fdDiagLogAppend(
    const uint8_t *mac, uint8_t subtype, int8_t rssi, uint8_t channel, uint16_t seq,
    uint8_t ouiClass, const char *ssid, const char *ieSig
) {
    if (!fdLogEnabled || fdFs == nullptr || fdDiagLogPath == "") return;
    File f = fdFs->open(fdDiagLogPath, FILE_APPEND);
    if (!f) return;
    char buf[220];
    snprintf(
        buf, sizeof(buf), "%lu,%02x:%02x:%02x:%02x:%02x:%02x,%u,%d,%u,%u,%u,\"%s\",\"%s\"\n",
        (unsigned long)millis(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)subtype,
        (int)rssi, (unsigned)channel, (unsigned)seq, (unsigned)ouiClass, ssid ? ssid : "",
        ieSig ? ieSig : ""
    );
    f.print(buf);
    f.close();
}

static const char *fdTierName(uint8_t tier) {
    switch (tier) {
        case FD_CONFIRMED: return "CONFIRMED";
        case FD_SUSPICIOUS: return "SUSPICIOUS";
        case FD_INFO: return "INFO";
        default: return "NONE";
    }
}

static void fdLogAppend(const FdDevice &d) {
    if (!fdLogEnabled || fdFs == nullptr || fdLogPath == "") return;
    File f = fdFs->open(fdLogPath, FILE_APPEND);
    if (!f) return;
    char macStr[18];
    snprintf(
        macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x", d.mac[0], d.mac[1], d.mac[2],
        d.mac[3], d.mac[4], d.mac[5]
    );
    double phLat, phLon;
    float phAcc;
    uint32_t phAge;
    bool havePhoneGps = flock_link_gps(phLat, phLon, phAcc, phAge);
    bool haveGps = fdGpsEnabled && fdGps.location.isValid();
    char buf[256];
    if (haveGps || havePhoneGps) {
        snprintf(
            buf, sizeof(buf), "%s,%s,%s,\"%s\",%d,%d,%lu,%u,%lu,%.6f,%.6f,%lu\n", macStr,
            fdTierName(d.tier), d.method, d.ssid, d.rssi, d.peakRssi, (unsigned long)d.channel,
            (unsigned)d.lastSeq, (unsigned long)d.hits,
            haveGps ? fdGps.location.lat() : phLat, haveGps ? fdGps.location.lng() : phLon,
            (unsigned long)d.lastSeen
        );
    } else {
        snprintf(
            buf, sizeof(buf), "%s,%s,%s,\"%s\",%d,%d,%lu,%u,%lu,,,%lu\n", macStr,
            fdTierName(d.tier), d.method, d.ssid, d.rssi, d.peakRssi, (unsigned long)d.channel,
            (unsigned)d.lastSeq, (unsigned long)d.hits, (unsigned long)d.lastSeen
        );
    }
    f.print(buf);
    f.close();
}

// ============================================================================
// ALERT (LED + sound)
// ============================================================================

static void fdFireAlert(const FdDevice &d) {
    fdAlertActive = true;
    fdAlertStart = millis();
    fdAlertUntil = fdAlertStart + FD_ALERT_SHOW_MS;
    fdAlertPainted = false;
    fdAlertDev = d;
#ifdef HAS_RGB_LED
    if (fdLedEnabled) setLedColor(CRGB::Red);
#endif
    if (fdSoundEnabled) {
        // Rising two-chirp alarm (honors bruceConfig.soundEnabled inside _tone).
        _tone(600, 60);
        _tone(1000, 90);
    }
}

static void fdClearAlert() {
    fdAlertActive = false;
#ifdef HAS_RGB_LED
    if (fdLedEnabled) setLedColor(CRGB::Black);
#endif
}

// ============================================================================
// DEVICE TRACKING
// ============================================================================

static void fdSeenSetTier(const uint8_t *mac, uint8_t tier); // diagnostic table, below

static int fdFindDevice(const uint8_t *mac) {
    for (size_t i = 0; i < fdDeviceCount; i++)
        if (memcmp(fdDevices[i].mac, mac, 6) == 0) return (int)i;
    return -1;
}

static int fdAllocDevice() {
    if (fdDeviceCount < FD_MAX_TRACKED) return (int)fdDeviceCount++;
    // Evict the least-recently-seen slot.
    int oldest = 0;
    for (size_t i = 1; i < fdDeviceCount; i++)
        if (fdDevices[i].lastSeen < fdDevices[oldest].lastSeen) oldest = (int)i;
    return oldest;
}

// Records a detection, updates its tracked entry, and fires an alarm on a
// first-seen CONFIRMED hit (with a per-MAC cooldown for re-alarms).
static void fdRecord(
    const uint8_t *mac, uint8_t tier, const char *method, const char *ssid, int8_t rssi,
    uint8_t channel, uint16_t seq = 0
) {
    uint32_t now = millis();
    int idx = fdFindDevice(mac);
    bool isNew = (idx < 0);
    if (isNew) {
        idx = fdAllocDevice();
        FdDevice &d = fdDevices[idx];
        memset(&d, 0, sizeof(d));
        memcpy(d.mac, mac, 6);
        d.firstSeen = now;
        d.peakRssi = -128;
        d.dispRssi = (int16_t)rssi * 8;   // start the eased bar at its true value
        d.flashUntil = now + FD_FLASH_MS; // highlight the new row briefly
        // Stable radar bearing: a cheap MAC hash, so a device keeps the same
        // angle across frames instead of jittering around the disc.
        uint32_t h = 2166136261u;
        for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
        d.blipAngle = (uint8_t)(h & 63);
    }
    FdDevice &d = fdDevices[idx];
    d.lastSeen = now;
    d.rssi = rssi;
    if (rssi > d.peakRssi) d.peakRssi = rssi;
    d.channel = channel;
    d.lastSeq = seq;
    d.hits++;

    bool tierUp = tier > d.tier;
    if (tierUp) {
        d.tier = tier;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.method[sizeof(d.method) - 1] = '\0';
    }
    if (ssid && ssid[0] && (isNew || tierUp)) {
        strncpy(d.ssid, ssid, sizeof(d.ssid) - 1);
        d.ssid[sizeof(d.ssid) - 1] = '\0';
    }

    if (isNew) {
        if (tier == FD_CONFIRMED) fdConfirmedCount++;
        else if (tier == FD_SUSPICIOUS) fdSuspiciousCount++;
    } else if (tierUp) {
        if (tier == FD_CONFIRMED) fdConfirmedCount++;
        else if (tier == FD_SUSPICIOUS) fdSuspiciousCount++;
    }

    // Alarm on CONFIRMED, throttled per-MAC (covers first-seen since
    // lastAlarmMs starts at 0).
    if (d.tier == FD_CONFIRMED && (now - d.lastAlarmMs >= FD_ALARM_COOLDOWN_MS || d.lastAlarmMs == 0)) {
        d.lastAlarmMs = now;
        fdFireAlert(d);
    }

    // Mirror the tier into the diagnostic "everything seen" table.
    fdSeenSetTier(mac, d.tier);

    // Log confirmed / suspicious detections (skip pure INFO noise) once per
    // new sighting or tier upgrade.
    if ((isNew || tierUp) && d.tier >= FD_SUSPICIOUS) {
        fdLogAppend(d);
        flock_link_send_detection(
            d.mac, d.tier, d.method, d.ssid, d.rssi, d.peakRssi, d.channel, d.lastSeq, d.hits, "",
            channel == 0
        );
    }
}

// ============================================================================
// DIAGNOSTIC TABLES
// ============================================================================

// Records a computed IE signature. Returns true the first time a given
// signature string is seen, so the caller can log it to SD exactly once.
static bool fdSigLogAdd(const char *sig, const uint8_t *mac) {
    if (!sig || !sig[0]) return false;
    for (uint8_t i = 0; i < fdSigLogCount; i++) {
        if (strcmp(fdSigLog[i].sig, sig) == 0) {
            if (fdSigLog[i].hits < 0xFFFF) fdSigLog[i].hits++;
            return false;
        }
    }
    uint8_t slot;
    if (fdSigLogCount < FD_SIGLOG_SLOTS) {
        slot = fdSigLogCount++;
    } else {
        slot = fdSigLogNext;
        fdSigLogNext = (fdSigLogNext + 1) % FD_SIGLOG_SLOTS;
    }
    strncpy(fdSigLog[slot].sig, sig, sizeof(fdSigLog[slot].sig) - 1);
    fdSigLog[slot].sig[sizeof(fdSigLog[slot].sig) - 1] = '\0';
    memcpy(fdSigLog[slot].oui, mac, 3);
    fdSigLog[slot].hits = 1;
    return true;
}

static int fdSeenFind(const uint8_t *mac) {
    for (size_t i = 0; i < fdSeenCount; i++)
        if (memcmp(fdSeen[i].mac, mac, 6) == 0) return (int)i;
    return -1;
}

// Records any management-frame source. Returns true on first sighting.
static bool fdSeenAdd(const uint8_t *mac, int8_t rssi, uint8_t ouiClass, const char *ssid, uint16_t seq) {
    int idx = fdSeenFind(mac);
    if (idx >= 0) {
        fdSeen[idx].rssi = rssi;
        fdSeen[idx].seq = seq;
        if (fdSeen[idx].hits < 0xFFFF) fdSeen[idx].hits++;
        if (ssid && ssid[0] && !fdSeen[idx].ssid[0]) {
            strncpy(fdSeen[idx].ssid, ssid, sizeof(fdSeen[idx].ssid) - 1);
            fdSeen[idx].ssid[sizeof(fdSeen[idx].ssid) - 1] = '\0';
        }
        return false;
    }
    if (fdSeenCount < FD_MAX_SEEN) {
        idx = (int)fdSeenCount++;
    } else {
        // Full: evict the least interesting entry (lowest tier, then fewest
        // hits) so a crowded channel cannot push a quiet target out.
        int worst = 0;
        for (size_t i = 1; i < fdSeenCount; i++) {
            bool lower = fdSeen[i].tier < fdSeen[worst].tier ||
                         (fdSeen[i].tier == fdSeen[worst].tier && fdSeen[i].hits < fdSeen[worst].hits);
            if (lower) worst = (int)i;
        }
        if (fdSeen[worst].tier != FD_NONE) return false; // nothing worth evicting
        idx = worst;
    }
    FdSeen &s = fdSeen[idx];
    memset(&s, 0, sizeof(s));
    memcpy(s.mac, mac, 6);
    s.rssi = rssi;
    s.ouiClass = ouiClass;
    s.tier = FD_NONE;
    s.hits = 1;
    s.seq = seq;
    if (ssid && ssid[0]) {
        strncpy(s.ssid, ssid, sizeof(s.ssid) - 1);
        s.ssid[sizeof(s.ssid) - 1] = '\0';
    }
    return true;
}

static void fdSeenSetTier(const uint8_t *mac, uint8_t tier) {
    int idx = fdSeenFind(mac);
    if (idx >= 0 && tier > fdSeen[idx].tier) fdSeen[idx].tier = tier;
}

// ============================================================================
// FRAME PROCESSING (runs in the loop, not the ISR)
// ============================================================================

static void fdProcessFrame(const uint8_t *payload, uint16_t rawLen, int8_t rssi, uint8_t channel) {
    fdFramesProcessed++;

    uint16_t len = rawLen;
    if (len > 4) len -= 4; // drop FCS
    if (len < 24) {
        fdParseFailed++;
        return;
    }

    uint8_t fc = payload[0];
    uint8_t subtype = (fc >> 4) & 0xF;
    const uint8_t *src = payload + 10; // addr2

    // Sequence Control, bytes 22-23: 12-bit sequence number, 4-bit fragment.
    // Idea from flock-back. The counter is per-radio and keeps incrementing
    // across MAC randomisation, so it can tie a randomising device to one
    // transmitter when the OUI vector is blind to it.
    uint16_t seq = (uint16_t)(((payload[23] << 8) | payload[22]) >> 4) & 0x0FFF;

    const uint8_t *tagged;
    if (subtype == 0x8 || subtype == 0x5) {
        if (len < 36) {
            fdParseFailed++;
            return;
        }
        tagged = payload + 36; // beacon / probe-resp fixed params
        if (subtype == 0x8) fdCntBeacon++;
        else fdCntProbeResp++;
    } else if (subtype == 0x4) {
        tagged = payload + 24; // probe-req
        fdCntProbeReq++;
    } else {
        return;
    }
    const uint8_t *end = payload + len;

    // Walk tagged parameters for the SSID element (tag 0).
    const uint8_t *ssidData = nullptr;
    uint8_t ssidLen = 0;
    bool tlvBroke = false;
    const uint8_t *p = tagged;
    while (p + 2 <= end) {
        uint8_t tagId = p[0];
        uint8_t tagLen = p[1];
        if (p + 2 + tagLen > end) {
            tlvBroke = true;
            break;
        }
        if (tagId == 0) {
            ssidData = p + 2;
            ssidLen = tagLen;
            break;
        }
        p += 2 + tagLen;
    }
    if (tlvBroke) fdParseFailed++;

    const char **vendorOut = nullptr;
    const char *camVendor = nullptr;
    vendorOut = &camVendor;
    FdOuiClass ouiClass = fdClassifyOui(src, vendorOut);
    if (ouiClass != FD_OUI_NONE) fdCandOui++;

    // --- Diagnostics: record the frame before any detection decision -------
    // Everything below can decide "not interesting" and return; this snapshot
    // is what proves a device was heard at all, and with which MAC and SSID.
    char seenSsid[20] = {0};
    if (ssidData && ssidLen > 0) {
        size_t c = ssidLen < sizeof(seenSsid) - 1 ? ssidLen : sizeof(seenSsid) - 1;
        for (size_t i = 0; i < c; i++) {
            char ch = (char)ssidData[i];
            seenSsid[i] = (ch >= 0x20 && ch < 0x7F) ? ch : '.';
        }
    }
    memcpy(fdLastMac, src, 6);
    fdLastRssi = rssi;
    fdLastSubtype = subtype;
    fdLastSeq = seq;
    strncpy(fdLastSsid, seenSsid, sizeof(fdLastSsid) - 1);
    fdLastSsid[sizeof(fdLastSsid) - 1] = '\0';
    bool firstSighting = fdSeenAdd(src, rssi, (uint8_t)ouiClass, seenSsid, seq);
    if (firstSighting && ouiClass == FD_OUI_NONE && !(src[0] & 0x02))
        fdDiagLogAppend(src, subtype, rssi, channel, seq, (uint8_t)ouiClass, seenSsid, "");

    // --- Vector 1: wildcard-probe IE signature (strongest) -----------------
    if (subtype == 0x4 && ssidData && ssidLen == 0) {
        fdCntWildcard++;
        const uint8_t *body = payload + 24;
        int bodyLen = (int)(end - body);
        char ieSig[96] = {0};
        bool match = bodyLen > 0 && fdProbeSigAndMatch(body, bodyLen, ieSig, sizeof(ieSig));
        // Logged whether or not it matched -- a near-miss here is the single
        // most useful piece of evidence when confirmed hardware goes unseen.
        if (ieSig[0] && fdSigLogAdd(ieSig, src))
            fdDiagLogAppend(src, subtype, rssi, channel, seq, (uint8_t)ouiClass, "", ieSig);
        if (match) {
            fdCandIe++;
            fdRecord(src, FD_CONFIRMED, "ie_sig", "", rssi, channel, seq);
            return;
        }
    }

    // --- Vector 3: hidden-SSID beacon/probe-resp from a Flock-ish OUI ------
    bool isBeaconResp = (subtype == 0x8 || subtype == 0x5);
    bool isHidden = isBeaconResp && fdIsHiddenSsid(ssidData ? ssidData : (const uint8_t *)"", ssidLen);
    if (isHidden && (ouiClass == FD_OUI_HIGH || ouiClass == FD_OUI_COMMUNITY || ouiClass == FD_OUI_MFR)) {
        if (!fdSeenHidden(src)) {
            fdAddHidden(src);
            fdRecord(src, FD_SUSPICIOUS, "hidden_ssid", "<hidden>", rssi, channel, seq);
        }
        return;
    }

    // --- Vector 2: SSID text patterns --------------------------------------
    char ssidRule[24] = {0};
    FdSsidClass ssidClass = FD_SSID_NONE;
    char ssidStr[33] = {0};
    if (ssidData && ssidLen > 0 && !isHidden) {
        size_t c = ssidLen < 32 ? ssidLen : 32;
        memcpy(ssidStr, ssidData, c);
        ssidStr[c] = '\0';
        ssidClass = fdMatchSsid(ssidStr, c, ssidRule, sizeof(ssidRule));
        if (ssidClass != FD_SSID_NONE) fdCandSsid++;
    }

    bool flockHighOui = (ouiClass == FD_OUI_HIGH);

    // --- Tier resolution (FlockSquawk computeWiFiAlertLevel shape) ---------
    if (ssidClass == FD_SSID_STRONG) {
        fdRecord(src, FD_CONFIRMED, ssidRule, ssidStr, rssi, channel, seq);
        return;
    }
    if (flockHighOui && ssidClass != FD_SSID_NONE) {
        // Flock hardware OUI + any SSID hit -> confirmed.
        fdRecord(src, FD_CONFIRMED, "oui+ssid", ssidStr, rssi, channel, seq);
        return;
    }
    if (flockHighOui) {
        fdRecord(src, FD_SUSPICIOUS, "oui_flock", ssidStr, rssi, channel, seq);
        return;
    }
    if (ouiClass == FD_OUI_COMMUNITY) {
        fdRecord(src, FD_SUSPICIOUS, "oui_community", ssidStr, rssi, channel, seq);
        return;
    }
    if (ouiClass == FD_OUI_MFR) {
        fdRecord(src, FD_SUSPICIOUS, "oui_mfr", ssidStr, rssi, channel, seq);
        return;
    }
    if (ssidClass == FD_SSID_WEAK) {
        fdRecord(src, FD_SUSPICIOUS, ssidRule, ssidStr, rssi, channel, seq);
        return;
    }
    if (ouiClass == FD_OUI_SOUND) {
        fdRecord(src, FD_INFO, "oui_soundthinking", ssidStr, rssi, channel, seq);
        return;
    }
    if (ouiClass == FD_OUI_CAM) {
        char m[20];
        snprintf(m, sizeof(m), "cam:%s", camVendor ? camVendor : "?");
        fdRecord(src, FD_INFO, m, ssidStr, rssi, channel, seq);
        return;
    }
}

static void fdDrainRing() {
    for (;;) {
        FdRawFrame frame;
        bool have = false;
        portENTER_CRITICAL(&fdRingMux);
        if (fdRingTail != fdRingHead) {
            frame = fdRing[fdRingTail];
            fdRingTail = (fdRingTail + 1) % FD_RING_SLOTS;
            have = true;
        }
        portEXIT_CRITICAL(&fdRingMux);
        if (!have) break;
        fdProcessFrame(frame.buf, frame.len, frame.rssi, frame.channel);
    }
}

// ============================================================================
// GPS (optional, external module on the Grove/QWIIC UART)
// ============================================================================

static void fdGpsStart() {
    if (fdGpsStarted) return;
    pinMode(bruceConfigPins.gps_bus.rx, INPUT); // release shared pin for the UART
    fdGpsSerial.begin(
        bruceConfigPins.gpsBaudrate, SERIAL_8N1, bruceConfigPins.gps_bus.rx, bruceConfigPins.gps_bus.tx
    );
    fdGpsStarted = true;
}

static void fdGpsStop() {
    if (!fdGpsStarted) return;
    fdGpsSerial.end();
    fdGpsStarted = false;
}

static void fdGpsPoll() {
    if (!fdGpsEnabled || !fdGpsStarted) return;
    while (fdGpsSerial.available() > 0) fdGps.encode(fdGpsSerial.read());
}

// ============================================================================
// UI
// ============================================================================
// Layout: the Bruce chrome owns y 0..44 -- the status bar and its separator
// line up to y=25, then printTitle() draws the FM-size (16px tall) title at
// BORDER_PAD_Y = 28. Content therefore starts at FD_TOP; the previous content
// origin of y=26 painted the scan banner straight over the title.
//
// The chrome is drawn once on entry and once after any takeover, never per
// frame: drawMainBorderWithTitle() defaults to clear=true, so calling it every
// frame did a full fillScreen plus a battery read over I2C. That both strobed
// the display and stole loop time from fdDrainRing(), which is why the redraw
// rate is a detection concern and not only a cosmetic one.

#define FD_TOP 46
#define FD_LEFT 8
#define FD_ROW_H 11
#define FD_RADAR_W 68

static inline int fdFooterY() { return tftHeight - 18; }
static inline int fdContentW() { return tftWidth - 2 * FD_LEFT; }
static inline int fdContentH() { return fdFooterY() - FD_TOP - 2; }
// The radar only earns its space on wider panels; narrow screens hand the
// whole content width to the list instead.
static inline bool fdHasRadar() { return fdContentW() >= 280 && fdContentH() >= 78; }
static inline int fdListW() { return fdContentW() - (fdHasRadar() ? FD_RADAR_W : 0); }

static bool fdSpriteOk = false;   // content double-buffer available
static uint16_t fdFrame = 0;      // animation tick, advances once per drawn frame
static uint16_t fdDwellPct = 0;   // 0..100 progress through the current channel dwell
static int fdOrder[FD_MAX_TRACKED]; // cached sort order, rebuilt at the data rate
static int fdOrderCount = 0;
static int fdDiagScroll = 0;

static uint16_t fdTierColor(uint8_t tier) {
    switch (tier) {
        case FD_CONFIRMED: return TFT_RED;
        case FD_SUSPICIOUS: return TFT_ORANGE;
        case FD_INFO: return TFT_DARKGREY;
        default: return bruceConfig.priColor;
    }
}

// Scales a 565 colour toward black; f = 0 (black) .. 255 (unchanged). Used for
// the radar sweep tail and for fading blips by age.
static uint16_t fdDim(uint16_t c, uint8_t f) {
    uint16_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (uint16_t)((r * f) >> 8);
    g = (uint16_t)((g * f) >> 8);
    b = (uint16_t)((b * f) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// 64-step sine table, +-127. Built once so the radar never calls sinf().
static int8_t fdSinLut[64];
static bool fdLutReady = false;
static void fdInitLut() {
    if (fdLutReady) return;
    for (int i = 0; i < 64; i++)
        fdSinLut[i] = (int8_t)lrintf(127.0f * sinf((float)i * 2.0f * PI / 64.0f));
    fdLutReady = true;
}
static inline int fdSinQ(int a) { return fdSinLut[a & 63]; }
static inline int fdCosQ(int a) { return fdSinLut[(a + 16) & 63]; }

// Maps an eased RSSI (stored in 1/8 dBm) to 0..100, 100 being the strongest.
static int fdRssiPct(int16_t rssi8) {
    int r = rssi8 / 8;
    if (r < -95) r = -95;
    if (r > -30) r = -30;
    return (r + 95) * 100 / 65;
}

static void fdSpriteBegin() {
    fdSpriteOk = false;
    sprite.deleteSprite();
    sprite.setColorDepth(16);
    if (sprite.createSprite(fdContentW(), fdContentH()) != nullptr) fdSpriteOk = true;
}

static void fdSpriteEnd() {
    sprite.deleteSprite();
    fdSpriteOk = false;
}

// Repaints the static chrome. Called on entry and after anything that takes
// the whole screen (options menu, alert) -- never inside the draw loop.
static void fdDrawChrome() {
    tft.fillScreen(bruceConfig.bgColor);
    drawMainBorderWithTitle("FlockDetect", false);
    tft.setTextSize(FP);
}

// Sorted view: highest tier first, then arrival order. The secondary key is
// firstSeen (not lastSeen) so rows keep their position between frames instead
// of reshuffling every time a beacon lands.
static void fdRebuildOrder() {
    fdOrderCount = (int)fdDeviceCount;
    for (int i = 0; i < fdOrderCount; i++) fdOrder[i] = i;
    for (int i = 1; i < fdOrderCount; i++) {
        int key = fdOrder[i];
        int j = i - 1;
        while (j >= 0) {
            FdDevice &a = fdDevices[fdOrder[j]];
            FdDevice &b = fdDevices[key];
            bool before = (b.tier > a.tier) || (b.tier == a.tier && b.firstSeen < a.firstSeen);
            if (!before) break;
            fdOrder[j + 1] = fdOrder[j];
            j--;
        }
        fdOrder[j + 1] = key;
    }
}

// Per-frame easing of presentation-only values.
static void fdAnimate() {
    for (size_t i = 0; i < fdDeviceCount; i++) {
        FdDevice &d = fdDevices[i];
        int16_t target = (int16_t)d.rssi * 8;
        int16_t delta = (int16_t)(target - d.dispRssi);
        if (delta > -4 && delta < 4) d.dispRssi = target;
        else d.dispRssi = (int16_t)(d.dispRssi + delta / 4);
    }
}

static void fdMacTail(char *out, size_t cap, const uint8_t *mac) {
    snprintf(out, cap, "%02x:%02x:%02x", mac[3], mac[4], mac[5]);
}

// Keeps the cursor and scroll window inside the current list, given how many
// rows the renderer can show. Shared by both renderers so the fallback path
// scrolls identically to the sprite one.
static void fdClampList(int rows) {
    if (fdOrderCount <= 0) {
        fdSelected = 0;
        fdScrollTop = 0;
        return;
    }
    if (fdSelected >= fdOrderCount) fdSelected = fdOrderCount - 1;
    if (fdSelected < 0) fdSelected = 0;
    if (fdSelected < fdScrollTop) fdScrollTop = fdSelected;
    if (fdSelected >= fdScrollTop + rows) fdScrollTop = fdSelected - rows + 1;
    if (fdScrollTop > fdOrderCount - rows) fdScrollTop = fdOrderCount > rows ? fdOrderCount - rows : 0;
    if (fdScrollTop < 0) fdScrollTop = 0;
}

// ---------------------------------------------------------------------------
// Radar
// ---------------------------------------------------------------------------
// Bearing is a stable hash of the MAC, radius tracks signal strength (strong
// toward the centre), brightness fades with time since the device was last
// heard. The sweep advances one LUT step per frame, so a full rotation takes
// 64 frames -- about four seconds at FD_FRAME_MS.
static void fdDrawRadar(int cx, int cy, int rad) {
    uint16_t base = bruceConfig.priColor;
    sprite.drawCircle(cx, cy, rad, fdDim(base, 90));
    sprite.drawCircle(cx, cy, rad * 2 / 3, fdDim(base, 55));
    sprite.drawCircle(cx, cy, rad / 3, fdDim(base, 55));

    int sweep = fdFrame & 63;
    for (int t = 0; t < 8; t++) {
        uint8_t f = (uint8_t)(210 - t * 25);
        int a = sweep - t;
        int ex = cx + (fdCosQ(a) * rad) / 127;
        int ey = cy + (fdSinQ(a) * rad) / 127;
        sprite.drawLine(cx, cy, ex, ey, fdDim(base, f));
    }

    uint32_t now = millis();
    for (size_t i = 0; i < fdDeviceCount; i++) {
        FdDevice &d = fdDevices[i];
        int r = rad - (fdRssiPct(d.dispRssi) * rad) / 100;
        uint32_t age = now - d.lastSeen;
        uint8_t f = age >= 20000 ? 55 : (uint8_t)(255 - (age * 200) / 20000);
        int bx = cx + (fdCosQ(d.blipAngle) * r) / 127;
        int by = cy + (fdSinQ(d.blipAngle) * r) / 127;
        sprite.fillCircle(bx, by, d.tier >= FD_SUSPICIOUS ? 3 : 2, fdDim(fdTierColor(d.tier), f));
    }
}

// ---------------------------------------------------------------------------
// Scan list
// ---------------------------------------------------------------------------

static void fdBannerText(char *out, size_t cap) {
    if (fdModeIsBle)
        snprintf(
            out, cap, "BLE  C:%u S:%u  %lu fr", (unsigned)fdConfirmedCount,
            (unsigned)fdSuspiciousCount, (unsigned long)fdFramesSeen
        );
    else
        snprintf(
            out, cap, "ch%-2d  C:%u S:%u  %lu fr", fdCurChannel, (unsigned)fdConfirmedCount,
            (unsigned)fdSuspiciousCount, (unsigned long)fdFramesSeen
        );
}

// Build badge, right-aligned on the banner row. Sits above the radar column
// rather than in the list, so it never eats row width.
static void fdDrawVersionBadge() {
    const int w = 6 * (int)strlen(FD_VERSION) + 10;
    const int x = fdContentW() - w;
    if (x < 0) return; // panel too narrow to spare the space
    sprite.drawRoundRect(x, 0, w, 11, 3, fdDim(bruceConfig.priColor, 150));
    sprite.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    sprite.drawString(FD_VERSION, x + 5, 2, 1);
}

// Formats one list row into the caller's buffers. Widths are derived from the
// list column so the same code fits both a 320px and a 240px panel.
static void fdDrawRow(int y, int listW, FdDevice &d, bool selected) {
    const int xDot = 2, xMac = 10, xBar = 58, xRssi = 96, xMethod = 120;
    const int ageW = 24;
    int xAge = listW - ageW;
    int methodW = xAge - xMethod - 4;
    int methodChars = methodW / 6;
    if (methodChars < 0) methodChars = 0;
    if (methodChars > 19) methodChars = 19;

    uint16_t fg = fdTierColor(d.tier);
    uint16_t bg = bruceConfig.bgColor;
    uint32_t now = millis();

    if (selected) {
        sprite.fillRect(0, y - 1, listW, FD_ROW_H, bruceConfig.priColor);
        bg = bruceConfig.priColor;
        fg = bruceConfig.bgColor;
    } else if (now < d.flashUntil) {
        // New-device highlight, fading out over FD_FLASH_MS.
        uint32_t left = d.flashUntil - now;
        uint8_t f = (uint8_t)((left * 200) / FD_FLASH_MS);
        bg = fdDim(fdTierColor(d.tier), f);
        sprite.fillRect(0, y - 1, listW, FD_ROW_H, bg);
        fg = TFT_WHITE;
    }

    // Tier dot: CONFIRMED pulses so it reads at a glance.
    int dotR = 2;
    if (d.tier >= FD_CONFIRMED) dotR = 2 + (fdSinQ(fdFrame * 4) > 0 ? 1 : 0);
    if (d.tier != FD_NONE) sprite.fillCircle(xDot + 2, y + 4, dotR, selected ? fg : fdTierColor(d.tier));

    sprite.setTextColor(fg, bg);

    char macTail[12];
    fdMacTail(macTail, sizeof(macTail), d.mac);
    sprite.drawString(macTail, xMac, y, 1);

    // Eased signal bar.
    int barW = xRssi - xBar - 4;
    int fill = (fdRssiPct(d.dispRssi) * barW) / 100;
    sprite.drawRect(xBar, y + 1, barW, 7, fdDim(fg, 140));
    if (fill > 0) sprite.fillRect(xBar, y + 1, fill, 7, fdTierColor(d.tier));

    char num[8];
    snprintf(num, sizeof(num), "%d", d.rssi);
    sprite.drawString(num, xRssi, y, 1);

    if (methodChars > 0) {
        char m[24];
        strncpy(m, d.method, sizeof(m) - 1);
        m[sizeof(m) - 1] = '\0';
        if ((int)strlen(m) > methodChars) m[methodChars] = '\0';
        sprite.drawString(m, xMethod, y, 1);
    }

    char age[8];
    snprintf(age, sizeof(age), "%us", (unsigned)((now - d.lastSeen) / 1000));
    sprite.drawString(age, xAge, y, 1);
}

// Footer is outside the sprite, so it is cleared explicitly each frame.
static void fdDrawFooter(const char *text) {
    int y = fdFooterY();
    tft.fillRect(FD_LEFT, y, fdContentW(), 9, bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString(text, FD_LEFT, y, 1);
}

static void fdDrawScan() {
    const int H = fdContentH();
    const int listW = fdListW();

    sprite.fillScreen(bruceConfig.bgColor);
    sprite.setTextSize(FP);
    sprite.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);

    char banner[56];
    fdBannerText(banner, sizeof(banner));
    sprite.drawString(banner, 0, 0, 1);
    fdDrawVersionBadge();

    // Channel dwell progress: ties the radar sweep to something real.
    int barW = listW - 4;
    sprite.drawRect(0, 11, barW, 3, fdDim(bruceConfig.priColor, 90));
    if (!fdModeIsBle && barW > 2)
        sprite.fillRect(1, 12, ((barW - 2) * fdDwellPct) / 100, 1, bruceConfig.priColor);

    const int listTop = 18;
    int rows = (H - listTop) / FD_ROW_H;
    if (rows < 1) rows = 1;

    if (fdOrderCount == 0) {
        sprite.setTextColor(TFT_DARKGREY, bruceConfig.bgColor);
        sprite.drawString("listening - no devices yet", 0, listTop + 6, 1);
    } else {
        fdClampList(rows);

        for (int r = 0; r < rows && (fdScrollTop + r) < fdOrderCount; r++) {
            FdDevice &d = fdDevices[fdOrder[fdScrollTop + r]];
            fdDrawRow(listTop + r * FD_ROW_H, listW, d, (fdScrollTop + r) == fdSelected);
        }
    }

    if (fdHasRadar()) {
        int cx = listW + FD_RADAR_W / 2;
        int cy = listTop + (H - listTop) / 2;
        int rad = FD_RADAR_W / 2 - 3;
        int maxR = (H - listTop) / 2 - 2;
        if (rad > maxR) rad = maxR;
        if (rad > 6) fdDrawRadar(cx, cy, rad);
    }

    sprite.pushSprite(FD_LEFT, FD_TOP);

    char foot[64];
    if (fdOrderCount > 0) {
        FdDevice &sd = fdDevices[fdOrder[fdSelected]];
        snprintf(
            foot, sizeof(foot), "peak%ddB x%lu %s", sd.peakRssi, (unsigned long)sd.hits,
            sd.ssid[0] ? sd.ssid : ""
        );
    } else {
        snprintf(foot, sizeof(foot), "SEL:menu  ESC:exit");
    }
    fdDrawFooter(foot);
}

// Reduced renderer for the case where the content sprite could not be
// allocated: same information, no animation, but still no full-screen clear.
static void fdDrawScanPlain() {
    const int H = fdContentH();
    int listTop = FD_TOP + 18;
    int rows = (H - 18) / FD_ROW_H;
    if (rows < 1) rows = 1;

    tft.setTextSize(FP);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    char banner[56];
    fdBannerText(banner, sizeof(banner));
    tft.fillRect(FD_LEFT, FD_TOP, fdContentW(), 10, bruceConfig.bgColor);
    tft.drawString(banner, FD_LEFT, FD_TOP, 1);
    // No sprite here, so the badge is drawn straight to the panel.
    tft.drawString(FD_VERSION, FD_LEFT + fdContentW() - 6 * (int)strlen(FD_VERSION), FD_TOP, 1);

    fdClampList(rows);

    for (int r = 0; r < rows; r++) {
        int y = listTop + r * FD_ROW_H;
        tft.fillRect(FD_LEFT, y - 1, fdContentW(), FD_ROW_H, bruceConfig.bgColor);
        if (fdScrollTop + r >= fdOrderCount) continue;
        FdDevice &d = fdDevices[fdOrder[fdScrollTop + r]];
        bool sel = (fdScrollTop + r) == fdSelected;
        if (sel) tft.fillRect(FD_LEFT, y - 1, fdContentW(), FD_ROW_H, bruceConfig.priColor);
        tft.setTextColor(
            sel ? bruceConfig.bgColor : fdTierColor(d.tier),
            sel ? bruceConfig.priColor : bruceConfig.bgColor
        );
        char macTail[12];
        fdMacTail(macTail, sizeof(macTail), d.mac);
        char line[64];
        snprintf(
            line, sizeof(line), "%02x:%02x:%02x:%s %c %ddB %s", d.mac[0], d.mac[1], d.mac[2], macTail,
            fdTierName(d.tier)[0], d.rssi, d.method
        );
        tft.drawString(line, FD_LEFT, y, 1);
    }
    fdDrawFooter("SEL:menu  ESC:exit");
}

// ---------------------------------------------------------------------------
// Diagnostic pages
// ---------------------------------------------------------------------------
// These exist to answer "which stage is failing" without a serial console.
// Page 1 walks the capture chain, page 2 shows the IE signatures actually
// computed from wildcard probes, page 3 lists every source heard regardless of
// tier (which is how you find a target's real MAC when it never scores).

// Diagnostics render through this one primitive so they work with or without
// the content sprite -- losing the double buffer must not cost us the pages
// that explain a detection failure.
static int fdDiagY = 0;

static void fdDiagLine(const char *s, uint16_t color) {
    if (fdDiagY + 9 > fdContentH()) return;
    if (fdSpriteOk) {
        sprite.setTextColor(color, bruceConfig.bgColor);
        sprite.drawString(s, 0, fdDiagY, 1);
    } else {
        tft.fillRect(FD_LEFT, FD_TOP + fdDiagY, fdContentW(), 10, bruceConfig.bgColor);
        tft.setTextColor(color, bruceConfig.bgColor);
        tft.drawString(s, FD_LEFT, FD_TOP + fdDiagY, 1);
    }
    fdDiagY += 10;
}

static bool fdDiagFull() { return fdDiagY + 9 > fdContentH(); }

static void fdDrawDiagCapture() {
    char b[80];
    fdDiagLine("DIAG 1/3  capture chain  " FD_VERSION, bruceConfig.priColor);

    snprintf(
        b, sizeof(b), "rx all %lu  mgmt %lu  drop %lu", (unsigned long)fdFramesAll,
        (unsigned long)fdFramesSeen, (unsigned long)fdRingDropped
    );
    fdDiagLine(b, fdFramesAll == 0 ? TFT_RED : TFT_WHITE);

    snprintf(
        b, sizeof(b), "rssiRej %lu  ringHigh %lu/%d", (unsigned long)fdRssiRejected,
        (unsigned long)fdRingHigh, FD_RING_SLOTS
    );
    fdDiagLine(b, fdRingDropped > 0 ? TFT_ORANGE : TFT_WHITE);

    snprintf(
        b, sizeof(b), "beacon %lu preq %lu presp %lu", (unsigned long)fdCntBeacon,
        (unsigned long)fdCntProbeReq, (unsigned long)fdCntProbeResp
    );
    fdDiagLine(b, TFT_WHITE);

    snprintf(
        b, sizeof(b), "wildcard %lu  proc %lu  fail %lu", (unsigned long)fdCntWildcard,
        (unsigned long)fdFramesProcessed, (unsigned long)fdParseFailed
    );
    fdDiagLine(b, TFT_WHITE);

    snprintf(
        b, sizeof(b), "cand oui %lu ssid %lu ie %lu", (unsigned long)fdCandOui,
        (unsigned long)fdCandSsid, (unsigned long)fdCandIe
    );
    fdDiagLine(b, (fdCandOui || fdCandSsid || fdCandIe) ? TFT_GREEN : TFT_WHITE);

    snprintf(
        b, sizeof(b), "localMacRej %lu  seen %u", (unsigned long)fdRejLocalMac, (unsigned)fdSeenCount
    );
    fdDiagLine(b, fdRejLocalMac > 0 && fdCandOui == 0 ? TFT_ORANGE : TFT_WHITE);

    if (fdLastSubtype != 0xFF) {
        const char *st = fdLastSubtype == 0x8 ? "beacon" : (fdLastSubtype == 0x4 ? "preq" : "presp");
        snprintf(
            b, sizeof(b), "last %02x:%02x:%02x:%02x:%02x:%02x %ddB %s q%u", fdLastMac[0],
            fdLastMac[1], fdLastMac[2], fdLastMac[3], fdLastMac[4], fdLastMac[5], fdLastRssi, st,
            (unsigned)fdLastSeq
        );
        fdDiagLine(b, TFT_CYAN);
        snprintf(b, sizeof(b), "  \"%s\"", fdLastSsid);
        fdDiagLine(b, TFT_CYAN);
    }
}

// Splits a long signature across lines that fit the content width.
static void fdDiagWrap(const char *s, uint16_t color) {
    int perLine = fdContentW() / 6;
    if (perLine < 8) perLine = 8;
    char chunk[64];
    if (perLine > (int)sizeof(chunk) - 1) perLine = (int)sizeof(chunk) - 1;
    int len = (int)strlen(s);
    for (int off = 0; off < len; off += perLine) {
        int n = len - off < perLine ? len - off : perLine;
        memcpy(chunk, s + off, n);
        chunk[n] = '\0';
        fdDiagLine(chunk, color);
        if (fdDiagFull()) return;
    }
}

static void fdDrawDiagSigs() {
    char b[80];
    snprintf(b, sizeof(b), "DIAG 2/3  IE sigs (%u)  " FD_VERSION, (unsigned)fdSigLogCount);
    fdDiagLine(b, bruceConfig.priColor);

    if (fdSigLogCount == 0) {
        fdDiagLine("no wildcard probes decoded yet.", TFT_DARKGREY);
        fdDiagLine("expected signature:", TFT_DARKGREY);
        fdDiagWrap(FLOCK_PROBE_IE_SIG_PRIMARY, TFT_DARKGREY);
        return;
    }

    if (fdDiagScroll >= fdSigLogCount) fdDiagScroll = fdSigLogCount - 1;
    if (fdDiagScroll < 0) fdDiagScroll = 0;

    for (int i = fdDiagScroll; i < fdSigLogCount; i++) {
        bool match = strcmp(fdSigLog[i].sig, FLOCK_PROBE_IE_SIG_PRIMARY) == 0;
        snprintf(
            b, sizeof(b), "%d) %02x:%02x:%02x x%u %s", i + 1, fdSigLog[i].oui[0], fdSigLog[i].oui[1],
            fdSigLog[i].oui[2], (unsigned)fdSigLog[i].hits, match ? "MATCH" : ""
        );
        fdDiagLine(b, match ? TFT_GREEN : TFT_ORANGE);
        fdDiagWrap(fdSigLog[i].sig, match ? TFT_GREEN : TFT_WHITE);
        if (fdDiagFull()) return;
    }
}

static void fdDrawDiagSeen() {
    char b[80];
    snprintf(b, sizeof(b), "DIAG 3/3  all sources (%u)  " FD_VERSION, (unsigned)fdSeenCount);
    fdDiagLine(b, bruceConfig.priColor);

    if (fdSeenCount == 0) {
        fdDiagLine("nothing heard yet.", TFT_DARKGREY);
        return;
    }
    if (fdDiagScroll >= (int)fdSeenCount) fdDiagScroll = (int)fdSeenCount - 1;
    if (fdDiagScroll < 0) fdDiagScroll = 0;

    static const char *ouiNames[] = {"-", "FLOCK", "COMM", "MFR", "SND", "CAM"};
    for (size_t i = (size_t)fdDiagScroll; i < fdSeenCount; i++) {
        FdSeen &s = fdSeen[i];
        snprintf(
            b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x %c %s %d q%u %s", s.mac[0], s.mac[1],
            s.mac[2], s.mac[3], s.mac[4], s.mac[5], fdTierName(s.tier)[0],
            ouiNames[s.ouiClass <= FD_OUI_CAM ? s.ouiClass : 0], s.rssi, (unsigned)s.seq, s.ssid
        );
        fdDiagLine(b, s.tier != FD_NONE ? fdTierColor(s.tier) : ((s.mac[0] & 0x02) ? TFT_DARKGREY : TFT_WHITE));
        if (fdDiagFull()) return;
    }
}

static void fdDrawDiag() {
    fdDiagY = 0;
    if (fdSpriteOk) {
        sprite.fillScreen(bruceConfig.bgColor);
        sprite.setTextSize(FP);
    } else {
        tft.setTextSize(FP);
    }
    switch (fdDiagPage) {
        case 1: fdDrawDiagCapture(); break;
        case 2: fdDrawDiagSigs(); break;
        default: fdDrawDiagSeen(); break;
    }
    if (fdSpriteOk) {
        sprite.pushSprite(FD_LEFT, FD_TOP);
    } else if (fdDiagY < fdContentH()) {
        // Direct mode clears line by line, so wipe whatever the previous page
        // left below the last line written.
        tft.fillRect(FD_LEFT, FD_TOP + fdDiagY, fdContentW(), fdContentH() - fdDiagY, bruceConfig.bgColor);
    }
    fdDrawFooter("SEL:page  UP/DN:scroll  ESC:back");
}

// ---------------------------------------------------------------------------
// Alert takeover
// ---------------------------------------------------------------------------
// Expanding rings, drawn incrementally: the previous ring is erased by
// redrawing it in the background colour and the text is repainted over the
// top, so no frame needs a fillScreen.

#define FD_ALERT_RINGS 3
static int fdAlertRingPrev[FD_ALERT_RINGS] = {-1, -1, -1};

static void fdAlertText() {
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextSize(FM);
    tft.drawCentreString("FLOCK DETECTED", tftWidth / 2, 12, 1);
    tft.setTextSize(FP);
    char macStr[18];
    snprintf(
        macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x", fdAlertDev.mac[0], fdAlertDev.mac[1],
        fdAlertDev.mac[2], fdAlertDev.mac[3], fdAlertDev.mac[4], fdAlertDev.mac[5]
    );
    tft.drawCentreString(macStr, tftWidth / 2, tftHeight / 2 - 8, 1);
    char info[48];
    if (fdAlertDev.channel > 0)
        snprintf(
            info, sizeof(info), "%s  %ddBm  ch%d", fdAlertDev.method, fdAlertDev.rssi,
            fdAlertDev.channel
        );
    else snprintf(info, sizeof(info), "%s  %ddBm  BLE", fdAlertDev.method, fdAlertDev.rssi);
    tft.drawCentreString(info, tftWidth / 2, tftHeight / 2 + 6, 1);
    tft.drawCentreString("press any key", tftWidth / 2, tftHeight - 14, 1);
}

static void fdDrawAlert() {
    const int cx = tftWidth / 2, cy = tftHeight / 2;
    const int maxR = tftWidth / 2;

    if (!fdAlertPainted) {
        tft.fillScreen(TFT_RED);
        for (int k = 0; k < FD_ALERT_RINGS; k++) fdAlertRingPrev[k] = -1;
        fdAlertPainted = true;
    }

    uint32_t t = millis() - fdAlertStart;
    for (int k = 0; k < FD_ALERT_RINGS; k++) {
        if (fdAlertRingPrev[k] >= 0) tft.drawCircle(cx, cy, fdAlertRingPrev[k], TFT_RED);
        uint32_t phase = (t + (uint32_t)k * 400) % 1200;
        int r = (int)((phase * maxR) / 1200);
        if (r > 2) {
            tft.drawCircle(cx, cy, r, r > maxR * 2 / 3 ? TFT_ORANGE : TFT_WHITE);
            fdAlertRingPrev[k] = r;
        } else {
            fdAlertRingPrev[k] = -1;
        }
    }

    // Repaint the text over any ring pixels that crossed it.
    fdAlertText();

    // Border flash, alternating each frame pair.
    uint16_t bc = ((t / 130) & 1) ? TFT_WHITE : TFT_YELLOW;
    tft.drawRect(2, 2, tftWidth - 4, tftHeight - 4, bc);
    tft.drawRect(3, 3, tftWidth - 6, tftHeight - 6, bc);
}

// ---------------------------------------------------------------------------

static void fdClearList() {
    fdDeviceCount = 0;
    fdConfirmedCount = 0;
    fdSuspiciousCount = 0;
    fdHiddenCount = 0;
    fdHiddenNext = 0;
    fdSelected = 0;
    fdScrollTop = 0;
    fdOrderCount = 0;
    fdSeenCount = 0;
    fdSigLogCount = 0;
    fdSigLogNext = 0;
    fdDiagScroll = 0;
}

// Nested submenu: pick which channels the sweep covers. Option::selected marks
// the active preset, matching how the rest of Bruce shows current state.
static void fdChannelMenu() {
    std::vector<Option> opts;
    for (uint8_t i = 0; i < FD_CH_PRESETS; i++) {
        // Sweep time is what actually matters to the operator, so show it.
        char label[40];
        snprintf(
            label, sizeof(label), "%s  (%.1fs)", FD_CH_SETS[i].label,
            (double)FD_CH_SETS[i].count * FD_DWELL_MS / 1000.0
        );
        Option o = {String(label), [i]() { fdChannelPreset = i; }};
        o.selected = (i == fdChannelPreset);
        opts.push_back(o);
    }
    loopOptions(opts, MENU_TYPE_SUBMENU, "Scan channels");
}

static void fdOptionsMenu() {
    std::vector<Option> opts;
    opts.push_back({"Diagnostics", [&]() {
                        fdDiagPage = 1;
                        fdDiagScroll = 0;
                    }});
    opts.push_back({String("Scan ch: ") + FD_CH_SETS[fdChannelPreset].label, [&]() {
                        fdChannelMenu();
                    }});
    opts.push_back({flock_link_active() ? "Phone link: ON" : "Phone link: OFF", [&]() {
                        if (flock_link_active()) flock_link_stop();
                        else flock_link_start();
                    }});
    opts.push_back({fdSoundEnabled ? "Sound: ON" : "Sound: OFF", [&]() { fdSoundEnabled = !fdSoundEnabled; }});
#ifdef HAS_RGB_LED
    opts.push_back({fdLedEnabled ? "LED: ON" : "LED: OFF", [&]() { fdLedEnabled = !fdLedEnabled; }});
#endif
    opts.push_back({fdLogEnabled ? "SD Log: ON" : "SD Log: OFF", [&]() {
                        fdLogEnabled = !fdLogEnabled;
                        if (fdLogEnabled && fdLogPath == "") fdLogInit();
                    }});
    opts.push_back({fdGpsEnabled ? "GPS: ON" : "GPS: OFF", [&]() {
                        fdGpsEnabled = !fdGpsEnabled;
                        if (fdGpsEnabled) fdGpsStart();
                        else fdGpsStop();
                    }});
    opts.push_back({"RSSI floor -5", [&]() {
                        if (fdRssiMin > -100) fdRssiMin -= 5;
                    }});
    opts.push_back({"RSSI floor +5", [&]() {
                        if (fdRssiMin < -40) fdRssiMin += 5;
                    }});
    opts.push_back({"Clear list", [&]() { fdClearList(); }});
    opts.push_back({"Exit FlockDetect", [&]() { returnToMenu = true; }});
    loopOptions(opts, MENU_TYPE_SUBMENU, "FlockDetect " FD_VERSION);
}

// Shared per-iteration UI handling for both scan modes. Returns:
//   0 = continue, 1 = exit requested, 2 = open options menu.
//
// The list data is rebuilt at FD_DATA_MS and the frame is drawn at FD_FRAME_MS;
// splitting the two is what stops rows churning while still allowing smooth
// animation.
static int fdUiTick(uint32_t &lastFrame, uint32_t &lastData) {
    uint32_t now = millis();

    if (check(EscPress)) {
        if (fdAlertActive) fdClearAlert();
        else if (fdDiagPage != 0) {
            fdDiagPage = 0;
            fdDiagScroll = 0;
            lastFrame = 0;
        } else return 1;
    }
    if (check(SelPress)) {
        if (fdAlertActive) fdClearAlert();
        else if (fdDiagPage != 0) {
            fdDiagPage = (uint8_t)(fdDiagPage % FD_DIAG_PAGES + 1); // wraps 3 -> 1
            fdDiagScroll = 0;
        } else return 2;
    }
    if (check(NextPress)) {
        if (fdDiagPage != 0) fdDiagScroll++;
        else if (fdDeviceCount > 0) fdSelected++;
    }
    if (check(PrevPress)) {
        if (fdDiagPage != 0) {
            if (fdDiagScroll > 0) fdDiagScroll--;
        } else if (fdDeviceCount > 0) fdSelected--;
    }

    if (fdAlertActive && now >= fdAlertUntil) fdClearAlert();

    if (fdAlertActive) {
        if (now - lastFrame >= FD_FRAME_MS) {
            fdDrawAlert();
            lastFrame = now;
        }
        return 0;
    }

    // An alert just ended: the takeover trashed the chrome, so repaint it.
    if (fdAlertPainted) {
        fdAlertPainted = false;
        fdDrawChrome();
        lastFrame = 0;
    }

    if (now - lastData >= FD_DATA_MS) {
        fdRebuildOrder();
        lastData = now;
        // The chrome is no longer repainted per frame, so refresh the status
        // bar occasionally to keep the clock and battery live. Every 5s keeps
        // the battery I2C read off the animation path.
        static uint32_t lastStatus = 0;
        if (now - lastStatus >= 5000) {
            drawStatusBar();
            tft.setTextSize(FP);
            lastStatus = now;
        }
    }

    if (now - lastFrame >= FD_FRAME_MS) {
        fdFrame++;
        if (fdSpriteOk) {
            fdAnimate();
            if (fdDiagPage != 0) fdDrawDiag();
            else fdDrawScan();
        } else if (fdDiagPage != 0) {
            fdDrawDiag();
        } else {
            fdDrawScanPlain();
        }
        lastFrame = now;
    }
    return 0;
}

// ============================================================================
// BLE DETECTION  (Raven sensors, XUNTONG mfr ID, Flock names/OUIs)
// ============================================================================
// Ported from flock-you-wifi-recon's BLE path (MIT). Raven acoustic gunshot
// sensors advertise a family of custom service UUIDs; Flock battery/camera
// gear advertises Flock-family names and the XUNTONG (0x09C8) manufacturer ID.

// Raven service UUIDs (short 16-bit forms; NimBLE renders these as the full
// 128-bit base UUID, so we match on the 8-hex prefix).
static const char *FD_RAVEN_UUID_PREFIX[] = {
    "0000180a", // Device Info (weak)
    "00001809", // old Health (weak)
    "00001819", // old Location (weak)
    "00003100", // GPS      (custom, strong)
    "00003200", // Power    (custom, strong)
    "00003300", // Network  (custom, strong)
    "00003400", // Upload   (custom, strong)
    "00003500", // Error    (custom, strong)
};
static const uint16_t FD_BLE_MFR_XUNTONG = 0x09C8;
static const char *FD_BLE_NAMES[] = {"FS Ext Battery", "Penguin", "Flock", "Pigvision"};

// Estimate Raven firmware generation from which services it exposes.
static const char *fdRavenFw(bool hasNewGps, bool hasOldLoc, bool hasPower) {
    if (hasOldLoc && !hasNewGps) return "1.1.x";
    if (hasNewGps && !hasPower) return "1.2.x";
    if (hasNewGps && hasPower) return "1.3.x";
    return "?";
}

// BLE hit queue: NimBLE's onResult runs on the host task, so it only classifies
// and enqueues; the UI loop drains and calls fdRecord (which drives the TFT /
// LED / tone alert).
struct FdBleHit {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t tier;
    char method[20];
    char name[24];
};
#define FD_BLE_QUEUE 24
static FdBleHit fdBleQueue[FD_BLE_QUEUE];
static volatile uint8_t fdBleQHead = 0;
static volatile uint8_t fdBleQTail = 0;
static portMUX_TYPE fdBleMux = portMUX_INITIALIZER_UNLOCKED;

static bool fdParseMac(const char *s, uint8_t out[6]) {
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

class FdBleCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        int rssi = dev->getRSSI();
        if (rssi < fdRssiMin) return;

        std::string addr = dev->getAddress().toString();
        uint8_t mac[6];
        if (!fdParseMac(addr.c_str(), mac)) return;

        uint8_t tier = FD_NONE;
        char method[20] = {0};
        char name[24] = {0};
        std::string devName = dev->haveName() ? dev->getName() : std::string();
        if (!devName.empty()) {
            strncpy(name, devName.c_str(), sizeof(name) - 1);
        }

        // 1. Raven service UUIDs (strongest BLE signal).
        if (dev->haveServiceUUID()) {
            bool hasNewGps = false, hasOldLoc = false, hasPower = false, strong = false, weak = false;
            int cnt = dev->getServiceUUIDCount();
            for (int i = 0; i < cnt; i++) {
                std::string u = dev->getServiceUUID(i).toString();
                for (size_t j = 0; j < sizeof(FD_RAVEN_UUID_PREFIX) / sizeof(FD_RAVEN_UUID_PREFIX[0]); j++) {
                    if (strncasecmp(u.c_str(), FD_RAVEN_UUID_PREFIX[j], 8) == 0) {
                        // The first three are weak standard UUIDs; the rest custom.
                        if (j < 3) weak = true;
                        else strong = true;
                        if (strncasecmp(u.c_str(), "00003100", 8) == 0) hasNewGps = true;
                        if (strncasecmp(u.c_str(), "00001819", 8) == 0) hasOldLoc = true;
                        if (strncasecmp(u.c_str(), "00003200", 8) == 0) hasPower = true;
                    }
                }
            }
            if (strong) {
                tier = FD_CONFIRMED;
                snprintf(method, sizeof(method), "raven_%s", fdRavenFw(hasNewGps, hasOldLoc, hasPower));
            } else if (weak && tier < FD_INFO) {
                tier = FD_INFO;
                snprintf(method, sizeof(method), "raven_uuid?");
            }
        }

        // 2. Flock-family BLE device name.
        //
        // Exact match only for CONFIRMED. flock-back's docs are explicit that the
        // advertised local_name equals the product name; a bare substring test
        // promoted anything merely containing "Flock" -- a speaker named "Flocking
        // Awesome" included -- straight to the top tier. Substring hits are kept,
        // but as SUSPICIOUS, so the wider net costs no confidence.
        if (!devName.empty()) {
            for (size_t i = 0; i < sizeof(FD_BLE_NAMES) / sizeof(FD_BLE_NAMES[0]); i++) {
                if (strcasecmp(devName.c_str(), FD_BLE_NAMES[i]) == 0) {
                    if (tier < FD_CONFIRMED) {
                        tier = FD_CONFIRMED;
                        snprintf(method, sizeof(method), "ble_name");
                    }
                    break;
                }
                if (fdContainsCI(devName.c_str(), devName.size(), FD_BLE_NAMES[i])) {
                    // Never downgrade a stronger signal (e.g. a Raven UUID hit).
                    if (tier < FD_SUSPICIOUS) {
                        tier = FD_SUSPICIOUS;
                        snprintf(method, sizeof(method), "ble_name?");
                    }
                    // Keep scanning: a later entry may still match exactly.
                }
            }
        }

        // 3. XUNTONG manufacturer company ID (from wgreenberg/flock-you).
        if (tier < FD_SUSPICIOUS && dev->getManufacturerDataCount() > 0) {
            std::string md = dev->getManufacturerData(0);
            if (md.size() >= 2) {
                uint16_t code = ((uint16_t)(uint8_t)md[1] << 8) | (uint16_t)(uint8_t)md[0];
                if (code == FD_BLE_MFR_XUNTONG) {
                    tier = FD_SUSPICIOUS;
                    snprintf(method, sizeof(method), "ble_mfr_xuntong");
                }
            }
        }

        // 4. OUI tables (shared with the WiFi path).
        if (tier < FD_SUSPICIOUS) {
            const char *vendor = nullptr;
            FdOuiClass oc = fdClassifyOui(mac, &vendor);
            if (oc == FD_OUI_HIGH) {
                tier = FD_SUSPICIOUS;
                snprintf(method, sizeof(method), "oui_flock");
            } else if (oc == FD_OUI_COMMUNITY || oc == FD_OUI_MFR) {
                tier = FD_SUSPICIOUS;
                snprintf(method, sizeof(method), "oui_flock?");
            } else if (oc == FD_OUI_SOUND) {
                tier = FD_INFO;
                snprintf(method, sizeof(method), "oui_soundthinking");
            } else if (oc == FD_OUI_CAM) {
                tier = FD_INFO;
                snprintf(method, sizeof(method), "cam:%s", vendor ? vendor : "?");
            }
        }

        if (tier == FD_NONE) return;

        portENTER_CRITICAL(&fdBleMux);
        uint8_t next = (fdBleQHead + 1) % FD_BLE_QUEUE;
        if (next != fdBleQTail) {
            FdBleHit &h = fdBleQueue[fdBleQHead];
            memcpy(h.mac, mac, 6);
            h.rssi = (int8_t)rssi;
            h.tier = tier;
            strncpy(h.method, method, sizeof(h.method) - 1);
            h.method[sizeof(h.method) - 1] = '\0';
            strncpy(h.name, name, sizeof(h.name) - 1);
            h.name[sizeof(h.name) - 1] = '\0';
            fdBleQHead = next;
        }
        portEXIT_CRITICAL(&fdBleMux);
    }
};

static FdBleCallbacks fdBleCallbacks;

static void fdDrainBleQueue() {
    for (;;) {
        FdBleHit h;
        bool have = false;
        portENTER_CRITICAL(&fdBleMux);
        if (fdBleQTail != fdBleQHead) {
            h = fdBleQueue[fdBleQTail];
            fdBleQTail = (fdBleQTail + 1) % FD_BLE_QUEUE;
            have = true;
        }
        portEXIT_CRITICAL(&fdBleMux);
        if (!have) break;
        // channel 0 marks a BLE detection.
        fdRecord(h.mac, h.tier, h.method, h.name, h.rssi, 0);
    }
}

// ============================================================================
// ENTRY POINT
// ============================================================================

static void fdResetSession() {
    returnToMenu = false;
    fdClearList();
    fdFramesSeen = 0;
    fdRingHead = 0;
    fdRingTail = 0;
    fdRingDropped = 0;
    fdBleQHead = 0;
    fdBleQTail = 0;
    fdAlertActive = false;
    fdAlertPainted = false;
    fdRssiMin = FD_RSSI_MIN_DEFAULT;
    fdFs = nullptr;
    fdLogPath = "";
    fdDiagLogPath = "";

    // Diagnostic counters are per-session: a stale count from a previous run
    // would be worse than no number at all.
    fdFramesAll = 0;
    fdRssiRejected = 0;
    fdRingHigh = 0;
    fdFramesProcessed = 0;
    fdParseFailed = 0;
    fdCntBeacon = fdCntProbeReq = fdCntProbeResp = fdCntWildcard = 0;
    fdCandOui = fdCandSsid = fdCandIe = fdRejLocalMac = 0;
    fdLastSubtype = 0xFF;
    fdLastSsid[0] = '\0';
    fdDiagPage = 0;
    fdDwellPct = 0;
    fdFrame = 0;

    fdInitLut();
}

void flock_detect_setup() {
    fdModeIsBle = false;
    fdResetSession();

    fd_start_wifi();
#ifdef HAS_RGB_LED
    beginLed();
#endif
    if (fdLogEnabled) fdLogInit();
    if (fdGpsEnabled) fdGpsStart();

    fdSpriteBegin();
    fdDrawChrome();

    int chIdx = 0;
    uint32_t lastFrame = 0;
    uint32_t lastData = 0;
    bool needChrome = false;

    while (!returnToMenu) {
        uint8_t ch = fdChannels()[chIdx];
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        fdCurChannel = ch;

        uint32_t t0 = millis();
        uint32_t elapsed = 0;
        while ((elapsed = millis() - t0) < FD_DWELL_MS) {
            if (returnToMenu) break;
            fdDwellPct = (uint16_t)((elapsed * 100) / FD_DWELL_MS);

            // Detection + GPS. Drained on both sides of the redraw so a frame
            // that takes a few milliseconds cannot let the ring back up.
            fdDrainRing();
            fdGpsPoll();
            if (flock_link_active()) {
                flock_link_set_status(
                    fdCurChannel, fdFramesAll, fdFramesSeen, fdRingDropped, fdConfirmedCount,
                    fdSuspiciousCount
                );
                flock_link_tick();
            }

            int act = fdUiTick(lastFrame, lastData);
            fdDrainRing();

            if (act == 1) {
                returnToMenu = true;
                break;
            }
            if (act == 2) {
                fdOptionsMenu();
                needChrome = true;
                break;
            }

            vTaskDelay(5 / portTICK_PERIOD_MS);
        }

        if (needChrome) {
            fdDrawChrome();
            lastFrame = 0;
            needChrome = false;
        }
        chIdx = (chIdx + 1) % fdChannelCount();
    }

    fdSpriteEnd();
    flock_link_stop();
    fd_stop_wifi();
    fdGpsStop();
#ifdef HAS_RGB_LED
    setLedColor(CRGB::Black);
#endif
}

// BLE scan mode: shares the tracker / tiers / alert / logging / UI with the
// WiFi mode, but the data source is a NimBLE active scan (no WiFi promiscuous,
// so the single 2.4GHz radio isn't contended).
void flock_detect_ble_setup() {
    fdModeIsBle = true;
    fdResetSession();

    if (!ble_scan_setup() || pBLEScan == nullptr) {
        displayError("BLE init failed", true);
        return;
    }
    pBLEScan->setScanCallbacks(&fdBleCallbacks);
    pBLEScan->setMaxResults(0); // callback-only; don't buffer results
    pBLEScan->start(0, false);  // 0 = scan continuously

#ifdef HAS_RGB_LED
    beginLed();
#endif
    if (fdLogEnabled) fdLogInit();

    fdSpriteBegin();
    fdDrawChrome();

    uint32_t lastFrame = 0;
    uint32_t lastData = 0;

    while (!returnToMenu) {
        fdDrainBleQueue();

        int act = fdUiTick(lastFrame, lastData);
        if (act == 1) {
            returnToMenu = true;
            break;
        }
        if (act == 2) {
            fdOptionsMenu();
            fdDrawChrome();
            lastFrame = 0;
        }

        // Keep the scan alive if it ended.
        if (!pBLEScan->isScanning()) pBLEScan->start(0, false);

        vTaskDelay(5 / portTICK_PERIOD_MS);
    }

    fdSpriteEnd();
    pBLEScan->stop();
    stopBLEStack();
#ifdef HAS_RGB_LED
    setLedColor(CRGB::Black);
#endif
}

#endif // !LITE_VERSION
