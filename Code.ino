#include <Arduino.h>
#include <DHT.h>
#include <driver/dac.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <time.h> 

// Provide the token generation process info.
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- YOUR WIFI CREDENTIALS ---
#define WIFI_SSID "Purush"
#define WIFI_PASSWORD "purush123"

// --- YOUR FIREBASE CREDENTIALS ---
#define API_KEY "AIzaSyCnpCzIuD51g87qgYFAhqeLNj1wxd5gUQY"
#define DATABASE_URL "kit-dashboard---sensors-default-rtdb.asia-southeast1.firebasedatabase.app"

// Firebase Objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Include your 4 voice files
#include "voice_normal.h"
#include "voice_warning.h"
#include "voice_critical.h"
#include "voice_error.h"

// Pins
#define DHTPIN 4
#define DHTTYPE DHT11
#define MQ2_PIN 34
#define AUDIO_PIN 25  // DAC1 - Connect to PAM8403 input
#define LDR_PIN 32    // LDR PIN

// Objects
DHT dht(DHTPIN, DHTTYPE);

// MQ-2 Constants
#define RL_VALUE 5.0
#define RO_CLEAN_AIR_FACTOR 9.8
#define LPG_A 26.57
#define LPG_B -0.42
#define VCC_ADC 3.3
#define VCC_SENSOR 5.0

// Global Variables
float Ro = 10.0;
bool baselineReady = false;
unsigned long warmupStartTime = 0;
unsigned long lastVoiceTime = 0;
int lastRiskLevel = -1;

// --- UNIQUE KIT VARIABLES ---
String kitID = "";
String dbPath = "";

// Voice database structure
struct VoiceMessage {
  const uint8_t* data;
  size_t length;
  const char* displayText;
};

// Map voice indices to your PROGMEM arrays
VoiceMessage voices[] = {
  {voice_normal, sizeof(voice_normal), "✅ All clear. Air quality normal."},
  {voice_warning, sizeof(voice_warning), "⚠️ Warning: Gas levels rising."},
  {voice_critical, sizeof(voice_critical), "🔴 DANGER! Gas leak! Evacuate!"},
  {voice_error, sizeof(voice_error), "❌ System error. Check sensors."}
};

// Play voice directly from PROGMEM
// Play voice directly from PROGMEM
void playVoice(int voiceIndex) {
  Serial.print("🔊 Playing: ");
  Serial.println(voices[voiceIndex].displayText);
  
  // IF CRITICAL: Play 3 loud alarm beeps BEFORE the voice
  if (voiceIndex == 2) {
    for(int i = 0; i < 3; i++) {
      playBeep(300, 1000); // 300ms long beep at 1000Hz frequency
      delay(150);          // Silence between beeps
    }
  }

  // Play the actual voice file
  dac_output_enable(DAC_CHANNEL_1);
  for (size_t i = 44; i < voices[voiceIndex].length; i++) { 
    uint8_t sample = pgm_read_byte(&voices[voiceIndex].data[i]);
    dac_output_voltage(DAC_CHANNEL_1, sample);  
    delayMicroseconds(62);  
  }
  dac_output_disable(DAC_CHANNEL_1);
  
  // IF CRITICAL: Play 3 loud alarm beeps AFTER the voice
  if (voiceIndex == 2) {
    delay(150);
    for(int i = 0; i < 3; i++) {
      playBeep(300, 1000); 
      delay(150);
    }
  } else {
    delay(500);  // Normal pause for other voices
  }
}

// Filtered ADC reading
int readMQ2Filtered() {
  long sum = 0;
  for (int i = 0; i < 30; i++) {
    sum += analogRead(MQ2_PIN);
    delayMicroseconds(500);
  }
  return sum / 30;
}

// Calibration
void calibrateMQ2() {
  Serial.println("\n📊 Calibrating MQ-2 sensor...");
  
  long total = 0;
  for (int i = 0; i < 100; i++) {
    total += readMQ2Filtered();
    delay(10);
  }
  
  float avgRaw = total / 100.0;
  if (avgRaw >= 4095) avgRaw = 4094;
  if (avgRaw <= 0) avgRaw = 1;
  
  float Vout_clean = (avgRaw / 4095.0) * VCC_ADC;
  if (Vout_clean < 0.01) Vout_clean = 0.01;
  
  float rs_clean = RL_VALUE * ((VCC_SENSOR - Vout_clean) / Vout_clean);
  Ro = rs_clean / RO_CLEAN_AIR_FACTOR;
  baselineReady = true;
  
  Serial.print("✅ Ro = "); Serial.println(Ro, 3);
}

