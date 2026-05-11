/*
 * BLE Prop Sniffer — Bomb The Bus
 * ESP32 WROOM-30
 *
 * Serial Monitor (115200): startup message + per-detection lines
 * Serial Plotter (115200): live RSSI graph for all 3 props
 *   — absent prop shows -100 so the line drops clearly
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ── Config ────────────────────────────────────────────────────────────────────
#define NUM_PROPS      3
#define ABSENT_RSSI   -100
#define TIMEOUT_MS    2000
#define PLOT_INTERVAL  200
#define EMA_ALPHA      0.2f   // smoothing: lower = smoother but slower to react

const char* PROP_NAME[NUM_PROPS] = { "BTB01", "BTB02", "BTB03" };

// ── Prop state (written from BLE task, read from loop) ────────────────────────
struct PropState {
    float             smoothed;   // EMA-filtered RSSI
    volatile uint32_t lastSeen;
} props[NUM_PROPS];

// ── BLE scan callback ─────────────────────────────────────────────────────────
class ScanCallback : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (!dev.haveName()) return;
        String name = dev.getName().c_str();
        for (int i = 0; i < NUM_PROPS; i++) {
            if (name == PROP_NAME[i]) {
                float raw = (float)dev.getRSSI();
                // Seed filter on first sight, then apply EMA
                if (props[i].lastSeen == 0)
                    props[i].smoothed = raw;
                else
                    props[i].smoothed = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * props[i].smoothed;
                props[i].lastSeen = millis();
                return;
            }
        }
    }
};

ScanCallback scanCallback;

// ── BLE scan task (runs continuously on core 0) ───────────────────────────────
void scanTask(void*) {
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&scanCallback, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    while (true) {
        scan->clearResults();
        scan->start(1, false);          // 1-second blocking scan, then repeat
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("=== BLE Prop Sniffer ===");
    Serial.println("Watching for BTB01 / BTB02 / BTB03");

    for (int i = 0; i < NUM_PROPS; i++) {
        props[i].smoothed = ABSENT_RSSI;
        props[i].lastSeen = 0;
    }

    BLEDevice::init("");
    xTaskCreatePinnedToCore(scanTask, "BLEScan", 4096, NULL, 1, NULL, 0);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastPlot = 0;
    uint32_t now = millis();
    if (now - lastPlot < PLOT_INTERVAL) return;
    lastPlot = now;

    // Age out props not seen recently
    for (int i = 0; i < NUM_PROPS; i++) {
        if (props[i].lastSeen > 0 && now - props[i].lastSeen > TIMEOUT_MS) {
            props[i].smoothed = ABSENT_RSSI;
            props[i].lastSeen = 0;
        }
    }

    // Plotter output — anchors pin the Y axis to -100..0
    for (int i = 0; i < NUM_PROPS; i++)
        Serial.printf("%s:%.1f ", PROP_NAME[i], props[i].smoothed);
    Serial.println("_min:-100 _max:0");
}
