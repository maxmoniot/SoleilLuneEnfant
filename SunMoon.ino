/*
 * SunMoon v1.0
 * Veilleuse Jour/Nuit pour Enfants
 * Programme pour TTGO T-Display v1 (ESP32)
 * Position: Verticale, USB vers le haut
 * 
 * https://github.com/votre-username/sunmoon
 */

#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include "esp_adc_cal.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "soleilimg.h"
#include "luneimg.h"

// ============================================
// CONFIGURATION WIFI
// ============================================
const char* ssid = "Maison";
const char* password = "jemeconnecteauwifi";

// ============================================
// CONFIGURATION MATÉRIELLE
// ============================================
#define BTN_JAUNE       2
#define BTN_VERT        15
#define BTN_OFF         13
#define ADC_EN          14
#define ADC_PIN         34

// ============================================
// CONFIGURATION NTP
// ============================================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;

// ============================================
// CONSTANTES
// ============================================
#define LUMINOSITE_JOUR     100
#define LUMINOSITE_NUIT     0    // Backlight éteint
#define LUMINOSITE_LOADING  10
#define WIFI_TIMEOUT_MS     8000
#define DEBOUNCE_MS         50
#define LONG_PRESS_MS       3000
#define INFO_DISPLAY_MS     2000
#define AUTO_REPEAT_DELAY   400
#define AUTO_REPEAT_RATE    150

// ============================================
// VARIABLES RTC (persistent deep sleep)
// ============================================
RTC_DATA_ATTR int heureReveil = 7;
RTC_DATA_ATTR int minuteReveil = 0;
RTC_DATA_ATTR int heureCoucher = 20;
RTC_DATA_ATTR int minuteCoucher = 0;
RTC_DATA_ATTR bool settingsInitialized = false;
RTC_DATA_ATTR bool timeWasSynced = false;

// ============================================
// VARIABLES GLOBALES
// ============================================
TFT_eSPI tft = TFT_eSPI();
int vref = 1100;
bool currentDisplayIsDay = false;
bool timeAcquired = false;
unsigned long lastDisplayUpdate = 0;
unsigned long bothButtonsStartTime = 0;
bool bothButtonsPressed = false;

bool lastBtnJaune = HIGH;
bool lastBtnVert = HIGH;
bool lastBtnOff = HIGH;
unsigned long lastDebounceJaune = 0;
unsigned long lastDebounceVert = 0;

#define TFT_ORANGE 0xFD20

// ============================================
// SETUP
// ============================================
void setup() {
    // Éteindre le backlight immédiatement
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW);
    
    Serial.begin(115200);
    
    // Vérifier la raison du reset
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool isBrownout = (reset_reason == ESP_RST_BROWNOUT);
    
    if (!settingsInitialized) {
        heureReveil = 7;
        minuteReveil = 0;
        heureCoucher = 20;
        minuteCoucher = 0;
        settingsInitialized = true;
    }
    
    initButtons();
    initADC();
    initDisplay();
    
    // Si brownout ET on a déjà sync l'heure : utiliser l'heure interne sans WiFi
    if (isBrownout && timeWasSynced) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            timeAcquired = true;
        }
    } else {
        // Sinon, tenter le WiFi normalement
        afficherChargement();
        syncTimeNTP();
        
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            timeAcquired = true;
            timeWasSynced = true;
        }
    }
    
    updateDisplay();
}

// ============================================
// LOOP
// ============================================
void loop() {
    handleButtons();
    
    if (millis() - lastDisplayUpdate > 30000) {
        updateDisplay();
        lastDisplayUpdate = millis();
    }
    
    delay(50);
}

// ============================================
// INIT DISPLAY
// ============================================
void initDisplay() {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, 0);
    
    delay(50);
    
    tft.init();
    tft.writecommand(TFT_DISPOFF);
    tft.setRotation(2);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);
    
    delay(20);
    tft.writecommand(TFT_DISPON);
}

// ============================================
// INIT BOUTONS
// ============================================
void initButtons() {
    pinMode(BTN_JAUNE, INPUT_PULLUP);
    pinMode(BTN_VERT, INPUT_PULLUP);
    pinMode(BTN_OFF, INPUT_PULLUP);
}

// ============================================
// INIT ADC
// ============================================
void initADC() {
    pinMode(ADC_EN, OUTPUT);
    digitalWrite(ADC_EN, HIGH);
    
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars
    );
    
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        vref = adc_chars.vref;
    }
}

// ============================================
// AFFICHER CHARGEMENT
// ============================================
void afficherChargement() {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("chargement", tft.width()/2, tft.height()/2);
    
    ledcWrite(0, LUMINOSITE_LOADING);
}

