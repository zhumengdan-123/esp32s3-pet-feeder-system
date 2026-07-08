#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include "Freenove_WS2812_Lib_for_ESP32.h"
#include <ESP32Servo.h>
#include <EEPROM.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

// ===================== 硬件基础配置 =====================
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, 10, 11, U8X8_PIN_NONE);

const char* ssid = "QianLiiiiii";
const char* password = "liuhe666";
WebServer server(80);

#define LEDS_COUNT 1
#define LEDS_PIN 48
#define CHANNEL 0
Freenove_ESP32_WS2812 strip(LEDS_COUNT, LEDS_PIN, CHANNEL);

#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

Servo myServo;
const int servoPin = 2;
const int closeAngle = 5;
const int openAngle = 85;
const int feedDelay = 800;

#define HX711_DT 17
#define HX711_SCK 16
long offset = 0;
float scaleFactor = 409.0;

#define WATER_PIN 14
#define RELAY_PIN 7
#define BEEP_PIN 13

float g_fullWeight = 200.0;
float g_lowWeight = 100.0;
const int MAX_FEED_COUNT = 5;
const long FEED_COOLDOWN = 3000;
const long SENSOR_READ_INTERVAL = 800;

bool feeding = false;
float temperature = 25.0;
float humidity = 50.0;
float foodWeight = 0;
String waterStatus = "水位充足";
unsigned long lastFeedTime = 0;
unsigned long lastSensorRead = 0;
int feedAttempts = 0;
bool autoFeedEnabled = true;
unsigned long lastAlarmTime = 0;

unsigned long servoTime = 0;
int servoStage = 0;

// ===================== 大模型AI配置区 =====================
const char* API_URL       = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions";
const char* API_KEY       = "tp-chvsy8pgg1en9u8zztnp4wvud9npt924pjdr15s4w4l6sbpw";
const char* MODEL         = "mimo-v2.5-pro";
WiFiClientSecure client;

struct PetInfo{
  String breed;
  float weight;
  int age;
  bool isNeuter;
  bool isFat;
  bool gutWeak;
} pet;

int feedTime[3] = {8, 12, 18};
float feedGram[3] = {40, 35, 45};

float dayTotalFood = 0;
float lastRecordedWeight = 0;
unsigned long lastReportTime = 0;
String aiReport = "等待生成...";
unsigned long dayStartTime = 0;

bool reportGenerating = false;
bool planGenerating = false;

// ===================== NTP定时投喂相关 =====================
bool mealFed[3] = {false, false, false};
bool mealCompleted[3] = {false, false, false};
int cachedHour = -1;
unsigned long lastTimeCheck = 0;

// ===================== 每日20:00任务 =====================
int lastReportDay = -1;

// ===================== 卡粮/缺粮警示 =====================
bool mealBlocked = false;
unsigned long lastBlockedAlarm = 0;

#define EEPROM_FEED_ADDR 100

// ===================== OLED静态显示函数 =====================
void oledRefresh(){
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_symbols);

  String stateText;
  if(feedAttempts >= MAX_FEED_COUNT){
    stateText = "Alert: No Food";
  }else if(foodWeight >= g_fullWeight){
    stateText = "Food Full";
  }else if(foodWeight >= g_lowWeight){
    stateText = "Low Food";
  }else{
    stateText = "Food Empty";
  }

  char numBuf[12];
  u8g2.drawStr(0,14,"Wt:");
  dtostrf(foodWeight, 4, 1, numBuf);
  u8g2.drawStr(30,14,numBuf);
  u8g2.drawStr(80,14," g");

  u8g2.drawStr(0,28,"Temp:");
  dtostrf(temperature, 4, 1, numBuf);
  u8g2.drawStr(45,28,numBuf);
  u8g2.drawStr(85,28," C");

  u8g2.drawStr(0,42,"Hum:");
  dtostrf(humidity, 4, 1, numBuf);
  u8g2.drawStr(30,42,numBuf);
  u8g2.drawStr(80,42," %");

  u8g2.drawStr(0,56,"Food:");
  u8g2.drawStr(40,56,stateText.c_str());

  u8g2.sendBuffer();
}

// ===================== 蜂鸣器 =====================
void beepShort() {
  digitalWrite(BEEP_PIN, HIGH);
  delay(180);
  digitalWrite(BEEP_PIN, LOW);
}
void beepAlarm() {
  for(int i = 0; i < 3; i++){
    digitalWrite(BEEP_PIN, HIGH);
    delay(150);
    digitalWrite(BEEP_PIN, LOW);
    delay(150);
  }
}
void beepBlocked() {
  digitalWrite(BEEP_PIN, HIGH);
  delay(150);
  digitalWrite(BEEP_PIN, LOW);
  delay(150);
  digitalWrite(BEEP_PIN, HIGH);
  delay(150);
  digitalWrite(BEEP_PIN, LOW);
}

