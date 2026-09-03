// ============================================================================
// servo_yagi_tracker.ino
// Направленная антенна на поворотке (pan/tilt) на ESP32-C6, наведение по
// максимуму RSSI источника BLE или Wi-Fi 2.4 ГГц, веб-морда по SoftAP.
//
// Плата:      ESP32-C6-N4 (типовой ESP32-C6-DevKitC-1), Arduino-ESP32 core 3.x
// Антенна:    5-эл. Yagi 2.4 ГГц, thing:6827189 (ширина лепестка ~40° по азимуту)
// Библиотеки: NimBLE-Arduino >= 2.5.x, ArduinoJson >= 7.0, Preferences (в составе core)
//
// Идеология: один физический радиомодуль на Wi-Fi/BLE/802.15.4. 802.15.4 в
// v1 не трогаем вообще. SoftAP для веб-морды поднят всегда. BLE и Wi-Fi
// скан НИКОГДА не выполняются буквально в одном тике — только по очереди,
// см. serviceScanState(). Полный поканальный Wi-Fi скан 1..13 — только в
// парковке по явной команде, не на каждой клетке грубой сетки.
//
// Конечный автомат без «убийственных» delay(): единственные короткие
// delay()-подобные паузы — это settle серво (80-150 мс) и dwell-окна
// измерения, но и они реализованы через millis()-таймеры внутри
// serviceScanState(), вызываемого из loop() на каждой итерации, так что
// server.handleClient() дергается каждый проход loop(), а не раз в клетку.
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <strings.h>   // strcasecmp — не всегда затягивается транзитивно

#include "config.h"
#include "web_page.h"

// ДИАГНОСТИКА ОСЦИЛЛОГРАФОМ (временно): GPIO-маркеры на резервных пинах
// (см. PIN_STATUS_LED/PIN_CAL_BUTTON в config.h — те же физические пины,
// свободны в v1). Ch1 осциллографа -> PIN_TRACE_BLE, Ch2 -> PIN_TRACE_LEDC.
#define PIN_TRACE_BLE   PIN_CAL_BUTTON   // GPIO21 — HIGH на время onResult()
#define PIN_TRACE_LEDC  PIN_STATUS_LED   // GPIO20 — HIGH на время ledcWrite()

// ---------------------------------------------------------------------------
// Типы
// ---------------------------------------------------------------------------
enum RadioMode : uint8_t { MODE_BLE = 0, MODE_WIFI = 1, MODE_BOTH = 2 };
enum SysState  : uint8_t { ST_IDLE, ST_MANUAL, ST_WIFI_FULLSCAN, ST_COARSE, ST_FINE, ST_REFINE, ST_TRACK };
enum Metric    : uint8_t { METRIC_NONE, METRIC_BLE, METRIC_WIFI };
// Важно: BLE- и Wi-Fi-измерения в режиме "оба" идут ПОСЛЕДОВАТЕЛЬНО —
// сперва окно BLE, потом окно Wi-Fi, никогда не в одном слоте одновременно
// (см. ТЗ про сосуществование радио). Отсюда два раздельных DWELL-состояния.
enum CellPhase : uint8_t { PH_NONE, PH_SETTLE, PH_DWELL_BLE, PH_DWELL_WIFI, PH_COMMIT };
enum RunMode   : uint8_t { RUN_COARSE_ONLY, RUN_FULL };

struct Cal {
  int panMinUs    = SERVO_PULSE_MIN_US_DEFAULT;
  int panMaxUs    = SERVO_PULSE_MAX_US_DEFAULT;
  int tiltMinUs   = SERVO_PULSE_MIN_US_DEFAULT;
  int tiltMaxUs   = SERVO_PULSE_MAX_US_DEFAULT;
  int panAngleMin = PAN_ANGLE_MIN_DEFAULT;
  int panAngleMax = PAN_ANGLE_MAX_DEFAULT;
  int tiltAngleMin= TILT_ANGLE_MIN_DEFAULT;
  int tiltAngleMax= TILT_ANGLE_MAX_DEFAULT;
  int rssiFloor   = RSSI_FLOOR_DBM_DEFAULT;
};

struct BleSeen {
  char mac[18] = "";
  char name[32] = "";
  int  rssi = RSSI_NOT_MEASURED;
  unsigned long lastMs = 0;
  bool used = false;
};

struct WifiSeen {
  char ssid[33] = "";
  char bssid[18] = "";
  uint8_t channel = 0;
  int rssi = RSSI_NOT_MEASURED;
};

// ---------------------------------------------------------------------------
// Глобальное состояние
// ---------------------------------------------------------------------------
Cal cal;
Preferences prefs;
WebServer server(WEB_SERVER_PORT);

RadioMode curMode = MODE_BLE;  // безопасный старт: ровно одно радио, см. handleMode()
SysState  state   = ST_IDLE;
Metric    activeMetric = METRIC_NONE;
RunMode   runMode = RUN_COARSE_ONLY;

bool measureBleThisRun  = false;
bool measureWifiThisRun = false;

int curPanDeg  = PARK_PAN_DEFAULT;
int curTiltDeg = PARK_TILT_DEFAULT;

// Очередь джога (см. servoTask() и handleManual()): HTTP-хендлер только
// кладёт СЮДА последнюю запрошенную цель и сразу отвечает клиенту — реальная
// запись в LEDC происходит в отдельной задаче, вне потока HTTP-сервера и без
// какого-либо взаимодействия с BLE-сканом. Очередь на 1 элемент +
// xQueueOverwrite: нужна только самая свежая цель, старые неприменённые
// значения просто теряются, и это нормально для джойстика.
struct JogCmd { int16_t pan; int16_t tilt; };
QueueHandle_t qJog = nullptr;

// цели
struct { bool active=false; char mac[18]=""; char name[32]=""; } bleTarget;
struct { bool active=false; char bssid[18]=""; char ssid[33]=""; uint8_t channel=0; } wifiTarget;

// списки обнаруженных устройств
BleSeen  bleSeen[BLE_SEEN_LIST_MAX];
WifiSeen wifiSeenArr[WIFI_SEEN_LIST_MAX];
int wifiSeenCount = 0;

// карта RSSI по градусам (0..180 x 0..90), RSSI_NOT_MEASURED = не измерено
int8_t   mapBle [MAP_PAN_MAX_DEG][MAP_TILT_MAX_DEG];
int8_t   mapWifi[MAP_PAN_MAX_DEG][MAP_TILT_MAX_DEG];
uint16_t mapTimeSec[MAP_PAN_MAX_DEG][MAP_TILT_MAX_DEG]; // секунды с загрузки на момент записи клетки

