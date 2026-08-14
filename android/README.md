# FlockDetect companion app

Android app that pairs with a FlockDetect board over Bluetooth LE, puts each
detection on a map, and lets you verify it by hand before exporting it for
[DeFlock](https://deflock.me) / OpenStreetMap.

The split is deliberate: **the device detects, the phone locates and reviews.** A
phone's GNSS receiver beats anything you'd solder onto the board, and every
coordinate gets a human look before it leaves the app.

## Flow

1. Power the board, open `WiFi → FlockDetect`, and turn on `SEL → Phone link`.
2. Hit **Connect**. The app scans for the Nordic UART service, subscribes, and
   starts streaming the phone's GPS to the device once a second.
3. A detection arrives stamped with the phone's current fix — that is, with
   **your** position on the road, not the camera's.
4. Tap the marker, **drag it onto the actual camera**, set which way it looks,
   approve.
5. **Export** approved candidates as GeoJSON (tagged for JOSM), CSV or GPX.

**Test it indoors:** hit **Test**, which sends `FYSIM`. The device replies with a
synthetic detection so the whole review path can be exercised with no Flock
hardware in range. It is a transport test, not evidence of an RF hit.

## Protocol

Compatible with CYD-Flock-You's DeFlock pairing protocol — Nordic UART Service,
newline-delimited JSON out, plain command lines in. `Protocol.kt` is the whole
contract, and it accepts boards advertising as either `FlockDetect` or
`CYD-Flock-You`.

Our device adds `tier`, `seq`, `peak_rssi`, `hits` and `ie_sig` as extra keys; a
CYD board simply omits them and the parser falls back to sane defaults, which is
covered by a test.

## Design notes

- **`Protocol.LineAssembler` exists because a GATT notification is capped at
  MTU−3 bytes.** A detection carrying a GPS block routinely straddles two
  notifications; anything parsing a notification as a whole message silently
  drops exactly the detections you care about. The app also requests a 247-byte
  MTU to make this rarer.
- **Dedupe uses two different rules.** The same MAC is the same unit however far
  the fix drifted. A *different* MAC within 76.2 m (their 250 ft) is treated as
  the same physical installation — which is what stops one camera becoming five
  pins as you drive past, and what MAC randomisation would otherwise cause.
- **A stronger sighting wins the position.** On a repeat hit with higher peak
  RSSI, the candidate moves there: nearer signal means nearer camera.
- **Direction is never inferred from travel heading.** That would record which
  way you drove, not which way the camera looks.
- **Export, not upload.** No OAuth, no changesets. A file you inspect first
  cannot quietly put bad geometry into a public map.

## Building

```sh
cd android && ./gradlew assembleDebug     # app/build/outputs/apk/debug/app-debug.apk
./gradlew testDebugUnitTest               # protocol tests, no device needed
```

Needs JDK 17 and an Android SDK with platform 34 / build-tools 34. Point
`local.properties` at it (`sdk.dir=/path/to/android-sdk`); that file is
gitignored since the path is machine-specific.

`Protocol.kt` and `CandidateStore.kt` are free of Android dependencies precisely
so the framing, parsing and dedupe logic can be tested on the JVM — that is the
only part verifiable without a phone and a board.
