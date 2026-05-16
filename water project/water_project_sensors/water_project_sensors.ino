#include <Wire.h>
#include "MPU9250.h"
#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>

// =========================================================================
// WiFi — смени на името и паролата на твоята мрежа (2.4 GHz)
// =========================================================================
const char WIFI_SSID[] = "AlexL";
const char WIFI_PASS[] = "nastya14b";

// Облачен MQTT — данните стигат до сървъра от всяка мрежа (HiveMQ публичен брокер)
const char MQTT_BROKER[] = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char MQTT_TOPIC[] = "waterproject/alexl/station/data";
const char MQTT_CLIENT_ID[] = "water-station-esp";

ESP8266WebServer webServer(80);
WiFiClient wifiMqttClient;
PubSubClient mqttClient(wifiMqttClient);
String lastJsonPayload = "{}";
bool wifiConnected = false;
bool mqttConnected = false;

// =========================================================================
// КОНФИГУРАЦИЯ НА ПИНОВЕТЕ (NodeMCU / ESP8266)
// =========================================================================
// GPS (NEO-6M): TX на модула → D7 (GPIO13), VCC → 3V, GND → G
//   RX на GPS модула — не свързвай (четем само NMEA)
// Serial Monitor: 115200 бода
const int GPS_RX_PIN = 13;       // D7 ← TX на GPS
const int GPS_TX_PIN = 15;       // D8 — не се използва (само за библиотеката)
const uint32_t GPS_Baud = 9600;       // NEO-6M обикновено 9600; ако 0 байта — пробва 38400
const uint32_t GPS_Baud_Alt = 38400;
const uint32_t DEBUG_Baud = 115200;

uint32_t gpsActiveBaud = GPS_Baud;
int gpsBytesSinceBoot = 0;
bool gpsLinkOk = false;
uint32_t gpsBytesLastPeriod = 0;
unsigned long lastGpsByteMs = 0;
unsigned long gpsFirstByteMs = 0;

const int WATER_SENSOR_PIN = 14; // D5 — сензор за ниво SEN0205
const bool WATER_LIQUID_IS_HIGH = true; // HIGH = вода; false ако е обратно
const int ONE_WIRE_BUS = 12;     // D6 — DS18B20

// =========================================================================
// ИНИЦИАЛИЗАЦИЯ И ГЛОБАЛНИ ПРОМЕНЛИВИ
// =========================================================================
MPU9250 mpu;
TinyGPSPlus gps;
SoftwareSerial gpsSerial(GPS_RX_PIN, GPS_TX_PIN);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

unsigned long lastTime = 0;
float pitch = 0, roll = 0;
float velocityZ = 0;
float waveHeight = 0;
float gravityOffset = 9.81;

float maxPeak = 0;
float minPeak = 0;

const float alpha = 0.98;      // доплеров филтър за ъгли (комплементарен)
const float hpfCoeff = 0.93;   // високочестотен коефициент при интегриране

// --- MPU / вълни: в покой ~0 см; при разклащане — отчитане ---
const float MOVEMENT_THRESHOLD = 0.28f;  // rad/s (~16°/s общо гиро)
const float ACCEL_DEADZONE = 0.18f;      // m/s² — филтър на шум в покой
const float ACCEL_MOVEMENT_MIN = 0.35f;  // m/s² — вертикално ускорение
const uint8_t MOVEMENT_ON_SAMPLES = 10;  // поредни „живи“ семпъла преди вълна
const uint8_t MOVEMENT_OFF_SAMPLES = 8;  // бързо нулиране в покой

float gyroBiasX = 0.0f;
float gyroBiasY = 0.0f;
float gyroBiasZ = 0.0f;
uint8_t movementOnCount = 0;
uint8_t movementOffCount = 0;
bool isMoving = false;

// Режим на драйвера: пълен MPU9250 (с магнитометър) или само MPU-6500 по I2C
enum MpuDriverMode {
  MPU_MODE_NONE = 0,
  MPU_MODE_LIB,       // hideakitai MPU9250 (има AK8963)
  MPU_MODE_6500_RAW   // GY-6500 без магнитометър — директно I2C
};
MpuDriverMode mpuMode = MPU_MODE_NONE;
uint8_t mpuI2cAddr = 0x68;

// Последни стойности за диагностика на клатушка/вълни (MPU)
bool mpuOk = false;
float mpuLastGyroRad = 0.0f;
float mpuLastVertAcc = 0.0f;
bool mpuLastInstantMotion = false;
bool mpuLastRealMotion = false;
float mpuLastPeakSpanCm = 0.0f;
float mpuLastWaveSpeedMps = 0.0f;
float mpuCalmGyroMax = 0.0f;
float mpuCalmAccelMax = 0.0f;
int mpuI2cSdaPin = D3;
int mpuI2cSclPin = D4;

