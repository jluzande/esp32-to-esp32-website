/*
 * ================================================================
 *  ESP32 #4 — Hello Kitty Hub (Web Dashboard + LEDs + Buzzer)
 * ================================================================
 *
 * Creates a WiFi Access Point and serves a Hello Kitty themed
 * web dashboard. Receives sensor data from 3 ESP32 nodes via UDP,
 * displays real-time data on the web page, controls 5 LEDs and
 * a passive buzzer based on sensor thresholds.
 *
 * Features:
 *   - WiFi AP mode (no router needed)
 *   - Hello Kitty themed web dashboard with 4 views
 *   - 5 LEDs: Red, Orange, Yellow, Green, Blue
 *   - Passive buzzer plays Hello Kitty melody on sensor screens
 *   - Real-time sensor data via AJAX polling
 *   - Servo control forwarding via UDP
 *   - CSV data logging with download from web dashboard
 *
 * Screens:
 *   0 — All Sensors Overview (buzzer silent)
 *   1 — Ultrasonic Detail
 *   2 — Temperature & Humidity Detail
 *   3 — Sound Detail
 *   4 — Servo Control
 *
 * LED Behavior:
 *   LEDs respond to ALL sensors simultaneously.
 *   Each sensor maps its value to one of 5 colors:
 *     Red (hot/close/loud) → Blue (cold/far/quiet)
 *   Multiple LEDs can be on at once.
 *
 * Buzzer Behavior:
 *   Plays Hello Kitty melody only on dedicated sensor screens.
 *   Tempo varies with alert level (faster = more urgent).
 *   Silent on the overview screen.
 *
 * Wiring:
 *   LED Red      → GPIO 13 (with 220Ω resistor)
 *   LED Orange   → GPIO 12 (with 220Ω resistor)
 *   LED Yellow   → GPIO 14 (with 220Ω resistor)
 *   LED Green    → GPIO 27 (with 220Ω resistor)
 *   LED Blue     → GPIO 26 (with 220Ω resistor)
 *   Buzzer +     → GPIO 25
 *   Buzzer -     → GND
 *
 * Connect to: http://192.168.4.1 after joining "HelloKittyHub" WiFi
 *
 * No extra libraries required beyond the ESP32 Arduino core.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>

// ======================== CONFIGURATION ========================

// WiFi Access Point
const char* AP_SSID = "JARVIS";
const char* AP_PASS = "kitty1234";

// LED Pins
#define LED_RED 13
#define LED_ORANGE 12
#define LED_YELLOW 14
#define LED_GREEN 27
#define LED_BLUE 26

// Buzzer Pin
#define BUZZER_PIN 25

// UDP
#define UDP_PORT 4210

// Sensor timeout — show "--" if no data for this long
#define SENSOR_TIMEOUT_MS 5000

// LED update rate
#define LED_UPDATE_MS 100

// Servo broadcast IP (AP subnet broadcast)
const IPAddress BROADCAST_IP(192, 168, 4, 255);

// ======================== NOTE DEFINITIONS ========================
#define NOTE_C4 262
#define NOTE_D4 294
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_G4 392
#define NOTE_A4 440
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_D5 587
#define NOTE_E5 659
#define NOTE_F5 698
#define NOTE_G5 784

// ======================== HELLO KITTY MELODY ========================
// A cute, playful melody inspired by Hello Kitty's kawaii style
const int hkMelody[] = {
  NOTE_G4, NOTE_E5, NOTE_D5, NOTE_C5,  // phrase 1 — bouncy opening
  NOTE_G4, NOTE_E5, NOTE_D5, 0,
  NOTE_C5, NOTE_D5, NOTE_E5, NOTE_G5,  // phrase 2 — ascending
  NOTE_E5, NOTE_D5, NOTE_C5, 0,
  NOTE_E5, NOTE_G5, NOTE_E5, NOTE_C5,  // phrase 3 — playful
  NOTE_D5, NOTE_E5, NOTE_D5, 0,
  NOTE_C5, NOTE_E5, NOTE_G5, NOTE_E5,  // phrase 4 — cheerful ending
  NOTE_D5, NOTE_C5, 0, 0
};

const int hkDurations[] = {
  220, 220, 220, 440,  // phrase 1
  220, 220, 440, 220,
  220, 220, 220, 440,  // phrase 2
  220, 220, 440, 330,
  220, 220, 220, 220,  // phrase 3
  220, 220, 440, 330,
  220, 220, 220, 220,  // phrase 4
  220, 440, 330, 550
};

const int HK_LENGTH = 32;

// ======================== OBJECTS ========================
WebServer server(80);
WiFiUDP udp;

// ======================== SENSOR DATA ========================
float ultraDistance = -1.0;
float temperature = 0.0;
float humidity = 0.0;
int soundLevel = -1;
bool tempIsNan = true;

unsigned long ultraLastRx = 0;
unsigned long tempLastRx = 0;
unsigned long soundLastRx = 0;

// ======================== UI STATE ========================
int currentScreen = 0;     // 0=All, 1=Ultra, 2=Temp, 3=Sound
int currentServoMode = 0;  // 0=Stop, 1=Fwd, 2=Rev, 3=Tick

// ======================== BUZZER STATE ========================
bool buzzerPlaying = false;
int melodyIndex = 0;
unsigned long noteStartTime = 0;

// ======================== TIMING ========================
unsigned long lastLedUpdate = 0;
unsigned long lastServoTxTime = 0;
unsigned long lastLogTime = 0;

// ======================== UDP BUFFER ========================
char packetBuffer[128];

// ======================== DATA LOGGING ========================
// Circular buffer for CSV data export
// Stores ~1 hour of data at 5-second intervals (720 entries)
#define LOG_INTERVAL_MS 5000
#define MAX_LOG_ENTRIES 720

struct LogEntry {
  unsigned long timestamp;  // millis since boot
  float distance;
  float temp;
  float hum;
  int sound;
  bool uOk;
  bool tOk;
  bool sOk;
};

LogEntry logBuffer[MAX_LOG_ENTRIES];
int logCount = 0;            // total entries stored (max MAX_LOG_ENTRIES)
int logHead = 0;             // next write position (circular)
unsigned long bootTime = 0;  // millis at boot for timestamp reference

// ======================== HTML PAGE (PROGMEM) ========================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no">
<meta name="theme-color" content="#FF6B9D">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>Hello Kitty Sensor Hub</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{
--pk:#FF6B9D;--pkd:#FF4E8C;--pkl:#FFB5D1;--pkbg:#FFF0F5;
--card:#fff;--txt:#4A3040;--txtl:#9B7A8D;
--red:#FF4E6A;--org:#FF9F43;--yel:#FECA57;--grn:#2ED573;--blu:#45AAF2;
}
body{
font-family:'Comic Sans MS','Chalkboard SE','Bradley Hand',cursive;
background:linear-gradient(145deg,#FFF0F5 0%,#FFE4EC 40%,#FFF5F7 100%);
background-attachment:fixed;color:var(--txt);
min-height:100vh;padding-bottom:68px;
-webkit-tap-highlight-color:transparent;
overflow-x:hidden;
}
/* Floating decorations */
.deco{position:fixed;pointer-events:none;opacity:.07;z-index:0;
animation:float 6s ease-in-out infinite}
@keyframes float{0%,100%{transform:translateY(0) rotate(0deg)}
50%{transform:translateY(-12px) rotate(5deg)}}

