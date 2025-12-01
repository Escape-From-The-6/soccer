#include "SoccerCommon.h"
#include <esp_system.h>

static String myName = "Soccer-server";

// ========== Roster ==========
struct Seen { String name; uint32_t ms; };
Seen seen[32];

void remember(const char* n){
  if(!n||!*n) return;
  for(int i=0;i<32;i++){
    if (seen[i].name.equalsIgnoreCase(n)){
      seen[i].ms = millis();
      return;
    }
  }
  for(int i=0;i<32;i++){
    if (!seen[i].name.length()){
      seen[i].name = n;
      seen[i].ms   = millis();
      return;
    }
  }
}

// ========== Names ==========
static const char* NAME_TOPLEFT  = "Soccer-topleft";
static const char* NAME_TOPRIGHT = "Soccer-topright";
static const char* NAME_SIDE     = "Soccer-side";

// ========== Game / Config ==========

enum Arena : uint8_t {
  ARENA_NONE = 0,
  ARENA_TOP  = 1,
  ARENA_SIDE = 2
};

struct Config {
  uint32_t r1DurationMs;       // Round1 duration (all levels)
  uint32_t r2DurationMs;       // Round2 duration (all levels)
  uint32_t r3DurationMs;       // Round3 duration (all levels)
  uint8_t  topBlocks;          // Level1/3: blocks on top
  uint8_t  sideBlocks;         // Level1/3: blocks on side
  uint32_t scoreRearmMs;       // per-sensor score re-arm
  uint32_t roundChangeGraceMs; // ignore hits for this long after round start
};

static Config cfg = {
  15000,  // r1DurationMs = 15s
  30000,  // r2DurationMs = 30s
  30000,  // r3DurationMs = 30s
  3,      // Level1/3: topBlocks = 3
  2,      // Level1/3: sideBlocks = 2
  200,    // scoreRearmMs (ms)
  400     // roundChangeGraceMs (ms)
};

// Level2/4 block counts (constants)
static const uint8_t L2_TOP_BLOCKS  = 5;
static const uint8_t L2_SIDE_BLOCKS = 3;

struct Game {
  bool     active        = false;
  uint8_t  lives         = 0;
  uint32_t score         = 0;

  uint8_t  levelIndex    = 0;   // 1..4
  bool     penaltyMode   = false; // Levels 3 & 4: wrong hits cost a life
  Arena    arena         = ARENA_NONE;

  uint8_t  roundIndex    = 0;   // 1..3 per level
  uint32_t gameStartMs   = 0;
  uint32_t roundStartMs  = 0;
  uint32_t roundDurationMs = 0;

  uint8_t  blockCount    = 0;   // blocks in current arena
  uint8_t  activeBlock   = 0;   // used by Round1 (target block index)

  uint8_t  blockMin[8];         // per-block sensor ID ranges
  uint8_t  blockMax[8];
  bool     blockCleared[8];     // used by R2/R3

  Arena    arenaR2       = ARENA_NONE; // arena chosen for Round2 (per layout)
};

static Game g;

// Per-sensor score re-arm (index by sensor_id; 0 unused)
static uint32_t lastScoreMsBySensor[32] = {0};

// Round-change grace: ignore scoring until this time (ms)
static uint32_t roundChangeIgnoreUntil = 0;

// Quickflash for Round 1 hits (blink block off briefly)
static bool     hitFlashActive    = false;
static uint8_t  hitFlashBlock     = 0;      // which block to flash (0-based)
static uint32_t hitFlashUntilMs   = 0;
static const uint32_t HIT_FLASH_MS = 120;   // how long the block is "off"

// ========== Target bitmasks (server -> clients) ==========
static uint8_t targetTopBits[3]  = {0,0,0}; // sensors 1..23
static uint8_t targetSideBits[2] = {0,0};   // sensors 1..13

static inline void clearTargets(){
  memset(targetTopBits,  0, sizeof(targetTopBits));
  memset(targetSideBits, 0, sizeof(targetSideBits));
}

static inline void setTopBit(uint8_t sensorId){
  if (sensorId<1 || sensorId>23) return;
  uint8_t idx = sensorId-1;
  targetTopBits[idx>>3] |= (1 << (idx&7));
}

static inline void setSideBit(uint8_t sensorId){
  if (sensorId<1 || sensorId>13) return;
  uint8_t idx = sensorId-1;
  targetSideBits[idx>>3] |= (1 << (idx&7));
}

// Split 1..totalSensors into blockCount contiguous blocks and
// return min/max sensor id for blockIdx (0-based).
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

