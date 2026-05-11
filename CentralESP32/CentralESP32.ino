/*
 * Bomb The Bus — Central Controller
 * Hardware: ESP32 WROOM-30
 *
 * Pins:
 *   GPIO 13  — WS2812B LED strip data
 *   GPIO 16  — DFPlayer Mini RX  (Serial1 TX on ESP32)
 *   GPIO 17  — DFPlayer Mini TX  (Serial1 RX on ESP32)
 *   GPIO 25  — DFPlayer Mini BUSY (LOW while playing)
 *   GPIO 26  — Defusal button A (active LOW, internal pull-up)
 *   GPIO 27  — Defusal button B (active LOW, internal pull-up)
 *
 * Required libraries (Arduino Library Manager):
 *   - FastLED                (by Daniel Garcia)
 *   - DFRobotDFPlayerMini    (by DFRobot)
 *   - ESP32 BLE Arduino      (built-in with ESP32 board package)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <FastLED.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include "../../2026 - AudioLibrary/audio_library.h"

// ╔══════════════════════════════════════════════════════════════════════════════╗
// ║  SETTINGS — edit here, nowhere else                                         ║
// ╚══════════════════════════════════════════════════════════════════════════════╝

// ── Pin assignments ───────────────────────────────────────────────────────────
#define LED_PIN         13    // WS2812B data
#define DF_RX_PIN       16    // DFPlayer TX  → ESP32 Serial1 RX
#define DF_TX_PIN       17    // DFPlayer RX  ← ESP32 Serial1 TX
#define BUSY_PIN        25    // DFPlayer BUSY (LOW while playing)
#define DEFUSE_BTN_A    26    // defusal button A (active LOW)
#define DEFUSE_BTN_B    27    // defusal button B (active LOW)

// ── LED strip ─────────────────────────────────────────────────────────────────
#define NUM_LEDS       512   // maximum / array size — runtime count set via cfg_num_leds
#define BRIGHTNESS      80    // global brightness 0–255

// Zone colors (one per prop) — tweak to taste
#define ZONE_COLOR_0  CRGB(255,  20,   0)   // zone 1 — red-orange
#define ZONE_COLOR_1  CRGB(  0, 200, 255)   // zone 2 — electric cyan
#define ZONE_COLOR_2  CRGB(180,   0, 255)   // zone 3 — violet

// Chase chasers: starts at MIN, grows to MAX as countdown progresses
#define CHASE_MIN       1
#define CHASE_MAX       6

// ── Audio ─────────────────────────────────────────────────────────────────────
#define AUDIO_VOLUME   25     // DFPlayer volume 0–30
#define AUDIO_MIN_GAP_MS  4000  // minimum ms between clip triggers (covers longest clip)

// ── BLE / prop detection ──────────────────────────────────────────────────────
#define RSSI_THRESHOLD  -60   // dBm — smoothed RSSI must be above this to count as present
#define EMA_ALPHA       0.2f  // RSSI smoothing factor (0 = no smoothing, 1 = instant)
#define PROP_TIMEOUT_MS 2000  // ms without a BLE packet before prop is considered gone

// ── Game timing ───────────────────────────────────────────────────────────────
#define ARMING_DELAY_MS    (5UL  * 1000UL)   // delay between all props present → ARMED
#define DETONATE_AFTER_MS  (1UL  * 60000UL)  // countdown duration (5 minutes)
#define DEFUSE_DURATION_MS (10UL * 1000UL)   // how long BOTH defusal buttons must be held simultaneously
#define DETONATE_EFFECT_MS (5UL  * 1000UL)   // explosion flash duration after detonation

// ╚══════════════════════════════════════════════════════════════════════════════╝

// ── Runtime-configurable settings (loaded from NVS; fall back to #define defaults above) ──
Preferences          prefs;
volatile bool        menuActive = false;   // suppresses BLE debug output during settings menu
uint32_t cfg_arming_delay_ms  = ARMING_DELAY_MS;
uint32_t cfg_countdown_ms     = DETONATE_AFTER_MS;
uint32_t cfg_defuse_hold_ms   = DEFUSE_DURATION_MS;
uint8_t  cfg_volume           = AUDIO_VOLUME;
uint8_t  cfg_brightness       = BRIGHTNESS;
int8_t   cfg_rssi_threshold   = RSSI_THRESHOLD;
uint16_t cfg_num_leds         = 50;   // default active LED count (≤ NUM_LEDS)

// ── Game state (declared early so Arduino prototype generator sees it) ────────
enum class State : uint8_t { WAITING, ARMING, ARMED, DEFUSING, DEFUSED, DETONATED };
State    gameState      = State::WAITING;
uint32_t armedAt        = 0;
uint32_t armingAt       = 0;
uint32_t detonatedAt    = 0;
uint32_t defusingStartMs = 0;

// ── LED ───────────────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

const CRGB ZONE_COLOR[3] = { ZONE_COLOR_0, ZONE_COLOR_1, ZONE_COLOR_2 };

inline void setZone(uint8_t zone, CRGB color) {
    uint16_t zoneSize = cfg_num_leds / 3;
    int start = zone * zoneSize;
    int end   = (zone == 2) ? cfg_num_leds : start + zoneSize;
    for (int i = start; i < end; i++) leds[i] = color;
}

// ── Audio ─────────────────────────────────────────────────────────────────────
HardwareSerial       dfSerial(1);
DFRobotDFPlayerMini  dfPlayer;
AirsoftAudio         audio(dfPlayer);

bool     audioMilestone[9] = {};   // 10 %–90 % fired flags
bool     audioWillDetonate  = false;
uint32_t audioLastMs        = 0;   // millis() of last play call

static const AudioClip MILESTONE_CLIP[9] = {
    AudioClip::BOMD_CHARGED_TO_10_PERCENT_I_REPEAT_BOMB_CHARGED_TO_10_PERCENT,
    AudioClip::BOMD_CHARGED_TO_20_PERCENT_I_REPEAT_BOMB_CHARGED_TO_20_PERCENT,
    AudioClip::BOMD_CHARGED_TO_30_PERCENT_I_REPEAT_BOMB_CHARGED_TO_30_PERCENT,
    AudioClip::BOMD_CHARGED_TO_40_PERCENT_I_REPEAT_BOMB_CHARGED_TO_40_PERCENT,
    AudioClip::BOMD_CHARGED_TO_50_PERCENT_I_REPEAT_BOMB_CHARGED_TO_50_PERCENT,
    AudioClip::BOMD_CHARGED_TO_60_PERCENT_I_REPEAT_BOMB_CHARGED_TO_60_PERCENT,
    AudioClip::BOMD_CHARGED_TO_70_PERCENT_I_REPEAT_BOMB_CHARGED_TO_70_PERCENT,
    AudioClip::BOMD_CHARGED_TO_80_PERCENT_I_REPEAT_BOMB_CHARGED_TO_80_PERCENT,
    AudioClip::BOMD_CHARGED_TO_90_PERCENT_I_REPEAT_BOMB_CHARGED_TO_90_PERCENT,
};

// Announce the current charge level regardless of audio gap — used on defusal abort
void announceCurrentCharge(uint32_t now) {
    float progress = min(1.0f, (float)(now - armedAt) / cfg_countdown_ms);
    if (progress >= 0.95f) {
        safePlay(AudioClip::BOMD_WILL_DETONATE);
    } else {
        int mIdx = (int)(progress * 10.0f) - 1;
        if (mIdx >= 0 && mIdx <= 8) safePlay(MILESTONE_CLIP[mIdx]);
    }
}

inline bool isPlaying() { return digitalRead(BUSY_PIN) == LOW; }

inline bool audioReady() { return (millis() - audioLastMs) >= AUDIO_MIN_GAP_MS; }

void safePlay(AudioClip clip) {
    Serial.printf("[AUDIO] play  clip=%d  file=%d\n", (int)clip, _AUDIO_BASE[(int)clip]);
    audio.play(clip);
    audioLastMs = millis();
}
void safePlayRandom(AudioClip clip) {
    Serial.printf("[AUDIO] playR clip=%d  base=%d  vars=%d\n",
        (int)clip, _AUDIO_BASE[(int)clip], _AUDIO_VARIANTS[(int)clip]);
    audio.playRandom(clip);
    audioLastMs = millis();
}

// ── BLE prop detection ────────────────────────────────────────────────────────
#define NUM_PROPS  3

const char* PROP_NAME[NUM_PROPS] = { "BTB01", "BTB02", "BTB03" };

struct PropState {
    float             smoothed;
    volatile uint32_t lastSeen;
    bool              wasPresent;   // tracks last known presence for change detection
} props[NUM_PROPS];

inline bool propPresent(int i) {
    return props[i].lastSeen > 0
        && (millis() - props[i].lastSeen) < PROP_TIMEOUT_MS
        && props[i].smoothed >= cfg_rssi_threshold;
}

// ── BLE defusal device detection ──────────────────────────────────────────────
#define DEFUSAL_TIMEOUT_MS   2000

#define MFR_COMPANY_LO   0xCD
#define MFR_COMPANY_HI   0xAB
#define MFR_TYPE_CENTRAL  0x01
#define MFR_TYPE_DEFUSAL  0x02

#define DEFUSAL_STATE_IDLE     0x00
#define DEFUSAL_STATE_ACTIVE   0x01
#define DEFUSAL_STATE_COMPLETE 0x02

struct DefusalState {
    volatile uint32_t lastSeen;
    volatile uint8_t  state;
} defusal = { 0, DEFUSAL_STATE_IDLE };

inline bool defusalPresent() {
    return defusal.lastSeen > 0
        && (millis() - defusal.lastSeen) < DEFUSAL_TIMEOUT_MS;
}

// ── BLE scan callback ─────────────────────────────────────────────────────────
class ScanCallback : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        // Props: detect by name
        if (dev.haveName()) {
            String name = dev.getName().c_str();
            for (int i = 0; i < NUM_PROPS; i++) {
                if (name == PROP_NAME[i]) {
                    float raw = (float)dev.getRSSI();
                    if (props[i].lastSeen == 0)
                        props[i].smoothed = raw;
                    else
                        props[i].smoothed = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * props[i].smoothed;
                    props[i].lastSeen = millis();
                    if (!menuActive)
                        Serial.printf("[BLE] %-6s  raw %4d  smooth %6.1f  %s\n",
                            PROP_NAME[i], (int)raw, props[i].smoothed,
                            propPresent(i) ? "PRESENT" : "weak");
                    return;
                }
            }

            // Defusal device detection — disabled for now
            // if (name == "BTB-Defusal") { ... }
        }
    }
};

ScanCallback scanCallback;

// ── BLE scan task ─────────────────────────────────────────────────────────────
void scanTask(void*) {
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&scanCallback, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    while (true) {
        scan->clearResults();
        scan->start(1, false);
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ── Chase effect ──────────────────────────────────────────────────────────────
uint16_t chasePos    = 0;
uint32_t lastChaseMs = 0;

// ── LED update ────────────────────────────────────────────────────────────────
void updateLEDs() {
    uint32_t now = millis();

    switch (gameState) {

        case State::WAITING: {
            float   raw   = beatsin8(25, 0, 255) / 255.0f;
            uint8_t pulse = (uint8_t)(pow(raw, 2.5f) * 220.0f);
            for (int z = 0; z < 3; z++) {
                CRGB c = propPresent(z) ? ZONE_COLOR[z] : CRGB(6, 6, 6);
                c.nscale8(propPresent(z) ? max((uint8_t)8, pulse) : 255);
                setZone(z, c);
            }
            FastLED.show();
            break;
        }

        case State::ARMING: {
            // All zones solid in their colors, brightness builds from 0 to full over the delay
            float   progress = min(1.0f, (float)(now - armingAt) / cfg_arming_delay_ms);
            uint8_t brightness = (uint8_t)(progress * 255.0f);
            for (int z = 0; z < 3; z++) {
                CRGB c = ZONE_COLOR[z]; c.nscale8(max((uint8_t)8, brightness));
                setZone(z, c);
            }
            FastLED.show();
            break;
        }

        case State::ARMED: {
            float    progress = min(1.0f, (float)(now - armedAt) / cfg_countdown_ms);
            uint32_t stepMs   = (uint32_t)(20.0f - 17.0f * progress);  // 20ms → 3ms
            int      count    = CHASE_MIN + (int)(progress * (CHASE_MAX - CHASE_MIN));
            if (now - lastChaseMs >= stepMs) {
                lastChaseMs = now;
                fill_solid(leds, cfg_num_leds, CRGB(8, 0, 0));
                uint16_t spacing = cfg_num_leds / count;
                for (int c = 0; c < count; c++) {
                    uint16_t h = (chasePos + c * spacing) % cfg_num_leds;
                    leds[h]                                    = CRGB(255, 200, 0);
                    leds[(h + 1) % cfg_num_leds]               = CRGB(180,  80, 0);
                    leds[(h + cfg_num_leds - 1) % cfg_num_leds] = CRGB( 80,  20, 0);
                }
                chasePos++;
                FastLED.show();
            }
            break;
        }

        case State::DEFUSING: {
            float progress = (defusingStartMs > 0)
                ? min(1.0f, (float)(now - defusingStartMs) / cfg_defuse_hold_ms)
                : 0.0f;
            int filled = (int)(progress * cfg_num_leds);
            for (int i = 0; i < cfg_num_leds; i++) {
                if (i < filled)
                    leds[i] = CRGB(0, 180, 255);
                else if (i == filled)
                    leds[i] = CRGB(180, 255, 255);  // bright leading edge
                else
                    leds[i] = CRGB(0, 0, 8);
            }
            FastLED.show();
            break;
        }

        case State::DEFUSED:
            fill_solid(leds, cfg_num_leds, CRGB(0, 200, 0));
            FastLED.show();
            break;

        case State::DETONATED:
            if (now - detonatedAt < DETONATE_EFFECT_MS)
                fill_solid(leds, cfg_num_leds, ((now / 100) % 2) ? CRGB(255, 0, 0) : CRGB::Black);
            else
                fill_solid(leds, cfg_num_leds, CRGB(20, 0, 0));  // dim red — game over
            FastLED.show();
            break;
    }
}

// ── Reset ─────────────────────────────────────────────────────────────────────
void resetGame() {
    gameState   = State::WAITING;
    armedAt     = 0;
    armingAt    = 0;
    detonatedAt = 0;
    chasePos    = 0;
    for (int i = 0; i < NUM_PROPS; i++) {
        props[i].smoothed   = -100.0f;
        props[i].lastSeen   = 0;
        props[i].wasPresent = false;
    }
    defusal.lastSeen  = 0;
    defusal.state     = DEFUSAL_STATE_IDLE;
    defusingStartMs   = 0;
    memset(audioMilestone, 0, sizeof(audioMilestone));
    audioWillDetonate = false;
    audioLastMs       = 0;
    fill_solid(leds, cfg_num_leds, CRGB::Black);
    FastLED.show();
    Serial.println("--- GAME RESET ---");
}

// ── Status print ──────────────────────────────────────────────────────────────
const char* stateName(State s) {
    switch (s) {
        case State::WAITING:   return "WAITING";
        case State::ARMING:    return "ARMING";
        case State::ARMED:     return "ARMED";
        case State::DEFUSING:  return "DEFUSING";
        case State::DEFUSED:   return "DEFUSED";
        case State::DETONATED: return "DETONATED";
        default:               return "?";
    }
}

void printStatus() {
    Serial.printf("[STATUS] %-9s |", stateName(gameState));
    for (int i = 0; i < NUM_PROPS; i++) {
        bool p = propPresent(i);
        if (props[i].lastSeen == 0)
            Serial.printf("  %s:  --- ", PROP_NAME[i]);
        else
            Serial.printf("  %s:%5.1f%s", PROP_NAME[i], props[i].smoothed, p ? "* " : "  ");
    }
    if (gameState == State::ARMED) {
        uint32_t rem = cfg_countdown_ms - (millis() - armedAt);
        Serial.printf("| T-%lus", rem / 1000);
    }
    Serial.println();
}

// ── Central advertisement — broadcasts game state so defusal LCD can read it ──
void updateCentralAdvertisement() {
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->stop();
    BLEAdvertisementData adData;
    adData.setFlags(0x06);
    String mfr;
    mfr += (char)MFR_COMPANY_LO;
    mfr += (char)MFR_COMPANY_HI;
    mfr += (char)MFR_TYPE_CENTRAL;
    mfr += (char)(uint8_t)gameState;
    adData.setManufacturerData(mfr);
    BLEAdvertisementData scanData;
    scanData.setName("BTB-Central");
    adv->setAdvertisementData(adData);
    adv->setScanResponseData(scanData);
    adv->start();
}

// ── NVS settings persistence ──────────────────────────────────────────────────
void loadSettings() {
    prefs.begin("btb", true);
    cfg_arming_delay_ms = prefs.getUInt("arm_delay",   ARMING_DELAY_MS);
    cfg_countdown_ms    = prefs.getUInt("countdown",   DETONATE_AFTER_MS);
    cfg_defuse_hold_ms  = prefs.getUInt("defuse",      DEFUSE_DURATION_MS);
    cfg_volume          = prefs.getUChar("volume",     AUDIO_VOLUME);
    cfg_brightness      = prefs.getUChar("brightness", BRIGHTNESS);
    cfg_rssi_threshold  = (int8_t)prefs.getInt("rssi", RSSI_THRESHOLD);
    cfg_num_leds        = prefs.getUShort("num_leds", 50);
    prefs.end();
    Serial.printf("[CFG] arm=%lus  countdown=%lus  defuse=%lus  vol=%u  bright=%u  rssi=%d  leds=%u\n",
        cfg_arming_delay_ms / 1000UL, cfg_countdown_ms / 1000UL, cfg_defuse_hold_ms / 1000UL,
        cfg_volume, cfg_brightness, (int)cfg_rssi_threshold, cfg_num_leds);
}

void saveSettings() {
    prefs.begin("btb", false);
    prefs.putUInt("arm_delay",   cfg_arming_delay_ms);
    prefs.putUInt("countdown",   cfg_countdown_ms);
    prefs.putUInt("defuse",      cfg_defuse_hold_ms);
    prefs.putUChar("volume",     cfg_volume);
    prefs.putUChar("brightness", cfg_brightness);
    prefs.putInt("rssi",         (int)cfg_rssi_threshold);
    prefs.putUShort("num_leds",  cfg_num_leds);
    prefs.end();
    Serial.println("[CFG] Saved to flash.");
}

void settingsMenu() {
    menuActive = true;
    Serial.setTimeout(30000);
    bool dirty = false;

    while (true) {
        Serial.println();
        Serial.println("=== BTB Settings (set line ending to Newline) ===");
        Serial.printf("  1. Arming delay   : %4lu s   (1-300)\n",   cfg_arming_delay_ms / 1000UL);
        Serial.printf("  2. Countdown      : %4lu s   (10-3600)\n", cfg_countdown_ms    / 1000UL);
        Serial.printf("  3. Defuse hold    : %4lu s   (1-300)\n",   cfg_defuse_hold_ms  / 1000UL);
        Serial.printf("  4. Audio volume   : %4u      (0-30)\n",    cfg_volume);
        Serial.printf("  5. LED brightness : %4u      (0-255)\n",   cfg_brightness);
        Serial.printf("  6. RSSI threshold : %4d dBm  (-100-0)\n",  (int)cfg_rssi_threshold);
        Serial.printf("  7. LED count      : %4u      (1-%d)\n",     cfg_num_leds, NUM_LEDS);
        Serial.println("  S. Save to flash");
        Serial.println("  X. Exit (unsaved changes lost)");
        Serial.print("Option: ");

        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        char opt = toupper((char)line[0]);

        if (opt == 'X') break;

        if (opt == 'S') {
            saveSettings();
            dirty = false;
            break;
        }

        int n = opt - '0';
        if (n < 1 || n > 7) { Serial.println("Invalid."); continue; }

        Serial.print("New value: ");
        String val = Serial.readStringUntil('\n');
        val.trim();
        if (val.length() == 0) { Serial.println("Cancelled."); continue; }
        long v = val.toInt();

        switch (n) {
            case 1: cfg_arming_delay_ms = (uint32_t)constrain(v, 1,    300)  * 1000UL; break;
            case 2: cfg_countdown_ms    = (uint32_t)constrain(v, 10,  3600)  * 1000UL; break;
            case 3: cfg_defuse_hold_ms  = (uint32_t)constrain(v, 1,    300)  * 1000UL; break;
            case 4:
                cfg_volume = (uint8_t)constrain(v, 0, 30);
                dfPlayer.volume(cfg_volume);
                break;
            case 5:
                cfg_brightness = (uint8_t)constrain(v, 0, 255);
                FastLED.setBrightness(cfg_brightness);
                FastLED.show();
                break;
            case 6: cfg_rssi_threshold = (int8_t)constrain(v, -100, 0); break;
            case 7:
                cfg_num_leds = (uint16_t)constrain(v, 1, NUM_LEDS);
                break;
        }
        dirty = true;
        Serial.println("OK.");
    }

    if (dirty) Serial.println("[CFG] Changes not saved — send M, pick S to save.");
    Serial.setTimeout(1000);
    menuActive = false;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Bomb The Bus — Central Controller ===");
    randomSeed(esp_random());
    loadSettings();

    for (int i = 0; i < NUM_PROPS; i++) {
        props[i].smoothed   = -100.0f;
        props[i].lastSeen   = 0;
        props[i].wasPresent = false;
    }

    FastLED.addLeds<WS2812B, LED_PIN, RGB>(leds, NUM_LEDS);
    FastLED.setBrightness(cfg_brightness);
    fill_solid(leds, cfg_num_leds, CRGB::Black);
    FastLED.show();
    Serial.printf("LED strip: %u LEDs on GPIO %d (max %d)\n", cfg_num_leds, LED_PIN, NUM_LEDS);

    // Startup sweep: each zone fades in then out in sequence
    for (int z = 0; z < 3; z++) {
        for (int v = 0; v < 256; v += 4) {
            CRGB c = ZONE_COLOR[z]; c.nscale8(v);
            setZone(z, c); FastLED.show(); delay(6);
        }
        for (int v = 255; v >= 0; v -= 4) {
            CRGB c = ZONE_COLOR[z]; c.nscale8(v);
            setZone(z, c); FastLED.show(); delay(6);
        }
    }
    fill_solid(leds, cfg_num_leds, CRGB::Black);
    FastLED.show();

    // DFPlayer
    pinMode(BUSY_PIN, INPUT);
    dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
    delay(1000);
    bool dfOK = dfPlayer.begin(dfSerial, false);  // false = no ACK
    Serial.printf("DFPlayer: begin=%s  BUSY=%d\n", dfOK ? "OK" : "FAILED", digitalRead(BUSY_PIN));
    dfPlayer.volume(cfg_volume);
    delay(50);
    dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
    delay(50);

    pinMode(DEFUSE_BTN_A, INPUT_PULLUP);
    pinMode(DEFUSE_BTN_B, INPUT_PULLUP);
    Serial.printf("Defusal buttons: A=GPIO%d  B=GPIO%d  hold %lus\n",
        DEFUSE_BTN_A, DEFUSE_BTN_B, cfg_defuse_hold_ms / 1000UL);

    BLEDevice::init("BTB-Central");
    updateCentralAdvertisement();
    xTaskCreatePinnedToCore(scanTask, "BLEScan", 4096, NULL, 1, NULL, 0);
    Serial.println("BLE: scanning");
    Serial.println("Waiting for props...");
    Serial.println("Send 'R' to reset, 'M' for settings menu.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static State    prevState  = State::WAITING;
    static uint32_t lastStatus = 0;
    uint32_t now = millis();

    // Serial commands
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'R' || c == 'r') resetGame();
        if (c == 'M' || c == 'm') {
            if (gameState == State::WAITING) settingsMenu();
            else Serial.println("[CFG] Reset game first (R) before opening settings.");
        }
    }

    // Periodic status line
    if (now - lastStatus >= 2000) {
        lastStatus = now;
        printStatus();
    }

    // Prop presence change alerts
    static const AudioClip PLANTED_CLIP[3] = {
        AudioClip::BOMB_ALPHA_HAS_BEEN_PLANTED,
        AudioClip::BOMB_BRAVO_HAS_BEEN_PLANTED,
        AudioClip::BOMB_CHARLIE_HAS_BEEN_PLANTED,
    };
    for (int i = 0; i < NUM_PROPS; i++) {
        bool p = propPresent(i);
        if (p != props[i].wasPresent) {
            Serial.printf("[PROP] %s %s\n", PROP_NAME[i], p ? "ARRIVED" : "GONE");
            props[i].wasPresent = p;
            if (p && gameState == State::WAITING && audioReady()) safePlayRandom(PLANTED_CLIP[i]);
        }
    }

    // ── Defusal buttons — read once, edge-detect for rapid-press reset ──────────
    static bool     lastBtnA   = false;
    static bool     lastBtnB   = false;
    static uint8_t  rapidCount = 0;
    static uint32_t rapidFirst = 0;
    bool curBtnA = (digitalRead(DEFUSE_BTN_A) == LOW);
    bool curBtnB = (digitalRead(DEFUSE_BTN_B) == LOW);
    if ((curBtnA && !lastBtnA) || (curBtnB && !lastBtnB)) {
        if (rapidCount == 0 || now - rapidFirst > 3000) {
            rapidCount = 1;
            rapidFirst = now;
        } else if (++rapidCount >= 8) {
            resetGame();
            rapidCount = 0;
        }
    }
    lastBtnA = curBtnA;
    lastBtnB = curBtnB;

    switch (gameState) {

        case State::WAITING:
            if (propPresent(0) && propPresent(1) && propPresent(2)) {
                gameState = State::ARMING;
                armingAt  = now;
                Serial.println(">>> All props present — ARMING <<<");
            }
            break;

        case State::ARMING:
            if (!(propPresent(0) && propPresent(1) && propPresent(2))) {
                gameState = State::WAITING;
                Serial.println("Prop lost during arming — back to WAITING");
            } else if (now - armingAt >= cfg_arming_delay_ms) {
                gameState = State::ARMED;
                armedAt   = now;
                safePlayRandom(AudioClip::THE_BOMB_IS_ARMED_I_REPEAT_THE_BOMB_IS_ARMED);
                Serial.println(">>> ARMED <<<");
            }
            break;

        case State::ARMED: {
            if (now - armedAt >= cfg_countdown_ms) {
                gameState   = State::DETONATED;
                detonatedAt = now;
                safePlay(AudioClip::BOMB_HAS_DETONATED_THE_BOMB_HAS_DETONATED_THE_ATTACKING_TEAM_WINS);
                Serial.println(">>> DETONATED <<<");
                break;
            }
            // Countdown audio milestones: 10 % … 90 %, then "will detonate" at 95 %
            {
                float progress = min(1.0f, (float)(now - armedAt) / cfg_countdown_ms);
                int   mIdx     = (int)(progress * 10.0f) - 1;   // 0–8 at 10 %–90 %
                if (mIdx >= 0 && mIdx <= 8 && !audioMilestone[mIdx]) {
                    if (audioReady()) safePlay(MILESTONE_CLIP[mIdx]);
                    audioMilestone[mIdx] = true;
                }
                if (progress >= 0.95f && !audioWillDetonate) {
                    if (audioReady()) safePlay(AudioClip::BOMD_WILL_DETONATE);
                    audioWillDetonate = true;
                }
            }
            // Two-button simultaneous defusal
            if (curBtnA && curBtnB) {
                gameState        = State::DEFUSING;
                defusingStartMs  = now;
                safePlay(AudioClip::BOMB_IS_BEING_DEFUSED);
                Serial.println(">>> DEFUSING — hold both buttons <<<");
            }
            break;
        }

        case State::DEFUSING: {
            if (!curBtnA || !curBtnB) {
                gameState       = State::ARMED;
                defusingStartMs = 0;
                safePlay(AudioClip::DEFUSAL_ATTEMPT_CANCELLED);
                Serial.println("Defusal aborted — button released");
            } else if (now - defusingStartMs >= cfg_defuse_hold_ms) {
                gameState = State::DEFUSED;
                safePlay(AudioClip::BOMB_HAS_BEEN_DEFUSED_THE_BOMB_HAS_BEEN_DEFUSED_THE_DEFENDING_TEAM_WINS);
                Serial.println(">>> DEFUSED <<<");
            }
            break;
        }

        case State::DEFUSED:
            break;

        case State::DETONATED:
            break;
    }

    if (gameState != prevState) {
        updateCentralAdvertisement();
        Serial.printf("State: %d → %d\n", (int)prevState, (int)gameState);
        prevState = gameState;
    }

    updateLEDs();
}
