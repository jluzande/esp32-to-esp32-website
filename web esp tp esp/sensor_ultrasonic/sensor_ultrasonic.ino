/*
 * ============================================
 *  ESP32 #1 — Ultrasonic Sensor Node (HC-SR04)
 * ============================================
 *
 * Reads distance from HC-SR04 every 500ms and
 * sends it via UDP to the hub node.
 *
 * Connects to the Hub's WiFi Access Point.
 *
 * Wiring:
 *   HC-SR04 VCC  → ESP32 5V
 *   HC-SR04 GND  → ESP32 GND
 *   HC-SR04 TRIG → ESP32 GPIO 5
 *   HC-SR04 ECHO → ESP32 GPIO 18
 *
 * UDP Packet Format: "U:<distance_cm>"
 * Example: "U:25.3"
 */

#include <WiFi.h>
#include <WiFiUdp.h>

// ========== CONFIGURATION ==========i
// WiFi credentials — Connect to the Hub's Access Point
const char *WIFI_SSID = "JARVIS";
const char *WIFI_PASSWORD = "kitty1234";

// UDP settings — send directly to the hub's AP IP
const int UDP_PORT = 4210;
const IPAddress HUB_IP(192, 168, 4, 1);

// Sensor pins
const int TRIG_PIN = 5;
const int ECHO_PIN = 18;

// Timing
const unsigned long SEND_INTERVAL_MS = 500; // Send every 500ms
// ====================================

WiFiUDP udp;
unsigned long lastSendTime = 0;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=================================");
  Serial.println(" ESP32 #1 — Ultrasonic Sensor Node");
  Serial.println("=================================");

  // Configure sensor pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

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

    // Read distance
    float distance = readDistanceCm();

    // Build and send UDP packet
    String packet = "U:" + String(distance, 1);

    udp.beginPacket(HUB_IP, UDP_PORT);
    udp.print(packet);
    udp.endPacket();

    Serial.println("[TX] " + packet);
  }
}

/**
 * Read distance in cm from the HC-SR04 sensor.
 * Returns -1.0 if no echo received (out of range).
 */
float readDistanceCm() {
  // Send a 10µs trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Measure echo duration (timeout at 30ms ≈ ~500cm max)
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1.0; // No echo — out of range
  }

  // Speed of sound ≈ 0.0343 cm/µs, round-trip so divide by 2
  float distance = (duration * 0.0343) / 2.0;
  return distance;
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
