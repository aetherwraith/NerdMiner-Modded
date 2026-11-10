#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "ShaTests/nerdSHA256plus.h"
#include "stratum.h"
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "timeconst.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "wManager.h"
#include <atomic>
#include <mutex>
#include <list>
#include <stddef.h> // offsetof (saved stats record)
#include <string.h>
#include <time.h>
#include <map>
#include "mbedtls/sha256.h"
#include "i2c_master.h"
#include <esp_timer.h>

// 10 Jobs per second
#define NONCE_PER_JOB_SW 4096
// The SW and HW miners must never hash the same nonce of a job (the pool
// refuses the second one as "Duplicate"). The pipelined HW loop searches
// byte-swapped nonces from its own cursor, so native-space chunk ranges alone
// don't keep it apart from the SW chunks: they split the nonce space by the
// low byte of the native nonce instead. SW chunks only use nonces whose low
// byte is below 0x80 (swNonce(): a 4096-nonce chunk spans 8192 natives, all
// allocations stay multiples of 256), while HW paths only use nonces whose
// low byte is 0x80 and above (hwNonce() or pipelined top-bit cursor).
#define SW_NONCE_SPAN (2 * NONCE_PER_JOB_SW)
static inline uint32_t swNonce(uint32_t start, uint32_t n)
{
  return start + ((n >> 7) << 8) + (n & 0x7Fu);
}
static inline uint32_t hwNonce(uint32_t start, uint32_t n)
{
  return start + ((n >> 7) << 8) + 0x80u + (n & 0x7Fu);
}

// Chunk size the classic-ESP32 pipelined path re-enters C for (256K nonces).
#define NONCE_PER_JOB_HW (256 * 1024)

//#define I2C_SLAVE
//#define SHA256_VALIDATE
//#define RANDOM_NONCE
#define RANDOM_NONCE_MASK 0xFFFFC000u

#ifdef HARDWARE_SHA265
#include <sha/sha_dma.h>
#include <hal/sha_hal.h>
#include <hal/sha_ll.h>

#if defined(CONFIG_IDF_TARGET_ESP32)
#include <sha/sha_parallel_engine.h>
#include <driver/periph_ctrl.h>
#include <hal/efuse_hal.h>
#endif

#endif

nvs_handle_t stat_handle;

uint32_t templates = 0;
// Hashes since the saved stats began (restored at boot + this run) and since
// boot (session), guarded by s_hashMux so 64-bit values never tear across cores.
static uint64_t s_hashTotal = 0;
static uint64_t s_sessionHashes = 0;
static portMUX_TYPE s_hashMux = portMUX_INITIALIZER_UNLOCKED;

void addHashes(uint32_t n)
{
  if (n == 0)
    return;
  portENTER_CRITICAL(&s_hashMux);
  s_hashTotal += n;
  s_sessionHashes += n;
  portEXIT_CRITICAL(&s_hashMux);
}

// 64 bits are two words here: read under the lock, never half updated.
uint64_t getTotalHashes()
{
  portENTER_CRITICAL(&s_hashMux);
  const uint64_t v = s_hashTotal;
  portEXIT_CRITICAL(&s_hashMux);
  return v;
}

uint64_t getSessionHashes()
{
  portENTER_CRITICAL(&s_hashMux);
  const uint64_t v = s_sessionHashes;
  portEXIT_CRITICAL(&s_hashMux);
  return v;
}

// Saved stats read back at boot: added to all-time total only, so session
// hashes still reflect work computed since boot.
static void restoreHashes(uint32_t mhashes)
{
  portENTER_CRITICAL(&s_hashMux);
  s_hashTotal += (uint64_t)mhashes * 1000000ULL;
  portEXIT_CRITICAL(&s_hashMux);
}

uint64_t totalKHashes = 0; // 32 bits wrapped after ~4.3 TH (about 50 days at 1 MH/s)
uint32_t elapsedKHs = 0;
uint64_t upTime = 0;

// Per-path attempt counters, only used to print a HW vs SW hashrate
// breakdown under DEBUG_MINING so real hardware can be A/B tested
// (see FORCE_SW_MINING in mining.h).
volatile uint32_t debugHashesHw = 0;
volatile uint32_t debugHashesSw[2] = {0, 0};
volatile uint32_t debugPipelinedHits = 0;
volatile uint32_t debugPipelinedHwSwMismatch = 0;
volatile uint32_t debugPipelinedAccepted = 0;
volatile uint32_t debugPipelinedRealHits = 0;
volatile uint32_t debugChunkGapUs = 0;
volatile uint32_t debugChunkCount = 0;
volatile uint32_t debugHitOverheadUs = 0;
volatile uint32_t debugHitOverheadCount = 0;

volatile uint32_t shares; // 32-bit shares (difficulty >= 1) the pool ACCEPTED
volatile uint32_t shares32Rejected = 0; // 32-bit shares the pool refused (see mining.h)
volatile uint32_t valids; // increased if blockhash <= target
volatile uint32_t rejects = 0; // increased if stratum pool rejects submission

// Stats are only written once they have been read back at boot: saving the
// still-empty in-RAM counters before that would wipe the saved ones.
static volatile bool s_statsLoaded = false;

// Event counters for buzzer / display / LAN celebration
static volatile uint32_t s_sharesAccepted = 0;
volatile uint32_t g_shareFoundEvents = 0; // see mining.h
volatile uint32_t g_blockFoundEvents = 0;
volatile uint32_t g_bestDiffEvents = 0;

// Track best diff (all-time and session)
double best_diff = 0.0;
static double s_sessionBestDiff = 0.0;
static portMUX_TYPE s_bestDiffMux = portMUX_INITIALIZER_UNLOCKED;

void updateBestDiff(double diff)
{
  if (!(diff > s_sessionBestDiff) && !(diff > best_diff))
    return;
  portENTER_CRITICAL(&s_bestDiffMux);
  if (diff > s_sessionBestDiff)
    s_sessionBestDiff = diff;
  if (diff > best_diff)
  {
    if (s_statsLoaded && best_diff > 0.0)
      g_bestDiffEvents++;
    best_diff = diff;
  }
  portEXIT_CRITICAL(&s_bestDiffMux);
}

double getBestDiff()
{
  portENTER_CRITICAL(&s_bestDiffMux);
  const double v = best_diff;
  portEXIT_CRITICAL(&s_bestDiffMux);
  return v;
}

double getSessionBestDiff()
{
  portENTER_CRITICAL(&s_bestDiffMux);
  const double v = s_sessionBestDiff;
  portEXIT_CRITICAL(&s_bestDiffMux);
  return v;
}

// Top NOTABLE_SHARES_MAX highest-difficulty shares of the session, guarded by s_hashMux.
static NotableShare s_notableShares[NOTABLE_SHARES_MAX];
static size_t s_notableCount = 0;

static void recordNotableShare(double diff, bool isBlock)
{
  if (!(diff > 0.0))
    return;
  const time_t now = time(nullptr);
  NotableShare entry;
  entry.epoch = (now > 1700000000) ? (uint32_t)now : 0;
  entry.uptimeS = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  entry.difficulty = diff;
  entry.is32Bit = (diff >= 1.0);
  entry.isBlock = isBlock;

  auto better = [](const NotableShare &a, const NotableShare &b) {
    if (a.isBlock != b.isBlock)
      return a.isBlock;
    return a.difficulty > b.difficulty;
  };

  portENTER_CRITICAL(&s_hashMux);
  if (s_notableCount < NOTABLE_SHARES_MAX || better(entry, s_notableShares[s_notableCount - 1]))
  {
    size_t pos = s_notableCount;
    if (pos < NOTABLE_SHARES_MAX)
      s_notableCount++;
    else
      pos = NOTABLE_SHARES_MAX - 1;
    while (pos > 0 && better(entry, s_notableShares[pos - 1]))
    {
      s_notableShares[pos] = s_notableShares[pos - 1];
      pos--;
    }
    s_notableShares[pos] = entry;
  }
  portEXIT_CRITICAL(&s_hashMux);
}

size_t getNotableShares(NotableShare *out, size_t maxCount)
{
  if (!out || maxCount == 0)
  {
    portENTER_CRITICAL(&s_hashMux);
    const size_t n = s_notableCount;
    portEXIT_CRITICAL(&s_hashMux);
    return n;
  }
  portENTER_CRITICAL(&s_hashMux);
  const size_t n = (s_notableCount < maxCount) ? s_notableCount : maxCount;
  memcpy(out, s_notableShares, n * sizeof(NotableShare));
  portEXIT_CRITICAL(&s_hashMux);
  return n;
}

extern TSettings Settings;

IPAddress serverIP(1, 1, 1, 1); // Temporarily save pool IP address

// Global work data
static WiFiClient client;
static volatile bool s_stratumConnected = false;
static TaskHandle_t s_stratumTaskHandle = nullptr;
static miner_data mMiner;
mining_subscribe mWorker;
mining_job mJob;
monitor_data mMonitor;
static bool volatile isMinerSuscribed = false;
unsigned long mLastTXtoPool = millis();

// Compact pending pool-change buffer (saves ~240 B + avoids TSettings String
// default-constructor heap churn vs storing a full TSettings). Guarded by g_settingsMutex.
struct PoolChange
{
  String PoolAddress;
  int PoolPort = 0;
  char BtcWallet[80]{};
  char PoolPassword[80]{};
  String BackupPoolAddress;
  int BackupPoolPort = 0;
  char BackupPoolPassword[80]{};
};
static bool s_poolChangePending = false;
static PoolChange s_poolChange;

static void storePoolChange(PoolChange &to, const TSettings &from)
{
  to.PoolAddress = from.PoolAddress;
  to.PoolPort = from.PoolPort;
  memcpy(to.BtcWallet, from.BtcWallet, sizeof(to.BtcWallet));
  to.BtcWallet[sizeof(to.BtcWallet) - 1] = 0;
  memcpy(to.PoolPassword, from.PoolPassword, sizeof(to.PoolPassword));
  to.PoolPassword[sizeof(to.PoolPassword) - 1] = 0;
  to.BackupPoolAddress = from.BackupPoolAddress;
  to.BackupPoolPort = from.BackupPoolPort;
  memcpy(to.BackupPoolPassword, from.BackupPoolPassword, sizeof(to.BackupPoolPassword));
  to.BackupPoolPassword[sizeof(to.BackupPoolPassword) - 1] = 0;
}

static void loadPoolChange(TSettings &to, const PoolChange &from)
{
  to.PoolAddress = from.PoolAddress;
  to.PoolPort = from.PoolPort;
  memcpy(to.BtcWallet, from.BtcWallet, sizeof(to.BtcWallet));
  to.BtcWallet[sizeof(to.BtcWallet) - 1] = 0;
  memcpy(to.PoolPassword, from.PoolPassword, sizeof(to.PoolPassword));
  to.PoolPassword[sizeof(to.PoolPassword) - 1] = 0;
  to.BackupPoolAddress = from.BackupPoolAddress;
  to.BackupPoolPort = from.BackupPoolPort;
  memcpy(to.BackupPoolPassword, from.BackupPoolPassword, sizeof(to.BackupPoolPassword));
  to.BackupPoolPassword[sizeof(to.BackupPoolPassword) - 1] = 0;
}

// Backup pool (see mining.h). Only the stratum task changes these.
static volatile bool s_onBackup = false;
static uint8_t s_poolFailures = 0;   // failed attempts in a row on the pool in use
static uint32_t s_backupSince = 0;   // millis() of the switch to the backup, or of the last look at the main pool

static bool backupPoolSet(void)
{
  return !Settings.BackupPoolAddress.isEmpty() && Settings.BackupPoolPort > 0;
}

