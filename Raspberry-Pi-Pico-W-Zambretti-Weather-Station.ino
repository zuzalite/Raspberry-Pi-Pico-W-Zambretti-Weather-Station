#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "ThingSpeak.h"
#include <time.h> 
#include <ArduinoJson.h> // Hivatalos ipari JSON feldolgozó könyvtár

// ==============================================================================
// --- FELHASZNÁLÓI BEÁLLÍTÁSOK (KONFIGURÁCIÓ) ---
// ==============================================================================
// WI-FI HÁLÓZATI BEÁLLÍTÁSOK
const char* WIFI_SSID     = "xxxxxxxx";
const char* WIFI_PASSWORD = "xxxxxxxxxx";

// THINGSPEAK BEÁLLÍTÁSOK
unsigned long myChannelNumber = xxxxxxx;
const char* myReadAPIKey      = "xxxxxxxxxxxxxxxx";

// METEOROLÓGIAI KÜSZÖBÉRTÉKEK (A TRÉNING ALAPJÁN FINOMHANGOLHATÓ)
const float STORM_THRESHOLD         = -1.8;   // Viharjelzés küszöbérték (hPa / 5 perc) -> pl. -4.20
const float BAD_WEATHER_THRESHOLD   = -1.1;   // Időjárás-romlás küszöb (hPa / 60 perc)
const float SUNNY_CLEAR_THRESHOLD   = 1.2;    // Tiszta/napos idő küszöb (hPa / 60 perc)
const float SLOW_IMPROV_THRESHOLD   = 0.5;    // Lassú javulás küszöb (hPa / 60 perc)
const float SEASONAL_OFFSET         = 0.3;    // Szezonális eltolás mértéke
const float WIND_MULTIPLIER         = 0.5;    // V OPTIMALIZÁLT SZÉLIRÁNY-SZORZÓ V

// ABSZOLÚT NYOMÁSI ZÓNÁK (hPa)
const float PRESSURE_EXTREME_HIGH   = 1035.0; // Extrém magas nyomás határ (Anticiklon)
const float PRESSURE_STANDARD_MID   = 1013.0; // Standard tengerszinti alapérték
const float PRESSURE_EXTREME_LOW    = 995.0;  // Extrém alacsony nyomás határ (Ciklon)

// IDŐZÍTÉSEK ÉS MATEMATIKAI ABLAKOK
const unsigned long UPDATE_INTERVAL_MS     = 60000UL;  // ThingSpeak adatletöltési gyakoriság (60 mp)
const unsigned long WIND_CHECK_INTERVAL_MS = 900000UL; // Szélirány és szezon frissítése (15 perc)
const unsigned long SCREEN_1_DURATION_MS   = 7000UL;   // 1. képernyő (Alapadatok) láthatósága (7 mp)
const unsigned long SCREEN_2_DURATION_MS   = 4000UL;   // 2. képernyő (Előrejelzés) láthatósága (4 mp)
const int MA_WINDOW                        = 5;        // Szenzorzaj-szűrés mozgóátlag ablaka (elem)
// ==============================================================================

// --- OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient client;

// Global measurement variables
float inTemp = 0.0, outTemp = 0.0, pressure = 0.0;

// MATEMATIKAI JAVÍTÁS: 61 elem kell a 60 perces, 11 elem a 10 perces tiszta időkülönbséghez
float pressureHistory[61] = {0.0};
float pressureMA[61] = {0.0};
float tempInHistory[11] = {0.0};
float tempOutHistory[11] = {0.0};

String currentWindDir = "N"; 
bool isBarometricCrash = false;      

// Timers
unsigned long lastUpdateCheck = 0;
unsigned long lastScreenSwitch = 0;
unsigned long lastWindCheck = 0;     
int currentScreen = 1;
bool historyReady = false;
bool isSummer = true;

// Status variables for Boot Screen
String wifiStatusStr = "WAIT";
String openMeteoStr   = "WAIT";
String tsStatusStr   = "WAIT";

void drawBootScreen(String currentAction) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("--- SYSTEM START ---");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  
  display.setCursor(0, 16);
  display.print("WiFi Network: "); display.println(wifiStatusStr);
  display.setCursor(0, 26);
  display.print("Open-Meteo:   "); display.println(openMeteoStr);
  display.setCursor(0, 36);
  display.print("ThingSpeak:   "); display.println(tsStatusStr);
  
  display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
  display.setCursor(0, 53);
  display.print(">"); display.print(currentAction);
  display.display();
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiStatusStr = "OK";
    return;
  }
  wifiStatusStr = "NOK";
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 30) {
    delay(500);
    attempt++;
    drawBootScreen("WiFi connecting (" + String(attempt/2) + "s)");
  }
  if (WiFi.status() == WL_CONNECTED) wifiStatusStr = "OK";
  else wifiStatusStr = "NOK";
}