/* ===== HEADER ===== */
.hdr{
background:linear-gradient(135deg,#FF85B1,#FF6B9D,#FF4E8C);
padding:10px 16px;text-align:center;color:#fff;
box-shadow:0 4px 18px rgba(255,78,140,.3);
position:sticky;top:0;z-index:50;
display:flex;align-items:center;justify-content:center;gap:8px;
}
.hdr h1{font-size:1.05em;letter-spacing:.4px;text-shadow:1px 1px 3px rgba(0,0,0,.12)}
.hdr svg{flex-shrink:0;filter:drop-shadow(1px 1px 2px rgba(0,0,0,.1))}
.conn{position:absolute;right:10px;top:50%;transform:translateY(-50%);
font-size:.58em;padding:2px 7px;border-radius:10px;
background:rgba(255,255,255,.18);backdrop-filter:blur(3px)}
.conn.ok{background:rgba(46,213,115,.3)}.conn.err{background:rgba(255,78,106,.4)}

/* ===== SCREENS ===== */
.scrs{padding:10px 12px;position:relative;z-index:1}
.scr{display:none;animation:fi .35s ease}
.scr.on{display:block}
@keyframes fi{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}

/* ===== CARDS ===== */
.cgrid{display:grid;grid-template-columns:1fr;gap:10px}
@media(min-width:480px){.cgrid{grid-template-columns:1fr 1fr}}
@media(min-width:720px){.cgrid{grid-template-columns:1fr 1fr 1fr}}

.crd{background:var(--card);border-radius:16px;padding:14px;
box-shadow:0 4px 16px rgba(255,107,157,.12);
border:1.5px solid #FFE4EC;transition:all .25s ease;
cursor:pointer;position:relative;overflow:hidden}
.crd:active{transform:scale(.97)}
.crd::before{content:'';position:absolute;top:0;left:0;right:0;height:3px;
background:linear-gradient(90deg,var(--pk),var(--pkl),var(--pk))}
.ch{display:flex;align-items:center;gap:7px;margin-bottom:8px}
.ci{font-size:1.4em}.ct{font-size:.82em;color:var(--txtl);font-weight:700}
.co{width:7px;height:7px;border-radius:50%;margin-left:auto}
.co.y{background:var(--grn);box-shadow:0 0 5px var(--grn);animation:pls 2s infinite}
.co.n{background:#ccc;animation:none}
@keyframes pls{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.5;transform:scale(1.4)}}
.cv{font-size:1.8em;font-weight:700;color:var(--pkd);line-height:1.1}
.cu{font-size:.65em;color:var(--txtl)}
.csub{font-size:.75em;color:var(--txtl);margin:2px 0}
.cst{font-size:.68em;font-weight:700;padding:2px 8px;border-radius:8px;
display:inline-block;margin:4px 0}

/* ===== BARS ===== */
.bar{background:#FFF0F5;border-radius:8px;height:8px;overflow:hidden;
border:1px solid #FFE4EC;margin-top:6px}
.bf{height:100%;border-radius:7px;transition:width .5s ease,background .4s ease;
background:linear-gradient(90deg,var(--pk),var(--pkl))}

/* ===== DETAIL VIEW ===== */
.dtl{background:var(--card);border-radius:20px;padding:20px 18px;
box-shadow:0 5px 20px rgba(255,107,157,.14);border:1.5px solid #FFE4EC;text-align:center}
.di{font-size:2.2em;margin-bottom:2px}
.dtl h2{color:var(--pkd);font-size:1em;margin-bottom:10px}
.bv{font-size:3.2em;font-weight:700;color:var(--pkd);line-height:1;
margin:8px 0;text-shadow:2px 2px 0 #FFE4EC;transition:color .4s ease}
.bu{font-size:.85em;color:var(--txtl);margin-bottom:10px}
.db{background:#FFF0F5;border-radius:10px;height:14px;overflow:hidden;
border:1.5px solid #FFE4EC;margin:8px 0}
.db .bf{height:100%;border-radius:8px}
.ds{font-size:.78em;font-weight:700;padding:4px 12px;border-radius:10px;
display:inline-block;margin:6px 0}
.hr{display:flex;align-items:center;justify-content:center;gap:6px;margin:8px 0}
.hv{font-size:1.6em;font-weight:700;color:var(--blu)}

/* ===== LED ROW ===== */
.lr{display:flex;gap:5px;align-items:center;justify-content:center;margin:8px 0}
.ld{width:12px;height:12px;border-radius:50%;border:1.5px solid #ddd;
transition:all .3s ease;opacity:.25}
.ld.a{opacity:1;transform:scale(1.25)}
.ld.r{background:var(--red)}.ld.r.a{border-color:var(--red);box-shadow:0 0 7px var(--red)}
.ld.o{background:var(--org)}.ld.o.a{border-color:var(--org);box-shadow:0 0 7px var(--org)}
.ld.y{background:var(--yel)}.ld.y.a{border-color:var(--yel);box-shadow:0 0 7px var(--yel)}
.ld.g{background:var(--grn)}.ld.g.a{border-color:var(--grn);box-shadow:0 0 7px var(--grn)}
.ld.b{background:var(--blu)}.ld.b.a{border-color:var(--blu);box-shadow:0 0 7px var(--blu)}

/* ===== SPARKLINE ===== */
.cw{margin:12px 0;padding:6px;background:#FFF8FA;border-radius:10px;
border:1px solid #FFE4EC}
.cw canvas{width:100%;height:55px;display:block}

/* ===== SERVO ===== */
.ss{margin-top:14px;padding:12px;background:#FFF8FA;border-radius:12px;
border:1px solid #FFE4EC}
.ss h3{color:var(--pkd);font-size:.88em;margin-bottom:8px}
.sbs{display:flex;gap:6px;flex-wrap:wrap;justify-content:center}
.sb{border:1.5px solid #FFE4EC;background:#fff;color:var(--pkd);
padding:7px 12px;border-radius:12px;font-family:inherit;
font-size:.75em;font-weight:700;cursor:pointer;transition:all .2s ease}
.sb:active{transform:scale(.93)}
.sb.a{background:var(--pk);color:#fff;border-color:var(--pkd)}

/* ===== STATUS ROW ===== */
.srow{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}
.mc{background:var(--card);border-radius:12px;padding:10px;
box-shadow:0 3px 12px rgba(255,107,157,.1);border:1px solid #FFE4EC;
text-align:center;font-size:.78em}
.mc .lbl{color:var(--txtl);display:block;margin-bottom:3px}
.mc .val{color:var(--pkd);font-weight:700;font-size:1.1em}

/* ===== DATA LOG CARD ===== */
.logcard{background:var(--card);border-radius:14px;padding:14px;
box-shadow:0 4px 16px rgba(255,107,157,.12);border:1.5px solid #FFE4EC;
margin-top:10px;text-align:center;position:relative;overflow:hidden}
.logcard::before{content:'';position:absolute;top:0;left:0;right:0;height:3px;
background:linear-gradient(90deg,var(--pk),var(--pkl),var(--pk))}
.logcard h3{color:var(--pkd);font-size:.9em;margin-bottom:8px}
.logcard .log-info{font-size:.75em;color:var(--txtl);margin-bottom:10px}
.logcard .log-count{font-size:1.6em;font-weight:700;color:var(--pkd);margin:4px 0}
.log-btns{display:flex;gap:8px;justify-content:center;flex-wrap:wrap}
.log-btn{border:1.5px solid #FFE4EC;background:#fff;color:var(--pkd);
padding:8px 16px;border-radius:12px;font-family:inherit;
font-size:.78em;font-weight:700;cursor:pointer;transition:all .2s ease}
.log-btn:active{transform:scale(.93)}
.log-btn.dl{background:linear-gradient(135deg,var(--pk),var(--pkd));color:#fff;
border-color:var(--pkd);box-shadow:0 3px 10px rgba(255,107,157,.25)}
.log-btn.dl:active{transform:scale(.93)}
.log-btn.clr{background:#FFF0F5;color:var(--red);border-color:#FFD6E5}

/* ===== BOTTOM NAV ===== */
.nav{position:fixed;bottom:0;left:0;right:0;display:flex;
justify-content:center;gap:3px;padding:6px 8px;
padding-bottom:max(6px,env(safe-area-inset-bottom));
background:rgba(255,255,255,.93);backdrop-filter:blur(8px);
-webkit-backdrop-filter:blur(8px);
border-top:1.5px solid #FFE4EC;z-index:100}
.nb{flex:1;border:none;background:#FFF0F5;color:var(--pk);
padding:6px 3px;border-radius:14px;font-family:inherit;
font-size:.65em;font-weight:700;cursor:pointer;transition:all .2s ease;
display:flex;flex-direction:column;align-items:center;gap:1px;max-width:85px}
.ni{font-size:1.2em}
.nb.a{background:linear-gradient(135deg,var(--pk),var(--pkd));color:#fff;
transform:scale(1.04);box-shadow:0 3px 10px rgba(255,107,157,.3)}
.nb:active:not(.a){background:#FFD6E5}

/* ===== BUZZER INDICATOR ===== */
.bz{font-size:.65em;color:var(--txtl);margin-top:6px}
.bz .on{color:var(--pk);font-weight:700}

/* ===== DESKTOP ENHANCEMENTS ===== */
@media(min-width:600px){
body{max-width:800px;margin:0 auto;border-left:1.5px solid #FFE4EC;border-right:1.5px solid #FFE4EC;box-shadow:0 0 30px rgba(255,107,157,.15)}
.hdr{border-radius:0 0 20px 20px;margin-bottom:15px}
.scrs{padding:20px 30px;min-height:calc(100vh - 140px);display:flex;align-items:center}
.scr{width:100%}
.nav{max-width:800px;margin:0 auto;border-radius:20px 20px 0 0;border-left:1.5px solid #FFE4EC;border-right:1.5px solid #FFE4EC}
.nb{font-size:.8em;padding:10px 5px}
.ni{font-size:1.4em}
.crd{padding:20px}
.ct{font-size:1em}
.cv{font-size:2.2em}
.dtl{padding:40px}
.di{font-size:3.5em}
.bv{font-size:4.5em}
.logcard{margin-top:20px;padding:25px}
.log-btn{font-size:.9em;padding:10px 20px}
}
</style>
</head>
<body>

<!-- Floating decorations -->
<div class="deco" style="top:12%;left:4%;font-size:2.8em">🎀</div>
<div class="deco" style="top:30%;right:6%;font-size:2em;animation-delay:2s">♡</div>
<div class="deco" style="top:55%;left:7%;font-size:2.2em;animation-delay:4s">✿</div>
<div class="deco" style="top:75%;right:4%;font-size:2.5em;animation-delay:1s">🎀</div>

<!-- Header -->
<div class="hdr">
<svg viewBox="0 0 100 90" width="38" height="34">
<polygon points="22,34 16,8 40,28" fill="#fff" stroke="#555" stroke-width="2.2" stroke-linejoin="round"/>
<polygon points="78,34 84,8 60,28" fill="#fff" stroke="#555" stroke-width="2.2" stroke-linejoin="round"/>
<ellipse cx="50" cy="54" rx="36" ry="32" fill="#fff" stroke="#555" stroke-width="2.2"/>
<ellipse cx="39" cy="50" rx="2.8" ry="3.8" fill="#555"/>
<ellipse cx="61" cy="50" rx="2.8" ry="3.8" fill="#555"/>
<ellipse cx="50" cy="57" rx="3" ry="2.2" fill="#FFC107" stroke="#555" stroke-width="1.2"/>
<line x1="17" y1="47" x2="34" y2="52" stroke="#555" stroke-width="1.2"/>
<line x1="15" y1="55" x2="34" y2="55" stroke="#555" stroke-width="1.2"/>
<line x1="17" y1="63" x2="34" y2="58" stroke="#555" stroke-width="1.2"/>
<line x1="83" y1="47" x2="66" y2="52" stroke="#555" stroke-width="1.2"/>
<line x1="85" y1="55" x2="66" y2="55" stroke="#555" stroke-width="1.2"/>
<line x1="83" y1="63" x2="66" y2="58" stroke="#555" stroke-width="1.2"/>
<ellipse cx="83" cy="20" rx="10" ry="6.5" fill="#FF6B9D" stroke="#555" stroke-width="1.2" transform="rotate(10,83,20)"/>
<ellipse cx="74" cy="14" rx="8.5" ry="5.5" fill="#FF6B9D" stroke="#555" stroke-width="1.2" transform="rotate(-12,74,14)"/>
<circle cx="78" cy="17" r="2.8" fill="#FFC107" stroke="#555" stroke-width=".8"/>
</svg>
<h1>Hello Kitty Sensor Hub</h1>
<div class="conn ok" id="conn">● Online</div>
</div>

<!-- Screens -->
<div class="scrs">

<!-- ===== Screen 0: Overview ===== -->
<div class="scr on" id="s0">
<div class="cgrid">

<div class="crd" onclick="sw(1)">
<div class="ch"><span class="ci">📏</span><span class="ct">Distance</span><span class="co n" id="uo"></span></div>
<div class="cv" id="uv">--</div>
<div class="cst" id="us">Waiting...</div>
<div class="bar"><div class="bf" id="ub" style="width:0"></div></div>
</div>

<div class="crd" onclick="sw(2)">
<div class="ch"><span class="ci">🌡️</span><span class="ct">Temperature</span><span class="co n" id="to"></span></div>
<div class="cv" id="tv">--</div>
<div class="csub" id="hv">💧 --</div>
<div class="cst" id="ts">Waiting...</div>
<div class="bar"><div class="bf" id="tb" style="width:0"></div></div>
</div>

<div class="crd" onclick="sw(3)">
<div class="ch"><span class="ci">🔊</span><span class="ct">Sound</span><span class="co n" id="so"></span></div>
<div class="cv" id="sv">--</div>
<div class="cst" id="ss2">Waiting...</div>
<div class="bar"><div class="bf" id="sb" style="width:0"></div></div>
</div>

</div>
<div class="srow">
<div class="mc" onclick="sw(4)" style="cursor:pointer;border-color:var(--pk);box-shadow:0 0 8px rgba(255,107,157,.2)"><span class="lbl">⚙️ Servo</span><span class="val" id="ov">OFF</span></div>
<div class="mc">
<span class="lbl">💡 LEDs</span>
<div class="lr">
<span class="ld r" id="or"></span><span class="ld o" id="oo"></span>
<span class="ld y" id="oy"></span><span class="ld g" id="og"></span><span class="ld b" id="ob"></span>
</div>
</div>
</div>

<div class="logcard">
<h3>📋 Data Log</h3>
<div class="log-count" id="lc">0</div>
<div class="log-info">entries recorded (every 5s, max 720 ≈ 1hr)</div>
<div class="log-info" id="lt">Uptime: 00:00:00</div>
<div class="log-btns">
<button class="log-btn dl" onclick="dlCSV()">📥 Download CSV</button>
<button class="log-btn clr" onclick="clrLog()">🗑️ Clear Log</button>
</div>
</div>

<div class="bz" style="text-align:center;margin-top:8px">🔔 Buzzer: <span id="bz0">Silent on overview</span></div>
</div>

<!-- ===== Screen 1: Ultrasonic Detail ===== -->
<div class="scr" id="s1">
<div class="dtl">
<div class="di">📏</div>
<h2>Ultrasonic Distance</h2>
<div class="bv" id="duv">--</div>
<div class="bu">cm</div>
<div class="db"><div class="bf" id="dub" style="width:0"></div></div>
<div class="ds" id="dus">Waiting for data...</div>
<div class="lr">
<span class="ld r" id="ur"></span><span class="ld o" id="uor"></span>
<span class="ld y" id="uy"></span><span class="ld g" id="ugr"></span><span class="ld b" id="ubl"></span>
</div>
<div class="cw"><canvas id="cu" width="300" height="55"></canvas></div>
<div class="bz">🔔 Buzzer: <span class="on" id="bz1">Playing melody ♪</span></div>
</div>
</div>

<!-- ===== Screen 2: Temp & Humidity Detail ===== -->
<div class="scr" id="s2">
<div class="dtl">
<div class="di">🌡️</div>
<h2>Temperature & Humidity</h2>
<div class="bv" id="dtv">--</div>
<div class="bu">°C</div>
<div class="db"><div class="bf" id="dtb" style="width:0"></div></div>
<div class="hr"><span style="font-size:1.3em">💧</span><span class="hv" id="dhv">--</span><span class="bu">%</span></div>
<div class="ds" id="dts">Waiting for data...</div>
<div class="lr">
<span class="ld r" id="tr"></span><span class="ld o" id="tor"></span>
<span class="ld y" id="ty"></span><span class="ld g" id="tgr"></span><span class="ld b" id="tbl"></span>
</div>
<div class="cw"><canvas id="ct" width="300" height="55"></canvas></div>
<div class="bz">🔔 Buzzer: <span class="on" id="bz2">Playing melody ♪</span></div>
</div>
</div>

<!-- ===== Screen 3: Sound Detail ===== -->
<div class="scr" id="s3">
<div class="dtl">
<div class="di">🔊</div>
<h2>Sound Level</h2>
<div class="bv" id="dsv">--</div>
<div class="bu">level</div>
<div class="db"><div class="bf" id="dsb" style="width:0"></div></div>
<div class="ds" id="dss">Waiting for data...</div>
<div class="lr">
<span class="ld r" id="sr"></span><span class="ld o" id="sor"></span>
<span class="ld y" id="sy"></span><span class="ld g" id="sgr"></span><span class="ld b" id="sbl"></span>
</div>
<div class="cw"><canvas id="cs" width="300" height="55"></canvas></div>
<div class="bz">🔔 Buzzer: <span class="on" id="bz3">Playing melody ♪</span></div>
</div>
</div>

<!-- ===== Screen 4: Servo Control ===== -->
<div class="scr" id="s4">
<div class="dtl">
<div class="di" style="font-size:3.5em;margin:15px 0">⚙️</div>
<h2>Servo Control Mode</h2>
<div class="ss" style="margin-top:20px;padding:20px;border-width:2px">
<div class="sbs" style="display:flex;flex-direction:column;gap:12px">
<button class="sb a" id="sv0" onclick="srv(0)" style="font-size:1.1em;padding:12px">🛑 Off</button>
<button class="sb" id="sv1" onclick="srv(1)" style="font-size:1.1em;padding:12px">▶️ Forward</button>
<button class="sb" id="sv2" onclick="srv(2)" style="font-size:1.1em;padding:12px">◀️ Reverse</button>
<button class="sb" id="sv3" onclick="srv(3)" style="font-size:1.1em;padding:12px">💓 Heartbeat</button>
</div>
</div>
<div class="bz" style="margin-top:15px">🔔 Buzzer: <span id="bz4">Silent</span></div>
</div>
</div>

</div>

<!-- Bottom Navigation -->
<nav class="nav">
<button class="nb a" id="n0" onclick="sw(0)"><span class="ni">🏠</span>All</button>
<button class="nb" id="n1" onclick="sw(1)"><span class="ni">📏</span>Dist</button>
<button class="nb" id="n2" onclick="sw(2)"><span class="ni">🌡️</span>Temp</button>
<button class="nb" id="n3" onclick="sw(3)"><span class="ni">🔊</span>Sound</button>
<button class="nb" id="n4" onclick="sw(4)"><span class="ni">⚙️</span>Servo</button>
</nav>

<script>
var cs=0,sm=0;
var H={u:[],t:[],s:[]};
var MX=20;

function sw(n){
cs=n;
for(var i=0;i<5;i++){
document.getElementById('s'+i).className='scr'+(i===n?' on':'');
document.getElementById('n'+i).className='nb'+(i===n?' a':'');
}
fetch('/api/screen?s='+n,{method:'POST'}).catch(function(){});
}

function srv(m){
sm=m;
fetch('/api/servo?m='+m,{method:'POST'}).catch(function(){});
for(var i=0;i<4;i++){
var el=document.getElementById('sv'+i);
if(el)el.className='sb'+(i===m?' a':'');
}
}

function poll(){
fetch('/api/data').then(function(r){return r.json()}).then(function(d){
upd(d);scon(true);
}).catch(function(){scon(false)});
}

function scon(ok){
var b=document.getElementById('conn');
b.textContent=ok?'\u25CF Online':'\u25CF Offline';
b.className='conn '+(ok?'ok':'err');
}

function $(id){return document.getElementById(id)}

function st(id,v){var e=$(id);if(e)e.textContent=v}
function sb(id,p,c){var e=$(id);if(e){e.style.width=p+'%';e.style.background=c}}
function ss(id,t,c){var e=$(id);if(e){e.textContent=t;e.style.background=c+'22';e.style.color=c}}
function so(id,ok){var e=$(id);if(e)e.className='co '+(ok?'y':'n')}

function uc(v){
if(v<=5)return'#FF4E6A';if(v<=10)return'#FF9F43';
if(v<=20)return'#FECA57';if(v<=50)return'#2ED573';return'#45AAF2';
}
function us2(v){
if(v<=5)return'\u26A0\uFE0F VERY CLOSE!';if(v<=10)return'\u26A0\uFE0F Close';
if(v<=20)return'Moderate';if(v<=50)return'\u2705 Safe';return'\u2705 Far Away';
}
function ul(v){if(v<=5)return'r';if(v<=10)return'o';if(v<=20)return'y';if(v<=50)return'g';return'b'}

function tc(v){
if(v>=26)return'#FF4E6A';if(v>=22)return'#FF9F43';
if(v>=19)return'#FECA57';if(v>=17)return'#2ED573';return'#45AAF2';
}
function ts2(v){
if(v>=26)return'\uD83D\uDD25 HOT!';if(v>=22)return'\u2600\uFE0F Warm';
if(v>=19)return'\uD83C\uDF24 Comfortable';if(v>=17)return'\uD83C\uDF25 Cool';return'\u2744\uFE0F COLD!';
}
function tl(v){if(v>=26)return'r';if(v>=22)return'o';if(v>=19)return'y';if(v>=17)return'g';return'b'}

function sc(v){
if(v>1000)return'#FF4E6A';if(v>500)return'#FF9F43';
if(v>250)return'#FECA57';if(v>100)return'#2ED573';return'#45AAF2';
}
function ss3(v){
if(v>1000)return'\uD83D\uDD0A VERY LOUD!';if(v>500)return'\uD83D\uDD0A Loud';
if(v>250)return'\uD83D\uDD09 Moderate';if(v>100)return'\uD83D\uDD08 Quiet';return'\uD83D\uDD07 Silent';
}
function sl(v){if(v>1000)return'r';if(v>500)return'o';if(v>250)return'y';if(v>100)return'g';return'b'}

function setLed(prefix,ids,active){
for(var i=0;i<ids.length;i++){
var e=$(ids[i]);
if(e){if(ids[i].indexOf(active)>=0&&ids[i].length<=3)e.classList.add('a');
else e.classList.remove('a')}
}
}

function dlCSV(){
window.location.href='/api/download';
}
function clrLog(){
if(confirm('Clear all logged data?')){
fetch('/api/clearlog',{method:'POST'}).then(function(){st('lc','0')}).catch(function(){});
}
}
function fmtUp(ms){
var s=Math.floor(ms/1000),m=Math.floor(s/60),h=Math.floor(m/60);
return (h<10?'0':'')+h+':'+(m%60<10?'0':'')+(m%60)+':'+(s%60<10?'0':'')+(s%60);
}

function upd(d){
// Log count & uptime
st('lc',d.log||0);
st('lt','Uptime: '+fmtUp(d.up||0));
// Overview
if(d.uOk){st('uv',d.u.toFixed(1)+' cm');ss('us',us2(d.u),uc(d.u));
sb('ub',Math.min(d.u/200*100,100),uc(d.u))}else{st('uv','--');st('us','Offline')}
so('uo',d.uOk);

if(d.tOk){st('tv',d.t.toFixed(1)+'\u00B0C');st('hv','\uD83D\uDCA7 '+d.h.toFixed(0)+'%');
ss('ts',ts2(d.t),tc(d.t));sb('tb',Math.min(Math.max((d.t+10)/60*100,0),100),tc(d.t))}
else{st('tv','--');st('hv','\uD83D\uDCA7 --');st('ts','Offline')}
so('to',d.tOk);

if(d.sOk){st('sv',d.s);ss('ss2',ss3(d.s),sc(d.s));
sb('sb',Math.min(d.s/4095*100,100),sc(d.s))}else{st('sv','--');st('ss2','Offline')}
so('so',d.sOk);

st('ov',['OFF','FWD','REV','HEARTBEAT'][d.srv]);
sm=d.srv;
for(var i=0;i<4;i++){var e=$('sv'+i);if(e)e.className='sb'+(i===d.srv?' a':'')}

// Detail - Ultrasonic
if(d.uOk){st('duv',d.u.toFixed(1));sb('dub',Math.min(d.u/200*100,100),uc(d.u));
ss('dus',us2(d.u),uc(d.u));$('duv').style.color=uc(d.u);
if(d.u<=5.0){st('bz1','Playing melody ♪');$('bz1').className='on'}else{st('bz1','Silent (safe distance)');$('bz1').className=''}}
else{st('duv','--');st('dus','No Signal');st('bz1','Silent');$('bz1').className=''}

// Detail - Temp
if(d.tOk){st('dtv',d.t.toFixed(1));sb('dtb',Math.min(Math.max((d.t+10)/60*100,0),100),tc(d.t));
ss('dts',ts2(d.t),tc(d.t));$('dtv').style.color=tc(d.t);
st('dhv',d.h.toFixed(0));
if(d.t<=16.0||d.t>=26.0){st('bz2','Playing melody ♪');$('bz2').className='on'}else{st('bz2','Silent (safe temp)');$('bz2').className=''}}
else{st('dtv','--');st('dts','No Signal');st('dhv','--');st('bz2','Silent');$('bz2').className=''}

// Detail - Sound
if(d.sOk){st('dsv',d.s);sb('dsb',Math.min(d.s/4095*100,100),sc(d.s));
ss('dss',ss3(d.s),sc(d.s));$('dsv').style.color=sc(d.s);
if(d.s>100){st('bz3','Playing melody ♪');$('bz3').className='on'}else{st('bz3','Silent (low noise)');$('bz3').className=''}}
else{st('dsv','--');st('dss','No Signal');st('bz3','Silent');$('bz3').className=''}

// History & sparklines
if(d.uOk){H.u.push(d.u);if(H.u.length>MX)H.u.shift()}
if(d.tOk){H.t.push(d.t);if(H.t.length>MX)H.t.shift()}
if(d.sOk){H.s.push(d.s);if(H.s.length>MX)H.s.shift()}
drawC('cu',H.u,'#FF6B9D');drawC('ct',H.t,'#FF4E8C');drawC('cs',H.s,'#FF9F43');

// LED indicators
var uL=d.uOk?ul(d.u):'x',tL=d.tOk?tl(d.t):'x',sL=d.sOk?sl(d.s):'x';
var ids5=['r','o','y','g','b'];
function ia(c,lvl){var i=ids5.indexOf(lvl);return i!==-1&&ids5.indexOf(c)>=i;}

// Overview LEDs
ids5.forEach(function(c){
var e=$('o'+c);if(e)e.classList.toggle('a',ia(c,uL)||ia(c,tL)||ia(c,sL))});

// Ultra detail LEDs
['ur','uor','uy','ugr','ubl'].forEach(function(id,i){
var e=$(id);if(e)e.classList.toggle('a',ia(ids5[i],uL))});

// Temp detail LEDs
['tr','tor','ty','tgr','tbl'].forEach(function(id,i){
var e=$(id);if(e)e.classList.toggle('a',ia(ids5[i],tL))});

// Sound detail LEDs
['sr','sor','sy','sgr','sbl'].forEach(function(id,i){
var e=$(id);if(e)e.classList.toggle('a',ia(ids5[i],sL))});
}

function drawC(id,data,color){
var c=$(id);if(!c||data.length<2)return;
var ctx=c.getContext('2d');
var w=c.width,h=c.height;
ctx.clearRect(0,0,w,h);
var mx=Math.max.apply(null,data),mn=Math.min.apply(null,data);
var rng=mx-mn;if(rng===0)rng=1;
var sx=w/(data.length-1);

ctx.beginPath();ctx.moveTo(0,h);
for(var i=0;i<data.length;i++){
var x=i*sx,y=h-((data[i]-mn)/rng)*(h-8)-4;
ctx.lineTo(x,y)}
ctx.lineTo(w,h);ctx.closePath();
var gd=ctx.createLinearGradient(0,0,0,h);
gd.addColorStop(0,color+'33');gd.addColorStop(1,color+'05');
ctx.fillStyle=gd;ctx.fill();

ctx.beginPath();
for(var i=0;i<data.length;i++){
var x=i*sx,y=h-((data[i]-mn)/rng)*(h-8)-4;
i===0?ctx.moveTo(x,y):ctx.lineTo(x,y)}
ctx.strokeStyle=color;ctx.lineWidth=2;ctx.lineJoin='round';ctx.stroke();

var lx=(data.length-1)*sx;
var ly=h-((data[data.length-1]-mn)/rng)*(h-8)-4;
ctx.beginPath();ctx.arc(lx,ly,3,0,Math.PI*2);
ctx.fillStyle=color;ctx.fill();
}

setInterval(poll,500);
poll();
</script>
</body>
</html>
)rawliteral";

// ======================== SETUP ========================

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("===========================================");
  Serial.println(" ESP32 #4 — Hello Kitty Hub");
  Serial.println("===========================================");

  // --- Initialize LED pins ---
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_ORANGE, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  allLEDsOff();

  // --- Initialize Buzzer ---
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  // --- Start WiFi Access Point ---
  Serial.println("[WiFi] Starting Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);  // Wait for AP to stabilize
  Serial.println("[WiFi] AP SSID: " + String(AP_SSID));
  Serial.println("[WiFi] AP IP:   " + WiFi.softAPIP().toString());

  // --- Start UDP listener ---
  udp.begin(UDP_PORT);
  Serial.println("[UDP] Listening on port " + String(UDP_PORT));

  // --- Configure Web Server Routes ---
  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/screen", handleScreen);
  server.on("/api/servo", handleServo);
  server.on("/api/download", handleDownload);
  server.on("/api/clearlog", handleClearLog);
  server.begin();
  Serial.println("[WEB] Server started on port 80");
  Serial.println("[WEB] Open http://" + WiFi.softAPIP().toString());
  Serial.println("[LOG] Data logging enabled (every 5s, max 720 entries)");
  Serial.println("===========================================");
}

// ======================== MAIN LOOP ========================

void loop() {
  // 1) Handle web server requests
  server.handleClient();

  // 2) Receive and parse UDP packets from sensors
  receiveUDP();

  // 3) Update LEDs based on sensor data
  unsigned long now = millis();
  if (now - lastLedUpdate >= LED_UPDATE_MS) {
    lastLedUpdate = now;
    updateLEDs();
  }

  // 4) Handle buzzer melody
  handleBuzzer();

  // 5) Periodically resend servo command to ensure delivery
  if (now - lastServoTxTime >= 1000) {
    lastServoTxTime = now;
    sendServoCommand();
  }

  // 6) Log sensor data to circular buffer
  if (now - lastLogTime >= LOG_INTERVAL_MS) {
    lastLogTime = now;
    logSensorData();
  }
}

// ======================== WEB HANDLERS ========================

/**
 * Serve the main HTML page.
 */
void handleRoot() {
  server.send(200, "text/html", HTML_PAGE);
}

/**
 * Return sensor data as JSON for AJAX polling.
 */
void handleData() {
  char json[320];
  bool uOk = !isSensorTimedOut(ultraLastRx);
  bool tOk = !isSensorTimedOut(tempLastRx);
  bool sOk = !isSensorTimedOut(soundLastRx);

  snprintf(json, sizeof(json),
           "{\"u\":%.1f,\"t\":%.1f,\"h\":%.1f,\"s\":%d,"
           "\"uOk\":%d,\"tOk\":%d,\"sOk\":%d,"
           "\"scr\":%d,\"srv\":%d,"
           "\"log\":%d,\"up\":%lu}",
           ultraDistance,
           tempIsNan ? 0.0f : temperature,
           tempIsNan ? 0.0f : humidity,
           soundLevel < 0 ? 0 : soundLevel,
           uOk ? 1 : 0,
           tOk ? 1 : 0,
           sOk ? 1 : 0,
           currentScreen,
           currentServoMode,
           logCount,
           millis());

  server.send(200, "application/json", json);
}

/**
 * Handle screen change from the web UI.
 */
void handleScreen() {
  if (server.hasArg("s")) {
    int s = server.arg("s").toInt();
    if (s >= 0 && s <= 4) {
      currentScreen = s;
      Serial.println("[WEB] Screen changed to: " + String(s));

      // Stop buzzer when switching to overview or servo screen
      if ((currentScreen == 0 || currentScreen == 4) && buzzerPlaying) {
        noTone(BUZZER_PIN);
        buzzerPlaying = false;
        melodyIndex = 0;
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

/**
 * Handle servo control from the web UI.
 */
void handleServo() {
  if (server.hasArg("m")) {
    int m = server.arg("m").toInt();
    if (m >= 0 && m <= 3) {
      currentServoMode = m;
      sendServoCommand();
      Serial.println("[WEB] Servo mode: " + String(m));
    }
  }
  server.send(200, "text/plain", "OK");
}

// ======================== UDP RECEIVE ========================

void receiveUDP() {
  int packetSize = udp.parsePacket();
  while (packetSize > 0) {
    int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
    if (len > 0) {
      packetBuffer[len] = '\0';
      parsePacket(String(packetBuffer));
    }
    packetSize = udp.parsePacket();
  }
}

void parsePacket(String data) {
  data.trim();
  unsigned long now = millis();

  if (data.startsWith("U:")) {
    ultraDistance = data.substring(2).toFloat();
    ultraLastRx = now;

  } else if (data.startsWith("T:")) {
    int commaIdx = data.indexOf(',');
    if (commaIdx > 0) {
      temperature = data.substring(2, commaIdx).toFloat();
      int hIdx = data.indexOf("H:", commaIdx);
      if (hIdx > 0) {
        humidity = data.substring(hIdx + 2).toFloat();
      }
      tempIsNan = false;
      tempLastRx = now;
    }

  } else if (data.startsWith("S:")) {
    soundLevel = data.substring(2).toInt();
    soundLastRx = now;
  }
}

// ======================== SERVO COMMAND ========================

void sendServoCommand() {
  String cmd = "CMD:SERVO_STOP";
  if (currentServoMode == 1) cmd = "CMD:SERVO_FWD";
  else if (currentServoMode == 2) cmd = "CMD:SERVO_REV";
  else if (currentServoMode == 3) cmd = "CMD:SERVO_TICK";

  udp.beginPacket(BROADCAST_IP, UDP_PORT);
  udp.print(cmd);
  udp.endPacket();
}

// ======================== LED CONTROL ========================

/**
 * Update LEDs based on ALL sensor values.
 * Multiple LEDs can be on simultaneously if different sensors
 * trigger different threshold colors.
 *
 * Temperature thresholds:
 *   ≥25°C → Red, 22-24 → Orange, 19-21 → Yellow, 17-18 → Green, ≤16 → Blue
 *
 * Ultrasonic thresholds:
 *   ≤3cm → Red, 4-10 → Orange, 11-20 → Yellow, 21-50 → Green, >50 → Blue
 *
 * Sound thresholds:
 *   >2500 → Red, 1501-2500 → Orange, 801-1500 → Yellow, 401-800 → Green, ≤400 → Blue
 */
void updateLEDs() {
  bool red = false, orange = false, yellow = false, green = false, blue = false;

  // Temperature
  if (!isSensorTimedOut(tempLastRx) && !tempIsNan) {
    if (temperature >= 26.0) red = true;
    else if (temperature >= 22.0) orange = true;
    else if (temperature >= 19.0) yellow = true;
    else if (temperature >= 17.0) green = true;
    else blue = true;
  }

  // Ultrasonic
  if (!isSensorTimedOut(ultraLastRx) && ultraDistance >= 0) {
    if (ultraDistance <= 5.0) red = true;
    else if (ultraDistance <= 10.0) orange = true;
    else if (ultraDistance <= 20.0) yellow = true;
    else if (ultraDistance <= 50.0) green = true;
    else blue = true;
  }

  // Sound
  if (!isSensorTimedOut(soundLastRx) && soundLevel >= 0) {
    if (soundLevel > 1000) red = true;
    else if (soundLevel > 500) orange = true;
    else if (soundLevel > 250) yellow = true;
    else if (soundLevel > 100) green = true;
    else blue = true;
  }

  // Cascade logic to create a Bar Graph effect
  if (red) orange = true;
  if (orange) yellow = true;
  if (yellow) green = true;
  if (green) blue = true;

  digitalWrite(LED_RED, red ? HIGH : LOW);
  digitalWrite(LED_ORANGE, orange ? HIGH : LOW);
  digitalWrite(LED_YELLOW, yellow ? HIGH : LOW);
  digitalWrite(LED_GREEN, green ? HIGH : LOW);
  digitalWrite(LED_BLUE, blue ? HIGH : LOW);
}

void allLEDsOff() {
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_ORANGE, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_BLUE, LOW);
}

// ======================== BUZZER CONTROL ========================

/**
 * Play Hello Kitty melody on dedicated sensor screens.
 * Tempo varies with alert level (faster when sensor is in alert zone).
 * Silent on overview screen or if the viewed sensor has timed out.
 */
void handleBuzzer() {
  // Overview screen — always silent
  if (currentScreen == 0) {
    if (buzzerPlaying) {
      noTone(BUZZER_PIN);
      buzzerPlaying = false;
      melodyIndex = 0;
    }
    return;
  }

  // Check if the sensor for the current screen has data
  bool sensorOk = false;
  static unsigned long soundAlarmUntil = 0;

  if (currentScreen == 1) sensorOk = !isSensorTimedOut(ultraLastRx) && ultraDistance >= 0 && ultraDistance <= 5.0;
  else if (currentScreen == 2) sensorOk = !isSensorTimedOut(tempLastRx) && !tempIsNan && (temperature <= 16.0 || temperature >= 26.0);
  else if (currentScreen == 3) {
    if (soundLevel > 100) soundAlarmUntil = millis() + 2000;  // Hold melody for 2s after noise
    sensorOk = !isSensorTimedOut(soundLastRx) && (millis() < soundAlarmUntil);
  }

  if (!sensorOk) {
    if (buzzerPlaying) {
      noTone(BUZZER_PIN);
      buzzerPlaying = false;
      melodyIndex = 0;
    }
    return;
  }

  // Get tempo multiplier based on alert level
  // Higher = faster (more urgent); Lower = slower (gentle)
  float tempo = getTempoMultiplier();

  unsigned long now = millis();

  if (!buzzerPlaying) {
    // Start the melody
    buzzerPlaying = true;
    melodyIndex = 0;
    noteStartTime = now;
    if (hkMelody[0] > 0) {
      tone(BUZZER_PIN, hkMelody[0]);
    } else {
      noTone(BUZZER_PIN);
    }
    return;
  }

  // Check if current note duration has elapsed
  int adjustedDuration = (int)((float)hkDurations[melodyIndex] / tempo);
  if (adjustedDuration < 50) adjustedDuration = 50;  // minimum note length

  if (now - noteStartTime >= (unsigned long)adjustedDuration) {
    melodyIndex++;
    if (melodyIndex >= HK_LENGTH) {
      melodyIndex = 0;  // Loop the melody
    }
    noteStartTime = now;

    if (hkMelody[melodyIndex] == 0) {
      noTone(BUZZER_PIN);
    } else {
      // Brief articulation gap if same note repeats
      if (melodyIndex > 0 && hkMelody[melodyIndex] == hkMelody[melodyIndex - 1]) {
        noTone(BUZZER_PIN);
        delayMicroseconds(800);  // Tiny gap, won't block significantly
      }
      tone(BUZZER_PIN, hkMelody[melodyIndex]);
    }
  }
}

/**
 * Get tempo multiplier based on the current screen's sensor alert level.
 * Red zone = 1.5 (fast/urgent), Blue zone = 0.5 (slow/gentle)
 */
float getTempoMultiplier() {
  if (currentScreen == 1) {
    // Ultrasonic: close = urgent
    if (ultraDistance <= 5.0) return 1.5;
    if (ultraDistance <= 10.0) return 1.2;
    if (ultraDistance <= 20.0) return 1.0;
    if (ultraDistance <= 50.0) return 0.7;
    return 0.5;
  } else if (currentScreen == 2) {
    // Temperature: extremes = urgent
    if (temperature >= 26.0 || temperature <= 16.0) return 1.5;
    if (temperature >= 22.0) return 1.2;
    if (temperature >= 19.0) return 1.0;
    if (temperature >= 17.0) return 0.7;
    return 0.5;
  } else if (currentScreen == 3) {
    // Sound: loud = urgent
    if (soundLevel > 1000) return 1.5;
    if (soundLevel > 500) return 1.2;
    if (soundLevel > 250) return 1.0;
    if (soundLevel > 100) return 0.7;
    return 0.5;
  }
  return 1.0;
}

// ======================== DATA LOGGING ========================

/**
 * Log current sensor values into the circular buffer.
 * Called every LOG_INTERVAL_MS (5 seconds).
 */
void logSensorData() {
  LogEntry entry;
  entry.timestamp = millis();
  entry.distance = ultraDistance;
  entry.temp = tempIsNan ? 0.0 : temperature;
  entry.hum = tempIsNan ? 0.0 : humidity;
  entry.sound = soundLevel < 0 ? 0 : soundLevel;
  entry.uOk = !isSensorTimedOut(ultraLastRx);
  entry.tOk = !isSensorTimedOut(tempLastRx) && !tempIsNan;
  entry.sOk = !isSensorTimedOut(soundLastRx) && soundLevel >= 0;

  logBuffer[logHead] = entry;
  logHead = (logHead + 1) % MAX_LOG_ENTRIES;
  if (logCount < MAX_LOG_ENTRIES) logCount++;
}

/**
 * Format milliseconds as HH:MM:SS string.
 */
String formatTime(unsigned long ms) {
  unsigned long totalSec = ms / 1000;
  int h = totalSec / 3600;
  int m = (totalSec % 3600) / 60;
  int s = totalSec % 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

/**
 * Serve the logged data as a downloadable CSV file.
 * We build the entire CSV string first to ensure Content-Length is sent,
 * which fixes the "-1b" download bug on mobile browsers.
 */
void handleDownload() {
  Serial.println("[LOG] CSV download requested (" + String(logCount) + " entries)");

  String fullCsv = "Time (HH:MM:SS),Elapsed (ms),Distance (cm),Dist Status,Temperature (C),Humidity (%),Temp Status,Sound Level,Sound Status\r\n";
  // Pre-allocate memory (approx 80 chars per row)
  fullCsv.reserve(fullCsv.length() + logCount * 85);

  // Calculate start index in circular buffer
  int startIdx = (logCount < MAX_LOG_ENTRIES) ? 0 : logHead;
  char row[120];

  for (int i = 0; i < logCount; i++) {
    int idx = (startIdx + i) % MAX_LOG_ENTRIES;
    LogEntry& e = logBuffer[idx];

    // Build row
    snprintf(row, sizeof(row),
             "%s,%lu,%.1f,%s,%.1f,%.0f,%s,%d,%s\r\n",
             formatTime(e.timestamp).c_str(),
             e.timestamp,
             e.uOk ? e.distance : 0.0f,
             e.uOk ? "Online" : "Offline",
             e.tOk ? e.temp : 0.0f,
             e.tOk ? e.hum : 0.0f,
             e.tOk ? "Online" : "Offline",
             e.sOk ? e.sound : 0,
             e.sOk ? "Online" : "Offline");
    fullCsv += row;
  }

  // Send headers for CSV file download
  server.sendHeader("Content-Disposition", "attachment; filename=hello_kitty_sensor_log.csv");
  server.send(200, "text/csv", fullCsv);
  Serial.println("[LOG] CSV download complete");
}

/**
 * Clear the data log buffer.
 */
void handleClearLog() {
  logCount = 0;
  logHead = 0;
  Serial.println("[LOG] Data log cleared");
  server.send(200, "text/plain", "OK");
}

// ======================== UTILITY ========================

bool isSensorTimedOut(unsigned long lastRx) {
  return (lastRx == 0) || (millis() - lastRx > SENSOR_TIMEOUT_MS);
}