bool onBackupPool(void)
{
  return s_onBackup;
}

String activePoolAddress(void)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  return s_onBackup ? Settings.BackupPoolAddress : Settings.PoolAddress;
}

int activePoolPort(void)
{
  return s_onBackup ? Settings.BackupPoolPort : Settings.PoolPort;
}

static const char *activePoolPassword(void)
{
  return s_onBackup ? Settings.BackupPoolPassword : Settings.PoolPassword;
}

// While on the backup: does the main pool take a connection again?
static bool mainPoolAnswers(void)
{
  String mainAddr;
  int mainPort = 0;
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    mainAddr = Settings.PoolAddress;
    mainPort = Settings.PoolPort;
  }
  IPAddress ip;
  if (!WiFi.hostByName(mainAddr.c_str(), ip))
    return false;
  WiFiClient probe;
  const bool ok = probe.connect(ip, mainPort, 3000);
  probe.stop();
  return ok;
}

// Pool difficulty (guarded by s_hashMux to prevent 64-bit double tearing)
static double s_poolDifficulty = DEFAULT_DIFFICULTY;
static void setPoolDifficulty(double d)
{
  portENTER_CRITICAL(&s_hashMux);
  s_poolDifficulty = d;
  portEXIT_CRITICAL(&s_hashMux);
}
static double getPoolDifficulty(void)
{
  portENTER_CRITICAL(&s_hashMux);
  const double d = s_poolDifficulty;
  portEXIT_CRITICAL(&s_hashMux);
  return d;
}

// mining.authorize state
static unsigned long s_authId = 0;
static volatile int8_t s_authorized = -1;

// Share submit -> ACK ping RTT tracking (8-slot ring buffer)
struct SubmitSentSlot
{
  unsigned long id;
  uint32_t sentMs;
};
static SubmitSentSlot s_submitSentRing[8] = {};
static uint8_t s_submitSentHead = 0;
static volatile int32_t s_lastPingMs = -1;

// kH counter samples (10 s window), guarded by s_hashMux against 64-bit tearing
#define STATUS_RATE_SAMPLES 11
static uint64_t s_rateKh[STATUS_RATE_SAMPLES];
static uint32_t s_rateMs[STATUS_RATE_SAMPLES];
static uint8_t s_rateHead = 0, s_rateCount = 0;
static uint64_t s_bootKh = 0;
static int64_t s_bootUs = 0, s_lastSampleUs = 0;

static void statusRateSample(uint64_t kh, uint32_t ms)
{
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL(&s_hashMux);
  const uint8_t i = (s_rateHead + 1) % STATUS_RATE_SAMPLES;
  s_rateKh[i] = kh;
  s_rateMs[i] = ms;
  s_lastSampleUs = nowUs;
  s_rateHead = i;
  if (s_rateCount < STATUS_RATE_SAMPLES)
    s_rateCount++;
  portEXIT_CRITICAL(&s_hashMux);
}

int8_t poolAuthorized(void)
{
  return s_authorized;
}

MinerStatus getMinerStatus(void)
{
  MinerStatus s{};
  uint64_t headKh = 0, oldestKh = 0, bootKh = 0, sessHashes = 0;
  uint32_t headMs = 0, oldestMs = 0;
  int64_t lastUs = 0, bootUs = 0;
  uint8_t n = 0;
  double poolDiff = DEFAULT_DIFFICULTY;

  portENTER_CRITICAL(&s_hashMux);
  const uint8_t head = s_rateHead;
  n = s_rateCount;
  if (n >= 2)
  {
    const uint8_t oldest = (head + STATUS_RATE_SAMPLES - (n - 1)) % STATUS_RATE_SAMPLES;
    headKh = s_rateKh[head];
    oldestKh = s_rateKh[oldest];
    headMs = s_rateMs[head];
    oldestMs = s_rateMs[oldest];
    lastUs = s_lastSampleUs;
    bootUs = s_bootUs;
    bootKh = s_bootKh;
  }
  poolDiff = s_poolDifficulty;
  sessHashes = s_sessionHashes;
  portEXIT_CRITICAL(&s_hashMux);

  if (n >= 2)
  {
    const uint32_t ms = headMs - oldestMs;
    if (ms)
      s.hashrateKHs = (headKh - oldestKh) * 1000.0f / ms;
    const int64_t elapsedBootUs = lastUs - bootUs;
    if (elapsedBootUs > 0)
      s.avgHashrateKHs = (double)(headKh - bootKh) * 1e6 / (double)elapsedBootUs;
  }
  s.accepted = s_sharesAccepted;
  s.rejected = rejects;
  s.subscribed = isMinerSuscribed;
  s.poolDifficulty = poolDiff;
  s.authorized = s_authorized;
  s.bestDiff = getBestDiff();
  s.sessionBestDiff = getSessionBestDiff();
  s.pingMs = s_lastPingMs;
  s.sessionHashes = sessHashes;
  return s;
}

// ---------------------------------------------------------------------------
// LAN UDP fleet share celebration (port 33339, 24-byte "NMCELEB1" packet)
// ---------------------------------------------------------------------------
static constexpr uint16_t CELEB_UDP_PORT = 33339;
static WiFiUDP s_celebUdp;
static bool s_celebUdpStarted = false;
static uint32_t s_seenShareEvents = 0;
static uint32_t s_seenBlockEvents = 0;
static uint32_t s_seenBestEvents = 0;
static uint32_t s_lastSentCelebNonce = 0;
static uint32_t s_lastCelebTxMs = 0;
static bool s_everSentCeleb = false;

void celebrateProcess(void)
{
  const uint32_t curShares = g_shareFoundEvents;
  const uint32_t curBlocks = g_blockFoundEvents;
  const uint32_t curBest = g_bestDiffEvents;

  uint32_t kindFlags = 0;
  if (curShares != s_seenShareEvents)
  {
    s_seenShareEvents = curShares;
    kindFlags |= 1u;
  }
  if (curBlocks != s_seenBlockEvents)
  {
    s_seenBlockEvents = curBlocks;
    kindFlags |= 2u;
  }
  if (curBest != s_seenBestEvents)
  {
    s_seenBestEvents = curBest;
    kindFlags |= 4u;
  }

  if (kindFlags != 0)
    triggerShareCelebration(4000);

  if (WiFi.status() != WL_CONNECTED)
  {
    if (s_celebUdpStarted)
    {
      s_celebUdp.stop();
      s_celebUdpStarted = false;
    }
    return;
  }

  if (!s_celebUdpStarted)
  {
    if (s_celebUdp.begin(CELEB_UDP_PORT))
      s_celebUdpStarted = true;
  }
  if (!s_celebUdpStarted)
    return;

  if (kindFlags != 0)
  {
    const uint32_t nowMs = millis();
    if (!s_everSentCeleb || (uint32_t)(nowMs - s_lastCelebTxMs) >= 5000U)
    {
      uint32_t nonce = esp_random();
      if (nonce == 0)
        nonce = 1;
      s_lastSentCelebNonce = nonce;
      s_lastCelebTxMs = nowMs;
      s_everSentCeleb = true;

      uint8_t pkt[24];
      memcpy(pkt, "NMCELEB1", 8);
      memcpy(pkt + 8, &nonce, 4);
      memcpy(pkt + 12, &kindFlags, 4);
      const double bd = getBestDiff();
      memcpy(pkt + 16, &bd, 8);

      if (s_celebUdp.beginPacket(IPAddress(255, 255, 255, 255), CELEB_UDP_PORT))
      {
        s_celebUdp.write(pkt, sizeof(pkt));
        s_celebUdp.endPacket();
      }
    }
  }

  for (int i = 0; i < 4; ++i)
  {
    const int pktLen = s_celebUdp.parsePacket();
    if (pktLen <= 0)
      break;
    uint8_t rx[24];
    const int rd = s_celebUdp.read(rx, sizeof(rx));
    while (s_celebUdp.available() > 0)
      s_celebUdp.read();
    if (rd >= 12 && memcmp(rx, "NMCELEB1", 8) == 0)
    {
      uint32_t rxNonce = 0;
      memcpy(&rxNonce, rx + 8, 4);
      if (rxNonce != 0 && rxNonce != s_lastSentCelebNonce)
        triggerShareCelebration(4000);
    }
  }
}

void requestPoolChange(const TSettings &s)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  storePoolChange(s_poolChange, s);
  s_poolChangePending = true;
}

void readPoolSettings(TSettings &s)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  if (s_poolChangePending)
  {
    loadPoolChange(s, s_poolChange);
  }
  else
  {
    PoolChange cur;
    storePoolChange(cur, Settings);
    loadPoolChange(s, cur);
  }
}

// Stratum task only. Swaps a waiting pool change into Settings; true if the
// pool in use changed and has to be connected again.
static bool takePoolChange(void)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  if (!s_poolChangePending)
    return false;
  const PoolChange &n = s_poolChange;
  const bool mainChanged = n.PoolAddress != Settings.PoolAddress || n.PoolPort != Settings.PoolPort ||
                           strcmp(n.BtcWallet, Settings.BtcWallet) != 0 ||
                           strcmp(n.PoolPassword, Settings.PoolPassword) != 0;
  const bool backupChanged = n.BackupPoolAddress != Settings.BackupPoolAddress ||
                             n.BackupPoolPort != Settings.BackupPoolPort ||
                             strcmp(n.BackupPoolPassword, Settings.BackupPoolPassword) != 0;
  loadPoolChange(Settings, s_poolChange);
  s_poolChangePending = false;
  if (!mainChanged && !(s_onBackup && backupChanged))
    return false;
  s_onBackup = s_onBackup && !mainChanged && backupPoolSet();
  s_poolFailures = 0;
  s_backupSince = millis();
  return true;
}

int saveIntervals[7] = {5 * 60, 15 * 60, 30 * 60, 1 * 3600, 3 * 3600, 6 * 3600, 12 * 3600};
#ifndef STATS_SAVE_MAX_s
#define STATS_SAVE_MAX_s (12 * 3600)
#endif
int saveIntervalsSize = sizeof(saveIntervals)/sizeof(saveIntervals[0]);
int currentIntervalIndex = 0;

bool checkPoolConnection(void) {
  if (client.connected()) {
    s_stratumConnected = true;
    return true;
  }

  s_stratumConnected = false;
  isMinerSuscribed = false;

  Serial.println("Client not connected, trying to connect...");
  const String poolAddr = activePoolAddress();

  // Resolve first time pool DNS and save IP
  if (serverIP == IPAddress(1, 1, 1, 1)) {
    WiFi.hostByName(poolAddr.c_str(), serverIP);
    Serial.printf("Resolved DNS and save ip (first time) got: %s\n", serverIP.toString().c_str());
  }

  // Try connecting pool IP
  if (!client.connect(serverIP, activePoolPort())) {
    Serial.println("Imposible to connect to : " + poolAddr);
    WiFi.hostByName(poolAddr.c_str(), serverIP);
    Serial.printf("Resolved DNS got: %s\n", serverIP.toString().c_str());
    return false;
  }

  s_stratumConnected = true;
  return true;
}

// Implements a socketKeepAlive function and checks if pool is not sending any data to reconnect again.
unsigned long mStart0Hashrate = 0;
bool checkPoolInactivity(unsigned int keepAliveTime, unsigned long inactivityTime) {
  uint64_t currentKHashes = getTotalHashes() / 1000;
  uint64_t elapsedKHs = currentKHashes - totalKHashes;

  uint32_t time_now = millis();

  if (time_now - mLastTXtoPool > keepAliveTime) // wrap-safe
  {
    mLastTXtoPool = time_now;
    Serial.println("  Sending  : KeepAlive suggest_difficulty");
    tx_suggest_difficulty(client, getPoolDifficulty());
  }

  if (elapsedKHs == 0) {
    if (mStart0Hashrate == 0)
      mStart0Hashrate = time_now;
    if ((time_now - mStart0Hashrate) > inactivityTime) {
      mStart0Hashrate = 0;
      return true;
    }
    return false;
  }

  mStart0Hashrate = 0;
  return false;
}

