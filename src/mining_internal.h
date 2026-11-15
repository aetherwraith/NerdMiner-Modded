#ifndef MINING_INTERNAL_H
#define MINING_INTERNAL_H

#include <Arduino.h>
#include <atomic>
#include <mutex>
#include "mining.h"
#include "ShaTests/nerdSHA256plus.h"
#include "utils.h"
#include "drivers/storage/storage.h"

#define NONCE_PER_JOB_SW 4096
#define NONCE_PER_JOB_HW (256 * 1024)

static inline uint32_t swNonce(uint32_t start, uint32_t n)
{
  return start + ((n >> 7) << 8) + (n & 0x7Fu);
}

static inline uint32_t hwNonce(uint32_t start, uint32_t n)
{
  return start + ((n >> 7) << 8) + 0x80u + (n & 0x7Fu);
}

struct ActiveJob
{
  uint32_t id;
  uint32_t nonce_base;
  double difficulty;
  uint8_t tail[16];
  uint32_t midstate[8];
  uint32_t bake[16];
#ifdef HARDWARE_SHA265
#if defined(CONFIG_IDF_TARGET_ESP32)
  uint32_t sha_buffer_swap[20];
#elif defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
  uint32_t hw_midstate[8];
#endif
#endif
};

#if defined(PIPELINED_ASM_MINING) && defined(CONFIG_IDF_TARGET_ESP32)
struct pl_ctx_t
{
  uint32_t hdr[20];
  uint32_t nonce;
  uint32_t budget;
  volatile uint32_t run;
  uint32_t pad;
  uint32_t chk;
};
extern pl_ctx_t s_pl_ctx;
#endif

extern std::mutex s_job_mutex;
extern ActiveJob s_jobs[2];
extern volatile uint8_t s_activeJobSlot;
extern std::atomic<uint32_t> s_swNextIndex;
#ifdef HARDWARE_SHA265
extern std::atomic<uint32_t> s_hwNextIndex;
#endif
extern std::atomic<uint32_t> s_working_current_job_id;
extern std::atomic<bool> s_thermalThrottled;
extern TSettings Settings;
extern std::mutex g_settingsMutex;

void submitShareResult(uint32_t jobId, uint32_t nonce, double diff, const uint8_t hash[32]);

#ifdef DEBUG_MINING
extern volatile uint32_t debugHashesSw[2];
extern volatile uint32_t debugHashesHw;
extern volatile uint32_t debugPipelinedHits;
extern volatile uint32_t debugPipelinedRealHits;
extern volatile uint32_t debugPipelinedAccepted;
extern volatile uint32_t debugPipelinedHwSwMismatch;
extern volatile uint32_t debugChunkGapUs;
extern volatile uint32_t debugChunkCount;
extern volatile uint32_t debugHitOverheadUs;
extern volatile uint32_t debugHitOverheadCount;
#endif

#endif // MINING_INTERNAL_H
