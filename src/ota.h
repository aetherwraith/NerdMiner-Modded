#ifndef _OTA_H_
#define _OTA_H_

#include <Arduino.h>
#include <WebServer.h>

enum OtaState {
  OTA_STATE_IDLE = 0,
  OTA_STATE_CHECKING,
  OTA_STATE_DOWNLOADING,
  OTA_STATE_FLASHING,
  OTA_STATE_SUCCESS,
  OTA_STATE_ERROR
};

struct OtaStatus {
  OtaState state;
  size_t bytesWritten;
  size_t totalBytes;
  int progressPercent;
  String errorMessage;
  String currentVersion;
  String latestVersion;
  String releaseUrl;
  String downloadUrl;
  String releaseNotes;
  bool hasUpdate;
  bool hasDualOta;
  String runningPartition;
  String nextPartition;
  String boardModel;
};

/**
 * @brief Initialize OTA endpoints and attach to WebServer
 */
void setup_ota(WebServer &server);

/**
 * @brief Background tick for OTA operations
 */
void otaProcess(void);

/**
 * @brief Get current OTA progress and system status
 */
OtaStatus getOtaStatus(void);

/**
 * @brief Trigger an OTA update from a binary URL (HTTP/HTTPS)
 */
bool startUrlOta(const String &url);

/**
 * @brief Trigger an OTA update from GitHub Releases
 */
bool startGitHubOta(const String &targetVersion = "latest");

/**
 * @brief Check GitHub Releases for new firmware versions
 */
bool checkGitHubUpdate(void);

/**
 * @brief Get board model string
 */
String getBoardModel(void);

/**
 * @brief Check if device has dual OTA partitions enabled
 */
bool hasDualOtaSupport(void);

#endif // _OTA_H_
