/*
 * ================================================
 *  ESP32 #2 — Temperature & Humidity Node (DHT11)
 * ================================================
 * 
 * Reads temperature and humidity from DHT11 every 2 seconds
 * and sends via UDP to the hub node.
 * 
 * Connects to the Hub's WiFi Access Point.
 * 
 * Wiring:
 *   DHT11 VCC  → ESP32 3.3V
 *   DHT11 GND  → ESP32 GND
 *   DHT11 DATA → ESP32 GPIO 4
 * 
 * UDP Packet Format: "T:<temp>,H:<humidity>"
 * Example: "T:28.5,H:65.0"
 * 
 * Required Library:
 *   Install "DHT sensor library" by Adafruit via Arduino Library Manager.
 *   It will also prompt to install "Adafruit Unified Sensor" — install that too.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <DHT.h>

// ========== CONFIGURATION ==========
// WiFi credentials — Connect to the Hub's Access Point
const char* WIFI_SSID     = "JARVIS";
const char* WIFI_PASSWORD = "kitty1234";

// UDP settings — send directly to the hub's AP IP
const int       UDP_PORT = 4210;
const IPAddress HUB_IP(192, 168, 4, 1);

// Sensor pin and type
const int DHT_PIN  = 4;
const int DHT_TYPE = DHT11;  // Change to DHT22 if using that sensor

// Timing — DHT11 needs at least 1 second between reads; we use 2s for stability
const unsigned long SEND_INTERVAL_MS = 2000;
// ====================================

WiFiUDP udp;
DHT dht(DHT_PIN, DHT_TYPE);
unsigned long lastSendTime = 0;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("==========================================");
  Serial.println(" ESP32 #2 — Temp & Humidity Sensor Node");
  Serial.println("==========================================");

  // Initialize DHT sensor
  dht.begin();
  Serial.println("[DHT] Sensor initialized on GPIO " + String(DHT_PIN));

  // Connect to Hub AP
  connectToWiFi();

  // Start UDP
  udp.begin(UDP_PORT);
  Serial.println("[UDP] Ready on port " + String(UDP_PORT));
}

void loop() {
  // Reconnect WiFi if lost
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Connection lost. Reconnecting...");
    connectToWiFi();
  }

  unsigned long now = millis();
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;

    // Read temperature and humidity
    float temperature = dht.readTemperature();  // Celsius
    float humidity    = dht.readHumidity();

    // Check for read errors
    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("[DHT] ERROR: Failed to read sensor!");
      return;  // Skip this cycle, try again next interval
    }

    // Build and send UDP packet
    String packet = "T:" + String(temperature, 1) + ",H:" + String(humidity, 1);

    udp.beginPacket(HUB_IP, UDP_PORT);
    udp.print(packet);
    udp.endPacket();

    Serial.println("[TX] " + packet);
  }
}

/**
 * Connect to WiFi (Hub AP) with retry logic.
 */
void connectToWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("[WiFi] Connected to Hub AP!");
    Serial.println("[WiFi] IP Address: " + WiFi.localIP().toString());
  } else {
    Serial.println();
    Serial.println("[WiFi] Failed to connect. Will retry...");
  }
}
