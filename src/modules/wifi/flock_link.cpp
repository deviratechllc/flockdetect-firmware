#if !defined(LITE_VERSION)
// ============================================================================
// FlockLink - BLE detection stream to a paired phone. See flock_link.h for the
// design rationale and the protocol's provenance.
// ============================================================================

#include "flock_link.h"

#include "core/display.h"
#include "modules/ble/ble_common.h"
#include <NimBLEDevice.h>
#include <globals.h>

// --- Nordic UART Service ----------------------------------------------------
// The de-facto standard BLE serial transport, and what CYD-Flock-You's app
// scans for. 128-bit UUIDs, so they cost 16 bytes in an advertisement.
#define FL_SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define FL_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // phone -> device
#define FL_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // device -> phone

#define FL_DEVICE_NAME "FlockDetect"
#define FL_PROTOCOL_VERSION 1
#define FL_STATUS_INTERVAL_MS 5000
#define FL_GPS_FRESH_MS 10000 // their semantics: a fix is stale after 10s

// Status mirrored in from flock_detect via flock_link_set_status(), rather than
// externing its file-statics -- the counters stay private to that module and
// the phone sees the same numbers as diagnostics page 1.
static uint8_t flChannel = 0;
static uint32_t flRxAll = 0, flRxMgmt = 0, flDrops = 0;
static uint32_t flConfirmed = 0, flSuspicious = 0;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static NimBLEServer *flServer = nullptr;
static NimBLEService *flService = nullptr;
static NimBLECharacteristic *flTx = nullptr;
static NimBLECharacteristic *flRx = nullptr;
static bool flActive = false;
static volatile bool flConnected = false;
static uint32_t flLastStatus = 0;
static uint32_t flDetections = 0;

// Freshest phone fix.
static double flLat = 0, flLon = 0;
static float flAcc = 0, flSpeed = 0, flCourse = 0;
static uint32_t flGpsAt = 0;
static bool flGpsValid = false;

// Commands arrive on the BLE host task; the loop drains them.
#define FL_CMD_QUEUE 8
#define FL_CMD_MAX 96
static char flCmdQueue[FL_CMD_QUEUE][FL_CMD_MAX];
static volatile uint8_t flCmdHead = 0;
static volatile uint8_t flCmdTail = 0;
static portMUX_TYPE flCmdMux = portMUX_INITIALIZER_UNLOCKED;

static bool flPendingSim = false;

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

// One newline-terminated line. bleNotifyRetry handles a busy stack; without a
// connection this is a no-op so callers never need to check.
static void flSendLine(const char *line) {
    if (!flActive || !flConnected || flTx == nullptr) return;
    size_t len = strlen(line);
    static uint8_t buf[320];
    if (len > sizeof(buf) - 2) len = sizeof(buf) - 2;
    memcpy(buf, line, len);
    buf[len] = '\n';
    bleNotifyRetry(flTx, buf, len + 1);
}

// Appends the shared "gps" object, or nothing when no fresh fix is held.
static int flAppendGps(char *out, size_t cap) {
    if (!flGpsValid) return 0;
    uint32_t age = millis() - flGpsAt;
    if (age > FL_GPS_FRESH_MS) return 0;
    return snprintf(
        out, cap, ",\"gps\":{\"latitude\":%.6f,\"longitude\":%.6f,\"accuracy\":%.1f,"
                  "\"age_ms\":%lu,\"source\":\"phone\"}",
        flLat, flLon, flAcc, (unsigned long)age
    );
}

static void flSendPairStatus() {
    char line[420];
    int n = snprintf(
        line, sizeof(line),
        "{\"event\":\"pair_status\",\"device\":\"%s\",\"protocol_version\":%d,"
        "\"features\":[\"wifi_promisc\",\"phone_gps\",\"sd_csv\",\"tft_status\",\"ble_uart\"],"
        "\"gps\":%s,\"sd\":%s,\"detections\":%lu,\"csv_rows\":%lu,"
        "\"scan_mode\":\"ch%u\",\"channel\":%u,\"rx_frames\":%lu,\"rx_mgmt\":%lu,"
        "\"rx_data\":0,\"queue_drops\":%lu,\"confirmed\":%lu,\"suspicious\":%lu}",
        FL_DEVICE_NAME, FL_PROTOCOL_VERSION, flGpsValid ? "true" : "false",
        sdcardMounted ? "true" : "false", (unsigned long)flDetections,
        (unsigned long)flDetections, (unsigned)flChannel, (unsigned)flChannel,
        (unsigned long)flRxAll, (unsigned long)flRxMgmt, (unsigned long)flDrops,
        (unsigned long)flConfirmed, (unsigned long)flSuspicious
    );
    if (n > 0) flSendLine(line);
}