// ============================================
// SYNC NTP
// ============================================
void syncTimeNTP() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_TIMEOUT_MS) {
        delay(250);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        
        struct tm timeinfo;
        int retry = 0;
        while (!getLocalTime(&timeinfo) && retry < 10) {
            delay(500);
            retry++;
        }
    }
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ============================================
// UPDATE DISPLAY
// ============================================
void updateDisplay() {
    struct tm timeinfo;
    bool shouldBeDay;
    
    if (getLocalTime(&timeinfo)) {
        timeAcquired = true;
        shouldBeDay = isTimeForSun();
    } else {
        shouldBeDay = true;
    }
    
    if (shouldBeDay) {
        tft.pushImage(0, 0, 135, 240, soleilimg);
        ledcWrite(0, LUMINOSITE_JOUR);
        currentDisplayIsDay = true;
    } else {
        tft.pushImage(0, 0, 135, 240, luneimg);
        ledcWrite(0, LUMINOSITE_NUIT);  // = 0, backlight éteint
        currentDisplayIsDay = false;
    }
    
    lastDisplayUpdate = millis();
}

// ============================================
// IS TIME FOR SUN
// ============================================
bool isTimeForSun() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return true;
    
    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int wakeMinutes = heureReveil * 60 + minuteReveil;
    int sleepMinutes = heureCoucher * 60 + minuteCoucher;
    
    if (sleepMinutes <= wakeMinutes) {
        return (currentMinutes >= wakeMinutes || currentMinutes < sleepMinutes);
    } else {
        return (currentMinutes >= wakeMinutes && currentMinutes < sleepMinutes);
    }
}

// ============================================
// HANDLE BUTTONS
// ============================================
void handleButtons() {
    bool btnJaune = digitalRead(BTN_JAUNE);
    bool btnVert = digitalRead(BTN_VERT);
    bool btnOff = digitalRead(BTN_OFF);
    
    if (btnJaune == LOW && btnVert == LOW) {
        if (!bothButtonsPressed) {
            bothButtonsPressed = true;
            bothButtonsStartTime = millis();
        } else if (millis() - bothButtonsStartTime >= LONG_PRESS_MS) {
            bothButtonsPressed = false;
            menuReglages();
            updateDisplay();
            return;
        }
    } else {
        bothButtonsPressed = false;
    }
    
    if (btnJaune == LOW && lastBtnJaune == HIGH && btnVert == HIGH) {
        if (millis() - lastDebounceJaune > DEBOUNCE_MS) {
            afficherReglages();
            lastDebounceJaune = millis();
        }
    }
    lastBtnJaune = btnJaune;
    
    if (btnVert == LOW && lastBtnVert == HIGH && btnJaune == HIGH) {
        if (millis() - lastDebounceVert > DEBOUNCE_MS) {
            afficherBatterie();
            lastDebounceVert = millis();
        }
    }
    lastBtnVert = btnVert;
    
    if (btnOff == LOW && lastBtnOff == HIGH) {
        goToSleep();
    }
    lastBtnOff = btnOff;
}

// ============================================
// AFFICHER RÉGLAGES
// ============================================
void afficherReglages() {
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    
    tft.fillScreen(TFT_BLACK);
    ledcWrite(0, LUMINOSITE_JOUR);
    
    int y = 20;
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("=== REGLAGES ===", tft.width()/2, y);
    
    y += 30;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Heure actuelle:", tft.width()/2, y);
    y += 20;
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(formatTime(timeinfo.tm_hour, timeinfo.tm_min), tft.width()/2, y);
    
    y += 40;
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Lever du soleil:", tft.width()/2, y);
    y += 20;
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(formatTime(heureReveil, minuteReveil), tft.width()/2, y);
    
    y += 40;
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Coucher du soleil:", tft.width()/2, y);
    y += 20;
    tft.setTextSize(2);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(formatTime(heureCoucher, minuteCoucher), tft.width()/2, y);
    
    delay(INFO_DISPLAY_MS);
    updateDisplay();
}

// ============================================
// AFFICHER BATTERIE
// ============================================
void afficherBatterie() {
    int percentage = getBatteryPercentage();
    float voltage = getBatteryVoltage();
    
    tft.fillScreen(TFT_BLACK);
    ledcWrite(0, LUMINOSITE_JOUR);
    
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("=== BATTERIE ===", tft.width()/2, 20);
    
    drawBatteryIcon(tft.width()/2 - 20, 60, percentage);
    
    tft.setTextSize(2);
    if (percentage > 50) tft.setTextColor(TFT_GREEN, TFT_BLACK);
    else if (percentage > 20) tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    else tft.setTextColor(TFT_RED, TFT_BLACK);
    
    tft.drawString(String(percentage) + "%", tft.width()/2, 170);
    
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(voltage, 2) + "V", tft.width()/2, 200);
    
    delay(INFO_DISPLAY_MS);
    updateDisplay();
}

