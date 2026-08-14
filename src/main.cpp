/*******************************************************
 * Smart Home ESP32 - TELEGRAM + THINGSBOARD Stage F Persistent Buffer
 * Board  : ESP32 DEVKIT
 * Input  : MQ2, DHT22, Magnet Sensor
 * Output : LED, Buzzer, Motor DC, LCD, Servo, Telegram Bot
 * Cloud  : ThingsBoard MQTT telemetry + event manager + server-side RPC
 * Storage: Persistent offline event buffer menggunakan ESP32 NVS
 *
 * Command Telegram:
 * /start, Help, Cek rumah, /status
 * Biru on/off, Merah on/off, Hijau on/off
 * Putih 1/2/3/off, Kipas 1/2/3/off, Bel
 * Pintu buka / Pintu tutup
 * Servo 0 ... Servo 180
 * Alarm off / /mute = matikan alarm kebakaran sementara
 * Alarm on / /unmute = aktifkan alarm kembali
 *******************************************************/
#include <Arduino.h>

#include <WiFi.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <FastBot.h>
#include <ESP32Servo.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <stddef.h>
#include "secrets.h"

// ========== WIFI SETTING ==========
const char* ssid = WIFI_SSID;
const char* pass = WIFI_PASSWORD;

// ========== TELEGRAM SETTING ==========
#define BOT_TOKEN TELEGRAM_TOKEN
#define CHAT_ID   TELEGRAM_CHAT_ID

FastBot bot(BOT_TOKEN);

// ========== THINGSBOARD MQTT SETTING ==========
const char* TB_TELEMETRY_TOPIC = "v1/devices/me/telemetry";
const char* TB_RPC_REQUEST_TOPIC = "v1/devices/me/rpc/request/+";
const char* TB_RPC_RESPONSE_PREFIX = "v1/devices/me/rpc/response/";

WiFiClient tbWiFiClient;
PubSubClient tbMqttClient(tbWiFiClient);

bool rpcSubscribed = false;
bool immediateTelemetryRequested = false;

// ========== PIN HARDWARE ==========
#define DHTPIN       4
#define DHTTYPE      DHT22
#define Lampu_biru   14   // Lampu depan
#define Lampu_putih  27   // Lampu tidur/PWM
#define Lampu_merah  26   // Lampu belakang
#define Lampu_hijau  25   // Lampu tengah
#define Kipas_angin  33   // Fan/PWM
#define Bell         32   // Buzzer/bell kuning S orange G
#define MQ2_SENSOR   16   // Digital output MQ2, aktif LOW pada program ini
#define Magnet       17   // Sensor magnet pintu
#define SERVO_PIN    13   // Motor servo pintu

// ========== SERVO SETTING ==========
Servo myServo;

// false untuk servo standar SG90 0-180 derajat.
// Ubah menjadi true hanya jika menggunakan servo continuous rotation/360 derajat.
const bool SERVO_360_CONTINUOUS = false;

const int SERVO_CLOSED_ANGLE = 0;     // Posisi pintu tertutup
const int SERVO_OPEN_ANGLE   = 180;   // Posisi pintu terbuka

// Pengaturan untuk servo continuous rotation/360 derajat.
// 90 = berhenti, 180 = putar arah buka, 0 = putar arah tutup.
const int SERVO_360_STOP        = 90;
const int SERVO_360_OPEN_SPEED  = 180;
const int SERVO_360_CLOSE_SPEED = 0;

const unsigned long SERVO_360_MANUAL_RUN_MS = 2500UL;
const unsigned long FIRE_SERVO_RUN_MS       = 4000UL;

int servoCommand = SERVO_CLOSED_ANGLE;
bool servoDoorOpen = false;
bool fireServoDone = false;

bool servoTimedMoveActive = false;
bool servoTimedTargetDoorOpen = false;
unsigned long servoTimedMoveStartMillis = 0;
unsigned long servoTimedMoveDurationMs = 0;

// ========== FIRE ALARM SETTING ==========
const float FIRE_TEMP_THRESHOLD       = 30.0;  // MQ2 aktif + suhu >= nilai ini => kebakaran
const float FIRE_TEMP_CLEAR_THRESHOLD = 29.0;  // alarm pulih jika gas aman dan suhu <= nilai ini
const float FIRE_CRITICAL_TEMP        = 60.0;  // suhu >= nilai ini => kebakaran meski MQ2 belum aktif
const unsigned long FIRE_BLINK_MS     = 300;
const unsigned long FIRE_LCD_MS       = 1000;
const unsigned long FIRE_MUTE_MS      = 60000UL; // 1 menit, untuk uji coba
const int FIRE_FAN_PWM                 = 255;     // Kipas maksimum selama alarm aktif

// ========== OBJECT ==========
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ========== SENSOR VALUE ==========
float humi = NAN;
float temp = NAN;
int MQ2_SENSOR_Value = HIGH;

// ========== STATUS FLAG ==========
bool fg = false;
bool fm = false;
bool fireActive = false;
bool fireNotifiedTelegram = false;
bool fireBlinkState = false;
bool fireMuted = false; // true = alarm fisik dibisukan sementara untuk uji coba

// Menyimpan kondisi koneksi agar status sensor dapat dikirim ulang
// ketika WiFi/Telegram tersambung kembali.
bool previousWiFiConnected = false;
bool initialOnlineSyncSent = false;
bool wifiStateInitialized = false;

// Status koneksi ThingsBoard khusus untuk event perubahan koneksi.
bool previousThingsBoardConnected = false;
bool thingsBoardStateInitialized = false;

// ========== OUTPUT STATE NORMAL ==========
int lampuBiruState  = LOW;
int lampuPutihPWM   = 0;
int lampuMerahState = LOW;
int lampuHijauState = LOW;
int kipasPWM        = 0;
int bellState       = LOW;

// ========== TIMER ==========
unsigned long lastDhtMillis = 0;
unsigned long lastLCDMillis = 0;
unsigned long lastFireBlinkMillis = 0;
unsigned long lastFireLcdMillis = 0;
unsigned long lastFireMuteMillis = 0;
unsigned long lastWiFiAttemptMillis = 0;
unsigned long lastSensorCheckMillis = 0;
unsigned long lastTelegramTickMillis = 0;
unsigned long lastFireTelegramTryMillis = 0;
unsigned long lastTelemetryMillis = 0;
unsigned long lastMqttRetryMillis = 0;
unsigned long lastRpcSubscribeMillis = 0;
int fireTelegramTryCount = 0;

const unsigned long FAST_SENSOR_CHECK_MS   = 100;
const unsigned long TELEGRAM_TICK_MS       = 1000;
const unsigned long TELEMETRY_INTERVAL_MS  = 15000UL;
const unsigned long MQTT_RETRY_MS          = 5000UL;
const unsigned long RPC_SUBSCRIBE_RETRY_MS = 5000UL;
const unsigned long FIRE_TELEGRAM_RETRY_MS = 5000UL;
const int FIRE_TELEGRAM_MAX_TRY = 3;

// Telegram bell pattern non-blocking
bool bellPatternActive = false;
bool bellPatternOn = false;
int bellBeepsLeft = 0;
unsigned long lastBellPatternMillis = 0;
const unsigned long BELL_ON_MS  = 250;
const unsigned long BELL_OFF_MS = 300;

// ========== EVENT MANAGER (RAM + PERSISTENT NVS BUFFER) ==========
const uint8_t EVENT_BUFFER_SIZE = 32;
const uint8_t MAX_EVENTS_PER_FLUSH = 3;

struct EventRecord {
  unsigned long sequence;
  char type[32];
  bool value;
  char source[20];
  unsigned long uptimeSeconds;
};

EventRecord eventBuffer[EVENT_BUFFER_SIZE];
uint8_t eventHead = 0;
uint8_t eventTail = 0;
uint8_t eventCount = 0;
unsigned long eventSequence = 0;
char lastEventType[32] = "NONE";

// ---------- Persistent storage untuk event offline ----------
// Preferences menggunakan NVS bawaan ESP32, sehingga tidak memerlukan library eksternal.
Preferences eventPreferences;

const char* EVENT_NVS_NAMESPACE = "eventbuf";
const char* EVENT_NVS_KEY       = "queue";
const uint32_t EVENT_STORAGE_MAGIC = 0x45565431UL; // "EVT1"
const uint16_t EVENT_STORAGE_VERSION = 1;

bool persistentStorageReady = false;
bool persistentStorageOk = false;
bool persistentBufferRestored = false;
uint8_t persistentStoredCount = 0;
unsigned long persistentSaveCount = 0;

struct PersistentEventSnapshot {
  uint32_t magic;
  uint16_t version;
  uint8_t count;
  uint8_t reserved;
  unsigned long eventSequence;
  char lastEventType[32];
  EventRecord events[EVENT_BUFFER_SIZE];
  uint32_t checksum;
};

// Satu buffer global menghindari alokasi snapshot sekitar 2 KB pada stack.
PersistentEventSnapshot persistentSnapshotBuffer;

// Cache sensor terpisah dari fg/fm agar event tidak terganggu oleh logika Telegram.
bool eventSensorStateInitialized = false;
bool previousGasForEvent = false;
bool previousDoorForEvent = false;

// =====================================================
// HELPER
// =====================================================
void lcdPrintLine(byte row, String text) {
  while (text.length() < 16) text += " ";
  lcd.setCursor(0, row);
  lcd.print(text.substring(0, 16));
}