// Прагове и класове на вълни
const float WAVE_NOISE_CM = 1.5f;        // под това — показваме 0 (штил)
const float GYRO_TO_HEIGHT_CM = 0.38f;   // °/s → прибл. см (клатушка на платката)
const float WAVE_LOW_MAX_CM = 18.0f;
const float WAVE_MED_MAX_CM = 38.0f;
const float WAVE_PERIOD_MIN_S = 0.25f;   // макс. ~4 Hz
const float WAVE_PERIOD_MAX_S = 3.0f;    // мин. бавни вълни

// Клас на вълните по наклон (°) — по-надеждно от височината на платката
const float WAVE_TILT_CALM_DEG = 8.0f;   // под това — штил
const float WAVE_TILT_LOW_MAX_DEG = 22.0f;  // ниски: 8°–22°
const float WAVE_TILT_MED_MAX_DEG = 40.0f;  // средни: 22°–40°, над 40° — високи

enum WaveClass {
  WAVE_CALM = 0,
  WAVE_LOW,
  WAVE_MEDIUM,
  WAVE_HIGH
};

static WaveClass classifyWaveByTilt(float maxTiltDeg) {
  if (maxTiltDeg < WAVE_TILT_CALM_DEG) {
    return WAVE_CALM;
  }
  if (maxTiltDeg < WAVE_TILT_LOW_MAX_DEG) {
    return WAVE_LOW;
  }
  if (maxTiltDeg < WAVE_TILT_MED_MAX_DEG) {
    return WAVE_MEDIUM;
  }
  return WAVE_HIGH;
}

static const __FlashStringHelper* waveClassLabel(WaveClass wc) {
  switch (wc) {
    case WAVE_LOW:
      return F("ниски");
    case WAVE_MEDIUM:
      return F("средни");
    case WAVE_HIGH:
      return F("високи");
    default:
      return F("штил / няма вълни");
  }
}

static const char* waveClassId(WaveClass wc) {
  switch (wc) {
    case WAVE_LOW:
      return "low";
    case WAVE_MEDIUM:
      return "medium";
    case WAVE_HIGH:
      return "high";
    default:
      return "calm";
  }
}

static const char* waveClassLabelUtf8(WaveClass wc) {
  switch (wc) {
    case WAVE_LOW:
      return "ниски";
    case WAVE_MEDIUM:
      return "средни";
    case WAVE_HIGH:
      return "високи";
    default:
      return "штил";
  }
}

static void appendJsonBool(String& j, const char* key, bool value) {
  j += F(",\"");
  j += key;
  j += F("\":");
  j += value ? F("true") : F("false");
}

// JSON за Serial (@DATA) и WiFi GET /data
static void buildTelemetryJson(String& j, float waveCm, WaveClass wc, float tiltMaxDeg,
                               float tempC, bool liquidDetected) {
  j.reserve(520);
  j = F("{");
  j += F("\"wave_cm\":");
  j += String(waveCm, 1);
  j += F(",\"wave_speed_ms\":");
  j += String(mpuLastWaveSpeedMps, 3);
  j += F(",\"wave_class\":\"");
  j += waveClassId(wc);
  j += F("\",\"wave_class_label\":\"");
  j += waveClassLabelUtf8(wc);
  j += F("\",\"tilt_max_deg\":");
  j += String(tiltMaxDeg, 1);
  j += F(",\"pitch_deg\":");
  j += String(pitch, 1);
  j += F(",\"roll_deg\":");
  j += String(roll, 1);
  j += F(",\"gyro_dps\":");
  j += String(mpuLastGyroRad * 57.2958f, 1);
  appendJsonBool(j, "motion", mpuLastRealMotion);
  appendJsonBool(j, "mpu_ok", mpuOk);
  j += F(",\"temp_c\":");
  if (tempC == DEVICE_DISCONNECTED_C) {
    j += F("null");
  } else {
    j += String(tempC, 1);
  }
  j += F(",\"water\":\"");
  j += liquidDetected ? F("liquid") : F("dry");
  j += F("\"");
  appendJsonBool(j, "gps_ok", gpsLinkOk);
  j += F(",\"gps_lat\":");
  if (gps.location.isValid()) {
    j += String(gps.location.lat(), 6);
  } else {
    j += F("null");
  }
  j += F(",\"gps_lng\":");
  if (gps.location.isValid()) {
    j += String(gps.location.lng(), 6);
  } else {
    j += F("null");
  }
  j += F(",\"gps_sats\":");
  if (gps.satellites.isValid()) {
    j += String(gps.satellites.value());
  } else {
    j += F("0");
  }
  j += F(",\"gps_status\":\"");
  if (!gpsLinkOk || gps.charsProcessed() < 10) {
    j += F("offline");
  } else if (gps.location.isValid()) {
    j += F("fix");
  } else {
    j += F("waiting");
  }
  j += F("\"");
  j += F(",\"uptime_ms\":");
  j += String(millis());
  appendJsonBool(j, "wifi_ok", wifiConnected);
  if (wifiConnected) {
    j += F(",\"wifi_ip\":\"");
    j += WiFi.localIP().toString();
    j += F("\"");
  }
  j += F("}");
}

