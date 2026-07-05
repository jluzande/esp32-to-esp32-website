/*
 * ==========================================
 *  ESP32 #3 — Sound Sensor Node (KY-038)
 *            + Servo Motor
 * ==========================================
 * 
 * Reads analog sound level from KY-038 every 500ms
 * and sends via UDP to the hub node.
 * Also controls a servo motor that can be toggled
 * remotely via UDP command from the hub.
 * 
 * Connects to the Hub's WiFi Access Point.
 * 
 * Wiring:
 *   KY-038 VCC → ESP32 3.3V
 *   KY-038 GND → ESP32 GND
 *   KY-038 A0  → ESP32 GPIO 34 (ADC input, input-only pin)
 *   Servo SIG  → ESP32 GPIO 26
 *   Servo VCC  → 5V (external power recommended for larger servos)
 *   Servo GND  → GND
 * 
 * UDP Packet Format (TX): "S:<analog_value>"
 * Example: "S:512"
 * 
 * UDP Command (RX): "CMD:SERVO_STOP/FWD/REV/TICK"
 * 
 * Required Library:
 *   Install "ESP32Servo" by Kevin Harrington via Arduino Library Manager.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>

// ========== CONFIGURATION ==========
// WiFi credentials — Connect to the Hub's Access Point
const char* WIFI_SSID     = "JARVIS";
const char* WIFI_PASSWORD = "kitty1234";

// UDP settings — send directly to the hub's AP IP
const int       UDP_PORT = 4210;
const IPAddress HUB_IP(192, 168, 4, 1);

// Sensor pin
const int SOUND_PIN = 34;  // GPIO 34 — ADC1_CH6, input-only

// Servo pin
const int SERVO_PIN = 26;  // GPIO 26 — PWM capable

// Timing
const unsigned long SEND_INTERVAL_MS = 500;   // Send every 500ms
const unsigned long SAMPLE_WINDOW_MS = 50;    // Sample window for peak detection
// ====================================

WiFiUDP udp;
unsigned long lastSendTime = 0;

// Servo
Servo myServo;
int currentServoMode = 0; // 0=Stop, 1=Fwd, 2=Rev, 3=Tick
int lastServoMode = -1;
unsigned long lastTickTime = 0;
char cmdBuffer[64];       // Buffer for incoming UDP commands

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("====================================");
  Serial.println(" ESP32 #3 — Sound Sensor Node");
  Serial.println("====================================");

  // Configure ADC
  // GPIO 34 is input-only by hardware, no need for pinMode
  // But we set it explicitly for clarity
  analogReadResolution(12);  // 12-bit ADC: 0–4095
  analogSetAttenuation(ADC_11db);  // Full range: 0–3.3V

  Serial.println("[SOUND] Sensor initialized on GPIO " + String(SOUND_PIN));

  // Initialize Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  myServo.setPeriodHertz(50);               // Standard 50Hz servo
  // We start detached so the continuous servo perfectly stops
  Serial.println("[SERVO] Initialized on GPIO " + String(SERVO_PIN) + " in STOP mode");

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

  // --- Check for incoming UDP commands (servo mode) ---
  checkForCommands();

  // --- Handle Servo State Machine ---
  unsigned long now = millis();
  
  if (currentServoMode != lastServoMode && currentServoMode != 3) {
    lastServoMode = currentServoMode;
    if (currentServoMode == 0) { // STOP
      myServo.detach(); // Force perfect stop by killing PWM
    } else if (currentServoMode == 1) { // FORWARD
      if (!myServo.attached()) myServo.attach(SERVO_PIN, 500, 2400);
      myServo.write(180);
    } else if (currentServoMode == 2) { // REVERSE
      if (!myServo.attached()) myServo.attach(SERVO_PIN, 500, 2400);
      myServo.write(0);
    }
  } else if (currentServoMode == 3) {
    lastServoMode = 3; // ensure it doesn't trigger above block on next loop
    if (now - lastTickTime >= 1000) {
      lastTickTime = now;
      
      // Beat 1 (Lub)
      if (!myServo.attached()) myServo.attach(SERVO_PIN, 500, 2400);
      myServo.write(180); 
      delay(80);          
      myServo.detach();   
      
      delay(120); // Short pause between beats
      
      // Beat 2 (Dub)
      myServo.attach(SERVO_PIN, 500, 2400);
      myServo.write(180); 
      delay(80);          
      myServo.detach();   
    }
  }

  // --- Send sound sensor data ---
  now = millis();
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;

    // Read peak sound level over a short sample window
    int soundLevel = readPeakSoundLevel();

    // Build and send UDP packet
    String packet = "S:" + String(soundLevel);

    udp.beginPacket(HUB_IP, UDP_PORT);
    udp.print(packet);
    udp.endPacket();

    Serial.println("[TX] " + packet);
  }
}

/**
 * Check for incoming UDP command packets.
 * Listens for servo mode commands from the hub.
 */
void checkForCommands() {
  int packetSize = udp.parsePacket();
  while (packetSize > 0) {
    int len = udp.read(cmdBuffer, sizeof(cmdBuffer) - 1);
    if (len > 0) {
      cmdBuffer[len] = '\0';
      String cmd = String(cmdBuffer);
      cmd.trim();

      if (cmd == "CMD:SERVO_STOP" && currentServoMode != 0) {
        currentServoMode = 0;
        Serial.println("[SERVO] Mode: STOP");
      } else if (cmd == "CMD:SERVO_FWD" && currentServoMode != 1) {
        currentServoMode = 1;
        Serial.println("[SERVO] Mode: FORWARD");
      } else if (cmd == "CMD:SERVO_REV" && currentServoMode != 2) {
        currentServoMode = 2;
        Serial.println("[SERVO] Mode: REVERSE");
      } else if (cmd == "CMD:SERVO_TICK" && currentServoMode != 3) {
        currentServoMode = 3;
        Serial.println("[SERVO] Mode: TICKING");
      }
    }
    packetSize = udp.parsePacket();
  }
}

/**
 * Sample the sound sensor rapidly over SAMPLE_WINDOW_MS 
 * and return the peak-to-peak amplitude.
 * This gives a better representation of sound loudness
 * than a single analogRead().
 */
int readPeakSoundLevel() {
  unsigned long startTime = millis();
  int signalMax = 0;
  int signalMin = 4095;

  // Collect samples during the window
  while (millis() - startTime < SAMPLE_WINDOW_MS) {
    int sample = analogRead(SOUND_PIN);
    if (sample > signalMax) signalMax = sample;
    if (sample < signalMin) signalMin = sample;
  }

  // Peak-to-peak amplitude
  int peakToPeak = signalMax - signalMin;
  return peakToPeak;
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