String onOff(int value) {
  return value ? "ON" : "OFF";
}

int getEffectiveFanPWM() {
  return fireActive ? FIRE_FAN_PWM : kipasPWM;
}

bool getAlarmAudible() {
  // Status logis alarm; tidak mengikuti pulsa fisik 300 ms.
  return fireActive && !fireMuted;
}

bool getBuzzerActiveTelemetry() {
  // Status logis stabil untuk dashboard, bukan level pin pada setiap kedipan.
  return getAlarmAudible() || bellPatternActive || bellState == HIGH;
}

const char* getBuzzerMode() {
  if (fireActive) {
    return fireMuted ? "FIRE_MUTED" : "FIRE_BLINK";
  }

  if (bellPatternActive || bellState == HIGH) {
    return "MANUAL_BELL";
  }

  return "OFF";
}

bool getAlarmLampLogicalState(int normalState) {
  if (fireActive) {
    return !fireMuted;
  }
  return normalState == HIGH;
}

int getAlarmWhiteLampLogicalPWM() {
  if (fireActive) {
    return fireMuted ? 0 : 255;
  }
  return lampuPutihPWM;
}

bool telegramConfigured() {
  return String(BOT_TOKEN) != "ISI_TOKEN_BOT_TELEGRAM" &&
         String(CHAT_ID)   != "ISI_CHAT_ID_TELEGRAM" &&
         String(BOT_TOKEN).length() > 10 &&
         String(CHAT_ID).length() > 3;
}

bool thingsBoardConfigured() {
  return String(TB_HOST).length() > 0 &&
         String(TB_ACCESS_TOKEN).length() > 5 &&
         TB_PORT > 0;
}

bool sendTelegram(String message) {
  if (!telegramConfigured()) {
    Serial.println("Telegram belum dikonfigurasi. Cek BOT_TOKEN dan CHAT_ID.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Telegram gagal dikirim: WiFi belum terhubung.");
    return false;
  }

  Serial.println("Kirim Telegram:");
  Serial.println(message);
  bot.sendMessage(message);
  return true;
}

bool isGasDetected() {
  MQ2_SENSOR_Value = digitalRead(MQ2_SENSOR);
  return (MQ2_SENSOR_Value == LOW);
}

bool isDoorOpen() {
  return (digitalRead(Magnet) == HIGH);
}

// Ringkasan status untuk pesan Telegram.
String getSensorIndicatorText() {
  String s = "Status indikator sensor:\n";
  s += "Gas/asap: ";
  s += isGasDetected() ? "AKTIF / TERDETEKSI" : "AMAN";
  s += "\nPintu magnet: ";
  s += isDoorOpen() ? "AKTIF / TERBUKA" : "TERTUTUP";
  s += "\nKebakaran: ";
  s += fireActive ? "AKTIF" : "NORMAL";
  return s;
}

// =====================================================
// EVENT MANAGER
// =====================================================
void copyEventText(char* destination, size_t destinationSize, const char* source) {
  if (destinationSize == 0) return;
  strncpy(destination, source, destinationSize - 1);
  destination[destinationSize - 1] = '\0';
}

uint32_t calculatePersistentSnapshotChecksum(
  const PersistentEventSnapshot& snapshot
) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&snapshot);
  const size_t dataLength = offsetof(PersistentEventSnapshot, checksum);

  // FNV-1a 32-bit: ringan dan cukup untuk mendeteksi snapshot NVS yang rusak.
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < dataLength; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

void resetEventQueueInRam() {
  memset(eventBuffer, 0, sizeof(eventBuffer));
  eventHead = 0;
  eventTail = 0;
  eventCount = 0;
  eventSequence = 0;
  copyEventText(lastEventType, sizeof(lastEventType), "NONE");
}

bool saveEventQueueToNvs() {
  if (!persistentStorageReady) {
    persistentStorageOk = false;
    return false;
  }

  PersistentEventSnapshot& snapshot = persistentSnapshotBuffer;
  memset(&snapshot, 0, sizeof(snapshot));
  snapshot.magic = EVENT_STORAGE_MAGIC;
  snapshot.version = EVENT_STORAGE_VERSION;
  snapshot.count = eventCount;
  snapshot.eventSequence = eventSequence;
  copyEventText(
    snapshot.lastEventType,
    sizeof(snapshot.lastEventType),
    lastEventType
  );

  // Simpan event dalam urutan FIFO yang linear agar mudah dipulihkan.
  for (uint8_t i = 0; i < eventCount; i++) {
    uint8_t sourceIndex = (eventHead + i) % EVENT_BUFFER_SIZE;
    snapshot.events[i] = eventBuffer[sourceIndex];
  }

  snapshot.checksum = calculatePersistentSnapshotChecksum(snapshot);

  size_t written = eventPreferences.putBytes(
    EVENT_NVS_KEY,
    &snapshot,
    sizeof(snapshot)
  );

  bool success = (written == sizeof(snapshot));
  persistentStorageOk = success;

  if (success) {
    persistentStoredCount = eventCount;
    persistentSaveCount++;
    Serial.print("Persistent event buffer disimpan ke NVS. count=");
    Serial.println(persistentStoredCount);
  } else {
    Serial.print("GAGAL menyimpan persistent event buffer. bytes=");
    Serial.print(written);
    Serial.print("/");
    Serial.println(sizeof(snapshot));
  }

  return success;
}

bool restoreEventQueueFromNvs() {
  if (!persistentStorageReady) {
    persistentStorageOk = false;
    return false;
  }

  size_t storedLength = eventPreferences.getBytesLength(EVENT_NVS_KEY);

  if (storedLength == 0) {
    persistentStorageOk = true;
    persistentBufferRestored = false;
    persistentStoredCount = 0;
    Serial.println("Persistent event buffer: belum ada data tersimpan.");
    return true;
  }

  if (storedLength != sizeof(PersistentEventSnapshot)) {
    Serial.print("Persistent event buffer tidak kompatibel. bytes=");
    Serial.print(storedLength);
    Serial.print(", expected=");
    Serial.println(sizeof(PersistentEventSnapshot));
    eventPreferences.clear();
    resetEventQueueInRam();
    persistentStorageOk = false;
    persistentStoredCount = 0;
    return false;
  }

  PersistentEventSnapshot& snapshot = persistentSnapshotBuffer;
  memset(&snapshot, 0, sizeof(snapshot));
  size_t readLength = eventPreferences.getBytes(
    EVENT_NVS_KEY,
    &snapshot,
    sizeof(snapshot)
  );

  uint32_t expectedChecksum = calculatePersistentSnapshotChecksum(snapshot);
  bool valid =
    readLength == sizeof(snapshot) &&
    snapshot.magic == EVENT_STORAGE_MAGIC &&
    snapshot.version == EVENT_STORAGE_VERSION &&
    snapshot.count <= EVENT_BUFFER_SIZE &&
    snapshot.checksum == expectedChecksum;

  if (!valid) {
    Serial.println("Persistent event buffer rusak/tidak valid dan akan direset.");
    eventPreferences.clear();
    resetEventQueueInRam();
    persistentStorageOk = false;
    persistentStoredCount = 0;
    return false;
  }

  resetEventQueueInRam();

  eventCount = snapshot.count;
  eventHead = 0;
  eventTail = eventCount % EVENT_BUFFER_SIZE;
  eventSequence = snapshot.eventSequence;
  copyEventText(lastEventType, sizeof(lastEventType), snapshot.lastEventType);

  for (uint8_t i = 0; i < eventCount; i++) {
    eventBuffer[i] = snapshot.events[i];

    // Pastikan sequence berikutnya selalu lebih besar daripada event yang dipulihkan.
    if (eventBuffer[i].sequence > eventSequence) {
      eventSequence = eventBuffer[i].sequence;
    }
  }

  persistentStoredCount = eventCount;
  persistentBufferRestored = eventCount > 0;
  persistentStorageOk = true;

  Serial.print("Persistent event buffer dipulihkan dari NVS. count=");
  Serial.print(eventCount);
  Serial.print(", sequence terakhir=");
  Serial.println(eventSequence);

  return true;
}

void initializePersistentEventStorage() {
  persistentStorageReady = eventPreferences.begin(
    EVENT_NVS_NAMESPACE,
    false
  );

  if (!persistentStorageReady) {
    persistentStorageOk = false;
    Serial.println("GAGAL membuka NVS untuk persistent event buffer.");
    return;
  }

  persistentStorageOk = true;
  restoreEventQueueFromNvs();
}

bool eventQueueMustBePersistedNow() {
  // Event hanya wajib ditulis ke flash ketika koneksi cloud tidak tersedia,
  // atau ketika masih ada backlog yang sebelumnya sudah disimpan.
  // Ini menghindari penulisan NVS berulang saat knob/RPC digunakan ketika online.
  return WiFi.status() != WL_CONNECTED ||
         !tbMqttClient.connected() ||
         persistentStoredCount > 0;
}

