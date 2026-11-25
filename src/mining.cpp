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
#include "mining_internal.h"
#include "utils.h"
#include "monitor.h"
#include "timeconst.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "wManager.h"
#include <atomic>
#include <mutex>
#include <stddef.h> // offsetof (saved stats record)
#include <string.h>
#include <time.h>
#include "mbedtls/sha256.h"
#include "i2c_master.h"
#include <esp_timer.h>

// SW and HW miners split the nonce space by the low byte of the native nonce:
// SW chunks use low byte < 0x80, HW paths use low byte >= 0x80.
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

uint64_t totalKHashes = 0; // 32 bits wrapped after ~4.3 TH (about 50 days at 1 MH/s)
uint32_t elapsedKHs = 0;
uint64_t upTime = 0;

uint64_t getTotalKHashes()
{
  return getTotalHashes() / 1000;
}

uint64_t getUpTime()
{
  portENTER_CRITICAL(&s_hashMux);
  const uint64_t v = upTime;
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
static std::atomic<bool> s_onBackup{false};
static uint8_t s_poolFailures = 0;   // failed attempts in a row on the pool in use
static uint32_t s_backupSince = 0;   // millis() of the switch to the backup, or of the last look at the main pool
static std::atomic<uint32_t> s_poolDisconnectedSinceMs{0};
std::atomic<bool> s_thermalThrottled{false};

uint32_t getPoolDisconnectedSinceMs(void)
{
  return s_poolDisconnectedSinceMs.load(std::memory_order_relaxed);
}

bool getThermalThrottled(void)
{
  return s_thermalThrottled.load(std::memory_order_relaxed);
}

const char* getResetReasonStr(void)
{
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "ext";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

static String activePoolAddressLocked(void)
{
  return s_onBackup.load(std::memory_order_relaxed) ? Settings.BackupPoolAddress : Settings.PoolAddress;
}

static int activePoolPortLocked(void)
{
  return s_onBackup.load(std::memory_order_relaxed) ? Settings.BackupPoolPort : Settings.PoolPort;
}

static String activePoolPasswordLocked(void)
{
  return String(s_onBackup.load(std::memory_order_relaxed) ? Settings.BackupPoolPassword : Settings.PoolPassword);
}

static bool backupPoolSet(void)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  return !Settings.BackupPoolAddress.isEmpty() && Settings.BackupPoolPort > 0;
}

bool onBackupPool(void)
{
  return s_onBackup.load(std::memory_order_relaxed);
}

String activePoolAddress(void)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  return activePoolAddressLocked();
}

int activePoolPort(void)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  return activePoolPortLocked();
}

static String activePoolPassword(void)
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  return activePoolPasswordLocked();
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
  const bool ok = probe.connect(ip, mainPort, 1000);
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
static unsigned long s_cfgId = 0;
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
  const bool hasBackup = !Settings.BackupPoolAddress.isEmpty() && Settings.BackupPoolPort > 0;
  s_onBackup = s_onBackup && !mainChanged && hasBackup;
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

static std::atomic<uint32_t> s_stratumConnectCount{0};

uint32_t getStratumReconnects(void)
{
  const uint32_t c = s_stratumConnectCount.load(std::memory_order_relaxed);
  return c > 1 ? (c - 1) : 0;
}

