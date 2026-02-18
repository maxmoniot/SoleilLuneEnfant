/*
 * SunMoon Robin v3.1
 * Programme pour TTGO T-Display v1 (ESP32)
 * Position: Verticale, USB vers le haut
 * 
 * CORRECTIONS v3.1:
 * - Fix deep sleep: l'horloge système ESP32 persiste via le RTC interne,
 *   plus besoin de esp_timer_get_time() (qui reboot à 0 au réveil)
 * - Fix timezone: reconfiguré systématiquement au boot (perdu en deep sleep)
 * - Fix réglage manuel: fonctionne correctement avec le RTC natif
 * - Ajout GPIO 0 (bouton BOOT) en parallèle de GPIO 2 (jaune)
 * - Ajout GPIO 35 (bouton TTGO) en parallèle de GPIO 15 (vert)
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
const char* ssid = "VOTRE_SSID_ICI";
const char* password = "VOTRE_MOT_DE_PASSE_WIFI_ICI";

// ============================================
// CONFIGURATION MATÉRIELLE
// ============================================
#define BTN_JAUNE       2     // Bouton jaune (externe)
#define BTN_JAUNE_ALT   0     // Bouton BOOT du TTGO (même fonction que jaune)
#define BTN_VERT        15    // Bouton vert (externe)
#define BTN_VERT_ALT    35    // Bouton droit du TTGO (même fonction que vert)
#define BTN_OFF         13
#define ADC_EN          14
#define ADC_PIN         34

// ============================================
// CONFIGURATION NTP
// ============================================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;      // UTC+1 (heure d'hiver France)
const int daylightOffset_sec = 3600;  // +1h supplémentaire en été

// ============================================
// CONSTANTES
// ============================================
#define LUMINOSITE_JOUR     100
#define LUMINOSITE_NUIT     5
#define LUMINOSITE_OFF      0
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
RTC_DATA_ATTR bool timeWasSynced = false;  // True si on a eu au moins une sync NTP ou réglage manuel

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
// PROTOTYPES
// ============================================
void setBacklight(uint8_t brightness);
bool syncTimeNTP();
bool readBtnJaune();
bool readBtnVert();

// ============================================
// LECTURE BOUTONS COMBINÉS
// ============================================
// Retourne true si le bouton est APPUYÉ (LOW sur l'un ou l'autre GPIO)
bool readBtnJaune() {
    return (digitalRead(BTN_JAUNE) == LOW) || (digitalRead(BTN_JAUNE_ALT) == LOW);
}

bool readBtnVert() {
    return (digitalRead(BTN_VERT) == LOW) || (digitalRead(BTN_VERT_ALT) == LOW);
}

// ============================================
// SETUP
// ============================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== SunMoon Robin v3.1 ===");
    
    // Vérifier la raison du reset
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool isBrownout = (reset_reason == ESP_RST_BROWNOUT);
    
    Serial.print("Reset reason: ");
    Serial.println(reset_reason);
    Serial.print("isBrownout: ");
    Serial.println(isBrownout);
    Serial.print("timeWasSynced: ");
    Serial.println(timeWasSynced);
    
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
    
    // ====== ÉTAPE 1: TOUJOURS reconfigurer le fuseau horaire ======
    // Le timezone est stocké en RAM, donc perdu après deep sleep.
    // L'horloge système (UTC) persiste via le RTC interne de l'ESP32.
    // configTime avec serveur vide = juste configurer le timezone, pas de NTP.
    configTime(gmtOffset_sec, daylightOffset_sec, "");
    
    // ====== ÉTAPE 2: Vérifier si le RTC a déjà une heure valide ======
    if (timeWasSynced) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 100) && timeinfo.tm_year > (2024 - 1900)) {
            timeAcquired = true;
            Serial.printf("Heure RTC valide: %02d:%02d:%02d\n",
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            Serial.println("RTC marque comme synced mais heure invalide");
            timeWasSynced = false;
        }
    }
    
    // ====== ÉTAPE 3: Tenter NTP si pas brownout et pas encore d'heure fiable ======
    if (!isBrownout) {
        if (!timeAcquired) {
            // Pas d'heure → NTP indispensable
            afficherChargement();
            if (syncTimeNTP()) {
                timeAcquired = true;
                timeWasSynced = true;
                Serial.println("Sync NTP reussie");
            } else {
                Serial.println("WiFi/NTP echoue - pas d'heure disponible");
            }
        } else {
            // On a une heure RTC, mais tenter NTP pour corriger la dérive
            // (en arrière-plan, sans bloquer longtemps)
            if (syncTimeNTP()) {
                Serial.println("Re-sync NTP reussie (correction derive)");
            } else {
                Serial.println("Re-sync NTP echouee, on garde l'heure RTC");
            }
        }
    } else {
        Serial.println("Brownout detecte - pas de WiFi");
        if (!timeAcquired) {
            Serial.println("Pas d'heure disponible - mode jour par defaut");
        }
    }
    
    updateDisplay();
}

// ============================================
// SYNC NTP (retourne true si succès)
// ============================================
bool syncTimeNTP() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    Serial.println("Connexion WiFi...");
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    
    bool success = false;
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connecte, sync NTP...");
        
        // configTime configure le timezone ET lance la sync NTP
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        
        struct tm timeinfo;
        int retry = 0;
        while (!getLocalTime(&timeinfo, 100) && retry < 10) {
            delay(500);
            retry++;
        }
        
        if (getLocalTime(&timeinfo, 100) && timeinfo.tm_year > (2024 - 1900)) {
            Serial.printf("NTP OK: %02d:%02d:%02d\n", 
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            success = true;
        } else {
            Serial.println("Echec NTP");
        }
    } else {
        Serial.println("WiFi non connecte");
    }
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    return success;
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
// SET BACKLIGHT
// ============================================
void setBacklight(uint8_t brightness) {
    ledcWrite(0, brightness);
}

// ============================================
// INIT DISPLAY
// ============================================
void initDisplay() {
    tft.init();
    tft.setSwapBytes(true);
    tft.setRotation(2);
    tft.fillScreen(TFT_BLACK);
    
    pinMode(TFT_BL, OUTPUT);
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, 0);
}

// ============================================
// INIT BOUTONS
// ============================================
void initButtons() {
    pinMode(BTN_JAUNE, INPUT_PULLUP);
    pinMode(BTN_JAUNE_ALT, INPUT_PULLUP);  // GPIO 0 - a un pull-up interne + externe sur TTGO
    pinMode(BTN_VERT, INPUT_PULLUP);
    pinMode(BTN_VERT_ALT, INPUT);          // GPIO 35 - input only, pull-up externe sur TTGO
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
    
    setBacklight(LUMINOSITE_LOADING);
}

// ============================================
// UPDATE DISPLAY
// ============================================
void updateDisplay() {
    struct tm timeinfo;
    bool shouldBeDay;
    
    if (getLocalTime(&timeinfo, 100)) {
        timeAcquired = true;
        shouldBeDay = isTimeForSun();
    } else {
        shouldBeDay = true;  // Par défaut: jour
    }
    
    if (shouldBeDay) {
        tft.pushImage(0, 0, 135, 240, soleilimg);
        setBacklight(LUMINOSITE_JOUR);
        currentDisplayIsDay = true;
    } else {
        tft.pushImage(0, 0, 135, 240, luneimg);
        setBacklight(LUMINOSITE_NUIT);
        currentDisplayIsDay = false;
    }
    
    lastDisplayUpdate = millis();
}

// ============================================
// IS TIME FOR SUN
// ============================================
bool isTimeForSun() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) return true;
    
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
    bool btnJaune = readBtnJaune();
    bool btnVert = readBtnVert();
    bool btnOff = (digitalRead(BTN_OFF) == LOW);
    
    // Appui simultané jaune + vert = menu réglages
    if (btnJaune && btnVert) {
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
    
    // Jaune seul = afficher réglages
    if (btnJaune && !lastBtnJaune && !btnVert) {
        if (millis() - lastDebounceJaune > DEBOUNCE_MS) {
            afficherReglages();
            lastDebounceJaune = millis();
        }
    }
    lastBtnJaune = btnJaune;
    
    // Vert seul = afficher batterie
    if (btnVert && !lastBtnVert && !btnJaune) {
        if (millis() - lastDebounceVert > DEBOUNCE_MS) {
            afficherBatterie();
            lastDebounceVert = millis();
        }
    }
    lastBtnVert = btnVert;
    
    // Bouton OFF = deep sleep
    if (btnOff && !lastBtnOff) {
        goToSleep();
    }
    lastBtnOff = btnOff;
}

// ============================================
// AFFICHER RÉGLAGES
// ============================================
void afficherReglages() {
    struct tm timeinfo;
    getLocalTime(&timeinfo, 100);
    
    tft.fillScreen(TFT_BLACK);
    setBacklight(LUMINOSITE_JOUR);
    
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
    setBacklight(LUMINOSITE_JOUR);
    
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
// FORMAT NUMBER / TIME
// ============================================
String formatNumber(int n) {
    return (n < 10) ? "0" + String(n) : String(n);
}

String formatTime(int h, int m) {
    return formatNumber(h) + ":" + formatNumber(m);
}

// ============================================
// MENU RÉGLAGES
// ============================================
void menuReglages() {
    struct tm timeinfo;
    getLocalTime(&timeinfo, 100);
    
    int tempHeureActuelle = timeinfo.tm_hour;
    int tempMinuteActuelle = timeinfo.tm_min;
    int tempHeureReveil = heureReveil;
    int tempMinuteReveil = minuteReveil;
    int tempHeureCoucher = heureCoucher;
    int tempMinuteCoucher = minuteCoucher;
    
    int currentDigit = 0;
    // 0=heureActuelle, 1=minuteActuelle, 2=heureReveil, 3=minuteReveil, 4=heureCoucher, 5=minuteCoucher, 6=validation
    int* values[] = {&tempHeureActuelle, &tempMinuteActuelle, &tempHeureReveil, &tempMinuteReveil, &tempHeureCoucher, &tempMinuteCoucher};
    int maxValues[] = {23, 59, 23, 59, 23, 59};
    
    unsigned long lastBlink = 0;
    bool blinkState = true;
    
    unsigned long btnJaunePressStart = 0;
    bool btnJauneWasPressed = false;
    unsigned long lastAutoRepeat = 0;
    bool btnVertWasPressed = false;
    
    int oldHActuelle = -1, oldMActuelle = -1;
    int oldHReveil = -1, oldMReveil = -1, oldHCoucher = -1, oldMCoucher = -1;
    int oldCurrentDigit = -1;
    bool oldBlinkState = true;
    bool validationDrawn = false;
    
    setBacklight(LUMINOSITE_JOUR);
    
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("=== REGLAGES ===", tft.width()/2, 10);
    
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("JAUNE: +  VERT: Suivant", tft.width()/2, 25);
    
    // Labels
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Heure actuelle:", tft.width()/2, 45);
    tft.drawString("Lever soleil:", tft.width()/2, 105);
    tft.drawString("Coucher soleil:", tft.width()/2, 165);
    
    // Deux points fixes
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(":", tft.width()/2, 70);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(":", tft.width()/2, 130);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(":", tft.width()/2, 190);
    
    // Attendre relâchement des boutons
    while (readBtnJaune() || readBtnVert()) delay(50);
    delay(100);
    
    while (true) {
        if (millis() - lastBlink > 300) {
            blinkState = !blinkState;
            lastBlink = millis();
        }
        
        tft.setTextSize(2);
        tft.setTextDatum(MC_DATUM);
        
        // Heure actuelle
        bool hActuelleChanged = (tempHeureActuelle != oldHActuelle) || 
                              (currentDigit == 0 && blinkState != oldBlinkState) ||
                              (oldCurrentDigit == 0 && currentDigit != 0);
        bool mActuelleChanged = (tempMinuteActuelle != oldMActuelle) || 
                              (currentDigit == 1 && blinkState != oldBlinkState) ||
                              (oldCurrentDigit == 1 && currentDigit != 1);
        
        // Heure réveil
        bool hReveilChanged = (tempHeureReveil != oldHReveil) || 
                              (currentDigit == 2 && blinkState != oldBlinkState) ||
                              (oldCurrentDigit == 2 && currentDigit != 2);
        bool mReveilChanged = (tempMinuteReveil != oldMReveil) || 
                              (currentDigit == 3 && blinkState != oldBlinkState) ||
                              (oldCurrentDigit == 3 && currentDigit != 3);
        
        // Heure coucher
        bool hCoucherChanged = (tempHeureCoucher != oldHCoucher) || 
                               (currentDigit == 4 && blinkState != oldBlinkState) ||
                               (oldCurrentDigit == 4 && currentDigit != 4);
        bool mCoucherChanged = (tempMinuteCoucher != oldMCoucher) || 
                               (currentDigit == 5 && blinkState != oldBlinkState) ||
                               (oldCurrentDigit == 5 && currentDigit != 5);
        
        // Affichage heure actuelle (vert)
        if (hActuelleChanged) {
            bool visible = !(currentDigit == 0 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldHActuelle >= 0 ? oldHActuelle : 0), tft.width()/2 - 30, 70);
            tft.setTextColor(visible ? TFT_GREEN : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempHeureActuelle), tft.width()/2 - 30, 70);
            oldHActuelle = tempHeureActuelle;
        }
        
        if (mActuelleChanged) {
            bool visible = !(currentDigit == 1 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldMActuelle >= 0 ? oldMActuelle : 0), tft.width()/2 + 30, 70);
            tft.setTextColor(visible ? TFT_GREEN : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempMinuteActuelle), tft.width()/2 + 30, 70);
            oldMActuelle = tempMinuteActuelle;
        }
        
        // Affichage heure réveil (jaune)
        if (hReveilChanged) {
            bool visible = !(currentDigit == 2 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldHReveil >= 0 ? oldHReveil : 0), tft.width()/2 - 30, 130);
            tft.setTextColor(visible ? TFT_YELLOW : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempHeureReveil), tft.width()/2 - 30, 130);
            oldHReveil = tempHeureReveil;
        }
        
        if (mReveilChanged) {
            bool visible = !(currentDigit == 3 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldMReveil >= 0 ? oldMReveil : 0), tft.width()/2 + 30, 130);
            tft.setTextColor(visible ? TFT_YELLOW : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempMinuteReveil), tft.width()/2 + 30, 130);
            oldMReveil = tempMinuteReveil;
        }
        
        // Affichage heure coucher (orange)
        if (hCoucherChanged) {
            bool visible = !(currentDigit == 4 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldHCoucher >= 0 ? oldHCoucher : 0), tft.width()/2 - 30, 190);
            tft.setTextColor(visible ? TFT_ORANGE : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempHeureCoucher), tft.width()/2 - 30, 190);
            oldHCoucher = tempHeureCoucher;
        }
        
        if (mCoucherChanged) {
            bool visible = !(currentDigit == 5 && !blinkState);
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(oldMCoucher >= 0 ? oldMCoucher : 0), tft.width()/2 + 30, 190);
            tft.setTextColor(visible ? TFT_ORANGE : TFT_BLACK, TFT_BLACK);
            tft.drawString(formatNumber(tempMinuteCoucher), tft.width()/2 + 30, 190);
            oldMCoucher = tempMinuteCoucher;
        }
        
        // Validation (currentDigit == 6)
        if (currentDigit == 6 && !validationDrawn) {
            tft.fillRect(0, 210, 135, 30, TFT_BLACK);
            tft.setTextSize(1);
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawString("JAUNE=OK", tft.width()/2 - 30, 225);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("VERT=NON", tft.width()/2 + 30, 225);
            validationDrawn = true;
        } else if (currentDigit != 6 && validationDrawn) {
            tft.fillRect(0, 210, 135, 30, TFT_BLACK);
            validationDrawn = false;
        }
        
        oldCurrentDigit = currentDigit;
        oldBlinkState = blinkState;
        
        // ---- Bouton Jaune (+ / Valider) ----
        bool btnJaune = readBtnJaune();
        
        if (btnJaune) {
            if (!btnJauneWasPressed) {
                btnJauneWasPressed = true;
                btnJaunePressStart = millis();
                lastAutoRepeat = millis();
                
                if (currentDigit < 6) {
                    (*values[currentDigit])++;
                    if (*values[currentDigit] > maxValues[currentDigit]) *values[currentDigit] = 0;
                } else {
                    // ===== SAUVEGARDE =====
                    heureReveil = tempHeureReveil;
                    minuteReveil = tempMinuteReveil;
                    heureCoucher = tempHeureCoucher;
                    minuteCoucher = tempMinuteCoucher;
                    
                    // Mettre à jour l'heure système
                    struct tm newTime;
                    newTime.tm_hour = tempHeureActuelle;
                    newTime.tm_min = tempMinuteActuelle;
                    newTime.tm_sec = 0;
                    newTime.tm_mday = timeinfo.tm_mday;
                    newTime.tm_mon = timeinfo.tm_mon;
                    newTime.tm_year = timeinfo.tm_year;
                    newTime.tm_isdst = timeinfo.tm_isdst;
                    
                    time_t newTimestamp = mktime(&newTime);
                    struct timeval tv;
                    tv.tv_sec = newTimestamp;
                    tv.tv_usec = 0;
                    settimeofday(&tv, NULL);
                    
                    // Marquer que l'heure est valide (pour deep sleep)
                    timeWasSynced = true;
                    timeAcquired = true;
                    
                    Serial.printf("Heure manuelle: %02d:%02d\n", tempHeureActuelle, tempMinuteActuelle);
                    Serial.printf("Reveil: %02d:%02d  Coucher: %02d:%02d\n",
                                  heureReveil, minuteReveil, heureCoucher, minuteCoucher);
                    
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
                // Auto-repeat en maintenant appuyé
                if (currentDigit < 6) {
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
        
        // ---- Bouton Vert (Suivant / Annuler) ----
        bool btnVert = readBtnVert();
        
        if (btnVert && !btnVertWasPressed) {
            btnVertWasPressed = true;
            if (currentDigit < 6) {
                currentDigit++;
            } else {
                // Annuler
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
    // Pas besoin de sauvegarder l'heure manuellement:
    // le RTC interne ESP32 maintient l'horloge système pendant le deep sleep.
    // Il suffit de reconfigurer le timezone au réveil (fait dans setup).
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
        Serial.printf("Deep sleep a %02d:%02d:%02d\n",
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    
    setBacklight(LUMINOSITE_OFF);
    
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("Bonne journee", tft.width()/2, tft.height()/2 - 10);
    tft.drawString("Robin !", tft.width()/2, tft.height()/2 + 10);
    
    setBacklight(LUMINOSITE_JOUR);
    delay(2000);
    
    setBacklight(LUMINOSITE_OFF);
    
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