void enqueueEvent(const char* type, bool value, const char* source) {
  eventSequence++;
  copyEventText(lastEventType, sizeof(lastEventType), type);

  // Jika penuh, buang event tertua agar event terbaru tetap tercatat.
  if (eventCount >= EVENT_BUFFER_SIZE) {
    Serial.print("Event buffer penuh. Event tertua dibuang: ");
    Serial.println(eventBuffer[eventHead].type);
    eventHead = (eventHead + 1) % EVENT_BUFFER_SIZE;
    eventCount--;
  }

  EventRecord& event = eventBuffer[eventTail];
  event.sequence = eventSequence;
  copyEventText(event.type, sizeof(event.type), type);
  event.value = value;
  copyEventText(event.source, sizeof(event.source), source);
  event.uptimeSeconds = millis() / 1000UL;

  eventTail = (eventTail + 1) % EVENT_BUFFER_SIZE;
  eventCount++;

  Serial.print("EVENT #");
  Serial.print(event.sequence);
  Serial.print(" ");
  Serial.print(event.type);
  Serial.print(" value=");
  Serial.print(event.value ? "true" : "false");
  Serial.print(" source=");
  Serial.print(event.source);
  Serial.print(" buffer=");
  Serial.println(eventCount);

  // Saat offline, simpan antrean seketika agar tetap ada jika listrik mati/reset.
  // Saat online tanpa backlog, event cukup berada di RAM lalu segera dipublish.
  if (eventQueueMustBePersistedNow()) {
    saveEventQueueToNvs();
  }
}

bool publishEventRecord(const EventRecord& event) {
  if (!tbMqttClient.connected()) return false;

  String payload;
  payload.reserve(360);
  payload += "{\"event_type\":\"";
  payload += event.type;
  payload += "\",\"event_sequence\":";
  payload += String(event.sequence);
  payload += ",\"event_value\":";
  payload += event.value ? "true" : "false";
  payload += ",\"event_source\":\"";
  payload += event.source;
  payload += "\",\"event_uptime_seconds\":";
  payload += String(event.uptimeSeconds);
  payload += ",\"last_event\":\"";
  payload += event.type;
  payload += "\",\"buffer_count\":";
  payload += String(eventCount > 0 ? eventCount - 1 : 0);
  payload += "}";

  bool published = tbMqttClient.publish(
    TB_TELEMETRY_TOPIC,
    payload.c_str()
  );

  Serial.print("Publish event ");
  Serial.print(event.type);
  Serial.print(": ");
  Serial.println(published ? "BERHASIL" : "GAGAL");

  if (published) {
    Serial.println(payload);
  }

  return published;
}

void flushEventQueue() {
  if (!tbMqttClient.connected() || eventCount == 0) return;

  uint8_t sentThisLoop = 0;
  bool queueChanged = false;
  bool publishFailed = false;

  while (eventCount > 0 &&
         sentThisLoop < MAX_EVENTS_PER_FLUSH &&
         tbMqttClient.connected()) {
    EventRecord& event = eventBuffer[eventHead];

    if (!publishEventRecord(event)) {
      publishFailed = true;
      break;
    }

    eventHead = (eventHead + 1) % EVENT_BUFFER_SIZE;
    eventCount--;
    sentThisLoop++;
    queueChanged = true;

    // Menjaga sesi MQTT tetap hidup tanpa membuat proses blocking panjang.
    tbMqttClient.loop();
  }

  bool connectionLostWithPendingEvents =
    eventCount > 0 && !tbMqttClient.connected();

  // Jika antrean berasal dari NVS, perbarui snapshot setelah satu batch berhasil.
  // Jika publish gagal/koneksi terputus, simpan sisa antrean agar tidak hilang.
  if ((queueChanged && persistentStoredCount > 0) ||
      publishFailed ||
      connectionLostWithPendingEvents) {
    saveEventQueueToNvs();
  }
}

void checkSensorEvents() {
  bool gasNow = isGasDetected();
  bool doorNow = isDoorOpen();

  if (!eventSensorStateInitialized) {
    previousGasForEvent = gasNow;
    previousDoorForEvent = doorNow;
    eventSensorStateInitialized = true;
    return;
  }

  if (gasNow != previousGasForEvent) {
    enqueueEvent(
      gasNow ? "GAS_DETECTED" : "GAS_CLEARED",
      gasNow,
      "SENSOR"
    );
    previousGasForEvent = gasNow;
  }

  if (doorNow != previousDoorForEvent) {
    enqueueEvent(
      doorNow ? "DOOR_OPENED" : "DOOR_CLOSED",
      doorNow,
      "SENSOR"
    );
    previousDoorForEvent = doorNow;
  }
}

void syncThingsBoardConnectionEvents() {
  bool nowConnected = tbMqttClient.connected();

  // Koneksi pertama setelah boot tidak disebut reconnect.
  if (!thingsBoardStateInitialized) {
    if (nowConnected) {
      thingsBoardStateInitialized = true;
      previousThingsBoardConnected = true;
    }
    return;
  }

  if (nowConnected != previousThingsBoardConnected) {
    enqueueEvent(
      nowConnected ? "THINGSBOARD_RECONNECTED" : "THINGSBOARD_DISCONNECTED",
      nowConnected,
      "SYSTEM"
    );
    previousThingsBoardConnected = nowConnected;
  }
}

// =====================================================
// SERVO CONTROL
// =====================================================
void writeServoRaw(int value) {
  servoCommand = constrain(value, 0, 180);
  myServo.write(servoCommand);
}

void setServoAngle(int angle) {
  servoTimedMoveActive = false;
  writeServoRaw(angle);

  int middleAngle = (SERVO_CLOSED_ANGLE + SERVO_OPEN_ANGLE) / 2;
  if (SERVO_OPEN_ANGLE >= SERVO_CLOSED_ANGLE) {
    servoDoorOpen = (servoCommand >= middleAngle);
  } else {
    servoDoorOpen = (servoCommand <= middleAngle);
  }
}

void startServoTimedMove(
  int command,
  unsigned long durationMs,
  bool targetDoorOpen
) {
  servoTimedMoveActive = true;
  servoTimedTargetDoorOpen = targetDoorOpen;
  servoTimedMoveStartMillis = millis();
  servoTimedMoveDurationMs = durationMs;
  writeServoRaw(command);
}

void updateServoTimedMove() {
  if (!servoTimedMoveActive) return;

  if (millis() - servoTimedMoveStartMillis >= servoTimedMoveDurationMs) {
    servoTimedMoveActive = false;

    if (SERVO_360_CONTINUOUS) {
      writeServoRaw(SERVO_360_STOP);
    }

    servoDoorOpen = servoTimedTargetDoorOpen;
  }
}

bool setDoorServo(bool open) {
  // Selama kebakaran, pintu tidak boleh ditutup dari Telegram maupun ThingsBoard.
  if (fireActive && !open) {
    servoDoorOpen = true;
    Serial.println("Pintu tidak ditutup karena status kebakaran masih aktif.");
    return false;
  }

  if (SERVO_360_CONTINUOUS) {
    int command = open ? SERVO_360_OPEN_SPEED : SERVO_360_CLOSE_SPEED;
    startServoTimedMove(
      command,
      SERVO_360_MANUAL_RUN_MS,
      open
    );
  } else {
    int targetAngle = open ? SERVO_OPEN_ANGLE : SERVO_CLOSED_ANGLE;
    setServoAngle(targetAngle);
    servoDoorOpen = open;
  }

  return true;
}

void triggerFireServoOpen() {
  if (fireServoDone) return;

  fireServoDone = true;
  Serial.println("==> Servo membuka pintu karena kebakaran");

  if (SERVO_360_CONTINUOUS) {
    startServoTimedMove(
      SERVO_360_OPEN_SPEED,
      FIRE_SERVO_RUN_MS,
      true
    );
  } else {
    setDoorServo(true);
  }
}

// =====================================================
// OUTPUT CONTROL
// =====================================================
void applyNormalOutputs() {
  digitalWrite(Lampu_biru, lampuBiruState);
  digitalWrite(Lampu_hijau, lampuHijauState);
  digitalWrite(Lampu_merah, lampuMerahState);
  analogWrite(Lampu_putih, lampuPutihPWM);
  analogWrite(Kipas_angin, kipasPWM);
  digitalWrite(Bell, bellState);
}

void setLampuBiru(int state) {
  lampuBiruState = state ? HIGH : LOW;
  if (!fireActive) digitalWrite(Lampu_biru, lampuBiruState);
}

void setLampuHijau(int state) {
  lampuHijauState = state ? HIGH : LOW;
  if (!fireActive) digitalWrite(Lampu_hijau, lampuHijauState);
}

void setLampuMerah(int state) {
  lampuMerahState = state ? HIGH : LOW;
  if (!fireActive) digitalWrite(Lampu_merah, lampuMerahState);
}

void setLampuPutihPWM(int pwm) {
  lampuPutihPWM = constrain(pwm, 0, 255);
  if (!fireActive) analogWrite(Lampu_putih, lampuPutihPWM);
}

void setKipasPWM(int pwm) {
  kipasPWM = constrain(pwm, 0, 255);
  if (!fireActive) analogWrite(Kipas_angin, kipasPWM);
}

void setBell(int state) {
  bellState = state ? HIGH : LOW;
  if (!fireActive && !bellPatternActive) digitalWrite(Bell, bellState);
}

void setFireOutputs(bool alarmPulseOn) {
  // Buzzer dan lampu alarm mengikuti status mute.
  bool audibleVisualAlarm = fireActive &&
                            !fireMuted &&
                            alarmPulseOn;

  digitalWrite(Lampu_biru,  audibleVisualAlarm ? HIGH : LOW);
  digitalWrite(Lampu_hijau, audibleVisualAlarm ? HIGH : LOW);
  digitalWrite(Lampu_merah, audibleVisualAlarm ? HIGH : LOW);
  analogWrite(Lampu_putih,  audibleVisualAlarm ? 255 : 0);
  digitalWrite(Bell,         audibleVisualAlarm ? HIGH : LOW);

  // Kipas tetap maksimum selama kebakaran, termasuk ketika mute.
  analogWrite(Kipas_angin, getEffectiveFanPWM());
}

