// MQTT publisher, see mqttPublisher.h. Uses ESP-IDF's MQTT client (esp-mqtt,
// already in the Arduino core): no extra library. The client runs in its own
// task and reconnects by itself; this file only starts/stops it and publishes.
#ifndef NO_MQTT // see mqttPublisher.h
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <mqtt_client.h>
#include <esp_idf_version.h>
#include <mutex>
#include "mqttPublisher.h"
#include "mining.h"
#include "webConfig.h"
#include "wManager.h"
#include "version.h"
#include "drivers/displays/display.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"

extern TSettings Settings;
extern nvMemory nvMem;

#define MQTT_PUBLISH_ms (30 * 1000UL)
#define MQTT_DISCOVERY_PREFIX "homeassistant"

static esp_mqtt_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;
static volatile bool s_announce = false; // send discovery + "online" (set on connect)
static bool s_restart = true;            // (re)read the settings and (re)start
static uint32_t s_lastPublish = 0;
// The client keeps pointers into these while it runs
static String s_uri, s_user, s_pass, s_clientId, s_base, s_availTopic, s_cmdTopic, s_node;
static std::mutex s_cmdMutex;
static String s_pendingCmd;

void mqttSettingsChanged(void) { s_restart = true; }

String mqttCleanHost(const String &in, int &port)
{
  String h = in;
  h.trim();
  const int scheme = h.indexOf("://"); // mqtt://, tcp://; mqtts:// would need TLS, not supported
  if (scheme >= 0)
    h = h.substring(scheme + 3);
  const int slash = h.indexOf('/');
  if (slash >= 0)
    h = h.substring(0, slash);
  const int colon = h.lastIndexOf(':');
  if (colon > 0 && h.indexOf(':') == colon) // host:port (an IPv6 address has several ':')
  {
    const long p = h.substring(colon + 1).toInt();
    if (p > 0 && p < 65536)
      port = (int)p;
    h = h.substring(0, colon);
  }
  return h;
}
bool mqttConnected(void) { return s_client && s_connected; }

static void onMqttEvent(void *, esp_event_base_t, int32_t id, void *event_data)
{
  auto ev = static_cast<esp_mqtt_event_handle_t>(event_data);
  if (id == MQTT_EVENT_CONNECTED)
  {
    s_connected = true;
    s_announce = true;
    if (s_cmdTopic.length())
      esp_mqtt_client_subscribe(s_client, s_cmdTopic.c_str(), 0);
    Serial.println("[MQTT] connected");
  }
  else if (id == MQTT_EVENT_DISCONNECTED)
  {
    if (s_connected)
      Serial.println("[MQTT] disconnected");
    s_connected = false;
  }
  else if (id == MQTT_EVENT_DATA && ev && ev->topic && ev->data && ev->data_len > 0 && ev->data_len <= 512)
  {
    if ((size_t)ev->topic_len == s_cmdTopic.length() &&
        memcmp(ev->topic, s_cmdTopic.c_str(), ev->topic_len) == 0)
    {
      char buf[513];
      memcpy(buf, ev->data, ev->data_len);
      buf[ev->data_len] = '\0';
      std::lock_guard<std::mutex> lock(s_cmdMutex);
      s_pendingCmd = buf;
    }
  }
}

static void stopClient()
{
  if (!s_client)
    return;
  if (s_connected)
  {
    // A clean disconnect doesn't trigger the last will: say "offline" before
    // going, or Home Assistant keeps showing the miner as available (with its
    // last values) after MQTT is switched off or moved to another broker.
    // publish() has written it to the socket when it returns, and the
    // DISCONNECT that follows on the same connection reaches the broker after
    // it: no need to hold the loop up with a delay.
    esp_mqtt_client_publish(s_client, s_availTopic.c_str(), "offline", 0, 1, 1);
    esp_mqtt_client_disconnect(s_client);
  }
  esp_mqtt_client_stop(s_client);
  esp_mqtt_client_destroy(s_client);
  s_client = nullptr;
  s_connected = false;
}

static String nodeId()
{
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[24];
  snprintf(id, sizeof(id), "nerdminer_%02x%02x%02x", mac[3], mac[4], mac[5]);
  return String(id);
}