static void mqttEnsureConnected() {
  if (!wifiConnected) {
    return;
  }
  if (mqttClient.connected()) {
    mqttConnected = true;
    return;
  }
  mqttConnected = false;
  Serial.print(F("MQTT connect... "));
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    mqttConnected = true;
    Serial.println(F("OK"));
    Serial.print(F("MQTT topic: "));
    Serial.println(MQTT_TOPIC);
  } else {
    Serial.print(F("fail, rc="));
    Serial.println(mqttClient.state());
  }
}

static void mqttPublishTelemetry() {
  if (!wifiConnected) {
    return;
  }
  mqttClient.loop();
  if (!mqttClient.connected()) {
    mqttEnsureConnected();
  }
  if (!mqttClient.connected()) {
    return;
  }
  const bool published = mqttClient.publish(MQTT_TOPIC, lastJsonPayload.c_str(), false);
  if (published) {
    Serial.println(F("MQTT publish OK"));
  } else {
    Serial.println(F("MQTT publish [НЕ ОК]"));
  }
}

static void emitDataJson(float waveCm, WaveClass wc, float tiltMaxDeg, float tempC,
                         bool liquidDetected) {
  buildTelemetryJson(lastJsonPayload, waveCm, wc, tiltMaxDeg, tempC, liquidDetected);
  Serial.print(F("@DATA "));
  Serial.println(lastJsonPayload);
  mqttPublishTelemetry();
}

static void addCorsHeaders() {
  webServer.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
  webServer.sendHeader(F("Access-Control-Allow-Methods"), F("GET, OPTIONS"));
  webServer.sendHeader(F("Access-Control-Allow-Headers"), F("Content-Type"));
}

static void handleHttpOptions() {
  addCorsHeaders();
  webServer.send(204);
}

static void handleHttpData() {
  addCorsHeaders();
  webServer.send(200, F("application/json"), lastJsonPayload);
}

static void handleHttpRoot() {
  addCorsHeaders();
  String msg = F("Water Project API\nGET /data — JSON от сензорите\n");
  if (wifiConnected) {
    msg += F("IP: ");
    msg += WiFi.localIP().toString();
    msg += F("\n");
  }
  webServer.send(200, F("text/plain; charset=utf-8"), msg);
}

static void initWifiServer() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(F("water-station"));
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print(F("WiFi "));
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 50) {
    delay(500);
    Serial.print(F("."));
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.print(F("WiFi OK, IP: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("JSON: http://"));
    Serial.print(WiFi.localIP());
    Serial.println(F("/data"));
  } else {
    wifiConnected = false;
    Serial.println(F("WiFi [НЕ ОК] — провери SSID/парола (2.4 GHz)"));
  }

  webServer.on(F("/data"), HTTP_GET, handleHttpData);
  webServer.on(F("/"), HTTP_GET, handleHttpRoot);
  webServer.onNotFound([]() {
    if (webServer.method() == HTTP_OPTIONS) {
      handleHttpOptions();
    } else {
      webServer.send(404, F("text/plain"), F("404 — опитай /data"));
    }
  });
  webServer.begin();
  Serial.println(F("HTTP сървър :80 стартиран"));

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(768);
  mqttEnsureConnected();
}

static const float MPU_DEG_TO_RAD = 0.01745329252f;
static const float G_TO_MS2 = 9.80665f;

// Префикс MPU_REG_/MPU_CHIP_ — не съвпада с макросите в библиотеката MPU9250
static const uint8_t MPU_REG_WHO_AM_I = 0x75;
static const uint8_t MPU_REG_PWR_MGMT_1 = 0x6B;
static const uint8_t MPU_REG_ACCEL_XOUT_H = 0x3B;
static const float MPU6500_ACCEL_LSB_PER_G = 16384.0f;
static const float MPU6500_GYRO_LSB_PER_DPS = 131.0f;

static const uint8_t MPU_CHIP_ID_9250 = 0x71;
static const uint8_t MPU_CHIP_ID_9255 = 0x73;
static const uint8_t MPU_CHIP_ID_6500 = 0x70;
static const uint8_t MPU_CHIP_ID_6050 = 0x68;

// Проверка дали има устройство на I2C адрес
static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static uint8_t i2cReadByte(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return 0xFF;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1)) != 1) {
    return 0xFF;
  }
  return Wire.available() ? Wire.read() : 0xFF;
}