// =====================================================
// BELL PATTERN UNTUK COMMAND TELEGRAM "Bel"
// =====================================================
void startBellPattern(int beepCount) {
  if (fireActive) return;
  bellPatternActive = true;
  bellPatternOn = true;
  bellBeepsLeft = beepCount;
  lastBellPatternMillis = millis();
  digitalWrite(Bell, HIGH);
}

void updateBellPattern() {
  if (!bellPatternActive || fireActive) return;

  if (bellPatternOn && millis() - lastBellPatternMillis >= BELL_ON_MS) {
    digitalWrite(Bell, LOW);
    bellPatternOn = false;
    bellBeepsLeft--;
    lastBellPatternMillis = millis();

    if (bellBeepsLeft <= 0) {
      bellPatternActive = false;
      bellState = LOW;
      return;
    }
  }

  if (!bellPatternOn && millis() - lastBellPatternMillis >= BELL_OFF_MS) {
    digitalWrite(Bell, HIGH);
    bellPatternOn = true;
    lastBellPatternMillis = millis();
  }
}

// =====================================================
// LCD NORMAL
// =====================================================
void showNormalLCD(String statusText = "Status:Aman") {
  if (fireActive) return;

  String line0;
  if (isnan(temp) || isnan(humi)) {
    line0 = "T=--C,H=--%";
  } else {
    line0 = "T=" + String(temp, 0) + "C,H=" + String(humi, 0) + "%";
  }
  lcdPrintLine(0, line0);
  lcdPrintLine(1, statusText);
}

// =====================================================
// BACA SENSOR DHT22
// =====================================================
void read_DHT22() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("Gagal membaca DHT22");
    if (!fireActive) showNormalLCD("Status:DHT Error");
    return;
  }

  humi = h;
  temp = t;
}

// =====================================================
// FIRE ALARM
// =====================================================
String fireMessage() {
  String msg = "KEBAKARAN TERDETEKSI!\n";
  msg += "Suhu: ";
  if (isnan(temp)) msg += "--";
  else msg += String(temp, 1);
  msg += " C\n";
  msg += "Gas/Asap: ";
  msg += isGasDetected() ? "TERDETEKSI" : "Tidak terdeteksi";
  msg += "\nPintu magnet: ";
  msg += isDoorOpen() ? "TERBUKA" : "TERTUTUP";
  msg += "\nKipas: MENYALA MAKSIMUM";
  msg += "\nServo: MEMBUKA PINTU";
  msg += "\nSegera periksa lokasi!";
  return msg;
}

void showFireLCD() {
  lcdPrintLine(0, "!! KEBAKARAN !!");
  String line1 = "T:";
  if (isnan(temp)) line1 += "--";
  else line1 += String(temp, 0);
  line1 += "C G:";
  line1 += isGasDetected() ? "YA" : "NO";
  lcdPrintLine(1, line1);
}

unsigned long fireMuteRemainingSeconds() {
  if (!fireMuted) return 0;
  unsigned long elapsed = millis() - lastFireMuteMillis;
  if (elapsed >= FIRE_MUTE_MS) return 0;
  return (FIRE_MUTE_MS - elapsed) / 1000UL;
}

void showFireMutedLCD() {
  lcdPrintLine(0, "ALARM DIMATIKAN");
  String line1 = "Sisa:" + String(fireMuteRemainingSeconds()) + " dtk";
  lcdPrintLine(1, line1);
}

bool muteFireAlarmFromSource(const char* source) {
  if (!fireActive) {
    fireMuted = false;
    applyNormalOutputs();
    showNormalLCD("Alarm tdk aktif");
    return false;
  }

  // Perintah mute berulang hanya memperpanjang waktu, bukan membuat event duplikat.
  if (fireMuted) {
    lastFireMuteMillis = millis();
    showFireMutedLCD();
    return true;
  }

  fireMuted = true;
  lastFireMuteMillis = millis();
  enqueueEvent("ALARM_MUTED", true, source);
  fireBlinkState = false;
  setFireOutputs(false);
  lcd.clear();
  showFireMutedLCD();
  Serial.println("==> Buzzer dan lampu alarm dimatikan sementara; kipas tetap maksimum");
  return true;
}

bool muteFireAlarm() {
  return muteFireAlarmFromSource("TELEGRAM");
}

void unmuteFireAlarmFromSource(const char* source) {
  bool wasMuted = fireMuted;
  fireMuted = false;
  lastFireMuteMillis = 0;

  if (wasMuted && fireActive) {
    enqueueEvent("ALARM_UNMUTED", false, source);
  }

  if (fireActive) {
    // Buzzer dan lampu melanjutkan pola alarm; kipas sejak awal tetap maksimum.
    setFireOutputs(false);
    lcd.clear();
    showFireLCD();
    Serial.println("==> Buzzer dan lampu alarm diaktifkan kembali; kipas tetap maksimum");
  } else {
    showNormalLCD("Status:Aman");
  }
}

void unmuteFireAlarm() {
  unmuteFireAlarmFromSource("TELEGRAM");
}

void updateFireAlarm() {
  bool gasDetected = isGasDetected();
  bool suhuValid = !isnan(temp);
  bool suhuTinggi = suhuValid && temp >= FIRE_TEMP_THRESHOLD;
  bool suhuKritis = suhuValid && temp >= FIRE_CRITICAL_TEMP;

  bool fireCondition = (gasDetected && suhuTinggi) || suhuKritis;
  bool clearCondition = !gasDetected && suhuValid && temp <= FIRE_TEMP_CLEAR_THRESHOLD;

  if (fireCondition && !fireActive) {
    fireActive = true;
    fireNotifiedTelegram = false;
    fireBlinkState = false;
    fireMuted = false;
    fireServoDone = false;
    bellPatternActive = false;
    lastFireMuteMillis = 0;
    lastFireBlinkMillis = 0;
    lastFireLcdMillis = 0;
    lastFireTelegramTryMillis = 0;
    fireTelegramTryCount = 0;

    // Simpan kondisi awal sensor. Setelah ini, perubahan gas atau pintu
    // selama kebakaran tetap dapat mengirim notifikasi Telegram.
    fg = gasDetected;
    fm = isDoorOpen();

    enqueueEvent("FIRE_STARTED", true, "LOCAL_EDGE");

    // Kipas langsung menyala penuh. Lampu dan buzzer mulai dari OFF,
    // lalu berkedip berdasarkan FIRE_BLINK_MS.
    setFireOutputs(false);

    Serial.println("==> KEBAKARAN TERDETEKSI - KIPAS MENYALA");

    // Pintu dibuka satu kali ketika kebakaran baru terdeteksi.
    triggerFireServoOpen();

    lcd.clear();
    showFireLCD();

    fireNotifiedTelegram = sendTelegram(fireMessage());
    lastFireTelegramTryMillis = millis();
    fireTelegramTryCount = 1;
  }

  if (!fireActive) return;

  if (!fireNotifiedTelegram &&
      fireTelegramTryCount < FIRE_TELEGRAM_MAX_TRY &&
      millis() - lastFireTelegramTryMillis >= FIRE_TELEGRAM_RETRY_MS) {
    lastFireTelegramTryMillis = millis();
    fireTelegramTryCount++;
    fireNotifiedTelegram = sendTelegram(fireMessage());
  }

  // Jika kondisi sudah aman, alarm otomatis kembali normal.
  if (clearCondition) {
    fireActive = false;
    fireNotifiedTelegram = false;
    fireBlinkState = false;
    fireMuted = false;
    fireServoDone = false;
    lastFireMuteMillis = 0;
    lastFireTelegramTryMillis = 0;
    fireTelegramTryCount = 0;

    // Alarm selesai: kipas kembali ke nilai yang sebelumnya diminta pengguna.
    // Servo tidak menutup pintu secara otomatis.
    setFireOutputs(false);
    applyNormalOutputs();
    enqueueEvent("FIRE_CLEARED", false, "LOCAL_EDGE");

    String clearMessage =
      "Status kebakaran kembali NORMAL. Gas/asap aman dan suhu sudah turun. "
      "Kipas kembali ke pengaturan pengguna (PWM ";
    clearMessage += String(kipasPWM);
    clearMessage += "). Pintu tetap terbuka dan dapat ditutup kembali dari Telegram.";
    sendTelegram(clearMessage);

    Serial.print("==> Status kebakaran normal - kipas kembali ke PWM ");
    Serial.println(kipasPWM);
    showNormalLCD("Status:Aman");
    return;
  }

  // Mute hanya sementara. Setelah 1 menit, alarm fisik aktif lagi bila kondisi masih kebakaran.
  if (fireMuted && millis() - lastFireMuteMillis >= FIRE_MUTE_MS) {
    fireMuted = false;
    lastFireMuteMillis = 0;
    enqueueEvent("ALARM_MUTE_EXPIRED", false, "LOCAL_TIMER");
    setFireOutputs(false);
    lcd.clear();
    showFireLCD();
    sendTelegram(
      "Waktu mute alarm sudah habis. Karena kondisi kebakaran masih terdeteksi, "
      "buzzer dan lampu alarm aktif kembali. Kipas tetap maksimum."
    );
    Serial.println("==> Waktu mute selesai, buzzer dan lampu aktif kembali; kipas tetap maksimum");
  }

  if (fireMuted) {
    setFireOutputs(false); // buzzer dan lampu alarm tetap mati, status kebakaran tetap aktif
    if (millis() - lastFireLcdMillis >= FIRE_LCD_MS) {
      lastFireLcdMillis = millis();
      showFireMutedLCD();
    }
    return;
  }

  if (millis() - lastFireBlinkMillis >= FIRE_BLINK_MS) {
    lastFireBlinkMillis = millis();
    fireBlinkState = !fireBlinkState;
    setFireOutputs(fireBlinkState);
  }

  if (millis() - lastFireLcdMillis >= FIRE_LCD_MS) {
    lastFireLcdMillis = millis();
    showFireLCD();
  }
}