// ---------------------------------------------------------------------------
// Double-buffered immutable ActiveJob + lock-free atomic chunk cursors
// (replaces per-chunk std::make_shared<JobRequest> heap churn)
// ---------------------------------------------------------------------------
struct ActiveJob
{
  uint32_t id;
  uint32_t nonce_base;
  double difficulty;
  uint8_t sha_buffer[128];
  uint32_t midstate[8];
  uint32_t bake[16];
#ifdef HARDWARE_SHA265
#if defined(CONFIG_IDF_TARGET_ESP32)
  uint32_t sha_buffer_swap[32];
#elif defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
  uint32_t hw_midstate[8];
#endif
#endif
};

struct JobResult
{
  uint32_t id;
  uint32_t nonce;
  double difficulty;
  uint8_t hash[32];
};

static std::mutex s_job_mutex;
std::mutex g_hwShaMutex;  // see mining.h for why this exists
volatile bool g_otaUpdating = false;
static volatile bool s_tlsPauseRequested = false;
#if defined(PIPELINED_S3_MINING)
std::atomic<int> g_hwShaWanted{0}; // see mining.h
#endif

static ActiveJob s_jobs[2];
static volatile uint8_t s_activeJobSlot = 0xFF;
static std::atomic<uint32_t> s_swNextIndex{0};
#ifdef HARDWARE_SHA265
static std::atomic<uint32_t> s_hwNextIndex{0};
#endif
std::list<std::shared_ptr<JobResult>> s_job_result_list;
static volatile uint8_t s_working_current_job_id = 0xFF;

static inline void notifyStratumTask(void)
{
  TaskHandle_t h = s_stratumTaskHandle;
  if (h)
    xTaskNotifyGive(h);
}

static void submitShareResult(uint32_t jobId, uint32_t nonce, double diff, const uint8_t hash[32])
{
  std::shared_ptr<JobResult> share = std::make_shared<JobResult>();
  share->id = jobId;
  share->nonce = nonce;
  share->difficulty = diff;
  memcpy(share->hash, hash, 32);
  {
    std::lock_guard<std::mutex> lock(s_job_mutex);
    if (s_job_result_list.size() < 256)
      s_job_result_list.push_back(share);
  }
  notifyStratumTask();
}

#if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
// What the pipelined HW loop works from (layout is known to the asm, see
// src/pipelined_hw_sha_classic_v2.S).
struct pl_ctx_t
{
  uint32_t hdr[20];       // header words as the SHA engine wants them (byte-swapped)
  uint32_t nonce;         // in: first nonce to try (swapped space); out: next one
  uint32_t budget;        // in: nonces to try; out: nonces not tried
  volatile uint32_t run;  // the asm reads it every nonce, so clearing it stops a chunk at once
  uint32_t pad;           // 0x80000000
  uint32_t chk;           // bench builds only
};
static pl_ctx_t s_pl_ctx;
#endif

void pauseHwMinerForTls(void)
{
  s_tlsPauseRequested = true;
#if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
  s_pl_ctx.run = 0;
#endif
}

struct Submition
{
  double diff;
  bool is32bit;
  bool isValid;
};

static void MiningJobStop(uint32_t &job_pool, std::map<uint32_t, std::shared_ptr<Submition>> &submition_map)
{
  {
    std::lock_guard<std::mutex> lock(s_job_mutex);
    s_job_result_list.clear();
    s_activeJobSlot = 0xFF;
    s_working_current_job_id = 0xFF;
  }
#if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
  s_pl_ctx.run = 0;
#endif
  job_pool = 0xFFFFFFFF;
  submition_map.clear();
  memset(s_submitSentRing, 0, sizeof(s_submitSentRing));
  mMonitor.NerdStatus = NM_Connecting; // back to NM_hashing with the next job
}