static void i2cWriteByte(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static bool mpuWhoAmIValid(uint8_t who) {
  return (who == MPU_CHIP_ID_9250) || (who == MPU_CHIP_ID_9255) ||
         (who == MPU_CHIP_ID_6500) || (who == MPU_CHIP_ID_6050);
}

static void printI2cScan() {
  Serial.print(F("I2C скан (D3/D4): "));
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (i2cPresent(addr)) {
      if (found > 0) {
        Serial.print(F(", "));
      }
      Serial.print(F("0x"));
      Serial.print(addr, HEX);
      found++;
    }
  }
  if (found == 0) {
    Serial.println(F("няма устройства"));
  } else {
    Serial.println();
  }
}

// Инициализация само на IMU (MPU-6500 / част от MPU-9250), без AK8963
static bool initMpu6500Raw(uint8_t addr) {
  i2cWriteByte(addr, MPU_REG_PWR_MGMT_1, 0x80);
  delay(100);
  i2cWriteByte(addr, MPU_REG_PWR_MGMT_1, 0x01);
  delay(100);
  i2cWriteByte(addr, 0x1A, 0x03);
  i2cWriteByte(addr, 0x19, 0x04);
  i2cWriteByte(addr, 0x1B, 0x00);
  i2cWriteByte(addr, 0x1C, 0x00);
  i2cWriteByte(addr, 0x1D, 0x03);

  const uint8_t who = i2cReadByte(addr, MPU_REG_WHO_AM_I);
  if (!mpuWhoAmIValid(who)) {
    return false;
  }

  mpuI2cAddr = addr;
  mpuMode = MPU_MODE_6500_RAW;
  mpuOk = true;
  mpuI2cSdaPin = D3;
  mpuI2cSclPin = D4;

  Serial.print(F("MPU-6500/IMU: [OK] D3/D4, адрес 0x"));
  Serial.print(addr, HEX);
  Serial.print(F(", WHO_AM_I=0x"));
  Serial.print(who, HEX);
  Serial.println(F(" (без магнитометър — нормално за GY-6500)"));
  return true;
}

static bool readMpu6500Raw(float& ax, float& ay, float& az,
                           float& gx, float& gy, float& gz) {
  Wire.beginTransmission(mpuI2cAddr);
  Wire.write(MPU_REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(mpuI2cAddr, static_cast<uint8_t>(14)) != 14) {
    return false;
  }

  const int16_t axRaw = (Wire.read() << 8) | Wire.read();
  const int16_t ayRaw = (Wire.read() << 8) | Wire.read();
  const int16_t azRaw = (Wire.read() << 8) | Wire.read();
  Wire.read();
  Wire.read();
  const int16_t gxRaw = (Wire.read() << 8) | Wire.read();
  const int16_t gyRaw = (Wire.read() << 8) | Wire.read();
  const int16_t gzRaw = (Wire.read() << 8) | Wire.read();

  ax = (axRaw / MPU6500_ACCEL_LSB_PER_G) * G_TO_MS2;
  ay = (ayRaw / MPU6500_ACCEL_LSB_PER_G) * G_TO_MS2;
  az = (azRaw / MPU6500_ACCEL_LSB_PER_G) * G_TO_MS2;
  gx = (gxRaw / MPU6500_GYRO_LSB_PER_DPS) * MPU_DEG_TO_RAD;
  gy = (gyRaw / MPU6500_GYRO_LSB_PER_DPS) * MPU_DEG_TO_RAD;
  gz = (gzRaw / MPU6500_GYRO_LSB_PER_DPS) * MPU_DEG_TO_RAD;
  return true;
}

// Чете MPU (библиотека или директно I2C)
static bool readMpuSensors(float& ax, float& ay, float& az,
                           float& gx, float& gy, float& gz) {
  if (mpuMode == MPU_MODE_6500_RAW) {
    return readMpu6500Raw(ax, ay, az, gx, gy, gz);
  }
  if (mpuMode != MPU_MODE_LIB) {
    return false;
  }
  if (!mpu.update()) {
    return false;
  }
  ax = mpu.getAcc(0) * G_TO_MS2;
  ay = mpu.getAcc(1) * G_TO_MS2;
  az = mpu.getAcc(2) * G_TO_MS2;
  gx = mpu.getGyro(0) * MPU_DEG_TO_RAD;
  gy = mpu.getGyro(1) * MPU_DEG_TO_RAD;
  gz = mpu.getGyro(2) * MPU_DEG_TO_RAD;
  return true;
}