// =====================================================
// MONITOR GAS DAN PINTU NORMAL
// =====================================================
void cek_MQ() {
  bool gasDetected = isGasDetected();

  if (gasDetected && !fg) {
    fg = true;
    Serial.println("==> Gas/asap terdeteksi");
    sendTelegram("AWAS! Gas/asap terdeteksi oleh sensor MQ2.\n" + getSensorIndicatorText());
    showNormalLCD("Status:AWAS GAS");
  }
  else if (!gasDetected && fg) {
    fg = false;
    sendTelegram("AMAN! Gas/asap sudah tidak terdeteksi oleh sensor MQ2.\n" + getSensorIndicatorText());
    showNormalLCD("Status:Aman");
  }
}

void cek_Magnet() {
  bool doorOpen = isDoorOpen();

  if (doorOpen && !fm) {
    fm = true;
    Serial.println("==> Pintu terbuka terdeteksi");
    sendTelegram("AWAS! Pintu terbuka terdeteksi.\n" + getSensorIndicatorText());
    showNormalLCD("Status:AWS PINTU");
  }
  else if (!doorOpen && fm) {
    fm = false;
    Serial.println("==> Pintu kembali tertutup");
    sendTelegram("AMAN! Pintu kembali tertutup.\n" + getSensorIndicatorText());
    showNormalLCD("Status:Aman");
  }
}

// =====================================================
// STATUS DAN COMMAND TELEGRAM
// =====================================================
String getStatusText() {
  String s = "ESP32 Smart Home\n";
  s += "Suhu: ";
  if (isnan(temp)) s += "--";
  else s += String(temp, 1);
  s += " C\n";
  s += "Kelembaban: ";
  if (isnan(humi)) s += "--";
  else s += String(humi, 1);
  s += " %\n";
  s += "Gas/Asap: ";
  s += isGasDetected() ? "TERDETEKSI" : "Aman";
  s += "\nPintu: ";
  s += isDoorOpen() ? "Terbuka" : "Tertutup";
  s += "\nKebakaran: ";
  s += fireActive ? "YA" : "Tidak";
  s += "\nAlarm fisik: ";
  if (fireActive && fireMuted) {
    s += "DIMATIKAN SEMENTARA (sisa ";
    s += String(fireMuteRemainingSeconds());
    s += " dtk)";
  } else if (fireActive) {
    s += "AKTIF";
  } else {
    s += "Normal";
  }
  s += "\n\nOutput:";
  s += "\nLampu biru: ";
  s += onOff(lampuBiruState);
  s += "\nLampu hijau: ";
  s += onOff(lampuHijauState);
  s += "\nLampu merah: ";
  s += onOff(lampuMerahState);
  s += "\nLampu putih PWM: ";
  s += String(lampuPutihPWM);
  s += "\nKipas PWM diminta: ";
  s += String(kipasPWM);
  s += "\nKipas PWM efektif: ";
  s += String(getEffectiveFanPWM());
  if (fireActive) {
    s += fireMuted
      ? " (kebakaran; buzzer/lampu dimute)"
      : " (mode kebakaran)";
  }

  s += "\n\nServo pintu:";
  s += "\nMode: ";
  s += SERVO_360_CONTINUOUS ? "Continuous/360" : "Standar 0-180";
  s += "\nStatus servo: ";
  s += servoDoorOpen ? "Terbuka" : "Tertutup";
  s += "\nPerintah servo: ";
  s += String(servoCommand);

  if (servoTimedMoveActive) {
    s += "\nGerakan servo: BERJALAN";
  }

  return s;
}

String helpText() {
  String h = "Command ESP32 Smart Home:\n";
  h += "Cek rumah / /status / /sensor\n";
  h += "Biru on | Biru off\n";
  h += "Hijau on | Hijau off\n";
  h += "Merah on | Merah off\n";
  h += "Putih 1 | Putih 2 | Putih 3 | Putih off\n";
  h += "Lampu on | Lampu off\n";
  h += "Kipas 1 | Kipas 2 | Kipas 3 | Kipas off\n";
  h += "Bel\n";
  h += "Pintu buka | Pintu tutup\n";
  h += "Servo 0 ... Servo 180\n";
  h += "Semua on | Semua off\n";
  h += "Alarm off / /mute = matikan alarm sementara\n";
  h += "Alarm on / /unmute = aktifkan alarm kembali\n";
  h += "Help";
  return h;
}

void newMsg(FB_msg& msg) {
  if (telegramConfigured() && msg.chatID != String(CHAT_ID)) {
    return;
  }

  String cmd = msg.text;
  cmd.trim();
  String lower = cmd;
  lower.toLowerCase();

  if (lower == "/start" || lower == "help" || lower == "/help") {
    sendTelegram(helpText());
  }
  else if (lower == "cek rumah" || lower == "/status" || lower == "status" ||
           lower == "/sensor" || lower == "status sensor") {
    sendTelegram(getStatusText());
  }
  else if (lower == "alarm off" || lower == "matikan alarm" || lower == "/mute" || lower == "mute") {
    if (muteFireAlarm()) {
      sendTelegram(
        "Buzzer dan lampu alarm dimatikan sementara selama 1 menit. "
        "Kipas tetap maksimum dan servo tetap terbuka selama kebakaran."
      );
    } else {
      sendTelegram("Alarm kebakaran sedang tidak aktif.");
    }
  }
  else if (lower == "alarm on" || lower == "nyalakan alarm" || lower == "/unmute" || lower == "unmute") {
    unmuteFireAlarm();
    if (fireActive) sendTelegram("Buzzer dan lampu alarm diaktifkan kembali. Kipas tetap maksimum.");
    else sendTelegram("Alarm kebakaran sedang tidak aktif.");
  }
  else if (lower == "biru on") {
    setLampuBiru(HIGH);
    sendTelegram("Lampu biru: ON");
  }
  else if (lower == "biru off") {
    setLampuBiru(LOW);
    sendTelegram("Lampu biru: OFF");
  }
  else if (lower == "hijau on") {
    setLampuHijau(HIGH);
    sendTelegram("Lampu hijau: ON");
  }
  else if (lower == "hijau off") {
    setLampuHijau(LOW);
    sendTelegram("Lampu hijau: OFF");
  }
  else if (lower == "merah on") {
    setLampuMerah(HIGH);
    sendTelegram("Lampu merah: ON");
  }
  else if (lower == "merah off") {
    setLampuMerah(LOW);
    sendTelegram("Lampu merah: OFF");
  }
  else if (lower == "putih 1") {
    setLampuPutihPWM(85);
    sendTelegram("Lampu putih: redup");
  }
  else if (lower == "putih 2") {
    setLampuPutihPWM(170);
    sendTelegram("Lampu putih: sedang");
  }
  else if (lower == "putih 3") {
    setLampuPutihPWM(255);
    sendTelegram("Lampu putih: terang");
  }
  else if (lower == "putih off" || lower == "putih 0") {
    setLampuPutihPWM(0);
    sendTelegram("Lampu putih: OFF");
  }
  else if (lower == "lampu on") {
    setLampuBiru(HIGH);
    setLampuHijau(HIGH);
    setLampuMerah(HIGH);
    setLampuPutihPWM(255);
    sendTelegram("Semua lampu dinyalakan");
  }
  else if (lower == "lampu off") {
    setLampuBiru(LOW);
    setLampuHijau(LOW);
    setLampuMerah(LOW);
    setLampuPutihPWM(0);
    sendTelegram("Semua lampu dimatikan");
  }
  else if (lower == "kipas 1") {
    setKipasPWM(120);
    sendTelegram("Kipas: lambat");
  }
  else if (lower == "kipas 2") {
    setKipasPWM(170);
    sendTelegram("Kipas: sedang");
  }
  else if (lower == "kipas 3") {
    setKipasPWM(255);
    sendTelegram("Kipas: cepat");
  }
  else if (lower == "kipas off" || lower == "kipas 0") {
    setKipasPWM(0);
    sendTelegram("Kipas: OFF");
  }
  else if (
    lower == "pintu buka" ||
    lower == "servo buka" ||
    lower == "/buka" ||
    lower == "/open"
  ) {
    if (fireActive) {
      triggerFireServoOpen();
      sendTelegram("Pintu dipertahankan TERBUKA karena status kebakaran masih aktif.");
    } else {
      setDoorServo(true);
      sendTelegram("Servo membuka pintu.");
    }
  }
  else if (
    lower == "pintu tutup" ||
    lower == "servo tutup" ||
    lower == "/tutup" ||
    lower == "/close"
  ) {
    if (!setDoorServo(false)) {
      sendTelegram("Pintu tidak dapat ditutup selama status kebakaran masih aktif.");
    } else {
      sendTelegram("Servo menutup pintu.");
    }
  }
  else if (lower.startsWith("servo ")) {
    String angleText = lower.substring(6);
    angleText.trim();

    bool numeric = angleText.length() > 0;
    for (unsigned int i = 0; i < angleText.length(); i++) {
      if (!isDigit(angleText.charAt(i))) {
        numeric = false;
        break;
      }
    }

    int angle = numeric ? angleText.toInt() : -1;

    if (!numeric || angle < 0 || angle > 180) {
      sendTelegram("Nilai servo harus berupa angka 0 sampai 180. Contoh: Servo 90");
    }
    else if (fireActive) {
      triggerFireServoOpen();
      sendTelegram("Kontrol sudut servo diabaikan karena kebakaran aktif. Pintu dipertahankan terbuka.");
    }
    else if (SERVO_360_CONTINUOUS) {
      servoTimedMoveActive = false;
      writeServoRaw(angle);
      servoDoorOpen = (angle == SERVO_360_OPEN_SPEED);

      String reply = "Perintah servo continuous: ";
      reply += String(angle);
      reply += ". Gunakan Servo 90 untuk berhenti.";
      sendTelegram(reply);
    }
    else {
      setServoAngle(angle);
      sendTelegram("Sudut servo diatur ke " + String(angle) + " derajat.");
    }
  }
  else if (lower == "bel" || lower == "bell") {
    startBellPattern(3);
    sendTelegram("Bell dibunyikan.");
  }
  else if (lower == "semua off") {
    setLampuBiru(LOW);
    setLampuHijau(LOW);
    setLampuMerah(LOW);
    setLampuPutihPWM(0);
    setKipasPWM(0);
    sendTelegram("Semua dimatikan.");
  }
  else if (lower == "semua on") {
    setLampuBiru(HIGH);
    setLampuHijau(HIGH);
    setLampuMerah(HIGH);
    setLampuPutihPWM(255);
    setKipasPWM(255);
    startBellPattern(3);
    sendTelegram("Semua dinyalakan.");
  }
  else {
    sendTelegram("Format salah. Ketik Help untuk melihat daftar command.");
  }

  if (fireActive && !fireMuted) {
    sendTelegram("Catatan: alarm kebakaran sedang aktif. Output fisik mengikuti mode alarm sampai kondisi aman atau alarm dimatikan sementara.");
  }

  immediateTelemetryRequested = true;
}