// Main detection function
void detectAndAlert() {
  int gasRaw = readMQ2Filtered();
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  bool dhtFailed = isnan(temperature) || isnan(humidity);

 // --- READ LDR & NORMALIZE FOR TESTING ---
  int ldrRaw = analogRead(LDR_PIN);
  
  // Since your values REDUCE when you cover it:
  float darknessNorm = 0.0;
  if (ldrRaw <= 100) {
    darknessNorm = 1.0;  // Fully covered = 100% Dark (Triggers CRITICAL)
  } 
  else if (ldrRaw <= 260) {
    darknessNorm = 0.55; // Shadow / Hand nearby = 55% Dark (Triggers WARNING)
  } 
  else {
    darknessNorm = 0.0;  // Normal Room Light = 0% Dark (NORMAL)
  }
  
  float gasPPM = 0;
  float ratio = 0;
  
  if (baselineReady) {
    int safeRaw = gasRaw;
    if (safeRaw >= 4095) safeRaw = 4094;
    if (safeRaw <= 0) safeRaw = 1;
    
    float Vout = (safeRaw / 4095.0) * VCC_ADC;
    if (Vout < 0.01) Vout = 0.01;
    
    float Rs = RL_VALUE * ((VCC_SENSOR - Vout) / Vout);
    
    // Auto-refine Ro during warmup
    if (millis() - warmupStartTime < 120000) {
      float estimatedRo = Rs / RO_CLEAN_AIR_FACTOR;
      Ro = (0.98 * Ro) + (0.02 * estimatedRo);
    }
    
    ratio = Rs / Ro;
    gasPPM = LPG_A * pow(ratio, LPG_B);
    if (gasPPM < 0) gasPPM = 0;
    if (gasPPM > 10000) gasPPM = 10000;
  }
  
  float tempNorm = 0;
  if (!dhtFailed) {
    tempNorm = temperature / 50.0;
    if (tempNorm > 1.0) tempNorm = 1.0;
  }
  
  float gasNorm = gasPPM / 5000.0;
  if (gasNorm > 1.0) gasNorm = 1.0;
  
  // --- TEST THRESHOLD: Darkness creates 85% of the risk score ---
  float riskScore = (gasNorm * 0.10) + (tempNorm * 0.05) + (darknessNorm * 0.85);
  
  int riskLevel;
  if (dhtFailed) {
    riskLevel = 3;  
  } else if (riskScore <= 0.30) {
    riskLevel = 0;  // Normal
  } else if (riskScore <= 0.65) {
    riskLevel = 1;  // Warning
  } else {
    riskLevel = 2;  // Critical
  }
  
  unsigned long now = millis();
  unsigned long repeatDelay;

  // VOICE REPEAT TIMERS
  if (riskLevel == 0) repeatDelay = 60000;      // Normal: Every 60s
  else if (riskLevel == 1) repeatDelay = 10000; // Warning: Every 10s
  else if (riskLevel == 2) repeatDelay = 0;     // Critical: Instantly repeats
  else repeatDelay = 15000;                     // Error: Every 15s

  if (riskLevel != lastRiskLevel) {
    playVoice(riskLevel);
    lastVoiceTime = now;
    lastRiskLevel = riskLevel;
  } else if (now - lastVoiceTime >= repeatDelay) {
    playVoice(riskLevel);
    lastVoiceTime = now;
  }
  
  String currentStatusMessage = "";
  switch(riskLevel) {
    case 0: currentStatusMessage = "NORMAL - Air safe"; break;
    case 1: currentStatusMessage = "WARNING - Danger Rising"; break;
    case 2: currentStatusMessage = "CRITICAL - EVACUATE"; break;
    case 3: currentStatusMessage = "ERROR - Sensor failure"; break;
  }

  // --- SERIAL MONITOR PRINTING ---
  Serial.println("\n========= REAL-TIME DATA =========");
  Serial.print("MQ2 Raw: "); Serial.println(gasRaw);
  Serial.print("Gas PPM: "); Serial.print(gasPPM, 1); Serial.println(" ppm");
  
  Serial.print("LDR Raw: "); Serial.print(ldrRaw); 
  if (ldrRaw > 260) Serial.println(" (Bright/Normal)");
  else if (ldrRaw > 100) Serial.println(" (Dim/Warning)");
  else Serial.println(" (Dark/Critical)");
  
  Serial.print("Darkness Level: "); Serial.print(darknessNorm * 100, 1); Serial.println(" %");
  
  if (!dhtFailed) {
    Serial.print("Temp: "); Serial.print(temperature, 1); Serial.println(" °C");
    Serial.print("Humidity: "); Serial.print(humidity, 1); Serial.println(" %");
  }
  Serial.print("Total Risk Score: "); Serial.println(riskScore, 3);
  Serial.println("STATUS: " + currentStatusMessage);

  // --- STRICT CLOUD DASHBOARD PUSH ---
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) { 
    FirebaseJson json;
    
    json.set("gasPPM", gasPPM);
    json.set("riskScore", riskScore);
    json.set("riskLevel", riskLevel);
    json.set("statusMessage", currentStatusMessage);
    // Send light level to dashboard (invert darkness so 100% = bright)
    json.set("lightPercent", (1.0 - darknessNorm) * 100.0); 
    json.set("uptime", millis()); 

    if (!dhtFailed) {
      json.set("temperature", temperature);
      json.set("humidity", humidity);
    } else {
      json.set("temperature", 0.0);
      json.set("humidity", 0.0);
    }

    if (Firebase.RTDB.setJSON(&fbdo, dbPath.c_str(), &json)) {
      Serial.println("☁️ Data synced to Cloud: " + dbPath);
    } else {
      Serial.print("❌ Cloud Sync Failed: ");
      Serial.println(fbdo.errorReason());
    }
  } else {
    Serial.println("⚠️ Waiting for WiFi/Cloud connection...");
  }
}
// --- NEW BEEP GENERATOR ---
void playBeep(int durationMs, int frequencyHz) {
  dac_output_enable(DAC_CHANNEL_1);
  int halfPeriod = 1000000 / frequencyHz / 2; // Calculate wavelength
  unsigned long start = millis();
  
  while (millis() - start < durationMs) {
    dac_output_voltage(DAC_CHANNEL_1, 255); // Speaker cone OUT
    delayMicroseconds(halfPeriod);
    dac_output_voltage(DAC_CHANNEL_1, 0);   // Speaker cone IN
    delayMicroseconds(halfPeriod);
  }
  dac_output_disable(DAC_CHANNEL_1);
}
void setup() {
  Serial.begin(115200);
  delay(1500);

  // --- WIFI INIT ---
  Serial.print("\nConnecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false); 

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n✅ WiFi Connected!");

  // --- NTP TIME SYNC ---
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time for Google SSL");
  time_t nowTime = time(nullptr);
  while (nowTime < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    nowTime = time(nullptr);
  }
  Serial.println("\n⏰ Time Synced!");

  // --- GENERATE UNIQUE KIT PAIRING CODE ---
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  kitID = "NEXUS-" + mac.substring(6); 
  dbPath = "/devices/" + kitID + "/sensors"; 

  Serial.println("\n==================================");
  Serial.println("🔑 YOUR UNIQUE KIT PAIRING CODE:");
  Serial.println(">> " + kitID + " <<");
  Serial.println("==================================\n");

  // --- FIREBASE INIT ---
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  fbdo.setResponseSize(2048);
  fbdo.setBSSLBufferSize(4096, 1024);
  config.timeout.socketConnection = 10 * 1000;
  config.timeout.serverResponse = 10 * 1000;
  
  config.token_status_callback = tokenStatusCallback; 
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true); 

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("✅ Firebase Auth Successful");
  } else {
    Serial.printf("❌ Firebase Auth Failed: %s\n", config.signer.signupError.message.c_str());
  }
  
  Serial.println("\n🎯 AI VOICE SAFETY SYSTEM");
  Serial.println("=========================");
  
  dht.begin();
  calibrateMQ2();
  warmupStartTime = millis();
  
  Serial.println("\n✅ SYSTEM READY - Monitoring Active");
  Serial.println("=========================\n");
}

void loop() {
  unsigned long now = millis();
  static unsigned long lastRead = 0;
  
  if (now - lastRead >= 5000) {
    lastRead = now;
    detectAndAlert();
  }
}