#ifdef RANDOM_NONCE
uint64_t s_random_state = 1;
static uint32_t RandomGet()
{
    s_random_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = s_random_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
#endif

void runStratumWorker(void *name) {
  s_stratumTaskHandle = xTaskGetCurrentTaskHandle();

  Serial.println("");
  Serial.printf("\n[WORKER] Started. Running %s on core %d\n", (char *)name, xPortGetCoreID());

  #ifdef DEBUG_MEMORY
  Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  #endif

  std::map<uint32_t, std::shared_ptr<Submition>> s_submition_map;

#ifdef I2C_SLAVE
  std::vector<uint8_t> i2c_slave_vector;

  if (i2c_master_start() == 0)
    i2c_slave_vector = i2c_master_scan(0x0, 0x80);
  Serial.printf("Found %d slave workers\n", i2c_slave_vector.size());
  if (!i2c_slave_vector.empty())
  {
    Serial.print("  Workers: ");
    for (size_t n = 0; n < i2c_slave_vector.size(); ++n)
      Serial.printf("0x%02X,", (uint32_t)i2c_slave_vector[n]);
    Serial.println("");
  }
#endif

  double currentPoolDifficulty = DEFAULT_DIFFICULTY;
  uint32_t nonce_pool = 0;
  uint32_t job_pool = 0xFFFFFFFF;
  uint32_t last_job_time = millis();

  String rxBuf;
  rxBuf.reserve(2048);
  bool rxDropUntilNewline = false;

  auto resetSocket = [&]() {
    client.stop();
    s_stratumConnected = false;
    isMinerSuscribed = false;
    rxBuf.remove(0);
    rxDropUntilNewline = false;
  };

  auto dropPool = [&]() {
    resetSocket();
    MiningJobStop(job_pool, s_submition_map);
    currentPoolDifficulty = DEFAULT_DIFFICULTY;
    setPoolDifficulty(currentPoolDifficulty);
    s_lastPingMs = -1;
    serverIP = IPAddress(1, 1, 1, 1);
    poolSettingsChanged(); // monitor: pool stats URL and data
  };
  auto switchPool = [&](bool backup, const char *why) {
    s_onBackup = backup;
    s_poolFailures = 0;
    s_backupSince = millis();
    Serial.printf("[STRATUM] %s, switching to the %s pool %s:%d\n", why, backup ? "backup" : "main",
                  activePoolAddress().c_str(), activePoolPort());
    dropPool();
  };
  auto poolFailed = [&]() {
    if (s_poolFailures < 255)
      s_poolFailures++;
    if (s_poolFailures < POOL_FAILOVER_TRIES || !backupPoolSet())
      return false;
    switchPool(!s_onBackup, s_onBackup ? "Backup pool not answering" : "Pool not answering");
    return true;
  };

  #if defined(HARDWARE_SHA265) && (defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3))
  uint32_t hw_midstate[8] = {0};
  #endif
  uint32_t diget_mid[8] = {0};
  uint32_t bake[16] = {0};
  #if defined(HARDWARE_SHA265) && defined(CONFIG_IDF_TARGET_ESP32)
  uint8_t sha_buffer_swap[128] = {0};
  #endif

  while (true) {

    if (takePoolChange()) {
      Serial.printf("[STRATUM] Pool settings changed, switching to %s:%d\n", activePoolAddress().c_str(), activePoolPort());
      dropPool();
    }

    if (WiFi.status() != WL_CONNECTED) {
      s_stratumConnected = false;
      mMonitor.NerdStatus = NM_Connecting;
      MiningJobStop(job_pool, s_submition_map);
      WiFi.reconnect();
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    if (!checkPoolConnection()) {
      MiningJobStop(job_pool, s_submition_map);
      if (poolFailed())
        continue;
      for (int s = 1 + rand() % 60; s > 0; s--) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (s_poolChangePending) break;
      }
      continue;
    }

    if (!isMinerSuscribed)
    {
      rxBuf.remove(0);
      rxDropUntilNewline = false;
      mWorker = init_mining_subscribe();

      // STEP 1: Pool server connection (SUBSCRIBE)
      if (!tx_mining_subscribe(client, mWorker)) {
        resetSocket();
        MiningJobStop(job_pool, s_submition_map);
        poolFailed();
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        strlcpy(mWorker.wName, Settings.BtcWallet, sizeof(mWorker.wName));
        strlcpy(mWorker.wPass, activePoolPassword(), sizeof(mWorker.wPass));
      }
      // STEP 2: Pool authorize work
      s_authorized = -1;
      tx_mining_auth(client, mWorker.wName, mWorker.wPass, s_authId);

      // STEP 3: Suggest pool difficulty
      tx_suggest_difficulty(client, currentPoolDifficulty);

      isMinerSuscribed = true;
      uint32_t time_now = millis();
      mLastTXtoPool = time_now;
      last_job_time = time_now;
    }

    if (checkPoolInactivity(KEEPALIVE_TIME_ms, POOLINACTIVITY_TIME_ms)) {
      Serial.println("  Detected more than 2 min without data form stratum server. Closing socket and reopening...");
      resetSocket();
      MiningJobStop(job_pool, s_submition_map);
      poolFailed();
      continue;
    }

    {
      uint32_t time_now = millis();
      if (time_now - last_job_time >= 10 * 60 * 1000) // 10 minutes without job (wrap-safe)
      {
        resetSocket();
        MiningJobStop(job_pool, s_submition_map);
        poolFailed();
        continue;
      }
    }

    // On the backup: go back to the main pool once it answers again.
    if (s_onBackup && millis() - s_backupSince >= POOL_FAILBACK_ms)
    {
      s_backupSince = millis();
      if (mainPoolAnswers())
      {
        switchPool(false, "Main pool answers again");
        continue;
      }
      String mainAddr;
      int mainPort = 0;
      {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        mainAddr = Settings.PoolAddress;
        mainPort = Settings.PoolPort;
      }
      Serial.printf("[STRATUM] Main pool %s:%d still not answering, staying on the backup\n",
                    mainAddr.c_str(), mainPort);
    }

    // Non-blocking chunked line reader into persistent rxBuf so split TCP
    // segments never trigger bogus STRATUM_PARSE_ERRORs.
    while (client.connected())
    {
      int nl = rxBuf.indexOf('\n');
      while (nl < 0 && client.available() > 0)
      {
        uint8_t tmp[256];
        int want = client.available();
        if (want > (int)sizeof(tmp))
          want = (int)sizeof(tmp);
        const int got = client.read(tmp, want);
        if (got <= 0)
          break;
        bool sawNl = false;
        for (int i = 0; i < got; ++i)
        {
          const char ch = (char)tmp[i];
          if (rxDropUntilNewline)
          {
            if (ch == '\n')
              rxDropUntilNewline = false;
            continue;
          }
          if (ch == '\r')
            continue;
          rxBuf += ch;
          if (ch == '\n')
            sawNl = true;
          else if (!sawNl && rxBuf.length() > MAX_NOTIFY_LEN + 64)
          {
            Serial.printf("[STRATUM] Line exceeded %u bytes, dropping\n", (unsigned)(MAX_NOTIFY_LEN + 64));
            rxBuf.remove(0);
            rxDropUntilNewline = true;
          }
        }
        nl = rxBuf.indexOf('\n');
      }
      if (nl < 0)
        break;

      String line = rxBuf.substring(0, nl);
      rxBuf.remove(0, nl + 1);
      if (line.isEmpty())
        continue;

      unsigned long rxId = 0;
      bool rxResult = false;
      stratum_method result = parse_mining_method(line, &rxId, &rxResult);
      switch (result)
      {
          case MINING_NOTIFY:         {
                                      mining_job newJob;
                                      if (parse_mining_notify(line, newJob))
                                      {
                                          HW_SHA_REQUEST(); // S3: the hashes below need the engine the HW miner is holding
                                          const String prevExtranonce2 = mWorker.extranonce2;
                                          miner_data newMiner = calculateMiningData(mWorker, newJob);
                                          if (!newMiner.valid)
                                          {
                                            mWorker.extranonce2 = prevExtranonce2;
                                            Serial.println("[STRATUM] New job could not be prepared, still mining the previous one");
                                            break;
                                          }
                                          mJob = newJob;
                                          mMiner = newMiner;

                                          templates++;
                                          s_poolFailures = 0;
                                          mMonitor.NerdStatus = NM_hashing;

                                          last_job_time = millis();
                                          mLastTXtoPool = last_job_time;

                                          memset(mMiner.bytearray_blockheader + 80, 0, 128 - 80);
                                          mMiner.bytearray_blockheader[80] = 0x80;
                                          mMiner.bytearray_blockheader[126] = 0x02;
                                          mMiner.bytearray_blockheader[127] = 0x80;

                                          nerd_mids(diget_mid, mMiner.bytearray_blockheader);
                                          nerd_sha256_bake(diget_mid, mMiner.bytearray_blockheader + 64, bake);

                                          #ifdef HARDWARE_SHA265
                                          #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
                                            g_hwShaMutex.lock();
                                            esp_sha_acquire_hardware();
                                            sha_hal_hash_block(SHA2_256, mMiner.bytearray_blockheader, 64 / 4, true);
                                            sha_hal_read_digest(SHA2_256, hw_midstate);
                                            esp_sha_release_hardware();
                                            g_hwShaMutex.unlock();
                                          #endif
                                          #endif

                                          #if defined(HARDWARE_SHA265) && defined(CONFIG_IDF_TARGET_ESP32)
                                          for (int i = 0; i < 32; ++i)
                                            ((uint32_t *)sha_buffer_swap)[i] = __builtin_bswap32(((const uint32_t *)(mMiner.bytearray_blockheader))[i]);
                                          #endif

                                          #ifdef RANDOM_NONCE
                                          nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                          #else
                                            #ifdef I2C_SLAVE
                                            if (!i2c_slave_vector.empty())
                                              nonce_pool = 0x10000000u;
                                            else
                                            #endif
                                              nonce_pool = 0xDA54E700u;
                                          #endif
                                          nonce_pool &= ~0xFFu; // low byte 0 so swNonce (<0x80) and hwNonce (>=0x80) never collide

                                          {
                                            std::lock_guard<std::mutex> lock(s_job_mutex);
                                            job_pool++;
                                            const uint8_t nextSlot = (s_activeJobSlot == 0) ? 1 : 0;
                                            ActiveJob &nj = s_jobs[nextSlot];
                                            nj.id = job_pool;
                                            nj.nonce_base = nonce_pool;
                                            nj.difficulty = currentPoolDifficulty;
                                            memcpy(nj.sha_buffer, mMiner.bytearray_blockheader, sizeof(nj.sha_buffer));
                                            memcpy(nj.midstate, diget_mid, sizeof(nj.midstate));
                                            memcpy(nj.bake, bake, sizeof(nj.bake));
                                            #ifdef HARDWARE_SHA265
                                            #if defined(CONFIG_IDF_TARGET_ESP32)
                                            memcpy(nj.sha_buffer_swap, sha_buffer_swap, sizeof(nj.sha_buffer_swap));
                                            #elif defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
                                            memcpy(nj.hw_midstate, hw_midstate, sizeof(nj.hw_midstate));
                                            #endif
                                            #endif
                                            s_swNextIndex.store(0, std::memory_order_relaxed);
                                            #ifdef HARDWARE_SHA265
                                            s_hwNextIndex.store(0, std::memory_order_relaxed);
                                            #endif
                                            s_activeJobSlot = nextSlot;
                                            s_working_current_job_id = job_pool & 0xFF;
                                            #if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
                                            s_pl_ctx.run = 0; // stop the HW loop mid-chunk right now
                                            #endif
                                          }
                                          #ifdef I2C_SLAVE
                                          i2c_feed_slaves(i2c_slave_vector, job_pool & 0xFF, 0x20, currentPoolDifficulty, mMiner.bytearray_blockheader);
                                          #endif
                                      } else
                                      {
                                        Serial.println("Parsing error, need restart");
                                        resetSocket();
                                        MiningJobStop(job_pool, s_submition_map);
                                      }
                                      }
                                      break;
          case MINING_SET_DIFFICULTY: if (parse_mining_set_difficulty(line, currentPoolDifficulty))
                                      {
                                        setPoolDifficulty(currentPoolDifficulty);
                                        std::lock_guard<std::mutex> lock(s_job_mutex);
                                        if (s_activeJobSlot < 2)
                                          s_jobs[s_activeJobSlot].difficulty = currentPoolDifficulty;
                                      }
                                      break;
          case STRATUM_SUCCESS:       {
                                        if (s_authId != 0 && rxId == s_authId)
                                        {
                                          s_authId = 0;
                                          s_authorized = rxResult ? 1 : 0;
                                          if (s_authorized == 1)
                                            Serial.println("[STRATUM] Worker authorized by the pool");
                                          else
                                            Serial.printf("[STRATUM] The pool REFUSED the worker \"%s\": check the BTC address in the settings\n", mWorker.wName);
                                          break;
                                        }
                                        if (rxId != 0)
                                        {
                                          for (int k = 0; k < 8; ++k)
                                          {
                                            if (s_submitSentRing[k].id == rxId)
                                            {
                                              s_lastPingMs = (int32_t)(millis() - s_submitSentRing[k].sentMs);
                                              s_submitSentRing[k].id = 0;
                                              break;
                                            }
                                          }
                                        }
                                        auto itt = s_submition_map.find(rxId);
                                        if (itt != s_submition_map.end())
                                        {
                                          const bool accepted = rxResult;
                                          if (accepted)
                                            s_sharesAccepted++;
                                          else
                                            rejects++; // "result":false without an error

                                          updateBestDiff(itt->second->diff);
                                          if (accepted || itt->second->isValid)
                                            recordNotableShare(itt->second->diff, itt->second->isValid);
                                          if (itt->second->is32bit && !accepted)
                                            shares32Rejected++;
                                          if (itt->second->is32bit && accepted)
                                          {
                                            shares++;
                                            g_shareFoundEvents++;
                                          }
                                          if (itt->second->isValid)
                                          {
                                            Serial.println("CONGRATULATIONS! Valid block found");
                                            valids++;
                                            g_blockFoundEvents++;
                                          }
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          case STRATUM_PARSE_ERROR:   {
                                        if (s_authId != 0 && rxId == s_authId)
                                        {
                                          s_authId = 0;
                                          s_authorized = 0;
                                          Serial.printf("[STRATUM] The pool REFUSED the worker \"%s\": check the BTC address in the settings\n", mWorker.wName);
                                          break;
                                        }
                                        auto itt = s_submition_map.find(rxId);
                                        if (itt != s_submition_map.end())
                                        {
                                          Serial.printf("Refuse submition %lu\n", rxId);
                                          rejects++;
                                          if (itt->second->is32bit)
                                            shares32Rejected++;
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          default:                    Serial.println("  Parsed JSON: unknown"); break;
      }
    }

    s_stratumConnected = client.connected();

    std::list<std::shared_ptr<JobResult>> job_result_list;
    #ifdef I2C_SLAVE
    if (!i2c_slave_vector.empty() && job_pool != 0xFFFFFFFF)
    {
      i2c_hit_slaves(i2c_slave_vector);
      vTaskDelay(5 / portTICK_PERIOD_MS);
      uint32_t nonces_done = 0;
      std::vector<uint32_t> nonce_vector = i2c_harvest_slaves(i2c_slave_vector, job_pool & 0xFF, nonces_done);
      addHashes(nonces_done);
      uint8_t i2c_tail[16];
      memcpy(i2c_tail, mMiner.bytearray_blockheader + 64, sizeof(i2c_tail));
      for (size_t n = 0; n < nonce_vector.size(); ++n)
      {
        std::shared_ptr<JobResult> result = std::make_shared<JobResult>();
        ((uint32_t *)(i2c_tail + 12))[0] = nonce_vector[n];
        if (nerd_sha256d_baked(diget_mid, i2c_tail, bake, result->hash))
        {
          result->id = job_pool;
          result->nonce = nonce_vector[n];
          result->difficulty = diff_from_target(result->hash);
          job_result_list.push_back(result);
        }
      }
    }
    #endif

    if (job_pool != 0xFFFFFFFF)
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      job_result_list.splice(job_result_list.end(), s_job_result_list);
    }

    while (!job_result_list.empty())
    {
      std::shared_ptr<JobResult> res = job_result_list.front();
      job_result_list.pop_front();

      updateBestDiff(res->difficulty);
      if (res->difficulty > currentPoolDifficulty && job_pool == res->id)
      {
        if (!client.connected())
        {
          s_stratumConnected = false;
          break;
        }
        unsigned long sumbit_id = 0;
        tx_mining_submit(client, mWorker, mJob, res->nonce, sumbit_id);
        const uint32_t sentNow = millis();
        mLastTXtoPool = sentNow;
        s_submitSentRing[s_submitSentHead] = {sumbit_id, sentNow};
        s_submitSentHead = (s_submitSentHead + 1) & 7;

        Serial.print("   - Current diff share: "); Serial.println(res->difficulty, 12);
        Serial.print("   - Current pool diff : "); Serial.println(currentPoolDifficulty, 12);
        Serial.print("   - TX SHARE: ");
        for (size_t i = 0; i < 32; i++)
            Serial.printf("%02x", res->hash[i]);
        Serial.println("");

        std::shared_ptr<Submition> submition = std::make_shared<Submition>();
        submition->diff = res->difficulty;
        submition->is32bit = (res->hash[29] == 0 && res->hash[28] == 0);
        if (submition->is32bit)
        {
          submition->isValid = checkValid(res->hash, mJob.nbits.c_str());
        } else
          submition->isValid = false;

        s_submition_map.insert(std::make_pair(sumbit_id, submition));
        if (s_submition_map.size() > 32)
          s_submition_map.erase(s_submition_map.begin());
      }
    }

    // Wake in <1 ms when a miner pushes a share, or poll socket every <=10 ms
    const bool hasSocketData = client.connected() && (client.available() > 0 || rxBuf.indexOf('\n') >= 0);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hasSocketData ? 0 : 10));
  }
}

//////////////////THREAD CALLS///////////////////

void minerWorkerSw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerSw Task!\n", miner_id);

  uint8_t hash[32];
  uint32_t midstate[8];
  uint32_t bake[16];
  uint8_t tail[16];
  uint32_t wdt_counter = 0;
  #ifdef DEBUG_MINING
  volatile uint32_t *myDebugCounter = &debugHashesSw[miner_id & 1];
  #endif

  while (1)
  {
    while (g_otaUpdating) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t job_id = 0, nonce_base = 0, sw_idx = 0;
    double job_difficulty = 0.0;
    bool has_job = false;
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      const uint8_t slot = s_activeJobSlot;
      if (slot < 2)
      {
        const uint32_t claimed = s_swNextIndex.fetch_add(NONCE_PER_JOB_SW, std::memory_order_relaxed);
        if (claimed < 0x80000000u - NONCE_PER_JOB_SW)
        {
          const ActiveJob &aj = s_jobs[slot];
          job_id = aj.id;
          nonce_base = aj.nonce_base;
          job_difficulty = aj.difficulty;
          memcpy(midstate, aj.midstate, sizeof(midstate));
          memcpy(bake, aj.bake, sizeof(bake));
          memcpy(tail, aj.sha_buffer + 64, sizeof(tail));
          sw_idx = claimed;
          has_job = true;
        }
      }
    }

    if (has_job)
    {
      const uint8_t job_in_work = job_id & 0xFF;
      uint32_t done = NONCE_PER_JOB_SW;
      for (uint32_t n = 0; n < NONCE_PER_JOB_SW; ++n)
      {
        const uint32_t nonce = swNonce(nonce_base, sw_idx + n);
        ((uint32_t *)(tail + 12))[0] = nonce;
        #ifdef DEBUG_MINING
        (*myDebugCounter)++;
        #endif
#if defined(NERD_SW_ASM)
        if (nerd_sha256d_prefilter(midstate, tail, bake) &&
            nerd_sha256d_baked(midstate, tail, bake, hash))
#else
        if (nerd_sha256d_baked(midstate, tail, bake, hash))
#endif
        {
          const double diff_hash = diff_from_target(hash);
          updateBestDiff(diff_hash);
          if (diff_hash > job_difficulty)
            submitShareResult(job_id, nonce, diff_hash, hash);
        }

        if ((uint16_t)(n & 0xFF) == 0 && s_working_current_job_id != job_in_work)
        {
          done = n + 1;
          break;
        }
      }
      addHashes(done);
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#ifdef HARDWARE_SHA265

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    REG_WRITE(&reg_addr_buf[0], data_words[0]);
    REG_WRITE(&reg_addr_buf[1], data_words[1]);
    REG_WRITE(&reg_addr_buf[2], data_words[2]);
    REG_WRITE(&reg_addr_buf[3], nonce);
    REG_WRITE(&reg_addr_buf[4], 0x00000080);
    REG_WRITE(&reg_addr_buf[5], 0x00000000);
    REG_WRITE(&reg_addr_buf[6], 0x00000000);
    REG_WRITE(&reg_addr_buf[7], 0x00000000);
    REG_WRITE(&reg_addr_buf[8], 0x00000000);
    REG_WRITE(&reg_addr_buf[9], 0x00000000);
    REG_WRITE(&reg_addr_buf[10], 0x00000000);
    REG_WRITE(&reg_addr_buf[11], 0x00000000);
    REG_WRITE(&reg_addr_buf[12], 0x00000000);
    REG_WRITE(&reg_addr_buf[13], 0x00000000);
    REG_WRITE(&reg_addr_buf[14], 0x00000000);
    REG_WRITE(&reg_addr_buf[15], 0x80020000);
}

static inline void nerd_sha_ll_fill_text_block_sha256_inter()
{
  uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

  REG_WRITE(&reg_addr_buf[0], REG_READ(SHA_H_BASE + 0 * 4));
  REG_WRITE(&reg_addr_buf[1], REG_READ(SHA_H_BASE + 1 * 4));
  REG_WRITE(&reg_addr_buf[2], REG_READ(SHA_H_BASE + 2 * 4));
  REG_WRITE(&reg_addr_buf[3], REG_READ(SHA_H_BASE + 3 * 4));
  REG_WRITE(&reg_addr_buf[4], REG_READ(SHA_H_BASE + 4 * 4));
  REG_WRITE(&reg_addr_buf[5], REG_READ(SHA_H_BASE + 5 * 4));
  REG_WRITE(&reg_addr_buf[6], REG_READ(SHA_H_BASE + 6 * 4));
  REG_WRITE(&reg_addr_buf[7], REG_READ(SHA_H_BASE + 7 * 4));

  REG_WRITE(&reg_addr_buf[8], 0x00000080);
  REG_WRITE(&reg_addr_buf[9], 0x00000000);
  REG_WRITE(&reg_addr_buf[10], 0x00000000);
  REG_WRITE(&reg_addr_buf[11], 0x00000000);
  REG_WRITE(&reg_addr_buf[12], 0x00000000);
  REG_WRITE(&reg_addr_buf[13], 0x00000000);
  REG_WRITE(&reg_addr_buf[14], 0x00000000);
  REG_WRITE(&reg_addr_buf[15], 0x00010000);
}

static inline void nerd_sha_ll_read_digest(void* ptr)
{
  ((uint32_t*)ptr)[0] = REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = REG_READ(SHA_H_BASE + 6 * 4);
  ((uint32_t*)ptr)[7] = REG_READ(SHA_H_BASE + 7 * 4);
}

static inline bool nerd_sha_ll_read_digest_if(void* ptr)
{
  uint32_t last = REG_READ(SHA_H_BASE + 7 * 4);
  if ((uint16_t)(last >> 16) != 0)
    return false;

  ((uint32_t*)ptr)[7] = last;
  ((uint32_t*)ptr)[0] = REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = REG_READ(SHA_H_BASE + 6 * 4);
  return true;
}

static inline void nerd_sha_ll_write_digest(void *digest_state)
{
    uint32_t *digest_state_words = (uint32_t *)digest_state;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_H_BASE);

    REG_WRITE(&reg_addr_buf[0], digest_state_words[0]);
    REG_WRITE(&reg_addr_buf[1], digest_state_words[1]);
    REG_WRITE(&reg_addr_buf[2], digest_state_words[2]);
    REG_WRITE(&reg_addr_buf[3], digest_state_words[3]);
    REG_WRITE(&reg_addr_buf[4], digest_state_words[4]);
    REG_WRITE(&reg_addr_buf[5], digest_state_words[5]);
    REG_WRITE(&reg_addr_buf[6], digest_state_words[6]);
    REG_WRITE(&reg_addr_buf[7], digest_state_words[7]);
}

static inline void nerd_sha_hal_wait_idle()
{
    while (REG_READ(SHA_BUSY_REG))
    {}
}

#if defined(PIPELINED_S3_MINING) && defined(CONFIG_IDF_TARGET_ESP32S3)
// Overlapped ESP32-S3 mining loop (build flag PIPELINED_S3_MINING).
static uint32_t __attribute__((noinline, optimize("O2")))
s3_pl_mine(const uint32_t *mid, const uint32_t *tail, uint32_t nonce, uint32_t count, bool *hit)
{
  volatile uint32_t *const text = (volatile uint32_t *)SHA_TEXT_BASE;
  volatile uint32_t *const h = (volatile uint32_t *)SHA_H_BASE;
  volatile uint32_t *const busy = (volatile uint32_t *)SHA_BUSY_REG;
  const uint32_t m0 = mid[0], m1 = mid[1], m2 = mid[2], m3 = mid[3], m4 = mid[4], m5 = mid[5], m6 = mid[6],
                 m7 = mid[7];
  const uint32_t t0 = tail[0], t1 = tail[1], t2 = tail[2];

  // Second block of the header for the first nonce
  text[0] = t0;
  text[1] = t1;
  text[2] = t2;
  text[3] = nonce;
  text[4] = 0x00000080;
  for (int i = 5; i < 15; ++i)
    text[i] = 0;
  text[15] = 0x80020000;

  for (uint32_t i = 0; i < count; ++i)
  {
    h[0] = m0;
    h[1] = m1;
    h[2] = m2;
    h[3] = m3;
    h[4] = m4;
    h[5] = m5;
    h[6] = m6;
    h[7] = m7;
    REG_WRITE(SHA_CONTINUE_REG, 1); // first hash
    text[8] = 0x00000080;           // meanwhile: the fixed words of the second hash's block
    text[15] = 0x00010000;          // (9..14 are zero in both blocks)
    while (*busy)
    {
    }
    text[0] = h[0];
    text[1] = h[1];
    text[2] = h[2];
    text[3] = h[3];
    text[4] = h[4];
    text[5] = h[5];
    text[6] = h[6];
    text[7] = h[7];
    REG_WRITE(SHA_START_REG, 1); // second hash
    text[0] = t0;                // meanwhile: the header block for the next nonce
    text[1] = t1;
    text[2] = t2;
    text[3] = nonce + i + 1;
    text[4] = 0x00000080;
    text[5] = 0;
    text[6] = 0;
    text[7] = 0;
    text[8] = 0;
    text[15] = 0x80020000;
    while (*busy)
    {
    }
    if ((h[7] >> 16) == 0)
    {
      *hit = true;
      return i + 1;
    }
  }
  *hit = false;
  return count;
}

static bool s3_pl_selftest()
{
  static const uint32_t expected[] = {0x642c, 0x66ce, 0x731b, 0x7697, 0x1478f};
  const uint32_t first = 0x1000, last = first + 131072;
  uint8_t hdr[80];
  uint32_t mid[8];
  for (int i = 0; i < 80; ++i)
    hdr[i] = (uint8_t)(i * 37 + 11);

  g_hwShaMutex.lock();
  esp_sha_acquire_hardware();
  sha_hal_hash_block(SHA2_256, hdr, 64 / 4, true);
  sha_hal_read_digest(SHA2_256, mid);
  REG_WRITE(SHA_MODE_REG, SHA2_256);
  size_t found = 0;
  bool ok = true;
  uint32_t n = first;
  while (n != last)
  {
    bool hit = false;
    n += s3_pl_mine(mid, (const uint32_t *)(hdr + 64), n, last - n, &hit);
    if (!hit)
      break;
    if (found >= sizeof(expected) / sizeof(expected[0]) || expected[found] != n - 1)
    {
      ok = false;
      break;
    }
    found++;
  }
  esp_sha_release_hardware();
  g_hwShaMutex.unlock();
  return ok && found == sizeof(expected) / sizeof(expected[0]);
}
#endif // PIPELINED_S3_MINING

//#define VALIDATION
void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHw Task!\n", miner_id);

  uint8_t hash[32];
  uint32_t digest_mid[8];
  uint32_t sw_mid[8];
  uint32_t sw_bake[16];
  uint8_t sha_buffer[64];
  uint32_t wdt_counter = 0;

#if defined(PIPELINED_S3_MINING) && defined(CONFIG_IDF_TARGET_ESP32S3)
  const bool s3_pl_ok = s3_pl_selftest() || s3_pl_selftest() || s3_pl_selftest();
  Serial.printf("[MINER] HW SHA loop: %s\n", s3_pl_ok ? "overlapped" : "stock (the overlapped loop failed its self-test)");
#endif

#ifdef VALIDATION
  uint8_t doubleHash[32];
#endif

  while (1)
  {
    while (g_otaUpdating) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t job_id = 0, nonce_base = 0, hw_idx = 0;
    double job_difficulty = 0.0;
    bool has_job = false;
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      const uint8_t slot = s_activeJobSlot;
      if (slot < 2)
      {
        const uint32_t claimed = s_hwNextIndex.fetch_add(NONCE_PER_JOB_HW, std::memory_order_relaxed);
        if (claimed < 0x80000000u - NONCE_PER_JOB_HW)
        {
          const ActiveJob &aj = s_jobs[slot];
          job_id = aj.id;
          nonce_base = aj.nonce_base;
          job_difficulty = aj.difficulty;
          memcpy(digest_mid, aj.hw_midstate, sizeof(digest_mid));
          memcpy(sw_mid, aj.midstate, sizeof(sw_mid));
          memcpy(sw_bake, aj.bake, sizeof(sw_bake));
          memcpy(sha_buffer, aj.sha_buffer + 64, sizeof(sha_buffer));
          hw_idx = claimed;
          has_job = true;
        }
      }
    }

    if (has_job)
    {
      const uint8_t job_in_work = job_id & 0xFF;

      g_hwShaMutex.lock();
      esp_sha_acquire_hardware();
      REG_WRITE(SHA_MODE_REG, SHA2_256);

#if defined(PIPELINED_S3_MINING) && defined(CONFIG_IDF_TARGET_ESP32S3)
      if (s3_pl_ok)
      {
        uint32_t pos = 0;
        uint32_t end_pos = NONCE_PER_JOB_HW;
        uint32_t reported = 0;
        while (pos != end_pos)
        {
          // Keep each run inside a single [..0x80 .. ..0xFF] 128-nonce window
          // so consecutive native nonces in s3_pl_mine never cross into SW's <0x80 half.
          uint32_t want = 128u - ((hw_idx + pos) & 0x7Fu);
          if (want > end_pos - pos)
            want = end_pos - pos;
          bool hit = false;
          pos += s3_pl_mine(digest_mid, (const uint32_t *)sha_buffer, hwNonce(nonce_base, hw_idx + pos), want, &hit);

          if (hit)
          {
            #ifdef DEBUG_MINING
            debugPipelinedHits++;
            #endif
            const uint32_t cand = hwNonce(nonce_base, hw_idx + pos - 1);
            ((uint32_t *)(sha_buffer + 12))[0] = cand;
            uint8_t sw_hash[32];
            bool confirmed = nerd_sha256d_baked(sw_mid, sha_buffer, sw_bake, sw_hash);
            #ifdef DEBUG_MINING
            if (confirmed)
              debugPipelinedRealHits++;
            else
              debugPipelinedHwSwMismatch++;
            #endif
            const double cand_diff = confirmed && isSha256Valid(sw_hash) ? diff_from_target(sw_hash) : 0.0;
            updateBestDiff(cand_diff);
            if (cand_diff > job_difficulty)
            {
              #ifdef DEBUG_MINING
              debugPipelinedAccepted++;
              #endif
              submitShareResult(job_id, cand, cand_diff, sw_hash);
            }
          }

          if (s_working_current_job_id != job_in_work || s_tlsPauseRequested)
          {
            end_pos = pos;
          }
          else if (pos != end_pos && g_hwShaWanted.load() != 0)
          {
            esp_sha_release_hardware();
            g_hwShaMutex.unlock();
            while (g_hwShaWanted.load() != 0)
              vTaskDelay(1);
            g_hwShaMutex.lock();
            esp_sha_acquire_hardware();
            REG_WRITE(SHA_MODE_REG, SHA2_256);
            continue;
          }
          else if (pos != end_pos && pos - reported >= 32768)
          {
            addHashes(pos - reported);
            reported = pos;
          }
        }
        addHashes(pos - reported);
        #ifdef DEBUG_MINING
        debugHashesHw += pos;
        #endif
      }
      else
#endif
      {
        uint32_t done = NONCE_PER_JOB_HW;
        for (uint32_t i = 0; i < NONCE_PER_JOB_HW; ++i)
        {
          const uint32_t n = hwNonce(nonce_base, hw_idx + i);
          #ifdef DEBUG_MINING
          debugHashesHw++;
          #endif
          nerd_sha_ll_write_digest(digest_mid);
          nerd_sha_ll_fill_text_block_sha256(sha_buffer, n);
          REG_WRITE(SHA_CONTINUE_REG, 1);

          sha_ll_load(SHA2_256);
          nerd_sha_hal_wait_idle();
          nerd_sha_ll_fill_text_block_sha256_inter();
          REG_WRITE(SHA_START_REG, 1);
          sha_ll_load(SHA2_256);
          nerd_sha_hal_wait_idle();
          if (nerd_sha_ll_read_digest_if(hash))
          {
#ifdef VALIDATION
            ((uint32_t *)(sha_buffer + 12))[0] = n;
            nerd_sha256d_baked(sw_mid, sha_buffer, sw_bake, doubleHash);
            for (int k = 0; k < 32; ++k)
            {
              if (hash[k] != doubleHash[k])
              {
                Serial.println("***HW sha256 esp32s3 bug detected***");
                break;
              }
            }
#endif
            const double diff_hash = diff_from_target(hash);
            if (isSha256Valid(hash))
            {
              updateBestDiff(diff_hash);
              if (diff_hash > job_difficulty)
                submitShareResult(job_id, n, diff_hash, hash);
            }
          }
          if ((uint8_t)(i & 0xFF) == 0 &&
              (s_working_current_job_id != job_in_work || s_tlsPauseRequested))
          {
            done = i + 1;
            break;
          }
        }
        addHashes(done);
      }

      esp_sha_release_hardware();
      g_hwShaMutex.unlock();
      if (s_tlsPauseRequested)
      {
        s_tlsPauseRequested = false;
        vTaskDelay(1);
      }
      #if defined(PIPELINED_S3_MINING) && defined(CONFIG_IDF_TARGET_ESP32S3)
      else if (s3_pl_ok)
        vTaskDelay(1);
      #endif
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#endif  // ESP32S2 || ESP32S3 || ESP32C3

#if defined(CONFIG_IDF_TARGET_ESP32)

static inline bool nerd_sha_ll_read_digest_swap_if(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  uint32_t fin = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
  if ((uint32_t)(fin & 0xFFFF) != 0)
  {
    DPORT_INTERRUPT_RESTORE();
    return false;
  }
  ((uint32_t*)ptr)[7] = __builtin_bswap32(fin);
  ((uint32_t*)ptr)[0] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 0 * 4));
  ((uint32_t*)ptr)[1] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 1 * 4));
  ((uint32_t*)ptr)[2] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 2 * 4));
  ((uint32_t*)ptr)[3] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 3 * 4));
  ((uint32_t*)ptr)[4] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 4 * 4));
  ((uint32_t*)ptr)[5] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 5 * 4));
  ((uint32_t*)ptr)[6] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 6 * 4));
  DPORT_INTERRUPT_RESTORE();
  return true;
}