// ===================== HX711称重 =====================
unsigned long readRaw() {
    unsigned long Count = 0;
    digitalWrite(HX711_SCK, LOW);

    int waitCount = 0;
    while (digitalRead(HX711_DT) == HIGH) {
        delayMicroseconds(10);  
        waitCount++;
        if (waitCount > 20000) return 0; 
    }
 
    for (int i = 0; i < 24; i++) {
        digitalWrite(HX711_SCK, HIGH);
        delayMicroseconds(1);    
        Count = Count << 1;
        digitalWrite(HX711_SCK, LOW);
        delayMicroseconds(1);    
        if (digitalRead(HX711_DT) == HIGH) Count++;
    }
    
    digitalWrite(HX711_SCK, HIGH);
    delayMicroseconds(1);
    digitalWrite(HX711_SCK, LOW);
    
    Count = Count ^ 0x800000;  
    return Count;
}
float getWeight() {
  if (feeding) return foodWeight;
  long sum = 0;
  int validCount = 0;
  for (int i = 0; i < 3; i++) {
    unsigned long raw = readRaw();
    if (raw > 0 && raw < 0xFFFFFF) {
      sum += raw;
      validCount++;
    }
    delay(5);
  }
  if (validCount == 0) return foodWeight;
  long raw = sum / validCount;
  float weight = (raw - offset) / scaleFactor;
  if (weight < 0) weight = 0;
  return weight;
}
void tareScale() {
  Serial.println("Clear scale, wait 3s to tare...");
  delay(3000);
  long sum = 0;
  int validCount = 0;
  for (int i = 0; i < 20; i++) {
    unsigned long raw = readRaw();
    if (raw > 0 && raw < 0xFFFFFF) {
      sum += raw;
      validCount++;
    }
    delay(50);
  }
  if (validCount > 0) {
    offset = sum / validCount;
    EEPROM.put(0, offset);
    EEPROM.commit();
    Serial.print("Tare done offset = ");
    Serial.println(offset);
  }
}
void calibrateScale(float targetWeight) {
  Serial.print("Calibrate, put ");
  Serial.print(targetWeight);
  Serial.println(" g weight...");
  delay(3000);
  long sum = 0;
  int validCount = 0;
  for (int i = 0; i < 20; i++) {
    unsigned long raw = readRaw();
    if (raw > 0 && raw < 0xFFFFFF) {
      sum += raw;
      validCount++;
    }
    delay(50);
  }
  if (validCount > 0) {
    long rawWithWeight = sum / validCount;
    float newScaleFactor = (rawWithWeight - offset) / targetWeight;
    if (newScaleFactor > 0 && newScaleFactor < 10000) {
      scaleFactor = newScaleFactor;
      EEPROM.put(sizeof(offset), scaleFactor);
      EEPROM.commit();
      Serial.print("Calibrate done! scaleFactor = ");
      Serial.println(scaleFactor);
    }
  }
}

// ===================== WS2812灯光 =====================
void updateStatusLED() {
  int currentHour = cachedHour;
  if (currentHour == -1) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      currentHour = timeinfo.tm_hour;
    }
  }

  bool isMealTime = false;
  float targetGram = 0;
  if (currentHour >= 7 && currentHour <= 11) {
    isMealTime = true;
    targetGram = feedGram[0];
  } else if (currentHour >= 12 && currentHour <= 17) {
    isMealTime = true;
    targetGram = feedGram[1];
  } else if (currentHour >= 18 && currentHour <= 19) {
    isMealTime = true;
    targetGram = feedGram[2];
  }

  if (isMealTime) {
    if (foodWeight >= targetGram) {
      strip.setLedColor(0, 0, 255, 0);
    } else {
      strip.setLedColor(0, 255, 0, 0);
    }
    strip.show();
    return;
  }

  if (feeding) {
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 200) {
      static bool ledState = false;
      ledState = !ledState;
      if (ledState) strip.setLedColor(0, 0, 0, 255);
      else strip.setLedColor(0, 0, 0, 0);
      strip.show();
      lastBlink = millis();
    }
    return;
  }
  if (feedAttempts >= MAX_FEED_COUNT) {
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 300) {
      static bool ledState = false;
      ledState = !ledState;
      if (ledState) strip.setLedColor(0, 255, 0, 0);
      else strip.setLedColor(0, 0, 0, 0);
      strip.show();
      lastBlink = millis();
    }
    return;
  }
  if (temperature > 35) {
    strip.setLedColor(0, 255, 165, 0);
    strip.show();
    return;
  }
  if (foodWeight >= g_fullWeight) strip.setLedColor(0, 0, 255, 0);
  else strip.setLedColor(0, 255, 0, 0);
  strip.show();
}

// ===================== 水泵 =====================
void waterPumpControl() {
  static unsigned long lastPumpStart = 0;
  static bool pumpRunning = false;
  if (digitalRead(WATER_PIN) == 1) {
    digitalWrite(RELAY_PIN, HIGH);
    waterStatus = "水位充足";
    pumpRunning = false;
  } else {
    if (!pumpRunning && (millis() - lastPumpStart > 10000)) {
      digitalWrite(RELAY_PIN, LOW);
      waterStatus = "补水运行中";
      pumpRunning = true;
      lastPumpStart = millis();
    } else if (pumpRunning && (millis() - lastPumpStart > 30000)) {
      digitalWrite(RELAY_PIN, HIGH);
      waterStatus = "补水超时";
      pumpRunning = false;
    }
  }
}

// ===================== 舵机 =====================
bool doFeedAction(String source) {
  if (feeding) { return false; }
  if (source == "auto" && !autoFeedEnabled) { return false; }
  if (millis() - lastFeedTime < FEED_COOLDOWN) { return false; }

  feeding = true;
  servoStage = 1;
  servoTime = millis();
  lastFeedTime = millis();

  Serial.print("Feed start: ");
  Serial.print(source);
  Serial.print(" No.");
  Serial.println(feedAttempts + 1);
  return true;
}
void servoTask() {
  if (!feeding) return;
  unsigned long now = millis();
  if (servoStage == 1) {
    myServo.write(openAngle);
    servoStage = 2;
    servoTime = now;
  } else if (servoStage == 2 && now - servoTime >= feedDelay) {
    myServo.write(closeAngle);
    servoStage = 3;
    servoTime = now;
  } else if (servoStage == 3 && now - servoTime >= 200) {
    feeding = false;
    servoStage = 0;
    beepShort();
    foodWeight = getWeight();

    feedAttempts++;
    Serial.print("Feed done, weight: ");
    Serial.print(foodWeight);
    Serial.println("g");

    if (foodWeight >= g_fullWeight) {
      feedAttempts = 0;
      autoFeedEnabled = true;
    }
    if (feedAttempts >= MAX_FEED_COUNT) {
      autoFeedEnabled = false;
    }
    updateStatusLED();
  }
}

