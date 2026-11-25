#ifndef _WEBCONFIG_H
#define _WEBCONFIG_H

#include "drivers/storage/storage.h"
#ifndef NATIVE_TEST
#include <esp_http_server.h>
#endif

// Minimal always-on LAN settings page, reachable at the device's WiFi IP
// once connected -- alternative to re-entering the device's own setup AP
// to change pool/wallet/etc. Protected by HTTP basic auth (see webConfig.cpp).
void setup_webConfig(void);
void webConfigProcess(void);

#ifndef NATIVE_TEST
String reqHeader(httpd_req_t *req, const char *name);
String reqQueryParam(httpd_req_t *req, const char *key);
String reqReadBody(httpd_req_t *req, size_t maxBytes = 4096);
esp_err_t sendCorsJson(httpd_req_t *req, int code, const char *json, bool noStore = false);
esp_err_t handleCorsOptions(httpd_req_t *req);
bool checkWebApiMutationAllowed(httpd_req_t *req);
bool isWebApiMutationPermitted(httpd_req_t *req);
#endif

TSettings currentSettings(void);
bool applyLiveSettings(const TSettings &before, const TSettings &next, bool &poolChangedOut);

// The miner's state as /api/status serves it (also the MQTT state message).
#include <ArduinoJson.h>
void fillMinerStatusJson(JsonDocument &doc);

#endif // _WEBCONFIG_H