static inline void nerd_sha_ll_read_digest(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 6 * 4);
  ((uint32_t*)ptr)[7] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
  DPORT_INTERRUPT_RESTORE();
}

static inline void nerd_sha_hal_wait_idle()
{
    while (DPORT_REG_READ(SHA_256_BUSY_REG))
    {}
}

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = data_words[3];
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
    reg_addr_buf[8]  = data_words[8];
    reg_addr_buf[9]  = data_words[9];
    reg_addr_buf[10] = data_words[10];
    reg_addr_buf[11] = data_words[11];
    reg_addr_buf[12] = data_words[12];
    reg_addr_buf[13] = data_words[13];
    reg_addr_buf[14] = data_words[14];
    reg_addr_buf[15] = data_words[15];
}

static inline void nerd_sha_ll_fill_text_block_sha256_upper(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = __builtin_bswap32(nonce);
    reg_addr_buf[4]  = 0x80000000;
    reg_addr_buf[5]  = 0x00000000;
    reg_addr_buf[6]  = 0x00000000;
    reg_addr_buf[7]  = 0x00000000;
    reg_addr_buf[8]  = 0x00000000;
    reg_addr_buf[9]  = 0x00000000;
    reg_addr_buf[10] = 0x00000000;
    reg_addr_buf[11] = 0x00000000;
    reg_addr_buf[12] = 0x00000000;
    reg_addr_buf[13] = 0x00000000;
    reg_addr_buf[14] = 0x00000000;
    reg_addr_buf[15] = 0x00000280;
}