static void startClient()
{
  s_node = nodeId();
  s_base = Settings.MqttTopic;
  s_base.trim();
  while (s_base.endsWith("/"))
    s_base.remove(s_base.length() - 1);
  if (s_base.isEmpty())
    s_base = "nerdminer/" + s_node.substring(10); // nerdminer/<last 3 MAC bytes>
  s_availTopic = s_base + "/status";
  s_cmdTopic = s_base + "/cmd";
  int port = Settings.MqttPort;
  const String host = mqttCleanHost(Settings.MqttHost, port); // also for a value saved before the page cleaned it
  s_uri = "mqtt://" + host + ":" + String(port);
  s_user = Settings.MqttUser;
  s_pass = Settings.MqttPass;
  s_clientId = s_node;

  esp_mqtt_client_config_t cfg = {};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  // ESP-IDF 5 (Arduino core 3) groups the fields
  cfg.broker.address.uri = s_uri.c_str();
  cfg.credentials.client_id = s_clientId.c_str();
  if (s_user.length())
    cfg.credentials.username = s_user.c_str();
  if (s_pass.length())
    cfg.credentials.authentication.password = s_pass.c_str();
  cfg.session.last_will.topic = s_availTopic.c_str(); // the broker says "offline" for us if we vanish
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = 60;
#else
  cfg.uri = s_uri.c_str();
  cfg.client_id = s_clientId.c_str();
  if (s_user.length())
    cfg.username = s_user.c_str();
  if (s_pass.length())
    cfg.password = s_pass.c_str();
  cfg.lwt_topic = s_availTopic.c_str(); // the broker says "offline" for us if we vanish
  cfg.lwt_msg = "offline";
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.keepalive = 60;
#endif
  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client)
  {
    Serial.println("[MQTT] client init failed");
    return;
  }
  esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, onMqttEvent, nullptr);
  esp_mqtt_client_start(s_client);
  Serial.printf("[MQTT] connecting to %s, topic %s\n", s_uri.c_str(), s_base.c_str());
}

static void publish(const String &topic, const String &payload, bool retain)
{
  esp_mqtt_client_publish(s_client, topic.c_str(), payload.c_str(), payload.length(), 1, retain ? 1 : 0);
}

// One Home Assistant sensor reading a field of the state message.
static void announceSensor(const char *key, const char *name, const char *tpl, const char *unit,
                           const char *devClass, const char *stateClass, const char *icon)
{
  DynamicJsonDocument d(1024);
  d["name"] = name;
  d["uniq_id"] = s_node + "_" + key;
  // Home Assistant deprecated object_id (obj_id) for default_entity_id, a
  // full entity id; versions that don't know the key just ignore it.
  d["default_entity_id"] = "sensor." + s_node + "_" + key;
  d["stat_t"] = s_base + "/state";
  d["val_tpl"] = tpl;
  d["avty_t"] = s_availTopic;
  if (unit) d["unit_of_meas"] = unit;
  if (devClass) d["dev_cla"] = devClass;
  if (stateClass) d["stat_cla"] = stateClass;
  if (icon) d["ic"] = icon;
  JsonObject dev = d.createNestedObject("dev");
  dev.createNestedArray("ids").add(s_node);
  dev["name"] = "NerdMiner " + s_node.substring(10);
  dev["mf"] = "NerdMiner Modded";
  dev["mdl"] = ESP.getChipModel();
  dev["sw"] = CURRENT_VERSION;
  dev["cu"] = "http://" + WiFi.localIP().toString() + "/";
  String out;
  serializeJson(d, out);
  publish(String(MQTT_DISCOVERY_PREFIX "/sensor/") + s_node + "/" + key + "/config", out, true);
}