// Build target bits from current Game state
static void rebuildTargetsFromGame(){
  clearTargets();
  if (!g.active) return;
  if (g.arena == ARENA_NONE) return;

  if (g.roundIndex == 1){
    // Single active block for warmup
    if (g.blockCount == 0) return;
    if (g.activeBlock >= g.blockCount) return;
    uint8_t b    = g.activeBlock;
    uint8_t minId = g.blockMin[b];
    uint8_t maxId = g.blockMax[b];
    if (g.arena == ARENA_TOP){
      for (uint8_t id = minId; id <= maxId; ++id) setTopBit(id);
    } else if (g.arena == ARENA_SIDE){
      for (uint8_t id = minId; id <= maxId; ++id) setSideBit(id);
    }
  } else if (g.roundIndex == 2 || g.roundIndex == 3){
    // All not-cleared blocks lit
    for (uint8_t b=0; b<g.blockCount; ++b){
      if (g.blockCleared[b]) continue;
      uint8_t minId = g.blockMin[b];
      uint8_t maxId = g.blockMax[b];
      if (g.arena == ARENA_TOP){
        for (uint8_t id = minId; id <= maxId; ++id) setTopBit(id);
      } else if (g.arena == ARENA_SIDE){
        for (uint8_t id = minId; id <= maxId; ++id) setSideBit(id);
      }
    }
  }
}

// ========== LED push cadence ==========
static uint32_t nextLedPushAt=0;
static const uint32_t LED_PUSH_MIN_MS=40;   // ~25 fps cap

static void pushLedStates(){
  // Local copies so we can apply hitFlash without mutating canonical targets
  uint8_t sendTop[3];  memcpy(sendTop,  targetTopBits,  sizeof(targetTopBits));
  uint8_t sendSide[2]; memcpy(sendSide, targetSideBits, sizeof(targetSideBits));

  uint32_t now = millis();

  // Expire hit flash if needed
  if (hitFlashActive && now >= hitFlashUntilMs){
    hitFlashActive = false;
  }

  // Apply quickflash only in Round 1: briefly turn off the hit block
  if (hitFlashActive && g.roundIndex == 1 && g.blockCount > 0){
    uint8_t b = hitFlashBlock;
    if (b < g.blockCount){
      uint8_t minId = g.blockMin[b];
      uint8_t maxId = g.blockMax[b];
      if (g.arena == ARENA_TOP){
        for (uint8_t id = minId; id <= maxId; ++id){
          if (id < 1 || id > 23) continue;
          uint8_t idx = id - 1;
          sendTop[idx>>3] &= ~(1 << (idx & 7));  // clear bit for this block
        }
      } else if (g.arena == ARENA_SIDE){
        for (uint8_t id = minId; id <= maxId; ++id){
          if (id < 1 || id > 13) continue;
          uint8_t idx = id - 1;
          sendSide[idx>>3] &= ~(1 << (idx & 7)); // clear bit for this block
        }
      }
    }
  }

  { // TOP targets
    LedTopMsg t{};
    t.h.kind = MSG_LED_TOP;
    strncpy(t.h.target, NAME_TOPRIGHT, sizeof(t.h.target)-1);
    memcpy(t.bits, sendTop, sizeof(sendTop));
    esp_now_send(BCAST, (uint8_t*)&t, sizeof(t));
  }
  { // SIDE targets
    LedSideMsg s{};
    s.h.kind = MSG_LED_SIDE;
    strncpy(s.h.target, NAME_SIDE, sizeof(s.h.target)-1);
    memcpy(s.bits, sendSide, sizeof(sendSide));
    esp_now_send(BCAST, (uint8_t*)&s, sizeof(s));
  }
}

// ========== HELLO drip ==========
static bool helloDripActive=false; static uint8_t helloDripRemain=0;
static uint32_t helloNextAt=0, listPrintAt=0;
static const uint8_t  HELLO_DRIP_COUNT=5;
static const uint32_t HELLO_DRIP_INTERVAL_MS=200, HELLO_SETTLE_MS=350;

static void sendHelloReqAll(){
  MsgHeader h{}; h.kind=MSG_HELLO_REQ; strncpy(h.target,"ALL",sizeof(h.target)-1);
  esp_now_send(BCAST,(uint8_t*)&h,sizeof(h));
}

