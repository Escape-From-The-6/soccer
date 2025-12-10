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
  uint32_t roundChangeGraceMs; // ignore hits for this long after round start
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
  uint32_t    roundStartMs  = 0;
  uint32_t    roundDurationMs = 0;

  // block layout (for warmup / hit-all)
  uint8_t     blockCount    = 0;
  uint8_t     blockMin[8];
  uint8_t     blockMax[8];
  bool        blockCleared[8];

  // warmup: which block is active
  uint8_t     activeBlock   = 0;
};

static Game g;

// Shrinking round (Round 4) state
struct ShrinkState {
  bool     active;
  uint8_t  center;         // current center sensor id
  uint8_t  stage;          // 0=full, 1=mid, 2=narrow
  uint8_t  widths[3];      // {7,5,3} or {5,3,1} depending on level
  uint8_t  minId;          // current min id (for scoring)
  uint8_t  maxId;          // current max id (for scoring)
};

static ShrinkState shrink = {false,0,0,{0,0,0},0,0};
static uint16_t shrinkHits = 0;

// Bonus sliding round state (Round 5)
struct BonusState {
  bool     active;
  uint8_t  center;         // current center
  int8_t   dir;            // +1 or -1 movement
  uint8_t  width;          // window width (e.g. 3)
  uint8_t  minId;
  uint8_t  maxId;
  uint32_t lastStepMs;     // last time we moved the window
};

static BonusState bonus = {false,0,1,3,0,0,0};
static const uint32_t BONUS_DURATION_MS = 15000;   // 15s (for now)
static const uint32_t BONUS_STEP_MS     = 80;      // how fast the window slides

// Per-sensor score re-arm (index by sensor_id; 0 unused)
static uint32_t lastScoreMsBySensor[32] = {0};

// Round-change grace: ignore scoring until this time (ms)
static uint32_t roundChangeIgnoreUntil = 0;

// Quickflash for WARMUP hits (blink block off briefly)
static bool     hitFlashActive    = false;
static uint8_t  hitFlashBlock     = 0;      // which block to flash (0-based)
static uint32_t hitFlashUntilMs   = 0;
static const uint32_t HIT_FLASH_MS = 120;   // how long the block is "off"

// Sensor counts
static const uint8_t TOP_SENSOR_COUNT  = 23;
static const uint8_t SIDE_SENSOR_COUNT = 13;

// ========== Target bitmasks (server -> clients) ==========
static uint8_t targetTopBits[3]  = {0,0,0}; // sensors 1..23
static uint8_t targetSideBits[2] = {0,0};   // sensors 1..13

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

// Rebuild target bits from current Game state for static rounds (WARMUP/HIT_ALL_*).
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

// Helper: initialize blocks for current round / arena
// NOTE: callers must set g.arena before calling this.
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

  // Apply quickflash only in WARMUP: briefly turn off the hit block
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