static void announceButton(const char *id, const char *name, const char *payload, const char *icon)
{
  DynamicJsonDocument d(768);
  d["name"] = name;
  d["uniq_id"] = s_node + "_" + id;
  d["default_entity_id"] = "button." + s_node + "_" + id;
  d["command_topic"] = s_cmdTopic;
  d["payload_press"] = payload;
  d["avty_t"] = s_availTopic;
  if (icon) d["ic"] = icon;
  JsonObject dev = d.createNestedObject("dev");
  dev.createNestedArray("ids").add(s_node);
  dev["name"] = "NerdMiner " + s_node.substring(10);
  dev["mf"] = "NerdMiner Modded";
  dev["mdl"] = ESP.getChipModel();
  dev["sw"] = CURRENT_VERSION;
  dev["cu"] = "http://" + WiFi.localIP().toString() + "/";
  String out;
  serializeJson(d, out);
  publish(String(MQTT_DISCOVERY_PREFIX "/button/") + s_node + "/" + id + "/config", out, true);
}

static void announce()
{
  publish(s_availTopic, "online", true);
  announceSensor("hashrate", "Hashrate", "{{ value_json.hashrate_khs }}", "kH/s", nullptr, "measurement", "mdi:pickaxe");
  announceSensor("hashrate_avg", "Average hashrate", "{{ value_json.hashrate_avg_khs }}", "kH/s", nullptr, "measurement", "mdi:pickaxe");
  announceSensor("best_diff", "Best difficulty", "{{ value_json.best_diff }}", nullptr, nullptr, "measurement", "mdi:trophy");
  announceSensor("session_best_difficulty", "Session best difficulty", "{{ value_json.session_best_difficulty }}", nullptr, nullptr, "measurement", "mdi:trophy-award");
  announceSensor("shares_32bit", "32-bit shares", "{{ value_json.shares_32bit }}", nullptr, nullptr, "total_increasing", "mdi:check-decagram");
  announceSensor("shares_accepted", "Pool shares accepted", "{{ value_json.shares_accepted }}", nullptr, nullptr, "total_increasing", "mdi:check");
  announceSensor("shares_rejected", "Pool shares rejected", "{{ value_json.shares_rejected }}", nullptr, nullptr, "total_increasing", "mdi:close");
  announceSensor("blocks_found", "Blocks found", "{{ value_json.blocks_found }}", nullptr, nullptr, "total_increasing", "mdi:cube-outline");
  announceSensor("templates", "Block templates", "{{ value_json.templates }}", nullptr, nullptr, "total_increasing", "mdi:file-document-outline");
  announceSensor("mining_time", "Mining time", "{{ value_json.mining_time_s }}", "s", "duration", nullptr, nullptr);
  announceSensor("pool_difficulty", "Pool difficulty", "{{ value_json.pool.difficulty }}", nullptr, nullptr, "measurement", "mdi:tune");
  announceSensor("pool_ping", "Pool ping", "{{ value_json.pool.ping_ms | default(0) }}", "ms", "duration", "measurement", "mdi:lan-connect");
  announceSensor("pool", "Pool", "{{ value_json.pool.address }}", nullptr, nullptr, nullptr, "mdi:server-network");
  announceSensor("luck", "Mining luck", "{{ value_json.luck_pct | default(0) }}", "%", nullptr, "measurement", "mdi:clover");
  announceSensor("rssi", "Wi-Fi signal", "{{ value_json.wifi.rssi }}", "dBm", "signal_strength", "measurement", nullptr);
  announceSensor("uptime", "Uptime", "{{ value_json.uptime_s }}", "s", "duration", nullptr, nullptr);
#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
  announceSensor("battery", "Battery", "{{ value_json.battery.percent }}", "%", "battery", "measurement", nullptr);
  announceSensor("battery_voltage", "Battery voltage", "{{ (value_json.battery.mv / 1000) | round(2) }}", "V", "voltage", "measurement", nullptr);
#endif
  announceButton("identify", "Identify Miner", "{\"identify\":true}", "mdi:bullseye-arrow");
  announceButton("screen_toggle", "Toggle Screen", "{\"screen\":\"toggle\"}", "mdi:television-ambient-light");
}

static void publishState()
{
  DynamicJsonDocument doc(2560);
  fillMinerStatusJson(doc);
  String out;
  serializeJson(doc, out);
  publish(s_base + "/state", out, false);
}