void syncInternetTime() {
  drawBootScreen("Syncing NTP Time...");
  configTime(1 * 3600, 0, "time.google.com", "pool.ntp.org");
  int retry = 0;
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 24 && retry < 20) {
    delay(500);
    now = time(nullptr);
    retry++;
  }
}

void updateSeason() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t != nullptr) {
    int month = t->tm_mon + 1;
    isSummer = (month >= 3 && month <= 9);
  } else {
    isSummer = true; 
  }
}

bool fetchWindDirection() {
  if (WiFi.status() != WL_CONNECTED) {
    openMeteoStr = "NOK (No Net)";
    return false;
  }
  
  HTTPClient http;
  http.begin("http://api.open-meteo.com/v1/forecast?latitude=47.4979&longitude=19.0402&current_weather=true"); 
  int httpCode = http.GET();
  bool success = false;
  
  if (httpCode == 200) {
    String payload = http.getString();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      float wind_deg = doc["current_weather"]["winddirection"];
      
      if (337.5 <= wind_deg || wind_deg < 22.5)       currentWindDir = "N";
      else if (22.5 <= wind_deg && wind_deg < 67.5)   currentWindDir = "NE";
      else if (67.5 <= wind_deg && wind_deg < 112.5)  currentWindDir = "E";
      else if (112.5 <= wind_deg && wind_deg < 157.5) currentWindDir = "SE";
      else if (157.5 <= wind_deg && wind_deg < 202.5) currentWindDir = "S";
      else if (202.5 <= wind_deg && wind_deg < 247.5) currentWindDir = "SW";
      else if (247.5 <= wind_deg && wind_deg < 292.5) currentWindDir = "W";
      else                                            currentWindDir = "NW";
      
      openMeteoStr = "OK";
      success = true;
    } else {
      openMeteoStr = "JSON ERR";
    }
  } else {
    openMeteoStr = "HTTP " + String(httpCode);
  }
  http.end();
  return success;
}

char getTempTrendChar(float current, float past) {
  if (past == 0.0) return '-'; 
  float delta = current - past;
  if (delta >= 0.20) return '^';
  if (delta <= -0.20) return 'v';
  return '-';
}

char getPressTrendChar(float current, float past) {
  if (past == 0.0) return '-'; 
  float delta = current - past;
  if (delta >= 0.10) return '^';
  if (delta <= -0.10) return 'v';
  return '-';
}

bool fetchLatestData() {
  if (WiFi.status() != WL_CONNECTED) {
    tsStatusStr = "NOK (No Net)";
    return false;
  }
  
  bool indoorFound = false;
  bool outdoorFound = false;
  bool pressureFound = false;

  // 1. Elsődleges olvasás: az összes adat lekérése egyben (mint a Pythonnál a last.json)
  int statusCode = ThingSpeak.readMultipleFields(myChannelNumber, myReadAPIKey);
  
  if (statusCode == 200) {
    float t1 = ThingSpeak.getFieldAsFloat(1);
    float p3 = ThingSpeak.getFieldAsFloat(3);
    float t5 = ThingSpeak.getFieldAsFloat(5);
    
    if (!isnan(t1) && t1 != 0.0) { inTemp = t1; indoorFound = true; }
    if (!isnan(t5) && t5 != 0.0) { outTemp = t5; outdoorFound = true; }
    if (!isnan(p3) && p3 > 950.0 && p3 < 1050.0) { pressure = p3; pressureFound = true; }
  }

  // 2. BIZTONSÁGI HÁLÓ (Fallback): Amelyik hiányzik, azt célzottan lekérjük egyedi kéréssel
  if (!indoorFound) {
    delay(1000); // 1 másodperc szünet az API limit elkerülése miatt
    float t1 = ThingSpeak.readFloatField(myChannelNumber, 1, myReadAPIKey);
    if (!isnan(t1) && t1 != 0.0) { inTemp = t1; indoorFound = true; }
  }
  
  if (!outdoorFound) {
    delay(1000);
    float t5 = ThingSpeak.readFloatField(myChannelNumber, 5, myReadAPIKey);
    if (!isnan(t5) && t5 != 0.0) { outTemp = t5; outdoorFound = true; }
  }
  
  if (!pressureFound) {
    delay(1000);
    float p3 = ThingSpeak.readFloatField(myChannelNumber, 3, myReadAPIKey);
    if (!isnan(p3) && p3 > 950.0 && p3 < 1050.0) { pressure = p3; pressureFound = true; }
  }

  if (indoorFound || outdoorFound || pressureFound) {
    tsStatusStr = "OK";
    return true;
  } else {
    tsStatusStr = "ERROR";
    return false;
  }
}

