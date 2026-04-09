#include "SoccerCommon.h"
#include <esp_system.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
// =============================================================
// Soccer Shootout ESP-NOW Server
//
// With standardized serial protocol for the main Player Management System (PMS)
//
// Standard protocol lines always start with:
//   !PMS
//
// PMS protocol (v1) ***DO NOT REMOVE***:
//   PMS -> Server:
//     !PMS PING
//     !PMS START level=1        (level 1..4; default 1)
//     !PMS STOP
//
//   Server -> PMS:
//     !PMS PONG v=1 game=soccer role=server
//     !PMS STATUS v=1 state=arming|playing|success level=.. score=.. lives=.. tleft_ms=.. last_reason=..
//       (STATUS prints every 250ms while arming/playing; STATUS is NOT emitted while idle,
//        except for a one-shot final STATUS with state=success right before a successful game_end)
//     !PMS EVENT v=1 name=game_start level=..
//     !PMS EVENT v=1 name=game_end reason=success|no_lives|stopped score=.. lives=..
//     !PMS EVENT v=1 name=score delta=.. total=.. bonus=0|1
//     !PMS EVENT v=1 name=life delta=-1 lives=..
//
// Build toggles (compile-time):
//   - PMS_STD_ENABLED: enable/disable PMS protocol support
//   - PMS_DEBUG_SERIAL: when 0, suppress non-!PMS Serial prints (keep PMS clean)
// =============================================================
//
// Notes:
// - `state=arming` is used during the Warmup round (RK_WARMUP). All other rounds use `state=playing`.
// - `tleft_ms` is time remaining in the *current round* (Warmup/HitAll/Shrink/Bonus).
// - `bonus=1` is emitted for score events that come from a BONUS WINDOW hit.
//   (Currently that only happens in RK_BONUS_MOVING, and the increment is
//    SOCCER_BONUS_SCORE_DELTA, default 2.)
// =============================================================

#ifndef PMS_STD_ENABLED
#define PMS_STD_ENABLED 1
#endif

#ifndef PMS_DEBUG_SERIAL
#define PMS_DEBUG_SERIAL 0
#endif

#ifndef PMS_STATUS_PERIOD_MS
#define PMS_STATUS_PERIOD_MS 250
#endif


#ifndef SOCCER_BONUS_SCORE_DELTA
#define SOCCER_BONUS_SCORE_DELTA 2
#endif

#ifndef SOCCER_SUCCESS_AFTER_MS
#define SOCCER_SUCCESS_AFTER_MS 360000UL
#endif

enum SoccerGameEndKind : uint8_t {
  SGE_NONE     = 0,
  SGE_STOPPED  = 1,
  SGE_SUCCESS  = 2,
  SGE_NO_LIVES = 3
};
#if PMS_DEBUG_SERIAL
  #define DBG_PRINT(...)    Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...)  Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...)    do { } while (0)
  #define DBG_PRINTLN(...)  do { } while (0)
  #define DBG_PRINTF(...)   do { } while (0)
#endif

// PMS output is NEVER suppressed by PMS_DEBUG_SERIAL.
#define PMS_PRINT(...)    Serial.print(__VA_ARGS__)
#define PMS_PRINTLN(...)  Serial.println(__VA_ARGS__)
#define PMS_PRINTF(...)   Serial.printf(__VA_ARGS__)


static String myName = "Soccer-server";

static uint8_t radioChannel = SOCCER_DEFAULT_ESPNOW_CHANNEL;

// ========== Roster ==========

struct Seen {
  String   name;
  uint32_t ms;
  uint32_t bootCount;
  char     build[BUILD_STR_LEN];
  uint32_t lastHelloPrintMs;
};
Seen seen[32];


static bool rememberHello(const char* n, uint32_t bootCount=0, const char* build=nullptr){
  if(!n||!*n) return false;
  uint32_t now = millis();

  const bool hasMeta = (bootCount != 0) || (build && *build);

  // Update existing
  for(int i=0;i<32;i++){
    if (seen[i].name.equalsIgnoreCase(n)){
      seen[i].ms = now;

      if (!hasMeta){
        // Passive "seen" update (e.g., sensor events) shouldn't affect HELLO printing.
        return false;
      }

      bool bootChanged  = (bootCount != 0 && bootCount != seen[i].bootCount);
      bool buildChanged = false;
      if (build && *build){
        if (strncmp(build, seen[i].build, BUILD_STR_LEN) != 0) buildChanged = true;
      }

      if (bootCount != 0) seen[i].bootCount = bootCount;
      if (build && *build){
        strncpy(seen[i].build, build, BUILD_STR_LEN-1);
        seen[i].build[BUILD_STR_LEN-1] = 0;
      }

      bool shouldPrint = bootChanged || buildChanged || (seen[i].lastHelloPrintMs==0) || (now - seen[i].lastHelloPrintMs > 1500);
      if (shouldPrint) seen[i].lastHelloPrintMs = now;
      return shouldPrint;
    }
  }

  // Add new
  for(int i=0;i<32;i++){
    if (!seen[i].name.length()){
      seen[i].name = n;
      seen[i].ms   = now;
      seen[i].bootCount = bootCount;

      seen[i].build[0] = 0;
      if (build && *build){
        strncpy(seen[i].build, build, BUILD_STR_LEN-1);
        seen[i].build[BUILD_STR_LEN-1] = 0;
      }

      // Only HELLOs should set lastHelloPrintMs
      seen[i].lastHelloPrintMs = hasMeta ? now : 0;
      return hasMeta; // print if it's a HELLO with meta
    }
  }
  return hasMeta;
}


// ========== Names ==========
static const char* NAME_TOPLEFT  = "Soccer-topleft";
static const char* NAME_TOPRIGHT = "Soccer-topright";
static const char* NAME_SIDE     = "Soccer-side";

// ========== Peer MACs (auto-learned) ==========
// We learn the MACs of the Soccer clients at runtime (HELLO and/or sensor events).
// This lets us unicast frequent messages (LED targets) instead of broadcasting.
static uint8_t macTopLeft[6]  = {0,0,0,0,0,0};
static uint8_t macTopRight[6] = {0,0,0,0,0,0};
static uint8_t macSide[6]     = {0,0,0,0,0,0};
static bool macTopLeftKnown=false, macTopRightKnown=false, macSideKnown=false;

static inline bool macEqual(const uint8_t a[6], const uint8_t b[6]){
  return memcmp(a,b,6)==0;
}

