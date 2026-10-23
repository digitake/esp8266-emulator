#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <string.h>

// Small MQTT 3.1.1 QoS 0 client for the emulator's broker and OTA harness.
// Build with REFERENCE_VERSION="A" or "B". Version A accepts an HTTP URL
// published to emulator/ota; B only reports its version.
#ifndef REFERENCE_VERSION
#define REFERENCE_VERSION "A"
#endif
#ifndef REFERENCE_UPDATE_ENABLED
#define REFERENCE_UPDATE_ENABLED 1
#endif

static const char *const WIFI_SSID = "emu-test";
static const char *const WIFI_PASSWORD = "emu-pass";
static const char *const BROKER_HOST = "broker.example.test";
static const uint16_t BROKER_PORT = 1883;
static const char *const VERSION_TOPIC = "emulator/version";
static const char *const OTA_TOPIC = "emulator/ota";
static const char *const CONFIG_TOPIC = "emulator/config";
static const char *const CLIENT_ID = "esp8266-reference";
static const char *const VERSION = REFERENCE_VERSION;
static const uint8_t CONFIG_MARKER[] = {'n', 'e', 't', '-', 'v', '1'};
static const size_t CONFIG_MARKER_SIZE = sizeof(CONFIG_MARKER);

static WiFiClient mqtt;
static WiFiClient updateClient;
static ESP8266HTTPUpdate updater(12000);
static ESP8266WebServer webServer(80);
static ESP8266HTTPUpdateServer httpUpdater;
static uint16_t mqttPacketId = 1;
static uint32_t lastPingMs = 0;
static bool webServerStarted = false;
static bool configMarkerPresent = false;
static uint8_t networkLifecycle = 0;
static bool lifecycleModeOffOk = false;
static bool lifecycleStationOnOk = false;

static bool writeAll(const uint8_t *data, size_t size)
{
    size_t sent = 0;
    uint32_t start = millis();
    while (sent < size && millis() - start < 5000) {
        int n = mqtt.write(data + sent, size - sent);
        if (n > 0) sent += (size_t)n;
        else delay(1);
    }
    return sent == size;
}

static size_t writeRemainingLength(uint8_t *out, size_t value)
{
    size_t n = 0;
    do {
        uint8_t byte = value % 128;
        value /= 128;
        if (value) byte |= 0x80;
        out[n++] = byte;
    } while (value && n < 4);
    return n;
}

static bool writeMqttString(uint8_t *out, size_t capacity, size_t &at,
                            const char *value)
{
    size_t len = strlen(value);
    if (len > 65535 || at + 2 + len > capacity) return false;
    out[at++] = (uint8_t)(len >> 8);
    out[at++] = (uint8_t)len;
    memcpy(out + at, value, len);
    at += len;
    return true;
}

static bool sendConnect(void)
{
    uint8_t packet[64];
    size_t at = 0;
    if (!writeMqttString(packet, sizeof packet, at, "MQTT")) return false;
    packet[at++] = 4; // MQTT 3.1.1
    packet[at++] = 2; // clean session; no credentials
    packet[at++] = 0;
    packet[at++] = 30; // 30-second keepalive
    if (!writeMqttString(packet, sizeof packet, at, CLIENT_ID)) return false;
    uint8_t header[5] = { 0x10 };
    size_t hn = writeRemainingLength(header + 1, at);
    return writeAll(header, hn + 1) && writeAll(packet, at);
}

static bool sendSubscribe(void)
{
    uint8_t payload[96];
    size_t at = 0;
    const uint16_t id = mqttPacketId++;
    payload[at++] = (uint8_t)(id >> 8);
    payload[at++] = (uint8_t)id;
    if (!writeMqttString(payload, sizeof payload, at, OTA_TOPIC)) return false;
    payload[at++] = 0; // QoS 0
    uint8_t header[5] = { 0x82 };
    size_t hn = writeRemainingLength(header + 1, at);
    return writeAll(header, hn + 1) && writeAll(payload, at);
}

static bool sendPublish(const char *topic, const char *message)
{
    uint8_t payload[256];
    size_t at = 0;
    if (!writeMqttString(payload, sizeof payload, at, topic)) return false;
    size_t len = strlen(message);
    if (at + len > sizeof payload) return false;
    memcpy(payload + at, message, len);
    at += len;
    uint8_t header[5] = { 0x30 };
    size_t hn = writeRemainingLength(header + 1, at);
    return writeAll(header, hn + 1) && writeAll(payload, at);
}

static bool readByte(uint8_t &value, uint32_t timeoutMs)
{
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (mqtt.available()) {
            int c = mqtt.read();
            if (c >= 0) { value = (uint8_t)c; return true; }
        }
        if (!mqtt.connected()) return false;
        delay(1);
    }
    return false;
}

static bool readPacket(uint8_t &header, uint8_t *payload, size_t capacity,
                       size_t &length, uint32_t timeoutMs)
{
    uint8_t byte;
    if (!readByte(header, timeoutMs)) return false;
    size_t remaining = 0;
    size_t multiplier = 1;
    bool complete = false;
    for (unsigned i = 0; i < 4; i++) {
        if (!readByte(byte, timeoutMs)) return false;
        remaining += (byte & 0x7f) * multiplier;
        if (!(byte & 0x80)) { complete = true; break; }
        multiplier *= 128;
    }
    if (!complete || remaining > capacity) return false;
    for (size_t i = 0; i < remaining; i++)
        if (!readByte(payload[i], timeoutMs)) return false;
    length = remaining;
    return true;
}

