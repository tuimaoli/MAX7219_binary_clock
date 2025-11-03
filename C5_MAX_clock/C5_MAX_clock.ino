#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "mis_font.h"

// ----------------- 硬件定义 -----------------
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   1
#define DATA_PIN      24
#define CLK_PIN       23
#define CS_PIN        2
#define BUTTON_PIN    3

// ----------------- WiFi & MQTT -----------------
const char* WIFI_SSID = "WIFI_Demo";
const char* WIFI_PASS = "18638227200";
const char* MQTT_SERVER = "192.168.124.13";
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "mqttuser";
const char* MQTT_PASS   = "123456";
const char* MQTT_TOPIC_FANS    = "dfrobot/fans";
const char* MQTT_TOPIC_WEATHER = "dfrobot/weather";
const char* MQTT_TOPIC_CONFIG  = "devices/config";

// ----------------- 显示对象 -----------------
MD_Parola display = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// ----------------- 状态 -----------------
volatile bool buttonIRQ = false;
unsigned long lastDebounceMillis = 0;
unsigned long lastActivityMillis = 0;
const unsigned long debounceMs = 200;
const unsigned long displayTimeoutMs = 15000;
uint8_t displayStage = 0; // 0=息屏,1=时间粗,2=笑脸,3=时间细,4=多区域

volatile int currentFans = -1;
volatile int lastFans = -1;

// ----------------- MQTT -----------------
WiFiClient netClient;
PubSubClient mqtt(netClient);
Preferences prefs;

// ----------------- 数据结构 -----------------
typedef struct {
  uint8_t hour_ten, hour_units;
  uint8_t min_ten,  min_units;
  uint8_t sec_ten,  sec_units;
  uint8_t month_ten, month_units;
  uint8_t day_ten,   day_units;
  uint8_t temp_ten,  temp_units;
  uint8_t hum_ten,   hum_units;
  uint8_t fans_ten,  fans_units;
} show_data_s;

// ----------------- 按键 ISR -----------------
void IRAM_ATTR buttonISR() { buttonIRQ = true; }

// ----------------- 按键轮询 -----------------
bool pollButtonPressed() {
  static bool lastState = HIGH;
  bool current = digitalRead(BUTTON_PIN);
  unsigned long now = millis();
  if (current != lastState) lastDebounceMillis = now;
  if ((now - lastDebounceMillis) > debounceMs) {
    if (lastState == HIGH && current == LOW) {
      lastState = current;
      return true;
    }
  }
  lastState = current;
  return false;
}

// ----------------- 显示函数（保留原有） -----------------
void drawImageEx(const uint8_t img[8], bool mirrorX = true, bool mirrorY = false)
{
  mx.clear();
  for (uint8_t row = 0; row < 8; row++) {
    uint8_t srcRow = mirrorY ? (7 - row) : row;
    uint8_t rowData = img[srcRow];
    for (uint8_t col = 0; col < 8; col++) {
      uint8_t srcBitIndex = mirrorX ? col : (7 - col);
      bool pixel = (rowData >> srcBitIndex) & 0x01;
      mx.setPoint(row, col, pixel);
    }
  }
}

void drawImage(const uint8_t img[8])
{
  drawImageEx(img, true, True);
}

// ----------------- 通用二进制显示函数 -----------------
// d1=小时十位, d2=小时个位, d3=分钟十位, d4=分钟个位
// thickness=1 表示 1x1, thickness=2 表示 2x2
// offsetX, offsetY 表示 d1 的最高位(即 bit3)的左上角坐标
void displayBinaryDigits(uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4,
                         uint8_t thickness, uint8_t offsetX, uint8_t offsetY)
{
  uint8_t digits[4] = { d1, d2, d3, d4 };
  for (uint8_t c = 0; c < 4; c++) {          // 4 个数字
    for (uint8_t r = 0; r < 4; r++) {        // 每个数字的 4 位二进制 (bit3在上，bit0在下)
      bool on = (digits[c] >> r) & 0x01;
      if (!on) continue;
      uint8_t baseX = offsetX + c * thickness;
      uint8_t baseY = offsetY + r * thickness;
      for (uint8_t dx = 0; dx < thickness; dx++) {
        for (uint8_t dy = 0; dy < thickness; dy++) {
          int x = baseX + dx;
          int y = baseY + dy;
          if (x < 8 && y < 8) mx.setPoint(y, x, true);
        }
      }
    }
  }
}

// ----------------- 转换函数 -----------------
show_data_s convertTimeData(struct tm* timeinfo, int temp, int hum, int fans) {
  show_data_s d;
  d.hour_ten   = timeinfo->tm_hour / 10;
  d.hour_units = timeinfo->tm_hour % 10;
  d.min_ten    = timeinfo->tm_min / 10;
  d.min_units  = timeinfo->tm_min % 10;
  d.sec_ten    = timeinfo->tm_sec / 10;
  d.sec_units  = timeinfo->tm_sec % 10;
  d.month_ten  = (timeinfo->tm_mon + 1) / 10;
  d.month_units= (timeinfo->tm_mon + 1) % 10;
  d.day_ten    = timeinfo->tm_mday / 10;
  d.day_units  = timeinfo->tm_mday % 10;
  d.temp_ten   = temp / 10;
  d.temp_units = temp % 10;
  d.hum_ten    = hum / 10;
  d.hum_units  = hum % 10;
  d.fans_ten   = fans / 10;
  d.fans_units = fans % 10;
  return d;
}