// ========== Sends (WIFI/OTA/NAME/LIFE_FLASH/MODE) ==========
static void sendWifiSet(const char* target,const String& ssid,const String& pass){
  WifiSetMsg m{}; m.h.kind=MSG_WIFI_SET;
  strncpy(m.h.target,target,sizeof(m.h.target)-1);
  strncpy(m.ssid,ssid.c_str(),sizeof(m.ssid)-1);
  strncpy(m.pass,pass.c_str(),sizeof(m.pass)-1);
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendOta(const char* target,const String& url){
  OtaMsg m{}; m.h.kind=MSG_OTA_TRIGGER;
  const char* tgt=(target&&*target)?target:"ALL";
  strncpy(m.h.target,tgt,sizeof(m.h.target)-1);
  strncpy(m.url,url.c_str(),sizeof(m.url)-1);
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendNameSet(const char* target,const String& newName){
  NameSetMsg m{}; m.h.kind=MSG_NAME_SET;
  strncpy(m.h.target,target,sizeof(m.h.target)-1);
  strncpy(m.name,newName.c_str(),sizeof(m.name)-1);
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendLifeFlash(uint16_t durationMs){
  LifeFlashMsg m{};
  m.h.kind = MSG_LIFE_FLASH;
  strncpy(m.h.target,"ALL",sizeof(m.h.target)-1);
  m.durationMs = durationMs;
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

static void sendMode(bool penalty){
  ModeMsg m{};
  m.h.kind = MSG_MODE;
  strncpy(m.h.target,"ALL",sizeof(m.h.target)-1);
  m.penaltyMode = penalty ? 1 : 0;
  esp_now_send(BCAST,(uint8_t*)&m,sizeof(m));
}

// ========== Game control helpers ==========

static void startLevel1Round1();
static void startLevel1Round2();
static void startLevel1Round3();
static void startLevel2Round1();
static void startLevel2Round2();
static void startLevel2Round3();

static void stopGame(const char* reason){
  if (!g.active) return;
  g.active = false;
  clearTargets();
  hitFlashActive = false;
  pushLedStates(); // clear LEDs on clients
  Serial.printf("[GAME] stopped (%s). score=%lu lives=%u\n",
    reason ? reason : "manual",
    (unsigned long)g.score,
    (unsigned)g.lives);
}

// Lose a life and restart the current round (for penaltyMode or R2/R3 timeouts)
static void loseLifeAndRestart(const char* reason){
  if (!g.active) return;
  if (g.lives > 0) g.lives--;
  Serial.printf("[L%uR%u] %s -> life lost, lives=%u (score=%lu)\n",
    (unsigned)g.levelIndex,
    (unsigned)g.roundIndex,
    reason ? reason : "life-loss",
    (unsigned)g.lives,
    (unsigned long)g.score);

  sendLifeFlash(300);

  if (g.lives == 0){
    Serial.printf("[L%uR%u] out of lives, ending game.\n",
      (unsigned)g.levelIndex,
      (unsigned)g.roundIndex);
    stopGame("no-lives");
    return;
  }

  // Regenerate current round (same layout, same levelIndex & penaltyMode)
  if (g.roundIndex == 1){
    if (g.levelIndex == 1 || g.levelIndex == 3) startLevel1Round1();
    else                                         startLevel2Round1();
  } else if (g.roundIndex == 2){
    if (g.levelIndex == 1 || g.levelIndex == 3) startLevel1Round2();
    else                                         startLevel2Round2();
  } else if (g.roundIndex == 3){
    if (g.levelIndex == 1 || g.levelIndex == 3) startLevel1Round3();
    else                                         startLevel2Round3();
  }
}

// ----- Layout 1 rounds (Levels 1 & 3) -----

static void startLevel1Round1(){
  clearTargets();
  hitFlashActive = false;
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));
  memset(g.blockCleared, 0, sizeof(g.blockCleared));

  const uint8_t TOTAL_TOP_SENSORS  = 23;
  const uint8_t TOTAL_SIDE_SENSORS = 13;

  uint32_t now = millis();
  g.roundIndex      = 1;
  g.roundDurationMs = cfg.r1DurationMs;
  g.roundStartMs    = now;

  // Start-of-round grace
  roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;

  // Choose arena: TOP or SIDE (both allowed)
  bool allowTop  = (cfg.topBlocks  > 0);
  bool allowSide = (cfg.sideBlocks > 0);
  bool useTop    = true;

  if (allowTop && allowSide){
    useTop = (random(0,2) == 0);  // 0 or 1
  } else if (allowTop){
    useTop = true;
  } else if (allowSide){
    useTop = false;
  } else {
    useTop = true;
  }

  g.arena = useTop ? ARENA_TOP : ARENA_SIDE;
  g.blockCount = useTop ? cfg.topBlocks : cfg.sideBlocks;
  if (g.blockCount == 0) g.blockCount = 1;

  // Compute block ranges for all blocks
  if (g.arena == ARENA_TOP){
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_TOP_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  } else {
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_SIDE_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  }

  // Pick active block for warmup
  g.activeBlock = (uint8_t)random(0, g.blockCount);

  rebuildTargetsFromGame();
  pushLedStates();
  nextLedPushAt = now + LED_PUSH_MIN_MS;

  const char* arenaName = (g.arena == ARENA_TOP ? "TOP" : (g.arena == ARENA_SIDE ? "SIDE" : "NONE"));
  uint8_t b = g.activeBlock;
  Serial.printf("[L%uR1] start arena=%s block %u/%u (ids %u..%u) dur=%lus\n",
    (unsigned)g.levelIndex,
    arenaName,
    (unsigned)(b+1),
    (unsigned)g.blockCount,
    (unsigned)g.blockMin[b],
    (unsigned)g.blockMax[b],
    (unsigned long)(g.roundDurationMs/1000));
}

static void startLevel1Round2(){
  clearTargets();
  hitFlashActive = false;
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));
  memset(g.blockCleared, 0, sizeof(g.blockCleared));

  const uint8_t TOTAL_TOP_SENSORS  = 23;
  const uint8_t TOTAL_SIDE_SENSORS = 13;

  uint32_t now = millis();
  g.roundIndex      = 2;
  g.roundDurationMs = cfg.r2DurationMs;
  g.roundStartMs    = now;

  // Start-of-round grace
  roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;

  // Choose Arena for Round2
  bool allowTop  = (cfg.topBlocks  > 0);
  bool allowSide = (cfg.sideBlocks > 0);
  bool useTop    = true;

  if (allowTop && allowSide){
    useTop = (random(0,2) == 0);
  } else if (allowTop){
    useTop = true;
  } else if (allowSide){
    useTop = false;
  } else {
    useTop = true;
  }

  g.arenaR2 = useTop ? ARENA_TOP : ARENA_SIDE;
  g.arena   = g.arenaR2;

  g.blockCount = (g.arena == ARENA_TOP) ? cfg.topBlocks : cfg.sideBlocks;
  if (g.blockCount == 0) g.blockCount = 1;

  if (g.arena == ARENA_TOP){
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_TOP_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  } else {
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_SIDE_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  }

  rebuildTargetsFromGame();
  pushLedStates();
  nextLedPushAt = now + LED_PUSH_MIN_MS;

  const char* arenaName = (g.arena == ARENA_TOP ? "TOP" : "SIDE");
  Serial.printf("[L%uR2] start arena=%s blocks=%u dur=%lus\n",
    (unsigned)g.levelIndex,
    arenaName,
    (unsigned)g.blockCount,
    (unsigned long)(g.roundDurationMs/1000));
}