// MPU: SDA=D3, SCL=D4; първо пълен MPU9250, иначе само IMU (GY-6500)
static bool initMpuAuto() {
  Wire.begin(D3, D4);
  Wire.setClock(100000);
  delay(100);

  mpuMode = MPU_MODE_NONE;
  mpuOk = false;
  printI2cScan();

  const uint8_t addrs[] = {0x68, 0x69};
  for (uint8_t i = 0; i < 2; i++) {
    const uint8_t addr = addrs[i];
    if (!i2cPresent(addr)) {
      continue;
    }

    const uint8_t who = i2cReadByte(addr, MPU_REG_WHO_AM_I);
    Serial.print(F("Адрес 0x"));
    Serial.print(addr, HEX);
    Serial.print(F(" WHO_AM_I=0x"));
    Serial.println(who, HEX);

    if (!mpuWhoAmIValid(who)) {
      continue;
    }

    mpu.verbose(false);
    if (mpu.setup(addr)) {
      mpuMode = MPU_MODE_LIB;
      mpuOk = true;
      mpuI2cAddr = addr;
      mpuI2cSdaPin = D3;
      mpuI2cSclPin = D4;
      Serial.print(F("MPU-9250: [OK] D3/D4, адрес 0x"));
      Serial.println(addr, HEX);
      return true;
    }

    Serial.println(F("  hideakitai: няма AK8963 — пробвам само акселерометър/гиро..."));
    if (initMpu6500Raw(addr)) {
      return true;
    }
  }

  mpuMode = MPU_MODE_NONE;
  mpuOk = false;
  Serial.println(F("MPU: [НЕ ОК]"));
  Serial.println(F("  SDA→D3, SCL→D4, VCC→3V, GND, AD0→GND (адрес 0x68)"));
  Serial.println(F("  Ако I2C скан е празен — провери проводите и захранването"));
  return false;
}

// Чете NMEA от GPS през SoftwareSerial (D7)
static void pollGps() {
  while (gpsSerial.available() > 0) {
    const char c = static_cast<char>(gpsSerial.read());
    gps.encode(c);
    gpsBytesSinceBoot++;
    gpsBytesLastPeriod++;
    lastGpsByteMs = millis();
    if (gpsFirstByteMs == 0) {
      gpsFirstByteMs = lastGpsByteMs;
    }
  }
}

// Тест на GPS: брой байтове + дали има NMEA редове ($...GGA/RMC)
static int testGpsAtBaud(uint32_t baud, uint16_t waitMs, char* sampleLine, size_t sampleLen) {
  gpsSerial.begin(baud);
  delay(100);

  int byteCount = 0;
  int lineCount = 0;
  size_t linePos = 0;

  if (sampleLen > 0) {
    sampleLine[0] = '\0';
  }

  const unsigned long t0 = millis();
  while (millis() - t0 < waitMs) {
    while (gpsSerial.available() > 0) {
      const char c = static_cast<char>(gpsSerial.read());
      byteCount++;
      gps.encode(c);

      if (c == '$') {
        linePos = 0;
        if (sampleLen > 1) {
          sampleLine[0] = '$';
          sampleLine[1] = '\0';
          linePos = 1;
        }
      } else if (linePos > 0 && linePos < sampleLen - 1) {
        sampleLine[linePos++] = c;
        sampleLine[linePos] = '\0';
        if (c == '\n' || c == '\r') {
          lineCount++;
          linePos = 0;
        }
      } else if (c == '\n' || c == '\r') {
        if (linePos > 4) {
          lineCount++;
        }
        linePos = 0;
      }
    }
    yield();
  }

  return (byteCount > 20 && lineCount > 0) ? byteCount : 0;
}

static void initGpsLink() {
  char sample[8];
  int n9600 = testGpsAtBaud(GPS_Baud, 3000, sample, sizeof(sample));

  if (n9600 > 0) {
    gpsActiveBaud = GPS_Baud;
    gpsLinkOk = true;
  } else {
    int n38400 = testGpsAtBaud(GPS_Baud_Alt, 3000, sample, sizeof(sample));
    if (n38400 > 0) {
      gpsActiveBaud = GPS_Baud_Alt;
      gpsLinkOk = true;
    } else {
      gpsActiveBaud = GPS_Baud;
      gpsLinkOk = false;
    }
  }

  gpsSerial.begin(gpsActiveBaud);
  Serial.print(F("GPS (D7): "));
  Serial.println(gpsLinkOk ? F("[OK] модулът отговаря") : F("[НЕ ОК] няма данни — TX→D7"));
}