// живые (сглаженные) значения RSSI текущей цели, для статуса и решений слежения
float bleEma  = NAN;
float wifiEma = NAN;
unsigned long bleLastSeenMs = 0, wifiLastSeenMs = 0;

// окно сэмплов BLE для текущей клетки сканирования (медианный фильтр)
int8_t bleWindow[RSSI_SAMPLE_MEDIAN_N];
int bleWindowCount = 0;

// однократный сэмпл Wi-Fi для текущей клетки
int  wifiCellSample  = RSSI_NOT_MEASURED;
bool wifiScanPending  = false;

// итератор растровой сетки (грубый/точный проход)
int  wpPanMin, wpPanMax, wpPanStep;
int  wpTiltMin, wpTiltMax, wpTiltStep;
int  wpPanCur, wpTiltCur;
bool wpDirForward, wpStarted, wpFinished;

// текущая клетка, которая обрабатывается конечным автоматом
int wpPan = 0, wpTilt = 0;
CellPhase phase = PH_NONE;
unsigned long phaseDeadline = 0;

// пик, найденный за проход (грубый/точный)
int bestPan = PARK_PAN_DEFAULT, bestTilt = PARK_TILT_DEFAULT, bestVal = RSSI_NOT_MEASURED;

// координатный спуск (точное донаведение / непрерывное слежение)
int refCenterPan, refCenterTilt, refCenterVal;
int refDir = 0, refIter = 0;
int refBestNeighborVal = RSSI_NOT_MEASURED, refBestNeighborPan = 0, refBestNeighborTilt = 0;
unsigned long trackNextProbeTime = 0;
static const int REFINE_NEI_DP[4] = { REFINE_STEP_DEG, -REFINE_STEP_DEG, 0, 0 };
static const int REFINE_NEI_DT[4] = { 0, 0, REFINE_STEP_DEG, -REFINE_STEP_DEG };

// полный поканальный Wi-Fi скан (в парковке)
bool wifiFullscanActive = false;

NimBLEScan* pBLEScan = nullptr;

// ---------------------------------------------------------------------------
// Прототипы (страхуемся от сюрпризов авто-генерации прототипов Arduino)
// ---------------------------------------------------------------------------
void loadCalibration();
void saveCalibration();
void writePulseUs(int pin, int us);
void setPan(int deg);
void setTilt(int deg);
void servoTask(void*);
int  clampPan(int v);
int  clampTilt(int v);
int  medianOf(const int8_t* arr, int n);
void updateEma(float& ema, int sample);
void initWaypoints(int panMin, int panMax, int panStep, int tiltMin, int tiltMax, int tiltStep);
bool wpNext(int& pan, int& tilt);
void beginCellPhaseMove();
void startDwell();
void startWifiSubScan();
void serviceDwellBle();
void serviceDwellWifi();
void commitCell();
void initRefine();
bool refineNext(int& pan, int& tilt, bool unlimited);
void serviceScanState();
void serviceWifiFullscan();
void serviceSerial();
void upsertBleSeen(const char* mac, const char* name, int rssi);
void buildWifiSeenFromResults(int n);
const char* stateName(SysState s);
const char* modeName(RadioMode m);

void handleRoot();
void handleStatus();
void handleBleList();
void handleWifiList();
void handleWifiFullscan();
void handleWifiFullscanStatus();
void handleSelect();
void handleMode();
void handleStart();
void handleStop();
void handlePark();
void handleManual();
void handleMapCsv();
void handleCalibrateGet();
void handleCalibrateSet();

// ---------------------------------------------------------------------------
// BLE: колбэк результатов скана
// ---------------------------------------------------------------------------
// ДИАГНОСТИКА ОСЦИЛЛОГРАФОМ: маркер HIGH на всё время выполнения тела
// колбэка — GPIO21, свободный/резервный пин по config.h.
class TrackerScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    digitalWrite(PIN_TRACE_BLE, HIGH);
    std::string addrStr = dev->getAddress().toString();
    if (!addrStr.empty()) {
      int rssi = dev->getRSSI();
      std::string nameStr = dev->getName();
      upsertBleSeen(addrStr.c_str(), nameStr.c_str(), rssi);

      if (bleTarget.active && strcasecmp(addrStr.c_str(), bleTarget.mac) == 0) {
        bleLastSeenMs = millis();
        updateEma(bleEma, rssi);
        if (bleWindowCount < RSSI_SAMPLE_MEDIAN_N) {
          bleWindow[bleWindowCount++] = (int8_t)rssi;
        } else {
          for (int i = 1; i < RSSI_SAMPLE_MEDIAN_N; i++) bleWindow[i - 1] = bleWindow[i];
          bleWindow[RSSI_SAMPLE_MEDIAN_N - 1] = (int8_t)rssi;
        }
      }
    }
    digitalWrite(PIN_TRACE_BLE, LOW);
  }
};
TrackerScanCallbacks scanCallbacks;

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(50);
  Serial.println();
  Serial.println("=== C6-Tracker boot ===");

  // ДИАГНОСТИКА ОСЦИЛЛОГРАФОМ (временно, см. PIN_TRACE_BLE/PIN_TRACE_LEDC).
  pinMode(PIN_TRACE_BLE, OUTPUT);
  pinMode(PIN_TRACE_LEDC, OUTPUT);
  digitalWrite(PIN_TRACE_BLE, LOW);
  digitalWrite(PIN_TRACE_LEDC, LOW);

  loadCalibration();

  memset(mapBle, RSSI_NOT_MEASURED, sizeof(mapBle));
  memset(mapWifi, RSSI_NOT_MEASURED, sizeof(mapWifi));
  memset(mapTimeSec, 0, sizeof(mapTimeSec));

  // Серво: LEDC, 50 Гц, 14 бит. Новый API Arduino-ESP32 core 3.x —
  // ledcAttach сам находит свободный канал, вручную канал указывать не надо.
  ledcAttach(PIN_SERVO_PAN, SERVO_PWM_FREQ_HZ, SERVO_PWM_RESOLUTION);
  ledcAttach(PIN_SERVO_TILT, SERVO_PWM_FREQ_HZ, SERVO_PWM_RESOLUTION);
  setPan(PARK_PAN_DEFAULT);
  setTilt(PARK_TILT_DEFAULT);

  // Очередь ручного джога + отдельная задача записи в серво — см. servoTask()
  // и комментарий у struct JogCmd выше про то, почему это вынесено из HTTP.
  qJog = xQueueCreate(1, sizeof(JogCmd));
  xTaskCreate(servoTask, "servoTask", 3072, nullptr, 1, nullptr);

  // Wi-Fi: AP всегда поднят (веб-морда), STA используется только для сканов,
  // к внешним сетям не подключаемся.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SOFTAP_SSID, SOFTAP_PASSWORD, SOFTAP_CHANNEL);
  // ДИАГНОСТИКА (гипотеза): по умолчанию STA-интерфейс (даже ни к чему не
  // подключённый, поднятый только ради сканов) держит modem sleep включённым.
  // Периодические переходы радио в сон/пробуждение — известный источник
  // гонок сосуществования Wi-Fi/BLE на ESP32. Пробуем убрать эту переменную
  // как возможную причину TG1 WDT сброса при LEDC-записи во время BLE-скана.
  WiFi.setSleep(false);
  Serial.print("SoftAP SSID: "); Serial.println(SOFTAP_SSID);
  Serial.print("SoftAP IP:   "); Serial.println(WiFi.softAPIP());

  // BLE: NimBLE, фоновый непрерывный пассивный (по умолчанию) скан.
  NimBLEDevice::init("C6Tracker");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(&scanCallbacks);
  pBLEScan->setActiveScan(BLE_ACTIVE_SCAN_DEFAULT);
  pBLEScan->setInterval(BLE_SCAN_INTERVAL_MS);
  pBLEScan->setWindow(BLE_SCAN_WINDOW_MS);
  pBLEScan->setMaxResults(0);   // не копим внутренний кэш NimBLE — экономим RAM, читаем только через колбэк
  pBLEScan->start(0, false);    // 0 = бессрочно, неблокирующий вызов

  // Веб-сервер
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/ble/list", handleBleList);
  server.on("/api/wifi/list", handleWifiList);
  server.on("/api/wifi/fullscan", handleWifiFullscan);
  server.on("/api/wifi/fullscan/status", handleWifiFullscanStatus);
  server.on("/api/select", handleSelect);
  server.on("/api/mode", handleMode);
  server.on("/api/start", handleStart);
  server.on("/api/stop", handleStop);
  server.on("/api/park", handlePark);
  server.on("/api/manual", handleManual);
  server.on("/api/map.csv", handleMapCsv);
  server.on("/api/calibrate", handleCalibrateGet);       // чтение текущей калибровки
  server.on("/api/calibrate/set", handleCalibrateSet);   // запись (GET с query-параметрами, см. web_page.h)
  server.begin();
  Serial.println("HTTP server started");
  Serial.println("Serial-команды: PARK / STOP / SAVE / STATUS / CAL <FIELD> <VALUE>");
}