static void startLevel1Round3(){
  clearTargets();
  hitFlashActive = false;
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));
  memset(g.blockCleared, 0, sizeof(g.blockCleared));

  const uint8_t TOTAL_TOP_SENSORS  = 23;
  const uint8_t TOTAL_SIDE_SENSORS = 13;

  uint32_t now = millis();
  g.roundIndex      = 3;
  g.roundDurationMs = cfg.r3DurationMs;
  g.roundStartMs    = now;

  // Start-of-round grace
  roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;

  // Arena = opposite of Round2 arena
  if (g.arenaR2 == ARENA_NONE){
    g.arena = (g.arena == ARENA_TOP ? ARENA_SIDE : ARENA_TOP);
  } else {
    g.arena = (g.arenaR2 == ARENA_TOP ? ARENA_SIDE : ARENA_TOP);
  }

  g.blockCount = (g.arena == ARENA_TOP) ? cfg.topBlocks : cfg.sideBlocks;
  if (g.blockCount == 0) g.blockCount = 1;

  if (g.arena == ARENA_TOP){
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_TOP_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  } else {
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_SIDE_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  }

  rebuildTargetsFromGame();
  pushLedStates();
  nextLedPushAt = now + LED_PUSH_MIN_MS;

  const char* arenaName = (g.arena == ARENA_TOP ? "TOP" : "SIDE");
  Serial.printf("[L%uR3] start arena=%s blocks=%u dur=%lus\n",
    (unsigned)g.levelIndex,
    arenaName,
    (unsigned)g.blockCount,
    (unsigned long)(g.roundDurationMs/1000));
}

// ----- Layout 2 rounds (Levels 2 & 4) -----

static void startLevel2Round1(){
  clearTargets();
  hitFlashActive = false;
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));
  memset(g.blockCleared, 0, sizeof(g.blockCleared));

  const uint8_t TOTAL_TOP_SENSORS  = 23;
  const uint8_t TOTAL_SIDE_SENSORS = 13;

  uint32_t now = millis();
  g.roundIndex      = 1;
  g.roundDurationMs = cfg.r1DurationMs;
  g.roundStartMs    = now;

  // Start-of-round grace
  roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;

  // Choose arena: TOP or SIDE (both allowed)
  bool allowTop  = (L2_TOP_BLOCKS  > 0);
  bool allowSide = (L2_SIDE_BLOCKS > 0);
  bool useTop    = true;

  if (allowTop && allowSide){
    useTop = (random(0,2) == 0);  // 0 or 1
  } else if (allowTop){
    useTop = true;
  } else if (allowSide){
    useTop = false;
  } else {
    useTop = true;
  }

  g.arena = useTop ? ARENA_TOP : ARENA_SIDE;
  g.blockCount = useTop ? L2_TOP_BLOCKS : L2_SIDE_BLOCKS;
  if (g.blockCount == 0) g.blockCount = 1;

  // Compute block ranges for all blocks
  if (g.arena == ARENA_TOP){
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_TOP_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  } else {
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_SIDE_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  }

  // Pick active block for warmup
  g.activeBlock = (uint8_t)random(0, g.blockCount);

  rebuildTargetsFromGame();
  pushLedStates();
  nextLedPushAt = now + LED_PUSH_MIN_MS;

  const char* arenaName = (g.arena == ARENA_TOP ? "TOP" : (g.arena == ARENA_SIDE ? "SIDE" : "NONE"));
  uint8_t b = g.activeBlock;
  Serial.printf("[L%uR1] start arena=%s block %u/%u (ids %u..%u) dur=%lus\n",
    (unsigned)g.levelIndex,
    arenaName,
    (unsigned)(b+1),
    (unsigned)g.blockCount,
    (unsigned)g.blockMin[b],
    (unsigned)g.blockMax[b],
    (unsigned long)(g.roundDurationMs/1000));
}