static bool connectBroker(void)
{
    if (!mqtt.connect(BROKER_HOST, BROKER_PORT)) return false;
    if (!sendConnect()) { mqtt.stop(); return false; }
    uint8_t header, payload[128];
    size_t length;
    if (!readPacket(header, payload, sizeof payload, length, 5000) ||
        header != 0x20 || length != 2 || payload[1] != 0) {
        mqtt.stop();
        return false;
    }
    if (!sendSubscribe()) { mqtt.stop(); return false; }
    lastPingMs = millis();
    Serial.println("MQTT connected");
    return true;
}

static void runHttpUpdate(const uint8_t *payload, size_t length)
{
    if (length == 0 || length >= 384) {
        Serial.println("OTA request rejected: URL length");
        return;
    }
    char url[384];
    memcpy(url, payload, length);
    url[length] = '\0';
    if (strncmp(url, "http://", 7) != 0) {
        Serial.println("OTA request rejected: expected http:// URL");
        return;
    }
    Serial.printf("OTA update requested: %s\n", url);
    HTTPUpdateResult result = updater.update(updateClient, String(url), VERSION);
    if (result == HTTP_UPDATE_OK) Serial.println("OTA update applied");
    else if (result == HTTP_UPDATE_NO_UPDATES) Serial.println("OTA image unchanged");
    else Serial.printf("OTA update failed: %d\n", updater.getLastError());
}

static void handlePacket(uint8_t header, const uint8_t *payload, size_t length)
{
    uint8_t type = header >> 4;
    if (type == 9) { // SUBACK
        sendPublish(VERSION_TOPIC, VERSION);
        Serial.printf("firmware version %s\n", VERSION);
        sendPublish(CONFIG_TOPIC, configMarkerPresent ? "persisted-v1" : "missing");
        Serial.printf("configuration marker %s\n",
                      configMarkerPresent ? "persisted-v1" : "missing");
        return;
    }
    if (type == 3 && length >= 2) { // incoming PUBLISH; QoS 0
        size_t topicLength = ((size_t)payload[0] << 8) | payload[1];
        if (topicLength + 2 > length) return;
        const uint8_t *body = payload + 2 + topicLength;
        size_t bodyLength = length - 2 - topicLength;
        if (topicLength == strlen(OTA_TOPIC) &&
            memcmp(payload + 2, OTA_TOPIC, topicLength) == 0) {
#if REFERENCE_UPDATE_ENABLED
            runHttpUpdate(body, bodyLength);
#else
            Serial.println("OTA trigger ignored by version B");
#endif
        }
    }
}

static void serviceBroker(void)
{
    if (!mqtt.connected()) {
        return;
    }
    uint8_t header, payload[512];
    size_t length;
    if (mqtt.available()) {
        if (!readPacket(header, payload, sizeof payload, length, 5000)) {
            mqtt.stop();
            return;
        }
        handlePacket(header, payload, length);
    }
    if (millis() - lastPingMs >= 15000) {
        const uint8_t ping[] = { 0xC0, 0x00 };
        if (!writeAll(ping, sizeof ping)) mqtt.stop();
        lastPingMs = millis();
    }
}

void setup()
{
    Serial.begin(74880);
    Serial.println();
    Serial.printf("network reference firmware %s\n", VERSION);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    updater.rebootOnUpdate(true);
    EEPROM.begin(32);
    bool matches = true;
    for (size_t i = 0; i < CONFIG_MARKER_SIZE; i++)
        if (EEPROM.read(i) != CONFIG_MARKER[i]) matches = false;
    if (strcmp(VERSION, "A") == 0 && !matches) {
        for (size_t i = 0; i < CONFIG_MARKER_SIZE; i++) EEPROM.write(i, CONFIG_MARKER[i]);
        EEPROM.commit();
        matches = true;
    }
    configMarkerPresent = matches;
    httpUpdater.setup(&webServer);
}

void loop()
{
    if (WiFi.status() != WL_CONNECTED) {
        if (mqtt.connected()) mqtt.stop();
        delay(100);
        return;
    }
    if (networkLifecycle == 0) {
        bool initiallyDhcp = WiFi.localIP() == IPAddress(10, 0, 2, 15);
        lifecycleModeOffOk = WiFi.mode(WIFI_OFF);
        delay(100);
        lifecycleModeOffOk = initiallyDhcp && lifecycleModeOffOk &&
                             WiFi.status() != WL_CONNECTED &&
                             WiFi.localIP() == IPAddress(0, 0, 0, 0);
        Serial.printf("network lifecycle mode-off %s\n",
                      lifecycleModeOffOk ? "disconnected" : "failed");
        lifecycleStationOnOk = WiFi.mode(WIFI_STA);
        if (lifecycleStationOnOk) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        networkLifecycle = 1;
        return;
    }
    if (networkLifecycle == 1) {
        if (WiFi.status() != WL_CONNECTED) {
            delay(100);
            return;
        }
        lifecycleStationOnOk = lifecycleStationOnOk &&
                               WiFi.localIP() == IPAddress(10, 0, 2, 15);
        networkLifecycle = 2;
        Serial.printf("network lifecycle station %s\n",
                      lifecycleModeOffOk && lifecycleStationOnOk ? "dhcp restored" : "failed");
    }
    if (!webServerStarted) {
        webServer.begin();
        webServerStarted = true;
        Serial.println("web updater listening on port 80");
    }
    webServer.handleClient();
    if (!mqtt.connected()) {
        if (!connectBroker()) delay(500);
        return;
    }
    serviceBroker();
    delay(1);
}
