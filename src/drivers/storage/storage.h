#ifndef _STORAGE_H_
#define _STORAGE_H_

#include <Arduino.h>

// config files

// Optional local configuration override file (git-ignored)
#if __has_include("config.local.h")
#include "config.local.h"
#endif

// default settings
#ifndef DEFAULT_SSID
#ifndef HAN
#define DEFAULT_SSID		"NerdMinerAP"
#else
#define DEFAULT_SSID		"HanSoloAP"
#endif
#endif

#ifndef DEFAULT_WIFIPW
#define DEFAULT_WIFIPW		"MineYourCoins"
#endif

#ifndef DEFAULT_POOLURL
#define DEFAULT_POOLURL		"public-pool.io"
#endif

#ifndef DEFAULT_POOLPASS
#define DEFAULT_POOLPASS	"x"
#endif

#ifndef DEFAULT_WALLETID
#define DEFAULT_WALLETID	"yourBtcAddress"
#endif

#ifndef DEFAULT_POOLPORT
#define DEFAULT_POOLPORT	3333
#endif

#ifndef DEFAULT_TIMEZONE
#define DEFAULT_TIMEZONE	"Europe/London"
#endif

#ifndef DEFAULT_SAVESTATS
#define DEFAULT_SAVESTATS	false
#endif

#ifndef DEFAULT_INVERTCOLORS
#ifdef TFT_INVERSION_ON
#define DEFAULT_INVERTCOLORS	true
#else
#define DEFAULT_INVERTCOLORS	false
#endif
#endif

#ifndef DEFAULT_BRIGHTNESS
#define DEFAULT_BRIGHTNESS	250
#endif

#ifndef DEFAULT_FLIPDISPLAY
#define DEFAULT_FLIPDISPLAY	false
#endif

#ifndef DEFAULT_CURRENCY
#define DEFAULT_CURRENCY	"usd"
#endif

#ifndef DEFAULT_AUTOBRIGHTNESS
#define DEFAULT_AUTOBRIGHTNESS	false
#endif

#ifndef DEFAULT_SCREENOFFTIMEOUT
#define DEFAULT_SCREENOFFTIMEOUT	0
#endif

#ifndef DEFAULT_STATUSLED
#define DEFAULT_STATUSLED	true
#endif

#ifndef DEFAULT_SCREENTIMEOUT
#define DEFAULT_SCREENTIMEOUT	0	// minutes, 0 = never
#endif

#ifndef DEFAULT_NIGHTDIM
#define DEFAULT_NIGHTDIM	false
#endif

#ifndef DEFAULT_NIGHTBRIGHTNESS
#define DEFAULT_NIGHTBRIGHTNESS	40	// backlight duty 1-255
#endif

#ifndef DEFAULT_NIGHTFADE
#define DEFAULT_NIGHTFADE	60	// minutes, half before and half after sunset / sunrise
#endif

// JSON config files
#define JSON_CONFIG_FILE	"/config.json"

// JSON config file SD card (for user interaction, readme.md)
#define JSON_KEY_SSID		"SSID"
#define JSON_KEY_PASW		"WifiPW"
#define JSON_KEY_POOLURL	"PoolUrl"
#define JSON_KEY_POOLPASS	"PoolPassword"
#define JSON_KEY_WALLETID	"BtcWallet"
#define JSON_KEY_POOLPORT	"PoolPort"
#define JSON_KEY_TIMEZONE	"Timezone"
#define JSON_KEY_STATS2NV	"SaveStats"
#define JSON_KEY_INVCOLOR	"invertColors"
#define JSON_KEY_BRIGHTNESS	"Brightness"
#define JSON_KEY_FLIPDISPLAY	"flipDisplay"
#define JSON_KEY_CURRENCY	"Currency"
#define JSON_KEY_AUTOBRIGHTNESS	"autoBrightness"
#define JSON_KEY_SCREENOFFTIMEOUT	"screenOffTimeout"

// JSON config file SPIFFS (different for backward compatibility with existing devices)
#define JSON_SPIFFS_KEY_POOLURL		"poolString"
#define JSON_SPIFFS_KEY_POOLPORT	"portNumber"
#define JSON_SPIFFS_KEY_POOLPASS	"poolPassword"
#define JSON_SPIFFS_KEY_WALLETID	"btcString"
#define JSON_SPIFFS_KEY_TIMEZONE	"gmtZone"
#define JSON_SPIFFS_KEY_STATS2NV	"saveStatsToNVS"
#define JSON_SPIFFS_KEY_INVCOLOR	"invertColors"
#define JSON_SPIFFS_KEY_BRIGHTNESS	"Brightness"
#define JSON_SPIFFS_KEY_FLIPDISPLAY	"flipDisplay"
#define JSON_SPIFFS_KEY_CURRENCY	"currency"
#define JSON_SPIFFS_KEY_AUTOBRIGHTNESS	"autoBrightness"
#define JSON_SPIFFS_KEY_SCREENOFFTIMEOUT	"screenOffTimeout"
#define JSON_SPIFFS_KEY_STATUSLED	"statusLed"
#define JSON_SPIFFS_KEY_SCREENTIMEOUT	"screenTimeout"
#define JSON_SPIFFS_KEY_NIGHTDIM	"nightDim"
#define JSON_SPIFFS_KEY_NIGHTBRIGHTNESS	"nightBrightness"
#define JSON_SPIFFS_KEY_NIGHTFADE	"nightFade"
#define JSON_SPIFFS_KEY_LATITUDE	"latitude"
#define JSON_SPIFFS_KEY_LONGITUDE	"longitude"

// settings
struct TSettings
{
	String WifiSSID{ DEFAULT_SSID };
	String WifiPW{ DEFAULT_WIFIPW };
	String PoolAddress{ DEFAULT_POOLURL };
	char BtcWallet[80]{ DEFAULT_WALLETID };
	char PoolPassword[80]{ DEFAULT_POOLPASS };
	int PoolPort{ DEFAULT_POOLPORT };
	String Timezone{ DEFAULT_TIMEZONE };
	bool saveStats{ DEFAULT_SAVESTATS };
	bool invertColors{ DEFAULT_INVERTCOLORS };
	int Brightness{ DEFAULT_BRIGHTNESS };
	bool flipDisplay{ DEFAULT_FLIPDISPLAY };	// rotate display 180 degrees (USB on the left)
	String Currency{ DEFAULT_CURRENCY };	// fiat currency for the BTC price (CoinGecko vs_currency code)
	bool autoBrightness{ DEFAULT_AUTOBRIGHTNESS }; // automatic screen brightness via LDR sensor
	int screenOffTimeout{ DEFAULT_SCREENOFFTIMEOUT }; // auto screen off timeout in seconds (0 = disabled)
	bool statusLed{ DEFAULT_STATUSLED };	// boards without a screen: status LED on/off
	int ScreenTimeout{ DEFAULT_SCREENTIMEOUT };	// minutes without a touch until the backlight goes off, 0 = never
	// Night dimming: the backlight fades from Brightness to NightBrightness across
	// NightFade minutes centred on sunset, and back at sunrise, at this location.
	bool nightDim{ DEFAULT_NIGHTDIM };
	int NightBrightness{ DEFAULT_NIGHTBRIGHTNESS };
	int NightFade{ DEFAULT_NIGHTFADE };
	float Latitude{ 0 };	// decimal degrees, north positive; 0,0 = not set
	float Longitude{ 0 };	// decimal degrees, east positive
};

#endif // _STORAGE_H_