static void startLevel2Round2(){
  clearTargets();
  hitFlashActive = false;
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));
  memset(g.blockCleared, 0, sizeof(g.blockCleared));

  const uint8_t TOTAL_TOP_SENSORS  = 23;
  const uint8_t TOTAL_SIDE_SENSORS = 13;

  uint32_t now = millis();
  g.roundIndex      = 2;
  g.roundDurationMs = cfg.r2DurationMs;
  g.roundStartMs    = now;

  // Start-of-round grace
  roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;

  // Choose Arena for Round2
  bool allowTop  = (L2_TOP_BLOCKS  > 0);
  bool allowSide = (L2_SIDE_BLOCKS > 0);
  bool useTop    = true;

  if (allowTop && allowSide){
    useTop = (random(0,2) == 0);
  } else if (allowTop){
    useTop = true;
  } else if (allowSide){
    useTop = false;
  } else {
    useTop = true;
  }

  g.arenaR2 = useTop ? ARENA_TOP : ARENA_SIDE;
  g.arena   = g.arenaR2;

  g.blockCount = (g.arena == ARENA_TOP) ? L2_TOP_BLOCKS : L2_SIDE_BLOCKS;
  if (g.blockCount == 0) g.blockCount = 1;

  if (g.arena == ARENA_TOP){
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_TOP_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  } else {
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_SIDE_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  }

  rebuildTargetsFromGame();
  pushLedStates();
  nextLedPushAt = now + LED_PUSH_MIN_MS;

  const char* arenaName = (g.arena == ARENA_TOP ? "TOP" : "SIDE");
  Serial.printf("[L%uR2] start arena=%s blocks=%u dur=%lus\n",
    (unsigned)g.levelIndex,
    arenaName,
    (unsigned)g.blockCount,
    (unsigned long)(g.roundDurationMs/1000));
}

static void startLevel2Round3(){
  clearTargets();
  hitFlashActive = false;
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));
  memset(g.blockCleared, 0, sizeof(g.blockCleared));

  const uint8_t TOTAL_TOP_SENSORS  = 23;
  const uint8_t TOTAL_SIDE_SENSORS = 13;

  uint32_t now = millis();
  g.roundIndex      = 3;
  g.roundDurationMs = cfg.r3DurationMs;
  g.roundStartMs    = now;

  // Start-of-round grace
  roundChangeIgnoreUntil = now + cfg.roundChangeGraceMs;

  // Arena = opposite of Round2 arena
  if (g.arenaR2 == ARENA_NONE){
    g.arena = (g.arena == ARENA_TOP ? ARENA_SIDE : ARENA_TOP);
  } else {
    g.arena = (g.arenaR2 == ARENA_TOP ? ARENA_SIDE : ARENA_TOP);
  }

  g.blockCount = (g.arena == ARENA_TOP) ? L2_TOP_BLOCKS : L2_SIDE_BLOCKS;
  if (g.blockCount == 0) g.blockCount = 1;

  if (g.arena == ARENA_TOP){
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_TOP_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  } else {
    for (uint8_t b=0; b<g.blockCount; ++b){
      computeBlockRange(TOTAL_SIDE_SENSORS, g.blockCount, b, g.blockMin[b], g.blockMax[b]);
      g.blockCleared[b] = false;
    }
  }

  rebuildTargetsFromGame();
  pushLedStates();
  nextLedPushAt = now + LED_PUSH_MIN_MS;

  const char* arenaName = (g.arena == ARENA_TOP ? "TOP" : "SIDE");
  Serial.printf("[L%uR3] start arena=%s blocks=%u dur=%lus\n",
    (unsigned)g.levelIndex,
    arenaName,
    (unsigned)g.blockCount,
    (unsigned long)(g.roundDurationMs/1000));
}

// ----- Game start wrappers -----

static void startGame(){  // Level 1 default
  memset(&g, 0, sizeof(g));
  hitFlashActive = false;
  g.active      = true;
  g.lives       = 5;      // lives per game
  g.score       = 0;
  g.levelIndex  = 1;
  g.penaltyMode = false;  // neutral hits are safe
  g.gameStartMs = millis();
  sendMode(false);
  startLevel1Round1();
}

static void startGameLevel2(){
  memset(&g, 0, sizeof(g));
  hitFlashActive = false;
  g.active      = true;
  g.lives       = 5;
  g.score       = 0;
  g.levelIndex  = 2;
  g.penaltyMode = false;  // neutral hits are safe
  g.gameStartMs = millis();
  sendMode(false);
  startLevel2Round1();
}

static void startGameLevel3(){ // same layout as Level1, but penalties ON
  memset(&g, 0, sizeof(g));
  hitFlashActive = false;
  g.active      = true;
  g.lives       = 5;
  g.score       = 0;
  g.levelIndex  = 3;
  g.penaltyMode = true;   // wrong hits cost a life
  g.gameStartMs = millis();
  sendMode(true);
  startLevel1Round1();
}

static void startGameLevel4(){ // same layout as Level2, but penalties ON
  memset(&g, 0, sizeof(g));
  hitFlashActive = false;
  g.active      = true;
  g.lives       = 5;
  g.score       = 0;
  g.levelIndex  = 4;
  g.penaltyMode = true;   // wrong hits cost a life
  g.gameStartMs = millis();
  sendMode(true);
  startLevel2Round1();
}

