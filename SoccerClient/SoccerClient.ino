#include "SoccerCommon.h"
#include <Adafruit_NeoPixel.h>

// ================= Identity & Roles =================
#ifndef HOSTNAME
#define HOSTNAME "Soccer-generic"
#endif
static String deviceName = HOSTNAME;
static bool ROLE_TOPLEFT=false, ROLE_TOPRIGHT=false, ROLE_SIDE=false;

// ================= LEDs =================
#define LED_PIN 23
static uint16_t LED_COUNT=0;

// Physical strip lengths (used by segment maps)
static const uint16_t TOP_STRIP_PIXELS  = 184; // top LED strip total pixels
static const uint16_t SIDE_STRIP_PIXELS = 107; // side LED strip total pixels (original mapping)
Adafruit_NeoPixel* strip=nullptr;
static const uint32_t FRAME_MIN_MS=25; // ~40 fps
static uint32_t nextFrameAt=0;
static bool ledsDirty=true;

// Life-loss red flash
static bool     lifeFlashActive = false;
static uint32_t lifeFlashUntil  = 0;

// Penalty mode (levels 3 & 4)
static bool penaltyMode = false;  // when true, non-target zones on active arena are red

// Bonus round flag (from server)
static bool bonusRoundActive = false;

// Rainbow animation phase
static uint8_t rainbowPhase = 0;

// ================= ESP-NOW control =================
static bool espNowStarted=false;
static void startEspNow(){ if(!espNowStarted){ wifiStaOnCh6(); if(nowInit()){ esp_now_register_recv_cb(nullptr); espNowStarted=true; }}}
static void stopEspNow(){ if(espNowStarted){ esp_now_deinit(); espNowStarted=false; }}


// ================= Boot HELLO (OTA verification) =================
static uint32_t bootCount = 0;

// Drip a few HELLOs at boot so the server reliably sees the reboot after OTA.
static uint8_t  helloDripRemain = 0;
static uint32_t helloNextAt     = 0;
static const uint8_t  HELLO_DRIP_COUNT       = 5;
static const uint16_t HELLO_DRIP_INTERVAL_MS = 120;


// Optional: TopLeft sensor #14 GPIO override (helps if the 14th top sensor is wired to a different pin)
// Stored in NVS under NVS_NS_DEV / NVS_KEY_TOP14PIN.
static uint8_t nvsLoadTop14Pin(uint8_t defPin){
  Preferences p; p.begin(NVS_NS_DEV, true);
  uint8_t pin = (uint8_t)p.getUChar(NVS_KEY_TOP14PIN, defPin);
  p.end();
  return pin;
}
static void nvsSaveTop14Pin(uint8_t pin){
  Preferences p; p.begin(NVS_NS_DEV, false);
  p.putUChar(NVS_KEY_TOP14PIN, pin);
  p.end();
}

static uint32_t nvsNextBootCount(){
  Preferences p; p.begin(NVS_NS_DEV,false);
  uint32_t b = p.getULong(NVS_KEY_BOOT, 0);
  b++;
  p.putULong(NVS_KEY_BOOT, b);
  p.end();
  return b;
}

static inline void sendHello(){
  if(!espNowStarted) return;
  HelloMsg hm{}; hm.h.kind=MSG_HELLO;
  strncpy(hm.h.target,"ALL",sizeof(hm.h.target)-1);
  strncpy(hm.name,deviceName.c_str(),sizeof(hm.name)-1);
  hm.bootCount = bootCount;
  snprintf(hm.build, sizeof(hm.build), "%s %s", __DATE__, __TIME__);
  esp_now_send(BCAST,(uint8_t*)&hm,sizeof(hm));
}

