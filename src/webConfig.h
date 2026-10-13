#ifndef _WEBCONFIG_H
#define _WEBCONFIG_H

#include "drivers/storage/storage.h"

// Minimal always-on LAN settings page, reachable at the device's WiFi IP
// once connected -- alternative to re-entering the device's own setup AP
// to change pool/wallet/etc. Protected by HTTP basic auth (see webConfig.cpp).
void setup_webConfig(void);
void webConfigProcess(void);

TSettings currentSettings(void);
bool applyLiveSettings(const TSettings &before, const TSettings &next, bool &poolChangedOut);

#endif // _WEBCONFIG_H