void loop() {
  server.handleClient();
  serviceWifiFullscan();
  serviceSerial();

  switch (state) {
    case ST_COARSE:
    case ST_FINE:
    case ST_REFINE:
    case ST_TRACK:
      serviceScanState();
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Серво
// ---------------------------------------------------------------------------
// ИСТОРИЯ РАССЛЕДОВАНИЯ (см. README, раздел "Развитие", там подробно):
// изначально казалось, что дело в ledcWrite() при резкой смене duty ОДНО-
// ВРЕМЕННО с активным фоновым BLE-сканом (Guru Meditation / Task Watchdog).
// ПРОВЕРЕНО и ОТБРОШЕНО: критическая секция (portENTER_CRITICAL/
// portEXIT_CRITICAL) вокруг ledcWrite() не крашит, а вешает плату НАВСЕГДА —
// ledcWrite(), похоже, сама ждёт что-то через прерывание, и с выключенными
// прерываниями это ожидание никогда не завершается. НЕ трогай это
// направление без осциллографа/JTAG-отладки.
// РЕАЛЬНАЯ ПРИЧИНА (найдена через JTAG + addr2line по .elf): крash уходит
// не в LEDC, а в lwIP (sys_arch.c: sys_mbox_trypost / sys_mutex_unlock) —
// то есть в сам сетевой стек, когда HTTP-поток держится занятым (busy-wait
// на остановку BLE-скана + delay()) во время активного BLE-скана поверх
// SoftAP. Официальная документация Espressif (coexist.html, esp32c6) прямо
// маркирует комбинацию Wi-Fi SoftAP + BLE Scan как "C1: supported but the
// performance is unstable" — то есть это не наш баг, а официально
// нестабильная комбинация радио на этом чипе. Лечится не борьбой с LEDC,
// а тем, чтобы не держать HTTP-обработчик занятым: см. servoTask()/qJog —
// вся реальная работа с серво вынесена в отдельную задачу, HTTP-хендлер
// handleManual() только кладёт цель в очередь и сразу отвечает.
// ДИАГНОСТИКА ОСЦИЛЛОГРАФОМ (оставлено для справки): маркер HIGH ровно на
// время самого вызова ledcWrite() — GPIO20, см. PIN_TRACE_LEDC/PIN_TRACE_BLE.
void writePulseUs(int pin, int us) {
  if (us < 1) us = 1;
  uint32_t duty = (uint32_t)(((uint64_t)us * SERVO_PWM_MAX_DUTY) / SERVO_PERIOD_US);
  if (duty > SERVO_PWM_MAX_DUTY) duty = SERVO_PWM_MAX_DUTY;
  digitalWrite(PIN_TRACE_LEDC, HIGH);
  ledcWrite(pin, duty);
  digitalWrite(PIN_TRACE_LEDC, LOW);
}

int clampPan(int v)  { return constrain(v, cal.panAngleMin, cal.panAngleMax); }
int clampTilt(int v) { return constrain(v, cal.tiltAngleMin, cal.tiltAngleMax); }

void setPan(int deg) {
  deg = clampPan(deg);
  long us = map((long)deg, (long)cal.panAngleMin, (long)cal.panAngleMax, (long)cal.panMinUs, (long)cal.panMaxUs);
  writePulseUs(PIN_SERVO_PAN, (int)us);
  curPanDeg = deg;
}

void setTilt(int deg) {
  deg = clampTilt(deg);
  long us = map((long)deg, (long)cal.tiltAngleMin, (long)cal.tiltAngleMax, (long)cal.tiltMinUs, (long)cal.tiltMaxUs);
  writePulseUs(PIN_SERVO_TILT, (int)us);
  curTiltDeg = deg;
}

// Задача ручного джога (см. JogCmd/qJog выше и handleManual() ниже).
// ПОЧЕМУ ОТДЕЛЬНАЯ ЗАДАЧА: расследование с JTAG (см. README, раздел
// "Развитие") показало, что крash при ручном джоге происходит не в этом
// коде, а внутри lwIP (sys_arch.c: sys_mbox_trypost / sys_mutex_unlock) —
// то есть виновата не сама запись в LEDC, а то, что HTTP-обработчик держал
// поток занятым (stop() скана + busy-wait + delay()) внутри контекста
// сетевого стека. Официальная документация Espressif прямо помечает
// SoftAP+BLE Scan на ESP32-C6 как "C1: supported but the performance is
// unstable" — то есть сама эта комбинация радио изначально нестабильна под
// нагрузкой, и лечится это не борьбой с LEDC, а тем, чтобы НЕ держать
// HTTP-поток занятым во время неё. Поэтому: handleManual() теперь только
// кладёт цель в очередь и сразу отвечает, а реальная запись в серво (и
// только она) происходит здесь, в отдельной задаче, вообще не трогающей
// BLE-скан.
void servoTask(void*) {
  JogCmd cmd;
  int lastPan = INT16_MIN, lastTilt = INT16_MIN;
  for (;;) {
    if (xQueueReceive(qJog, &cmd, pdMS_TO_TICKS(50)) == pdTRUE && state == ST_MANUAL) {
      if (cmd.pan != lastPan)   { setPan(cmd.pan);   lastPan  = cmd.pan; }
      if (cmd.tilt != lastTilt) { setTilt(cmd.tilt); lastTilt = cmd.tilt; }
    }
  }
}

// ---------------------------------------------------------------------------
// Фильтрация RSSI
// ---------------------------------------------------------------------------
int medianOf(const int8_t* arr, int n) {
  if (n <= 0) return RSSI_NOT_MEASURED;
  int8_t tmp[RSSI_SAMPLE_MEDIAN_N];
  for (int i = 0; i < n; i++) tmp[i] = arr[i];
  for (int i = 1; i < n; i++) {
    int8_t k = tmp[i]; int j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = k;
  }
  return tmp[n / 2];
}

void updateEma(float& ema, int sample) {
  if (isnan(ema)) ema = sample;
  else ema += (RSSI_EMA_ALPHA_PERCENT / 100.0f) * (sample - ema);
}

// ---------------------------------------------------------------------------
// Итератор растровой сетки (грубый/точный проход, «змейка»)
// ---------------------------------------------------------------------------
void initWaypoints(int panMin, int panMax, int panStep, int tiltMin, int tiltMax, int tiltStep) {
  wpPanMin = panMin; wpPanMax = panMax; wpPanStep = max(1, panStep);
  wpTiltMin = tiltMin; wpTiltMax = tiltMax; wpTiltStep = max(1, tiltStep);
  wpPanCur = panMin; wpTiltCur = tiltMin;
  wpDirForward = true; wpStarted = false; wpFinished = false;
}

static void wpAdvanceRow() {
  if (wpTiltCur >= wpTiltMax) { wpFinished = true; return; }
  wpTiltCur += wpTiltStep;
  if (wpTiltCur > wpTiltMax) wpTiltCur = wpTiltMax;
  wpDirForward = !wpDirForward;
}

bool wpNext(int& pan, int& tilt) {
  if (wpFinished) return false;
  if (!wpStarted) { wpStarted = true; pan = wpPanCur; tilt = wpTiltCur; return true; }

  if (wpDirForward) {
    if (wpPanCur >= wpPanMax) { wpAdvanceRow(); if (wpFinished) return false; pan = wpPanCur; tilt = wpTiltCur; return true; }
    wpPanCur += wpPanStep;
    if (wpPanCur > wpPanMax) wpPanCur = wpPanMax;
  } else {
    if (wpPanCur <= wpPanMin) { wpAdvanceRow(); if (wpFinished) return false; pan = wpPanCur; tilt = wpTiltCur; return true; }
    wpPanCur -= wpPanStep;
    if (wpPanCur < wpPanMin) wpPanCur = wpPanMin;
  }
  pan = wpPanCur; tilt = wpTiltCur;
  return true;
}

// ---------------------------------------------------------------------------
// Конечный автомат одной клетки: MOVE -> SETTLE -> DWELL -> COMMIT
// ---------------------------------------------------------------------------
void beginCellPhaseMove() {
  setPan(wpPan);
  setTilt(wpTilt);
  phase = PH_SETTLE;
  phaseDeadline = millis() + SERVO_SETTLE_MS_DEFAULT;
}

// Начало измерения в клетке. ПОСЛЕДОВАТЕЛЬНО: сперва BLE-окно (если нужно),
// потом, отдельным шагом, Wi-Fi-скан (если нужен). Никогда не запускаем
// Wi-Fi-скан, пока не закрыто BLE-окно — так радио реально не делят один
// слот на двоих, это честная сериализация, а не просто "авось коэксистенс
// разрулит".
void startDwell() {
  bleWindowCount = 0;
  wifiCellSample = RSSI_NOT_MEASURED;
  wifiScanPending = false;

  if (measureBleThisRun) {
    bool fineGrained = (state == ST_FINE || state == ST_REFINE || state == ST_TRACK);
    phase = PH_DWELL_BLE;
    phaseDeadline = millis() + (fineGrained ? BLE_DWELL_MS_FINE : BLE_DWELL_MS_COARSE);
  } else if (measureWifiThisRun && wifiTarget.active) {
    startWifiSubScan();
  } else {
    phase = PH_COMMIT; // ни один радиомодуль не активен для этой цели
  }
}

void startWifiSubScan() {
  WiFi.scanNetworks(true, true, false, WIFI_CHAN_SCAN_MS, wifiTarget.channel);
  wifiScanPending = true;
  phase = PH_DWELL_WIFI;
  phaseDeadline = millis() + WIFI_CHAN_SCAN_MS + 60;
}

void serviceDwellBle() {
  // Окно BLE — просто ждём millis(), сэмплы приходят асинхронно из
  // фонового NimBLE-скана через колбэк onResult() в bleWindow[].
  if (millis() < phaseDeadline) return;
  if (measureWifiThisRun && wifiTarget.active) startWifiSubScan();
  else phase = PH_COMMIT;
}

void serviceDwellWifi() {
  if (wifiScanPending) {
    int16_t n = WiFi.scanComplete();
    if (n >= 0) {
      wifiCellSample = RSSI_NOT_MEASURED;
      for (int i = 0; i < n; i++) {
        if (strcasecmp(WiFi.BSSIDstr(i).c_str(), wifiTarget.bssid) == 0) {
          wifiCellSample = WiFi.RSSI(i);
          break;
        }
      }
      WiFi.scanDelete();
      wifiScanPending = false;
      if (wifiCellSample > RSSI_NOT_MEASURED) { updateEma(wifiEma, wifiCellSample); wifiLastSeenMs = millis(); }
    } else if (n == WIFI_SCAN_FAILED) {
      wifiScanPending = false;
    }
  }
  if (millis() >= phaseDeadline && !wifiScanPending) phase = PH_COMMIT;
}

void commitCell() {
  int bleVal  = measureBleThisRun  ? medianOf(bleWindow, bleWindowCount) : RSSI_NOT_MEASURED;
  int wifiVal = measureWifiThisRun ? wifiCellSample : RSSI_NOT_MEASURED;

  if (wpPan >= 0 && wpPan < MAP_PAN_MAX_DEG && wpTilt >= 0 && wpTilt < MAP_TILT_MAX_DEG) {
    if (measureBleThisRun)  mapBle[wpPan][wpTilt]  = (int8_t)bleVal;
    if (measureWifiThisRun) mapWifi[wpPan][wpTilt] = (int8_t)wifiVal;
    mapTimeSec[wpPan][wpTilt] = (uint16_t)(millis() / 1000UL);
  }

  Serial.printf("CSV,%d,%d,%d,%d,%lu\n", wpPan, wpTilt, bleVal, wifiVal, millis());

  int metricVal = (activeMetric == METRIC_BLE) ? bleVal : (activeMetric == METRIC_WIFI) ? wifiVal : RSSI_NOT_MEASURED;
  // Порог отсечки: сигнал слабее cal.rssiFloor не считается кандидатом на
  // пик (в карту он всё равно попал выше, для визуализации).
  if (metricVal <= cal.rssiFloor) metricVal = RSSI_NOT_MEASURED;
  if (metricVal > RSSI_NOT_MEASURED) {
    if (state == ST_REFINE || state == ST_TRACK) {
      if (metricVal > refBestNeighborVal) { refBestNeighborVal = metricVal; refBestNeighborPan = wpPan; refBestNeighborTilt = wpTilt; }
    } else {
      if (metricVal > bestVal) { bestVal = metricVal; bestPan = wpPan; bestTilt = wpTilt; }
    }
  }
  phase = PH_NONE;
}

// ---------------------------------------------------------------------------
// Точное донаведение / непрерывное слежение — координатный спуск ±1°
// ---------------------------------------------------------------------------
void initRefine() {
  refCenterPan = bestPan; refCenterTilt = bestTilt; refCenterVal = bestVal;
  refDir = 0; refIter = 0; refBestNeighborVal = RSSI_NOT_MEASURED;
}

bool refineNext(int& pan, int& tilt, bool unlimited) {
  if (!unlimited && refIter >= MAX_REFINE_ITERS) return false;

  if (refDir > 3) {
    bool improved = (refBestNeighborVal > RSSI_NOT_MEASURED) && (refBestNeighborVal > refCenterVal + RSSI_DEADBAND_DB);
    if (improved) { refCenterPan = refBestNeighborPan; refCenterTilt = refBestNeighborTilt; refCenterVal = refBestNeighborVal; }
    refDir = 0; refBestNeighborVal = RSSI_NOT_MEASURED; refIter++;

    if (unlimited) { trackNextProbeTime = millis() + TRACK_PERIOD_MS; return false; }
    if (!improved) return false; // локальный пик найден с точностью до REFINE_STEP_DEG
  }

  pan  = clampPan(refCenterPan  + REFINE_NEI_DP[refDir]);
  tilt = clampTilt(refCenterTilt + REFINE_NEI_DT[refDir]);
  refDir++;
  return true;
}

// ---------------------------------------------------------------------------
// Главный диспетчер сканирующих состояний — вызывается КАЖДЫЙ проход loop()
// ---------------------------------------------------------------------------
void serviceScanState() {
  if (phase == PH_SETTLE)     { if (millis() >= phaseDeadline) startDwell(); return; }
  if (phase == PH_DWELL_BLE)  { serviceDwellBle(); return; }
  if (phase == PH_DWELL_WIFI) { serviceDwellWifi(); return; }
  if (phase == PH_COMMIT)     { commitCell(); return; }

  // phase == PH_NONE: клетка завершена (или первый вход) — берём следующую точку
  int p, t;
  switch (state) {
    case ST_COARSE:
      if (wpNext(p, t)) { wpPan = p; wpTilt = t; beginCellPhaseMove(); return; }
      // грубый растр пройден целиком
      if (runMode == RUN_COARSE_ONLY) { state = ST_IDLE; return; }
      if (bestVal <= RSSI_NOT_MEASURED) {
        Serial.println("WARN,target_not_found_on_coarse_grid");
        state = ST_IDLE; return;
      }
      initWaypoints(clampPan(bestPan - FINE_WINDOW_DEG), clampPan(bestPan + FINE_WINDOW_DEG), FINE_STEP_DEG_DEFAULT,
                    clampTilt(bestTilt - FINE_WINDOW_DEG), clampTilt(bestTilt + FINE_WINDOW_DEG), FINE_STEP_DEG_DEFAULT);
      { int cp = bestPan, ct = bestTilt; bestVal = RSSI_NOT_MEASURED; bestPan = cp; bestTilt = ct; }
      state = ST_FINE;
      return;

    case ST_FINE:
      if (wpNext(p, t)) { wpPan = p; wpTilt = t; beginCellPhaseMove(); return; }
      if (bestVal <= RSSI_NOT_MEASURED) { state = ST_IDLE; return; }
      initRefine();
      state = ST_REFINE;
      return;

    case ST_REFINE:
      if (refineNext(p, t, false)) { wpPan = p; wpTilt = t; beginCellPhaseMove(); return; }
      // сошлось — переходим в непрерывное слежение вокруг найденного пика
      state = ST_TRACK;
      refIter = 0; refDir = 0; refBestNeighborVal = RSSI_NOT_MEASURED;
      trackNextProbeTime = millis();
      return;

    case ST_TRACK:
      if (millis() < trackNextProbeTime) return; // ждём следующего цикла опроса, никого не дёргаем
      if (refineNext(p, t, true)) { wpPan = p; wpTilt = t; beginCellPhaseMove(); return; }
      return; // refineNext сам переставил trackNextProbeTime

    default:
      return;
  }
}

// ---------------------------------------------------------------------------
// Полный поканальный Wi-Fi скан (1..13), только в парковке, по явной команде
// ---------------------------------------------------------------------------
void serviceWifiFullscan() {
  if (!wifiFullscanActive) return;
  int16_t n = WiFi.scanComplete();
  if (n >= 0) {
    buildWifiSeenFromResults(n);
    WiFi.scanDelete();
    wifiFullscanActive = false;
    state = ST_IDLE;
  } else if (n == WIFI_SCAN_FAILED) {
    wifiFullscanActive = false;
    state = ST_IDLE;
  }
}

void buildWifiSeenFromResults(int n) {
  wifiSeenCount = 0;
  for (int i = 0; i < n && wifiSeenCount < WIFI_SEEN_LIST_MAX; i++) {
    WifiSeen& e = wifiSeenArr[wifiSeenCount];
    strncpy(e.ssid, WiFi.SSID(i).c_str(), sizeof(e.ssid) - 1); e.ssid[sizeof(e.ssid) - 1] = 0;
    strncpy(e.bssid, WiFi.BSSIDstr(i).c_str(), sizeof(e.bssid) - 1); e.bssid[sizeof(e.bssid) - 1] = 0;
    e.channel = (uint8_t)WiFi.channel(i);
    e.rssi = WiFi.RSSI(i);
    wifiSeenCount++;
  }
}

// ---------------------------------------------------------------------------
// BLE: список обнаруженных устройств
// ---------------------------------------------------------------------------
void upsertBleSeen(const char* mac, const char* name, int rssi) {
  int idx = -1;
  for (int i = 0; i < BLE_SEEN_LIST_MAX; i++) {
    if (bleSeen[i].used && strcasecmp(bleSeen[i].mac, mac) == 0) { idx = i; break; }
  }
  if (idx < 0) {
    unsigned long oldest = 0xFFFFFFFFUL; idx = 0;
    for (int i = 0; i < BLE_SEEN_LIST_MAX; i++) {
      if (!bleSeen[i].used) { idx = i; break; }
      if (bleSeen[i].lastMs < oldest) { oldest = bleSeen[i].lastMs; idx = i; }
    }
    strncpy(bleSeen[idx].mac, mac, sizeof(bleSeen[idx].mac) - 1); bleSeen[idx].mac[sizeof(bleSeen[idx].mac) - 1] = 0;
    bleSeen[idx].name[0] = 0;
    bleSeen[idx].used = true;
  }
  if (name && name[0]) {
    strncpy(bleSeen[idx].name, name, sizeof(bleSeen[idx].name) - 1);
    bleSeen[idx].name[sizeof(bleSeen[idx].name) - 1] = 0;
  }
  bleSeen[idx].rssi = rssi;
  bleSeen[idx].lastMs = millis();
}

// ---------------------------------------------------------------------------
// NVS калибровка
// ---------------------------------------------------------------------------
void loadCalibration() {
  prefs.begin(NVS_NAMESPACE, true);
  cal.panMinUs     = prefs.getInt("panMinUs", SERVO_PULSE_MIN_US_DEFAULT);
  cal.panMaxUs     = prefs.getInt("panMaxUs", SERVO_PULSE_MAX_US_DEFAULT);
  cal.tiltMinUs    = prefs.getInt("tiltMinUs", SERVO_PULSE_MIN_US_DEFAULT);
  cal.tiltMaxUs    = prefs.getInt("tiltMaxUs", SERVO_PULSE_MAX_US_DEFAULT);
  cal.panAngleMin  = prefs.getInt("panAngMin", PAN_ANGLE_MIN_DEFAULT);
  cal.panAngleMax  = prefs.getInt("panAngMax", PAN_ANGLE_MAX_DEFAULT);
  cal.tiltAngleMin = prefs.getInt("tiltAngMin", TILT_ANGLE_MIN_DEFAULT);
  cal.tiltAngleMax = prefs.getInt("tiltAngMax", TILT_ANGLE_MAX_DEFAULT);
  cal.rssiFloor    = prefs.getInt("rssiFloor", RSSI_FLOOR_DBM_DEFAULT);
  prefs.end();
}

void saveCalibration() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt("panMinUs", cal.panMinUs);
  prefs.putInt("panMaxUs", cal.panMaxUs);
  prefs.putInt("tiltMinUs", cal.tiltMinUs);
  prefs.putInt("tiltMaxUs", cal.tiltMaxUs);
  prefs.putInt("panAngMin", cal.panAngleMin);
  prefs.putInt("panAngMax", cal.panAngleMax);
  prefs.putInt("tiltAngMin", cal.tiltAngleMin);
  prefs.putInt("tiltAngMax", cal.tiltAngleMax);
  prefs.putInt("rssiFloor", cal.rssiFloor);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Имена состояний/режимов для JSON
// ---------------------------------------------------------------------------
const char* stateName(SysState s) {
  switch (s) {
    case ST_IDLE: return "idle";
    case ST_MANUAL: return "manual";
    case ST_WIFI_FULLSCAN: return "wifi_fullscan";
    case ST_COARSE: return "coarse_scan";
    case ST_FINE: return "fine_scan";
    case ST_REFINE: return "refine";
    case ST_TRACK: return "tracking";
  }
  return "?";
}
const char* modeName(RadioMode m) {
  switch (m) { case MODE_BLE: return "ble"; case MODE_WIFI: return "wifi"; case MODE_BOTH: return "both"; }
  return "?";
}

// ---------------------------------------------------------------------------
// HTTP-хендлеры
// ---------------------------------------------------------------------------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  JsonDocument doc;
  doc["state"] = stateName(state);
  doc["mode"]  = modeName(curMode);
  doc["pan"]   = curPanDeg;
  doc["tilt"]  = curTiltDeg;
  doc["rssi_ble"]  = isnan(bleEma)  ? RSSI_NOT_MEASURED : (int)roundf(bleEma);
  doc["rssi_wifi"] = isnan(wifiEma) ? RSSI_NOT_MEASURED : (int)roundf(wifiEma);
  doc["target_ble_mac"]  = bleTarget.active ? bleTarget.mac : "";
  doc["target_ble_name"] = bleTarget.name;
  doc["target_wifi_bssid"] = wifiTarget.active ? wifiTarget.bssid : "";
  doc["target_wifi_ssid"]  = wifiTarget.ssid;
  doc["target_wifi_ch"]    = wifiTarget.channel;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleBleList() {
  JsonDocument doc; JsonArray arr = doc.to<JsonArray>();
  unsigned long now = millis();
  for (int i = 0; i < BLE_SEEN_LIST_MAX; i++) {
    if (!bleSeen[i].used) continue;
    if (now - bleSeen[i].lastMs > BLE_SEEN_TIMEOUT_MS) continue;
    JsonObject o = arr.add<JsonObject>();
    o["mac"] = bleSeen[i].mac; o["name"] = bleSeen[i].name; o["rssi"] = bleSeen[i].rssi;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleWifiList() {
  JsonDocument doc; JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < wifiSeenCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = wifiSeenArr[i].ssid; o["bssid"] = wifiSeenArr[i].bssid;
    o["channel"] = wifiSeenArr[i].channel; o["rssi"] = wifiSeenArr[i].rssi;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleWifiFullscan() {
  if (state == ST_COARSE || state == ST_FINE || state == ST_REFINE || state == ST_TRACK || wifiFullscanActive) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  setPan(clampPan(PARK_PAN_DEFAULT));
  setTilt(clampTilt(PARK_TILT_DEFAULT));
  WiFi.scanNetworks(true, true, false, WIFI_FULLSCAN_MS_PER_CHAN, 0);
  wifiFullscanActive = true;
  state = ST_WIFI_FULLSCAN;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleWifiFullscanStatus() {
  JsonDocument doc; doc["busy"] = wifiFullscanActive;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSelect() {
  String type = server.arg("type");
  if (type == "ble") {
    String mac = server.arg("mac"); String name = server.arg("name");
    strncpy(bleTarget.mac, mac.c_str(), sizeof(bleTarget.mac) - 1); bleTarget.mac[sizeof(bleTarget.mac) - 1] = 0;
    strncpy(bleTarget.name, name.c_str(), sizeof(bleTarget.name) - 1); bleTarget.name[sizeof(bleTarget.name) - 1] = 0;
    bleTarget.active = true;
    bleEma = NAN; bleWindowCount = 0; bleLastSeenMs = 0;
    server.send(200, "application/json", "{\"ok\":true}");
  } else if (type == "wifi") {
    String bssid = server.arg("bssid"); String ssid = server.arg("ssid"); int ch = server.arg("channel").toInt();
    strncpy(wifiTarget.bssid, bssid.c_str(), sizeof(wifiTarget.bssid) - 1); wifiTarget.bssid[sizeof(wifiTarget.bssid) - 1] = 0;
    strncpy(wifiTarget.ssid, ssid.c_str(), sizeof(wifiTarget.ssid) - 1); wifiTarget.ssid[sizeof(wifiTarget.ssid) - 1] = 0;
    wifiTarget.channel = (uint8_t)constrain(ch, 1, 13);
    wifiTarget.active = true;
    wifiEma = NAN; wifiLastSeenMs = 0;
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad type\"}");
  }
}

void handleMode() {
  // ВАЖНО: режим "оба сразу" (MODE_BOTH) намеренно убран из принимаемых
  // значений — см. README, раздел про Wi-Fi/BLE coexistence. Одновременная
  // работа BLE-скана и Wi-Fi/HTTP-активности на этом чипе — самый рискованный
  // режим из всех, что мы гоняли (см. полное расследование в README). Раньше
  // "иначе -> MODE_BOTH" было скрытой ловушкой: любая опечатка в параметре
  // molча включала самый нестабильный режим. Теперь допустимы только "ble"
  // и "wifi" явно, всё остальное — ошибка, ничего не меняем втихую.
  String m = server.arg("mode");
  if (m == "ble") {
    curMode = MODE_BLE;
  } else if (m == "wifi") {
    curMode = MODE_WIFI;
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"mode must be 'ble' or 'wifi'\"}");
    return;
  }

  // Явная политика сосуществования: если Wi-Fi-only — фоновый BLE скан
  // останавливаем совсем, не делаем вид, что он "бесплатно" крутится рядом.
  if (curMode == MODE_WIFI) {
    if (pBLEScan->isScanning()) pBLEScan->stop();
  } else {
    if (!pBLEScan->isScanning()) pBLEScan->start(0, false);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStart() {
  if (wifiFullscanActive) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"wifi fullscan busy\"}");
    return;
  }
  String what = server.arg("what");

  if (what == "coarse") {
    runMode = RUN_COARSE_ONLY;
    activeMetric = bleTarget.active ? METRIC_BLE : (wifiTarget.active ? METRIC_WIFI : METRIC_NONE);
  } else if (what == "track_ble") {
    if (!bleTarget.active) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"no ble target\"}"); return; }
    runMode = RUN_FULL; activeMetric = METRIC_BLE;
  } else if (what == "track_wifi") {
    if (!wifiTarget.active) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"no wifi target\"}"); return; }
    runMode = RUN_FULL; activeMetric = METRIC_WIFI;
    // "track_both" намеренно убран — см. handleMode() и README про
    // разделение BLE/Wi-Fi: одновременная работа обоих радио — самый
    // рискованный режим на этом чипе, выбирай ровно одно.
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad what\"}");
    return;
  }

  measureBleThisRun  = bleTarget.active  && (curMode != MODE_WIFI || activeMetric == METRIC_BLE);
  measureWifiThisRun = wifiTarget.active && (curMode != MODE_BLE  || activeMetric == METRIC_WIFI);

  // Если для наведения нужен BLE, а фоновый скан был выключен (режим был
  // переключён на "только Wi-Fi") — включаем его обратно. Без этого цель
  // по BLE физически никогда не будет услышана.
  if (measureBleThisRun && pBLEScan && !pBLEScan->isScanning()) pBLEScan->start(0, false);

  bestVal = RSSI_NOT_MEASURED; bestPan = curPanDeg; bestTilt = curTiltDeg;
  initWaypoints(cal.panAngleMin, cal.panAngleMax, COARSE_STEP_DEG_DEFAULT,
                cal.tiltAngleMin, cal.tiltAngleMax, COARSE_STEP_DEG_DEFAULT);
  phase = PH_NONE;
  state = ST_COARSE;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStop() {
  state = ST_IDLE; phase = PH_NONE;
  if (wifiScanPending) { WiFi.scanDelete(); wifiScanPending = false; }
  // Возвращаем фоновый BLE-скан, выключенный на весь ручной режим при
  // входе в него (см. handleManual()).
  if (curMode != MODE_WIFI && pBLEScan && !pBLEScan->isScanning()) pBLEScan->start(0, false);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handlePark() {
  state = ST_IDLE; phase = PH_NONE;
  setPan(PARK_PAN_DEFAULT);
  setTilt(PARK_TILT_DEFAULT);
  // См. комментарий в handleStop().
  if (curMode != MODE_WIFI && pBLEScan && !pBLEScan->isScanning()) pBLEScan->start(0, false);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleManual() {
  int pan = server.arg("pan").toInt();
  int tilt = server.arg("tilt").toInt();

  // См. подробный комментарий у servoTask() про то, почему тут больше нет
  // ни busy-wait/delay() вокруг BLE-скана на каждый запрос, ни прямого
  // вызова setPan/setTilt: HTTP-обработчик не должен ничего блокировать —
  // только (опционально) попросить скан остановиться и сразу же положить
  // цель в очередь. Экспериментально подтверждено (см. README): даже
  // полностью без единого обращения к BLE-скану в этом хендлере плата
  // всё равно падает почти сразу под реалистичной нагрузкой — сама
  // комбинация SoftAP+BLE Scan официально нестабильна на этом чипе
  // (Espressif coexist.html, статус "C1"), это не лечится на уровне
  // приложения полностью. Единственное, что реально снижает частоту —
  // не давать BLE-скану работать одновременно с активным ручным режимом
  // вообще: останавливаем его ОДИН РАЗ при входе в ST_MANUAL (не на
  // каждый джог), без ожидания подтверждения — сам stop() неблокирующий.
  bool enteringManual = (state != ST_MANUAL);
  state = ST_MANUAL; phase = PH_NONE;
  if (enteringManual && curMode != MODE_WIFI && pBLEScan && pBLEScan->isScanning()) {
    pBLEScan->stop();
  }

  JogCmd cmd{ (int16_t)pan, (int16_t)tilt };
  xQueueOverwrite(qJog, &cmd);

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleMapCsv() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("pan,tilt,rssi_ble,rssi_wifi,t_ms\n");
  String buf; buf.reserve(900);
  for (int p = cal.panAngleMin; p <= cal.panAngleMax; p++) {
    for (int t = cal.tiltAngleMin; t <= cal.tiltAngleMax; t++) {
      int8_t bv = mapBle[p][t], wv = mapWifi[p][t];
      if (bv == RSSI_NOT_MEASURED && wv == RSSI_NOT_MEASURED) continue;
      // ВАЖНО: приводим int8_t к int явно — иначе String() выберет
      // перегрузку String(char) и вместо числа напечатает символ.
      buf += String(p); buf += ','; buf += String(t); buf += ',';
      buf += String((int)bv); buf += ','; buf += String((int)wv); buf += ',';
      buf += String((uint32_t)mapTimeSec[p][t] * 1000UL); buf += '\n';
      if (buf.length() > 800) { server.sendContent(buf); buf = ""; }
    }
  }
  if (buf.length()) server.sendContent(buf);
}

void handleCalibrateGet() {
  JsonDocument doc;
  doc["pan_min_us"] = cal.panMinUs; doc["pan_max_us"] = cal.panMaxUs;
  doc["tilt_min_us"] = cal.tiltMinUs; doc["tilt_max_us"] = cal.tiltMaxUs;
  doc["pan_angle_min"] = cal.panAngleMin; doc["pan_angle_max"] = cal.panAngleMax;
  doc["tilt_angle_min"] = cal.tiltAngleMin; doc["tilt_angle_max"] = cal.tiltAngleMax;
  doc["rssi_floor"] = cal.rssiFloor;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static int argIntOr(const char* name, int cur) {
  if (!server.hasArg(name)) return cur;
  String v = server.arg(name);
  if (v.length() == 0) return cur;
  return v.toInt();
}

void handleCalibrateSet() {
  cal.panMinUs     = argIntOr("pan_min_us", cal.panMinUs);
  cal.panMaxUs     = argIntOr("pan_max_us", cal.panMaxUs);
  cal.tiltMinUs    = argIntOr("tilt_min_us", cal.tiltMinUs);
  cal.tiltMaxUs    = argIntOr("tilt_max_us", cal.tiltMaxUs);
  cal.panAngleMin  = argIntOr("pan_angle_min", cal.panAngleMin);
  cal.panAngleMax  = argIntOr("pan_angle_max", cal.panAngleMax);
  cal.tiltAngleMin = argIntOr("tilt_angle_min", cal.tiltAngleMin);
  cal.tiltAngleMax = argIntOr("tilt_angle_max", cal.tiltAngleMax);
  cal.rssiFloor    = argIntOr("rssi_floor", cal.rssiFloor);
  saveCalibration();
  setPan(curPanDeg); setTilt(curTiltDeg); // применяем новую калибровку немедленно
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Простые команды по Serial (калибровка/отладка без веба)
// ---------------------------------------------------------------------------
void serviceSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) {
        if (line == "PARK") { setPan(PARK_PAN_DEFAULT); setTilt(PARK_TILT_DEFAULT); state = ST_IDLE; Serial.println("OK PARK"); }
        else if (line == "STOP") { state = ST_IDLE; phase = PH_NONE; Serial.println("OK STOP"); }
        else if (line == "SAVE") { saveCalibration(); Serial.println("OK SAVE"); }
        else if (line == "STATUS") {
          Serial.printf("pan=%d tilt=%d panUs=[%d..%d] tiltUs=[%d..%d] panAng=[%d..%d] tiltAng=[%d..%d] floor=%d\n",
            curPanDeg, curTiltDeg, cal.panMinUs, cal.panMaxUs, cal.tiltMinUs, cal.tiltMaxUs,
            cal.panAngleMin, cal.panAngleMax, cal.tiltAngleMin, cal.tiltAngleMax, cal.rssiFloor);
        } else if (line.startsWith("CAL ")) {
          int sp1 = line.indexOf(' ', 4);
          if (sp1 > 0) {
            String field = line.substring(4, sp1);
            int val = line.substring(sp1 + 1).toInt();
            if (field == "PAN_MIN_US") cal.panMinUs = val;
            else if (field == "PAN_MAX_US") cal.panMaxUs = val;
            else if (field == "TILT_MIN_US") cal.tiltMinUs = val;
            else if (field == "TILT_MAX_US") cal.tiltMaxUs = val;
            else if (field == "PAN_ANGLE_MIN") cal.panAngleMin = val;
            else if (field == "PAN_ANGLE_MAX") cal.panAngleMax = val;
            else if (field == "TILT_ANGLE_MIN") cal.tiltAngleMin = val;
            else if (field == "TILT_ANGLE_MAX") cal.tiltAngleMax = val;
            else if (field == "RSSI_FLOOR") cal.rssiFloor = val;
            else { Serial.println("ERR unknown field"); line = ""; continue; }
            setPan(curPanDeg); setTilt(curTiltDeg);
            Serial.println("OK CAL (не забудь SAVE)");
          }
        } else {
          Serial.println("ERR unknown command");
        }
      }
      line = "";
    } else if (line.length() < 60) {
      line += c;
    }
  }
}
