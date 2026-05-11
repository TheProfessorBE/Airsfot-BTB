/*
 * Bomb The Bus — Defusal Device
 * Hardware: ESP32 WROOM-30
 *
 * Pins:
 *   GPIO 21  — I2C SDA  (LCD)
 *   GPIO 22  — I2C SCL  (LCD)
 *   GPIO  0  — Defusal button (boot button, active LOW; change if using another pin)
 *
 * LCD: 20x4 or 16x4 I2C module with PCF8574 controller
 *   Default I2C address: 0x27 — if display stays blank try 0x3F
 *
 * Required libraries (Arduino Library Manager):
 *   - LiquidCrystal I2C   (by Frank de Brabander)
 *   - ESP32 BLE Arduino   (built-in with ESP32 board package)
 *
 * BLE manufacturer data format sent to central:
 *   Byte 0-1: company ID 0xCD 0xAB (little-endian 0xABCD)
 *   Byte 2:   device type 0x02 (defusal)
 *   Byte 3:   state  0x00=idle  0x01=button held  0x02=defused
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── Hardware ──────────────────────────────────────────────────────────────────
#define BUTTON_PIN       0     // active LOW
#define LCD_ADDR        0x27   // try 0x3F if blank
#define LCD_COLS          20
#define LCD_ROWS           4

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ── BLE ───────────────────────────────────────────────────────────────────────
// Must match CentralESP32.ino
#define DEFUSAL_SVC_UUID "dead0001-beef-cafe-babe-c0ffee000001"

#define MFR_COMPANY_LO   0xCD
#define MFR_COMPANY_HI   0xAB
#define MFR_TYPE_CENTRAL  0x01   // must match CentralESP32.ino
#define MFR_TYPE_DEFUSAL  0x02

// Bomb states broadcast by central (matches State enum in CentralESP32.ino)
#define BOMB_WAITING    0
#define BOMB_ARMING     1
#define BOMB_ARMED      2
#define BOMB_DEFUSING   3
#define BOMB_DEFUSED    4
#define BOMB_DETONATED  5
#define BOMB_UNKNOWN  0xFF

#define CENTRAL_TIMEOUT_MS  3000
#define EMA_ALPHA           0.2f

struct {
    float             rssi;
    volatile uint32_t lastSeen;
    volatile uint8_t  state;
} central = { -100.0f, 0, BOMB_UNKNOWN };

inline bool centralPresent() {
    return central.lastSeen > 0 && (millis() - central.lastSeen) < CENTRAL_TIMEOUT_MS;
}

// ── Central scan callback ─────────────────────────────────────────────────────
class CentralScanCallback : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (!dev.haveName()) return;
        if (String(dev.getName().c_str()) != "BTB-Central") return;
        float raw = (float)dev.getRSSI();
        central.rssi     = (central.lastSeen == 0) ? raw
                         : EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * central.rssi;
        central.lastSeen = millis();
        if (dev.haveManufacturerData()) {
            String mfr = dev.getManufacturerData();
            if (mfr.length() >= 4
                && (uint8_t)mfr[0] == MFR_COMPANY_LO
                && (uint8_t)mfr[1] == MFR_COMPANY_HI
                && (uint8_t)mfr[2] == MFR_TYPE_CENTRAL) {
                central.state = (uint8_t)mfr[3];
            }
        }
    }
};
CentralScanCallback centralCallback;

// ── Central scan task ─────────────────────────────────────────────────────────
void centralScanTask(void*) {
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&centralCallback, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    while (true) {
        scan->clearResults();
        scan->start(1, false);
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

#define STATE_IDLE     0x00
#define STATE_ACTIVE   0x01
#define STATE_COMPLETE 0x02

BLEAdvertising* bleAdv = nullptr;
uint8_t         currentBLEState = STATE_IDLE;

// ── Defusal config ────────────────────────────────────────────────────────────
const uint32_t HOLD_DURATION_MS = 10000;  // hold button this long to defuse

// ── Device state ──────────────────────────────────────────────────────────────
enum class DevState : uint8_t { IDLE, DEFUSING, DEFUSED, FAILED };
DevState devState = DevState::IDLE;
uint32_t holdStart = 0;

// ── BLE advertisement helpers ─────────────────────────────────────────────────
void setBLEState(uint8_t state) {
    if (state == currentBLEState) return;
    currentBLEState = state;

    bleAdv->stop();

    BLEAdvertisementData adData;
    adData.setFlags(0x06);  // BR/EDR not supported, LE general discoverable

    // Manufacturer data: [company LE 2B][type 1B][state 1B]
    String mfr;
    mfr += (char)MFR_COMPANY_LO;
    mfr += (char)MFR_COMPANY_HI;
    mfr += (char)MFR_TYPE_DEFUSAL;
    mfr += (char)state;
    adData.setManufacturerData(std::string(mfr.c_str(), mfr.length()));

    BLEAdvertisementData scanData;
    scanData.setName("BTB-Defusal");
    BLEUUID svcUUID(DEFUSAL_SVC_UUID);
    scanData.setCompleteServices(svcUUID);

    bleAdv->setAdvertisementData(adData);
    bleAdv->setScanResponseData(scanData);
    bleAdv->start();
}

// ── LCD helpers ───────────────────────────────────────────────────────────────
void lcdLine(uint8_t row, const char* text) {
    lcd.setCursor(0, row);
    // Pad to full width so previous content is cleared
    char buf[LCD_COLS + 1];
    snprintf(buf, sizeof(buf), "%-*s", LCD_COLS, text);
    lcd.print(buf);
}

void showIdle() {
    char sig[LCD_COLS + 1];
    snprintf(sig, sizeof(sig), " Signal:%4.0f dBm   ", central.rssi);

    if (!centralPresent()) {
        lcdLine(0, "  BOMB THE BUS  ");
        lcdLine(1, "~~~~~~~~~~~~~~~~");
        lcdLine(2, "  Searching...  ");
        lcdLine(3, "                ");
        return;
    }
    switch (central.state) {
        case BOMB_WAITING:
            lcdLine(0, "  BOMB THE BUS  ");
            lcdLine(1, "   Stand by...  ");
            lcdLine(2, " Props not set  ");
            lcdLine(3, sig);
            break;
        case BOMB_ARMING:
            lcdLine(0, "  BOMB THE BUS  ");
            lcdLine(1, "  !! ARMING !!  ");
            lcdLine(2, "   Stand by...  ");
            lcdLine(3, sig);
            break;
        case BOMB_ARMED:
            lcdLine(0, "!!  BOMB ARMED !!");
            lcdLine(1, "                ");
            lcdLine(2, " Hold btn:DEFUSE");
            lcdLine(3, sig);
            break;
        case BOMB_DEFUSED:
            lcdLine(0, "                ");
            lcdLine(1, " ** DEFUSED! ** ");
            lcdLine(2, "  Mission done! ");
            lcdLine(3, "                ");
            break;
        case BOMB_DETONATED:
            lcdLine(0, "                ");
            lcdLine(1, " !! DETONATED !!");
            lcdLine(2, "   GAME  OVER   ");
            lcdLine(3, "                ");
            break;
        default:
            lcdLine(0, "  BOMB THE BUS  ");
            lcdLine(1, "~~~~~~~~~~~~~~~~");
            lcdLine(2, " Hold button to ");
            lcdLine(3, "     DEFUSE     ");
            break;
    }
}

void showDefusing(uint32_t remainingMs) {
    int secs = (remainingMs + 999) / 1000;
    char timeStr[LCD_COLS + 1];
    snprintf(timeStr, sizeof(timeStr), "  Defusing: %2ds  ", secs);
    lcdLine(0, "  ** HOLD IT ** ");
    lcdLine(1, timeStr);
    lcdLine(2, "Don't release!  ");

    // Progress bar (row 3)
    int filled = map(remainingMs, HOLD_DURATION_MS, 0, 0, LCD_COLS);
    filled = constrain(filled, 0, LCD_COLS);
    char bar[LCD_COLS + 1];
    for (int i = 0; i < LCD_COLS; i++) bar[i] = (i < filled) ? '#' : ' ';
    bar[LCD_COLS] = '\0';
    lcdLine(3, bar);
}

void showDefused() {
    lcdLine(0, "                ");
    lcdLine(1, " *** DEFUSED! **");
    lcdLine(2, "  Mission done! ");
    lcdLine(3, "                ");
}

void showFailed() {
    lcdLine(0, "                ");
    lcdLine(1, "  !! FAILED !!  ");
    lcdLine(2, "  Released too  ");
    lcdLine(3, "     early!     ");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Bomb The Bus — Defusal Device ===");

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // LCD
    Wire.begin();
    lcd.init();
    lcd.backlight();
    showIdle();
    Serial.printf("LCD: init at 0x%02X\n", LCD_ADDR);

    // BLE advertising
    BLEDevice::init("BTB-Defusal");
    bleAdv = BLEDevice::getAdvertising();
    currentBLEState = 0xFF;  // force first setBLEState to apply
    setBLEState(STATE_IDLE);
    xTaskCreatePinnedToCore(centralScanTask, "CentralScan", 4096, NULL, 1, NULL, 0);
    Serial.println("BLE: advertising + scanning for BTB-Central");
    Serial.println("Ready — waiting for button press");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static bool lastBtn = HIGH;
    static uint32_t failedAt = 0;

    bool btn = digitalRead(BUTTON_PIN);
    uint32_t now = millis();

    switch (devState) {

        case DevState::IDLE: {
            // Refresh LCD periodically so signal strength + bomb state stay current
            static uint32_t lastLcd = 0;
            if (now - lastLcd >= 500) { lastLcd = now; showIdle(); }

            // Only allow defusal when bomb is actually armed
            if (btn == LOW && lastBtn == HIGH && central.state == BOMB_ARMED) {
                devState  = DevState::DEFUSING;
                holdStart = now;
                setBLEState(STATE_ACTIVE);
                Serial.println("Defusal started — hold button...");
            }
            break;
        }

        case DevState::DEFUSING:
            if (btn == HIGH) {
                // Released early
                devState  = DevState::FAILED;
                failedAt  = now;
                setBLEState(STATE_IDLE);
                showFailed();
                Serial.println("Failed — button released early");
            } else {
                uint32_t elapsed = now - holdStart;
                if (elapsed >= HOLD_DURATION_MS) {
                    devState = DevState::DEFUSED;
                    setBLEState(STATE_COMPLETE);
                    showDefused();
                    Serial.println("DEFUSED!");
                } else {
                    showDefusing(HOLD_DURATION_MS - elapsed);
                }
            }
            break;

        case DevState::FAILED:
            // Show failure for 2s then return to idle
            if (now - failedAt >= 2000) {
                devState = DevState::IDLE;
                setBLEState(STATE_IDLE);
                showIdle();
                Serial.println("Reset to idle");
            }
            break;

        case DevState::DEFUSED:
            // Hold final state — power cycle to reset
            break;
    }

    lastBtn = btn;
    delay(50);
}