bool checkPoolConnection(void) {
  if (client.connected()) {
    s_stratumConnected = true;
    s_poolDisconnectedSinceMs.store(0, std::memory_order_relaxed);
    return true;
  }

  s_stratumConnected = false;
  if (s_poolDisconnectedSinceMs.load(std::memory_order_relaxed) == 0)
    s_poolDisconnectedSinceMs.store(millis() | 1u, std::memory_order_relaxed);
  isMinerSuscribed = false;

  Serial.println("Client not connected, trying to connect...");
  const String poolAddr = activePoolAddress();

  if (serverIP == IPAddress(1, 1, 1, 1) || serverIP == IPAddress(0, 0, 0, 0)) {
    if (!WiFi.hostByName(poolAddr.c_str(), serverIP) || serverIP == IPAddress(0, 0, 0, 0)) {
      serverIP = IPAddress(1, 1, 1, 1);
      Serial.println("DNS resolution failed for: " + poolAddr);
      return false;
    }
    Serial.printf("Resolved DNS got: %s\n", serverIP.toString().c_str());
  }

  // Try connecting pool IP with a 3s timeout so reconnects never stall the task
  if (!client.connect(serverIP, activePoolPort(), 3000)) {
    Serial.println("Imposible to connect to : " + poolAddr);
    IPAddress resolved;
    if (WiFi.hostByName(poolAddr.c_str(), resolved) && resolved != IPAddress(0, 0, 0, 0))
      serverIP = resolved;
    else
      serverIP = IPAddress(1, 1, 1, 1);
    Serial.printf("Resolved DNS got: %s\n", serverIP.toString().c_str());
    return false;
  }

  s_stratumConnected = true;
  s_poolDisconnectedSinceMs.store(0, std::memory_order_relaxed);
  s_stratumConnectCount.fetch_add(1, std::memory_order_relaxed);
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

std::mutex s_job_mutex;
std::mutex g_hwShaMutex;  // see mining.h for why this exists
volatile bool g_otaUpdating = false;
std::atomic<int> g_tlsPauseCount{0};
#if defined(PIPELINED_S3_MINING)
std::atomic<int> g_hwShaWanted{0}; // see mining.h
#endif

ActiveJob s_jobs[2];
volatile uint8_t s_activeJobSlot = 0xFF;
std::atomic<uint32_t> s_swNextIndex{0};
#ifdef HARDWARE_SHA265
std::atomic<uint32_t> s_hwNextIndex{0};
#endif
struct JobResult
{
  uint32_t id;
  uint32_t nonce;
  double difficulty;
  uint8_t hash[32];
};

#define JOB_RESULT_RING_SIZE 64
static JobResult s_jobResultRing[JOB_RESULT_RING_SIZE];
static uint8_t s_jobResultHead = 0;
static uint8_t s_jobResultTail = 0;
static uint8_t s_jobResultCount = 0;
std::atomic<uint32_t> s_working_current_job_id{0xFFFFFFFFu};

static inline void notifyStratumTask(void)
{
  TaskHandle_t h = s_stratumTaskHandle;
  if (h)
    xTaskNotifyGive(h);
}

void submitShareResult(uint32_t jobId, uint32_t nonce, double diff, const uint8_t hash[32])
{
  {
    std::lock_guard<std::mutex> lock(s_job_mutex);
    if (s_jobResultCount < JOB_RESULT_RING_SIZE)
    {
      JobResult &share = s_jobResultRing[s_jobResultTail];
      share.id = jobId;
      share.nonce = nonce;
      share.difficulty = diff;
      memcpy(share.hash, hash, 32);
      s_jobResultTail = (s_jobResultTail + 1) % JOB_RESULT_RING_SIZE;
      s_jobResultCount++;
    }
  }
  notifyStratumTask();
}

#if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
pl_ctx_t s_pl_ctx;
#endif

void pauseHwMinerForTls(void)
{
#if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
  s_pl_ctx.run = 0;
#endif
}

TlsPauseGuard::TlsPauseGuard()
{
  g_tlsPauseCount.fetch_add(1, std::memory_order_release);
  pauseHwMinerForTls();
  g_hwShaMutex.lock();
}

TlsPauseGuard::~TlsPauseGuard()
{
  g_hwShaMutex.unlock();
  g_tlsPauseCount.fetch_sub(1, std::memory_order_release);
}

#define SUBMITION_RING_SIZE 32
struct SubmitionSlot
{
  uint32_t id;
  double diff;
  bool is32bit;
  bool isValid;
  bool active;
};

static SubmitionSlot *findSubmition(SubmitionSlot ring[SUBMITION_RING_SIZE], unsigned long id)
{
  for (size_t i = 0; i < SUBMITION_RING_SIZE; ++i)
  {
    if (ring[i].active && ring[i].id == (uint32_t)id)
      return &ring[i];
  }
  return nullptr;
}

static void MiningJobStop(uint32_t &job_pool, SubmitionSlot submition_ring[SUBMITION_RING_SIZE], uint8_t &submition_head)
{
  {
    std::lock_guard<std::mutex> lock(s_job_mutex);
    s_jobResultHead = 0;
    s_jobResultTail = 0;
    s_jobResultCount = 0;
    s_activeJobSlot = 0xFF;
    s_working_current_job_id.store(0xFFFFFFFFu, std::memory_order_relaxed);
  }
#if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
  s_pl_ctx.run = 0;
#endif
  job_pool = 0xFFFFFFFF;
  memset(submition_ring, 0, sizeof(SubmitionSlot) * SUBMITION_RING_SIZE);
  submition_head = 0;
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
  esp_task_wdt_add(NULL);

  Serial.println("");
  Serial.printf("\n[WORKER] Started. Running %s on core %d\n", (char *)name, xPortGetCoreID());

  #ifdef DEBUG_MEMORY
  Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  #endif

  SubmitionSlot s_submition_ring[SUBMITION_RING_SIZE] = {};
  uint8_t s_submition_head = 0;

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
    MiningJobStop(job_pool, s_submition_ring, s_submition_head);
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
  uint32_t sha_buffer_swap[20] = {0};
  #endif
  bool adaptiveDiffSent = false;

  auto drainAndSubmitResults = [&]() {
    if (job_pool == 0xFFFFFFFF)
      return;
    bool more = true;
    while (more)
    {
      JobResult batch[16];
      uint8_t batchCount = 0;
      {
        std::lock_guard<std::mutex> lock(s_job_mutex);
        while (s_jobResultCount > 0 && batchCount < 16)
        {
          batch[batchCount++] = s_jobResultRing[s_jobResultHead];
          s_jobResultHead = (s_jobResultHead + 1) % JOB_RESULT_RING_SIZE;
          s_jobResultCount--;
        }
        more = (s_jobResultCount > 0);
      }
      for (uint8_t idx = 0; idx < batchCount; ++idx)
      {
        const JobResult &res = batch[idx];
        updateBestDiff(res.difficulty);
        if (res.difficulty > currentPoolDifficulty && job_pool == res.id)
        {
          if (!client.connected())
          {
            s_stratumConnected = false;
            return;
          }
          unsigned long sumbit_id = 0;
          tx_mining_submit(client, mWorker, mJob, res.nonce, sumbit_id, 0, mWorker.version_rolling_enabled);
          const uint32_t sentNow = millis();
          mLastTXtoPool = sentNow;
          s_submitSentRing[s_submitSentHead] = {sumbit_id, sentNow};
          s_submitSentHead = (s_submitSentHead + 1) & 7;

          Serial.print("   - Current diff share: "); Serial.println(res.difficulty, 12);
          Serial.print("   - Current pool diff : "); Serial.println(currentPoolDifficulty, 12);
          Serial.print("   - TX SHARE: ");
          for (size_t i = 0; i < 32; i++)
              Serial.printf("%02x", res.hash[i]);
          Serial.println("");

          SubmitionSlot &slot = s_submition_ring[s_submition_head];
          slot.id = (uint32_t)sumbit_id;
          slot.diff = res.difficulty;
          slot.is32bit = (res.hash[29] == 0 && res.hash[28] == 0);
          slot.isValid = slot.is32bit ? checkValid(res.hash, mJob.nbits.c_str()) : false;
          slot.active = true;
          s_submition_head = (s_submition_head + 1) % SUBMITION_RING_SIZE;
        }
      }
    }
  };

  while (true) {
    esp_task_wdt_reset();

    if (takePoolChange()) {
      Serial.printf("[STRATUM] Pool settings changed, switching to %s:%d\n", activePoolAddress().c_str(), activePoolPort());
      dropPool();
    }

    if (WiFi.status() != WL_CONNECTED) {
      s_stratumConnected = false;
      mMonitor.NerdStatus = NM_Connecting;
      MiningJobStop(job_pool, s_submition_ring, s_submition_head);
      WiFi.reconnect();
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    if (!checkPoolConnection()) {
      MiningJobStop(job_pool, s_submition_ring, s_submition_head);
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
      mWorker = init_mining_subscribe();

      // STEP 1: Pool server connection (SUBSCRIBE + optional BIP-320 CONFIGURE)
      if (!tx_mining_subscribe(client, mWorker)) {
        resetSocket();
        MiningJobStop(job_pool, s_submition_ring, s_submition_head);
        poolFailed();
        continue;
      }

      s_cfgId = 0;
      tx_mining_configure(client, s_cfgId);
      adaptiveDiffSent = false;

      {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        strlcpy(mWorker.wName, Settings.BtcWallet, sizeof(mWorker.wName));
        strlcpy(mWorker.wPass, activePoolPasswordLocked().c_str(), sizeof(mWorker.wPass));
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
      MiningJobStop(job_pool, s_submition_ring, s_submition_head);
      poolFailed();
      continue;
    }

    {
      uint32_t time_now = millis();
      if (time_now - last_job_time >= 10 * 60 * 1000) // 10 minutes without job (wrap-safe)
      {
        resetSocket();
        MiningJobStop(job_pool, s_submition_ring, s_submition_head);
        poolFailed();
        continue;
      }
    }

    // On the backup: go back to the main pool once it answers again.
    if (s_onBackup && millis() - s_backupSince >= POOL_FAILBACK_ms)
    {
      s_backupSince = millis();
      drainAndSubmitResults();
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
        int i = 0;
        if (rxDropUntilNewline)
        {
          while (i < got && tmp[i] != '\n')
            ++i;
          if (i < got)
          {
            rxDropUntilNewline = false;
            ++i; // skip '\n'
          }
        }
        int outLen = 0;
        bool sawNl = false;
        for (; i < got; ++i)
        {
          const uint8_t ch = tmp[i];
          if (ch == '\r')
            continue;
          tmp[outLen++] = ch;
          if (ch == '\n')
            sawNl = true;
        }
        if (outLen > 0)
        {
          if (!sawNl && rxBuf.length() + (size_t)outLen > MAX_NOTIFY_LEN + 64)
          {
            Serial.printf("[STRATUM] Line exceeded %u bytes, dropping\n", (unsigned)(MAX_NOTIFY_LEN + 64));
            rxBuf.remove(0);
            rxDropUntilNewline = true;
          }
          else
          {
            rxBuf.concat((const char *)tmp, (size_t)outLen);
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
                                          // Drain any shares found for the previous job before replacing mJob/job_pool
                                          drainAndSubmitResults();
                                          // Drop large coinbase/merkle strings once binary midstate/header is built
                                          newJob.coinb1 = String();
                                          newJob.coinb2 = String();
                                          std::vector<String>().swap(newJob.merkle_branch);
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
                                          for (int i = 0; i < 20; ++i)
                                            sha_buffer_swap[i] = __builtin_bswap32(((const uint32_t *)(mMiner.bytearray_blockheader))[i]);
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
                                            memcpy(nj.tail, mMiner.bytearray_blockheader + 64, sizeof(nj.tail));
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
                                            s_working_current_job_id.store(job_pool, std::memory_order_relaxed);
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
                                        MiningJobStop(job_pool, s_submition_ring, s_submition_head);
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
                                        if (s_cfgId != 0 && rxId == s_cfgId)
                                        {
                                          s_cfgId = 0;
                                          uint32_t mask = 0;
                                          if (parse_mining_configure(line, mask)) {
                                            mWorker.version_rolling_enabled = true;
                                            mWorker.version_rolling_mask = mask;
                                            Serial.printf("[STRATUM] BIP-320 version rolling enabled (mask 0x%08lx)\n", (unsigned long)mask);
                                          }
                                          break;
                                        }
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
                                        SubmitionSlot *slot = findSubmition(s_submition_ring, rxId);
                                        if (slot)
                                        {
                                          const bool accepted = rxResult;
                                          if (accepted)
                                            s_sharesAccepted++;
                                          else
                                            rejects++; // "result":false without an error

                                          updateBestDiff(slot->diff);
                                          if (accepted || slot->isValid)
                                            recordNotableShare(slot->diff, slot->isValid);
                                          if (slot->is32bit && !accepted)
                                            shares32Rejected++;
                                          if (slot->is32bit && accepted)
                                          {
                                            shares++;
                                            g_shareFoundEvents++;
                                          }
                                          if (slot->isValid)
                                          {
                                            Serial.println("CONGRATULATIONS! Valid block found");
                                            valids++;
                                            g_blockFoundEvents++;
                                          }
                                          slot->active = false;
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
                                        SubmitionSlot *slot = findSubmition(s_submition_ring, rxId);
                                        if (slot)
                                        {
                                          Serial.printf("Refuse submition %lu\n", rxId);
                                          rejects++;
                                          if (slot->is32bit)
                                            shares32Rejected++;
                                          slot->active = false;
                                        }
                                      }
                                      break;
          default:                    Serial.println("  Parsed JSON: unknown"); break;
      }
    }

    s_stratumConnected = client.connected();

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
        uint8_t i2c_hash[32];
        ((uint32_t *)(i2c_tail + 12))[0] = nonce_vector[n];
        if (nerd_sha256d_baked(diget_mid, i2c_tail, bake, i2c_hash))
        {
          submitShareResult(job_pool, nonce_vector[n], diff_from_target(i2c_hash), i2c_hash);
        }
      }
    }
    #endif

    drainAndSubmitResults();

    // Adaptive suggest_difficulty once measured hashrate stabilizes (~15s)
    if (!adaptiveDiffSent && isMinerSuscribed && client.connected() && millis() >= 15000) {
      double khs = (double)getSessionHashes() / (double)millis();
      if (khs > 10.0) {
        double targetDiff = (khs * 1000.0 * 20.0) / 4294967296.0;
        if (targetDiff < 0.0001) targetDiff = 0.0001;
        if (targetDiff > 1.0) targetDiff = 1.0;
        if (targetDiff > currentPoolDifficulty * 3.0 || targetDiff < currentPoolDifficulty / 3.0) {
          tx_suggest_difficulty(client, targetDiff);
          mLastTXtoPool = millis();
        }
        adaptiveDiffSent = true;
      }
    }

    // Wake in <1 ms when a miner pushes a share, or poll socket every <=10 ms
    const bool hasSocketData = client.connected() && (client.available() > 0 || rxBuf.indexOf('\n') >= 0);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hasSocketData ? 0 : 10));
  }
}


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

  if (!Settings.saveStats) {
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
  esp_task_wdt_add(NULL);
  restoreStat();

  unsigned long mLastCheck = 0;

  resetToFirstScreen();

  unsigned long frame = 0;

  uint32_t seconds_elapsed = 0;
  uint32_t zero_hash_seconds = 0;

  totalKHashes = getTotalHashes() / 1000;
  uint32_t uptime_frac = 0;
  statusRateSample(totalKHashes, millis());
  portENTER_CRITICAL(&s_hashMux);
  s_bootKh = totalKHashes;
  s_bootUs = s_lastSampleUs;
  portEXIT_CRITICAL(&s_hashMux);

  while (1)
  {
    esp_task_wdt_reset();
    uint32_t now_millis = millis();
    uint32_t mElapsed = now_millis - mLastCheck;
    if (mElapsed >= 1000)
    {
      mLastCheck = now_millis;

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
        portENTER_CRITICAL(&s_hashMux);
        upTime++;
        portEXIT_CRITICAL(&s_hashMux);
      }

      applyPendingSettings(); // from the LAN settings page
      webMonitorDataTick();   // price / block height for the live web monitor
      {
        int tLim = 0;
        {
          std::lock_guard<std::mutex> lock(g_settingsMutex);
          tLim = Settings.TempLimit;
        }
        if (tLim > 0) {
          float tc = temperatureRead();
          if (tc >= (float)tLim)
            s_thermalThrottled.store(true, std::memory_order_relaxed);
          else if (tc <= (float)(tLim - 2))
            s_thermalThrottled.store(false, std::memory_order_relaxed);
        } else {
          s_thermalThrottled.store(false, std::memory_order_relaxed);
        }
      }
      backlightTick();
      drawCurrentScreen(mElapsed);

      if (elapsedKHs == 0)
      {
        #ifndef DEBUG_MINING
        if ((zero_hash_seconds % 30) == 0)
        #endif
        {
          Serial.printf(">>> [i] Client: connected>%s / subscribed>%s / wificonnected>%s\n",
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