// ===================== 重置系统 =====================
void resetAutoFeed() {
  autoFeedEnabled = true;
  feedAttempts = 0;
  foodWeight = getWeight();
  for (int i = 0; i < 3; i++) {
    mealFed[i] = false;
    mealCompleted[i] = false;
  }
  mealBlocked = false;
  aiReport = "系统已重置，等待生成新报告...";
  updateStatusLED();
  Serial.println("System reset: all cleared.");
}

// ===================== EEPROM 方案保存/读取 =====================
void saveFeedPlan() {
  EEPROM.put(EEPROM_FEED_ADDR, feedTime);
  EEPROM.put(EEPROM_FEED_ADDR + sizeof(feedTime), feedGram);
  EEPROM.put(EEPROM_FEED_ADDR + sizeof(feedTime) + sizeof(feedGram), g_fullWeight);
  EEPROM.put(EEPROM_FEED_ADDR + sizeof(feedTime) + sizeof(feedGram) + sizeof(g_fullWeight), g_lowWeight);
  EEPROM.commit();
  Serial.println("Feed plan saved to EEPROM");
}

void loadFeedPlan() {
  int tempTime[3];
  float tempGram[3];
  float tempFull, tempLow;
  EEPROM.get(EEPROM_FEED_ADDR, tempTime);
  if (tempTime[0] >= 0 && tempTime[0] <= 23) {
    for (int i = 0; i < 3; i++) feedTime[i] = tempTime[i];
    EEPROM.get(EEPROM_FEED_ADDR + sizeof(feedTime), tempGram);
    for (int i = 0; i < 3; i++) feedGram[i] = tempGram[i];
    EEPROM.get(EEPROM_FEED_ADDR + sizeof(feedTime) + sizeof(feedGram), tempFull);
    EEPROM.get(EEPROM_FEED_ADDR + sizeof(feedTime) + sizeof(feedGram) + sizeof(tempFull), tempLow);
    g_fullWeight = tempFull;
    g_lowWeight = tempLow;
    Serial.println("Loaded feed plan from EEPROM:");
    Serial.printf("  Breakfast: %d:00 %.1fg\n", feedTime[0], feedGram[0]);
    Serial.printf("  Lunch:     %d:00 %.1fg\n", feedTime[1], feedGram[1]);
    Serial.printf("  Dinner:    %d:00 %.1fg\n", feedTime[2], feedGram[2]);
    Serial.printf("  FullWeight: %.1f, LowWeight: %.1f\n", g_fullWeight, g_lowWeight);
  } else {
    Serial.println("No valid feed plan in EEPROM, using defaults");
  }
}

// ===================== AI辅助函数 =====================
String cleanAIResponse(String content) {
  content.trim();
  if (content.startsWith("```")) {
    int firstNewline = content.indexOf('\n');
    if (firstNewline != -1) {
      content = content.substring(firstNewline + 1);
    } else {
      content = content.substring(3);
    }
  }
  if (content.endsWith("```")) {
    content = content.substring(0, content.length() - 3);
  }
  content.replace("**", "");
  content.trim();
  return content;
}

void generateFeedPlan(){
  if(WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }
  
  HTTPClient http;
  client.setInsecure();
  http.setTimeout(60000);
  http.setConnectTimeout(15000);
  http.begin(client, API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_KEY);

  String prompt = "You are a pet nutritionist. Today is one day. Give ONLY today's 3 meal times and grams in format: hour,gram,hour,gram,hour,gram. NO table, NO explanation, NO markdown. Example: 8,40,12,35,18,45. Pet:";
  prompt += pet.breed;
  prompt += ",";
  prompt += String(pet.weight);
  prompt += "kg,";
  prompt += String(pet.age);
  prompt += "mo,neuter:";
  prompt += String(pet.isNeuter?"Y":"N");
  prompt += ",fat:";
  prompt += String(pet.isFat?"Y":"N");
  prompt += ",gut:";
  prompt += String(pet.gutWeak?"Y":"N");

  StaticJsonDocument<512> doc;
  doc["model"] = MODEL;
  doc["messages"][0]["role"] = "user";
  doc["messages"][0]["content"] = prompt;
  doc["temperature"] = 0.1;
  doc["max_tokens"] = 4096;
  String payload;
  serializeJson(doc, payload);

  Serial.println("Requesting AI feed plan...");
  Serial.println("Payload size: " + String(payload.length()));
  
  unsigned long reqStart = millis();
  int httpCode = http.POST(payload);
  unsigned long reqTime = millis() - reqStart;
  Serial.println("Request took: " + String(reqTime) + "ms");
  
  if(httpCode == HTTP_CODE_OK){
    String res = http.getString();
    Serial.println("API response: " + res);
    
    StaticJsonDocument<4096> resDoc;
    DeserializationError err = deserializeJson(resDoc, res);
    if (err) {
      Serial.println("Parse API response failed: " + String(err.c_str()));
      http.end();
      return;
    }
    
    String content = resDoc["choices"][0]["message"]["content"].as<String>();
    content = cleanAIResponse(content);
    Serial.println("Cleaned content: [" + content + "]");
    
    if (content.length() == 0) {
      Serial.println("ERROR: content is empty!");
      http.end();
      return;
    }
    
    int t1, t2, t3;
    float g1, g2, g3;
    int parsed = sscanf(content.c_str(), "%d,%f,%d,%f,%d,%f", &t1, &g1, &t2, &g2, &t3, &g3);
    
    if (parsed != 6) {
      Serial.println("Parse numbers failed, got " + String(parsed) + " values");
      http.end();
      return;
    }
    
    if (t1 < 0 || t1 > 23 || t2 < 0 || t2 > 23 || t3 < 0 || t3 > 23 ||
        g1 <= 0 || g2 <= 0 || g3 <= 0 || g1 > 500 || g2 > 500 || g3 > 500) {
      Serial.println("Invalid values: " + String(t1) + "," + String(g1) + "," + 
                     String(t2) + "," + String(g2) + "," + String(t3) + "," + String(g3));
      http.end();
      return;
    }
    
    feedTime[0] = t1; feedGram[0] = g1;
    feedTime[1] = t2; feedGram[1] = g2;
    feedTime[2] = t3; feedGram[2] = g3;

    g_fullWeight = g1 + g2 + g3;
    g_lowWeight = g_fullWeight * 0.5;
    
    saveFeedPlan();
    
    Serial.println("Feed plan OK:");
    Serial.println("  T1:" + String(t1) + " G1:" + String(g1));
    Serial.println("  T2:" + String(t2) + " G2:" + String(g2));
    Serial.println("  T3:" + String(t3) + " G3:" + String(g3));

    aiReport = "方案已生成：早餐" + String(t1) + ":00 " + String(g1) + "g，午餐" + String(t2) + ":00 " + String(g2) + "g，晚餐" + String(t3) + ":00 " + String(g3) + "g";
  } else {
    Serial.print("HTTP failed: ");
    Serial.println(httpCode);
    if (httpCode < 0) {
      Serial.println("Error: " + http.errorToString(httpCode));
    } else {
      String res = http.getString();
      Serial.println("Error response: " + res);
    }
  }
  http.end();
}