// Кратък тест на MPU при старт
static void initMpuWaveCheck() {
  if (!mpuOk) {
    Serial.println(F("MPU / вълни: [НЕ ОК] не е намерен"));
    return;
  }

  float ax, ay, az, gx, gy, gz;
  float maxGyro = 0.0f;
  for (int i = 0; i < 40; i++) {
    if (!readMpuSensors(ax, ay, az, gx, gy, gz)) {
      continue;
    }
    gx -= gyroBiasX;
    gy -= gyroBiasY;
    gz -= gyroBiasZ;
    const float gyroMag = sqrt(gx * gx + gy * gy + gz * gz);
    if (gyroMag > maxGyro) {
      maxGyro = gyroMag;
    }
    delay(8);
  }
  mpuCalmGyroMax = maxGyro;

  Serial.print(F("MPU / вълни: [OK] сензорът отговаря"));
  if (maxGyro >= MOVEMENT_THRESHOLD) {
    Serial.println(F(" (не мърдай платата при старт)"));
  } else {
    Serial.println();
  }
}

// Общ статус: работи ли всичко (на всеки 2 сек) — без см, само OK / не OK
static void printSystemHealth(float tempC, bool liquidDetected) {
  bool allOk = true;

  Serial.println(F("-------- СТАТУС (OK / не OK) --------"));

  Serial.print(F("MPU (вълни): "));
  if (mpuOk) {
    Serial.println(F("[OK]"));
  } else {
    Serial.println(F("[НЕ ОК] — провери I2C проводите"));
    allOk = false;
  }

  Serial.print(F("Температура: "));
  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println(F("[НЕ ОК]"));
    allOk = false;
  } else {
    Serial.print(F("[OK] "));
    Serial.print(tempC, 1);
    Serial.println(F(" °C"));
  }

  Serial.print(F("Ниво вода: "));
  Serial.print(F("[OK] "));
  Serial.println(liquidDetected ? F("течност") : F("сухо"));

  Serial.print(F("GPS: "));
  if (!gpsLinkOk || gps.charsProcessed() < 10) {
    Serial.println(F("[НЕ ОК] няма връзка (TX→D7)"));
    allOk = false;
  } else if (gps.location.isValid()) {
    Serial.print(F("[OK] "));
    Serial.print(gps.location.lat(), 5);
    Serial.print(F(", "));
    Serial.println(gps.location.lng(), 5);
  } else {
    Serial.print(F("[ЧАКА] модулът работи"));
    if (gps.satellites.isValid()) {
      Serial.print(F(", сателити: "));
      Serial.println(gps.satellites.value());
    } else {
      Serial.println(F(" — изнеси на открито"));
    }
  }

  Serial.print(F("ОБЩО: "));
  Serial.println(allOk ? F("всичко OK") : F("има проблем — виж [НЕ ОК]"));
  Serial.println(F("------------------------------------"));
}

void setup() {
  Serial.begin(DEBUG_Baud);
  delay(500);

  Serial.println(F("\n--- Инициализация ---"));
  Serial.println(F("Проверка: работи ли всеки сензор [OK] / [НЕ ОК]"));
  Serial.println(F("Монитор: 115200"));

  if (initMpuAuto()) {
    delay(1000);
    float ax, ay, az, gx, gy, gz;
    float sumG = 0;
    for (int i = 0; i < 300; i++) {
      if (readMpuSensors(ax, ay, az, gx, gy, gz)) {
        sumG += sqrt(ax * ax + ay * ay + az * az);
      }
      delay(4);
    }
    gravityOffset = sumG / 300.0;
    Serial.print(F("Калибрирано g: "));
    Serial.println(gravityOffset);

    float sumGx = 0.0f, sumGy = 0.0f, sumGz = 0.0f;
    for (int i = 0; i < 200; i++) {
      if (readMpuSensors(ax, ay, az, gx, gy, gz)) {
        sumGx += gx;
        sumGy += gy;
        sumGz += gz;
      }
      delay(4);
    }
    gyroBiasX = sumGx / 200.0f;
    gyroBiasY = sumGy / 200.0f;
    gyroBiasZ = sumGz / 200.0f;
    initMpuWaveCheck();
  }

  initGpsLink();
  pinMode(WATER_SENSOR_PIN, INPUT);
  tempSensor.begin();

  initWifiServer();

  lastTime = micros();
}