// =====================================================
// CONNECTION MANAGEMENT
// =====================================================
void startWiFi() {
  Serial.print("Memulai koneksi WiFi ke ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);

  // Mengurangi kemungkinan koneksi MQTT terputus karena power-save Wi-Fi.
  WiFi.setSleep(false);

  WiFi.begin(ssid, pass);
  lastWiFiAttemptMillis = millis();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWiFiAttemptMillis < 15000UL) {
    return;
  }

  lastWiFiAttemptMillis = millis();
  Serial.println("Mencoba reconnect WiFi...");
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
}

bool connectThingsBoard() {
  if (!thingsBoardConfigured()) {
    Serial.println("ThingsBoard belum dikonfigurasi. Cek secrets.h.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (tbMqttClient.connected()) {
    return true;
  }

  String clientId = "ESP32-SmartHome-";
  clientId += WiFi.macAddress();
  clientId.replace(":", "");

  Serial.print("Menghubungkan ThingsBoard MQTT ke ");
  Serial.print(TB_HOST);
  Serial.print(":");
  Serial.println(TB_PORT);

  bool connected = tbMqttClient.connect(
    clientId.c_str(),
    TB_ACCESS_TOKEN,
    nullptr
  );

  if (connected) {
    Serial.println("ThingsBoard MQTT terhubung.");

    rpcSubscribed = tbMqttClient.subscribe(TB_RPC_REQUEST_TOPIC);
    lastRpcSubscribeMillis = millis();

    Serial.print("Subscribe RPC ");
    Serial.print(TB_RPC_REQUEST_TOPIC);
    Serial.print(": ");
    Serial.println(rpcSubscribed ? "BERHASIL" : "GAGAL");
  } else {
    rpcSubscribed = false;
    Serial.print("ThingsBoard MQTT gagal. State=");
    Serial.println(tbMqttClient.state());
  }

  return connected;
}

void maintainThingsBoard() {
  if (WiFi.status() != WL_CONNECTED) {
    if (tbMqttClient.connected()) {
      tbMqttClient.disconnect();
    }
    rpcSubscribed = false;
    return;
  }

  if (tbMqttClient.connected()) {
    if (!rpcSubscribed &&
        millis() - lastRpcSubscribeMillis >= RPC_SUBSCRIBE_RETRY_MS) {
      lastRpcSubscribeMillis = millis();
      rpcSubscribed = tbMqttClient.subscribe(TB_RPC_REQUEST_TOPIC);

      Serial.print("Retry subscribe RPC: ");
      Serial.println(rpcSubscribed ? "BERHASIL" : "GAGAL");
    }

    tbMqttClient.loop();
    return;
  }

  rpcSubscribed = false;

  if (millis() - lastMqttRetryMillis < MQTT_RETRY_MS) {
    return;
  }

  lastMqttRetryMillis = millis();
  connectThingsBoard();
}

bool publishTelemetry() {
  if (!tbMqttClient.connected()) {
    return false;
  }

  bool gasDetected = isGasDetected();
  bool doorOpen = isDoorOpen();
  bool dhtValid = !isnan(temp) && !isnan(humi);

  String payload;
  payload.reserve(1100);
  payload += "{";

  bool hasPreviousField = false;

  if (dhtValid) {
    payload += "\"temperature\":";
    payload += String(temp, 1);
    payload += ",\"humidity\":";
    payload += String(humi, 1);
    hasPreviousField = true;
  }

  if (hasPreviousField) payload += ",";
  payload += "\"gas_detected\":";
  payload += gasDetected ? "true" : "false";

  payload += ",\"door_open\":";
  payload += doorOpen ? "true" : "false";

  payload += ",\"fire_active\":";
  payload += fireActive ? "true" : "false";

  payload += ",\"fire_muted\":";
  payload += fireMuted ? "true" : "false";

  payload += ",\"alarm_audible\":";
  payload += getAlarmAudible() ? "true" : "false";

  payload += ",\"lamp_blue\":";
  payload += getAlarmLampLogicalState(lampuBiruState) ? "true" : "false";

  payload += ",\"lamp_green\":";
  payload += getAlarmLampLogicalState(lampuHijauState) ? "true" : "false";

  payload += ",\"lamp_red\":";
  payload += getAlarmLampLogicalState(lampuMerahState) ? "true" : "false";

  payload += ",\"lamp_white_pwm\":";
  payload += String(getAlarmWhiteLampLogicalPWM());

  payload += ",\"fan_pwm_requested\":";
  payload += String(kipasPWM);

  payload += ",\"fan_pwm_effective\":";
  payload += String(getEffectiveFanPWM());

  payload += ",\"buzzer_active\":";
  payload += getBuzzerActiveTelemetry() ? "true" : "false";

  payload += ",\"buzzer_mode\":\"";
  payload += getBuzzerMode();
  payload += "\"";

  payload += ",\"servo_angle\":";
  payload += String(servoCommand);

  payload += ",\"servo_door_open\":";
  payload += servoDoorOpen ? "true" : "false";

  payload += ",\"wifi_status\":true";

  payload += ",\"wifi_rssi\":";
  payload += String(WiFi.RSSI());

  payload += ",\"thingsboard_connected\":true";

  payload += ",\"uptime_seconds\":";
  payload += String(millis() / 1000UL);

  payload += ",\"buffer_count\":";
  payload += String(eventCount);

  payload += ",\"persistent_buffer_count\":";
  payload += String(persistentStoredCount);

  payload += ",\"persistent_buffer_restored\":";
  payload += persistentBufferRestored ? "true" : "false";

  payload += ",\"persistent_storage_ok\":";
  payload += persistentStorageOk ? "true" : "false";

  payload += ",\"persistent_save_count\":";
  payload += String(persistentSaveCount);

  payload += ",\"last_event\":\"";
  payload += lastEventType;
  payload += "\"";

  payload += ",\"event_sequence\":";
  payload += String(eventSequence);

  payload += ",\"lamp_blue_requested\":";
  payload += lampuBiruState == HIGH ? "true" : "false";

  payload += ",\"lamp_green_requested\":";
  payload += lampuHijauState == HIGH ? "true" : "false";

  payload += ",\"lamp_red_requested\":";
  payload += lampuMerahState == HIGH ? "true" : "false";

  payload += ",\"lamp_white_pwm_requested\":";
  payload += String(lampuPutihPWM);

  payload += ",\"lamp_blue_effective\":";
  payload += getAlarmLampLogicalState(lampuBiruState)
    ? "true"
    : "false";

  payload += ",\"lamp_green_effective\":";
  payload += getAlarmLampLogicalState(lampuHijauState)
    ? "true"
    : "false";

  payload += ",\"lamp_red_effective\":";
  payload += getAlarmLampLogicalState(lampuMerahState)
    ? "true"
    : "false";

  payload += ",\"lamp_white_pwm_effective\":";
  payload += String(getAlarmWhiteLampLogicalPWM());

  payload += "}";

  bool published = tbMqttClient.publish(
    TB_TELEMETRY_TOPIC,
    payload.c_str()
  );

  Serial.print("Publish ThingsBoard: ");
  Serial.println(published ? "BERHASIL" : "GAGAL");

  if (published) {
    Serial.println(payload);
  }

  return published;
}


// =====================================================
// THINGSBOARD RPC
// =====================================================
String jsonValueRaw(const String& json, const char* key) {
  String token = "\"";
  token += key;
  token += "\"";

  int keyIndex = json.indexOf(token);
  if (keyIndex < 0) return "";

  int colonIndex = json.indexOf(':', keyIndex + token.length());
  if (colonIndex < 0) return "";

  int start = colonIndex + 1;
  while (start < (int)json.length() &&
         (json.charAt(start) == ' ' ||
          json.charAt(start) == '\t' ||
          json.charAt(start) == '\r' ||
          json.charAt(start) == '\n')) {
    start++;
  }

  if (start >= (int)json.length()) return "";

  if (json.charAt(start) == '"') {
    int endQuote = json.indexOf('"', start + 1);
    if (endQuote < 0) return "";
    return json.substring(start + 1, endQuote);
  }

  int end = start;
  while (end < (int)json.length() &&
         json.charAt(end) != ',' &&
         json.charAt(end) != '}') {
    end++;
  }

  String result = json.substring(start, end);
  result.trim();
  return result;
}

bool parseRpcBool(const String& rawValue, bool& result) {
  String value = rawValue;
  value.trim();
  value.toLowerCase();

  if (value == "true" || value == "1" || value == "on") {
    result = true;
    return true;
  }

  if (value == "false" || value == "0" || value == "off") {
    result = false;
    return true;
  }

  return false;
}

bool parseRpcInt(const String& rawValue, int& result) {
  String value = rawValue;
  value.trim();

  if (value.length() == 0) return false;

  int start = 0;
  if (value.charAt(0) == '-') {
    if (value.length() == 1) return false;
    start = 1;
  }

  for (int i = start; i < (int)value.length(); i++) {
    if (!isDigit(value.charAt(i))) return false;
  }

  result = value.toInt();
  return true;
}

bool publishRpcResponse(const String& requestId, const String& responseJson) {
  if (!tbMqttClient.connected() || requestId.length() == 0) {
    return false;
  }

  String responseTopic = TB_RPC_RESPONSE_PREFIX;
  responseTopic += requestId;

  bool published = tbMqttClient.publish(
    responseTopic.c_str(),
    responseJson.c_str()
  );

  Serial.print("RPC response ");
  Serial.print(responseTopic);
  Serial.print(": ");
  Serial.println(published ? "BERHASIL" : "GAGAL");

  if (published) {
    Serial.println(responseJson);
  }

  return published;
}

String rpcBaseResponse(bool success, const String& method, const String& message) {
  String response;
  response.reserve(220);
  response += "{\"success\":";
  response += success ? "true" : "false";
  response += ",\"method\":\"";
  response += method;
  response += "\",\"message\":\"";
  response += message;
  response += "\"";
  return response;
}

void thingsBoardRpcCallback(char* topic, byte* payload, unsigned int length) {
  String topicText = String(topic);

  String requestBody;
  requestBody.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    requestBody += (char)payload[i];
  }

  int requestIdSeparator = topicText.lastIndexOf('/');
  String requestId = requestIdSeparator >= 0
    ? topicText.substring(requestIdSeparator + 1)
    : "";

  String method = jsonValueRaw(requestBody, "method");
  String params = jsonValueRaw(requestBody, "params");

  Serial.println();
  Serial.println("=== THINGSBOARD RPC DITERIMA ===");
  Serial.print("Topic  : ");
  Serial.println(topicText);
  Serial.print("Payload: ");
  Serial.println(requestBody);
  Serial.print("Method : ");
  Serial.println(method);
  Serial.print("Params : ");
  Serial.println(params);

  String response;
  bool commandProcessed = false;

  if (method == "setLampBlue") {
    bool value;
    if (parseRpcBool(params, value)) {
      setLampuBiru(value ? HIGH : LOW);
      enqueueEvent("RPC_LAMP_BLUE", value, "THINGSBOARD_RPC");

      response = rpcBaseResponse(true, method, "lamp_blue_updated");
      response += ",\"requested\":";
      response += value ? "true" : "false";
      response += ",\"effective\":";
      response += getAlarmLampLogicalState(lampuBiruState) ? "true" : "false";
      response += "}";
      commandProcessed = true;
    } else {
      response = rpcBaseResponse(false, method, "invalid_boolean_params");
      response += "}";
    }
  }
  else if (method == "setLampGreen") {
    bool value;
    if (parseRpcBool(params, value)) {
      setLampuHijau(value ? HIGH : LOW);
      enqueueEvent("RPC_LAMP_GREEN", value, "THINGSBOARD_RPC");

      response = rpcBaseResponse(true, method, "lamp_green_updated");
      response += ",\"requested\":";
      response += value ? "true" : "false";
      response += ",\"effective\":";
      response += getAlarmLampLogicalState(lampuHijauState) ? "true" : "false";
      response += "}";
      commandProcessed = true;
    } else {
      response = rpcBaseResponse(false, method, "invalid_boolean_params");
      response += "}";
    }
  }
  else if (method == "setLampRed") {
    bool value;
    if (parseRpcBool(params, value)) {
      setLampuMerah(value ? HIGH : LOW);
      enqueueEvent("RPC_LAMP_RED", value, "THINGSBOARD_RPC");

      response = rpcBaseResponse(true, method, "lamp_red_updated");
      response += ",\"requested\":";
      response += value ? "true" : "false";
      response += ",\"effective\":";
      response += getAlarmLampLogicalState(lampuMerahState) ? "true" : "false";
      response += "}";
      commandProcessed = true;
    } else {
      response = rpcBaseResponse(false, method, "invalid_boolean_params");
      response += "}";
    }
  }
  else if (method == "setWhitePwm") {
    int value;
    if (parseRpcInt(params, value) && value >= 0 && value <= 255) {
      setLampuPutihPWM(value);
      enqueueEvent("RPC_WHITE_PWM", value > 0, "THINGSBOARD_RPC");

      response = rpcBaseResponse(true, method, "white_pwm_updated");
      response += ",\"requested\":";
      response += String(value);
      response += ",\"effective\":";
      response += String(getAlarmWhiteLampLogicalPWM());
      response += "}";
      commandProcessed = true;
    } else {
      response = rpcBaseResponse(false, method, "params_must_be_0_to_255");
      response += "}";
    }
  }
  else if (method == "setFanPwm") {
    int value;
    if (parseRpcInt(params, value) && value >= 0 && value <= 255) {
      setKipasPWM(value);
      enqueueEvent("RPC_FAN_PWM", value > 0, "THINGSBOARD_RPC");

      response = rpcBaseResponse(true, method, "fan_pwm_updated");
      response += ",\"requested\":";
      response += String(kipasPWM);
      response += ",\"effective\":";
      response += String(getEffectiveFanPWM());
      response += "}";
      commandProcessed = true;
    } else {
      response = rpcBaseResponse(false, method, "params_must_be_0_to_255");
      response += "}";
    }
  }
  else if (method == "openDoor") {
    bool accepted = setDoorServo(true);

    enqueueEvent(
      accepted ? "RPC_DOOR_OPEN" : "RPC_DOOR_OPEN_REJECTED",
      accepted,
      "THINGSBOARD_RPC"
    );

    response = rpcBaseResponse(
      accepted,
      method,
      accepted ? "door_opened" : "door_open_rejected"
    );
    response += ",\"servo_angle\":";
    response += String(servoCommand);
    response += ",\"servo_door_open\":";
    response += servoDoorOpen ? "true" : "false";
    response += "}";
    commandProcessed = accepted;
  }
  else if (method == "closeDoor") {
    bool accepted = setDoorServo(false);

    enqueueEvent(
      accepted ? "RPC_DOOR_CLOSE" : "RPC_DOOR_CLOSE_REJECTED",
      accepted,
      "THINGSBOARD_RPC"
    );

    response = rpcBaseResponse(
      accepted,
      method,
      accepted ? "door_closed" : "rejected_fire_active"
    );
    response += ",\"fire_active\":";
    response += fireActive ? "true" : "false";
    response += ",\"servo_angle\":";
    response += String(servoCommand);
    response += ",\"servo_door_open\":";
    response += servoDoorOpen ? "true" : "false";
    response += "}";
    commandProcessed = accepted;
  }
  else if (method == "ringBell") {
    if (fireActive) {
      enqueueEvent("RPC_BELL_REJECTED", false, "THINGSBOARD_RPC");
      response = rpcBaseResponse(false, method, "rejected_fire_active");
      response += "}";
    } else {
      startBellPattern(3);
      enqueueEvent("RPC_BELL", true, "THINGSBOARD_RPC");
      response = rpcBaseResponse(true, method, "bell_started");
      response += "}";
      commandProcessed = true;
    }
  }
  else if (method == "muteAlarm") {
    bool accepted = muteFireAlarmFromSource("THINGSBOARD_RPC");

    response = rpcBaseResponse(
      accepted,
      method,
      accepted ? "alarm_muted" : "alarm_not_active"
    );
    response += ",\"fire_active\":";
    response += fireActive ? "true" : "false";
    response += ",\"fire_muted\":";
    response += fireMuted ? "true" : "false";
    response += ",\"fan_pwm_effective\":";
    response += String(getEffectiveFanPWM());
    response += "}";
    commandProcessed = accepted;
  }
  else if (method == "unmuteAlarm") {
    bool wasMuted = fireMuted;
    bool alarmWasActive = fireActive;

    unmuteFireAlarmFromSource("THINGSBOARD_RPC");

    bool accepted = alarmWasActive && wasMuted;
    response = rpcBaseResponse(
      accepted,
      method,
      accepted ? "alarm_unmuted" : "alarm_not_muted"
    );
    response += ",\"fire_active\":";
    response += fireActive ? "true" : "false";
    response += ",\"fire_muted\":";
    response += fireMuted ? "true" : "false";
    response += "}";
    commandProcessed = accepted;
  }
  else if (method == "getLampBlue") {
    response = lampuBiruState == HIGH ? "true" : "false";
    commandProcessed = true;
  }
  else if (method == "getLampGreen") {
    response = lampuHijauState == HIGH ? "true" : "false";
    commandProcessed = true;
  }
  else if (method == "getLampRed") {
    response = lampuMerahState == HIGH ? "true" : "false";
    commandProcessed = true;
  }
  else if (method == "getWhitePwm") {
    // Respons harus berupa angka agar dapat langsung dibaca Knob Control.
    response = String(lampuPutihPWM);
    commandProcessed = true;
  }
  else if (method == "getFanPwm") {
    // Mengembalikan nilai yang diminta pengguna, bukan PWM efektif saat kebakaran.
    response = String(kipasPWM);
    commandProcessed = true;
  }
  else if (method == "getStatus") {
    response = rpcBaseResponse(true, method, "status");
    response += ",\"temperature\":";
    response += isnan(temp) ? "null" : String(temp, 1);
    response += ",\"humidity\":";
    response += isnan(humi) ? "null" : String(humi, 1);
    response += ",\"gas_detected\":";
    response += isGasDetected() ? "true" : "false";
    response += ",\"door_open\":";
    response += isDoorOpen() ? "true" : "false";
    response += ",\"fire_active\":";
    response += fireActive ? "true" : "false";
    response += ",\"fire_muted\":";
    response += fireMuted ? "true" : "false";
    response += ",\"fan_pwm_requested\":";
    response += String(kipasPWM);
    response += ",\"fan_pwm_effective\":";
    response += String(getEffectiveFanPWM());
    response += ",\"servo_angle\":";
    response += String(servoCommand);
    response += ",\"servo_door_open\":";
    response += servoDoorOpen ? "true" : "false";
    response += ",\"buffer_count\":";
    response += String(eventCount);
    response += ",\"persistent_buffer_count\":";
    response += String(persistentStoredCount);
    response += ",\"persistent_buffer_restored\":";
    response += persistentBufferRestored ? "true" : "false";
    response += ",\"persistent_storage_ok\":";
    response += persistentStorageOk ? "true" : "false";
    response += "}";
    commandProcessed = true;
  }
  else {
    response = rpcBaseResponse(false, method, "unknown_method");
    response += "}";
  }

  publishRpcResponse(requestId, response);

  // Telemetry terbaru dipublikasikan pada iterasi loop berikutnya,
  // bukan langsung dari callback, agar callback MQTT tetap singkat.
  immediateTelemetryRequested = true;

  Serial.print("RPC diproses: ");
  Serial.println(commandProcessed ? "YA" : "TIDAK / DITOLAK");
  Serial.println("================================");
}