// ============================================
// DRAW BATTERY ICON
// ============================================
void drawBatteryIcon(int x, int y, int percentage) {
    int batWidth = 40, batHeight = 80, capWidth = 20, capHeight = 8, border = 3;
    int fillHeight = map(percentage, 0, 100, 0, batHeight - 2 * border);
    
    uint16_t fillColor = (percentage > 50) ? TFT_GREEN : (percentage > 20) ? TFT_YELLOW : TFT_RED;
    
    tft.fillRoundRect(x + (batWidth - capWidth)/2, y, capWidth, capHeight, 2, TFT_WHITE);
    tft.fillRoundRect(x, y + capHeight, batWidth, batHeight, 4, TFT_WHITE);
    tft.fillRoundRect(x + border, y + capHeight + border, batWidth - 2*border, batHeight - 2*border, 2, TFT_BLACK);
    
    if (fillHeight > 0) {
        int fillY = y + capHeight + batHeight - border - fillHeight;
        tft.fillRoundRect(x + border + 2, fillY, batWidth - 2*border - 4, fillHeight - 2, 2, fillColor);
    }
}

// ============================================
// FORMAT NUMBER
// ============================================
String formatNumber(int n) {
    return (n < 10) ? "0" + String(n) : String(n);
}

// ============================================
// MENU RÉGLAGES
// ============================================
void menuReglages() {
    int tempHeureReveil = heureReveil;
    int tempMinuteReveil = minuteReveil;
    int tempHeureCoucher = heureCoucher;
    int tempMinuteCoucher = minuteCoucher;
    
    int currentDigit = 0;
    int* values[] = {&tempHeureReveil, &tempMinuteReveil, &tempHeureCoucher, &tempMinuteCoucher};
    int maxValues[] = {23, 59, 23, 59};
    
    unsigned long lastBlink = 0;
    bool blinkState = true;
    
    unsigned long btnJaunePressStart = 0;
    bool btnJauneWasPressed = false;
    unsigned long lastAutoRepeat = 0;
    bool btnVertWasPressed = false;
    
    int oldHReveil = -1, oldMReveil = -1, oldHCoucher = -1, oldMCoucher = -1;
    int oldCurrentDigit = -1;
    bool oldBlinkState = true;
    bool validationDrawn = false;
    
    ledcWrite(0, LUMINOSITE_JOUR);
    
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("=== REGLAGES ===", tft.width()/2, 15);
    
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("JAUNE: +  VERT: Suivant", tft.width()/2, 35);
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Lever soleil:", tft.width()/2, 60);
    tft.drawString("Coucher soleil:", tft.width()/2, 120);
    
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(":", tft.width()/2, 85);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(":", tft.width()/2, 145);
    
    while (digitalRead(BTN_JAUNE) == LOW || digitalRead(BTN_VERT) == LOW) delay(50);
    delay(100);
    
    while (true) {
        if (millis() - lastBlink > 300) {
            blinkState = !blinkState;
            lastBlink = millis();
        }
        
        tft.setTextSize(2);
        tft.setTextDatum(MC_DATUM);
        
        bool hReveilChanged = (tempHeureReveil != oldHReveil) || 
                              (currentDigit == 0 && blinkState != oldBlinkState) ||
                              (oldCurrentDigit == 0 && currentDigit != 0);
        bool mReveilChanged = (tempMinuteReveil != oldMReveil) || 
                              (currentDigit == 1 && blinkState != oldBlinkState) ||
                              (oldCurrentDigit == 1 && currentDigit != 1);
        bool hCoucherChanged = (tempHeureCoucher != oldHCoucher) || 
                               (currentDigit == 2 && blinkState != oldBlinkState) ||
                               (oldCurrentDigit == 2 && currentDigit != 2);
        bool mCoucherChanged = (tempMinuteCoucher != oldMCoucher) || 
                               (currentDigit == 3 && blinkState != oldBlinkState) ||
                               (oldCurrentDigit == 3 && currentDigit != 3);
        
        if (hReveilChanged) {
            bool visible = !(currentDigit == 0 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldHReveil >= 0 ? oldHReveil : 0), tft.width()/2 - 30, 85);
            tft.setTextColor(visible ? TFT_YELLOW : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempHeureReveil), tft.width()/2 - 30, 85);
            oldHReveil = tempHeureReveil;
        }
        
        if (mReveilChanged) {
            bool visible = !(currentDigit == 1 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldMReveil >= 0 ? oldMReveil : 0), tft.width()/2 + 30, 85);
            tft.setTextColor(visible ? TFT_YELLOW : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempMinuteReveil), tft.width()/2 + 30, 85);
            oldMReveil = tempMinuteReveil;
        }
        
        if (hCoucherChanged) {
            bool visible = !(currentDigit == 2 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldHCoucher >= 0 ? oldHCoucher : 0), tft.width()/2 - 30, 145);
            tft.setTextColor(visible ? TFT_ORANGE : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempHeureCoucher), tft.width()/2 - 30, 145);
            oldHCoucher = tempHeureCoucher;
        }
        
        if (mCoucherChanged) {
            bool visible = !(currentDigit == 3 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldMCoucher >= 0 ? oldMCoucher : 0), tft.width()/2 + 30, 145);
            tft.setTextColor(visible ? TFT_ORANGE : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempMinuteCoucher), tft.width()/2 + 30, 145);
            oldMCoucher = tempMinuteCoucher;
        }
        
        if (currentDigit == 4 && !validationDrawn) {
            tft.fillRect(0, 170, 135, 70, TFT_BLACK);
            tft.setTextSize(1);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString("Confirmer ?", tft.width()/2, 180);
            tft.setTextSize(2);
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawString("JAUNE=OUI", tft.width()/2, 205);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("VERT=NON", tft.width()/2, 230);
            validationDrawn = true;
        } else if (currentDigit != 4 && validationDrawn) {
            tft.fillRect(0, 170, 135, 70, TFT_BLACK);
            validationDrawn = false;
        }
        
        oldCurrentDigit = currentDigit;
        oldBlinkState = blinkState;
        
        bool btnJaune = (digitalRead(BTN_JAUNE) == LOW);
        
        if (btnJaune) {
            if (!btnJauneWasPressed) {
                btnJauneWasPressed = true;
                btnJaunePressStart = millis();
                lastAutoRepeat = millis();
                
                if (currentDigit < 4) {
                    (*values[currentDigit])++;
                    if (*values[currentDigit] > maxValues[currentDigit]) *values[currentDigit] = 0;
                } else {
                    heureReveil = tempHeureReveil;
                    minuteReveil = tempMinuteReveil;
                    heureCoucher = tempHeureCoucher;
                    minuteCoucher = tempMinuteCoucher;
                    
                    tft.fillScreen(TFT_BLACK);
                    tft.setTextDatum(MC_DATUM);
                    tft.setTextColor(TFT_GREEN, TFT_BLACK);
                    tft.setTextSize(2);
                    tft.drawString("SAUVEGARDE", tft.width()/2, tft.height()/2 - 10);
                    tft.drawString("OK!", tft.width()/2, tft.height()/2 + 20);
                    delay(1500);
                    return;
                }
            } else {
                if (currentDigit < 4) {
                    unsigned long holdTime = millis() - btnJaunePressStart;
                    if (holdTime > AUTO_REPEAT_DELAY && millis() - lastAutoRepeat > AUTO_REPEAT_RATE) {
                        (*values[currentDigit])++;
                        if (*values[currentDigit] > maxValues[currentDigit]) *values[currentDigit] = 0;
                        lastAutoRepeat = millis();
                    }
                }
            }
        } else {
            btnJauneWasPressed = false;
        }
        
        bool btnVert = (digitalRead(BTN_VERT) == LOW);
        
        if (btnVert && !btnVertWasPressed) {
            btnVertWasPressed = true;
            if (currentDigit < 4) {
                currentDigit++;
            } else {
                tft.fillScreen(TFT_BLACK);
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_RED, TFT_BLACK);
                tft.setTextSize(2);
                tft.drawString("ANNULE", tft.width()/2, tft.height()/2);
                delay(1000);
                return;
            }
        } else if (!btnVert) {
            btnVertWasPressed = false;
        }
        
        delay(30);
    }
}