// ========== Sensor handling ==========
static void handleSensorEvent(const SensorEventMsg* m){
  remember(m->name);
  if (m->state != 1) return; // only TRIGGER events
  if (!g.active) return;

  bool isTopL  = !strcasecmp(m->name, NAME_TOPLEFT);
  bool isTopR  = !strcasecmp(m->name, NAME_TOPRIGHT);
  bool isSide  = !strcasecmp(m->name, NAME_SIDE);

  Arena eventArena = ARENA_NONE;
  uint8_t id = m->sensor_id;

  if ((isTopL || isTopR) && id >= 1 && id <= 23){
    eventArena = ARENA_TOP;
  } else if (isSide && id >= 1 && id <= 13){
    eventArena = ARENA_SIDE;
  } else {
    return;
  }

  // Only handle events from the current arena for the round
  if (eventArena != g.arena) return;

  uint32_t now = millis();

  // Round-change grace: ignore early hits immediately after a round starts
  if (now < roundChangeIgnoreUntil) return;

  if (id < (sizeof(lastScoreMsBySensor)/sizeof(lastScoreMsBySensor[0]))){
    uint32_t last = lastScoreMsBySensor[id];
    if ((uint32_t)(now - last) < cfg.scoreRearmMs) return; // per-sensor score cooldown
    lastScoreMsBySensor[id] = now;
  }

  if (g.roundIndex == 1){
    // ===== Round 1: warmup, single block =====
    if (g.blockCount == 0) return;
    uint8_t b = g.activeBlock;
    if (b >= g.blockCount) return;

    if (id < g.blockMin[b] || id > g.blockMax[b]){
      // Missed the target block
      if (g.penaltyMode){
        loseLifeAndRestart("WRONG HIT R1");
      }
      return;
    }

    g.score++;

    // Quickflash for this block
    hitFlashActive  = true;
    hitFlashBlock   = b;
    hitFlashUntilMs = now + HIT_FLASH_MS;
    nextLedPushAt   = now;  // push immediately

    Serial.printf("[L%uR1] HIT arena=%s sensor=%u score=%lu\n",
      (unsigned)g.levelIndex,
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)id,
      (unsigned long)g.score);
    return;
  }

  // ===== Rounds 2 & 3: hit each block once =====
  if (g.roundIndex == 2 || g.roundIndex == 3){
    if (g.blockCount == 0) return;
    uint8_t bFound = 255;
    for (uint8_t b=0; b<g.blockCount; ++b){
      if (id >= g.blockMin[b] && id <= g.blockMax[b]){
        bFound = b;
        break;
      }
    }

    bool badHit = (bFound == 255) || (bFound < g.blockCount && g.blockCleared[bFound]);

    if (badHit){
      if (g.penaltyMode){
        loseLifeAndRestart("WRONG HIT R2/R3");
      }
      return;
    }

    // Valid hit on an uncleared block
    g.blockCleared[bFound] = true;
    g.score++;
    Serial.printf("[L%uR%u] HIT arena=%s block=%u/%u sensor=%u score=%lu\n",
      (unsigned)g.levelIndex,
      (unsigned)g.roundIndex,
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)(bFound+1),
      (unsigned)g.blockCount,
      (unsigned)id,
      (unsigned long)g.score);

    // Rebuild targets to hide cleared block
    rebuildTargetsFromGame();
    nextLedPushAt = now;

    // Check if all blocks cleared
    bool allCleared = true;
    for (uint8_t b=0; b<g.blockCount; ++b){
      if (!g.blockCleared[b]){
        allCleared = false;
        break;
      }
    }

    if (allCleared){
      if (g.roundIndex == 2){
        // Advance to Round 3 (same layout, same level, same penalty mode)
        if (g.levelIndex == 1 || g.levelIndex == 3){
          Serial.printf("[L%uR2] all blocks cleared, advancing to Round 3\n", (unsigned)g.levelIndex);
          startLevel1Round3();
        } else {
          Serial.printf("[L%uR2] all blocks cleared, advancing to Round 3\n", (unsigned)g.levelIndex);
          startLevel2Round3();
        }
      } else if (g.roundIndex == 3){
        // Level completion behaviour
        if (g.levelIndex == 1){
          Serial.printf("[L1R3] all blocks cleared, Level 1 complete. Advancing to Level 2 (score=%lu, lives=%u)\n",
            (unsigned long)g.score, (unsigned)g.lives);
          g.levelIndex  = 2;
          g.penaltyMode = false;
          sendMode(false);
          startLevel2Round1();
        } else if (g.levelIndex == 2){
          Serial.printf("[L2R3] all blocks cleared, Level 2 complete. Advancing to Level 3 (score=%lu, lives=%u)\n",
            (unsigned long)g.score, (unsigned)g.lives);
          g.levelIndex  = 3;
          g.penaltyMode = true;
          sendMode(true);
          startLevel1Round1();
        } else if (g.levelIndex == 3){
          Serial.printf("[L3R3] all blocks cleared, Level 3 complete. Advancing to Level 4 (score=%lu, lives=%u)\n",
            (unsigned long)g.score, (unsigned)g.lives);
          g.levelIndex  = 4;
          g.penaltyMode = true;
          sendMode(true);
          startLevel2Round1();
        } else if (g.levelIndex == 4){
          Serial.printf("[L4R3] all blocks cleared, Level 4 complete. final score=%lu lives=%u\n",
            (unsigned long)g.score, (unsigned)g.lives);
          stopGame("level4-complete");
        }
      }
    }
    return;
  }
}