void publishImmediateTelemetryIfRequested() {
  if (!immediateTelemetryRequested) return;

  if (publishTelemetry()) {
    immediateTelemetryRequested = false;
    lastTelemetryMillis = millis();
  }
}

void publishPeriodicTelemetry() {
  if (millis() - lastTelemetryMillis < TELEMETRY_INTERVAL_MS) {
    return;
  }

  lastTelemetryMillis = millis();

  if (!publishTelemetry()) {
    Serial.println("Telemetry belum dikirim: ThingsBoard tidak terhubung.");
  }
}

// Saat WiFi pertama kali tersambung, kirim pesan siap.
// Pada koneksi berikutnya, kirim sinkronisasi status terkini.
void syncTelegramAfterReconnect() {
  bool nowConnected = (WiFi.status() == WL_CONNECTED);

  if (nowConnected && !previousWiFiConnected) {
    Serial.print("WiFi tersambung. IP: ");
    Serial.println(WiFi.localIP());

    // Koneksi pertama setelah boot tidak disebut reconnect.
    if (wifiStateInitialized) {
      enqueueEvent("WIFI_RECONNECTED", true, "SYSTEM");
    } else {
      wifiStateInitialized = true;
    }

    String msg;

    if (!initialOnlineSyncSent) {
      msg = "ESP32 Smart Home ready. Mode Telegram + ThingsBoard aktif. "
            "Ketik Help untuk command.\n\n";
      initialOnlineSyncSent = true;
    } else {
      msg = "WiFi ESP32 tersambung kembali.\n";
    }

    msg += getSensorIndicatorText();
    sendTelegram(msg);

    // Bila kebakaran masih aktif, kirim ulang notifikasi prioritas.
    if (fireActive) {
      fireNotifiedTelegram = sendTelegram(fireMessage());
      lastFireTelegramTryMillis = millis();
      fireTelegramTryCount = fireNotifiedTelegram ? 1 : 0;
    }
  }

  if (!nowConnected && previousWiFiConnected) {
    Serial.println("WiFi terputus. Alarm lokal tetap berjalan.");
    enqueueEvent("WIFI_DISCONNECTED", false, "SYSTEM");
  }

  previousWiFiConnected = nowConnected;
}