static void handleCommand(String cmd)
{
  cmd.trim();
  if (cmd.isEmpty())
    return;
  Serial.printf("[MQTT] cmd: %s\n", cmd.c_str());

  bool handled = false;
  bool doReboot = false;

  if (cmd.startsWith("{"))
  {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, cmd.c_str()))
    {
      if (doc.containsKey("identify"))
      {
        if (doc["identify"].is<bool>())
        {
          if (doc["identify"].as<bool>())
          {
            triggerIdentify(10000);
            handled = true;
          }
        }
        else if (doc["identify"].is<int>() && doc["identify"].as<int>() > 0)
        {
          triggerIdentify((uint32_t)doc["identify"].as<int>());
          handled = true;
        }
      }
      if (doc.containsKey("screen"))
      {
        String s = doc["screen"].as<String>();
        s.toLowerCase();
        if (s == "on")
        {
          wakeScreen();
          handled = true;
        }
        else if (s == "off")
        {
          sleepScreen();
          handled = true;
        }
        else if (s == "toggle")
        {
          alternateScreenState();
          handled = true;
        }
      }
      if (doc.containsKey("page") && doc["page"].is<int>())
      {
        int p = doc["page"].as<int>();
        if (p >= 0 && currentDisplayDriver && currentDisplayDriver->num_cyclic_screens > 0)
        {
          currentDisplayDriver->current_cyclic_screen = p % currentDisplayDriver->num_cyclic_screens;
          wakeScreen();
          handled = true;
        }
      }
      bool saveLed = false;
      TSettings ledSnap;
      {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (doc.containsKey("ledMode") && doc["ledMode"].is<int>())
        {
          Settings.ledMode = constrain(doc["ledMode"].as<int>(), 0, 2);
          saveLed = true;
        }
        if (doc.containsKey("ledBrightness") && doc["ledBrightness"].is<int>())
        {
          Settings.ledBrightness = constrain(doc["ledBrightness"].as<int>(), 1, 255);
          saveLed = true;
        }
        if (saveLed)
          ledSnap = Settings;
      }
      if (saveLed)
      {
        nvMem.saveConfig(&ledSnap);
        handled = true;
      }
      if (doc.containsKey("reboot") && doc["reboot"].as<bool>())
      {
        doReboot = true;
        handled = true;
      }
    }
  }
  else
  {
    String kw = cmd;
    kw.toLowerCase();
    if (kw == "identify")
    {
      triggerIdentify(10000);
      handled = true;
    }
    else if (kw == "screen_on")
    {
      wakeScreen();
      handled = true;
    }
    else if (kw == "screen_off")
    {
      sleepScreen();
      handled = true;
    }
    else if (kw == "screen_toggle")
    {
      alternateScreenState();
      handled = true;
    }
    else if (kw == "next_page")
    {
      if (currentDisplayDriver && currentDisplayDriver->num_cyclic_screens > 0)
      {
        currentDisplayDriver->current_cyclic_screen =
            (currentDisplayDriver->current_cyclic_screen + 1) % currentDisplayDriver->num_cyclic_screens;
      }
      wakeScreen();
      handled = true;
    }
    else if (kw == "reboot")
    {
      doReboot = true;
      handled = true;
    }
  }

  if (doReboot)
  {
    saveStat();
    delay(200);
    ESP.restart();
  }

  if (handled)
  {
    s_lastPublish = millis() - MQTT_PUBLISH_ms;
  }
}

void mqttProcess(void)
{
  if (s_restart)
  {
    s_restart = false;
    stopClient();
    if (Settings.MqttEnabled && Settings.MqttHost.length() && WiFi.status() == WL_CONNECTED)
      startClient();
    else if (Settings.MqttEnabled && Settings.MqttHost.length())
      s_restart = true; // try again once the Wi-Fi is up
  }

  String cmd;
  {
    std::lock_guard<std::mutex> lock(s_cmdMutex);
    if (s_pendingCmd.length())
    {
      cmd = s_pendingCmd;
      s_pendingCmd = String();
    }
  }
  if (cmd.length())
    handleCommand(cmd);

  if (!mqttConnected())
    return;
  if (s_announce)
  {
    s_announce = false;
    announce();
    s_lastPublish = millis() - MQTT_PUBLISH_ms; // state straight away
  }
  if (millis() - s_lastPublish >= MQTT_PUBLISH_ms)
  {
    s_lastPublish = millis();
    publishState();
  }
}
#endif // NO_MQTT