// 2.4GHz channel centre frequency. Channel 14 breaks the arithmetic sequence.
static uint16_t flFreqForChannel(uint8_t ch) {
    if (ch == 0) return 0;
    if (ch == 14) return 2484;
    return (uint16_t)(2407 + ch * 5);
}

void flock_link_send_detection(
    const uint8_t *mac, uint8_t tier, const char *method, const char *ssid, int8_t rssi,
    int8_t peakRssi, uint8_t channel, uint16_t seq, uint32_t hits, const char *ieSig, bool isBle
) {
    if (!flActive || !flConnected) return;

    char line[480];
    int n = snprintf(
        line, sizeof(line),
        "{\"event\":\"detection\",\"detection_method\":\"%s\",\"protocol\":\"%s\","
        "\"mac_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"oui\":\"%02x:%02x:%02x\","
        "\"device_name\":\"\",\"rssi\":%d,\"channel\":%u,\"frequency\":%u,\"ssid\":\"%s\""
        // Extra keys past this point are ours; a CYD-protocol client ignores them.
        ",\"tier\":\"%s\",\"seq\":%u,\"peak_rssi\":%d,\"hits\":%lu,\"ie_sig\":\"%s\"",
        method ? method : "", isBle ? "ble" : "wifi_2_4ghz", mac[0], mac[1], mac[2], mac[3],
        mac[4], mac[5], mac[0], mac[1], mac[2], (int)rssi, (unsigned)channel,
        (unsigned)flFreqForChannel(channel), ssid ? ssid : "",
        tier == 3 ? "CONFIRMED" : (tier == 2 ? "SUSPICIOUS" : "INFO"), (unsigned)seq,
        (int)peakRssi, (unsigned long)hits, ieSig ? ieSig : ""
    );
    if (n < 0 || (size_t)n >= sizeof(line)) return;

    n += flAppendGps(line + n, sizeof(line) - n);
    if ((size_t)n + 2 < sizeof(line)) {
        line[n++] = '}';
        line[n] = '\0';
        flDetections++;
        flSendLine(line);
    }
}