// =====================================================
// SETUP & LOOP
// =====================================================
void setup() {
  Serial.begin(115200);

  // Pulihkan antrean event offline sebelum SYSTEM_BOOT baru ditambahkan.
  initializePersistentEventStorage();

  pinMode(Magnet, INPUT_PULLUP);
  pinMode(MQ2_SENSOR, INPUT_PULLUP);
  pinMode(Lampu_biru, OUTPUT);
  pinMode(Lampu_putih, OUTPUT);
  pinMode(Lampu_merah, OUTPUT);
  pinMode(Lampu_hijau, OUTPUT);
  pinMode(Kipas_angin, OUTPUT);
  pinMode(Bell, OUTPUT);

  // Servo di-attach sekali pada setup agar loop tetap non-blocking.
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);

  if (SERVO_360_CONTINUOUS) {
    writeServoRaw(SERVO_360_STOP);
  } else {
    writeServoRaw(SERVO_CLOSED_ANGLE);
  }
  servoDoorOpen = false;

  applyNormalOutputs();

  lcd.begin(16, 2);
  lcd.init();
  lcd.backlight();
  lcdPrintLine(0, "= Smart Home =");
  lcdPrintLine(1, "Telegram + TB");
  delay(500);

  dht.begin();

  tbMqttClient.setServer(TB_HOST, TB_PORT);
  tbMqttClient.setBufferSize(2048);
  tbMqttClient.setKeepAlive(45);
  tbMqttClient.setSocketTimeout(5);
  tbMqttClient.setCallback(thingsBoardRpcCallback);

  bot.setChatID(CHAT_ID);
  bot.attach(newMsg);

  read_DHT22();
  showNormalLCD("Status:Aman");

  // Tetapkan baseline sensor agar kondisi awal tidak dianggap perubahan.
  previousGasForEvent = isGasDetected();
  previousDoorForEvent = isDoorOpen();
  eventSensorStateInitialized = true;

  // Event lama dari NVS tetap berada di depan antrean. SYSTEM_BOOT baru
  // ditambahkan paling belakang dan dikirim setelah MQTT tersedia.
  enqueueEvent("SYSTEM_BOOT", true, "SYSTEM");

  previousWiFiConnected = false;
  startWiFi();
}

void loop() {
  maintainWiFi();
  syncTelegramAfterReconnect();

  // Menyelesaikan gerakan servo continuous tanpa delay.
  updateServoTimedMove();

  updateFireAlarm();

  // Status gas dan pintu tetap dipantau saat fire alarm aktif.
  if (millis() - lastSensorCheckMillis >= FAST_SENSOR_CHECK_MS) {
    lastSensorCheckMillis = millis();
    checkSensorEvents();
    cek_MQ();
    cek_Magnet();
  }

  if (millis() - lastDhtMillis >= 2000) {
    lastDhtMillis = millis();
    read_DHT22();
  }

  if (!fireActive) {
    updateBellPattern();

    if (!fg && !fm && millis() - lastLCDMillis >= 1000) {
      lastLCDMillis = millis();
      showNormalLCD("Status:Aman");
    }
  }

  // Komunikasi eksternal dijalankan setelah proses keselamatan lokal.
  maintainThingsBoard();
  syncThingsBoardConnectionEvents();
  flushEventQueue();
  publishImmediateTelemetryIfRequested();
  publishPeriodicTelemetry();

  if (WiFi.status() == WL_CONNECTED &&
      telegramConfigured() &&
      millis() - lastTelegramTickMillis >= TELEGRAM_TICK_MS) {
    lastTelegramTickMillis = millis();
    bot.tick();
  }

  delay(2);
}
