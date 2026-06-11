#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "ThingSpeak.h"
#include <time.h> 
#include <ArduinoJson.h> // Hivatalos ipari JSON feldolgozó könyvtár

// --- WI-FI SETTINGS ---
const char* WIFI_SSID = "xxxxxxxxxxxxxxxx";
const char* WIFI_PASSWORD = "xxxxxxxxxxxxx";

// --- THINGSPEAK ---
unsigned long myChannelNumber = xxxxxxxxxxx;
const char* myReadAPIKey = "xxxxxxxxxxxx";

// --- OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient client;

// Global measurement variables
float inTemp = 0.0, outTemp = 0.0, pressure = 0.0;
float pressureHistory[6] = {0.0};
float pressureMA[6] = {0.0};
float tempInHistory[10] = {0.0};
float tempOutHistory[10] = {0.0};
const int MA_WINDOW = 3;

String currentWindDir = "N"; 
bool isBarometricCrash = false;      

// Timers
unsigned long lastUpdateCheck = 0;
unsigned long lastScreenSwitch = 0;
unsigned long lastHourlyCheck = 0;
unsigned long lastWindCheck = 0;     
int currentScreen = 1;
bool historyReady = false;
bool isSummer = true;

// Status variables for Boot Screen
String wifiStatusStr = "WAIT";
String openMeteoStr  = "WAIT";
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

// ÚJ, ABSZOLÚT TÉVEDHETETLEN JSON PARSOLÓ FÜGGVÉNY
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
    
    // Memória lefoglalása a JSON struktúrának
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      // Közvetlen, strukturált adatkiolvasás az API-ból
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

char getTrendChar(float current, float past) {
  if (past == 0.0) return '-'; 
  float delta = current - past;
  if (delta >= 0.25) return '^';
  if (delta <= -0.25) return 'v';
  return '-';
}

bool fetchLatestData() {
  if (WiFi.status() != WL_CONNECTED) {
    tsStatusStr = "NOK (No Net)";
    return false;
  }
  
  float t1 = ThingSpeak.readFloatField(myChannelNumber, 1, myReadAPIKey);
  delay(500); 
  float t5 = ThingSpeak.readFloatField(myChannelNumber, 5, myReadAPIKey);
  delay(500);
  float p3 = ThingSpeak.readFloatField(myChannelNumber, 3, myReadAPIKey);
  
  if (!isnan(t1) || !isnan(t5) || !isnan(p3)) {
    tsStatusStr = "OK";
    if (!isnan(t1) && t1 != 0.0) inTemp = t1;
    if (!isnan(t5) && t5 != 0.0) outTemp = t5;
    if (!isnan(p3) && p3 > 950.0 && p3 < 1050.0) pressure = p3;
    return true;
  } else {
    tsStatusStr = "ERROR";
    return false;
  }
}

void updateLocalHistory() {
  unsigned long currentMillis = millis();
  
  if (lastHourlyCheck == 0 || currentMillis - lastHourlyCheck >= 3600000UL) {
    lastHourlyCheck = currentMillis;
    
    for (int i = 0; i < 9; i++) {
      tempInHistory[i] = tempInHistory[i+1];
      tempOutHistory[i] = tempOutHistory[i+1];
    }
    tempInHistory[9] = inTemp;
    tempOutHistory[9] = outTemp;

    for (int i = 0; i < 5; i++) {
      pressureHistory[i] = pressureHistory[i+1];
      pressureMA[i] = pressureMA[i+1];
    }
    pressureHistory[5] = pressure;
    
    float sum = 0.0;
    int count = 0;
    for (int i = 6 - MA_WINDOW; i < 6; i++) {
      if (pressureHistory[i] > 950.0) { sum += pressureHistory[i]; count++; }
    }
    pressureMA[5] = (count > 0) ? sum / count : pressure;
    
    float shortTrend = pressureMA[5] - pressureMA[4];
    if (shortTrend <= -1.0 && pressureMA[4] > 950.0) {
      isBarometricCrash = true; 
    } else {
      isBarometricCrash = false;
    }
    
    int valid = 0;
    for (int i = 0; i < 6; i++) if (pressureHistory[i] > 950.0) valid++;
    if (valid >= MA_WINDOW) historyReady = true;
  }
}

String getForecastText() {
  if (!historyReady || pressureMA[5] < 950.0) return "COLLECTING...";
  if (isBarometricCrash) return "STORM WARNING"; 
  
  float p = pressureMA[5];
  float raw_trend = pressureMA[5] - pressureMA[0]; 
  int wind_mod = 0;
  
  if (currentWindDir == "S" || currentWindDir == "SW" || currentWindDir == "SE") wind_mod = 2; 
  else if (currentWindDir == "W" || currentWindDir == "E") wind_mod = 1;
  
  float trend = raw_trend - (wind_mod * 0.4);
  float seasonalFactor = isSummer ? -0.5 : 0.5;
  
  if (trend <= -4.0 + seasonalFactor) return (p < 1005) ? "STORMY RAIN" : "RAIN/WEATHER";
  if (trend <= -2.0) return "BAD WEATHER";
  if (trend >= 3.0 + seasonalFactor) return "SUNNY/CLEAR"; 
  if (trend >= 1.5) return "SLOW IMPROV.";
  
  if (p >= 1020) return "STABLE SUNNY";
  if (p >= 1013) return isSummer ? "SUNNY/DRY" : "CLOUDY/DRY"; 
  if (p >= 1005) return "CLOUDY/STAB.";
  return "LOW/CLOUDY";
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  if (currentScreen == 1) {
    char trendIn = getTrendChar(inTemp, tempInHistory[0]);
    char trendOut = getTrendChar(outTemp, tempOutHistory[0]);
    char trendP = getTrendChar(pressure, pressureMA[4]);
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
    for(int i=0; i<10; i++) { tempInHistory[i] = inTemp; tempOutHistory[i] = outTemp; }
    for(int i=0; i<6; i++) { pressureHistory[i] = pressure; pressureMA[i] = pressure; }
  }
  delay(2000); 
  updateDisplay();
}

void loop() {
  unsigned long currentMillis = millis();
  
  if (currentMillis - lastUpdateCheck >= 60000UL || lastUpdateCheck == 0) {
    lastUpdateCheck = currentMillis;
    connectWiFi();
    if (fetchLatestData()) {
      updateLocalHistory();
      if (currentScreen == 1) updateDisplay(); 
    }
  }
  
  if (currentMillis - lastWindCheck >= 900000UL || lastWindCheck == 0) {
    lastWindCheck = currentMillis;
    fetchWindDirection();
    updateSeason(); 
  }
  
  if (currentScreen == 1 && currentMillis - lastScreenSwitch >= 7000UL) {
    lastScreenSwitch = currentMillis;
    currentScreen = 2;
    updateDisplay();
  } 
  else if (currentScreen == 2 && currentMillis - lastScreenSwitch >= 4000UL) {
    lastScreenSwitch = currentMillis;
    currentScreen = 1;
    updateDisplay();
  }
}