// ========== RX ==========
static void onNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len){
  if (len < (int)sizeof(MsgHeader)) return;
  const MsgHeader* h=(const MsgHeader*)data;

  if (h->kind == MSG_HELLO && len >= (int)sizeof(HelloMsg)){
    const HelloMsg* m=(const HelloMsg*)data;
    remember(m->name);
    uint32_t now = millis();
    // When a display client reappears, repush current targets
    if (!strcasecmp(m->name, NAME_TOPRIGHT) || !strcasecmp(m->name, NAME_TOPLEFT) || !strcasecmp(m->name, NAME_SIDE)) {
      if ((int32_t)(now - nextLedPushAt) > 0) nextLedPushAt = now;
    }
    return;
  }

  if (h->kind == MSG_SENSOR_EVENT && len >= (int)sizeof(SensorEventMsg)){
    const SensorEventMsg* m=(const SensorEventMsg*)data;
    handleSensorEvent(m);
    // DEBUG
    Serial.printf("[EVT] %s s%u gpio%u state=%u seq=%u\n",
      m->name, m->sensor_id, m->gpio, m->state, m->seq);
    return;
  }
}

// ========== CLI ==========
static void printCfg(){
  Serial.println("Config:");
  Serial.printf("  R1DUR = %lu ms\n", (unsigned long)cfg.r1DurationMs);
  Serial.printf("  R2DUR = %lu ms\n", (unsigned long)cfg.r2DurationMs);
  Serial.printf("  R3DUR = %lu ms\n", (unsigned long)cfg.r3DurationMs);
  Serial.printf("  L1TOP = %u blocks\n", cfg.topBlocks);
  Serial.printf("  L1SIDE= %u blocks\n", cfg.sideBlocks);
  Serial.printf("  L2TOP = %u blocks (fixed)\n", (unsigned)L2_TOP_BLOCKS);
  Serial.printf("  L2SIDE= %u blocks (fixed)\n", (unsigned)L2_SIDE_BLOCKS);
  Serial.printf("  REARM = %lu ms\n", (unsigned long)cfg.scoreRearmMs);
  Serial.printf("  GRACE = %lu ms\n", (unsigned long)cfg.roundChangeGraceMs);
  Serial.println();
}

static void printHelp(){
  Serial.println();
  Serial.println("=== Soccer Server CLI ===");
  Serial.println("WIFI ALL <ssid> <pass>");
  Serial.println("WIFI <name> <ssid> <pass>");
  Serial.println("OTA ALL <url>");
  Serial.println("OTA <name> <url>");
  Serial.println("NAME <ALL|currentName> <newName>");
  Serial.println("LIST");
  Serial.println("GAME START   (Level 1)");
  Serial.println("GAME L1      (Level 1)");
  Serial.println("GAME L2      (Level 2)");
  Serial.println("GAME L3      (Level 3 - penalty mode, layout 1)");
  Serial.println("GAME L4      (Level 4 - penalty mode, layout 2)");
  Serial.println("GAME STOP");
  Serial.println("CFG");
  Serial.println("SET <R1DUR|R2DUR|R3DUR|L1TOP|L1SIDE|REARM|GRACE> <value>");
  Serial.println();
  Serial.println("Examples:");
  Serial.println("  WIFI ALL AndrewiPhone 12345678");
  Serial.println("  OTA ALL http://172.20.10.2:8000/Soccer/SoccerClient/build/esp32.esp32.esp32/SoccerClient.ino.bin");
  Serial.println("  GAME START");
  Serial.println("  GAME L3");
  Serial.println("===========================");
}