// ============================================
// GO TO SLEEP
// ============================================
void goToSleep() {
    ledcWrite(0, 0);
    digitalWrite(TFT_BL, LOW);
    
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("Bonne journee !", tft.width()/2, tft.height()/2);
    
    ledcWrite(0, LUMINOSITE_JOUR);
    delay(2000);
    
    ledcWrite(0, 0);
    digitalWrite(TFT_BL, LOW);
    
    tft.writecommand(TFT_DISPOFF);
    tft.writecommand(TFT_SLPIN);
    
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_13, 1);
    
    esp_deep_sleep_start();
}

// ============================================
// BATTERY FUNCTIONS
// ============================================
int getBatteryPercentage() {
    float voltage = getBatteryVoltage();
    int percentage = (int)((voltage - 3.0) / (4.2 - 3.0) * 100);
    return constrain(percentage, 0, 100);
}

float getBatteryVoltage() {
    digitalWrite(ADC_EN, HIGH);
    delay(10);
    
    uint32_t total = 0;
    for (int i = 0; i < 10; i++) {
        total += analogRead(ADC_PIN);
        delay(5);
    }
    
    return ((float)(total / 10) / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
}

// ============================================
// FORMAT TIME
// ============================================
String formatTime(int h, int m) {
    return formatNumber(h) + ":" + formatNumber(m);
}