void updateLocalHistory() {
  // JAVÍTVA: Léptetés a 11 elemű hőmérséklet tömbben (0-tól 9-ig fut, így a 10. index szabadul fel)
  for (int i = 0; i < 10; i++) {
    tempInHistory[i] = tempInHistory[i+1];
    tempOutHistory[i] = tempOutHistory[i+1];
  }
  tempInHistory[10] = inTemp;
  tempOutHistory[10] = outTemp;

  // JAVÍTVA: Léptetés a 61 elemű nyomás tömbben (0-tól 59-ig fut, a 60. index szabadul fel)
  for (int i = 0; i < 60; i++) {
    pressureHistory[i] = pressureHistory[i+1];
    pressureMA[i] = pressureMA[i+1];
  }
  pressureHistory[60] = pressure;
  
  // JAVÍTVA: Mozgóátlag az utolsó 3 elemre a 61 elemű tömbben (58, 59, 60. indexek)
  float sum = 0.0;
  int count = 0;
  for (int i = 61 - MA_WINDOW; i < 61; i++) {
    if (pressureHistory[i] > 950.0) { sum += pressureHistory[i]; count++; }
  }
  pressureMA[60] = (count > 0) ? sum / count : pressure;
  
  // JAVÍTVA: Barometric crash pontosan 5 perces ablakban (60. index vs 55. index: delta = 5) - Küszöb a konfigurációból vett értékre cserélve
  float shortTrend = pressureMA[60] - pressureMA[55];
  if (shortTrend <= STORM_THRESHOLD && pressureMA[55] > 950.0) {
    isBarometricCrash = true; 
  } else {
    isBarometricCrash = false;
  }
  
  // JAVÍTVA: Inicializációs ellenőrzés igazítása 61 elemre
  int valid = 0;
  for (int i = 0; i < 61; i++) if (pressureHistory[i] > 950.0) valid++;
  if (valid >= MA_WINDOW) historyReady = true;
}

