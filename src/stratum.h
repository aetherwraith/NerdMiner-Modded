#ifndef STRATUM_API_H
#define STRATUM_API_H

#include <stdint.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <vector>

#define MAX_MERKLE_BRANCHES 32
#define MAX_EXTRANONCE2_SIZE 16 // bytes; calculateMiningData writes up to 2x that many hex digits
#define HASH_SIZE 32

#define BUFFER 1024
// Longest mining.notify accepted (its JSON document is sized from the line):
// pools with many payout outputs send coinbases of several KB.
#define MAX_NOTIFY_LEN 32768

typedef struct {
    String sub_details;
    String extranonce1;
    String extranonce2;
    int extranonce2_size;
    char wName[80];  // same size as Settings.BtcWallet
    char wPass[80];  // same size as Settings.PoolPassword (20 overflowed with a long password)
    bool version_rolling_enabled;
    uint32_t version_rolling_mask;
} mining_subscribe;

typedef struct {
    String job_id;
    String prev_block_hash;
    String coinb1;
    String coinb2;
    String nbits;
    std::vector<String> merkle_branch; // own copy: a JsonArray pointed into the shared JSON document
    String version;
    uint32_t target;
    String ntime;
    bool clean_jobs;
} mining_job;

typedef enum {
    STRATUM_SUCCESS,
    STRATUM_UNKNOWN,
    STRATUM_PARSE_ERROR,
    MINING_NOTIFY,
    MINING_SET_DIFFICULTY
} stratum_method;

unsigned long getNextId(unsigned long id);
bool checkError(const JsonDocument& doc);

// Method Mining.subscribe
mining_subscribe init_mining_subscribe(void);
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe);
bool parse_mining_subscribe(const String& line, mining_subscribe& mSubscribe);

// Method Mining.authorise
bool tx_mining_auth(WiFiClient& client, const char * user, const char * pass, unsigned long &auth_id);
stratum_method parse_mining_method(const String& line, unsigned long *idOut = nullptr, bool *resultOut = nullptr);
bool parse_mining_notify(const String& line, mining_job& mJob);

// Method Mining.configure (BIP-320 version rolling)
bool tx_mining_configure(WiFiClient& client, unsigned long &cfg_id);
bool parse_mining_configure(const String& line, uint32_t& maskOut);

// Method Mining.submit
bool tx_mining_submit(WiFiClient& client, const mining_subscribe& mWorker, const mining_job& mJob, unsigned long nonce, unsigned long &submit_id, uint32_t versionBits = 0, bool useVersionBits = false);

// Difficulty Methods
bool tx_suggest_difficulty(WiFiClient& client, double difficulty);
bool parse_mining_set_difficulty(const String& line, double& difficulty);

#endif // STRATUM_API_H
