#include "mining_internal.h"
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <string.h>

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

static inline void waitIfPaused(bool checkTlsPause)
{
  while (g_otaUpdating)
  {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (checkTlsPause)
  {
    while (g_tlsPauseCount.load(std::memory_order_acquire) > 0)
    {
      esp_task_wdt_reset();
      vTaskDelay(1);
    }
  }
}

static inline bool claimActiveJob(std::atomic<uint32_t> &nextIdx, uint32_t chunk, uint32_t limit,
                                  uint32_t &idxOut, ActiveJob &jobOut)
{
  std::lock_guard<std::mutex> lock(s_job_mutex);
  const uint8_t slot = s_activeJobSlot;
  if (slot >= 2)
    return false;
  const uint32_t claimed = nextIdx.fetch_add(chunk, std::memory_order_relaxed);
  if (claimed >= limit)
    return false;
  jobOut = s_jobs[slot];
  idxOut = claimed;
  return true;
}

void minerWorkerSw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerSw Task!\n", miner_id);

  uint8_t hash[32];
  ActiveJob aj;
  uint32_t wdt_counter = 0;
  #ifdef DEBUG_MINING
  volatile uint32_t *myDebugCounter = &debugHashesSw[miner_id & 1];
  #endif

  while (1)
  {
    waitIfPaused(false);

    uint32_t sw_idx = 0;
    const bool has_job = claimActiveJob(s_swNextIndex, NONCE_PER_JOB_SW, 0x80000000u - NONCE_PER_JOB_SW,
                                        sw_idx, aj);

    if (has_job)
    {
      uint32_t done = NONCE_PER_JOB_SW;
      for (uint32_t outer = 0; outer < NONCE_PER_JOB_SW; outer += 128)
      {
        if (s_working_current_job_id.load(std::memory_order_relaxed) != aj.id)
        {
          done = outer;
          break;
        }
        for (uint32_t inner = 0; inner < 128; ++inner)
        {
          const uint32_t n = outer + inner;
          const uint32_t nonce = swNonce(aj.nonce_base, sw_idx + n);
          ((uint32_t *)(aj.tail + 12))[0] = nonce;
          #ifdef DEBUG_MINING
          (*myDebugCounter)++;
          #endif
#if defined(NERD_SW_ASM)
          if (nerd_sha256d_prefilter(aj.midstate, aj.tail, aj.bake) &&
              nerd_sha256d_baked(aj.midstate, aj.tail, aj.bake, hash))
#else
          if (nerd_sha256d_baked(aj.midstate, aj.tail, aj.bake, hash))
#endif
          {
            const double diff_hash = diff_from_target(hash);
            updateBestDiff(diff_hash);
            if (diff_hash > aj.difficulty)
              submitShareResult(aj.id, nonce, diff_hash, hash);
          }
        }
      }
      addHashes(done);
      if (s_thermalThrottled.load(std::memory_order_relaxed))
        vTaskDelay(pdMS_TO_TICKS(10));
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

#if defined(HARDWARE_SHA265) && (defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3))

static inline void nerd_sha_ll_zero_unused_text_words()
{
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);
    for (int i = 5; i < 15; ++i)
        REG_WRITE(&reg_addr_buf[i], 0x00000000);
}

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    REG_WRITE(&reg_addr_buf[0], data_words[0]);
    REG_WRITE(&reg_addr_buf[1], data_words[1]);
    REG_WRITE(&reg_addr_buf[2], data_words[2]);
    REG_WRITE(&reg_addr_buf[3], nonce);
    REG_WRITE(&reg_addr_buf[4], 0x00000080);
    REG_WRITE(&reg_addr_buf[8], 0x00000000);
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
    REG_WRITE(&reg_addr_buf[15], 0x00010000);
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
static uint32_t IRAM_ATTR __attribute__((noinline, optimize("O2")))
s3_pl_mine(const uint32_t *mid, const uint32_t *tail, uint32_t nonce, uint32_t count, bool *hit)
{
  volatile uint32_t *const text = (volatile uint32_t *)SHA_TEXT_BASE;
  volatile uint32_t *const h = (volatile uint32_t *)SHA_H_BASE;
  volatile uint32_t *const busy = (volatile uint32_t *)SHA_BUSY_REG;
  const uint32_t m0 = mid[0], m1 = mid[1], m2 = mid[2], m3 = mid[3], m4 = mid[4], m5 = mid[5], m6 = mid[6],
                 m7 = mid[7];
  const uint32_t t0 = tail[0], t1 = tail[1], t2 = tail[2];

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
    REG_WRITE(SHA_CONTINUE_REG, 1);
    text[8] = 0x00000080;
    text[15] = 0x00010000;
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
    REG_WRITE(SHA_START_REG, 1);
    text[0] = t0;
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

void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHw Task!\n", miner_id);

  uint8_t hash[32];
  ActiveJob aj;
  uint32_t wdt_counter = 0;

#if defined(PIPELINED_S3_MINING) && defined(CONFIG_IDF_TARGET_ESP32S3)
  const bool s3_pl_ok = s3_pl_selftest() || s3_pl_selftest() || s3_pl_selftest();
  Serial.printf("[MINER] HW SHA loop: %s\n", s3_pl_ok ? "overlapped" : "stock (the overlapped loop failed its self-test)");
#endif

  while (1)
  {
    waitIfPaused(true);

    uint32_t hw_idx = 0;
    const bool has_job = claimActiveJob(s_hwNextIndex, NONCE_PER_JOB_HW, 0x80000000u - NONCE_PER_JOB_HW,
                                        hw_idx, aj);

    if (has_job)
    {
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
          uint32_t want = 128u - ((hw_idx + pos) & 0x7Fu);
          if (want > end_pos - pos)
            want = end_pos - pos;
          bool hit = false;
          pos += s3_pl_mine(aj.hw_midstate, (const uint32_t *)aj.tail, hwNonce(aj.nonce_base, hw_idx + pos), want, &hit);

          if (hit)
          {
            #ifdef DEBUG_MINING
            debugPipelinedHits++;
            #endif
            const uint32_t cand = hwNonce(aj.nonce_base, hw_idx + pos - 1);
            ((uint32_t *)(aj.tail + 12))[0] = cand;
            uint8_t sw_hash[32];
            bool confirmed = nerd_sha256d_baked(aj.midstate, aj.tail, aj.bake, sw_hash);
            #ifdef DEBUG_MINING
            if (confirmed)
              debugPipelinedRealHits++;
            else
              debugPipelinedHwSwMismatch++;
            #endif
            const double cand_diff = confirmed && isSha256Valid(sw_hash) ? diff_from_target(sw_hash) : 0.0;
            updateBestDiff(cand_diff);
            if (cand_diff > aj.difficulty)
            {
              #ifdef DEBUG_MINING
              debugPipelinedAccepted++;
              #endif
              submitShareResult(aj.id, cand, cand_diff, sw_hash);
            }
          }

          if (s_working_current_job_id.load(std::memory_order_relaxed) != aj.id ||
              g_tlsPauseCount.load(std::memory_order_acquire) > 0)
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
        nerd_sha_ll_zero_unused_text_words();
        uint32_t done = NONCE_PER_JOB_HW;
        for (uint32_t i = 0; i < NONCE_PER_JOB_HW; ++i)
        {
          const uint32_t n = hwNonce(aj.nonce_base, hw_idx + i);
          #ifdef DEBUG_MINING
          debugHashesHw++;
          #endif
          nerd_sha_ll_write_digest(aj.hw_midstate);
          nerd_sha_ll_fill_text_block_sha256(aj.tail, n);
          REG_WRITE(SHA_CONTINUE_REG, 1);

          sha_ll_load(SHA2_256);
          nerd_sha_hal_wait_idle();
          nerd_sha_ll_fill_text_block_sha256_inter();
          REG_WRITE(SHA_START_REG, 1);
          sha_ll_load(SHA2_256);
          nerd_sha_hal_wait_idle();
          if (nerd_sha_ll_read_digest_if(hash))
          {
            const double diff_hash = diff_from_target(hash);
            if (isSha256Valid(hash))
            {
              updateBestDiff(diff_hash);
              if (diff_hash > aj.difficulty)
                submitShareResult(aj.id, n, diff_hash, hash);
            }
          }
          if ((uint8_t)(i & 0xFF) == 0 &&
              (s_working_current_job_id.load(std::memory_order_relaxed) != aj.id ||
               g_tlsPauseCount.load(std::memory_order_acquire) > 0))
          {
            done = i + 1;
            break;
          }
        }
        addHashes(done);
      }

      esp_sha_release_hardware();
      g_hwShaMutex.unlock();
      if (g_tlsPauseCount.load(std::memory_order_acquire) > 0)
        vTaskDelay(1);
      else if (s_thermalThrottled.load(std::memory_order_relaxed))
        vTaskDelay(pdMS_TO_TICKS(20));
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

#endif // ESP32S2 || ESP32S3 || ESP32C3

#if defined(HARDWARE_SHA265) && defined(CONFIG_IDF_TARGET_ESP32)

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

static inline void nerd_sha_hal_wait_idle()
{
    while (DPORT_REG_READ(SHA_256_BUSY_REG))
    {}
}

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    for (int i = 0; i < 16; ++i)
        reg_addr_buf[i] = data_words[i];
}

static inline void nerd_sha_ll_fill_text_block_sha256_upper(const void *input_text, uint32_t nonce, bool bswapNonce = true)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = bswapNonce ? __builtin_bswap32(nonce) : nonce;
    reg_addr_buf[4]  = 0x80000000;
    for (int i = 5; i < 15; ++i)
        reg_addr_buf[i] = 0x00000000;
    reg_addr_buf[15] = 0x00000280;
}

static inline void nerd_sha_ll_fill_text_block_sha256_double()
{
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[8]  = 0x80000000;
    for (int i = 9; i < 15; ++i)
        reg_addr_buf[i] = 0x00000000;
    reg_addr_buf[15] = 0x00000100;
}

#ifdef PIPELINED_ASM_MINING
typedef uint32_t (*pl_mine_fn)(volatile uint32_t *sha_base, pl_ctx_t *ctx);

extern "C" uint32_t pipelined_hw_mine_classic_v2(volatile uint32_t *sha_base, pl_ctx_t *ctx);
extern "C" uint32_t pipelined_hw_mine_classic_v2_margin(volatile uint32_t *sha_base, pl_ctx_t *ctx);
extern "C" uint32_t pipelined_hw_mine_classic_v2_wide(volatile uint32_t *sha_base, pl_ctx_t *ctx);

#if defined(HW_SHA256_TEST) || defined(SHA_BENCH)
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
    nerd_sha_ll_fill_text_block_sha256_upper(ctx->hdr + 16, ctx->nonce, false);
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
  ActiveJob aj;

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
    waitIfPaused(true);

    uint32_t hw_idx = 0;
    const bool has_job = claimActiveJob(s_hwNextIndex, NONCE_PER_JOB_HW, 0x7F000000u - NONCE_PER_JOB_HW,
                                        hw_idx, aj);

    if (has_job)
    {
      if (s_working_current_job_id.load(std::memory_order_relaxed) != aj.id ||
          g_tlsPauseCount.load(std::memory_order_acquire) > 0)
      {
        if (g_tlsPauseCount.load(std::memory_order_acquire) > 0)
          vTaskDelay(1);
      }
      else
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

      g_hwShaMutex.lock();
      esp_sha_lock_engine(SHA2_256);
#ifdef PIPELINED_ASM_MINING
      {
        if (aj.id != pipelined_last_job_id)
        {
          pipelined_last_job_id = aj.id;
          pipelined_swapped_cursor = __builtin_bswap32(aj.nonce_base) | 0x80000000u;
        }

        memcpy(s_pl_ctx.hdr, aj.sha_buffer_swap, sizeof(s_pl_ctx.hdr));
        s_pl_ctx.nonce = pipelined_swapped_cursor;
        s_pl_ctx.budget = NONCE_PER_JOB_HW;
        s_pl_ctx.pad = 0x80000000u;
        s_pl_ctx.run = 1;
        if (s_working_current_job_id.load(std::memory_order_relaxed) != aj.id ||
            g_tlsPauseCount.load(std::memory_order_acquire) > 0)
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
          ((uint32_t *)(aj.tail + 12))[0] = cand_native_pl;
          uint8_t sw_hash[32];
          bool confirmed = nerd_sha256d_baked(aj.midstate, aj.tail, aj.bake, sw_hash);
          pl_health(confirmed);
          #ifdef DEBUG_MINING
          if (confirmed)
            debugPipelinedRealHits++;
          else
            debugPipelinedHwSwMismatch++;
          #endif

          const double cand_diff = confirmed && isSha256Valid(sw_hash) ? diff_from_target(sw_hash) : 0.0;
          updateBestDiff(cand_diff);
          if (cand_diff > aj.difficulty)
          {
            #ifdef DEBUG_MINING
            debugPipelinedAccepted++;
            #endif
            submitShareResult(aj.id, cand_native_pl, cand_diff, sw_hash);
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
          const uint32_t nonce = hwNonce(aj.nonce_base, hw_idx + i);
          #ifdef DEBUG_MINING
          debugHashesHw++;
          #endif
          nerd_sha_ll_fill_text_block_sha256(aj.sha_buffer_swap);
          sha_ll_start_block(SHA2_256);

          nerd_sha_hal_wait_idle();
          nerd_sha_ll_fill_text_block_sha256_upper(aj.sha_buffer_swap + 16, nonce);
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
              if (diff_hash > aj.difficulty)
                submitShareResult(aj.id, nonce, diff_hash, hash);
            }
          }
          if ((uint8_t)(i & 0xFF) == 0 &&
              (s_working_current_job_id.load(std::memory_order_relaxed) != aj.id ||
               g_tlsPauseCount.load(std::memory_order_acquire) > 0))
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
      if (g_tlsPauseCount.load(std::memory_order_acquire) > 0)
        vTaskDelay(1);
      else if (s_thermalThrottled.load(std::memory_order_relaxed))
        vTaskDelay(pdMS_TO_TICKS(20));
      #ifdef DEBUG_MINING
      debugChunkEndUs = esp_timer_get_time();
      #endif
      }
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    esp_task_wdt_reset();
  }
}

#endif // CONFIG_IDF_TARGET_ESP32
