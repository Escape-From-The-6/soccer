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

// TopLeft & Side: sensors 1..13
static Sensor S_left_side[] = {
  { 1,34,HIGH,HIGH,0,0},{ 2,35,HIGH,HIGH,0,0},{ 3,32,HIGH,HIGH,0,0},{ 4,33,HIGH,HIGH,0,0},
  { 5,25,HIGH,HIGH,0,0},{ 6,26,HIGH,HIGH,0,0},{ 7,27,HIGH,HIGH,0,0},{ 8,14,HIGH,HIGH,0,0},
  { 9,22,HIGH,HIGH,0,0},{10,13,HIGH,HIGH,0,0},{11,16,HIGH,HIGH,0,0},{12,17,HIGH,HIGH,0,0},
  {13,18,HIGH,HIGH,0,0}
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
  {0,0},{0,10},{11,18},{19,26},{27,33},{34,41},{42,49},{50,56},{57,64},{65,71},{72,79},{80,87},{88,94},{95,106}
};
#define B(a,b) { (uint16_t)(183-(b)), (uint16_t)(183-(a)) }
static const Seg TOP_MAP[24] = {
  {0,0}, B(1,11),B(12,19),B(20,27),B(28,34),B(35,42),B(43,50),B(51,57),
  B(58,65),B(66,72),B(73,80),B(81,88),B(89,95),B(96,103),B(104,110),B(111,118),
  B(119,126),B(127,133),B(134,141),B(142,149),B(151,156),B(158,164),B(165,171),B(172,183)
};
#undef B

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
  if (h->kind == MSG_HELLO_REQ){
    HelloMsg hm{}; hm.h.kind=MSG_HELLO; strncpy(hm.h.target,"ALL",sizeof(hm.h.target)-1);
    strncpy(hm.name,deviceName.c_str(),sizeof(hm.name)-1);
    esp_now_send(BCAST,(uint8_t*)&hm,sizeof(hm));
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
      for (int id=1; id<=23; ++id){
        if (!bitIsSet(bits, id)) continue;
        Seg s = TOP_MAP[id];
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          uint8_t pos = (uint8_t)((p + rainbowPhase) & 0xFF);
          strip->setPixelColor(p, wheelColor(pos));
        }
      }
    } else if (!penaltyMode || !any){
      // Normal: only targets are green
      for (int id=1; id<=23; ++id){
        if (!bitIsSet(bits, id)) continue;
        Seg s = TOP_MAP[id];
        for (uint16_t p=s.a; p<=s.b && p<LED_COUNT; ++p){
          strip->setPixelColor(p, strip->Color(0,255,0)); // GREEN target
        }
      }
    } else {
      // Penalty mode: active arena -> non-target = RED, target = GREEN
      for (int id=1; id<=23; ++id){
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

  if (ROLE_TOPRIGHT){
    S=S_topright; S_COUNT=sizeof(S_topright)/sizeof(S_topright[0]); LED_COUNT=183;
  }
  else{
    S=S_left_side; S_COUNT=sizeof(S_left_side)/sizeof(S_left_side[0]);
    LED_COUNT = ROLE_SIDE ? 107 : 0;
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

  HelloMsg hm{}; hm.h.kind=MSG_HELLO; strncpy(hm.h.target,"ALL",sizeof(hm.h.target)-1);
  strncpy(hm.name,deviceName.c_str(),sizeof(hm.name)-1); esp_now_send(BCAST,(uint8_t*)&hm,sizeof(hm));

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