bool flock_link_gps(double &lat, double &lon, float &accuracy, uint32_t &ageMs) {
    if (!flGpsValid) return false;
    uint32_t age = millis() - flGpsAt;
    if (age > FL_GPS_FRESH_MS) return false;
    lat = flLat;
    lon = flLon;
    accuracy = flAcc;
    ageMs = age;
    return true;
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

// FYGPS,<lat>,<lon>,<acc>,<speed>,<course>,<sats>,<hdop>[,<unix>,<utc_off>]
// Both the short and extended forms are accepted, per the protocol doc.
static void flParseGps(const char *body) {
    double lat = 0, lon = 0;
    float acc = 0, spd = 0, crs = 0;
    int got = sscanf(body, "%lf,%lf,%f,%f,%f", &lat, &lon, &acc, &spd, &crs);
    if (got < 2) return;
    if (lat == 0.0 && lon == 0.0) return; // null island is not a fix
    flLat = lat;
    flLon = lon;
    flAcc = (got >= 3) ? acc : 0.0f;
    flSpeed = (got >= 4) ? spd : 0.0f;
    flCourse = (got >= 5) ? crs : 0.0f;
    flGpsAt = millis();
    flGpsValid = true;
}

static void flHandleCommand(const char *cmd) {
    if (strncmp(cmd, "FYHELLO", 7) == 0) {
        flSendPairStatus();
    } else if (strncmp(cmd, "FYGPS,", 6) == 0) {
        flParseGps(cmd + 6);
    } else if (strncmp(cmd, "FYSIM", 5) == 0) {
        // Bench hook: exercises the phone's whole review flow indoors with no
        // Flock hardware in range. Deliberately marked so it can never be
        // mistaken for a real RF hit.
        flPendingSim = true;
    }
    // '$'-prefixed raw NMEA is accepted by the protocol but not parsed here;
    // the app is expected to prefer FYGPS.
}

class FlRxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        std::string v = c->getValue();
        // Writes may batch several newline-delimited commands.
        size_t start = 0;
        while (start < v.size()) {
            size_t nl = v.find('\n', start);
            size_t end = (nl == std::string::npos) ? v.size() : nl;
            size_t len = end - start;
            while (len > 0 && (v[start + len - 1] == '\r' || v[start + len - 1] == ' ')) len--;
            if (len > 0 && len < FL_CMD_MAX) {
                portENTER_CRITICAL(&flCmdMux);
                uint8_t next = (flCmdHead + 1) % FL_CMD_QUEUE;
                if (next != flCmdTail) {
                    memcpy(flCmdQueue[flCmdHead], v.data() + start, len);
                    flCmdQueue[flCmdHead][len] = '\0';
                    flCmdHead = next;
                }
                portEXIT_CRITICAL(&flCmdMux);
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
};

class FlServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
        flConnected = true;
        // Same latency tuning BLE_API applies.
        s->updateConnParams(info.getConnHandle(), 6, 24, 0, 400);
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override { flConnected = false; }
};

static FlRxCallbacks *flRxCb = nullptr;
static FlServerCallbacks *flSrvCb = nullptr;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool flock_link_start() {
    if (flActive) return true;

    NimBLEDevice::init(FL_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    flServer = NimBLEDevice::createServer();
    if (!flServer) return false;
    flServer->advertiseOnDisconnect(true);
    flSrvCb = new FlServerCallbacks();
    flServer->setCallbacks(flSrvCb);

    flService = flServer->createService(FL_SVC_UUID);
    flTx = flService->createCharacteristic(FL_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    flRx = flService->createCharacteristic(
        FL_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    flRxCb = new FlRxCallbacks();
    flRx->setCallbacks(flRxCb);
    // No explicit service start: this NimBLE starts services with the server,
    // which advertising start triggers.

    NimBLEAdvertising *adv = flServer->getAdvertising();
    adv->addServiceUUID(flService->getUUID());
    // A 128-bit UUID eats 16 of the 31 advertisement bytes, leaving no room for
    // an 11-character name -- so the name goes in the scan response. BLE_API
    // sidesteps this by shortening "Bruce" to "Bruc"; we want the full name
    // because the phone matches on it.
    adv->enableScanResponse(true);
    adv->setName(FL_DEVICE_NAME);
    adv->start();

    flActive = true;
    flConnected = false;
    flLastStatus = 0;
    flCmdHead = 0;
    flCmdTail = 0;
    return true;
}

void flock_link_stop() {
    if (!flActive) return;
    flActive = false;
    flConnected = false;
    if (flServer) {
        NimBLEAdvertising *adv = flServer->getAdvertising();
        if (adv) adv->stop();
    }
    NimBLEDevice::deinit(true);
    flServer = nullptr;
    flService = nullptr;
    flTx = flRx = nullptr;
    delete flRxCb;
    flRxCb = nullptr;
    delete flSrvCb;
    flSrvCb = nullptr;
    flGpsValid = false;
}

void flock_link_set_status(
    uint8_t channel, uint32_t rxAll, uint32_t rxMgmt, uint32_t drops, uint32_t confirmed,
    uint32_t suspicious
) {
    flChannel = channel;
    flRxAll = rxAll;
    flRxMgmt = rxMgmt;
    flDrops = drops;
    flConfirmed = confirmed;
    flSuspicious = suspicious;
}

bool flock_link_active() { return flActive; }
bool flock_link_connected() { return flActive && flConnected; }

void flock_link_tick() {
    if (!flActive) return;

    for (;;) {
        char cmd[FL_CMD_MAX];
        bool have = false;
        portENTER_CRITICAL(&flCmdMux);
        if (flCmdTail != flCmdHead) {
            memcpy(cmd, flCmdQueue[flCmdTail], FL_CMD_MAX);
            flCmdTail = (flCmdTail + 1) % FL_CMD_QUEUE;
            have = true;
        }
        portEXIT_CRITICAL(&flCmdMux);
        if (!have) break;
        flHandleCommand(cmd);
    }

    if (flPendingSim) {
        flPendingSim = false;
        static const uint8_t simMac[6] = {0x70, 0xc9, 0x4e, 0xde, 0xad, 0xbe};
        flock_link_send_detection(
            simMac, 3, "sim_bench_test", "", -55, -55, flChannel, 0, 1, "", false
        );
    }

    uint32_t now = millis();
    if (flConnected && now - flLastStatus >= FL_STATUS_INTERVAL_MS) {
        flLastStatus = now;
        flSendPairStatus();
    }
}

#endif // !LITE_VERSION