String getForecastText() {
  // JAVÍTVA: Index igazítása 60-ra
  if (!historyReady || pressureMA[60] < 950.0) return "COLLECTING...";
  if (isBarometricCrash) return "STORM WARNING"; 
  
  float p = pressureMA[60];
  // JAVÍTVA: Pontosan 60 perc különbség (60. index - 0. index)
  float raw_trend = pressureMA[60] - pressureMA[0]; 
  
  // VÉRMEZŐRE OPTIMALIZÁLT SÚLYOZÁSI MÁTRIX (Kivonásra felkészítve a tréning alapján)
  float wind_mod = 0.0;
  if (currentWindDir == "S" || currentWindDir == "SW") {
      wind_mod = 2.0;   // Déli áramlások kompenzációs alapértéke
  } else if (currentWindDir == "SE" || currentWindDir == "W") {
      wind_mod = 0.5;   // Enyhébb délies/nyugati hatások
  } else if (currentWindDir == "NW") {
      wind_mod = (raw_trend < 0) ? 0.6 : -0.2;
  } else if (currentWindDir == "E" || currentWindDir == "NE" || currentWindDir == "N") {
      wind_mod = -0.6;  // Északi/keleti javító tényezők
  }
  
  // AZ OPTIMALIZÁLÓ ÁLTAL MEGHATÁROZOTT KÉPLET
  float trend = raw_trend - (wind_mod * WIND_MULTIPLIER);
  float seasonalFactor = isSummer ? -SEASONAL_OFFSET : SEASONAL_OFFSET; 
  
  if (trend <= STORM_THRESHOLD + seasonalFactor) return (p < 1005.0) ? "STORMY RAIN" : "RAIN/WEATHER";
  if (trend <= BAD_WEATHER_THRESHOLD) return "BAD WEATHER";
  if (trend >= SUNNY_CLEAR_THRESHOLD + seasonalFactor) return "SUNNY/CLEAR"; 
  if (trend >= SLOW_IMPROV_THRESHOLD) return "SLOW IMPROV.";
  
  // JAVÍTVA: Abszolút értékek beállítása a konfigurált határokhoz igazítva
  if (p >= PRESSURE_EXTREME_HIGH) return isSummer ? "STABLE SUNNY" : "COLD/FOGGY ANTI";
  if (p >= PRESSURE_STANDARD_MID) return isSummer ? "SUNNY/DRY" : "CLOUDY/DRY"; 
  if (p <= PRESSURE_EXTREME_LOW)  return "LOW/HEAVY STORM";
  
  return "STABLE/FAIR";
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  if (currentScreen == 1) {
    // JAVÍTVA: Pontosan 10 perces delta (10. index - 0. index)
    char trendIn = getTempTrendChar(tempInHistory[10], tempInHistory[0]);
    char trendOut = getTempTrendChar(tempOutHistory[10], tempOutHistory[0]);
    
    // JAVÍTVA: Pontosan 10 perces delta a légnyomásnál (60. index - 50. index)
    char trendP = getPressTrendChar(pressureMA[60], pressureMA[50]);
    float diff = inTemp - outTemp;
    
    display.setTextSize(1);
    display.setCursor(0, 2);
    display.print("Indoor:  "); display.print(inTemp, 1); display.print(" C "); display.print(trendIn);
    display.setCursor(0, 18);
    display.print("Outdoor: "); display.print(outTemp, 1); display.print(" C "); display.print(trendOut);
    display.setCursor(0, 34);
    display.print("Delta:   "); if (diff >= 0) display.print("+"); display.print(diff, 1); display.print(" C");
    display.setCursor(0, 50);
    display.print("Baro:    "); display.print(pressure, 1); display.print(" hPa "); display.print(trendP);
  } else {
    display.setTextSize(1);
    display.setCursor(0, 2);
    display.print(isSummer ? "ZAMB(SUMMER)|Wind:" : "ZAMB(WINTER)|Wind:");
    display.print(currentWindDir);
    display.drawLine(0, 15, 128, 15, SSD1306_WHITE);
    
    display.setTextSize(2);
    display.setCursor(0, 25);
    display.print(getForecastText());
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  
  drawBootScreen("System booting...");
  delay(1000);
  
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    wifiStatusStr = "NOK"; openMeteoStr = "X"; tsStatusStr = "X";
    while(true) { drawBootScreen("WiFi ERROR! Halted."); delay(1000); }
  }
  drawBootScreen("WiFi connected!");
  delay(500);
  
  syncInternetTime();
  updateSeason();
  delay(500);
  
  ThingSpeak.begin(client);
  drawBootScreen("ThingSpeak fetch...");
  fetchLatestData(); 
  delay(500);
  
  drawBootScreen("Open-Meteo fetch...");
  fetchWindDirection();
  delay(500);
  
  drawBootScreen("All ready!");
  if (tsStatusStr == "OK") {
    // JAVÍTVA: Feltöltés az új tömbméretek szerint (11 és 61)
    for(int i=0; i<11; i++) { tempInHistory[i] = inTemp; tempOutHistory[i] = outTemp; }
    for(int i=0; i<61; i++) { pressureHistory[i] = pressure; pressureMA[i] = pressure; }
  }
  delay(2000); 
  updateDisplay();
}

void loop() {
  unsigned long currentMillis = millis();
  
  if (currentMillis - lastUpdateCheck >= UPDATE_INTERVAL_MS || lastUpdateCheck == 0) {
    lastUpdateCheck = currentMillis;
    connectWiFi();
    if (fetchLatestData()) {
      updateLocalHistory(); 
      if (currentScreen == 1) updateDisplay(); 
    }
  }
  
  if (currentMillis - lastWindCheck >= WIND_CHECK_INTERVAL_MS || lastWindCheck == 0) {
    lastWindCheck = currentMillis;
    fetchWindDirection();
    updateSeason(); 
  }
  
  if (currentScreen == 1 && currentMillis - lastScreenSwitch >= SCREEN_1_DURATION_MS) {
    lastScreenSwitch = currentMillis;
    currentScreen = 2;
    updateDisplay();
  } 
  else if (currentScreen == 2 && currentMillis - lastScreenSwitch >= SCREEN_2_DURATION_MS) {
    lastScreenSwitch = currentMillis;
    currentScreen = 1;
    updateDisplay();
  }
}