void createHealthReport(){
  if(WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    aiReport = "WiFi未连接，无法生成报告";
    return;
  }
  
  HTTPClient http;
  client.setInsecure();
  http.setTimeout(60000);
  http.setConnectTimeout(15000);
  http.begin(client, API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_KEY);

  String prompt = "你是宠物健康顾问。今日累计进食";
  prompt += String(dayTotalFood, 0);
  prompt += "克，环境温度";
  prompt += String(temperature, 0);
  prompt += "度，湿度";
  prompt += String(humidity, 0);
  prompt += "%。请用40字以内中文给出喂养建议，必须包含'进食X克'这个信息，不要markdown。";

  StaticJsonDocument<512> doc;
  doc["model"] = MODEL;
  doc["messages"][0]["role"] = "user";
  doc["messages"][0]["content"] = prompt;
  doc["temperature"] = 0.3;
  doc["max_tokens"] = 4096;
  String payload;
  serializeJson(doc, payload);

  Serial.println("Requesting AI report...");
  unsigned long reqStart = millis();
  int httpCode = http.POST(payload);
  Serial.println("Report request took: " + String(millis() - reqStart) + "ms");
  
  if(httpCode == HTTP_CODE_OK){
    String res = http.getString();
    Serial.println("Report API response: " + res);
    
    StaticJsonDocument<4096> resDoc;
    DeserializationError err = deserializeJson(resDoc, res);
    if (err) {
      Serial.println("Parse report response failed: " + String(err.c_str()));
      http.end();
      return;
    }
    
    String content = resDoc["choices"][0]["message"]["content"].as<String>();
    content = cleanAIResponse(content);
    
    if (content.length() == 0) {
      Serial.println("ERROR: report content is empty!");
      aiReport = "报告生成失败，请重试";
      http.end();
      return;
    }
    
    if (mealBlocked) {
      aiReport = "⚠️ 警告：当前餐可能卡粮或粮仓已空，请检查！\n" + content;
    } else {
      aiReport = content;
    }
    lastReportTime = millis();
    Serial.println("Report: [" + aiReport + "]");
  } else {
    Serial.print("Report HTTP failed: ");
    Serial.println(httpCode);
    if (httpCode < 0) {
      Serial.println("Error: " + http.errorToString(httpCode));
    } else {
      String res = http.getString();
      Serial.println("Error response: " + res);
    }
    aiReport = "报告生成失败，请检查网络";
  }
  http.end();
}
// ===================== Web页面 =====================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1.0'>
<title>智能宠物喂食器</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:Arial,Helvetica,sans-serif;background:#f0f0f0;padding:0;margin:0;}
.container{max-width:500px;margin:0 auto;min-height:100vh;}
.card{background:white;border-radius:15px;padding:20px;margin:10px;box-shadow:0 2px 8px rgba(0,0,0,0.1);}
.title{font-size:24px;text-align:center;margin-bottom:15px;color:#333;}
.row{display:flex;justify-content:space-between;padding:12px 0;border-bottom:1px solid #eee;align-items:center;}
.row:last-child{border-bottom:none;}
.label{font-weight:bold;color:#555;}
.warning{color:#e74c3c;font-weight:bold;}
.success{color:#27ae60;font-weight:bold;}
button{width:100%;padding:15px;font-size:18px;background:#3498db;color:white;border:none;border-radius:10px;margin-top:10px;cursor:pointer;transition:opacity 0.2s;}
button:hover{opacity:0.9;}
button:active{opacity:0.8;}
.reset-btn{background:#e67e22;}
.calibrate-btn{background:#27ae60;}
.ai-btn{background:#9b59b6;}
input{padding:10px;font-size:16px;width:100%;margin-top:10px;border-radius:5px;border:1px solid #ccc;box-sizing:border-box;}
label{display:block;margin-top:8px;color:#555;}
.info-text{text-align:center;font-size:12px;color:#666;margin-top:10px;padding:0 10px;}
.tips{background:#fff3cd;color:#856404;padding:12px;border-radius:8px;margin:10px;text-align:center;font-weight:bold;font-size:14px;}
.ai-report{background:#f0f8ff;border:1px solid #b0d4f1;padding:12px;border-radius:8px;margin-top:10px;min-height:40px;}
.ai-report b{color:#2c5aa0;}
.status-loading{color:#999;font-style:italic;}

/* Tab 样式 */
.tab-bar{display:flex;background:#fff;position:sticky;top:0;z-index:100;box-shadow:0 2px 4px rgba(0,0,0,0.1);}
.tab-btn{flex:1;padding:14px 0;text-align:center;font-size:14px;color:#666;background:#fff;border:none;cursor:pointer;border-bottom:3px solid transparent;transition:all 0.2s;}
.tab-btn.active{color:#3498db;border-bottom-color:#3498db;font-weight:bold;}
.tab-content{display:none;padding-bottom:20px;}
.tab-content.active{display:block;}
</style>
</head>
<body>
<div class='container'>

<!-- Tab 导航 -->
<div class='tab-bar'>
<button class='tab-btn active' onclick='switchTab(0)'>状态监控</button>
<button class='tab-btn' onclick='switchTab(1)'>投喂控制</button>
<button class='tab-btn' onclick='switchTab(2)'>AI 喂养</button>
</div>

<!-- 页面1: 状态监控 -->
<div class='tab-content active' id='tab0'>
<div class='card'>
<div class='title'>智能宠物喂食器</div>
<div class='row'><span class='label'>温度</span><span id='temp'>-- C</span></div>
<div class='row'><span class='label'>湿度</span><span id='humi'>-- %</span></div>
<div class='row'><span class='label'>剩余食物</span><span id='weight'>-- g</span></div>
<div class='row'><span class='label'>充足重量</span><span id='fullWt'>200 g</span></div>
<div class='row'><span class='label'>投喂阈值</span><span id='lowWt'>100 g</span></div>
<div class='row'><span class='label'>已投喂次数</span><span id='feedCount'>--/5</span></div>
<div class='row'><span class='label'>水位</span><span id='water'>--</span></div>
<div class='row'><span class='label'>自动投喂</span><span id='autoStatus'>--</span></div>
<div class='row'><span class='label'>今日进食</span><span id='dayFood'>-- g</span></div>
<div class='row'><span class='label'>北京时间</span><span id='currentTime'>--:--:--</span></div>
</div>

<div class='card'>
<div class='title' style='font-size:20px'>当前投喂方案</div>
<div class='row'><span class='label'>早餐</span><span id='breakfast'>--:-- --g</span></div>
<div class='row'><span class='label'>午餐</span><span id='lunch'>--:-- --g</span></div>
<div class='row'><span class='label'>晚餐</span><span id='dinner'>--:-- --g</span></div>
<div class='row'><span class='label'>今日状态</span><span id='mealStatus'>未喂/未喂/未喂</span></div>
</div>

<div class='card'>
<div class='title' style='font-size:20px'>AI喂养建议</div>
<div style='padding:10px 0;word-wrap:break-word;'><span id='reportText'>等待生成...</span></div>
</div>
</div>

<!-- 页面2: 投喂控制 -->
<div class='tab-content' id='tab1'>
<div class='card'>
<div class='title'>手动控制</div>
<button onclick='manualFeed()'>手动投喂</button>
<button onclick='resetSystem()' class='reset-btn'>重置投喂系统</button>
<button onclick='resetDayFood()' style='background:#8e44ad;'>清零今日进食</button>
</div>

<div class='card'>
<div class='title' style='font-size:20px'>重量校准</div>
<div class='row'><span class='label'>1. 清空秤盘</span><button onclick='tare()' style='width:auto;padding:8px 20px;margin:0;font-size:14px;'>去皮</button></div>
<div class='row'><span class='label'>2. 放上砝码(g)</span><input type='number' id='calWeight' placeholder='例如 100'></div>
<div class='row'><button onclick='calibrate()' class='calibrate-btn'>开始校准</button></div>
</div>

<div class='tips'>提示：连续投喂5次仍重量不足，喂粮卡到了，或者粮桶内粮食不足，请及时检查补充</div>
<div class='info-text'>灯光规则：用餐时段（7-11,12-17,18-19）显示重量是否达到该餐建议量：绿灯(达标) / 红灯(不足) | 其他时段：蓝(投喂) 橙(高温) 绿(满) 红闪(缺粮)</div>
</div>

<!-- 页面3: AI 个性化 -->
<div class='tab-content' id='tab2'>
<div class='card'>
<div class='title'>AI个性化喂养</div>
<input type='text' id='breed' placeholder='宠物品种（如：柯基、英短）'>
<input type='number' id='petWt' placeholder='体重(kg)' step='0.1'>
<input type='number' id='petAge' placeholder='年龄(月)'>
<label><input type='checkbox' id='neuter'> 已绝育</label>
<label><input type='checkbox' id='fat'> 偏肥胖</label>
<label><input type='checkbox' id='gut'> 肠胃虚弱</label>
<button onclick='makePlan()' style='background:#27ae60;margin-top:10px'>生成当日投喂方案</button>
<button onclick='makeReport()' class='ai-btn' style='margin-top:8px'>立即生成喂养报告</button>
<div class='ai-report'>
<b>AI喂养建议：</b><br>
<span id='aiTip' class='status-loading'>等待生成...</span>
</div>
</div>
</div>

</div>
<script>
function switchTab(n){
  document.querySelectorAll('.tab-btn').forEach((b,i)=>b.classList.toggle('active',i==n));
  document.querySelectorAll('.tab-content').forEach((c,i)=>c.classList.toggle('active',i==n));
}

function savePetInfo() {
  var info = {
    breed: document.getElementById('breed').value,
    weight: document.getElementById('petWt').value,
    age: document.getElementById('petAge').value,
    neuter: document.getElementById('neuter').checked,
    fat: document.getElementById('fat').checked,
    gut: document.getElementById('gut').checked
  };
  localStorage.setItem('petInfo', JSON.stringify(info));
}
function loadPetInfo() {
  var saved = localStorage.getItem('petInfo');
  if (saved) {
    try {
      var info = JSON.parse(saved);
      document.getElementById('breed').value = info.breed || '';
      document.getElementById('petWt').value = info.weight || '';
      document.getElementById('petAge').value = info.age || '';
      document.getElementById('neuter').checked = info.neuter || false;
      document.getElementById('fat').checked = info.fat || false;
      document.getElementById('gut').checked = info.gut || false;
    } catch(e) { /* ignore */ }
  }
}
window.addEventListener('DOMContentLoaded', loadPetInfo);

function fetchData(){
  fetch('/api/data').then(res=>res.json()).then(data=>{
    document.getElementById('temp').innerText = data.temp + ' C';
    document.getElementById('humi').innerText = data.humi + ' %';
    let w = document.getElementById('weight');
    w.innerText = data.weight + ' g';
    w.className = (data.weight >= data.fullWeight) ? 'success' : ((data.weight >= data.lowWeight) ? '' : 'warning');
    document.getElementById('fullWt').innerText = data.fullWeight + ' g';
    document.getElementById('lowWt').innerText = data.lowWeight + ' g';
    document.getElementById('feedCount').innerText = data.feedCount + '/5';
    document.getElementById('water').innerText = data.water;
    document.getElementById('autoStatus').innerText = data.autoEnabled ? '运行中' : '已停止';
    document.getElementById('dayFood').innerText = data.dayTotalFood + ' g';
    document.getElementById('currentTime').innerText = data.currentTime || '--:--:--';
    if (data.feedTime0 !== undefined) {
      document.getElementById('breakfast').innerText = data.feedTime0 + ':00 ' + data.feedGram0 + 'g';
      document.getElementById('lunch').innerText = data.feedTime1 + ':00 ' + data.feedGram1 + 'g';
      document.getElementById('dinner').innerText = data.feedTime2 + ':00 ' + data.feedGram2 + 'g';
    }
    if (data.mealStatus) {
      document.getElementById('mealStatus').innerText = data.mealStatus;
    }
    if (data.aiReport && data.aiReport !== '等待生成...' && 
        data.aiReport !== '报告生成中，请等待约30-60秒...' &&
        data.aiReport !== '方案生成中，请等待约30-60秒...' &&
        data.aiReport !== 'WiFi未连接，无法生成报告') {
      document.getElementById('reportText').innerText = data.aiReport;
    } else if (data.aiReport && data.aiReport.includes('生成中')) {
      document.getElementById('reportText').innerText = '生成中，请稍候...';
    } else {
      document.getElementById('reportText').innerText = '等待生成...';
    }
    if(data.aiReport && data.aiReport !== '' && data.aiReport !== 'Wait data...' && 
       data.aiReport !== '报告生成中，请等待约30-60秒...' && 
       data.aiReport !== '报告生成失败，请检查网络') {
      document.getElementById('aiTip').innerText = data.aiReport;
      document.getElementById('aiTip').className = '';
    } else if (data.aiReport === '报告生成中，请等待约30-60秒...') {
      document.getElementById('aiTip').innerText = data.aiReport;
      document.getElementById('aiTip').className = 'status-loading';
    } else {
      document.getElementById('aiTip').innerText = '等待生成...';
      document.getElementById('aiTip').className = 'status-loading';
    }
  }).catch(err => {
    console.error('数据获取失败:', err);
  });
}

function manualFeed(){
  fetch('/api/feed',{method:'POST'}).then(()=>{
    console.log('手动投喂已触发');
  }).catch(err => alert('投喂失败: ' + err));
}

function resetSystem(){
  fetch('/api/reset',{method:'POST'}).then(()=>{
    alert('系统已重置');
    fetchData();
  });
}

function tare(){
  fetch('/api/tare',{method:'POST'}).then(()=>alert('去皮完成，请等待3秒稳定...'));
}

function calibrate(){
  let w = document.getElementById('calWeight').value;
  if(w > 0){
    fetch('/api/calibrate?weight='+w,{method:'POST'}).then(()=>alert('校准中，请等待5秒...'));
  } else {
    alert('请输入砝码重量');
  }
}

function resetDayFood(){
  fetch('/api/resetDayFood', {method:'POST'})
    .then(() => {
      alert('今日进食已清零');
      fetchData();
    })
    .catch(err => alert('清零失败: ' + err));
}

function makePlan(){
  savePetInfo();
  let data = {
    breed: document.getElementById("breed").value,
    weight: parseFloat(document.getElementById("petWt").value),
    age: parseInt(document.getElementById("petAge").value),
    neuter: document.getElementById("neuter").checked,
    fat: document.getElementById("fat").checked,
    gut: document.getElementById("gut").checked
  };
  if(!data.breed || isNaN(data.weight) || isNaN(data.age)) {
    alert("请填写完整的宠物信息");
    return;
  }
  document.getElementById('aiTip').innerText = "方案生成中，请等待约30-60秒...";
  document.getElementById('aiTip').className = "status-loading";
  fetch('/api/plan', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(data)
  }).then(res => res.text())
  .then(text => {
    if (text === "generating") {
      alert("已有方案正在生成中，请稍后刷新查看");
    } else {
      alert("方案生成请求已发送，请等待约22秒后自动刷新");
      let checkInterval = setInterval(() => {
        fetch('/api/data').then(res => res.json()).then(data => {
          if (data.aiReport && data.aiReport.includes('方案已生成')) {
            document.getElementById('aiTip').innerText = data.aiReport;
            document.getElementById('aiTip').className = '';
            clearInterval(checkInterval);
          }
        });
      }, 5000);
      setTimeout(() => clearInterval(checkInterval), 60000);
    }
  }).catch(err => {
    alert("请求发送失败: " + err);
  });
}

function makeReport(){
  document.getElementById('aiTip').innerText = "报告生成中，请等待约30-60秒...";
  document.getElementById('aiTip').className = "status-loading";
  fetch('/api/report',{method:'POST'})
  .then(res => res.text())
  .then(text => {
    if (text === "generating") {
      alert("已有报告正在生成中，请稍后刷新查看");
    } else {
      alert("报告生成请求已发送，请等待30-60秒后自动刷新");
      let checkInterval = setInterval(() => {
        fetch('/api/data').then(res => res.json()).then(data => {
          if (data.aiReport && 
              data.aiReport !== 'Wait data...' && 
              data.aiReport !== '报告生成中，请等待约30-60秒...' && 
              data.aiReport !== '报告生成失败，请检查网络' &&
              data.aiReport !== 'WiFi未连接，无法生成报告') {
            document.getElementById('aiTip').innerText = data.aiReport;
            document.getElementById('aiTip').className = '';
            clearInterval(checkInterval);
          }
        });
      }, 5000);
      setTimeout(() => clearInterval(checkInterval), 60000);
    }
  })
  .catch(err => {
    alert("请求失败: " + err);
  });
}

setInterval(fetchData, 1000);
fetchData();
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}
// ===================== API处理函数 =====================
String readRequestBody() {
  String body = "";
  if (server.hasArg("plain")) {
    body = server.arg("plain");
  } else {
    body = server.arg(0);
  }
  return body;
}

void handleAPIPlan(){
  if(server.method() != HTTP_POST){
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  if (planGenerating) {
    server.send(200, "text/plain", "generating");
    return;
  }
  String body = readRequestBody();
  if (body.length() == 0) {
    server.send(400, "text/plain", "Empty body");
    return;
  }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  pet.breed = doc["breed"].as<String>();
  pet.weight = doc["weight"].as<float>();
  pet.age = doc["age"].as<int>();
  pet.isNeuter = doc["neuter"].as<bool>();
  pet.isFat = doc["fat"].as<bool>();
  pet.gutWeak = doc["gut"].as<bool>();
  planGenerating = true;
  server.send(200, "text/plain", "ok");
}

void handleAPIReport(){
  if(server.method() != HTTP_POST){
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  if (reportGenerating) {
    server.send(200, "text/plain", "generating");
    return;
  }
  reportGenerating = true;
  aiReport = "报告生成中，请等待约30-60秒...";
  server.send(200, "text/plain", "ok");
}

void handleAPIData() {
  struct tm timeinfo;
  char timeStr[20] = "--:--:--";
  if (getLocalTime(&timeinfo, 0)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  }
  String mealStatus = String(mealFed[0]?"已喂":"未喂") + "/" +
                      String(mealFed[1]?"已喂":"未喂") + "/" +
                      String(mealFed[2]?"已喂":"未喂");
  String json = "{";
  json += "\"temp\":" + String(temperature, 1);
  json += ",\"dayTotalFood\":" + String(dayTotalFood, 1);
  json += ",\"humi\":" + String(humidity, 1);
  json += ",\"weight\":" + String(foodWeight, 1);
  json += ",\"fullWeight\":" + String(g_fullWeight, 1);
  json += ",\"lowWeight\":" + String(g_lowWeight, 1);
  json += ",\"feedCount\":" + String(feedAttempts);
  json += ",\"autoEnabled\":" + String(autoFeedEnabled ? "true" : "false");
  json += ",\"water\":\"" + waterStatus + "\"";
  json += ",\"feedTime0\":" + String(feedTime[0]);
  json += ",\"feedGram0\":" + String(feedGram[0], 1);
  json += ",\"feedTime1\":" + String(feedTime[1]);
  json += ",\"feedGram1\":" + String(feedGram[1], 1);
  json += ",\"feedTime2\":" + String(feedTime[2]);
  json += ",\"feedGram2\":" + String(feedGram[2], 1);
  json += ",\"aiReport\":\"" + aiReport + "\"";
  json += ",\"currentTime\":\"" + String(timeStr) + "\"";
  json += ",\"mealStatus\":\"" + mealStatus + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleAPIFeed() { 
  bool ok = doFeedAction("manual"); 
  server.send(200, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0,\"msg\":\"busy\"}"); 
}

void handleAPIReset() { 
  resetAutoFeed(); 
  server.send(200, "text/plain", "ok"); 
}

void handleAPITare() { 
  tareScale(); 
  server.send(200, "text/plain", "ok"); 
}

void handleAPICalibrate() {
  if (server.hasArg("weight")) {
    float targetWeight = server.arg("weight").toFloat();
    if (targetWeight > 0) { 
      calibrateScale(targetWeight); 
      server.send(200, "text/plain", "ok"); 
      return; 
    }
  }
  server.send(400, "application/json", "{\"error\":\"invalid weight\"}");
}

void handleAPIResetDayFood() {
    dayTotalFood = 0;
    dayStartTime = millis();
    server.send(200, "text/plain", "ok");
}

// ===================== WiFi初始化 =====================
void initWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected");
    Serial.print("IP: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connect fail");
  }
}

// ===================== FreeRTOS AI任务 =====================
void aiTask(void *parameter) {
  while (true) {
    if (reportGenerating) {
      Serial.println("[AI Task] Generating report...");
      createHealthReport();
      reportGenerating = false;
      Serial.println("[AI Task] Report done");
    }
    if (planGenerating) {
      Serial.println("[AI Task] Generating plan...");
      generateFeedPlan();
      planGenerating = false;
      Serial.println("[AI Task] Plan done");
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===================== 系统初始化 =====================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n===== Pet Feeder Start =====");
  EEPROM.begin(512);
  loadFeedPlan();

  pinMode(BEEP_PIN, OUTPUT);
  digitalWrite(BEEP_PIN, LOW);

  myServo.attach(servoPin, 500, 2500);
  myServo.write(closeAngle);
  delay(1500);
  Serial.println("Servo ready");

  strip.begin();
  strip.setLedColor(0, 0, 255, 0);
  strip.show();
  dht.begin();

  pinMode(HX711_DT, INPUT);
  pinMode(HX711_SCK, OUTPUT);
  digitalWrite(HX711_SCK, LOW);

  EEPROM.get(0, offset);
  EEPROM.get(sizeof(offset), scaleFactor);
  if (offset == 0 || offset == 0xFFFFFFFF) tareScale();

  pinMode(WATER_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_symbols);
  u8g2.drawStr(0,15,"Feeder Ready");
  u8g2.sendBuffer();
  delay(800);

  initWiFi();
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP time sync started...");
  
  server.on("/", handleRoot);
  server.on("/api/data", handleAPIData);
  server.on("/api/feed", HTTP_POST, handleAPIFeed);
  server.on("/api/reset", HTTP_POST, handleAPIReset);
  server.on("/api/tare", handleAPITare);
  server.on("/api/calibrate", HTTP_POST, handleAPICalibrate);
  server.on("/api/plan", HTTP_POST, handleAPIPlan);
  server.on("/api/report", HTTP_POST, handleAPIReport);
  server.on("/api/resetDayFood", HTTP_POST, handleAPIResetDayFood);
  server.begin();
  Serial.println("Web Server Started");

  xTaskCreatePinnedToCore(aiTask, "AI Task", 8192, NULL, 1, NULL, 0);
  Serial.println("AI Task Created");

  delay(500);
  foodWeight = getWeight();
  lastRecordedWeight = foodWeight;
  Serial.print("Init weight: ");
  Serial.print(foodWeight);
  Serial.println("g");
  Serial.println("===== System Ready =====");
  beepShort();
}

// ===================== 主循环 =====================
void loop() {
  server.handleClient();
  servoTask();

  // 24小时清零进食统计
  if (millis() - dayStartTime >= 86400000UL || dayStartTime == 0) {
    dayTotalFood = 0;
    dayStartTime = millis();
    Serial.println("===== 24小时清零，进食统计已重置 =====");
  }

  // 传感器读取
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    if (!isnan(temp)) temperature = temp;
    if (!isnan(hum)) humidity = hum;
    if (!feeding) {
      float newWeight = getWeight();
      if (newWeight < lastRecordedWeight) {
        dayTotalFood += (lastRecordedWeight - newWeight);
      }
      foodWeight = newWeight;
      lastRecordedWeight = newWeight;
    }
    oledRefresh();
  }

  // ===================== 时间缓存 & 每日任务 =====================
  if (millis() - lastTimeCheck > 5000) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      cachedHour = timeinfo.tm_hour;
      int currentDay = timeinfo.tm_mday;
      static int lastDay = -1;
      if (currentDay != lastDay) {
        for (int i = 0; i < 3; i++) {
          mealFed[i] = false;
          mealCompleted[i] = false;
        }
        mealBlocked = false;
        feedAttempts = 0;
        lastDay = currentDay;
        Serial.println("===== 新的一天，三餐标记已重置 =====");
      }

      // 每日20:00补喂与报告
      if (cachedHour == 20 && currentDay != lastReportDay) {
        lastReportDay = currentDay;
        Serial.println("===== 20:00 补喂与报告任务开始 =====");

        if (dayTotalFood < g_fullWeight) {
          Serial.printf("今日进食量 %.1fg 低于推荐 %.1fg，开始补喂\n", dayTotalFood, g_fullWeight);
          int feedCount = 0;
          while (foodWeight < 60.0 && feedCount < MAX_FEED_COUNT && !feeding) {
            if (doFeedAction("auto")) {
              feedCount++;
              delay(1500);
              foodWeight = getWeight();
              Serial.printf("补喂第%d次，当前食盆重量 %.1fg\n", feedCount, foodWeight);
            } else {
              break;
            }
          }
          if (feedCount > 0) {
            aiReport = "今日进食不足，已自动补喂至食盆60g，请观察宠物进食情况。";
          } else {
            aiReport = "今日进食不足，但补喂失败（系统忙碌），请手动检查。";
          }
        } else {
          aiReport = "今日进食充足，无需补喂。";
        }

        if (!reportGenerating) {
          reportGenerating = true;
          aiReport = "报告生成中，请等待约30-60秒...";
        }
        Serial.println("===== 20:00 任务结束 =====");
      }
    }
    lastTimeCheck = millis();
  }

  // ===================== 用餐时段补投 =====================
  int mealIndex = -1;
  if (cachedHour >= 7 && cachedHour <= 11) mealIndex = 0;
  else if (cachedHour >= 12 && cachedHour <= 17) mealIndex = 1;
  else if (cachedHour >= 18 && cachedHour <= 19) mealIndex = 2;

  if (mealIndex != -1 && !mealCompleted[mealIndex]) {
    if (foodWeight < feedGram[mealIndex] && feedAttempts < MAX_FEED_COUNT) {
      if (doFeedAction("auto")) {
        // 等待伺服任务完成，下次循环继续判断
      }
    } else if (foodWeight >= feedGram[mealIndex]) {
      mealCompleted[mealIndex] = true;
      mealFed[mealIndex] = true;
      feedAttempts = 0;
      Serial.printf("Meal %d completed (target reached).\n", mealIndex+1);
    } else if (feedAttempts >= MAX_FEED_COUNT) {
      mealCompleted[mealIndex] = true;
      mealFed[mealIndex] = true;
      mealBlocked = true;
      aiReport = "⚠️ 警告：当前餐可能卡粮或粮仓已空，请检查！";
      Serial.printf("Meal %d blocked (max attempts reached).\n", mealIndex+1);
    }
  }

  // ===================== 卡粮报警 =====================
  if (mealBlocked) {
    if (millis() - lastBlockedAlarm > 10000) {
      beepBlocked();
      lastBlockedAlarm = millis();
    }
  }

  // ===================== 缺粮告警（用餐达标时停止） =====================
  bool mealTargetReached = false;
  if (cachedHour != -1) {
    if (cachedHour >= 7 && cachedHour <= 11 && foodWeight >= feedGram[0]) {
      mealTargetReached = true;
    } else if (cachedHour >= 12 && cachedHour <= 17 && foodWeight >= feedGram[1]) {
      mealTargetReached = true;
    } else if (cachedHour >= 18 && cachedHour <= 19 && foodWeight >= feedGram[2]) {
      mealTargetReached = true;
    }
  }
  if (feedAttempts >= MAX_FEED_COUNT && foodWeight < g_lowWeight && !mealTargetReached && !mealBlocked) {
    if(millis() - lastAlarmTime > 5000){
      beepAlarm();
      lastAlarmTime = millis();
    }
  }

  waterPumpControl();
  updateStatusLED();
}