static inline void macToStr(const uint8_t mac[6], char* out, size_t outLen){
  snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static void notePeerMac(const char* name, const uint8_t srcMac[6]){
  if(!name || !*name || !srcMac) return;
  if (memcmp(srcMac, BCAST, 6) == 0) return;

  if (!strcasecmp(name, NAME_TOPLEFT)){
    if (!macTopLeftKnown || !macEqual(macTopLeft, srcMac)){
      memcpy(macTopLeft, srcMac, 6);
      macTopLeftKnown = true;
      nowEnsurePeer(macTopLeft, radioChannel);
      char buf[24]; macToStr(macTopLeft, buf, sizeof(buf));
      DBG_PRINTF("[PEER] %s = %s\n", NAME_TOPLEFT, buf);
    }
    return;
  }
  if (!strcasecmp(name, NAME_TOPRIGHT)){
    if (!macTopRightKnown || !macEqual(macTopRight, srcMac)){
      memcpy(macTopRight, srcMac, 6);
      macTopRightKnown = true;
      nowEnsurePeer(macTopRight, radioChannel);
      char buf[24]; macToStr(macTopRight, buf, sizeof(buf));
      DBG_PRINTF("[PEER] %s = %s\n", NAME_TOPRIGHT, buf);
    }
    return;
  }
  if (!strcasecmp(name, NAME_SIDE)){
    if (!macSideKnown || !macEqual(macSide, srcMac)){
      memcpy(macSide, srcMac, 6);
      macSideKnown = true;
      nowEnsurePeer(macSide, radioChannel);
      char buf[24]; macToStr(macSide, buf, sizeof(buf));
      DBG_PRINTF("[PEER] %s = %s\n", NAME_SIDE, buf);
    }
    return;
  }
}

// ========== Game / Config ==========

enum Arena : uint8_t {
  ARENA_NONE = 0,
  ARENA_TOP  = 1,
  ARENA_SIDE = 2
};

enum RoundKind : uint8_t {
  RK_NONE           = 0,
  RK_WARMUP         = 1,   // one block, hits inside only
  RK_HIT_ALL_A      = 2,   // hit-all-blocks on arena A
  RK_HIT_ALL_B      = 3,   // hit-all-blocks on opposite arena B
  RK_SHRINKING      = 4,   // shrinking target window around fixed center
  RK_BONUS_MOVING   = 5    // sliding bonus round
};

struct Config {
  uint32_t r1DurationMs;       // Warmup
  uint32_t r2DurationMs;       // HitAll A
  uint32_t r3DurationMs;       // HitAll B
  uint32_t r4DurationMs;       // Shrinking round
  uint8_t  topBlocks;          // Level1/3: blocks on top
  uint8_t  sideBlocks;         // Level1/3: blocks on side
  uint32_t scoreRearmMs;       // per-sensor score re-arm
  uint32_t roundChangeGraceMs; // ignore hits for this long after round start/shrink
};

static Config cfg = {
  15000,  // r1DurationMs = 15s
  30000,  // r2DurationMs = 30s
  30000,  // r3DurationMs = 30s
  30000,  // r4DurationMs = 30s
  3,      // Level1/3: topBlocks = 3
  2,      // Level1/3: sideBlocks = 2
  200,    // scoreRearmMs (ms)
  400     // roundChangeGraceMs (ms)
};

static const uint8_t L2_TOP_BLOCKS  = 5;   // Level2/4 layout
static const uint8_t L2_SIDE_BLOCKS = 3;

struct LevelConfig {
  uint8_t   index;           // 1..4
  bool      penaltyMode;     // Levels 3 & 4
  bool      useLayout2;      // layout1 vs layout2
  uint8_t   numRounds;       // how many rounds in this level
  RoundKind rounds[5];       // up to 5 rounds per level
};

// Levels definition
static const struct LevelConfig levelConfigs[] = {
  // Level 1: layout1, no penalty, Warmup -> HitAll A -> HitAll B -> Shrinking -> Bonus slide
  {1, false, false, 5, {RK_WARMUP, RK_HIT_ALL_A, RK_HIT_ALL_B, RK_SHRINKING, RK_BONUS_MOVING}},
  // Level 2: layout2, no penalty, Warmup -> HitAll A -> HitAll B -> Shrinking -> Bonus slide
  {2, false, true,  5, {RK_WARMUP, RK_HIT_ALL_A, RK_HIT_ALL_B, RK_SHRINKING, RK_BONUS_MOVING}},
  // Level 3: layout1, penalty, Warmup -> HitAll A -> HitAll B -> Shrinking -> Bonus slide
  {3, true,  false, 5, {RK_WARMUP, RK_HIT_ALL_A, RK_HIT_ALL_B, RK_SHRINKING, RK_BONUS_MOVING}},
  // Level 4: layout2, penalty, Warmup -> HitAll A -> HitAll B -> Shrinking -> Bonus slide
  {4, true,  true,  5, {RK_WARMUP, RK_HIT_ALL_A, RK_HIT_ALL_B, RK_SHRINKING, RK_BONUS_MOVING}}
};

struct Game {
  bool        active        = false;
  uint8_t     lives         = 0;
  uint32_t    score         = 0;

  const struct LevelConfig* level  = nullptr;
  uint8_t     levelIndex    = 0;
  bool        penaltyMode   = false;

  uint8_t     roundIndex    = 0;   // 0..numRounds-1
  RoundKind   roundKind     = RK_NONE;
  Arena       arena         = ARENA_NONE;
  Arena       arenaA        = ARENA_NONE; // for HIT_ALL_A/B mapping

  uint32_t    gameStartMs   = 0;
  uint32_t    gameEndMs     = 0;   // overall 6-minute success timer
  SoccerGameEndKind lastEndKind = SGE_NONE;
  uint32_t    roundStartMs  = 0;
  uint32_t    roundDurationMs = 0;

  uint8_t     blockCount    = 0;
  uint8_t     blockMin[8];
  uint8_t     blockMax[8];
  bool        blockCleared[8];

  uint8_t     activeBlock   = 0;   // warmup: which block is active
};

static Game g;

// Shrinking round (Round 4) state
struct ShrinkState {
  bool     active;
  uint8_t  center;         // center sensor id
  uint8_t  stage;          // 0=full, 1=mid, 2=narrow
  uint8_t  widths[3];      // {7,5,3} or {5,3,1}
  uint8_t  minId;          // current min id
  uint8_t  maxId;          // current max id
};

static ShrinkState shrink = {false,0,0,{0,0,0},0,0};
static uint16_t shrinkHits = 0;

// Bonus sliding round state (Round 5)
struct BonusState {
  bool     active;
  uint8_t  center;         // current center
  int8_t   dir;            // +1 or -1
  uint8_t  width;          // window width (e.g. 3)
  uint8_t  minId;
  uint8_t  maxId;
  uint32_t lastStepMs;     // last time we moved the window
};

static BonusState bonus = {false,0,1,3,0,0,0};
static const uint32_t BONUS_DURATION_MS = 15000;   // 15s
static const uint32_t BONUS_STEP_MS     = 80;      // sliding speed

// Per-sensor score re-arm
static uint32_t lastScoreMsBySensor[32] = {0};

// Global hit grace: after any meaningful hit (score or life change),
// ignore further events for a brief period so a single ball can't
// generate multiple processed hits across different sensors.
static uint32_t lastHitGlobalMs = 0;
static const uint32_t HIT_GLOBAL_GRACE_MS = 120;  // ms; tune as needed

// Grace window after round start/shrink
static uint32_t roundChangeIgnoreUntil = 0;

// Warmup hit flash
static bool     hitFlashActive    = false;
static uint8_t  hitFlashBlock     = 0;
static uint32_t hitFlashUntilMs   = 0;
static const uint32_t HIT_FLASH_MS = 120;

// Sensor counts
static const uint8_t TOP_SENSOR_COUNT  = 24;
static const uint8_t SIDE_SENSOR_COUNT = 13;

// Target hit forgiveness in *sensor* units (± this many sensors).
// Applied to "correct hit" checks across all rounds that have a target window.
// Wrong hits remain strict: only hits outside the expanded window are treated as wrong.
static uint8_t graceTopSensors  = 1;  // per-side forgiveness around correct targets (TOP)
static uint8_t graceSideSensors = 2;  // per-side forgiveness around correct targets (SIDE)

// ========== TEST MODE (sensor->LED mapping verification) ==========
// When enabled, the server ignores game state and simply lights the
// LED segment corresponding to the last sensor that triggered.
//
// Usage (Server CLI):
//   TEST ON
//   TEST OFF
//   TEST CLEAR
static bool testMode = false;

// Targets (server -> clients)
static uint8_t targetTopBits[3]  = {0,0,0};
static uint8_t targetSideBits[2] = {0,0};

// LED cadence
static uint32_t nextLedPushAt=0;
static const uint32_t LED_PUSH_MIN_MS=40;  // ms (restores pre-v2 visual timing; avoids accidental ultra-spam)

// ========== Target helpers ==========
static inline void clearTargets(){
  memset(targetTopBits,  0, sizeof(targetTopBits));
  memset(targetSideBits, 0, sizeof(targetSideBits));
}

static inline void setTopBit(uint8_t sensorId){
  if (sensorId<1 || sensorId>TOP_SENSOR_COUNT) return;
  uint8_t idx = sensorId-1;
  targetTopBits[idx>>3] |= (1 << (idx&7));
}

static inline void setSideBit(uint8_t sensorId){
  if (sensorId<1 || sensorId>SIDE_SENSOR_COUNT) return;
  uint8_t idx = sensorId-1;
  targetSideBits[idx>>3] |= (1 << (idx&7));
}

static void computeBlockRange(uint8_t totalSensors, uint8_t blockCount, uint8_t blockIdx,
                              uint8_t& outMinId, uint8_t& outMaxId){
  if (blockCount == 0){
    outMinId = 1;
    outMaxId = totalSensors;
    return;
  }
  if (blockIdx >= blockCount) blockIdx %= blockCount;

  uint8_t base = totalSensors / blockCount;
  uint8_t rem  = totalSensors % blockCount;

  uint8_t startIdx = blockIdx * base + (blockIdx < rem ? blockIdx : rem);
  uint8_t length   = base + (blockIdx < rem ? 1 : 0);

  outMinId = startIdx + 1;
  outMaxId = outMinId + length - 1;
}


static inline uint8_t graceForArena(uint8_t arena){
  return (arena == ARENA_TOP) ? graceTopSensors : graceSideSensors;
}

static inline void computeGraceRange(uint8_t minId, uint8_t maxId, uint8_t maxSensors, uint8_t arena,
                                     uint8_t& outMinId, uint8_t& outMaxId){
  uint8_t g = graceForArena(arena);
  outMinId = (minId > g) ? (uint8_t)(minId - g) : 1;
  uint16_t mx = (uint16_t)maxId + (uint16_t)g;
  outMaxId = (mx <= maxSensors) ? (uint8_t)mx : maxSensors;
}

static void rebuildTargetsFromBlocks(){
  clearTargets();
  if (!g.active) return;
  if (g.arena == ARENA_NONE) return;

  if (g.roundKind == RK_WARMUP){
    if (g.blockCount == 0) return;
    if (g.activeBlock >= g.blockCount) return;
    uint8_t b = g.activeBlock;
    uint8_t minId = g.blockMin[b];
    uint8_t maxId = g.blockMax[b];
    if (g.arena == ARENA_TOP){
      for (uint8_t id=minId; id<=maxId; ++id) setTopBit(id);
    } else if (g.arena == ARENA_SIDE){
      for (uint8_t id=minId; id<=maxId; ++id) setSideBit(id);
    }
  } else if (g.roundKind == RK_HIT_ALL_A || g.roundKind == RK_HIT_ALL_B){
    for (uint8_t b=0; b<g.blockCount; ++b){
      if (g.blockCleared[b]) continue;
      uint8_t minId = g.blockMin[b];
      uint8_t maxId = g.blockMax[b];
      if (g.arena == ARENA_TOP){
        for (uint8_t id=minId; id<=maxId; ++id) setTopBit(id);
      } else if (g.arena == ARENA_SIDE){
        for (uint8_t id=minId; id<=maxId; ++id) setSideBit(id);
      }
    }
  }
}

static void initBlocksForArena(uint8_t blockCount,
                               uint8_t totalTopSensors,
                               uint8_t totalSideSensors,
                               bool resetClearedFlags){
  g.blockCount = blockCount;
  if (g.blockCount == 0) g.blockCount = 1;

  uint8_t totalSensors = (g.arena == ARENA_TOP) ? totalTopSensors : totalSideSensors;

  for (uint8_t b=0; b<g.blockCount; ++b){
    computeBlockRange(totalSensors, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
    if (resetClearedFlags){
      g.blockCleared[b] = false;
    }
  }
}

// ========== LED push ==========
static void pushLedStates(){
  uint8_t sendTop[3];  memcpy(sendTop,  targetTopBits,  sizeof(targetTopBits));
  uint8_t sendSide[2]; memcpy(sendSide, targetSideBits, sizeof(targetSideBits));

  uint32_t now = millis();

  if (hitFlashActive && now >= hitFlashUntilMs){
    hitFlashActive = false;
  }

  if (hitFlashActive && g.roundKind == RK_WARMUP && g.blockCount > 0){
    uint8_t b = hitFlashBlock;
    if (b < g.blockCount){
      uint8_t minId = g.blockMin[b];
      uint8_t maxId = g.blockMax[b];
      if (g.arena == ARENA_TOP){
        for (uint8_t id=minId; id<=maxId; ++id){
          if (id < 1 || id > TOP_SENSOR_COUNT) continue;
          uint8_t idx = id - 1;
          sendTop[idx>>3] &= ~(1 << (idx & 7));
        }
      } else if (g.arena == ARENA_SIDE){
        for (uint8_t id=minId; id<=maxId; ++id){
          if (id < 1 || id > SIDE_SENSOR_COUNT) continue;
          uint8_t idx = id - 1;
          sendSide[idx>>3] &= ~(1 << (idx & 7));
        }
      }
    }
  }

  { // TOP targets
    LedTopMsg t{};
    headerInit(t.h, MSG_LED_TOP, NAME_TOPRIGHT);
    memcpy(t.bits, sendTop, sizeof(sendTop));
    const uint8_t* dest = macTopRightKnown ? macTopRight : BCAST;
    esp_now_send(dest, (uint8_t*)&t, sizeof(t));
  }
  { // SIDE targets
    LedSideMsg s{};
    headerInit(s.h, MSG_LED_SIDE, NAME_SIDE);
    memcpy(s.bits, sendSide, sizeof(sendSide));
    const uint8_t* dest = macSideKnown ? macSide : BCAST;
    esp_now_send(dest, (uint8_t*)&s, sizeof(s));
  }
}

// ========== Shrinking round (Round 4) ==========
static void shrinkingInitWidths(){
  if (g.levelIndex == 1 || g.levelIndex == 3){
    shrink.widths[0] = 7;
    shrink.widths[1] = 5;
    shrink.widths[2] = 3;
  } else {
    shrink.widths[0] = 5;
    shrink.widths[1] = 3;
    shrink.widths[2] = 1;
  }
}

static void updateShrinkingTargets(){
  if (!shrink.active || !g.active) return;
  if (g.roundKind != RK_SHRINKING) return;
  if (g.arena == ARENA_NONE) return;

  uint8_t width = shrink.widths[shrink.stage];
  if (width < 1) width = 1;

  uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
  int halfSpan = width / 2;

  int minId = (int)shrink.center - halfSpan;
  int maxId = minId + width - 1;

  if (minId < 1) { maxId += (1 - minId); minId = 1; }
  if (maxId > maxSensors){
    int diff = maxId - maxSensors;
    minId -= diff;
    maxId = maxSensors;
    if (minId < 1) minId = 1;
  }

  shrink.minId = (uint8_t)minId;
  shrink.maxId = (uint8_t)maxId;

  clearTargets();
  if (g.arena == ARENA_TOP){
    for (uint8_t id=shrink.minId; id<=shrink.maxId; ++id) setTopBit(id);
  } else if (g.arena == ARENA_SIDE){
    for (uint8_t id=shrink.minId; id<=shrink.maxId; ++id) setSideBit(id);
  }
}

// ========== Bonus sliding round (Round 5) ==========
static void updateBonusTargets(){
  if (!bonus.active || !g.active) return;
  if (g.roundKind != RK_BONUS_MOVING) return;
  if (g.arena == ARENA_NONE) return;

  uint32_t now = millis();
  if ((uint32_t)(now - bonus.lastStepMs) < BONUS_STEP_MS) return;
  bonus.lastStepMs = now;

  uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
  int halfSpan = bonus.width / 2;

  int newCenter = (int)bonus.center + (int)bonus.dir;
  if (newCenter - halfSpan < 1){
    newCenter = 1 + halfSpan;
    bonus.dir = +1;
  } else if (newCenter + halfSpan > maxSensors){
    newCenter = maxSensors - halfSpan;
    bonus.dir = -1;
  }

  bonus.center = (uint8_t)newCenter;
  bonus.minId  = (uint8_t)(bonus.center - halfSpan);
  bonus.maxId  = (uint8_t)(bonus.center + halfSpan);

  clearTargets();
  if (g.arena == ARENA_TOP){
    for (uint8_t id=bonus.minId; id<=bonus.maxId; ++id) setTopBit(id);
  } else if (g.arena == ARENA_SIDE){
    for (uint8_t id=bonus.minId; id<=bonus.maxId; ++id) setSideBit(id);
  }

  nextLedPushAt = now;
}

// ========== HELLO drip ==========
static bool helloDripActive=false; static uint8_t helloDripRemain=0;
static uint32_t helloNextAt=0, listPrintAt=0;
static const uint8_t  HELLO_DRIP_COUNT=5;
static const uint32_t HELLO_DRIP_INTERVAL_MS=200, HELLO_SETTLE_MS=350;

// ========== Sends ==========
static void sendHelloReqAll(){
  MsgHeader h{};
  headerInit(h, MSG_HELLO_REQ, "ALL");
  esp_now_send(BCAST,(uint8_t*)&h,sizeof(h));
}

static void sendWifiSet(const char* target,const String& ssid,const String& pass){
  WifiSetMsg m{};
  headerInit(m.h, MSG_WIFI_SET, target);
  strncpy(m.ssid,ssid.c_str(),sizeof(m.ssid)-1);
  strncpy(m.pass,pass.c_str(),sizeof(m.pass)-1);

  // WiFi/OTA config is infrequent; broadcast is fine (target filtering happens on clients).
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendOta(const char* target,const String& url){
  OtaMsg m{};
  const char* tgt=(target&&*target)?target:"ALL";
  headerInit(m.h, MSG_OTA_TRIGGER, tgt);
  strncpy(m.url,url.c_str(),sizeof(m.url)-1);
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendNameSet(const char* target,const String& newName){
  NameSetMsg m{};
  headerInit(m.h, MSG_NAME_SET, target);
  strncpy(m.name,newName.c_str(),sizeof(m.name)-1);
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendPinSet(const char* target, uint8_t sensorId, uint8_t gpio){
  PinSetMsg m{};
  const char* tgt=(target&&*target)?target:"ALL";
  headerInit(m.h, MSG_PIN_SET, tgt);
  m.sensor_id = sensorId;
  m.gpio      = gpio;
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendLifeFlash(uint16_t durationMs){
  LifeFlashMsg m{};
  headerInit(m.h, MSG_LIFE_FLASH, "ALL");
  m.durationMs = durationMs;
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendMode(bool penalty){
  ModeMsg m{};
  headerInit(m.h, MSG_MODE, "ALL");
  m.penaltyMode = penalty ? 1 : 0;
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendRoundInfo(bool bonusRound){
  // Round info is what drives BONUS rainbow on clients.
  // In v2 we unicast most high-rate traffic; this message was still broadcast-only.
  // If a single broadcast is missed, clients will stay in "normal" coloring for the whole bonus round.
  //
  // So we:
  //  1) Unicast to known peers (ACKed) for reliability.
  //  2) Also broadcast as a fallback for brand-new/replaced boards that haven't been learned yet.

  RoundInfoMsg m{};
  m.bonusRound = bonusRound ? 1 : 0;

  // Unicast (reliable) to known clients
  if (macTopRightKnown){
    headerInit(m.h, MSG_ROUND_INFO, NAME_TOPRIGHT);
    esp_now_send(macTopRight, (uint8_t*)&m, sizeof(m));
  }
  if (macSideKnown){
    headerInit(m.h, MSG_ROUND_INFO, NAME_SIDE);
    esp_now_send(macSide, (uint8_t*)&m, sizeof(m));
  }
  if (macTopLeftKnown){
    headerInit(m.h, MSG_ROUND_INFO, NAME_TOPLEFT);
    esp_now_send(macTopLeft, (uint8_t*)&m, sizeof(m));
  }

  // Broadcast fallback
  headerInit(m.h, MSG_ROUND_INFO, "ALL");
  esp_now_send(BCAST, (uint8_t*)&m, sizeof(m));
}

static void sendChanSetAll(uint8_t channel){
  ChannelSetMsg m{};
  headerInit(m.h, MSG_CHAN_SET, "ALL");
  m.channel = channel;
  m.reserved = 0;
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}


// Forward declarations
static void startRound(uint8_t roundIdx);
static void startLevel(uint8_t levelIndex, bool resetScoreAndLives);
static void restartCurrentRound();
static void onRoundCleared();
static void onShrinkCleared();
static void onBonusCleared();
static void finishLevelAdvance();

#if PMS_STD_ENABLED
// Score events are queued from the ESPNOW receive callback (sensor hits) and
// flushed from the main loop (pmsTick) to avoid Serial interleaving.
static void pmsQueueScoreEvent(int32_t delta, uint32_t total, bool bonus);
static void pmsClearScoreQueue();
#endif

// Centralized scoring helper so PMS can correctly tag bonus hits.
static inline void addScore(uint32_t delta, bool isBonus){
  g.score += delta;
#if PMS_STD_ENABLED
  if (g.active){
    pmsQueueScoreEvent((int32_t)delta, g.score, isBonus);
  }
#endif
}

// ========== Life loss helpers ==========
static void loseLifeAndRestart(const char* reason){
  if (!g.active) return;
  if (g.lives > 0) g.lives--;

  // Global hit grace: a life-loss is also a "hit event"
  lastHitGlobalMs = millis();

  DBG_PRINTF("[L%uR%u] %s -> life lost, lives=%u (score=%lu)\n",
    (unsigned)g.levelIndex,
    (unsigned)(g.roundIndex+1),
    reason ? reason : "life-loss",
    (unsigned)g.lives,
    (unsigned long)g.score);

  sendLifeFlash(300);

  if (g.lives == 0){
    DBG_PRINTF("[L%uR%u] out of lives, ending game.\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1));
    g.lastEndKind = SGE_NO_LIVES;
    g.active = false;
    clearTargets();
    pushLedStates();
    return;
  }

  restartCurrentRound();
}

static void loseLifeOnly(const char* reason){
  if (!g.active) return;
  if (g.lives > 0) g.lives--;

  // Global hit grace
  lastHitGlobalMs = millis();

  DBG_PRINTF("[L%uR%u] %s -> life lost (no restart), lives=%u (score=%lu)\n",
    (unsigned)g.levelIndex,
    (unsigned)(g.roundIndex+1),
    reason ? reason : "life-loss",
    (unsigned)g.lives,
    (unsigned long)g.score);

  sendLifeFlash(300);

  if (g.lives == 0){
    DBG_PRINTF("[L%uR%u] out of lives, ending game.\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1));
    g.lastEndKind = SGE_NO_LIVES;
    g.active = false;
    clearTargets();
    pushLedStates();
  }
}

// ========== Level helpers ==========
static const struct LevelConfig* getLevelConfig(uint8_t idx){
  for (size_t i=0;i<sizeof(levelConfigs)/sizeof(levelConfigs[0]);++i){
    if (levelConfigs[i].index == idx) return &levelConfigs[i];
  }
  return nullptr;
}

static void getLayoutBlocks(uint8_t& topBlocks, uint8_t& sideBlocks){
  if (!g.level) { topBlocks = 0; sideBlocks = 0; return; }
  if (g.level->useLayout2){
    topBlocks  = L2_TOP_BLOCKS;
    sideBlocks = L2_SIDE_BLOCKS;
  } else {
    topBlocks  = cfg.topBlocks;
    sideBlocks = cfg.sideBlocks;
  }
}

static void restartCurrentRound(){
  if (!g.level) return;
  startRound(g.roundIndex);
}

static void onRoundCleared(){
  if (!g.level) return;

  uint8_t nextRound = g.roundIndex + 1;
  if (nextRound < g.level->numRounds){
    DBG_PRINTF("[L%uR%u] round cleared, advancing to R%u\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (unsigned)(nextRound+1));
    startRound(nextRound);
    return;
  }

  if (g.roundKind != RK_SHRINKING && g.roundKind != RK_BONUS_MOVING){
    finishLevelAdvance();
  }
}

static void finishLevelAdvance(){
  if (g.levelIndex == 1){
    DBG_PRINTF("[L1] complete, advancing to Level 2 (score=%lu, lives=%u)\n",
      (unsigned long)g.score, (unsigned)g.lives);
    startLevel(2, false);
  } else if (g.levelIndex == 2){
    DBG_PRINTF("[L2] complete, advancing to Level 3 (score=%lu, lives=%u)\n",
      (unsigned long)g.score, (unsigned)g.lives);
    startLevel(3, false);
  } else if (g.levelIndex == 3){
    DBG_PRINTF("[L3] complete, advancing to Level 4 (score=%lu, lives=%u)\n",
      (unsigned long)g.score, (unsigned)g.lives);
    startLevel(4, false);
  } else if (g.levelIndex == 4){
    const uint32_t now = millis();
    const bool successTimerExpired =
      (g.gameEndMs > 0) && ((int32_t)(now - g.gameEndMs) >= 0);

    if (successTimerExpired){
      DBG_PRINTF("[L4] complete at success timer, ending SUCCESS. final score=%lu lives=%u\n",
        (unsigned long)g.score, (unsigned)g.lives);
      g.lastEndKind = SGE_SUCCESS;
      g.active = false;
      clearTargets();
      pushLedStates();
    } else {
      DBG_PRINTF("[L4] complete before 6:00 success timer, restarting Level 4. score=%lu lives=%u\n",
        (unsigned long)g.score, (unsigned)g.lives);
      startLevel(4, false);
    }
  }
}

static void onShrinkCleared(){
  shrink.active = false;
  clearTargets();
  pushLedStates();

  if (g.level){
    uint8_t nextRound = g.roundIndex + 1;
    if (nextRound < g.level->numRounds &&
        g.level->rounds[nextRound] == RK_BONUS_MOVING){
      DBG_PRINTF("[L%uR%u] shrinking round cleared, starting BONUS round R%u\n",
        (unsigned)g.levelIndex,
        (unsigned)(g.roundIndex+1),
        (unsigned)(nextRound+1));
      startRound(nextRound);
      return;
    }
  }

  finishLevelAdvance();
}

static void onBonusCleared(){
  bonus.active = false;
  clearTargets();
  pushLedStates();

  // Extra safety: explicitly tell clients bonus is over
  sendRoundInfo(false);

  DBG_PRINTF("[L%uR%u] BONUS round complete (score=%lu, lives=%u)\n",
    (unsigned)g.levelIndex,
    (unsigned)(g.roundIndex+1),
    (unsigned long)g.score,
    (unsigned)g.lives);

  finishLevelAdvance();
}

// ========== Rounds ==========
static void startRound(uint8_t roundIdx){
  if (!g.level) return;
  if (roundIdx >= g.level->numRounds) return;

  uint32_t now = millis();
  g.roundIndex   = roundIdx;
  g.roundKind    = g.level->rounds[roundIdx];
  g.roundStartMs = now;

  roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;
  hitFlashActive = false;
  memset(g.blockCleared, 0, sizeof(g.blockCleared));
  shrink.active = false;
  bonus.active  = false;

  sendRoundInfo(g.roundKind == RK_BONUS_MOVING);

  if (g.roundKind == RK_WARMUP){
    g.roundDurationMs = cfg.r1DurationMs;
  } else if (g.roundKind == RK_HIT_ALL_A){
    g.roundDurationMs = cfg.r2DurationMs;
  } else if (g.roundKind == RK_HIT_ALL_B){
    g.roundDurationMs = cfg.r3DurationMs;
  } else if (g.roundKind == RK_SHRINKING){
    g.roundDurationMs = cfg.r4DurationMs;
  } else if (g.roundKind == RK_BONUS_MOVING){
    g.roundDurationMs = BONUS_DURATION_MS;
  } else {
    g.roundDurationMs = 0;
  }

  uint8_t topBlocks=0, sideBlocks=0;
  getLayoutBlocks(topBlocks, sideBlocks);

  const uint8_t TOTAL_TOP_SENSORS  = TOP_SENSOR_COUNT;
  const uint8_t TOTAL_SIDE_SENSORS = SIDE_SENSOR_COUNT;

  if (g.roundKind == RK_WARMUP){
    bool allowTop  = (topBlocks  > 0);
    bool allowSide = (sideBlocks > 0);
    bool useTop    = true;
    if (allowTop && allowSide) useTop = (random(0,2)==0);
    else if (allowTop)         useTop = true;
    else if (allowSide)        useTop = false;

    g.arena = useTop ? ARENA_TOP : ARENA_SIDE;
    uint8_t blockCount = useTop ? topBlocks : sideBlocks;

    initBlocksForArena(blockCount, TOTAL_TOP_SENSORS, TOTAL_SIDE_SENSORS, false);

    g.activeBlock = (uint8_t)random(0, g.blockCount);
    rebuildTargetsFromBlocks();
    nextLedPushAt = now;

    DBG_PRINTF("[L%uR1] start Warmup arena=%s block %u/%u ids=%u..%u dur=%lus\n",
      (unsigned)g.levelIndex,
      (g.arena==ARENA_TOP?"TOP":"SIDE"),
      (unsigned)(g.activeBlock+1),
      (unsigned)g.blockCount,
      (unsigned)g.blockMin[g.activeBlock],
      (unsigned)g.blockMax[g.activeBlock],
      (unsigned long)(g.roundDurationMs/1000));
    return;
  }

  if (g.roundKind == RK_HIT_ALL_A){
    bool allowTop  = (topBlocks  > 0);
    bool allowSide = (sideBlocks > 0);
    bool useTop    = true;
    if (allowTop && allowSide) useTop = (random(0,2)==0);
    else if (allowTop)         useTop = true;
    else if (allowSide)        useTop = false;

    g.arenaA = useTop ? ARENA_TOP : ARENA_SIDE;
    g.arena  = g.arenaA;

    uint8_t blockCount = useTop ? topBlocks : sideBlocks;

    initBlocksForArena(blockCount, TOTAL_TOP_SENSORS, TOTAL_SIDE_SENSORS, true);

    rebuildTargetsFromBlocks();
    nextLedPushAt = now;

    DBG_PRINTF("[L%uR%u] start HitAll A arena=%s blocks=%u dur=%lus\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (g.arena==ARENA_TOP?"TOP":"SIDE"),
      (unsigned)g.blockCount,
      (unsigned long)(g.roundDurationMs/1000));
    return;
  }

  if (g.roundKind == RK_HIT_ALL_B){
    if (g.arenaA == ARENA_NONE){
      bool useTop = (random(0,2)==0);
      g.arenaA = useTop ? ARENA_TOP : ARENA_SIDE;
    }

    Arena arenaB = (g.arenaA == ARENA_TOP) ? ARENA_SIDE : ARENA_TOP;
    g.arena = arenaB;

    uint8_t useTopBlocks  = g.level->useLayout2 ? L2_TOP_BLOCKS  : cfg.topBlocks;
    uint8_t useSideBlocks = g.level->useLayout2 ? L2_SIDE_BLOCKS : cfg.sideBlocks;

    uint8_t blockCount = (arenaB == ARENA_TOP) ? useTopBlocks : useSideBlocks;

    initBlocksForArena(blockCount, TOTAL_TOP_SENSORS, TOTAL_SIDE_SENSORS, true);

    rebuildTargetsFromBlocks();
    nextLedPushAt = now;

    DBG_PRINTF("[L%uR%u] start HitAll B arena=%s blocks=%u dur=%lus\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (g.arena==ARENA_TOP?"TOP":"SIDE"),
      (unsigned)g.blockCount,
      (unsigned long)(g.roundDurationMs/1000));
    return;
  }

  if (g.roundKind == RK_SHRINKING){
    shrink.active = true;
    shrink.stage  = 0;
    shrinkHits    = 0;
    shrinkingInitWidths();

    bool useTop = (random(0,2)==0);
    g.arena = useTop ? ARENA_TOP : ARENA_SIDE;

    uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
    shrink.center = random(1, maxSensors+1);

    updateShrinkingTargets();
    nextLedPushAt = now;

    DBG_PRINTF("[L%uR%u] start Shrinking arena=%s center=%u dur=%lus (widths=%u,%u,%u)\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (g.arena==ARENA_TOP?"TOP":"SIDE"),
      (unsigned)shrink.center,
      (unsigned long)(g.roundDurationMs/1000),
      (unsigned)shrink.widths[0], (unsigned)shrink.widths[1], (unsigned)shrink.widths[2]);
    return;
  }

  if (g.roundKind == RK_BONUS_MOVING){
    bonus.active = true;
    bonus.width  = 3;
    bonus.dir    = (random(0,2)==0) ? 1 : -1;
    bonus.lastStepMs = now;

    bool useTop = (random(0,2)==0);
    g.arena = useTop ? ARENA_TOP : ARENA_SIDE;

    uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
    int halfSpan = bonus.width / 2;

    if (maxSensors <= bonus.width){
      bonus.center = (maxSensors+1)/2;
    } else {
      bonus.center = random(1+halfSpan, maxSensors-halfSpan+1);
    }

    bonus.minId = (uint8_t)(bonus.center - halfSpan);
    bonus.maxId = (uint8_t)(bonus.center + halfSpan);

    clearTargets();
    if (g.arena == ARENA_TOP){
      for (uint8_t id=bonus.minId; id<=bonus.maxId; ++id) setTopBit(id);
    } else if (g.arena == ARENA_SIDE){
      for (uint8_t id=bonus.minId; id<=bonus.maxId; ++id) setSideBit(id);
    }
    nextLedPushAt = now;

    DBG_PRINTF("[L%uR%u] start BONUS sliding arena=%s center=%u width=%u dur=%lus\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (g.arena==ARENA_TOP?"TOP":"SIDE"),
      (unsigned)bonus.center,
      (unsigned)bonus.width,
      (unsigned long)(g.roundDurationMs/1000));
    return;
  }
}

// ========== Level start ==========
static void startLevel(uint8_t levelIndex, bool resetScoreAndLives){
  const struct LevelConfig* lc = getLevelConfig(levelIndex);
  if (!lc) return;

  const uint32_t now = millis();

  // Extra safety: ensure bonus flag is off at level start
  sendRoundInfo(false);

  g.level      = lc;
  g.levelIndex = lc->index;
  g.penaltyMode= lc->penaltyMode;
  g.arena      = ARENA_NONE;
  g.arenaA     = ARENA_NONE;
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));

  if (resetScoreAndLives){
    g.score = 0;
    g.lives = 5;
    g.gameStartMs = now;
    g.gameEndMs   = now + SOCCER_SUCCESS_AFTER_MS;
    g.lastEndKind = SGE_NONE;

#if PMS_STD_ENABLED
    // New game => discard any pending score events from a previous run.
    pmsClearScoreQueue();
#endif
  } else if (g.gameStartMs == 0 || g.gameEndMs == 0){
    g.gameStartMs = now;
    g.gameEndMs   = now + SOCCER_SUCCESS_AFTER_MS;
  }

  sendMode(g.penaltyMode);

  DBG_PRINTF("[GAME] start Level %u  penalty=%s  lives=%u  score=%lu\n",
    (unsigned)g.levelIndex,
    g.penaltyMode ? "YES" : "NO",
    (unsigned)g.lives,
    (unsigned long)g.score);

  startRound(0);
}

// ========== Sensor handling ==========
static void handleSensorEvent(const SensorEventMsg* m){
  rememberHello(m->name);
  if (m->state != 1) return;

  bool isTopL  = !strcasecmp(m->name, NAME_TOPLEFT);
  bool isTopR  = !strcasecmp(m->name, NAME_TOPRIGHT);
  bool isSide  = !strcasecmp(m->name, NAME_SIDE);

  Arena   eventArena = ARENA_NONE;
  uint8_t id         = m->sensor_id;

  if ((isTopL || isTopR) && id >= 1 && id <= TOP_SENSOR_COUNT){
    eventArena = ARENA_TOP;
  } else if (isSide && id >= 1 && id <= SIDE_SENSOR_COUNT){
    eventArena = ARENA_SIDE;
  } else {
    return;
  }

  // ===== TEST MODE =====
  // Light up the last triggered sensor as a target, so you can verify
  // sensor wiring and the LED segment mapping visually.
  if (testMode){
    uint32_t now = millis();
    if (eventArena == ARENA_TOP){
      memset(targetTopBits, 0, sizeof(targetTopBits));
      setTopBit(id);
    } else if (eventArena == ARENA_SIDE){
      memset(targetSideBits, 0, sizeof(targetSideBits));
      setSideBit(id);
    }
    nextLedPushAt = now;
    DBG_PRINTF("[TEST] %s arena=%s sensor=%u\n",
      m->name,
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)id);
    return;
  }

  if (!g.active) return;

  // Wrong arena hits are simply ignored (per your physical layout)
  if (eventArena != g.arena) return;

  uint32_t now = millis();

  // Ignore hits right after round start/shrink
  if (now < roundChangeIgnoreUntil) return;

  // Global hit grace window
  if ((uint32_t)(now - lastHitGlobalMs) < HIT_GLOBAL_GRACE_MS) return;

  // Per-sensor re-arm
  if (id < (sizeof(lastScoreMsBySensor)/sizeof(lastScoreMsBySensor[0]))){
    uint32_t last = lastScoreMsBySensor[id];
    if ((uint32_t)(now - last) < cfg.scoreRearmMs) return;
    lastScoreMsBySensor[id] = now;
  }

  // ===== SHRINKING (Round 4) =====
  if (g.roundKind == RK_SHRINKING){
    if (!shrink.active) return;
    uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
    uint8_t minOk, maxOk;
    computeGraceRange(shrink.minId, shrink.maxId, maxSensors, g.arena, minOk, maxOk);

    bool inTarget = (id >= minOk && id <= maxOk);
    if (!inTarget){
      if (g.penaltyMode){
        loseLifeOnly("WRONG HIT R4");
      }
      return;
    }

    addScore(1, false);
    shrinkHits++;

    // Global hit grace
    lastHitGlobalMs = now;

    DBG_PRINTF("[L%uR%u] HIT (SHRINKING) arena=%s sensor=%u stage=%u hits=%u score=%lu\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)id,
      (unsigned)shrink.stage,
      (unsigned)shrinkHits,
      (unsigned long)g.score);

    if (shrink.stage < 2){
      shrink.stage++;
      updateShrinkingTargets();
      // Add grace after shrink so immediate out-of-window hits don't cost life
      roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;
      nextLedPushAt = now;
    } else {
      onShrinkCleared();
    }
    return;
  }

    // ===== BONUS MOVING (Round 5) - with configurable forgiveness =====
  if (g.roundKind == RK_BONUS_MOVING){
    if (!bonus.active) return;

    uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
    uint8_t minOk, maxOk;
    computeGraceRange(bonus.minId, bonus.maxId, maxSensors, g.arena, minOk, maxOk);

    bool inTarget = (id >= minOk && id <= maxOk);

    if (!inTarget){
      // Bonus: wrong hits are just ignored, never cost lives
      return;
    }

    addScore(SOCCER_BONUS_SCORE_DELTA, true);

    // Global hit grace
    lastHitGlobalMs = now;

    DBG_PRINTF("[L%uR%u] HIT (BONUS) arena=%s sensor=%u score=%lu\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)id,
      (unsigned long)g.score);

    onBonusCleared();
    return;
  }


  // ===== WARMUP =====
  if (g.roundKind == RK_WARMUP){
    if (g.blockCount == 0) return;
    uint8_t b = g.activeBlock;
    if (b >= g.blockCount) return;
    // Current active block range
    uint8_t blockMin = g.blockMin[b];
    uint8_t blockMax = g.blockMax[b];

    // Allow a forgiveness margin around the active block (± GRACE_SENSORS (per-arena))
    uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
    uint8_t minOk, maxOk;
    computeGraceRange(blockMin, blockMax, maxSensors, g.arena, minOk, maxOk);

    bool inTarget = (id >= minOk && id <= maxOk);

    if (!inTarget){
      // Outside the expanded window -> wrong hit
      if (g.penaltyMode){
        loseLifeOnly("WRONG HIT R1");
      }
      return;
    }

    // Correct hit (inside block OR within grace)
    addScore(1, false);


    // Global hit grace
    lastHitGlobalMs = now;

    // Flash the block that was just hit
    hitFlashActive  = true;
    hitFlashBlock   = b;
    hitFlashUntilMs = now + HIT_FLASH_MS;

    // Pick a new active block within the same arena
    if (g.blockCount > 1){
      uint8_t newBlock = random(0, g.blockCount - 1);
      if (newBlock >= b) newBlock++;   // ensure different from previous
      g.activeBlock = newBlock;
    } else {
      g.activeBlock = b; // only one block available
    }

    // Rebuild targets for the newly selected active block
    rebuildTargetsFromBlocks();
    nextLedPushAt   = now;

    DBG_PRINTF("[L%uR1] HIT arena=%s sensor=%u score=%lu nextBlock=%u/%u\n",
      (unsigned)g.levelIndex,
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)id,
      (unsigned long)g.score,
      (unsigned)(g.activeBlock+1),
      (unsigned)g.blockCount);
    return;
  }

    // ===== HIT_ALL_A / HIT_ALL_B =====
  if (g.roundKind == RK_HIT_ALL_A || g.roundKind == RK_HIT_ALL_B){
    if (g.blockCount == 0) return;

    uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;

    // Credit the hit to an *uncleared* (green) block if the sensor is within
    // that block OR within ±GRACE_SENSORS (per-arena) of that block boundary.
    // Cleared (red) blocks are never "correct", and they do NOT get any grace.
    int8_t  bestBlock = -1;
    uint8_t bestDist  = 255;

    for (uint8_t bIdx=0; bIdx<g.blockCount; ++bIdx){
      if (g.blockCleared[bIdx]) continue;

      uint8_t minOk, maxOk;
      computeGraceRange(g.blockMin[bIdx], g.blockMax[bIdx], maxSensors, g.arena, minOk, maxOk);

      if (id < minOk || id > maxOk) continue;

      uint8_t dist = 0;
      if (id < g.blockMin[bIdx]) dist = (uint8_t)(g.blockMin[bIdx] - id);
      else if (id > g.blockMax[bIdx]) dist = (uint8_t)(id - g.blockMax[bIdx]);

      if (dist < bestDist){
        bestDist  = dist;
        bestBlock = (int8_t)bIdx;
        if (dist == 0) break; // can't beat an in-block hit
      }
    }

    if (bestBlock >= 0){
      g.blockCleared[(uint8_t)bestBlock] = true;
      addScore(1, false);

      // Global hit grace
      lastHitGlobalMs = now;

      DBG_PRINTF("[L%uR%u] HIT arena=%s block=%u/%u sensor=%u (graceDist=%u) score=%lu\n",
        (unsigned)g.levelIndex,
        (unsigned)(g.roundIndex+1),
        (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
        (unsigned)((uint8_t)bestBlock+1),
        (unsigned)g.blockCount,
        (unsigned)id,
        (unsigned)bestDist,
        (unsigned long)g.score);

      rebuildTargetsFromBlocks();
      nextLedPushAt = now;

      bool allCleared = true;
      for (uint8_t bIdx=0; bIdx<g.blockCount; ++bIdx){
        if (!g.blockCleared[bIdx]){
          allCleared = false;
          break;
        }
      }

      if (allCleared){
        onRoundCleared();
      }
      return;
    }

    // No uncleared block within grace -> only penalize if the hit is in a cleared block.
    uint8_t bFound = 255;
    for (uint8_t bIdx=0; bIdx<g.blockCount; ++bIdx){
      if (id >= g.blockMin[bIdx] && id <= g.blockMax[bIdx]){
        bFound = bIdx;
        break;
      }
    }

    if (bFound == 255) return;

    if (g.blockCleared[bFound] && g.penaltyMode){
      loseLifeOnly("HIT CLEARED BLOCK R2/R3");
    }
    return;
  }



}

// ========== RX ==========
static void onNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len){
  if (len < (int)sizeof(MsgHeader)) return;
  const MsgHeader* h=(const MsgHeader*)data;
  if (!headerValid(h)) return;

  
// HELLO (clients announce themselves at boot; includes optional build/bootCount)
const int HELLO_V1_SIZE = (int)(sizeof(MsgHeader) + 32); // header + name[32]
if (h->kind == MSG_HELLO && len >= HELLO_V1_SIZE){
  const HelloMsg* m=(const HelloMsg*)data;

  // Backward compatible: older clients only send name[].
  uint32_t boot = 0;
  const char* build = "legacy";
  if (len >= (int)(offsetof(HelloMsg, bootCount) + sizeof(uint32_t))){
    boot = m->bootCount;
  }
  if (len >= (int)(offsetof(HelloMsg, build) + 1)){
    build = m->build;
    if (!build[0]) build = "unknown";
  }

  notePeerMac(m->name, info->src_addr);
  bool shouldPrint = rememberHello(m->name, boot, build);
  if (shouldPrint){
    DBG_PRINTF("[HELLO] %s  boot=%lu  build=%s\n",
      m->name, (unsigned long)boot, build);
  }

  uint32_t now = millis();
  if (!strcasecmp(m->name, NAME_TOPRIGHT) || !strcasecmp(m->name, NAME_TOPLEFT) || !strcasecmp(m->name, NAME_SIDE)) {
    if ((int32_t)(now - nextLedPushAt) > 0) nextLedPushAt = now;
  }
  return;
}


  if (h->kind == MSG_SENSOR_EVENT && len >= (int)sizeof(SensorEventMsg)){
    const SensorEventMsg* m=(const SensorEventMsg*)data;
    notePeerMac(m->name, info->src_addr);
    handleSensorEvent(m);
    DBG_PRINTF("[EVT] %s s%u gpio%u state=%u seq=%u\n",
      m->name, m->sensor_id, m->gpio, m->state, m->seq);
    return;
  }
}

// ========== CLI ==========
static void printCfg(){
  DBG_PRINTLN("Config:");
  DBG_PRINTF("  R1DUR = %lu ms\n", (unsigned long)cfg.r1DurationMs);
  DBG_PRINTF("  R2DUR = %lu ms\n", (unsigned long)cfg.r2DurationMs);
  DBG_PRINTF("  R3DUR = %lu ms\n", (unsigned long)cfg.r3DurationMs);
  DBG_PRINTF("  R4DUR = %lu ms\n", (unsigned long)cfg.r4DurationMs);
  DBG_PRINTF("  L1TOP = %u blocks\n", cfg.topBlocks);
  DBG_PRINTF("  L1SIDE= %u blocks\n", cfg.sideBlocks);
  DBG_PRINTF("  L2TOP = %u blocks (fixed)\n", (unsigned)L2_TOP_BLOCKS);
  DBG_PRINTF("  L2SIDE= %u blocks (fixed)\n", (unsigned)L2_SIDE_BLOCKS);
  DBG_PRINTF("  REARM = %lu ms\n", (unsigned long)cfg.scoreRearmMs);
  DBG_PRINTF("  GRACE = %lu ms\n", (unsigned long)cfg.roundChangeGraceMs);
  DBG_PRINTF("  GRACE_TOP_SENSORS  = %u\n", (unsigned)graceTopSensors);
  DBG_PRINTF("  GRACE_SIDE_SENSORS = %u\n", (unsigned)graceSideSensors);
  DBG_PRINTF("  BONUS = %lu ms\n", (unsigned long)BONUS_DURATION_MS);
  DBG_PRINTLN();
}

static void printHelp(){
  DBG_PRINTLN();
  DBG_PRINTLN("=== Soccer Server CLI ===");
  DBG_PRINTLN("WIFI ALL <ssid> <pass>");
  DBG_PRINTLN("WIFI <name> <ssid> <pass>");
  DBG_PRINTLN("OTA ALL <url>");
  DBG_PRINTLN("OTA <name> <url>");
  DBG_PRINTLN("NAME <ALL|currentName> <newName>");
  DBG_PRINTLN("PIN <name> <sensorId> <gpio>  (configure client sensor GPIO; ex: PIN Soccer-topleft 14 19)");
  DBG_PRINTLN("LIST");
  DBG_PRINTLN("CHAN <1-13>   (set ESPNOW channel for ALL Soccer nodes, save+reboot)");
  DBG_PRINTLN("GAME START   (Level 1)");
  DBG_PRINTLN("GAME L1      (Level 1)");
  DBG_PRINTLN("GAME L2      (Level 2)");
  DBG_PRINTLN("GAME L3      (Level 3 - penalty mode, layout 1, WITH warmup + bonus)");
  DBG_PRINTLN("GAME L4      (Level 4 - penalty mode, layout 2, WITH warmup + bonus)");
  DBG_PRINTLN("GAME STOP");
  DBG_PRINTLN("TEST ON     (sensor -> LED mapping test mode)");
  DBG_PRINTLN("TEST OFF");
  DBG_PRINTLN("TEST CLEAR  (turn all targets off while staying in TEST mode)");
  DBG_PRINTLN("TEST TOP <id>   (force a top segment to light)");
  DBG_PRINTLN("TEST SIDE <id>  (force a side segment to light)");
  DBG_PRINTLN("CFG");
  DBG_PRINTLN("SET <R1DUR|R2DUR|R3DUR|R4DUR|L1TOP|L1SIDE|REARM|GRACE|TOPGRACE|SIDEGRACE> <value>");
  DBG_PRINTLN();
  DBG_PRINTLN("Examples:");
  DBG_PRINTLN("  WIFI ALL AndrewiPhone 12345678");
  DBG_PRINTLN("  OTA ALL http://172.20.10.3:8000/Soccer/SoccerClient/build/esp32.esp32.esp32/SoccerClient.ino.bin");
  DBG_PRINTLN("  GAME START");
  DBG_PRINTLN("  GAME L3");
  DBG_PRINTLN("===========================");
}


// ========== PMS v1 (!PMS) Standard Serial Protocol ==========
// NOTE: PMS output lines always start with "!PMS" and are NEVER suppressed by PMS_DEBUG_SERIAL.
static bool pmsStopRequested = false;

#if PMS_STD_ENABLED

static int32_t pmsParseKeyInt(const String& args, const char* key, int32_t defaultVal){
  String pattern = String(key) + "=";
  int idx = args.indexOf(pattern);
  if (idx < 0) return defaultVal;

  idx += pattern.length();
  int end = idx;
  while (end < (int)args.length()){
    char c = args.charAt(end);
    if (c==' ' || c=='\t' || c=='\r' || c=='\n') break;
    end++;
  }
  String val = args.substring(idx, end);
  val.trim();
  if (!val.length()) return defaultVal;
  return val.toInt();
}

static String pmsFirstToken(const String& s){
  int sp = s.indexOf(' ');
  if (sp < 0) return s;
  return s.substring(0, sp);
}

static String pmsAfterFirstToken(const String& s){
  int sp = s.indexOf(' ');
  if (sp < 0) return "";
  return s.substring(sp + 1);
}

static const char* pmsStateStr(){
  // Soccer uses Warmup round as "arming"
  if (!g.active) return "idle";
  if (g.roundKind == RK_WARMUP) return "arming";
  return "playing";
}

static void pmsPrintPong(){
  PMS_PRINTLN("!PMS PONG v=1 game=soccer role=server");
}

static void pmsPrintEventGameStart(uint8_t level){
  PMS_PRINT("!PMS EVENT v=1 name=game_start level=");
  PMS_PRINTLN(level);
}

static void pmsPrintEventGameEnd(const char* reason, uint32_t score, uint8_t lives){
  PMS_PRINT("!PMS EVENT v=1 name=game_end reason=");
  PMS_PRINT(reason);
  PMS_PRINT(" score=");
  PMS_PRINT(score);
  PMS_PRINT(" lives=");
  PMS_PRINTLN(lives);
}

static void pmsPrintEventScore(int32_t delta, uint32_t total, bool bonus){
  PMS_PRINT("!PMS EVENT v=1 name=score delta=");
  PMS_PRINT(delta);
  PMS_PRINT(" total=");
  PMS_PRINT(total);
  PMS_PRINT(" bonus=");
  PMS_PRINTLN(bonus ? 1 : 0);
}

// --- Score event queue (producer: ESPNOW receive callback; consumer: pmsTick) ---
// This prevents Serial interleaving and guarantees that the `bonus=` flag reflects
// *what was hit* (bonus window or not), instead of whatever round happens to be
// active when the 250ms PMS tick runs.
//
// NOTE (Arduino .ino quirk):
// Avoid custom types in function signatures here because the Arduino preprocessor
// can auto-generate prototypes before the type is declared.
static const uint8_t PMS_SCORE_Q_CAP = 16;

// Ring buffer storage
static int32_t  pmsScoreDeltaQ[PMS_SCORE_Q_CAP];
static uint32_t pmsScoreTotalQ[PMS_SCORE_Q_CAP];
static uint8_t  pmsScoreBonusQ[PMS_SCORE_Q_CAP]; // 0/1
static uint8_t  pmsScoreHead = 0;
static uint8_t  pmsScoreTail = 0;
static portMUX_TYPE  pmsScoreMux  = portMUX_INITIALIZER_UNLOCKED;

static void pmsClearScoreQueue(){
  portENTER_CRITICAL(&pmsScoreMux);
  pmsScoreHead = 0;
  pmsScoreTail = 0;
  portEXIT_CRITICAL(&pmsScoreMux);
}

static void pmsQueueScoreEvent(int32_t delta, uint32_t total, bool bonus){
  portENTER_CRITICAL(&pmsScoreMux);

  uint8_t next = (uint8_t)(pmsScoreHead + 1);
  if (next >= PMS_SCORE_Q_CAP) next = 0;

  // If the queue is full, drop the oldest event.
  if (next == pmsScoreTail){
    pmsScoreTail = (uint8_t)(pmsScoreTail + 1);
    if (pmsScoreTail >= PMS_SCORE_Q_CAP) pmsScoreTail = 0;
  }

  pmsScoreDeltaQ[pmsScoreHead] = delta;
  pmsScoreTotalQ[pmsScoreHead] = total;
  pmsScoreBonusQ[pmsScoreHead] = bonus ? 1 : 0;
  pmsScoreHead = next;

  portEXIT_CRITICAL(&pmsScoreMux);
}

static bool pmsPopScoreEvent(int32_t &delta, uint32_t &total, bool &bonus){
  portENTER_CRITICAL(&pmsScoreMux);
  if (pmsScoreTail == pmsScoreHead){
    portEXIT_CRITICAL(&pmsScoreMux);
    return false;
  }

  delta = pmsScoreDeltaQ[pmsScoreTail];
  total = pmsScoreTotalQ[pmsScoreTail];
  bonus = (pmsScoreBonusQ[pmsScoreTail] != 0);

  pmsScoreTail = (uint8_t)(pmsScoreTail + 1);
  if (pmsScoreTail >= PMS_SCORE_Q_CAP) pmsScoreTail = 0;

  portEXIT_CRITICAL(&pmsScoreMux);
  return true;
}

static void pmsFlushScoreEvents(){
  int32_t  delta;
  uint32_t total;
  bool     bonus;
  while (pmsPopScoreEvent(delta, total, bonus)){
    pmsPrintEventScore(delta, total, bonus);
  }
}

static void pmsPrintEventLife(int32_t delta, uint8_t lives){
  PMS_PRINT("!PMS EVENT v=1 name=life delta=");
  PMS_PRINT(delta);
  PMS_PRINT(" lives=");
  PMS_PRINTLN(lives);
}

static void pmsPrintStatus(const char* state, uint8_t level, uint32_t score, uint8_t lives, uint32_t tleftMs, const char* lastReason){
  PMS_PRINT("!PMS STATUS v=1 state=");
  PMS_PRINT(state);
  PMS_PRINT(" level=");
  PMS_PRINT(level);
  PMS_PRINT(" score=");
  PMS_PRINT(score);
  PMS_PRINT(" lives=");
  PMS_PRINT(lives);
  PMS_PRINT(" tleft_ms=");
  PMS_PRINT(tleftMs);
  PMS_PRINT(" last_reason=");
  PMS_PRINTLN(lastReason);
}

// PMS input:
//   !PMS PING
//   !PMS START level=1
//   !PMS STOP
static void handlePmsLine(const String& rawLine){
  String line = rawLine;
  line.trim();
  if (!line.startsWith("!PMS")) return;

  String rest = line.substring(4);
  rest.trim();
  if (!rest.length()) return;

  String kind = pmsFirstToken(rest);
  String args = pmsAfterFirstToken(rest);
  kind.toUpperCase();

  if (kind == "PING"){
    pmsPrintPong();
    return;
  }

  if (kind == "START"){
    int32_t level = pmsParseKeyInt(args, "level", 1);
    if (level < 1) level = 1;
    if (level > 4) level = 4;

    // Ignore START if already active (helps PMS retries without restarting the game).
    if (g.active) return;

    // Ensure we are not in TEST mode when starting a real game.
    testMode = false;

    // Reset "manual stop" flag for a new run.
    pmsStopRequested = false;

    g.active = true;
    startLevel((uint8_t)level, true);
    return;
  }

  if (kind == "STOP"){
    if (!g.active) return;

    pmsStopRequested = true;
    g.lastEndKind = SGE_STOPPED;

    g.active = false;
    clearTargets();
    pushLedStates();
    return;
  }

  // Unknown PMS command: stay silent in production; log only in debug builds.
  DBG_PRINT("Unknown PMS command: ");
  DBG_PRINTLN(rawLine);
}

#endif // PMS_STD_ENABLED

static void handleSerialServer(){
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;

#if PMS_STD_ENABLED
  if (line.startsWith("!PMS")){ handlePmsLine(line); return; }
#endif

  String norm; norm.reserve(line.length()); bool sp=false;
  for (size_t i=0;i<line.length();++i){
    char c=line[i];
    if (c==' '||c=='\t'){
      if(!sp){norm+=' '; sp=true;}
    } else {
      norm+=c; sp=false;
    }
  }

  int sp1 = norm.indexOf(' ');
  String cmd = (sp1<0)? norm : norm.substring(0,sp1); cmd.toUpperCase(); cmd.trim();

  String arg1="", rest="";
  if (sp1>=0){
    rest=norm.substring(sp1+1); rest.trim();
    int sp2=rest.indexOf(' ');
    if (sp2<0){
      arg1=rest; rest="";
    } else {
      arg1=rest.substring(0,sp2); arg1.trim();
      rest=rest.substring(sp2+1); rest.trim();
    }
  }

  if (cmd=="HELP"){ printHelp(); return; }
  if (cmd=="LIST"){
    helloDripActive=true; helloDripRemain=HELLO_DRIP_COUNT;
    helloNextAt=millis(); listPrintAt=0;
    DBG_PRINTLN("Requesting HELLOs (drip)...");
    return;
  }
  if (cmd=="CHAN" || cmd=="CHANNEL"){
    if(!arg1.length()){ DBG_PRINTLN("Usage: CHAN <1-13>"); return; }
    int ch = arg1.toInt();
    if (ch < 1 || ch > 13){ DBG_PRINTLN("Channel must be 1..13"); return; }

    DBG_PRINTF("[RADIO] Sending CHAN_SET=%d to ALL clients, then rebooting server...\n", ch);
    sendChanSetAll((uint8_t)ch);

    delay(250); // give packets time to go out
    nvsSaveChannel((uint8_t)ch);
    delay(100);
    ESP.restart();
  }

  if (cmd=="WIFI"){
    if(!arg1.length()){DBG_PRINTLN("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    int sp=rest.indexOf(' ');
    if(sp<0){DBG_PRINTLN("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    String ssid=rest.substring(0,sp), pass=rest.substring(sp+1); ssid.trim(); pass.trim();
    if(!ssid.length()||!pass.length()){DBG_PRINTLN("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    sendWifiSet(arg1.c_str(),ssid,pass);
    DBG_PRINTF("Sent WIFI_SET to %s (ssid=%s)\n",arg1.c_str(),ssid.c_str());
    return;
  }
  if (cmd=="OTA"){
    if(!arg1.length()||!rest.length()){DBG_PRINTLN("Usage: OTA <ALL|name> <url>"); return;}
    sendOta(arg1.c_str(),rest);
    DBG_PRINTF("Sent OTA_TRIGGER to %s  url=%s\n",arg1.c_str(),rest.c_str());
    return;
  }
  if (cmd=="NAME"){
    if(!arg1.length()||!rest.length()){DBG_PRINTLN("Usage: NAME <ALL|currentName> <newName>");return;}
    sendNameSet(arg1.c_str(),rest);
    DBG_PRINTF("Sent NAME_SET to %s -> %s\n",arg1.c_str(),rest.c_str());
    return;
  }
  if (cmd=="PIN"){
    if(!arg1.length()||!rest.length()){
      DBG_PRINTLN("Usage: PIN <name> <sensorId> <gpio>");
      return;
    }
    int sp=rest.indexOf(' ');
    if(sp<0){
      DBG_PRINTLN("Usage: PIN <name> <sensorId> <gpio>");
      return;
    }
    String sIdStr = rest.substring(0,sp); sIdStr.trim();
    String gpioStr = rest.substring(sp+1); gpioStr.trim();
    int sid  = sIdStr.toInt();
    int gpio = gpioStr.toInt();
    if (sid < 1 || sid > 32){
      DBG_PRINTLN("sensorId must be 1..32");
      return;
    }
    if (gpio < 0 || gpio > 39){
      DBG_PRINTLN("gpio must be 0..39");
      return;
    }
    sendPinSet(arg1.c_str(), (uint8_t)sid, (uint8_t)gpio);
    DBG_PRINTF("Sent PIN_SET to %s  sensor=%d  gpio=%d\n", arg1.c_str(), sid, gpio);
    return;
  }

  if (cmd=="GAME"){
    if (!arg1.length()){
      DBG_PRINTLN("Usage: GAME START|L1|L2|L3|L4|STOP");
      return;
    }
    if (arg1.equalsIgnoreCase("START") || arg1.equalsIgnoreCase("L1")){
      if (g.active){
        DBG_PRINTLN("Game already active. Use GAME STOP first.");
      } else {
        testMode = false;
        pmsStopRequested = false;
        g.active = true;
        startLevel(1, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L2")){
      if (g.active){
        DBG_PRINTLN("Game already active. Use GAME STOP first.");
      } else {
        testMode = false;
        pmsStopRequested = false;
        g.active = true;
        startLevel(2, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L3")){
      if (g.active){
        DBG_PRINTLN("Game already active. Use GAME STOP first.");
      } else {
        testMode = false;
        pmsStopRequested = false;
        g.active = true;
        startLevel(3, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L4")){
      if (g.active){
        DBG_PRINTLN("Game already active. Use GAME STOP first.");
      } else {
        testMode = false;
        pmsStopRequested = false;
        g.active = true;
        startLevel(4, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("STOP")){
      if (!g.active) DBG_PRINTLN("No active game.");
      else {
        pmsStopRequested = true;
        g.lastEndKind = SGE_STOPPED;
        g.active = false;
        clearTargets();
        pushLedStates();
        DBG_PRINTLN("[GAME] stopped by CLI.");
      }
      return;
    }
    DBG_PRINTLN("Usage: GAME START|L1|L2|L3|L4|STOP");
    return;
  }
  if (cmd=="TEST"){
    if (!arg1.length()){
      DBG_PRINTLN("Usage: TEST ON|OFF|CLEAR|TOP <id>|SIDE <id>");
      return;
    }
    // Force a specific sensor to light (no physical trigger needed)
    if (arg1.equalsIgnoreCase("TOP") || arg1.equalsIgnoreCase("SIDE")){
      if (!rest.length()){
        DBG_PRINTLN("Usage: TEST TOP <id>   or   TEST SIDE <id>");
        return;
      }
      int id = rest.toInt();
      if (arg1.equalsIgnoreCase("TOP")){
        if (id < 1 || id > TOP_SENSOR_COUNT){ DBG_PRINTLN("TOP id out of range"); return; }
      } else {
        if (id < 1 || id > SIDE_SENSOR_COUNT){ DBG_PRINTLN("SIDE id out of range"); return; }
      }

      if (g.active){
        pmsStopRequested = true;
        g.lastEndKind = SGE_STOPPED;
        g.active = false;
        DBG_PRINTLN("[TEST] Stopping active game.");
      }

      testMode = true;
      hitFlashActive = false;
      clearTargets();

      // Force clients into a simple, readable state.
      sendMode(false);
      sendRoundInfo(false);

      if (arg1.equalsIgnoreCase("TOP")) setTopBit((uint8_t)id);
      else setSideBit((uint8_t)id);

      nextLedPushAt = millis();
      DBG_PRINTF("[TEST] Forced %s sensor=%d\n", arg1.c_str(), id);
      return;
    }

    if (arg1.equalsIgnoreCase("ON")){
      // Stop any active game so we don't mix targets / rules.
      if (g.active){
        pmsStopRequested = true;
        g.lastEndKind = SGE_STOPPED;
        g.active = false;
        DBG_PRINTLN("[TEST] Stopping active game.");
      }

      testMode = true;
      hitFlashActive = false;
      clearTargets();

      // Force clients into a simple, readable state.
      sendMode(false);
      sendRoundInfo(false);

      nextLedPushAt = millis();
      DBG_PRINTLN("[TEST] ON. Trigger a sensor; the corresponding LED segment should light GREEN.");
      return;
    }
    if (arg1.equalsIgnoreCase("OFF")){
      testMode = false;
      hitFlashActive = false;
      clearTargets();
      nextLedPushAt = millis();
      DBG_PRINTLN("[TEST] OFF.");
      return;
    }
    if (arg1.equalsIgnoreCase("CLEAR")){
      clearTargets();
      nextLedPushAt = millis();
      DBG_PRINTLN("[TEST] Cleared targets.");
      return;
    }
    DBG_PRINTLN("Usage: TEST ON|OFF|CLEAR|TOP <id>|SIDE <id>");
    return;
  }
  if (cmd=="CFG"){
    printCfg();
    return;
  }
  if (cmd=="SET"){
    if (!arg1.length() || !rest.length()){
      DBG_PRINTLN("Usage: SET <R1DUR|R2DUR|R3DUR|R4DUR|L1TOP|L1SIDE|REARM|GRACE|TOPGRACE|SIDEGRACE> <value>");
      return;
    }
    long v = rest.toInt();
    if (arg1.equalsIgnoreCase("R1DUR")){
      if (v <= 0){ DBG_PRINTLN("R1DUR must be >0"); return; }
      cfg.r1DurationMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("R2DUR")){
      if (v <= 0){ DBG_PRINTLN("R2DUR must be >0"); return; }
      cfg.r2DurationMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("R3DUR")){
      if (v <= 0){ DBG_PRINTLN("R3DUR must be >0"); return; }
      cfg.r3DurationMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("R4DUR")){
      if (v <= 0){ DBG_PRINTLN("R4DUR must be >0"); return; }
      cfg.r4DurationMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("L1TOP")){
      if (v < 1 || v > 8){ DBG_PRINTLN("L1TOP must be 1..8"); return; }
      cfg.topBlocks = (uint8_t)v;
    } else if (arg1.equalsIgnoreCase("L1SIDE")){
      if (v < 1 || v > 8){ DBG_PRINTLN("L1SIDE must be 1..8"); return; }
      cfg.sideBlocks = (uint8_t)v;
    } else if (arg1.equalsIgnoreCase("REARM")){
      if (v < 0){ DBG_PRINTLN("REARM must be >=0"); return; }
      cfg.scoreRearmMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("GRACE")){
      if (v < 0){ DBG_PRINTLN("GRACE must be >=0"); return; }
      cfg.roundChangeGraceMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("TOPGRACE")){
      if (v < 0 || v > 6){ DBG_PRINTLN("TOPGRACE must be 0..6"); return; }
      graceTopSensors = (uint8_t)v;
    } else if (arg1.equalsIgnoreCase("SIDEGRACE")){
      if (v < 0 || v > 6){ DBG_PRINTLN("SIDEGRACE must be 0..6"); return; }
      graceSideSensors = (uint8_t)v;
    } else {
      DBG_PRINTLN("Unknown key. Use R1DUR, R2DUR, R3DUR, R4DUR, L1TOP, L1SIDE, REARM, GRACE, TOPGRACE, SIDEGRACE");
      return;
    }
    DBG_PRINTF("SET %s = %ld\n", arg1.c_str(), v);
    return;
  }

  DBG_PRINTLN("Unknown command. Type HELP.");
}


#if PMS_STD_ENABLED

static uint32_t pmsLastTickMs = 0;
static bool     pmsPrevValid  = false;
static bool     pmsPrevActive = false;
static uint32_t pmsPrevScore  = 0;
static uint8_t  pmsPrevLives  = 0;
static uint8_t  pmsPrevLevel  = 1;
static uint8_t  pmsPrevRoundIndex = 0;
static RoundKind pmsPrevRoundKind = RK_NONE;

static const char* pmsLastReasonStr(bool stateChanged, int64_t scoreDelta, int32_t livesDelta){
  if (livesDelta < 0) return "life";
  if (scoreDelta > 0) return "score";
  if (stateChanged)   return "state";
  return "none";
}

// Prints STATUS every PMS_STATUS_PERIOD_MS while arming/playing (silent while idle),
// and emits EVENT lines based on state/score/life changes.
static void pmsTick(){
  uint32_t now = millis();
  if ((uint32_t)(now - pmsLastTickMs) < (uint32_t)PMS_STATUS_PERIOD_MS) return;
  pmsLastTickMs = now;

  bool     active = g.active;
  uint8_t  level  = g.levelIndex ? g.levelIndex : 1;
  uint32_t score  = g.score;
  uint8_t  lives  = g.lives;

  uint32_t tleftMs = 0;
  if (active && g.roundDurationMs > 0){
    uint32_t elapsed = (uint32_t)(now - g.roundStartMs);
    tleftMs = (elapsed >= g.roundDurationMs) ? 0 : (g.roundDurationMs - elapsed);
  }

  if (!pmsPrevValid){
    pmsPrevValid = true;

    // First PMS tick since boot/attach.
    // If a game is already active, announce it and flush any early score events
    // that may have occurred before the first tick.
    if (active){
      pmsPrintEventGameStart(level);
      pmsFlushScoreEvents();
      pmsPrintStatus(pmsStateStr(), level, score, lives, tleftMs, "state");
    }

    // Establish baseline
    pmsPrevActive = active;
    pmsPrevScore = score;
    pmsPrevLives = lives;
    pmsPrevLevel = level;
    pmsPrevRoundIndex = g.roundIndex;
    pmsPrevRoundKind  = g.roundKind;
    return;
  }

  bool stateChanged = (active != pmsPrevActive);

  // Treat level/round changes as a "state" change for last_reason (helps UI sync).
  if (active && pmsPrevActive){
    if (level != pmsPrevLevel ||
        g.roundIndex != pmsPrevRoundIndex ||
        g.roundKind  != pmsPrevRoundKind){
      stateChanged = true;
    }
  }

  int64_t scoreDelta = (int64_t)score - (int64_t)pmsPrevScore;
  int32_t livesDelta = (int32_t)lives - (int32_t)pmsPrevLives;

  bool started = (!pmsPrevActive && active);
  bool ended   = (pmsPrevActive && !active);

  // --- Events ---
  if (started){
    pmsPrintEventGameStart(level);
  }

  // Emit life events even if the last life-loss ended the game (active just turned off).
  if (livesDelta < 0 && (active || pmsPrevActive)){
    pmsPrintEventLife(livesDelta, lives);
  }

  // Flush queued score events. This guarantees:
  //  - bonus=1 only when a BONUS hit was actually scored
  //  - score events aren't lost if the game ends between ticks
  //  - score never appears before game_start (because we print game_start first)
  pmsFlushScoreEvents();

  if (ended){
    const char* reason = "stopped";
    if (g.lastEndKind == SGE_SUCCESS){
      pmsPrintStatus("success", level, score, lives, 0, "state");
      reason = "success";
    } else if (g.lastEndKind == SGE_NO_LIVES || lives == 0){
      reason = "no_lives";
    } else if (g.lastEndKind == SGE_STOPPED || pmsStopRequested){
      reason = "stopped";
    }
    pmsPrintEventGameEnd(reason, score, lives);

    // Consume stop flag once reported
    pmsStopRequested = false;
  }

  // --- Status (silent while idle) ---
  if (active){
    const char* lastReason = pmsLastReasonStr(stateChanged, scoreDelta, livesDelta);
    pmsPrintStatus(pmsStateStr(), level, score, lives, tleftMs, lastReason);
  }

  // Update baseline
  pmsPrevActive = active;
  pmsPrevScore = score;
  pmsPrevLives = lives;
  pmsPrevLevel = level;
  pmsPrevRoundIndex = g.roundIndex;
  pmsPrevRoundKind  = g.roundKind;
}

#endif // PMS_STD_ENABLED

// ========== setup / loop ==========
void setup(){
  Serial.begin(115200); delay(200);
  nvsSaveName(myName);

  radioChannel = nvsLoadChannel(SOCCER_DEFAULT_ESPNOW_CHANNEL);
  wifiStaOnChannel(radioChannel);
  if (!nowInit(radioChannel)) DBG_PRINTLN("ESP-NOW init failed!");
  esp_now_register_recv_cb(onNowRecv);

  DBG_PRINTF("[RADIO] ESPNOW channel=%u  MAC=%s\n", (unsigned)radioChannel, WiFi.macAddress().c_str());

  HelloMsg hm{};
  headerInit(hm.h, MSG_HELLO, "ALL");
  strncpy(hm.name,myName.c_str(),sizeof(hm.name)-1);
  hm.bootCount = 0;
  snprintf(hm.build, sizeof(hm.build), "%s %s", __DATE__, __TIME__);
  esp_now_send(BCAST,(uint8_t*)&hm,sizeof(hm));
  randomSeed(esp_random());

  // Auto-request HELLOs at boot (no manual LIST required; helps with any boot order)
  helloDripActive=true; helloDripRemain=HELLO_DRIP_COUNT;
  helloNextAt=millis(); listPrintAt=0;

  DBG_PRINTLN("Server ready. Type HELP for commands.");
}

void loop(){
  handleSerialServer();

#if PMS_STD_ENABLED
  pmsTick();
#endif


  uint32_t now = millis();

  if (helloDripActive){
    if (helloDripRemain && (int32_t)(now - helloNextAt) >= 0){
      sendHelloReqAll(); helloDripRemain--; helloNextAt=now+HELLO_DRIP_INTERVAL_MS;
      if (helloDripRemain==0){ helloDripActive=false; listPrintAt=helloNextAt+HELLO_SETTLE_MS; }
    }
  }
  if (listPrintAt && (int32_t)(now - listPrintAt) >= 0){
    listPrintAt=0; DBG_PRINTLN("Last seen clients:");
    for (int i=0;i<32;i++) if (seen[i].name.length())
      DBG_PRINTF(" - %s  boot=%lu  build=%s  (%lus ago)\n",
        seen[i].name.c_str(),
        (unsigned long)seen[i].bootCount,
        (seen[i].build[0] ? seen[i].build : "-"),
        (unsigned long)((now - seen[i].ms)/1000));
  }

  if (g.active && g.roundKind == RK_SHRINKING){
    updateShrinkingTargets();
  }

  if (g.active && g.roundKind == RK_BONUS_MOVING){
    updateBonusTargets();
  }

  if (g.active && g.gameEndMs > 0 && (int32_t)(now - g.gameEndMs) >= 0){
    DBG_PRINTF("[GAME] SUCCESS: survived overall 6:00 timer. score=%lu lives=%u\n",
      (unsigned long)g.score,
      (unsigned)g.lives);
    g.lastEndKind = SGE_SUCCESS;
    g.active = false;
    clearTargets();
    pushLedStates();
  }

  if (g.active && g.roundDurationMs > 0){
    if ((uint32_t)(now - g.roundStartMs) >= g.roundDurationMs){
      if (g.roundKind == RK_WARMUP){
        DBG_PRINTF("[L%uR%u] TIMEOUT (Warmup) -> next round, score=%lu lives=%u\n",
          (unsigned)g.levelIndex,
          (unsigned)(g.roundIndex+1),
          (unsigned long)g.score,
          (unsigned)g.lives);
        uint8_t nextRound = g.roundIndex + 1;
        startRound(nextRound);
      } else if (g.roundKind == RK_BONUS_MOVING){
        DBG_PRINTF("[L%uR%u] TIMEOUT (BONUS) -> finish level, score=%lu lives=%u\n",
          (unsigned)g.levelIndex,
          (unsigned)(g.roundIndex+1),
          (unsigned long)g.score,
          (unsigned)g.lives);
        onBonusCleared();
      } else {
        loseLifeAndRestart("TIMEOUT");
      }
    }
  }

  if ((g.active || testMode) && (int32_t)(now - nextLedPushAt) >= 0){
    pushLedStates();
    nextLedPushAt = now + LED_PUSH_MIN_MS;
  }

  delay(2);
}