static void handleSerialServer(){
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;

  // collapse whitespace
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
    Serial.println("Requesting HELLOs (drip)...");
    return;
  }
  if (cmd=="WIFI"){
    if(!arg1.length()){Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    int sp=rest.indexOf(' ');
    if(sp<0){Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    String ssid=rest.substring(0,sp), pass=rest.substring(sp+1); ssid.trim(); pass.trim();
    if(!ssid.length()||!pass.length()){Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    sendWifiSet(arg1.c_str(),ssid,pass);
    Serial.printf("Sent WIFI_SET to %s (ssid=%s)\n",arg1.c_str(),ssid.c_str());
    return;
  }
  if (cmd=="OTA"){
    if(!arg1.length()||!rest.length()){Serial.println("Usage: OTA <ALL|name> <url>"); return;}
    sendOta(arg1.c_str(),rest);
    Serial.printf("Sent OTA_TRIGGER to %s  url=%s\n",arg1.c_str(),rest.c_str());
    return;
  }
  if (cmd=="NAME"){
    if(!arg1.length()||!rest.length()){Serial.println("Usage: NAME <ALL|currentName> <newName>");return;}
    sendNameSet(arg1.c_str(),rest);
    Serial.printf("Sent NAME_SET to %s -> %s\n",arg1.c_str(),rest.c_str());
    return;
  }
  if (cmd=="GAME"){
    if (!arg1.length()){
      Serial.println("Usage: GAME START|L1|L2|L3|L4|STOP");
      return;
    }
    if (arg1.equalsIgnoreCase("START") || arg1.equalsIgnoreCase("L1")){
      if (g.active){
        Serial.println("Game already active. Use GAME STOP first.");
      } else {
        startGame(); // Level 1
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L2")){
      if (g.active){
        Serial.println("Game already active. Use GAME STOP first.");
      } else {
        startGameLevel2();
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L3")){
      if (g.active){
        Serial.println("Game already active. Use GAME STOP first.");
      } else {
        startGameLevel3();
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L4")){
      if (g.active){
        Serial.println("Game already active. Use GAME STOP first.");
      } else {
        startGameLevel4();
      }
      return;
    }
    if (arg1.equalsIgnoreCase("STOP")){
      if (!g.active) Serial.println("No active game.");
      else stopGame("CLI");
      return;
    }
    Serial.println("Usage: GAME START|L1|L2|L3|L4|STOP");
    return;
  }
  if (cmd=="CFG"){
    printCfg();
    return;
  }
  if (cmd=="SET"){
    if (!arg1.length() || !rest.length()){
      Serial.println("Usage: SET <R1DUR|R2DUR|R3DUR|L1TOP|L1SIDE|REARM|GRACE> <value>");
      return;
    }
    long v = rest.toInt();
    if (arg1.equalsIgnoreCase("R1DUR")){
      if (v <= 0){ Serial.println("R1DUR must be >0"); return; }
      cfg.r1DurationMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("R2DUR")){
      if (v <= 0){ Serial.println("R2DUR must be >0"); return; }
      cfg.r2DurationMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("R3DUR")){
      if (v <= 0){ Serial.println("R3DUR must be >0"); return; }
      cfg.r3DurationMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("L1TOP")){
      if (v < 1 || v > 8){ Serial.println("L1TOP must be 1..8"); return; }
      cfg.topBlocks = (uint8_t)v;
    } else if (arg1.equalsIgnoreCase("L1SIDE")){
      if (v < 1 || v > 8){ Serial.println("L1SIDE must be 1..8"); return; }
      cfg.sideBlocks = (uint8_t)v;
    } else if (arg1.equalsIgnoreCase("REARM")){
      if (v < 0){ Serial.println("REARM must be >=0"); return; }
      cfg.scoreRearmMs = (uint32_t)v;
    } else if (arg1.equalsIgnoreCase("GRACE")){
      if (v < 0){ Serial.println("GRACE must be >=0"); return; }
      cfg.roundChangeGraceMs = (uint32_t)v;
    } else {
      Serial.println("Unknown key. Use R1DUR, R2DUR, R3DUR, L1TOP, L1SIDE, REARM, GRACE");
      return;
    }
    Serial.printf("SET %s = %ld\n", arg1.c_str(), v);
    return;
  }

  Serial.println("Unknown command. Type HELP.");
}

// ========== setup / loop ==========

void setup(){
  Serial.begin(115200); delay(200);
  nvsSaveName(myName);

  wifiStaOnCh6();
  if (!nowInit()) Serial.println("ESP-NOW init failed!");
  esp_now_register_recv_cb(onNowRecv);

  HelloMsg hm{}; hm.h.kind=MSG_HELLO; strncpy(hm.h.target,"ALL",sizeof(hm.h.target)-1);
  strncpy(hm.name,myName.c_str(),sizeof(hm.name)-1); esp_now_send(BCAST,(uint8_t*)&hm,sizeof(hm));

  randomSeed(esp_random());

  Serial.println("Server ready. Type HELP for commands.");
}

void loop(){
  handleSerialServer();

  uint32_t now = millis();

  // HELLO drip
  if (helloDripActive){
    if (helloDripRemain && (int32_t)(now - helloNextAt) >= 0){
      sendHelloReqAll(); helloDripRemain--; helloNextAt=now+HELLO_DRIP_INTERVAL_MS;
      if (helloDripRemain==0){ helloDripActive=false; listPrintAt=helloNextAt+HELLO_SETTLE_MS; }
    }
  }
  if (listPrintAt && (int32_t)(now - listPrintAt) >= 0){
    listPrintAt=0; Serial.println("Last seen clients:");
    for (int i=0;i<32;i++) if (seen[i].name.length())
      Serial.printf(" - %s  (%lus ago)\n", seen[i].name.c_str(), (unsigned long)((now - seen[i].ms)/1000));
  }

  // Round timers:
  // Round 1: timeout => advance to Round 2 (all levels, no life loss).
  // Rounds 2 & 3: timeout => lose a life, red flash, retry same round (all levels).
  if (g.active && g.roundDurationMs > 0){
    if ((uint32_t)(now - g.roundStartMs) >= g.roundDurationMs){
      if (g.roundIndex == 1){
        Serial.printf("[L%uR1] TIMEOUT -> advancing to Round 2 (no life lost) score=%lu lives=%u\n",
          (unsigned)g.levelIndex,
          (unsigned long)g.score,
          (unsigned)g.lives);

        if (g.levelIndex == 1 || g.levelIndex == 3) startLevel1Round2();
        else                                         startLevel2Round2();
      } else {
        loseLifeAndRestart("TIMEOUT");
      }
    }
  }

  // Push LED targets periodically
  if ((int32_t)(now - nextLedPushAt) >= 0){
    pushLedStates();
    nextLedPushAt = now + LED_PUSH_MIN_MS;
  }

  delay(2);
}