// ----------------- MQTT回调 -----------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);

  if (t == MQTT_TOPIC_CONFIG) {
    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, msg) == DeserializationError::Ok) {
      if (doc.containsKey("ssid") && doc.containsKey("pass")) {
        String ssid = doc["ssid"].as<String>();
        String pass = doc["pass"].as<String>();
        prefs.putString("wifi_ssid", ssid);
        prefs.putString("wifi_pass", pass);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_MODE_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
      }
    }
  }

  if (t == MQTT_TOPIC_FANS) {
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, payload, length) == DeserializationError::Ok) {
      if (doc.containsKey("fans")) {
        lastFans = currentFans;
        currentFans = doc["fans"].as<int>();
        Serial.printf("粉丝数更新: %d\n", currentFans);
      }
    }
  }

  if (t == MQTT_TOPIC_WEATHER) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
      if (doc.containsKey("now")) {
        JsonObject now = doc["now"];
        const char* temp     = now["temp"];
        const char* text     = now["text"];
        const char* windDir  = now["windDir"];
        const char* humidity = now["humidity"];
        const char* obsTime  = now["obsTime"];
        Serial.printf("观测时间: %s\n", obsTime);
        Serial.printf("温度: %s°C, 天气: %s\n", temp, text);
        Serial.printf("风向: %s, 湿度: %s%%\n", windDir, humidity);
      }
    } else {
      Serial.println("Weather JSON parse error");
    }
  }
}

// ----------------- MQTT任务 -----------------
void mqttTask(void* pv) {
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  const TickType_t tick50 = pdMS_TO_TICKS(50);
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqtt.connected()) {
        String clientId = "ESP32Client-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
          mqtt.subscribe(MQTT_TOPIC_FANS);
          mqtt.subscribe(MQTT_TOPIC_CONFIG);
          mqtt.subscribe(MQTT_TOPIC_WEATHER);
          Serial.println("MQTT connected");
        }
      }
      mqtt.loop();
    }
    vTaskDelay(tick50);
  }
}

// ----------------- WiFi任务 -----------------
void connectWiFi(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_MODE_STA);
  Serial.printf("→ WiFi connecting: %s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n→ WiFi IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n→ WiFi failed");
  }
}

void wifiTask(void* pv) {
  const TickType_t retry = pdMS_TO_TICKS(5000);
  bool ntpDone = false;
  String ssid = prefs.getString("wifi_ssid", String(WIFI_SSID));
  String pass = prefs.getString("wifi_pass", String(WIFI_PASS));
  Serial.println(ssid);
  Serial.println(pass);
  connectWiFi(ssid, pass);
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("→ Wi‑Fi lost, retry...");
      connectWiFi(ssid, pass);
      vTaskDelay(retry);
    } else {
      if (!ntpDone) {
        configTime(8*3600, 0, "pool.ntp.org", "ntp.aliyun.com");
        Serial.println("→ NTP sync");
        ntpDone = true;
      }
      vTaskDelay(pdMS_TO_TICKS(10000));
    }
  }
}

// ----------------- setup -----------------
void setup() {
  Serial.begin(115200);
  prefs.begin("cfg", false);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(BUTTON_PIN, buttonISR, FALLING);

  display.begin();
  mx.begin();
  display.setIntensity(0);
  display.setSpeed(50);
  display.setPause(1000);
  display.setTextAlignment(PA_LEFT);

  xTaskCreate(wifiTask, "WiFi", 4096, NULL, 1, NULL);
  xTaskCreate(mqttTask, "MQTT", 4096, NULL, 1, NULL);

  mx.clear();
  displayStage = 0;
  lastActivityMillis = millis();
}

// ----------------- loop -----------------
void loop() {
  bool pressed = pollButtonPressed();
  if (!pressed && buttonIRQ) {
    buttonIRQ = false;
    pressed = true;
  }

  if (pressed) {
    displayStage = (displayStage + 1) % 5; // 0~4
    lastActivityMillis = millis();
    Serial.printf("Button pressed, stage=%u\n", displayStage);
  }

  if (displayStage == 0) {
    mx.clear();
  } else {
    time_t nowt = time(nullptr);
    struct tm* timeinfo = localtime(&nowt);

    if (displayStage == 1) {
      // 粗显示：2x2，左上角 (0,0)
      show_data_s d = convertTimeData(timeinfo, 35, 92, (currentFans>=0?currentFans:59));
      mx.clear();
      displayBinaryDigits(d.hour_ten, d.hour_units,
                          d.min_ten, d.min_units,
                          2, 0, 0);
    } 
    else if (displayStage == 2) {
      // 笑脸
      drawImage(IMG_XIAO[0]);
    } 
    else if (displayStage == 3) {
      // 多区域显示，100ms 刷新
      static unsigned long lastUpdate = 0;
      if (millis() - lastUpdate >= 100) {
        lastUpdate = millis();
        show_data_s d = convertTimeData(timeinfo, 35, 92, (currentFans>=0?currentFans:59));

        mx.clear();
        // 月日 (0,4)
        displayBinaryDigits(d.month_ten, d.month_units,
                            d.day_ten, d.day_units,
                            1, 0, 4);

        // 时分 (4,4)
        displayBinaryDigits(d.hour_ten, d.hour_units,
                            d.min_ten, d.min_units,
                            1, 4, 4);

        // 温湿度 (0,0)
        displayBinaryDigits(d.temp_ten, d.temp_units,
                            d.hum_ten, d.hum_units,
                            1, 0, 0);

        // 粉丝+秒数 (4,0)
        displayBinaryDigits(d.fans_ten, d.fans_units,
                            d.sec_ten, d.sec_units,
                            1, 4, 0);
      }
    }
  }
  if (!displayTimeoutMs) {
    if (displayStage != 0 && millis() - lastActivityMillis > displayTimeoutMs) {
      displayStage = 0;
      mx.clear();
      Serial.println("Timeout, back to sleep");
    }
  }
  delay(100);
}