static inline void nerd_sha_ll_fill_text_block_sha256_double()
{
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[8]  = 0x80000000;
    reg_addr_buf[9]  = 0x00000000;
    reg_addr_buf[10] = 0x00000000;
    reg_addr_buf[11] = 0x00000000;
    reg_addr_buf[12] = 0x00000000;
    reg_addr_buf[13] = 0x00000000;
    reg_addr_buf[14] = 0x00000000;
    reg_addr_buf[15] = 0x00000100;
}

#ifdef PIPELINED_ASM_MINING
typedef uint32_t (*pl_mine_fn)(volatile uint32_t *sha_base, pl_ctx_t *ctx);

// src/pipelined_hw_sha_classic_v2.S: counted delays, no status polling.
extern "C" uint32_t pipelined_hw_mine_classic_v2(volatile uint32_t *sha_base, pl_ctx_t *ctx);
extern "C" uint32_t pipelined_hw_mine_classic_v2_margin(volatile uint32_t *sha_base, pl_ctx_t *ctx);
extern "C" uint32_t pipelined_hw_mine_classic_v2_wide(volatile uint32_t *sha_base, pl_ctx_t *ctx);

#if defined(HW_SHA256_TEST) || defined(SHA_BENCH)
// src/pipelined_hw_sha_classic.cpp: legacy BUSY-polling loop (kept for bench/test only; saves 293 B IRAM).
extern "C" bool pipelined_hw_mine_classic(
    volatile uint32_t *sha_base,
    const uint32_t *header_swapped,
    uint32_t *nonce_swapped_inout,
    volatile uint32_t *hash_count_low,
    volatile bool *mining_flag,
    uint32_t iter_budget);
extern "C" void pipelined_hw_mine_classic_reinit(void);

static uint32_t pl_mine_legacy(volatile uint32_t *sha_base, pl_ctx_t *ctx)
{
  uint32_t done = 0;
  bool hit = pipelined_hw_mine_classic(sha_base, ctx->hdr, &ctx->nonce, &done,
                                       (volatile bool *)&ctx->run, ctx->budget);
  ctx->budget -= done;
  if (hit)
    pipelined_hw_mine_classic_reinit();
  else if (ctx->budget == 0)
    ctx->run = 1;
  return hit;
}
#endif

static uint32_t pl_mine_plain(volatile uint32_t *sha_base, pl_ctx_t *ctx)
{
  (void)sha_base;
  uint8_t hash[32];
  while (ctx->run && ctx->budget > 0)
  {
    nerd_sha_ll_fill_text_block_sha256(ctx->hdr);
    sha_ll_start_block(SHA2_256);

    nerd_sha_hal_wait_idle();
    nerd_sha_ll_fill_text_block_sha256_upper(ctx->hdr + 16, __builtin_bswap32(ctx->nonce));
    sha_ll_continue_block(SHA2_256);

    nerd_sha_hal_wait_idle();
    sha_ll_load(SHA2_256);

    nerd_sha_hal_wait_idle();
    nerd_sha_ll_fill_text_block_sha256_double();
    sha_ll_start_block(SHA2_256);

    nerd_sha_hal_wait_idle();
    sha_ll_load(SHA2_256);

    ctx->nonce++;
    ctx->budget--;
    if (nerd_sha_ll_read_digest_swap_if(hash))
      return 1;
  }
  return 0;
}

static const struct { pl_mine_fn fn; const char *name; } s_pl_loops[] = {
  {pipelined_hw_mine_classic_v2,        "fast"},
  {pipelined_hw_mine_classic_v2_margin, "fast, +2 margins"},
  {pipelined_hw_mine_classic_v2_wide,   "fast, wide margins"},
#if defined(HW_SHA256_TEST) || defined(SHA_BENCH)
  {pl_mine_legacy,                      "polled"},
#endif
  {pl_mine_plain,                       "plain"},
};
#define PL_LOOP_COUNT (sizeof(s_pl_loops) / sizeof(s_pl_loops[0]))
#define PL_LOOP_PLAIN (PL_LOOP_COUNT - 1)
static unsigned s_pl_loop = 0;