// ================= Control Queues (WiFi/OTA/Name) =================
static char qWifiSsid[32]={0}; static char qWifiPass[64]={0}; static volatile bool qWifiPending=false;
static char qOtaUrl[192]={0};  static volatile bool qOtaPending=false;
static inline void queueWifiSet(const char* ssid,const char* pass){
  if(ssid){strncpy(qWifiSsid,ssid,31);qWifiSsid[31]=0;}else qWifiSsid[0]=0;
  if(pass){strncpy(qWifiPass,pass,63);qWifiPass[63]=0;}else qWifiPass[0]=0;
  qWifiPending=true;
}
static inline void queueOta(const char* url){
  if(url){strncpy(qOtaUrl,url,191);qOtaUrl[191]=0;}else qOtaUrl[0]=0;
  qOtaPending=true;
}
static bool connectForOta(String* why=nullptr){
  String ssid,pass; nvsLoadWifi(ssid,pass);
  WiFi.disconnect(true,true); delay(50);
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  WiFi.setHostname(deviceName.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0=millis(); wl_status_t st;
  while((st=WiFi.status())!=WL_CONNECTED && millis()-t0<15000) delay(100);
  if(st==WL_CONNECTED) return true;
  if(why) *why=String("status=")+String((int)st)+" ssid="+ssid+" ip="+WiFi.localIP().toString();
  return false;
}

// ================= Sensors (poll, like your test) =================
struct Sensor {
  uint8_t id;
  uint8_t pin;
  int     lastRaw;         // last raw read
  int     debounced;       // debounced level
  uint32_t lastChangeMs;   // last raw change time
  uint32_t lastTrigMs;     // last time we emitted TRIGGER
};

static const uint32_t DEBOUNCE_MS = 10;   // small, to catch quick passes but kill chatter
static const uint32_t TRIG_COOLDOWN_MS = 120; // per-sensor cooldown on sending events
static uint16_t seq = 1;

static inline void sendTrigger(uint8_t id, uint8_t gpio){
  SensorEventMsg m{}; m.h.kind=MSG_SENSOR_EVENT; strncpy(m.h.target,"ALL",sizeof(m.h.target)-1);
  strncpy(m.name,deviceName.c_str(),sizeof(m.name)-1);
  m.sensor_id=id; m.gpio=gpio; m.state=1; m.seq=seq++; m.ts_ms=millis();
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

// SIDE: sensors 1..13
static Sensor S_side[] = {
  { 1,34,HIGH,HIGH,0,0},{ 2,35,HIGH,HIGH,0,0},{ 3,32,HIGH,HIGH,0,0},{ 4,33,HIGH,HIGH,0,0},
  { 5,25,HIGH,HIGH,0,0},{ 6,26,HIGH,HIGH,0,0},{ 7,27,HIGH,HIGH,0,0},{ 8,14,HIGH,HIGH,0,0},
  { 9,22,HIGH,HIGH,0,0},{10,13,HIGH,HIGH,0,0},{11,16,HIGH,HIGH,0,0},{12,17,HIGH,HIGH,0,0},
  {13,18,HIGH,HIGH,0,0}
};

// TOPLEFT: sensors 1..14
// NOTE: sensor #14 pin is assumed to be IO23 (free on TopLeft because it has no LED strip).
// If your wiring uses a different pin, change the pin number below.
static Sensor S_topleft[] = {
  { 1,34,HIGH,HIGH,0,0},{ 2,35,HIGH,HIGH,0,0},{ 3,32,HIGH,HIGH,0,0},{ 4,33,HIGH,HIGH,0,0},
  { 5,25,HIGH,HIGH,0,0},{ 6,26,HIGH,HIGH,0,0},{ 7,27,HIGH,HIGH,0,0},{ 8,14,HIGH,HIGH,0,0},
  { 9,22,HIGH,HIGH,0,0},{10,13,HIGH,HIGH,0,0},{11,16,HIGH,HIGH,0,0},{12,17,HIGH,HIGH,0,0},
  {13,18,HIGH,HIGH,0,0},{14,23,HIGH,HIGH,0,0}
};
// TopRight: sensors 15..24
static Sensor S_topright[] = {
  {15,34,HIGH,HIGH,0,0},{16,35,HIGH,HIGH,0,0},{17,32,HIGH,HIGH,0,0},{18,33,HIGH,HIGH,0,0},{19,25,HIGH,HIGH,0,0},
  {20,26,HIGH,HIGH,0,0},{21,27,HIGH,HIGH,0,0},{22,14,HIGH,HIGH,0,0},{23,22,HIGH,HIGH,0,0},{24,13,HIGH,HIGH,0,0}
};

static Sensor* S=nullptr; static uint8_t S_COUNT=0;

// ================= Segment Maps =================
struct Seg { uint16_t a,b; }; // inclusive
static const Seg SIDE_MAP[14] = {
  // 0th entry unused (keeps 1-based indexing)
  {0,0},
  // Original side mapping (uneven physical spacing)
  {0,10}, {11,18}, {19,26}, {27,33}, {34,41}, {42,49}, {50,56},
  {57,64}, {65,71}, {72,79}, {80,87}, {88,94}, {95,106}
};
#define BR(a,b) { (uint16_t)((TOP_STRIP_PIXELS-1)-(b)), (uint16_t)((TOP_STRIP_PIXELS-1)-(a)) }
static const Seg TOP_MAP[25] = {
  // 0th entry unused (keeps 1-based sensor indexing)
  {0,0},
  BR(0,7),    BR(8,15),   BR(16,22),  BR(23,30),  BR(31,38),  BR(39,45),
  BR(46,53),  BR(54,61),  BR(62,68),  BR(69,76),  BR(77,84),  BR(85,91),
  BR(92,99),  BR(100,107),BR(108,114),BR(115,122),BR(123,130),BR(131,137),
  BR(138,145),BR(146,153),BR(154,160),BR(161,168),BR(169,176),BR(177,183)
};
#undef BR


// ===== Server-driven target bits =====
// We will interpret these as target zones.
// Normal: green; penalty: red/green; bonus: rainbow targets only.
static uint8_t overlayTopBits[3]  = {0,0,0};
static uint8_t overlaySideBits[2] = {0,0};

static inline bool bitIsSet(const uint8_t* b, int idx){
  if(idx<1) return false;
  idx-=1;
  return (b[idx>>3] >> (idx&7)) & 1;
}

// Rainbow helper
static uint32_t wheelColor(uint8_t pos){
  pos = 255 - pos;
  if(pos < 85){
    return strip->Color(255 - pos * 3, 0, pos * 3);
  }
  if(pos < 170){
    pos -= 85;
    return strip->Color(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return strip->Color(pos * 3, 255 - pos * 3, 0);
}

// ================= RX (apply overlays; keep control queues) =================
static void onNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len){
  if (len < (int)sizeof(MsgHeader)) return; const MsgHeader* h=(const MsgHeader*)data;

  if (h->kind == MSG_WIFI_SET && len >= (int)sizeof(WifiSetMsg)){
    const WifiSetMsg* m=(const WifiSetMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    queueWifiSet(m->ssid,m->pass);
    return;
  }
  if (h->kind == MSG_OTA_TRIGGER && len >= (int)sizeof(OtaMsg)){
    const OtaMsg* m=(const OtaMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    if(!m->url[0]) return;
    queueOta(m->url);
    return;
  }
  if (h->kind == MSG_NAME_SET && len >= (int)sizeof(NameSetMsg)){
    const NameSetMsg* m=(const NameSetMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    String nn=String(m->name); nn.trim();
    if(nn.length()){
      nvsSaveName(nn);
      deviceName=nn;
      delay(150);
      ESP.restart();
    }
    return;
  }
  // Configure a sensor GPIO (used for TopLeft sensor #14 if needed)
  if (h->kind == MSG_PIN_SET && len >= (int)sizeof(PinSetMsg)){
    const PinSetMsg* m=(const PinSetMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;

    // Only TopLeft uses sensor 14 in our wiring split (1..14 on TopLeft, 15..24 on TopRight).
    if (ROLE_TOPLEFT && m->sensor_id == 14){
      if (m->gpio > 0 && m->gpio <= 39){
        nvsSaveTop14Pin(m->gpio);
        delay(150);
        ESP.restart();
      }
    }
    return;
  }

  if (h->kind == MSG_HELLO_REQ){
    sendHello();
    return;
  }

  // Target overlays from server
  if (ROLE_TOPRIGHT && h->kind == MSG_LED_TOP && len >= (int)sizeof(LedTopMsg)){
    const LedTopMsg* m=(const LedTopMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    memcpy(overlayTopBits, m->bits, sizeof(overlayTopBits));
    ledsDirty = true;
    return;
  }
  if (ROLE_SIDE && h->kind == MSG_LED_SIDE && len >= (int)sizeof(LedSideMsg)){
    const LedSideMsg* m=(const LedSideMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    memcpy(overlaySideBits, m->bits, sizeof(overlaySideBits));
    ledsDirty = true;
    return;
  }

  // Life-loss red flash
  if (h->kind == MSG_LIFE_FLASH && len >= (int)sizeof(LifeFlashMsg)){
    const LifeFlashMsg* m=(const LifeFlashMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    uint16_t dur = m->durationMs;
    if (dur == 0) dur = 300; // fallback
    lifeFlashActive = true;
    lifeFlashUntil  = millis() + dur;
    ledsDirty = true;
    return;
  }

  // Mode: penalty on/off
  if (h->kind == MSG_MODE && len >= (int)sizeof(ModeMsg)){
    const ModeMsg* m=(const ModeMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    penaltyMode = (m->penaltyMode != 0);
    ledsDirty = true;
    return;
  }

  // Round info: bonus round flag
  if (h->kind == MSG_ROUND_INFO && len >= (int)sizeof(RoundInfoMsg)){
    const RoundInfoMsg* m=(const RoundInfoMsg*)data;
    if(!nameMatches(m->h.target,deviceName)) return;
    bonusRoundActive = (m->bonusRound != 0);
    ledsDirty = true;
    return;
  }
}

// ================= Rendering =================
static void renderFrame(){
  if (LED_COUNT==0 || strip==nullptr) return;

  uint32_t now = millis();

  // Continue HELLO drip (useful after OTA reboot)
  if (espNowStarted && helloDripRemain && (int32_t)(now - helloNextAt) >= 0){
    sendHello();
    helloDripRemain--;
    helloNextAt = now + HELLO_DRIP_INTERVAL_MS;
  }

  if (lifeFlashActive && now >= lifeFlashUntil){
    lifeFlashActive = false;
  }

  // Advance rainbow phase every frame
  rainbowPhase++;

  if (lifeFlashActive){
    // Full red flash on life loss
    for (uint16_t i=0;i<LED_COUNT;i++) strip->setPixelColor(i, strip->Color(255,0,0));
    strip->show();
    return;
  }

  // Base neutral (off)
  for (uint16_t i=0;i<LED_COUNT;i++) strip->setPixelColor(i, strip->Color(0,0,0));

  // ----- SIDE -----
  if (ROLE_SIDE){
    uint8_t bits[2];
    bits[0] = overlaySideBits[0];
    bits[1] = overlaySideBits[1];

    bool any = bits[0] || bits[1];

    if (bonusRoundActive && any){
      // BONUS: rainbow targets, no penalty coloring
      for (int id=1; id<=13; ++id){
        if (!bitIsSet(bits, id)) continue;
        Seg s = SIDE_MAP[id];
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          uint8_t pos = (uint8_t)((p + rainbowPhase) & 0xFF);
          strip->setPixelColor(p, wheelColor(pos));
        }
      }
    } else if (!penaltyMode || !any){
      // Normal: only targets are green, everything else off
      for (int id=1; id<=13; ++id){
        if (!bitIsSet(bits, id)) continue;
        Seg s = SIDE_MAP[id];
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          strip->setPixelColor(p, strip->Color(0,255,0)); // GREEN target
        }
      }
    } else {
      // Penalty mode: active arena -> non-target = RED, target = GREEN
      for (int id=1; id<=13; ++id){
        Seg s = SIDE_MAP[id];
        bool isTarget = bitIsSet(bits, id);
        uint32_t color = isTarget ? strip->Color(0,255,0) : strip->Color(255,0,0);
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          strip->setPixelColor(p, color);
        }
      }
    }
  }

  // ----- TOPRIGHT -----
  else if (ROLE_TOPRIGHT){
    uint8_t bits[3];
    bits[0] = overlayTopBits[0];
    bits[1] = overlayTopBits[1];
    bits[2] = overlayTopBits[2];

    bool any = bits[0] || bits[1] || bits[2];

    if (bonusRoundActive && any){
      // BONUS: rainbow targets, no penalty coloring
      for (int id=1; id<=24; ++id){
        if (!bitIsSet(bits, id)) continue;
        Seg s = TOP_MAP[id];
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          uint8_t pos = (uint8_t)((p + rainbowPhase) & 0xFF);
          strip->setPixelColor(p, wheelColor(pos));
        }
      }
    } else if (!penaltyMode || !any){
      // Normal: only targets are green
      for (int id=1; id<=24; ++id){
        if (!bitIsSet(bits, id)) continue;
        Seg s = TOP_MAP[id];
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          strip->setPixelColor(p, strip->Color(0,255,0)); // GREEN target
        }
      }
    } else {
      // Penalty mode: active arena -> non-target = RED, target = GREEN
      for (int id=1; id<=24; ++id){
        Seg s = TOP_MAP[id];
        bool isTarget = bitIsSet(bits, id);
        uint32_t color = isTarget ? strip->Color(0,255,0) : strip->Color(255,0,0);
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          strip->setPixelColor(p, color);
        }
      }
    }
  }

  strip->show();
}

// ================= Setup / Loop =================
void setup(){
  Serial.begin(115200); delay(200);

  String saved=nvsLoadName(); if(saved.length()) deviceName=saved; else nvsSaveName(deviceName);
  ROLE_TOPLEFT  = deviceName.equalsIgnoreCase("Soccer-topleft");
  ROLE_TOPRIGHT = deviceName.equalsIgnoreCase("Soccer-topright");
  ROLE_SIDE     = deviceName.equalsIgnoreCase("Soccer-side");

  bootCount = nvsNextBootCount();

  // If this is the TopLeft board, allow sensor #14 pin to be overridden from NVS.
  // This helps if the "11th from the right" top sensor is wired to a different GPIO.
  if (ROLE_TOPLEFT){
    uint8_t p14 = nvsLoadTop14Pin(23); // default is 23 unless overridden
    for (uint8_t i=0;i<sizeof(S_topleft)/sizeof(S_topleft[0]); ++i){
      if (S_topleft[i].id == 14){
        S_topleft[i].pin = p14;
      }
    }
    Serial.printf("[PIN] TopLeft sensor14 GPIO=%u (override via server PIN cmd)\n", (unsigned)p14);
  }



  if (ROLE_TOPRIGHT){
    S = S_topright;
    S_COUNT = sizeof(S_topright)/sizeof(S_topright[0]);
    LED_COUNT = TOP_STRIP_PIXELS;
  } else if (ROLE_SIDE){
    S = S_side;
    S_COUNT = sizeof(S_side)/sizeof(S_side[0]);
    LED_COUNT = SIDE_STRIP_PIXELS;
  } else if (ROLE_TOPLEFT){
    S = S_topleft;
    S_COUNT = sizeof(S_topleft)/sizeof(S_topleft[0]);
    LED_COUNT = 0;
  } else {
    // Fallback: sensors-only (helps bring up generic boards without LEDs)
    S = S_side;
    S_COUNT = sizeof(S_side)/sizeof(S_side[0]);
    LED_COUNT = 0;
  }

  for (uint8_t i=0;i<S_COUNT;i++){
    pinMode(S[i].pin, INPUT_PULLUP);      // strong bias
    int r=digitalRead(S[i].pin);
    S[i].lastRaw = S[i].debounced = r;
    S[i].lastChangeMs = S[i].lastTrigMs = millis();
  }

  if (LED_COUNT>0){
    strip = new Adafruit_NeoPixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
    strip->begin(); strip->setBrightness(255); strip->clear(); strip->show();
    ledsDirty=true;
  } else {
    Serial.println("[LED] no LEDs for this role");
  }

  startEspNow();
  esp_now_register_recv_cb(onNowRecv);

  // HELLO at boot (drip a few times for reliability)
  sendHello();
  helloDripRemain = (HELLO_DRIP_COUNT>0) ? (HELLO_DRIP_COUNT-1) : 0;
  helloNextAt = millis() + HELLO_DRIP_INTERVAL_MS;

  Serial.printf("Boot %s  (role: %s)  LEDs=%u on IO%d\n",
    deviceName.c_str(),
    ROLE_TOPLEFT ? "TopLeft(no-LED)" : (ROLE_TOPRIGHT ? "TopRight" : (ROLE_SIDE ? "Side" : "Unknown")),
    LED_COUNT, LED_PIN);
}

void loop(){
  // WiFi/OTA queues
  if (qWifiPending){
    qWifiPending=false;
    nvsSaveWifi(String(qWifiSsid),String(qWifiPass));
    delay(150);
    ESP.restart();
  }
  if (qOtaPending){
    qOtaPending=false;
    String url=String(qOtaUrl);
    stopEspNow(); delay(50);
    String why;
    if (connectForOta(&why)){
      WiFiClient client; HTTPUpdate up; up.rebootOnUpdate(true); up.update(client,url);
    }
    wifiStaOnCh6();
    startEspNow();
  }

  // Poll sensors (fast), like your test
  uint32_t now = millis();

  for (uint8_t i=0;i<S_COUNT;i++){
    Sensor &s = S[i];
    int raw = digitalRead(s.pin);

    if (raw != s.lastRaw){
      s.lastRaw = raw;
      s.lastChangeMs = now;
    }

    // simple debounce
    if ((now - s.lastChangeMs) >= DEBOUNCE_MS && raw != s.debounced){
      int prev = s.debounced;
      s.debounced = raw;

      // Edge emit: only on HIGH->LOW (TRIGGER), with cooldown
      if (prev==HIGH && s.debounced==LOW){
        if ((now - s.lastTrigMs) >= TRIG_COOLDOWN_MS){
          sendTrigger(s.id, s.pin);
          s.lastTrigMs = now;
        }
      }
      // We do NOT send CLEARs anymore.
    }
  }

  if (LED_COUNT>0 && (int32_t)(now - nextFrameAt) >= 0){
    renderFrame();
    nextFrameAt = now + FRAME_MIN_MS;
  }

  delay(1);
}