void loop() {
  static unsigned long lastLog = 0;

  webServer.handleClient();

  if (wifiConnected) {
    if (!mqttClient.connected()) {
      mqttEnsureConnected();
    }
    mqttClient.loop();
  }

  // --- Блок: четене на GPS (NMEA на D7) ---
  pollGps();

  // --- Блок: изчисляване на времевия интервал dt ---
  unsigned long currentTime = micros();
  float dt = (currentTime - lastTime) / 1000000.0;
  lastTime = currentTime;

  if (dt > 0.05f) {
    dt = 0.05f;
  }

  static bool waveDetectedInPeriod = false;
  static float prevWaveH = 0.0f;
  static bool waveRising = false;
  static unsigned long lastPeakMs = 0;
  static float periodSumSec = 0.0f;
  static uint8_t periodCount = 0;
  static float periodMaxGyroRad = 0.0f;
  static float prevGyroMag = 0.0f;
  static bool gyroRising = false;
  static unsigned long lastGyroPeakMs = 0;
  static float periodMaxTiltDeg = 0.0f;

  if (mpuOk) {
    float ax, ay, az, gx, gy, gz;
    if (readMpuSensors(ax, ay, az, gx, gy, gz)) {
      gx -= gyroBiasX;
      gy -= gyroBiasY;
      gz -= gyroBiasZ;

      float gyroXRate = gx * 57.2958f;
      float gyroYRate = gy * 57.2958f;

      const float totalGyroMovement = sqrt(gx * gx + gy * gy + gz * gz);

      float accPitch = atan2(-ax, sqrt(ay * ay + az * az)) * 57.2958f;
      float accRoll = atan2(ay, az) * 57.2958f;

      pitch = alpha * (pitch + gyroXRate * dt) + (1.0f - alpha) * accPitch;
      roll = alpha * (roll + gyroYRate * dt) + (1.0f - alpha) * accRoll;

      float pitchRad = pitch / 57.2958f;
      float rollRad = roll / 57.2958f;

      float accZ_world = ax * sin(pitchRad) - ay * sin(rollRad) * cos(pitchRad) +
                         az * cos(rollRad) * cos(pitchRad);
      float pureVerticalAcc = accZ_world - gravityOffset;

      if (fabs(pureVerticalAcc) < ACCEL_DEADZONE) {
        pureVerticalAcc = 0.0f;
      }

      const bool instantMotion = (totalGyroMovement >= MOVEMENT_THRESHOLD) ||
                                 (fabs(pureVerticalAcc) >= ACCEL_MOVEMENT_MIN);

      const float tiltMagDeg = sqrt(pitch * pitch + roll * roll);
      if (instantMotion && tiltMagDeg > periodMaxTiltDeg) {
        periodMaxTiltDeg = tiltMagDeg;
      }

      mpuLastGyroRad = totalGyroMovement;
      mpuLastVertAcc = pureVerticalAcc;
      mpuLastInstantMotion = instantMotion;

      if (instantMotion) {
        if (totalGyroMovement > periodMaxGyroRad) {
          periodMaxGyroRad = totalGyroMovement;
        }
        if (totalGyroMovement > prevGyroMag + 0.04f) {
          gyroRising = true;
        } else if (gyroRising && totalGyroMovement < prevGyroMag - 0.04f) {
          const unsigned long nowMs = millis();
          if (lastGyroPeakMs > 0) {
            const float periodS = (nowMs - lastGyroPeakMs) / 1000.0f;
            if (periodS >= WAVE_PERIOD_MIN_S && periodS <= WAVE_PERIOD_MAX_S) {
              periodSumSec += periodS;
              if (periodCount < 255) {
                periodCount++;
              }
            }
          }
          lastGyroPeakMs = nowMs;
          gyroRising = false;
        }
        prevGyroMag = totalGyroMovement;
      } else {
        gyroRising = false;
        prevGyroMag = 0.0f;
      }

      if (instantMotion) {
        movementOnCount++;
        movementOffCount = 0;
        if (movementOnCount > MOVEMENT_ON_SAMPLES) {
          movementOnCount = MOVEMENT_ON_SAMPLES;
        }
      } else {
        movementOffCount++;
        if (movementOffCount > MOVEMENT_OFF_SAMPLES) {
          movementOffCount = MOVEMENT_OFF_SAMPLES;
        }
        if (movementOffCount >= 3) {
          movementOnCount = 0;
        }
      }

      const bool realMotion = (movementOnCount >= MOVEMENT_ON_SAMPLES);
      mpuLastRealMotion = realMotion;
      if (realMotion) {
        waveDetectedInPeriod = true;
      }

      if (!realMotion) {
        velocityZ = 0.0f;
        waveHeight = 0.0f;
        isMoving = false;
        gravityOffset = (gravityOffset * 0.995f) + (accZ_world * 0.005f);
        waveRising = false;
        prevWaveH = 0.0f;
      } else {
        isMoving = true;
        velocityZ = hpfCoeff * (velocityZ + pureVerticalAcc * dt);
        waveHeight = hpfCoeff * (waveHeight + velocityZ * dt);

        if (waveHeight > maxPeak) {
          maxPeak = waveHeight;
        }
        if (waveHeight < minPeak) {
          minPeak = waveHeight;
        }

        // Период между пикове → прибл. фазова скорост c = g·T/(2π) (дълбока вода)
        if (waveHeight > prevWaveH + 0.0005f) {
          waveRising = true;
        } else if (waveRising && waveHeight < prevWaveH - 0.0005f) {
          const unsigned long nowMs = millis();
          if (lastPeakMs > 0) {
            const float periodS = (nowMs - lastPeakMs) / 1000.0f;
            if (periodS >= WAVE_PERIOD_MIN_S && periodS <= WAVE_PERIOD_MAX_S) {
              periodSumSec += periodS;
              if (periodCount < 255) {
                periodCount++;
              }
            }
          }
          lastPeakMs = nowMs;
          waveRising = false;
        }
        prevWaveH = waveHeight;
      }
    }
  }

  if (millis() - lastLog > 2000) {
    lastLog = millis();

    Serial.println();

    float totalWaveHeight = 0.0f;
    float logMaxTiltDeg = 0.0f;
    WaveClass waveClass = WAVE_CALM;

    if (mpuOk) {
      float integratedCm = 0.0f;
      if (waveDetectedInPeriod && (maxPeak > minPeak)) {
        integratedCm = (maxPeak - minPeak) * 100.0f;
      }
      const float gyroCm = periodMaxGyroRad * 57.2958f * GYRO_TO_HEIGHT_CM;
      totalWaveHeight = integratedCm;
      if (gyroCm > totalWaveHeight) {
        totalWaveHeight = gyroCm;
      }
      if (!waveDetectedInPeriod && periodMaxGyroRad >= MOVEMENT_THRESHOLD) {
        waveDetectedInPeriod = true;
        totalWaveHeight = gyroCm;
      }
      if (totalWaveHeight < WAVE_NOISE_CM) {
        totalWaveHeight = 0.0f;
      }
      mpuLastPeakSpanCm = totalWaveHeight;
      logMaxTiltDeg = periodMaxTiltDeg;
      waveClass = classifyWaveByTilt(logMaxTiltDeg);

      mpuLastWaveSpeedMps = 0.0f;
      if (periodCount > 0 && (totalWaveHeight >= WAVE_NOISE_CM || logMaxTiltDeg >= WAVE_TILT_CALM_DEG)) {
        const float avgPeriodS = periodSumSec / periodCount;
        mpuLastWaveSpeedMps = 9.81f * avgPeriodS / 6.2831853f;
      }
      periodSumSec = 0.0f;
      periodCount = 0;
      lastPeakMs = 0;
      lastGyroPeakMs = 0;
      periodMaxGyroRad = 0.0f;
      periodMaxTiltDeg = 0.0f;

      maxPeak = 0.0f;
      minPeak = 0.0f;
      velocityZ = 0.0f;
      waveHeight = 0.0f;
      waveDetectedInPeriod = false;
    }

    Serial.print(F("Височина на вълната (пик-пик, MPU): ~"));
    Serial.print(totalWaveHeight, 1);
    Serial.println(F(" см"));

    Serial.print(F("Скорост на вълната (прибл., MPU): ~"));
    if (mpuOk && mpuLastWaveSpeedMps > 0.05f) {
      Serial.print(mpuLastWaveSpeedMps, 2);
      Serial.print(F(" m/s ("));
      Serial.print(mpuLastWaveSpeedMps * 3.6f, 1);
      Serial.println(F(" km/h)"));
    } else if (mpuOk) {
      Serial.println(F("0.0 m/s — трясни платката за период"));
    } else {
      Serial.println(F("— MPU [НЕ ОК]"));
    }

    if (mpuOk) {
      Serial.print(F("Наклон (pitch/roll): "));
      Serial.print(pitch, 1);
      Serial.print(F("° / "));
      Serial.print(roll, 1);
      Serial.println(F("°"));
      Serial.print(F("MPU живо: гиро "));
      Serial.print(mpuLastGyroRad * 57.2958f, 1);
      Serial.print(F(" °/s, движение "));
      Serial.println(mpuLastRealMotion ? F("да") : F("не"));
    }

    Serial.print(F("Клас на вълната (по наклон): "));
    if (mpuOk) {
      Serial.print(waveClassLabel(waveClass));
      Serial.print(F(" — макс. "));
      Serial.print(logMaxTiltDeg, 1);
      Serial.println(F("°"));
    } else {
      Serial.println(F("— MPU [НЕ ОК]"));
    }

    tempSensor.requestTemperatures();
    const float tempC = tempSensor.getTempCByIndex(0);

    const int waterStatus = digitalRead(WATER_SENSOR_PIN);
    const bool liquidDetected = WATER_LIQUID_IS_HIGH
                                    ? (waterStatus == HIGH)
                                    : (waterStatus == LOW);

    emitDataJson(totalWaveHeight, waveClass, logMaxTiltDeg, tempC, liquidDetected);
    printSystemHealth(tempC, liquidDetected);
    gpsBytesLastPeriod = 0;
  }
}