static bool pl_selftest(pl_mine_fn fn)
{
  static const uint32_t expected[] = {0xa94c, 0xe496, 0x1f476, 0x302f0, 0x35af9, 0x3cf5c};
  const uint32_t first = 0x1000;
  static pl_ctx_t c;
  for (uint32_t i = 0; i < 19; ++i)
    c.hdr[i] = 0x9E3779B9u * (i + 1);
  c.nonce = first;
  c.budget = 262144;
  c.pad = 0x80000000u;
  c.run = 1;
  size_t found = 0;
  while (c.budget > 0 && fn((volatile uint32_t *)SHA_TEXT_BASE, &c))
  {
    if (found >= sizeof(expected) / sizeof(expected[0]) || c.nonce - 1 - first != expected[found])
      return false;
    found++;
  }
  return found == sizeof(expected) / sizeof(expected[0]) && c.budget == 0;
}

static void pl_select_loop()
{
  g_hwShaMutex.lock();
  esp_sha_lock_engine(SHA2_256);
  periph_module_reset(PERIPH_SHA_MODULE);
  unsigned n = 0;
  const unsigned chip_rev = efuse_hal_get_major_chip_version();
  if (chip_rev < 3)
  {
    Serial.printf("[MINER] chip revision %u: the pipelined HW SHA loops need revision 3\n", chip_rev);
    n = PL_LOOP_PLAIN;
  }
  while (n + 1 < PL_LOOP_COUNT && !pl_selftest(s_pl_loops[n].fn) && !pl_selftest(s_pl_loops[n].fn) &&
         !pl_selftest(s_pl_loops[n].fn))
  {
    Serial.printf("[MINER] HW SHA loop '%s' failed its self-test\n", s_pl_loops[n].name);
    n++;
  }
  #ifdef PL_FORCE_LOOP
  n = PL_FORCE_LOOP;
  #endif
  s_pl_loop = n;
  esp_sha_unlock_engine(SHA2_256);
  g_hwShaMutex.unlock();
  Serial.printf("[MINER] HW SHA loop: %s\n", s_pl_loops[n].name);
}

static void pl_health(bool confirmed)
{
  static uint32_t hits = 0, unconfirmed = 0;
  hits++;
  if (!confirmed)
    unconfirmed++;
  if (hits < 256)
    return;
  if (unconfirmed >= 8 && s_pl_loop + 1 < PL_LOOP_COUNT)
  {
    s_pl_loop++;
    Serial.printf("[MINER] %u of %u HW candidates not confirmed; switching HW SHA loop to: %s\n",
                  unconfirmed, hits, s_pl_loops[s_pl_loop].name);
  }
  hits = unconfirmed = 0;
}
#endif // PIPELINED_ASM_MINING

void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHwEsp32D Task!\n", miner_id);

  #ifndef PIPELINED_ASM_MINING
  uint8_t hash[32];
  #endif
  uint8_t sha_buffer[128];
  uint32_t sw_mid[8];
  uint32_t sw_bake[16];
  uint8_t sw_tail[16];

  #ifdef PIPELINED_ASM_MINING
  uint32_t pipelined_swapped_cursor = 0;
  uint32_t pipelined_last_job_id = 0xFFFFFFFF;
  pl_select_loop();
  #endif

  #ifdef DEBUG_MINING
  int64_t debugChunkEndUs = 0;
  #endif

  while (1)
  {
    while (g_otaUpdating) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t job_id = 0, nonce_base = 0, hw_idx = 0;
    double job_difficulty = 0.0;
    bool has_job = false;
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      const uint8_t slot = s_activeJobSlot;
      if (slot < 2)
      {
        const uint32_t claimed = s_hwNextIndex.fetch_add(NONCE_PER_JOB_HW, std::memory_order_relaxed);
        if (claimed < 0x7F000000u - NONCE_PER_JOB_HW)
        {
          const ActiveJob &aj = s_jobs[slot];
          job_id = aj.id;
          nonce_base = aj.nonce_base;
          job_difficulty = aj.difficulty;
          memcpy(sha_buffer, aj.sha_buffer_swap, sizeof(sha_buffer));
          memcpy(sw_mid, aj.midstate, sizeof(sw_mid));
          memcpy(sw_bake, aj.bake, sizeof(sw_bake));
          memcpy(sw_tail, aj.sha_buffer + 64, sizeof(sw_tail));
          hw_idx = claimed;
          has_job = true;
        }
      }
    }

    if (has_job)
    {
      #ifdef DEBUG_MINING
      if (debugChunkEndUs != 0)
      {
        int64_t gap = esp_timer_get_time() - debugChunkEndUs;
        if (gap > 0 && gap < 10000000)
        {
          debugChunkGapUs += (uint32_t)gap;
          debugChunkCount++;
        }
      }
      #endif

      const uint8_t job_in_work = job_id & 0xFF;

      g_hwShaMutex.lock();
      esp_sha_lock_engine(SHA2_256);
#ifdef PIPELINED_ASM_MINING
      {
        if (job_id != pipelined_last_job_id)
        {
          pipelined_last_job_id = job_id;
          // Top bit set -> native low byte >= 0x80, disjoint from swNonce() (< 0x80).
          pipelined_swapped_cursor = __builtin_bswap32(nonce_base) | 0x80000000u;
        }

        memcpy(s_pl_ctx.hdr, sha_buffer, sizeof(s_pl_ctx.hdr));
        s_pl_ctx.nonce = pipelined_swapped_cursor;
        s_pl_ctx.budget = NONCE_PER_JOB_HW;
        s_pl_ctx.pad = 0x80000000u;
        s_pl_ctx.run = 1;
        if (s_working_current_job_id != job_in_work || s_tlsPauseRequested)
          s_pl_ctx.run = 0;

        periph_module_reset(PERIPH_SHA_MODULE);

        while (s_pl_ctx.run && s_pl_ctx.budget > 0)
        {
          if (!s_pl_loops[s_pl_loop].fn((volatile uint32_t *)SHA_TEXT_BASE, &s_pl_ctx))
            break;

          #ifdef DEBUG_MINING
          int64_t hitOverheadStartUs = esp_timer_get_time();
          debugPipelinedHits++;
          #endif

          uint32_t cand_native_pl = __builtin_bswap32(s_pl_ctx.nonce - 1);
          ((uint32_t *)(sw_tail + 12))[0] = cand_native_pl;
          uint8_t sw_hash[32];
          bool confirmed = nerd_sha256d_baked(sw_mid, sw_tail, sw_bake, sw_hash);
          pl_health(confirmed);
          #ifdef DEBUG_MINING
          if (confirmed)
            debugPipelinedRealHits++;
          else
            debugPipelinedHwSwMismatch++;
          #endif

          const double cand_diff = confirmed && isSha256Valid(sw_hash) ? diff_from_target(sw_hash) : 0.0;
          updateBestDiff(cand_diff);
          if (cand_diff > job_difficulty)
          {
            #ifdef DEBUG_MINING
            debugPipelinedAccepted++;
            #endif
            submitShareResult(job_id, cand_native_pl, cand_diff, sw_hash);
          }

          #ifdef DEBUG_MINING
          debugHitOverheadUs += (uint32_t)(esp_timer_get_time() - hitOverheadStartUs);
          debugHitOverheadCount++;
          #endif
        }

        pipelined_swapped_cursor = s_pl_ctx.nonce | 0x80000000u;
        const uint32_t done = NONCE_PER_JOB_HW - s_pl_ctx.budget;
        addHashes(done);
        #ifdef DEBUG_MINING
        debugHashesHw += done;
        #endif
      }
#else
      {
        uint32_t done = NONCE_PER_JOB_HW;
        for (uint32_t i = 0; i < NONCE_PER_JOB_HW; ++i)
        {
          const uint32_t nonce = hwNonce(nonce_base, hw_idx + i);
          #ifdef DEBUG_MINING
          debugHashesHw++;
          #endif
          nerd_sha_ll_fill_text_block_sha256(sha_buffer);
          sha_ll_start_block(SHA2_256);

          nerd_sha_hal_wait_idle();
          nerd_sha_ll_fill_text_block_sha256_upper(sha_buffer + 64, nonce);
          sha_ll_continue_block(SHA2_256);

          nerd_sha_hal_wait_idle();
          sha_ll_load(SHA2_256);

          nerd_sha_hal_wait_idle();
          nerd_sha_ll_fill_text_block_sha256_double();
          sha_ll_start_block(SHA2_256);

          nerd_sha_hal_wait_idle();
          sha_ll_load(SHA2_256);
          if (nerd_sha_ll_read_digest_swap_if(hash))
          {
            const double diff_hash = diff_from_target(hash);
            if (isSha256Valid(hash))
            {
              updateBestDiff(diff_hash);
              if (diff_hash > job_difficulty)
                submitShareResult(job_id, nonce, diff_hash, hash);
            }
          }
          if ((uint8_t)(i & 0xFF) == 0 &&
              (s_working_current_job_id != job_in_work || s_tlsPauseRequested))
          {
            done = i + 1;
            break;
          }
        }
        addHashes(done);
      }
#endif // PIPELINED_ASM_MINING
      esp_sha_unlock_engine(SHA2_256);
      g_hwShaMutex.unlock();
      if (s_tlsPauseRequested)
      {
        s_tlsPauseRequested = false;
        vTaskDelay(1);
      }
      #ifdef DEBUG_MINING
      debugChunkEndUs = esp_timer_get_time();
      #endif
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    esp_task_wdt_reset();
  }
}

#endif  // CONFIG_IDF_TARGET_ESP32

#endif  // HARDWARE_SHA265


#define DELAY 100
#define REDRAW_EVERY 10

// ---------------------------------------------------------------------------
// Saved mining stats: two slots, "statA" / "statB", written in turn.
// ---------------------------------------------------------------------------
#define STATS_MAGIC_V1 0x53524D4EUL // "NMRS": first slot format, no rejected count
#define STATS_MAGIC    0x32524D4EUL // "NMR2": + 32-bit shares the pool refused

struct StatsRecordV1 { // read only (migration)
  uint32_t magic;
  uint32_t seq;
  double best_diff;
  uint32_t Mhashes;
  uint32_t shares;
  uint32_t valids;
  uint32_t templates;
  uint64_t upTime;
  uint32_t crc;
};

struct StatsRecord {
  uint32_t magic;
  uint32_t seq;
  double best_diff;
  uint32_t Mhashes;
  uint32_t shares;           // 32-bit shares accepted
  uint32_t valids;
  uint32_t templates;
  uint32_t shares32Rejected; // 32-bit shares refused
  uint64_t upTime;
  uint32_t crc; // over every field above
};

static std::mutex s_statsMutex;
static uint32_t s_statsSeq = 0;

static uint32_t statsRecordCrc(const StatsRecord &r)
{
  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &r, offsetof(StatsRecord, crc));
  return crc32_finish(crc);
}

static uint32_t statsRecordV1Crc(const StatsRecordV1 &r)
{
  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &r, offsetof(StatsRecordV1, crc));
  return crc32_finish(crc);
}

static bool readStatsSlot(const char *key, StatsRecord &r, bool &missing)
{
  union { StatsRecord v2; StatsRecordV1 v1; } buf;
  size_t size = sizeof(buf);
  const esp_err_t err = nvs_get_blob(stat_handle, key, &buf, &size);
  missing = (err == ESP_ERR_NVS_NOT_FOUND);
  if (err != ESP_OK)
    return false;
  if (size == sizeof(StatsRecord) && buf.v2.magic == STATS_MAGIC && buf.v2.crc == statsRecordCrc(buf.v2)) {
    r = buf.v2;
    return true;
  }
  if (size == sizeof(StatsRecordV1) && buf.v1.magic == STATS_MAGIC_V1 && buf.v1.crc == statsRecordV1Crc(buf.v1)) {
    const StatsRecordV1 old = buf.v1;
    memset(&r, 0, sizeof(r));
    r.magic = STATS_MAGIC;
    r.seq = old.seq;
    r.best_diff = old.best_diff;
    r.Mhashes = old.Mhashes;
    r.shares = old.shares;
    r.valids = old.valids;
    r.templates = old.templates;
    r.shares32Rejected = 0;
    r.upTime = old.upTime;
    return true;
  }
  return false;
}