// ========== Shrinking round (Round 4) ==========
static void shrinkingInitWidths(){
  // Levels 1 & 3 -> widths 7,5,3
  // Levels 2 & 4 -> widths 5,3,1
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

// ========== Bonus sliding target update (Round 5) ==========
static void updateBonusTargets(){
  if (!bonus.active || !g.active) return;
  if (g.roundKind != RK_BONUS_MOVING) return;
  if (g.arena == ARENA_NONE) return;

  uint32_t now = millis();
  if ((uint32_t)(now - bonus.lastStepMs) < BONUS_STEP_MS) return;
  bonus.lastStepMs = now;

  uint8_t maxSensors = (g.arena == ARENA_TOP) ? TOP_SENSOR_COUNT : SIDE_SENSOR_COUNT;
  int halfSpan = bonus.width / 2;

  // Move center
  int newCenter = (int)bonus.center + (int)bonus.dir;

  // Bounce at edges (keeping window in range)
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

  // Force LED push ASAP
  nextLedPushAt = now;
}

// ========== HELLO drip ==========
static bool helloDripActive=false; static uint8_t helloDripRemain=0;
static uint32_t helloNextAt=0, listPrintAt=0;
static const uint8_t  HELLO_DRIP_COUNT=5;
static const uint32_t HELLO_DRIP_INTERVAL_MS=200, HELLO_SETTLE_MS=350;

// ========== Sends (WIFI/OTA/NAME/LIFE_FLASH/MODE/ROUND_INFO) ==========
static void sendHelloReqAll(){
  MsgHeader h{}; h.kind=MSG_HELLO_REQ; strncpy(h.target,"ALL",sizeof(h.target)-1);
  esp_now_send(BCAST,(uint8_t*)&h,sizeof(h));
}

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

static void sendRoundInfo(bool bonusRound){
  RoundInfoMsg m{};
  m.h.kind = MSG_ROUND_INFO;
  strncpy(m.h.target,"ALL",sizeof(m.h.target)-1);
  m.bonusRound = bonusRound ? 1 : 0;
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

// Lose a life and restart the current round (for timeouts)
static void loseLifeAndRestart(const char* reason){
  if (!g.active) return;
  if (g.lives > 0) g.lives--;
  Serial.printf("[L%uR%u] %s -> life lost, lives=%u (score=%lu)\n",
    (unsigned)g.levelIndex,
    (unsigned)(g.roundIndex+1),
    reason ? reason : "life-loss",
    (unsigned)g.lives,
    (unsigned long)g.score);

  sendLifeFlash(300);

  if (g.lives == 0){
    Serial.printf("[L%uR%u] out of lives, ending game.\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1));
    g.active = false;
    clearTargets();
    pushLedStates();
    return;
  }

  restartCurrentRound();
}

// Lose a life without restarting (wrong hits in penaltyMode)
static void loseLifeOnly(const char* reason){
  if (!g.active) return;
  if (g.lives > 0) g.lives--;
  Serial.printf("[L%uR%u] %s -> life lost (no restart), lives=%u (score=%lu)\n",
    (unsigned)g.levelIndex,
    (unsigned)(g.roundIndex+1),
    reason ? reason : "life-loss",
    (unsigned)g.lives,
    (unsigned long)g.score);

  sendLifeFlash(300);

  if (g.lives == 0){
    Serial.printf("[L%uR%u] out of lives, ending game.\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1));
    g.active = false;
    clearTargets();
    pushLedStates();
  }
}

// Helper: get LevelConfig by index
static const struct LevelConfig* getLevelConfig(uint8_t idx){
  for (size_t i=0;i<sizeof(levelConfigs)/sizeof(levelConfigs[0]);++i){
    if (levelConfigs[i].index == idx) return &levelConfigs[i];
  }
  return nullptr;
}

// Determine layout blocks for current level
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

// Restart current round after timeout
static void restartCurrentRound(){
  if (!g.level) return;
  startRound(g.roundIndex);
}

// Advance to next static round (WARMUP/HIT_ALL) when cleared
static void onRoundCleared(){
  if (!g.level) return;

  uint8_t nextRound = g.roundIndex + 1;
  if (nextRound < g.level->numRounds){
    Serial.printf("[L%uR%u] round cleared, advancing to R%u\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (unsigned)(nextRound+1));
    startRound(nextRound);
    return;
  }

  // Fallback: if we somehow finish last static round without SHRINKING/BONUS, finish level
  if (g.roundKind != RK_SHRINKING && g.roundKind != RK_BONUS_MOVING){
    finishLevelAdvance();
  }
}

// Level progression after finishing all rounds
static void finishLevelAdvance(){
  if (g.levelIndex == 1){
    Serial.printf("[L1] complete, advancing to Level 2 (score=%lu, lives=%u)\n",
      (unsigned long)g.score, (unsigned)g.lives);
    startLevel(2, false);
  } else if (g.levelIndex == 2){
    Serial.printf("[L2] complete, advancing to Level 3 (score=%lu, lives=%u)\n",
      (unsigned long)g.score, (unsigned)g.lives);
    startLevel(3, false);
  } else if (g.levelIndex == 3){
    Serial.printf("[L3] complete, advancing to Level 4 (score=%lu, lives=%u)\n",
      (unsigned long)g.score, (unsigned)g.lives);
    startLevel(4, false);
  } else if (g.levelIndex == 4){
    Serial.printf("[L4] complete, game finished. final score=%lu lives=%u\n",
      (unsigned long)g.score, (unsigned)g.lives);
    g.active = false;
    clearTargets();
    pushLedStates();
  }
}

// After shrinking round (Round 4) is cleared
static void onShrinkCleared(){
  shrink.active = false;
  clearTargets();
  pushLedStates();

  if (g.level){
    uint8_t nextRound = g.roundIndex + 1;
    if (nextRound < g.level->numRounds &&
        g.level->rounds[nextRound] == RK_BONUS_MOVING){
      Serial.printf("[L%uR%u] shrinking round cleared, starting BONUS round R%u\n",
        (unsigned)g.levelIndex,
        (unsigned)(g.roundIndex+1),
        (unsigned)(nextRound+1));
      startRound(nextRound);
      return;
    }
  }

  finishLevelAdvance();
}

// After bonus sliding round (Round 5) is cleared or times out
static void onBonusCleared(){
  bonus.active = false;
  clearTargets();
  pushLedStates();

  Serial.printf("[L%uR%u] BONUS round complete (score=%lu, lives=%u)\n",
    (unsigned)g.levelIndex,
    (unsigned)(g.roundIndex+1),
    (unsigned long)g.score,
    (unsigned)g.lives);

  finishLevelAdvance();
}

// Start a specific round of the current level
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

  // Inform clients whether this is a bonus round
  sendRoundInfo(g.roundKind == RK_BONUS_MOVING);

  // Round duration
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
    nextLedPushAt = now + LED_PUSH_MIN_MS;

    Serial.printf("[L%uR1] start Warmup arena=%s block %u/%u ids=%u..%u dur=%lus\n",
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
    nextLedPushAt = now + LED_PUSH_MIN_MS;

    Serial.printf("[L%uR%u] start HitAll A arena=%s blocks=%u dur=%lus\n",
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
    nextLedPushAt = now + LED_PUSH_MIN_MS;

    Serial.printf("[L%uR%u] start HitAll B arena=%s blocks=%u dur=%lus\n",
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
    nextLedPushAt = now + LED_PUSH_MIN_MS;

    Serial.printf("[L%uR%u] start Shrinking arena=%s center=%u dur=%lus (widths=%u,%u,%u)\n",
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
    nextLedPushAt = now + LED_PUSH_MIN_MS;

    Serial.printf("[L%uR%u] start BONUS sliding arena=%s center=%u width=%u dur=%lus\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (g.arena==ARENA_TOP?"TOP":"SIDE"),
      (unsigned)bonus.center,
      (unsigned)bonus.width,
      (unsigned long)(g.roundDurationMs/1000));
    return;
  }
}

// Start Level
static void startLevel(uint8_t levelIndex, bool resetScoreAndLives){
  const struct LevelConfig* lc = getLevelConfig(levelIndex);
  if (!lc) return;

  g.level      = lc;
  g.levelIndex = lc->index;
  g.penaltyMode= lc->penaltyMode;
  g.arena      = ARENA_NONE;
  g.arenaA     = ARENA_NONE;
  g.gameStartMs= millis();
  memset(lastScoreMsBySensor, 0, sizeof(lastScoreMsBySensor));

  if (resetScoreAndLives){
    g.score = 0;
    g.lives = 5;
  }

  sendMode(g.penaltyMode);

  Serial.printf("[GAME] start Level %u  penalty=%s  lives=%u  score=%lu\n",
    (unsigned)g.levelIndex,
    g.penaltyMode ? "YES" : "NO",
    (unsigned)g.lives,
    (unsigned long)g.score);

  startRound(0);
}

// ========== Sensor handling ==========
static void handleSensorEvent(const SensorEventMsg* m){
  remember(m->name);
  if (!g.active) return;
  if (m->state != 1) return;

  bool isTopL  = !strcasecmp(m->name, NAME_TOPLEFT);
  bool isTopR  = !strcasecmp(m->name, NAME_TOPRIGHT);
  bool isSide  = !strcasecmp(m->name, NAME_SIDE);

  Arena eventArena = ARENA_NONE;
  uint8_t id = m->sensor_id;

  if ((isTopL || isTopR) && id >= 1 && id <= TOP_SENSOR_COUNT){
    eventArena = ARENA_TOP;
  } else if (isSide && id >= 1 && id <= SIDE_SENSOR_COUNT){
    eventArena = ARENA_SIDE;
  } else {
    return;
  }

  if (eventArena != g.arena) return;

  uint32_t now = millis();
  if (now < roundChangeIgnoreUntil) return;

  if (id < (sizeof(lastScoreMsBySensor)/sizeof(lastScoreMsBySensor[0]))){
    uint32_t last = lastScoreMsBySensor[id];
    if ((uint32_t)(now - last) < cfg.scoreRearmMs) return;
    lastScoreMsBySensor[id] = now;
  }

  // Shrinking round (Round 4)
  if (g.roundKind == RK_SHRINKING){
    if (!shrink.active) return;

    bool inTarget = (id >= shrink.minId && id <= shrink.maxId);

    if (!inTarget){
      if (g.penaltyMode){
        loseLifeOnly("WRONG HIT R4");
      }
      return;
    }

    g.score++;
    shrinkHits++;

    Serial.printf("[L%uR%u] HIT (SHRINKING) arena=%s sensor=%u stage=%u hits=%u score=%lu\n",
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
      nextLedPushAt = now;
    } else {
      onShrinkCleared();
    }
    return;
  }

  // Bonus sliding round (Round 5)
  if (g.roundKind == RK_BONUS_MOVING){
    if (!bonus.active) return;

    bool inTarget = (id >= bonus.minId && id <= bonus.maxId);

    // Bonus: no life loss; wrong hits ignored
    if (!inTarget){
      return;
    }

    g.score++;

    Serial.printf("[L%uR%u] HIT (BONUS) arena=%s sensor=%u score=%lu\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)id,
      (unsigned long)g.score);

    onBonusCleared();
    return;
  }

  // Warmup
  if (g.roundKind == RK_WARMUP){
    if (g.blockCount == 0) return;
    uint8_t b = g.activeBlock;
    if (b >= g.blockCount) return;

    bool inTarget = (id >= g.blockMin[b] && id <= g.blockMax[b]);

    if (!inTarget){
      if (g.penaltyMode){
        loseLifeOnly("WRONG HIT R1");
      }
      return;
    }

    g.score++;

    hitFlashActive  = true;
    hitFlashBlock   = b;
    hitFlashUntilMs = now + HIT_FLASH_MS;

    if (g.blockCount > 1){
      uint8_t newBlock = random(0, g.blockCount - 1);
      if (newBlock >= b) newBlock++;
      g.activeBlock = newBlock;
    } else {
      g.activeBlock = b;
    }

    rebuildTargetsFromBlocks();
    nextLedPushAt   = now;

    Serial.printf("[L%uR1] HIT arena=%s sensor=%u score=%lu nextBlock=%u/%u\n",
      (unsigned)g.levelIndex,
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)id,
      (unsigned long)g.score,
      (unsigned)(g.activeBlock+1),
      (unsigned)g.blockCount);
    return;
  }

  // Hit-all rounds
  if (g.roundKind == RK_HIT_ALL_A || g.roundKind == RK_HIT_ALL_B){
    if (g.blockCount == 0) return;

    uint8_t bFound = 255;
    for (uint8_t bIdx=0; bIdx<g.blockCount; ++bIdx){
      if (id >= g.blockMin[bIdx] && id <= g.blockMax[bIdx]){
        bFound = bIdx;
        break;
      }
    }

    bool badHit = (bFound == 255) || (bFound < g.blockCount && g.blockCleared[bFound]);

    if (badHit){
      if (g.penaltyMode){
        loseLifeOnly("WRONG HIT R2/R3");
      }
      return;
    }

    g.blockCleared[bFound] = true;
    g.score++;
    Serial.printf("[L%uR%u] HIT arena=%s block=%u/%u sensor=%u score=%lu\n",
      (unsigned)g.levelIndex,
      (unsigned)(g.roundIndex+1),
      (eventArena==ARENA_TOP ? "TOP" : "SIDE"),
      (unsigned)(bFound+1),
      (unsigned)g.blockCount,
      (unsigned)id,
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
}

// ========== RX ==========
static void onNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len){
  if (len < (int)sizeof(MsgHeader)) return;
  const MsgHeader* h=(const MsgHeader*)data;

  if (h->kind == MSG_HELLO && len >= (int)sizeof(HelloMsg)){
    const HelloMsg* m=(const HelloMsg*)data;
    remember(m->name);
    uint32_t now = millis();
    if (!strcasecmp(m->name, NAME_TOPRIGHT) || !strcasecmp(m->name, NAME_TOPLEFT) || !strcasecmp(m->name, NAME_SIDE)) {
      if ((int32_t)(now - nextLedPushAt) > 0) nextLedPushAt = now;
    }
    return;
  }

  if (h->kind == MSG_SENSOR_EVENT && len >= (int)sizeof(SensorEventMsg)){
    const SensorEventMsg* m=(const SensorEventMsg*)data;
    handleSensorEvent(m);
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
  Serial.printf("  R4DUR = %lu ms\n", (unsigned long)cfg.r4DurationMs);
  Serial.printf("  L1TOP = %u blocks\n", cfg.topBlocks);
  Serial.printf("  L1SIDE= %u blocks\n", cfg.sideBlocks);
  Serial.printf("  L2TOP = %u blocks (fixed)\n", (unsigned)L2_TOP_BLOCKS);
  Serial.printf("  L2SIDE= %u blocks (fixed)\n", (unsigned)L2_SIDE_BLOCKS);
  Serial.printf("  REARM = %lu ms\n", (unsigned long)cfg.scoreRearmMs);
  Serial.printf("  GRACE = %lu ms\n", (unsigned long)cfg.roundChangeGraceMs);
  Serial.printf("  BONUS = %lu ms\n", (unsigned long)BONUS_DURATION_MS);
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
  Serial.println("GAME L3      (Level 3 - penalty mode, layout 1, WITH warmup + bonus)");
  Serial.println("GAME L4      (Level 4 - penalty mode, layout 2, WITH warmup + bonus)");
  Serial.println("GAME STOP");
  Serial.println("CFG");
  Serial.println("SET <R1DUR|R2DUR|R3DUR|R4DUR|L1TOP|L1SIDE|REARM|GRACE> <value>");
  Serial.println();
  Serial.println("Examples:");
  Serial.println("  WIFI ALL AndrewiPhone 12345678");
  Serial.println("  OTA ALL http://172.20.10.3:8000/Soccer/SoccerClient/build/esp32.esp32.esp32/SoccerClient.ino.bin");
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
        g.active = true;
        startLevel(1, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L2")){
      if (g.active){
        Serial.println("Game already active. Use GAME STOP first.");
      } else {
        g.active = true;
        startLevel(2, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L3")){
      if (g.active){
        Serial.println("Game already active. Use GAME STOP first.");
      } else {
        g.active = true;
        startLevel(3, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("L4")){
      if (g.active){
        Serial.println("Game already active. Use GAME STOP first.");
      } else {
        g.active = true;
        startLevel(4, true);
      }
      return;
    }
    if (arg1.equalsIgnoreCase("STOP")){
      if (!g.active) Serial.println("No active game.");
      else {
        g.active = false;
        clearTargets();
        pushLedStates();
        Serial.println("[GAME] stopped by CLI.");
      }
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
      Serial.println("Usage: SET <R1DUR|R2DUR|R3DUR|R4DUR|L1TOP|L1SIDE|REARM|GRACE> <value>");
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
    } else if (arg1.equalsIgnoreCase("R4DUR")){
      if (v <= 0){ Serial.println("R4DUR must be >0"); return; }
      cfg.r4DurationMs = (uint32_t)v;
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
      Serial.println("Unknown key. Use R1DUR, R2DUR, R3DUR, R4DUR, L1TOP, L1SIDE, REARM, GRACE");
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

  // Update shrinking round targets
  if (g.active && g.roundKind == RK_SHRINKING){
    updateShrinkingTargets();
  }

  // Update bonus sliding round targets
  if (g.active && g.roundKind == RK_BONUS_MOVING){
    updateBonusTargets();
  }

  // Timers:
  // - Warmup / Bonus: timeout => advance (no life loss)
  // - HitAll / Shrinking: timeout => lose a life + restart same round.
  if (g.active && g.roundDurationMs > 0){
    if ((uint32_t)(now - g.roundStartMs) >= g.roundDurationMs){
      if (g.roundKind == RK_WARMUP){
        Serial.printf("[L%uR%u] TIMEOUT (Warmup) -> next round, score=%lu lives=%u\n",
          (unsigned)g.levelIndex,
          (unsigned)(g.roundIndex+1),
          (unsigned long)g.score,
          (unsigned)g.lives);
        uint8_t nextRound = g.roundIndex + 1;
        startRound(nextRound);
      } else if (g.roundKind == RK_BONUS_MOVING){
        Serial.printf("[L%uR%u] TIMEOUT (BONUS) -> finish level, score=%lu lives=%u\n",
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

  if ((int32_t)(now - nextLedPushAt) >= 0){
    pushLedStates();
    nextLedPushAt = now + LED_PUSH_MIN_MS;
  }

  delay(2);
}
