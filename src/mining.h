#ifndef MINING_API_H
#define MINING_API_H

#include <Arduino.h>
#include <mutex>

// Serializes classic-ESP32 (CONFIG_IDF_TARGET_ESP32) hardware SHA register
// access (SHA_TEXT_BASE) against mbedTLS's own hardware-accelerated TLS
// handshake hashing. Confirmed via a decoded crash backtrace: the classic
// ESP32 SHA engine's esp_sha_lock_engine() locks *per algorithm type* (see
// sha_parallel_engine.h), so our mining code locking SHA2_256 does NOT block
// mbedTLS locking SHA2_384/512 for a TLS "Finished" message hash, even
// though both share the same physical SHA_TEXT_BASE registers -- causing
// either silently wrong hashes or a hard abort() inside sha_hal_read_digest()
// depending on timing. Any code that touches SHA_TEXT_BASE directly (the
// plain-C and pipelined-asm classic-ESP32 HW mining paths) must hold this
// while doing so; any code that can trigger a TLS handshake with hardware
// SHA active (e.g. the pool-stats HTTPS fetch or OTA) must hold it too.
extern std::mutex g_hwShaMutex;
extern volatile bool g_otaUpdating;

// Immediately request any in-flight hardware SHA chunk to yield so the caller
// can acquire g_hwShaMutex without waiting up to 265-1190 ms for the chunk end.
void pauseHwMinerForTls(void);

// Bumped by the stratum task when the pool accepts a 32-bit share (difficulty
// >= 1, the "32BITS SHARES" counter -- not every pool share, those come in
// more than once a second), when a valid block is found, or when a new
// all-time best difficulty is set after warmup.
extern volatile uint32_t g_shareFoundEvents;
extern volatile uint32_t g_blockFoundEvents;
extern volatile uint32_t g_bestDiffEvents;

// 32-bit shares (difficulty >= 1) the pool refused; the accepted ones are
// the existing "shares" counter. Saved with the other stats.
extern volatile uint32_t shares32Rejected;

// ESP32-S3 overlapped HW loop only (PIPELINED_S3_MINING). On the S3 every
// mbedTLS hash (the coinbase and merkle hashes of a new job, TLS handshakes)
// goes through the one SHA engine, which that loop holds almost all the time:
// a task that wants the engine would otherwise wait for the end of the
// miner's current chunk for every single hash, and a new job would take
// seconds to prepare. A task that is about to use the engine puts
// HW_SHA_REQUEST() in the scope doing so; the miner looks at the count every
// 256 nonces and stays off the engine until it is back to zero. Expands to
// nothing in every other build.
#if defined(PIPELINED_S3_MINING)
#include <atomic>
extern std::atomic<int> g_hwShaWanted;

struct HwShaRequest
{
  HwShaRequest() { g_hwShaWanted++; }
  ~HwShaRequest() { g_hwShaWanted--; }
};
#define HW_SHA_REQUEST() HwShaRequest hwShaRequest_
#else
#define HW_SHA_REQUEST()
#endif

// Mining
#define MAX_NONCE_STEP  5000000U
#define MAX_NONCE       25000000U
#define TARGET_NONCE    471136297U
#define DEFAULT_DIFFICULTY  0.00015
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000

#define HARDWARE_SHA265

#ifdef FORCE_SW_MINING
#undef HARDWARE_SHA265
#endif

void runMonitor(void *name);
void runStratumWorker(void *name);

// Pool address, port, wallet and pool password from the LAN settings page:
// the stratum task swaps them into Settings between connections and
// reconnects to the pool, no restart needed. readPoolSettings() gives the
// ones in use, or the ones waiting to be swapped in.
struct TSettings;
void requestPoolChange(const TSettings &s);
void readPoolSettings(TSettings &s);

// Backup pool: after POOL_FAILOVER_TRIES failed attempts in a row on the pool
// in use, the stratum task moves to the other one (when a backup is set).
// While on the backup it checks every POOL_FAILBACK_ms whether the main pool
// takes a connection again, and goes back if it does. The pool in use:
#define POOL_FAILOVER_TRIES 3
#ifndef POOL_FAILBACK_ms
#define POOL_FAILBACK_ms    (10 * 60 * 1000UL)
#endif
bool onBackupPool(void);
String activePoolAddress(void);
int activePoolPort(void);

// Live miner state for the LAN status endpoint (/api/status).
struct MinerStatus
{
  float hashrateKHs;       // last 10 s
  float avgHashrateKHs;    // since boot
  uint32_t accepted;       // shares the pool accepted since boot
  uint32_t rejected;       // ...and refused
  bool subscribed;         // connected and subscribed to the pool
  double poolDifficulty;
  int8_t authorized;       // mining.authorize answer: -1 none yet, 0 refused, 1 accepted
  double bestDiff;         // all-time best share difficulty (NVS + session)
  double sessionBestDiff;  // best share difficulty since boot
  int32_t pingMs;          // last mining.submit -> ACK round-trip in ms (-1 if none yet)
  uint64_t sessionHashes;  // hashes computed since boot
};
MinerStatus getMinerStatus(void);
int8_t poolAuthorized(void);

#define NOTABLE_SHARES_MAX 8
struct NotableShare
{
  uint32_t epoch;     // UTC unix timestamp (0 if NTP not synced yet)
  uint32_t uptimeS;   // seconds since boot when recorded
  double difficulty;
  bool is32Bit;       // difficulty >= 1.0
  bool isBlock;       // met network target
};
size_t getNotableShares(NotableShare *out, size_t maxCount);

// Non-blocking LAN UDP fleet share celebration (called from loop()).
void celebrateProcess(void);

void runMiner(void *name);
void minerWorkerSw(void * task_id);
void minerWorkerHw(void * task_id);

String printLocalTime(void);

void resetStat();
void updateBestDiff(double diff); // keep the best hash difficulty seen (any core)
void addHashes(uint32_t n);        // stratum task: hashes the miners report
uint64_t getTotalHashes();         // since the saved stats began; any task
uint64_t getSessionHashes();       // since boot; any task
double getBestDiff();
double getSessionBestDiff();
void saveStat();   // write the stats to NVS now (before a planned restart / power off)

extern volatile uint32_t shares;
extern volatile uint32_t valids;
extern volatile uint32_t rejects;

typedef struct{
  uint8_t merkle_result[32];
  uint8_t bytearray_blockheader[128];
  bool valid; // false: the job could not be prepared (see calculateMiningData)
} miner_data;

#endif // MINING_API_H