static bool readLegacyStats()
{
  double nv_best = 0.0;
  uint32_t nv_mhashes = 0, nv_shares = 0, nv_valids = 0, nv_templates = 0, nv_crc = 0;
  uint64_t nv_uptime = 0;
  size_t required_size = sizeof(double);
  if (nvs_get_u32(stat_handle, "crc32", &nv_crc) != ESP_OK)
    return false;
  nvs_get_blob(stat_handle, "best_diff", &nv_best, &required_size);
  nvs_get_u32(stat_handle, "Mhashes", &nv_mhashes);
  nvs_get_u32(stat_handle, "shares", &nv_shares);
  nvs_get_u32(stat_handle, "valids", &nv_valids);
  nvs_get_u32(stat_handle, "templates", &nv_templates);
  nvs_get_u64(stat_handle, "upTime", &nv_uptime);

  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &nv_best, sizeof(nv_best));
  crc = crc32_add(crc, &nv_mhashes, sizeof(nv_mhashes));
  crc = crc32_add(crc, &nv_shares, sizeof(nv_shares));
  crc = crc32_add(crc, &nv_valids, sizeof(nv_valids));
  crc = crc32_add(crc, &nv_templates, sizeof(nv_templates));
  crc = crc32_add(crc, &nv_uptime, sizeof(nv_uptime));
  crc = crc32_finish(crc);
  if (crc != nv_crc)
    return false;

  portENTER_CRITICAL(&s_bestDiffMux);
  best_diff = nv_best;
  portEXIT_CRITICAL(&s_bestDiffMux);
  restoreHashes(nv_mhashes);
  shares = nv_shares;
  valids = nv_valids;
  templates = nv_templates;
  upTime = nv_uptime;
  return true;
}

void restoreStat() {
  if (!Settings.saveStats) {
    s_statsLoaded = true;
    return;
  }
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.printf("[MONITOR] NVS partition is full or has invalid version, erasing...\n");
    nvs_flash_erase();
    ret = nvs_flash_init();
  }

  ret = nvs_open("state", NVS_READWRITE, &stat_handle);
  if (ret != ESP_OK) {
    Serial.printf("[MONITOR] Stats: NVS open failed (%d), not saved this run\n", ret);
    s_statsLoaded = true;
    return;
  }

  std::lock_guard<std::mutex> lock(s_statsMutex);
  StatsRecord a, b;
  bool missingA, missingB;
  const bool okA = readStatsSlot("statA", a, missingA);
  const bool okB = readStatsSlot("statB", b, missingB);
  if (okA || okB) {
    const StatsRecord &r = (okA && (!okB || (int32_t)(a.seq - b.seq) > 0)) ? a : b;
    portENTER_CRITICAL(&s_bestDiffMux);
    best_diff = r.best_diff;
    portEXIT_CRITICAL(&s_bestDiffMux);
    restoreHashes(r.Mhashes);
    shares = r.shares;
    valids = r.valids;
    templates = r.templates;
    shares32Rejected = r.shares32Rejected;
    upTime = r.upTime;
    s_statsSeq = r.seq;
    const bool otherMissing = (&r == &a) ? missingB : missingA;
    Serial.printf("[MONITOR] Stats restored from slot %s (save #%u)%s\n", &r == &a ? "A" : "B",
                  (unsigned)r.seq, (okA && okB) ? "" : otherMissing ? ", other slot empty" : ", other slot damaged");
  } else if (readLegacyStats()) {
    shares32Rejected = 0;
    s_statsSeq = 0;
    Serial.printf("[MONITOR] Stats restored from the old format, now saved in slots A/B\n");
  } else {
    portENTER_CRITICAL(&s_bestDiffMux);
    best_diff = 0.0;
    portEXIT_CRITICAL(&s_bestDiffMux);
    shares = 0;
    valids = 0;
    templates = 0;
    shares32Rejected = 0;
    upTime = 0;
    s_statsSeq = 0;
    Serial.printf("[MONITOR] No saved stats, starting from zero\n");
  }
  s_statsLoaded = true;
}

void saveStat() {
  if (!Settings.saveStats) return;
  if (!s_statsLoaded) return;
  if (!stat_handle) return;
  std::lock_guard<std::mutex> lock(s_statsMutex);

  StatsRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = STATS_MAGIC;
  r.seq = s_statsSeq + 1;
  r.best_diff = getBestDiff();
  r.Mhashes = (uint32_t)(getTotalHashes() / 1000000);
  r.shares = shares;
  r.valids = valids;
  r.templates = templates;
  r.shares32Rejected = shares32Rejected;
  r.upTime = upTime;
  r.crc = statsRecordCrc(r);

  const char *slot = (r.seq & 1) ? "statA" : "statB";
  if (nvs_set_blob(stat_handle, slot, &r, sizeof(r)) == ESP_OK && nvs_commit(stat_handle) == ESP_OK) {
    s_statsSeq = r.seq;
    Serial.printf("[MONITOR] Stats saved (slot %s, #%u)\n", slot + 4, (unsigned)r.seq);
  } else {
    Serial.printf("[MONITOR] Stats save failed (slot %s)\n", slot + 4);
  }
}

void resetStat() {
    Serial.printf("[MONITOR] Resetting NVS stats\n");
    templates = totalKHashes = elapsedKHs = upTime = shares = valids = rejects = s_sharesAccepted = 0;
    portENTER_CRITICAL(&s_hashMux);
    s_hashTotal = 0;
    s_sessionHashes = 0;
    s_notableCount = 0;
    portEXIT_CRITICAL(&s_hashMux);
    shares32Rejected = 0;
    portENTER_CRITICAL(&s_bestDiffMux);
    best_diff = 0.0;
    s_sessionBestDiff = 0.0;
    portEXIT_CRITICAL(&s_bestDiffMux);
    if (stat_handle) s_statsLoaded = true;
    saveStat();
}

void runMonitor(void *name)
{
  (void)name;
  Serial.println("[MONITOR] started");
  restoreStat();

  unsigned long mLastCheck = 0;

  resetToFirstScreen();

  unsigned long frame = 0;

  uint32_t seconds_elapsed = 0;
  uint32_t zero_hash_seconds = 0;

  totalKHashes = getTotalHashes() / 1000;
  uint32_t last_update_millis = millis();
  uint32_t uptime_frac = 0;
  statusRateSample(totalKHashes, last_update_millis);
  portENTER_CRITICAL(&s_hashMux);
  s_bootKh = totalKHashes;
  s_bootUs = s_lastSampleUs;
  portEXIT_CRITICAL(&s_hashMux);

  while (1)
  {
    uint32_t now_millis = millis();
    uint32_t mElapsed = now_millis - mLastCheck;
    if (mElapsed >= 1000)
    {
      mLastCheck = now_millis;
      last_update_millis = now_millis;

      #ifdef DEBUG_MINING
      {
        static uint32_t lastHw = 0, lastSw0 = 0, lastSw1 = 0;
        uint32_t curHw = debugHashesHw, curSw0 = debugHashesSw[0], curSw1 = debugHashesSw[1];
        Serial.printf("[HASHRATE] HW=%u/s SW0=%u/s SW1=%u/s (total=%u/s)\n",
                      curHw - lastHw, curSw0 - lastSw0, curSw1 - lastSw1,
                      (curHw - lastHw) + (curSw0 - lastSw0) + (curSw1 - lastSw1));
        lastHw = curHw; lastSw0 = curSw0; lastSw1 = curSw1;
        #if defined(PIPELINED_S3_MINING) && !defined(PIPELINED_ASM_MINING)
        Serial.printf("[PLDBG] hits=%u real=%u mismatch=%u accepted=%u\n",
                      debugPipelinedHits, debugPipelinedRealHits, debugPipelinedHwSwMismatch, debugPipelinedAccepted);
        #endif
        #ifdef PIPELINED_ASM_MINING
        Serial.printf("[PLDBG] hits=%u real=%u mismatch=%u accepted=%u\n",
                      debugPipelinedHits, debugPipelinedRealHits, debugPipelinedHwSwMismatch, debugPipelinedAccepted);
        {
          static uint32_t lastGapUs = 0, lastGapCount = 0;
          uint32_t curGapUs = debugChunkGapUs, curGapCount = debugChunkCount;
          uint32_t dGapUs = curGapUs - lastGapUs, dGapCount = curGapCount - lastGapCount;
          Serial.printf("[CHUNKGAP] chunks=%u totalGapUs=%u avgGapUs=%u\n",
                        dGapCount, dGapUs, dGapCount ? (dGapUs / dGapCount) : 0);
          lastGapUs = curGapUs; lastGapCount = curGapCount;
        }
        {
          static uint32_t lastHitUs = 0, lastHitCount = 0;
          uint32_t curHitUs = debugHitOverheadUs, curHitCount = debugHitOverheadCount;
          uint32_t dHitUs = curHitUs - lastHitUs, dHitCount = curHitCount - lastHitCount;
          Serial.printf("[HITOVERHEAD] hits=%u totalUs=%u avgUs=%u\n",
                        dHitCount, dHitUs, dHitCount ? (dHitUs / dHitCount) : 0);
          lastHitUs = curHitUs; lastHitCount = curHitCount;
        }
        #endif
      }
      #endif

      uint64_t currentKHashes = getTotalHashes() / 1000;
      elapsedKHs = (uint32_t)(currentKHashes - totalKHashes);
      totalKHashes = currentKHashes;
      statusRateSample(currentKHashes, now_millis);

      uptime_frac += mElapsed;
      while (uptime_frac >= 1000)
      {
        uptime_frac -= 1000;
        upTime++;
      }

      applyPendingSettings(); // from the LAN settings page
      webMonitorDataTick();   // price / block height for the live web monitor
      backlightTick();
      drawCurrentScreen(mElapsed);

      if (elapsedKHs == 0)
      {
        #ifndef DEBUG_MINING
        if ((zero_hash_seconds % 30) == 0)
        #endif
        {
          Serial.printf(">>> [i] Miner: newJob>%s / inRun>%s) - Client: connected>%s / subscribed>%s / wificonnected>%s\n",
              "true",
              isMinerSuscribed ? "true" : "false",
              s_stratumConnected ? "true" : "false",
              isMinerSuscribed ? "true" : "false",
              WiFi.status() == WL_CONNECTED ? "true" : "false");
        }
        zero_hash_seconds++;
      }
      else
      {
        zero_hash_seconds = 0;
      }

      #ifdef DEBUG_MEMORY
      Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
      Serial.printf("### Max stack usage: %d\n", uxTaskGetStackHighWaterMark(NULL));
      #endif

      seconds_elapsed++;

      int saveInterval = saveIntervals[currentIntervalIndex];
      if (saveInterval > STATS_SAVE_MAX_s) saveInterval = STATS_SAVE_MAX_s;
      if (seconds_elapsed % saveInterval == 0) {
        saveStat();
        seconds_elapsed = 0;
        if (currentIntervalIndex < saveIntervalsSize - 1)
          currentIntervalIndex++;
      }
    }
    animateCurrentScreen(frame);
    doLedStuff(frame);

    vTaskDelay(DELAY / portTICK_PERIOD_MS);
    frame++;
  }
}
