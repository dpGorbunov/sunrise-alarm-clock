/*
  Рассвет-будильник v3
  - ESP8266 (NodeMCU) + WS2812B + DS3231
  - Источник времени: NTP (системное время ESP), DS3231 - резерв и память между перезагрузками
  - Работает без WiFi: будильник идёт по RTC, сеть и веб поднимаются когда появятся
  - Веб-интерфейс (PWA) на http://sunrise.local
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <time.h>
#include <sys/time.h>
#include <coredecls.h> // settimeofday_cb

// ===== НАСТРОЙКИ WiFi =====
#include "secrets.h" // ssid, password и otaPassword, см. secrets.h.example

// ===== НАСТРОЙКИ ЖЕЛЕЗА =====
#define LED_PIN D4
#define LED_COUNT 176
#define POWER_LIMIT 200 // Потолок яркости ленты 200/255 (~78%) - защита БП, раньше делалось strip.setBrightness(200)

// ===== ВРЕМЯ =====
const long tzOffset = 3 * 3600; // MSK, GMT+3, без перехода на летнее время
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.google.com";
const char* ntpServer3 = "time.cloudflare.com";
const time_t MIN_VALID_EPOCH = 1700000000; // 2023-11-14; всё что раньше - время не установлено
bool ntpSynced = false;
bool rtcFound = false;
volatile bool rtcWritePending = false; // NTP пришёл - записать время в DS3231 (делаем в loop, не в колбэке)

// ===== ОБЪЕКТЫ =====
ESP8266WebServer server(80);
RTC_DS3231 rtc;
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiEventHandler wifiGotIpHandler, wifiDisconnectedHandler;
volatile bool wifiJustConnected = false;
bool mdnsStarted = false;
bool otaStarted = false;

// ===== НАСТРОЙКИ (хранятся в EEPROM) =====
// Будильник
int alarmHour = 7;
int alarmMinute = 30;
bool alarmEnabled = true;
uint8_t weekdays = 0b00011111; // биты 0-6 = пн-вс, пн-пт включены

// Рассвет
int sunriseDuration = 20;         // минут
String sunrisePreset = "classic"; // classic, warm, soft, custom
uint32_t sunriseColors[3] = {0x990000, 0xFF4400, 0xFFE4C4}; // свой градиент

// После будильника
String afterAlarm = "30"; // keep, 30, 60

// Свечение
String lightPreset = "red"; // red, orange, yellow, white, rainbow, custom
uint32_t customColor = 0xFF8844;
int smoothness = 5;       // 0-10, шаг плавности ручного света
int autoOffMinutes = 5;   // 0 = выкл, иначе 5-120 минут

// Какой запуск будильника (дата + время) пользователь уже выключил. Переживает перезагрузку.
int32_t dismissedKey = -1;

// ===== РУЧНОЙ СВЕТ =====
int brightness = 0;             // целевая яркость 0-100 (её видит UI)
float currentBrightness = 0;    // текущая, плавно догоняет brightness
unsigned long lastInteractionTime = 0;

// ===== СОСТОЯНИЕ БУДИЛЬНИКА =====
// Окно запуска будильника, в которое попадает текущее время (объявлено здесь, чтобы Arduino
// мог сгенерировать прототипы функций, которые его возвращают)
struct AlarmWindow {
  bool active;    // текущее время попадает в окно какого-то запуска
  bool inSunrise; // true = идёт рассвет, false = время будильника уже наступило (окно удержания 5 мин)
  float progress; // 0..1
  int32_t key;
};

enum Phase { PHASE_IDLE, PHASE_SUNRISE, PHASE_HOLD };
Phase phase = PHASE_IDLE;
int32_t activeKey = -1;            // ключ запуска, который сейчас идёт
float sunriseProgress = 0;         // 0..1, обновляется раз в секунду из часов
unsigned long holdStartMillis = 0; // начало PHASE_HOLD, от него считается afterAlarm
long snoozeUntil = 0;              // локальное время (сек от 1970), когда отложенный будильник включится; 0 = нет

// Демо рассвета - со своими цветами, настройки будильника не трогает
bool demoActive = false;
unsigned long demoStartTime = 0;
int demoSpeed = 30;
String demoPreset = "classic";
uint32_t demoColors[3] = {0x990000, 0xFF4400, 0xFFE4C4};

// Текущий кадр рассвета/демо (float для плавности)
float curR = 0, curG = 0, curB = 0, curBr = 0; // текущие значения
float tgtR = 0, tgtG = 0, tgtB = 0, tgtBr = 0; // целевые значения

// ===== EEPROM =====
#define EEPROM_SIZE 64
#define ADDR_HOUR 0
#define ADDR_MINUTE 1
#define ADDR_ENABLED 2
#define ADDR_WEEKDAYS 3
#define ADDR_DURATION 4
#define ADDR_BRIGHTNESS 5        // не используется, оставлен ради раскладки
#define ADDR_SUNRISE_PRESET 6    // 1 byte: 0=classic, 1=warm, 2=soft, 3=custom
#define ADDR_LIGHT_PRESET 7      // 1 byte: 0=red, 1=orange, 2=yellow, 3=white, 4=rainbow, 5=custom
#define ADDR_CUSTOM_COLOR 8      // 3 bytes RGB
#define ADDR_SUNRISE_C1 11       // 3 bytes
#define ADDR_SUNRISE_C2 14       // 3 bytes
#define ADDR_SUNRISE_C3 17       // 3 bytes
#define ADDR_AFTER_ALARM 20      // 1 byte: 0=keep, 1=30, 2=60
#define ADDR_SMOOTHNESS 21       // 1 byte: 0-10
#define ADDR_COLOR_SMOOTHNESS 22 // не используется
#define ADDR_AUTO_OFF 23         // 1 byte: 0=off, или минуты (5-120)
#define ADDR_MAGIC 24            // 1 byte: magic number для проверки инициализации
#define ADDR_DISMISSED_KEY 25    // 4 bytes int32
#define MAGIC_VALUE 0xAB

// Настройки пишем во флеш не на каждое движение слайдера, а через 3 с после последнего изменения
bool settingsDirty = false;
unsigned long settingsDirtySince = 0;
const unsigned long EEPROM_COMMIT_DELAY = 3000;

// ===== ЖУРНАЛ =====
// Последние строки лога хранятся в памяти и отдаются по http://sunrise.local/log.
// Живут до перезагрузки. 40 строк по 160 байт (кириллица в UTF-8 по 2 байта) = 6.4 КБ.
#define LOG_LINES 40
#define LOG_LINE_LEN 160
char logBuf[LOG_LINES][LOG_LINE_LEN];
uint8_t logHead = 0;  // куда писать следующую строку
uint8_t logCount = 0; // сколько строк заполнено

// ===== HTML =====
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <title>Рассвет</title>
  <style>
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
    }
    
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(165deg, #1a1a2e 0%, #16213e 50%, #0f0f23 100%);
      color: #fff;
      min-height: 100vh;
      overflow-x: hidden;
    }
    
    /* ===== ГЛАВНЫЙ ЭКРАН ===== */
    .main-screen {
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 40px 20px;
      gap: 24px;
    }
    
    /* Блок будильника */
    .alarm-block {
      text-align: center;
    }
    
    .section-label {
      font-size: 0.75em;
      color: #666;
      text-transform: uppercase;
      letter-spacing: 1.5px;
      margin-bottom: 12px;
    }
    
    .alarm-icon {
      font-size: 1.5em;
      margin-bottom: 8px;
    }
    
    .time-display {
      font-size: 4em;
      font-weight: 200;
      letter-spacing: -2px;
      cursor: pointer;
      transition: opacity 0.2s;
    }
    
    .time-display:active {
      opacity: 0.7;
    }
    
    .toggle-wrapper {
      margin-top: 12px;
    }
    
    .toggle {
      width: 56px;
      height: 30px;
      background: #333;
      border-radius: 15px;
      position: relative;
      cursor: pointer;
      transition: background 0.3s;
    }
    
    .toggle.active {
      background: #ffd93d;
    }
    
    .toggle::after {
      content: '';
      position: absolute;
      width: 24px;
      height: 24px;
      background: #fff;
      border-radius: 50%;
      top: 3px;
      left: 3px;
      transition: transform 0.3s;
      box-shadow: 0 2px 4px rgba(0,0,0,0.3);
    }
    
    .toggle.active::after {
      transform: translateX(26px);
    }
    
    /* Дни недели */
    .weekdays {
      display: flex;
      gap: 8px;
      justify-content: center;
      margin-top: 16px;
    }
    
    .weekday {
      width: 34px;
      height: 34px;
      border-radius: 50%;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 0.7em;
      font-weight: 500;
      cursor: pointer;
      background: rgba(255,255,255,0.08);
      color: #555;
      transition: all 0.2s;
    }
    
    .weekday.active {
      background: rgba(255,217,61,0.2);
      color: #ffd93d;
    }
    
    /* Разделитель */
    .divider {
      width: 60px;
      height: 1px;
      background: rgba(255,255,255,0.15);
      margin: 8px 0;
    }
    
    /* Блок света */
    .light-block {
      width: 100%;
      max-width: 300px;
      text-align: center;
    }
    
    /* Пресеты цветов */
    .color-presets {
      display: flex;
      gap: 10px;
      justify-content: center;
      margin-bottom: 20px;
    }
    
    .color-preset {
      width: 38px;
      height: 38px;
      border-radius: 50%;
      cursor: pointer;
      transition: transform 0.2s, box-shadow 0.2s;
      border: 2px solid transparent;
    }
    
    .color-preset:active {
      transform: scale(0.9);
    }
    
    .color-preset.selected {
      box-shadow: 0 0 0 3px rgba(255,255,255,0.3);
      transform: scale(1.1);
    }
    
    .preset-red { background: #ff0000; }
    .preset-orange { background: #ff5500; }
    .preset-yellow { background: #ffcc00; }
    .preset-white { background: #ffffff; }
    .preset-rainbow { background: conic-gradient(#ff0000, #ffff00, #00ff00, #00ffff, #0000ff, #ff00ff, #ff0000); }
    .preset-custom { 
      background: rgba(255,255,255,0.1);
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 1em;
      border: 2px dashed rgba(255,255,255,0.3);
    }
    
    /* Слайдер яркости */
    .brightness-block {
      width: 100%;
    }
    
    .slider-wrapper {
      position: relative;
      padding: 10px 0;
    }
    
    .slider-tooltip {
      position: absolute;
      top: -30px;
      left: 50%;
      transform: translateX(-50%);
      background: #ffd93d;
      color: #1a1a2e;
      padding: 4px 10px;
      border-radius: 8px;
      font-size: 0.85em;
      font-weight: 600;
      opacity: 0;
      transition: opacity 0.15s;
      pointer-events: none;
      white-space: nowrap;
    }
    
    .slider-tooltip::after {
      content: '';
      position: absolute;
      bottom: -6px;
      left: 50%;
      transform: translateX(-50%);
      border-left: 6px solid transparent;
      border-right: 6px solid transparent;
      border-top: 6px solid #ffd93d;
    }
    
    .slider-tooltip.visible {
      opacity: 1;
    }
    
    .slider {
      width: 100%;
      height: 6px;
      border-radius: 3px;
      -webkit-appearance: none;
      background: rgba(255,255,255,0.15);
      outline: none;
    }
    
    .slider::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 28px;
      height: 28px;
      background: #fff;
      border-radius: 50%;
      cursor: pointer;
      box-shadow: 0 2px 8px rgba(0,0,0,0.3);
    }
    
    .brightness-label {
      text-align: center;
      margin-top: 8px;
      font-size: 0.8em;
      color: #666;
    }
    
    /* Кнопка настроек */
    .settings-btn {
      width: 44px;
      height: 44px;
      border-radius: 50%;
      background: rgba(255,255,255,0.08);
      border: none;
      color: #666;
      font-size: 1.2em;
      cursor: pointer;
      transition: all 0.2s;
      margin-top: 8px;
    }
    
    .settings-btn:active {
      transform: scale(0.95);
      background: rgba(255,255,255,0.12);
    }
    
    /* Экран рассвета - iOS стиль */
    .sunrise-screen {
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: linear-gradient(180deg, #0a0a15 0%, #1a1020 30%, #2d1b2e 50%, #3d2040 70%, #2a1a2e 100%);
      display: none;
      flex-direction: column;
      align-items: center;
      justify-content: space-between;
      padding: 60px 20px 50px;
      z-index: 100;
    }
    
    .sunrise-screen.active {
      display: flex;
    }
    
    .sunrise-top {
      text-align: center;
    }
    
    .sunrise-label {
      font-size: 1.1em;
      font-weight: 400;
      color: rgba(255,255,255,0.6);
      letter-spacing: 0.5px;
      margin-bottom: 8px;
    }
    
    .sunrise-time {
      font-size: 5em;
      font-weight: 200;
      color: #fff;
      letter-spacing: -3px;
      line-height: 1;
    }
    
    .sunrise-center {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 20px;
    }
    
    .sunrise-glow {
      width: 180px;
      height: 180px;
      border-radius: 50%;
      background: radial-gradient(circle, rgba(255,150,50,0.4) 0%, rgba(255,100,50,0.1) 50%, transparent 70%);
      display: flex;
      align-items: center;
      justify-content: center;
      animation: glow 3s ease-in-out infinite;
    }
    
    @keyframes glow {
      0%, 100% { 
        transform: scale(1);
        opacity: 0.8;
      }
      50% { 
        transform: scale(1.15);
        opacity: 1;
      }
    }
    
    .sunrise-icon {
      font-size: 4.5em;
      filter: drop-shadow(0 0 30px rgba(255,150,50,0.5));
    }
    
    .sunrise-status {
      font-size: 1em;
      color: rgba(255,255,255,0.5);
      font-weight: 400;
    }
    
    .sunrise-bottom {
      width: 100%;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    
    .sunrise-off-btn {
      width: 100%;
      padding: 18px;
      border-radius: 14px;
      background: rgba(255,255,255,0.15);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      border: 1px solid rgba(255,255,255,0.1);
      color: #fff;
      font-size: 1.1em;
      font-weight: 500;
      cursor: pointer;
      transition: all 0.2s;
    }
    
    .sunrise-off-btn:active {
      transform: scale(0.98);
      background: rgba(255,255,255,0.25);
    }
    
    .sunrise-snooze-btn {
      width: 100%;
      padding: 18px;
      border-radius: 14px;
      background: rgba(255,150,50,0.3);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      border: 1px solid rgba(255,150,50,0.2);
      color: #fff;
      font-size: 1.1em;
      font-weight: 500;
      cursor: pointer;
      transition: all 0.2s;
    }
    
    .sunrise-snooze-btn:active {
      transform: scale(0.98);
      background: rgba(255,150,50,0.4);
    }
    
    /* ===== ЭКРАН НАСТРОЕК ===== */
    .settings-screen {
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: linear-gradient(165deg, #1a1a2e 0%, #16213e 50%, #0f0f23 100%);
      transform: translateX(100%);
      transition: transform 0.3s ease;
      overflow-y: auto;
      padding: 20px;
      padding-bottom: 40px;
    }
    
    .settings-screen.open {
      transform: translateX(0);
    }
    
    .settings-header {
      display: flex;
      align-items: center;
      gap: 16px;
      margin-bottom: 32px;
    }
    
    .back-btn {
      width: 40px;
      height: 40px;
      border-radius: 50%;
      background: rgba(255,255,255,0.1);
      border: none;
      color: #fff;
      font-size: 1.2em;
      cursor: pointer;
    }
    
    .settings-title {
      font-size: 1.3em;
      font-weight: 500;
    }
    
    .section {
      margin-bottom: 32px;
    }
    
    .section-title {
      font-size: 0.75em;
      color: #888;
      text-transform: uppercase;
      letter-spacing: 1.5px;
      margin-bottom: 16px;
    }
    
    .section-content {
      background: rgba(255,255,255,0.05);
      border-radius: 16px;
      padding: 20px;
    }
    
    /* Минуты до рассвета */
    .duration-selector {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 20px;
    }
    
    .duration-btn {
      width: 44px;
      height: 44px;
      border-radius: 50%;
      background: rgba(255,255,255,0.1);
      border: none;
      color: #fff;
      font-size: 1.5em;
      cursor: pointer;
    }
    
    .duration-btn:active {
      background: rgba(255,255,255,0.2);
    }
    
    .duration-value {
      font-size: 1.5em;
      min-width: 100px;
      text-align: center;
    }
    
    /* Кастомный градиент */
    .sunrise-presets {
      display: flex;
      gap: 10px;
      margin-bottom: 20px;
    }
    
    .sunrise-preset {
      flex: 1;
      cursor: pointer;
      text-align: center;
      padding: 8px;
      border-radius: 12px;
      background: rgba(255,255,255,0.05);
      transition: all 0.2s;
    }
    
    .sunrise-preset.selected {
      background: rgba(255,217,61,0.15);
    }
    
    .sunrise-preset-preview {
      height: 40px;
      border-radius: 8px;
      margin-bottom: 8px;
    }
    
    .sunrise-preset span {
      font-size: 0.75em;
      color: #888;
    }
    
    .sunrise-preset.selected span {
      color: #ffd93d;
    }
    
    .custom-sunrise-title {
      font-size: 0.8em;
      color: #666;
      margin-bottom: 12px;
      padding-top: 16px;
      border-top: 1px solid rgba(255,255,255,0.1);
    }
    
    .gradient-preview {
      height: 50px;
      border-radius: 12px;
      margin-bottom: 20px;
    }
    
    .gradient-stops {
      display: flex;
      justify-content: space-between;
    }
    
    .color-stop {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 8px;
    }
    
    .color-stop input[type="color"] {
      width: 50px;
      height: 50px;
      border: none;
      border-radius: 50%;
      cursor: pointer;
      background: none;
    }
    
    .color-stop input[type="color"]::-webkit-color-swatch-wrapper {
      padding: 0;
    }
    
    .color-stop input[type="color"]::-webkit-color-swatch {
      border: 3px solid rgba(255,255,255,0.2);
      border-radius: 50%;
    }
    
    .color-stop span {
      font-size: 0.7em;
      color: #888;
    }
    
    .apply-gradient-btn {
      width: 100%;
      padding: 14px;
      border-radius: 12px;
      background: rgba(255,217,61,0.2);
      border: none;
      color: #ffd93d;
      font-size: 0.95em;
      font-weight: 500;
      cursor: pointer;
      margin-top: 16px;
    }
    
    .apply-gradient-btn:active {
      background: rgba(255,217,61,0.3);
    }
    
    /* Демо */
    .demo-btn {
      width: 100%;
      padding: 16px;
      border-radius: 12px;
      background: linear-gradient(135deg, #ff5500 0%, #ffcc00 100%);
      border: none;
      color: #1a1a2e;
      font-size: 1em;
      font-weight: 600;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      margin-bottom: 16px;
    }
    
    .demo-btn:active {
      transform: scale(0.98);
    }
    
    .demo-btn.running {
      background: linear-gradient(135deg, #990000 0%, #ff4400 100%);
    }
    
    .demo-speed {
      margin-top: 8px;
    }
    
    .demo-speed-label {
      display: flex;
      justify-content: space-between;
      font-size: 0.75em;
      color: #666;
      margin-top: 8px;
    }
    
    .smoothness-labels {
      display: flex;
      justify-content: space-between;
      font-size: 0.75em;
      color: #666;
      margin-top: 8px;
    }
    
    .slider-value-center {
      text-align: center;
      font-size: 0.85em;
      color: #ffd93d;
      font-weight: 500;
      margin-top: 4px;
    }
    
    /* После будильника */
    .radio-group {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    
    .radio-item {
      display: flex;
      align-items: center;
      gap: 12px;
      cursor: pointer;
    }
    
    .radio-circle {
      width: 22px;
      height: 22px;
      border-radius: 50%;
      border: 2px solid #444;
      position: relative;
      flex-shrink: 0;
    }
    
    .radio-item.selected .radio-circle {
      border-color: #ffd93d;
    }
    
    .radio-item.selected .radio-circle::after {
      content: '';
      position: absolute;
      width: 12px;
      height: 12px;
      background: #ffd93d;
      border-radius: 50%;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -50%);
    }
    
    .radio-label {
      font-size: 0.95em;
    }
    
    /* Инфо */
    .info-row {
      display: flex;
      justify-content: space-between;
      padding: 8px 0;
      font-size: 0.9em;
    }
    
    .info-row .label {
      color: #888;
    }
    
    .info-row .value {
      color: #fff;
    }
    
    .setting-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 10px;
    }
    
    .setting-label {
      font-size: 0.9em;
      color: #aaa;
    }
    
    .setting-value {
      font-size: 0.9em;
      color: #ffd93d;
      font-weight: 500;
    }
    
    .setting-hint {
      font-size: 0.7em;
      color: #555;
      margin-top: 8px;
    }
    
    /* ===== МОДАЛКА ВРЕМЕНИ ===== */
    .time-modal {
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: rgba(0,0,0,0.85);
      display: flex;
      align-items: flex-end;
      justify-content: center;
      opacity: 0;
      visibility: hidden;
      transition: all 0.3s;
      z-index: 1000;
    }
    
    .time-modal.open {
      opacity: 1;
      visibility: visible;
    }
    
    .time-modal-content {
      background: #1e1e2e;
      width: 100%;
      max-width: 400px;
      border-radius: 20px 20px 0 0;
      padding: 0 0 30px 0;
      transform: translateY(100%);
      transition: transform 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    }
    
    .time-modal.open .time-modal-content {
      transform: translateY(0);
    }
    
    .modal-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 16px 20px;
      border-bottom: 1px solid rgba(255,255,255,0.1);
    }
    
    .modal-cancel {
      background: none;
      border: none;
      color: #888;
      font-size: 1em;
      cursor: pointer;
      padding: 8px 12px;
    }
    
    .modal-done {
      background: none;
      border: none;
      color: #ffd93d;
      font-size: 1em;
      font-weight: 600;
      cursor: pointer;
      padding: 8px 12px;
    }
    
    .modal-title {
      font-size: 1em;
      font-weight: 500;
      color: #aaa;
    }
    
    /* ===== БАРАБАНЫ (iOS стиль) ===== */
    .picker-wrapper {
      position: relative;
      height: 220px;
      overflow: hidden;
    }
    
    /* Затемнение сверху и снизу */
    .picker-wrapper::before,
    .picker-wrapper::after {
      content: '';
      position: absolute;
      left: 0;
      right: 0;
      height: 88px;
      pointer-events: none;
      z-index: 10;
    }
    
    .picker-wrapper::before {
      top: 0;
      background: linear-gradient(to bottom, #1e1e2e 0%, transparent 100%);
    }
    
    .picker-wrapper::after {
      bottom: 0;
      background: linear-gradient(to top, #1e1e2e 0%, transparent 100%);
    }
    
    /* Подсветка выбранного */
    .picker-highlight {
      position: absolute;
      left: 20px;
      right: 20px;
      top: 50%;
      transform: translateY(-50%);
      height: 44px;
      background: rgba(255,255,255,0.08);
      border-radius: 10px;
      pointer-events: none;
    }
    
    .picker-container {
      display: flex;
      justify-content: center;
      align-items: center;
      height: 100%;
    }
    
    .picker-column {
      width: 80px;
      height: 100%;
      overflow: hidden;
      position: relative;
      cursor: grab;
    }
    
    .picker-column:active {
      cursor: grabbing;
    }
    
    .picker-scroll {
      padding: 88px 0;
    }
    
    .picker-item {
      height: 44px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 22px;
      font-weight: 400;
      color: #444;
      transition: color 0.15s, transform 0.15s;
      user-select: none;
    }
    
    .picker-separator {
      font-size: 28px;
      font-weight: 300;
      color: #fff;
      padding: 0 4px;
      user-select: none;
    }
    .snooze-info {
      min-height: 18px;
      margin-top: 4px;
      font-size: 14px;
      color: #ffb366;
      text-align: center;
    }
  </style>
</head>
<body>

  <!-- Главный экран -->
  <div class="main-screen" id="mainScreen">
    
    <!-- Блок будильника -->
    <div class="alarm-block">
      <div class="section-label">Будильник-рассвет</div>
      <div class="alarm-icon">☀️</div>
      <div class="time-display" id="timeDisplay" onclick="openTimeModal()">07:30</div>
      <div class="snooze-info" id="snoozeInfo"></div>
      <div class="toggle-wrapper">
        <div class="toggle active" id="alarmToggle" onclick="toggleAlarm()"></div>
      </div>
      <div class="weekdays" id="weekdays">
        <div class="weekday active" data-day="0">пн</div>
        <div class="weekday active" data-day="1">вт</div>
        <div class="weekday active" data-day="2">ср</div>
        <div class="weekday active" data-day="3">чт</div>
        <div class="weekday active" data-day="4">пт</div>
        <div class="weekday" data-day="5">сб</div>
        <div class="weekday" data-day="6">вс</div>
      </div>
    </div>
    
    <div class="divider"></div>
    
    <!-- Блок света -->
    <div class="light-block">
      <div class="section-label">Свет</div>
      <div class="color-presets">
        <div class="color-preset preset-red selected" onclick="selectPreset(this, 'red')" title="Красный"></div>
        <div class="color-preset preset-orange" onclick="selectPreset(this, 'orange')" title="Оранжевый"></div>
        <div class="color-preset preset-yellow" onclick="selectPreset(this, 'yellow')" title="Жёлтый"></div>
        <div class="color-preset preset-white" onclick="selectPreset(this, 'white')" title="Белый"></div>
        <div class="color-preset preset-rainbow" onclick="selectPreset(this, 'rainbow')" title="Радуга"></div>
        <div class="color-preset preset-custom" onclick="openColorPicker()" title="Свой цвет">✎</div>
      </div>
      
      <!-- Скрытый пикер для своего цвета -->
      <input type="color" id="customColorPicker" style="display:none" onchange="applyCustomColor(this.value)">
      
      <div class="brightness-block">
        <div class="slider-wrapper">
          <div class="slider-tooltip" id="brightnessTooltip">0%</div>
          <input type="range" class="slider" id="brightness" min="0" max="100" value="0" 
            oninput="updateBrightnessLabel(this.value)" 
            onchange="sendBrightness()"
            ontouchstart="showTooltip('brightnessTooltip', this)"
            ontouchmove="moveTooltip('brightnessTooltip', this)"
            ontouchend="hideTooltip('brightnessTooltip')"
            onmousedown="showTooltip('brightnessTooltip', this)"
            onmousemove="moveTooltip('brightnessTooltip', this)"
            onmouseup="hideTooltip('brightnessTooltip')"
            onmouseleave="hideTooltip('brightnessTooltip')">
        </div>
        <div class="brightness-label" id="brightnessLabel">Выключено</div>
      </div>
    </div>
    
    <button class="settings-btn" onclick="openSettings()">⚙️</button>
  </div>

  <!-- Экран рассвета -->
  <div class="sunrise-screen" id="sunriseScreen">
    <div class="sunrise-top">
      <div class="sunrise-label">Будильник</div>
      <div class="sunrise-time" id="sunriseTime">07:30</div>
    </div>
    
    <div class="sunrise-center">
      <div class="sunrise-glow">
        <div class="sunrise-icon">☀️</div>
      </div>
      <div class="sunrise-status">Рассвет</div>
    </div>
    
    <div class="sunrise-bottom">
      <button class="sunrise-snooze-btn" onclick="snoozeSunrise()">Отложить на 5 мин</button>
      <button class="sunrise-off-btn" onclick="turnOffSunrise()">Выключить</button>
    </div>
  </div>

  <!-- Экран настроек -->
  <div class="settings-screen" id="settingsScreen">
    <div class="settings-header">
      <button class="back-btn" onclick="closeSettings()">←</button>
      <div class="settings-title">Настройки</div>
    </div>
    
    <div class="section">
      <div class="section-title">Рассвет</div>
      <div class="section-content">
        <div class="duration-selector">
          <button class="duration-btn" onclick="changeDuration(-5)">−</button>
          <div class="duration-value" id="durationValue">20 мин</div>
          <button class="duration-btn" onclick="changeDuration(5)">+</button>
        </div>
      </div>
    </div>
    
    <div class="section">
      <div class="section-title">Градиент рассвета</div>
      <div class="section-content">
        <div class="sunrise-presets">
          <div class="sunrise-preset selected" onclick="selectSunrisePreset(this, 'classic')" data-preset="classic">
            <div class="sunrise-preset-preview" style="background: linear-gradient(90deg, #990000, #ff4400, #ffe4c4)"></div>
            <span>Классика</span>
          </div>
          <div class="sunrise-preset" onclick="selectSunrisePreset(this, 'warm')" data-preset="warm">
            <div class="sunrise-preset-preview" style="background: linear-gradient(90deg, #881100, #ff5500, #ffdaaa)"></div>
            <span>Тёплый</span>
          </div>
          <div class="sunrise-preset" onclick="selectSunrisePreset(this, 'soft')" data-preset="soft">
            <div class="sunrise-preset-preview" style="background: linear-gradient(90deg, #663300, #ff7722, #ffeedd)"></div>
            <span>Мягкий</span>
          </div>
        </div>
        
        <div class="custom-sunrise-title">Свой градиент</div>
        <div class="gradient-preview" id="gradientPreview" style="background: linear-gradient(90deg, #990000 0%, #ff4400 50%, #ffe4c4 100%)"></div>
        <div class="gradient-stops">
          <div class="color-stop">
            <input type="color" id="color1" value="#990000" onchange="updateCustomGradient()">
            <span>Начало</span>
          </div>
          <div class="color-stop">
            <input type="color" id="color2" value="#ff4400" onchange="updateCustomGradient()">
            <span>Середина</span>
          </div>
          <div class="color-stop">
            <input type="color" id="color3" value="#ffe4c4" onchange="updateCustomGradient()">
            <span>Конец</span>
          </div>
        </div>
        <button class="apply-gradient-btn" onclick="applyCustomSunrise()">Применить свой градиент</button>
      </div>
    </div>
    
    <div class="section">
      <div class="section-title">Демо рассвета</div>
      <div class="section-content">
        <button class="demo-btn" id="demoBtn" onclick="toggleDemo()">
          <span id="demoBtnIcon">▶</span>
          <span id="demoBtnText">Запустить рассвет</span>
        </button>
        <div class="demo-speed">
          <div class="slider-wrapper">
            <div class="slider-tooltip" id="demoSpeedTooltip">1 мин 5 сек</div>
            <input type="range" class="slider" id="demoSpeed" min="10" max="120" value="65" 
              oninput="updateDemoSpeed(this.value)"
              ontouchstart="showTooltip('demoSpeedTooltip', this)"
              ontouchmove="moveTooltip('demoSpeedTooltip', this)"
              ontouchend="hideTooltip('demoSpeedTooltip')"
              onmousedown="showTooltip('demoSpeedTooltip', this)"
              onmousemove="moveTooltip('demoSpeedTooltip', this)"
              onmouseup="hideTooltip('demoSpeedTooltip')"
              onmouseleave="hideTooltip('demoSpeedTooltip')">
          </div>
          <div class="demo-speed-label">
            <span>10 сек</span>
            <span>2 мин</span>
          </div>
          <div class="slider-value-center" id="demoSpeedValue">1 мин 5 сек</div>
        </div>
      </div>
    </div>
    
    <div class="section">
      <div class="section-title">Скорость изменения яркости</div>
      <div class="section-content">
        <div class="slider-wrapper">
          <div class="slider-tooltip" id="smoothnessTooltip">5</div>
          <input type="range" class="slider" id="smoothness" min="0" max="10" value="5" 
            oninput="updateSmoothness(this.value)"
               onchange="sendData()"
            ontouchstart="showTooltip('smoothnessTooltip', this)"
            ontouchmove="moveTooltip('smoothnessTooltip', this)"
            ontouchend="hideTooltip('smoothnessTooltip')"
            onmousedown="showTooltip('smoothnessTooltip', this)"
            onmousemove="moveTooltip('smoothnessTooltip', this)"
            onmouseup="hideTooltip('smoothnessTooltip')"
            onmouseleave="hideTooltip('smoothnessTooltip')">
        </div>
        <div class="smoothness-labels">
          <span>Плавно</span>
          <span>Резко</span>
        </div>
        <div class="slider-value-center" id="smoothnessValue">5</div>
      </div>
    </div>

    <div class="section">
      <div class="section-title">Автовыключение при бездействии</div>
      <div class="section-content">
        <div class="slider-wrapper">
          <div class="slider-tooltip" id="autoOffTooltip">5 мин</div>
          <input type="range" class="slider" id="autoOff" min="0" max="120" step="5" value="5" 
            oninput="updateAutoOff(this.value)"
               onchange="sendData()"
            ontouchstart="showTooltip('autoOffTooltip', this)"
            ontouchmove="moveTooltip('autoOffTooltip', this)"
            ontouchend="hideTooltip('autoOffTooltip')"
            onmousedown="showTooltip('autoOffTooltip', this)"
            onmousemove="moveTooltip('autoOffTooltip', this)"
            onmouseup="hideTooltip('autoOffTooltip')"
            onmouseleave="hideTooltip('autoOffTooltip')">
        </div>
        <div class="smoothness-labels">
          <span>Выкл</span>
          <span>2 часа</span>
        </div>
        <div class="slider-value-center" id="autoOffValue">5 мин</div>
      </div>
    </div>

    <div class="section">
      <div class="section-title">После будильника</div>
      <div class="section-content">
        <div class="radio-group" id="afterAlarm">
          <div class="radio-item" onclick="selectAfterAlarm(this, 'keep')">
            <div class="radio-circle"></div>
            <span class="radio-label">Держать свет</span>
          </div>
          <div class="radio-item selected" onclick="selectAfterAlarm(this, '30')">
            <div class="radio-circle"></div>
            <span class="radio-label">Выключить через 30 мин</span>
          </div>
          <div class="radio-item" onclick="selectAfterAlarm(this, '60')">
            <div class="radio-circle"></div>
            <span class="radio-label">Выключить через 1 час</span>
          </div>
        </div>
      </div>
    </div>
    
    <div class="section">
      <div class="section-title">Информация</div>
      <div class="section-content">
        <div class="info-row">
          <span class="label">WiFi</span>
          <span class="value" id="wifiName">—</span>
        </div>
        <div class="info-row">
          <span class="label">IP</span>
          <span class="value" id="ipAddr">—</span>
        </div>
        <div class="info-row">
          <span class="label">Время устройства</span>
          <span class="value" id="devTime">—</span>
        </div>
        <div class="info-row">
          <span class="label">Источник времени</span>
          <span class="value" id="timeSrc">—</span>
        </div>
        <div class="info-row">
          <span class="label">Часы DS3231</span>
          <span class="value" id="rtcStatus">—</span>
        </div>
        <div class="info-row">
          <span class="label">Журнал</span>
          <span class="value"><a href="/log" target="_blank" style="color:#ffb366">открыть</a></span>
        </div>
        <div class="info-row">
          <span class="label">Адрес</span>
          <span class="value">sunrise.local</span>
        </div>
      </div>
    </div>
    
    <div class="section">
      <div class="section-content" style="padding-top: 10px;">
        <button class="demo-btn" onclick="resetSettings()" style="background: linear-gradient(135deg, #ff4444 0%, #cc0000 100%);">
          <span>⟲</span>
          <span>Сбросить настройки</span>
        </button>
      </div>
    </div>
  </div>

  <!-- Модалка выбора времени -->
  <div class="time-modal" id="timeModal">
    <div class="time-modal-content" onclick="event.stopPropagation()">
      <div class="modal-header">
        <button class="modal-cancel" onclick="closeTimeModal()">Отмена</button>
        <span class="modal-title">Будильник</span>
        <button class="modal-done" onclick="saveTime()">Готово</button>
      </div>
      
      <div class="picker-wrapper">
        <div class="picker-highlight"></div>
        
        <div class="picker-container">
          <div class="picker-column" id="hourColumn">
            <div class="picker-scroll" id="hourScroll"></div>
          </div>
          
          <div class="picker-separator">:</div>
          
          <div class="picker-column" id="minuteColumn">
            <div class="picker-scroll" id="minuteScroll"></div>
          </div>
        </div>
      </div>
    </div>
  </div>

  <script>
    // Состояние приложения
    let alarmEnabled = true;
    let alarmHours = 7;
    let alarmMinutes = 30;
    let brightness = 0;
    let duration = 20;
    let selectedPreset = 'red';
    let selectedSunrisePreset = 'classic';
    let customColor = '#ff5500';
    let smoothness = 5;
    let autoOffMinutes = 5;
    let weekdaysBits = 0b00011111;
    let afterAlarmValue = '30';
    let demoRunning = false;
    let demoInterval = null;
    
    // ===== PICKER =====
    class WheelPicker {
      constructor(container, scrollEl, itemCount, initialValue) {
        this.container = container;
        this.scrollEl = scrollEl;
        this.itemCount = itemCount;
        this.value = initialValue;
        this.itemHeight = 44;
        
        this.currentY = 0;
        this.startY = 0;
        this.lastY = 0;
        this.lastTime = 0;
        this.velocity = 0;
        this.isDragging = false;
        this.animationId = null;
        
        this.init();
      }
      
      init() {
        // Генерируем элементы
        this.scrollEl.innerHTML = '';
        for (let i = 0; i < this.itemCount; i++) {
          const item = document.createElement('div');
          item.className = 'picker-item';
          item.textContent = String(i).padStart(2, '0');
          item.dataset.value = i;
          this.scrollEl.appendChild(item);
        }
        
        // Устанавливаем начальную позицию
        this.currentY = -this.value * this.itemHeight;
        this.updatePosition(false);
        
        // События
        this.container.addEventListener('touchstart', this.onTouchStart.bind(this), { passive: false });
        this.container.addEventListener('touchmove', this.onTouchMove.bind(this), { passive: false });
        this.container.addEventListener('touchend', this.onTouchEnd.bind(this));
        
        this.container.addEventListener('mousedown', this.onMouseDown.bind(this));
        document.addEventListener('mousemove', this.onMouseMove.bind(this));
        document.addEventListener('mouseup', this.onMouseUp.bind(this));
        
        this.container.addEventListener('wheel', this.onWheel.bind(this), { passive: false });
      }
      
      onTouchStart(e) {
        this.stopAnimation();
        this.isDragging = true;
        this.startY = e.touches[0].clientY;
        this.lastY = this.startY;
        this.lastTime = Date.now();
        this.velocity = 0;
      }
      
      onTouchMove(e) {
        if (!this.isDragging) return;
        e.preventDefault();
        
        const y = e.touches[0].clientY;
        const deltaY = y - this.lastY;
        const now = Date.now();
        const deltaTime = now - this.lastTime;
        
        if (deltaTime > 0) {
          this.velocity = deltaY / deltaTime * 15;
        }
        
        this.currentY += deltaY;
        this.lastY = y;
        this.lastTime = now;
        
        this.updatePosition(false);
      }
      
      onTouchEnd() {
        if (!this.isDragging) return;
        this.isDragging = false;
        this.finishScroll();
      }
      
      onMouseDown(e) {
        this.stopAnimation();
        this.isDragging = true;
        this.startY = e.clientY;
        this.lastY = this.startY;
        this.lastTime = Date.now();
        this.velocity = 0;
        e.preventDefault();
      }
      
      onMouseMove(e) {
        if (!this.isDragging) return;
        
        const y = e.clientY;
        const deltaY = y - this.lastY;
        const now = Date.now();
        const deltaTime = now - this.lastTime;
        
        if (deltaTime > 0) {
          this.velocity = deltaY / deltaTime * 15;
        }
        
        this.currentY += deltaY;
        this.lastY = y;
        this.lastTime = now;
        
        this.updatePosition(false);
      }
      
      onMouseUp() {
        if (!this.isDragging) return;
        this.isDragging = false;
        this.finishScroll();
      }
      
      onWheel(e) {
        e.preventDefault();
        this.stopAnimation();
        
        const delta = e.deltaY > 0 ? 1 : -1;
        this.value = Math.max(0, Math.min(this.itemCount - 1, this.value + delta));
        this.currentY = -this.value * this.itemHeight;
        this.updatePosition(true);
      }
      
      finishScroll() {
        // Инерция
        if (Math.abs(this.velocity) > 0.5) {
          this.animateWithVelocity();
        } else {
          this.snapToNearest();
        }
      }
      
      animateWithVelocity() {
        const friction = 0.95;
        
        const animate = () => {
          this.velocity *= friction;
          this.currentY += this.velocity;
          
          // Ограничиваем границы
          const minY = -(this.itemCount - 1) * this.itemHeight;
          const maxY = 0;
          
          if (this.currentY > maxY) {
            this.currentY = maxY;
            this.velocity = 0;
          } else if (this.currentY < minY) {
            this.currentY = minY;
            this.velocity = 0;
          }
          
          this.updatePosition(false);
          
          if (Math.abs(this.velocity) > 0.5) {
            this.animationId = requestAnimationFrame(animate);
          } else {
            this.snapToNearest();
          }
        };
        
        this.animationId = requestAnimationFrame(animate);
      }
      
      snapToNearest() {
        const targetValue = Math.round(-this.currentY / this.itemHeight);
        this.value = Math.max(0, Math.min(this.itemCount - 1, targetValue));
        this.currentY = -this.value * this.itemHeight;
        this.updatePosition(true);
      }
      
      stopAnimation() {
        if (this.animationId) {
          cancelAnimationFrame(this.animationId);
          this.animationId = null;
        }
      }
      
      updatePosition(animate) {
        if (animate) {
          this.scrollEl.style.transition = 'transform 0.3s cubic-bezier(0.2, 0, 0.2, 1)';
        } else {
          this.scrollEl.style.transition = 'none';
        }
        
        this.scrollEl.style.transform = `translateY(${this.currentY}px)`;
        
        // Обновляем стили элементов
        const items = this.scrollEl.querySelectorAll('.picker-item');
        const centerIndex = Math.round(-this.currentY / this.itemHeight);
        
        items.forEach((item, index) => {
          const distance = Math.abs(index - centerIndex);
          
          if (distance === 0) {
            item.style.color = '#fff';
            item.style.transform = 'scale(1)';
          } else if (distance === 1) {
            item.style.color = '#888';
            item.style.transform = 'scale(0.9)';
          } else {
            item.style.color = '#444';
            item.style.transform = 'scale(0.85)';
          }
        });
      }
      
      setValue(val) {
        this.value = val;
        this.currentY = -this.value * this.itemHeight;
        this.updatePosition(false);
      }
      
      getValue() {
        return this.value;
      }
    }
    
    let hourPicker, minutePicker;
    
    function initPickers() {
      const hourColumn = document.getElementById('hourColumn');
      const hourScroll = document.getElementById('hourScroll');
      const minuteColumn = document.getElementById('minuteColumn');
      const minuteScroll = document.getElementById('minuteScroll');
      
      hourPicker = new WheelPicker(hourColumn, hourScroll, 24, alarmHours);
      minutePicker = new WheelPicker(minuteColumn, minuteScroll, 60, alarmMinutes);
    }
    
    // Тумблер будильника
    function toggleAlarm() {
      alarmEnabled = !alarmEnabled;
      document.getElementById('alarmToggle').classList.toggle('active', alarmEnabled);
      sendData();
    }
    
    // Дни недели
    document.querySelectorAll('.weekday').forEach((day, index) => {
      day.addEventListener('click', () => {
        day.classList.toggle('active');
        updateWeekdays();
        sendData();
      });
    });
    
    function updateWeekdays() {
      weekdaysBits = 0;
      document.querySelectorAll('.weekday').forEach((el, i) => {
        if (el.classList.contains('active')) weekdaysBits |= (1 << i);
      });
    }
    
    // Пресеты цветов свечения
    function selectPreset(el, preset) {
      document.querySelectorAll('.color-preset').forEach(p => p.classList.remove('selected'));
      el.classList.add('selected');
      selectedPreset = preset;
      sendLight();
    }
    
    // Свой цвет свечения
    function openColorPicker() {
      document.getElementById('customColorPicker').click();
    }
    
    function applyCustomColor(color) {
      selectedPreset = 'custom';
      showCustomColor(color);
      markLightPreset('custom');
      sendLight();
    }
    
    // Пресеты градиента рассвета
    function selectSunrisePreset(el, preset) {
      document.querySelectorAll('.sunrise-preset').forEach(p => p.classList.remove('selected'));
      el.classList.add('selected');
      selectedSunrisePreset = preset;
      sendData();
    }
    
    // Свой градиент рассвета
    function applyCustomSunrise() {
      document.querySelectorAll('.sunrise-preset').forEach(p => p.classList.remove('selected'));
      selectedSunrisePreset = 'custom';
      
      // Визуальный фидбек
      const btn = document.querySelector('.apply-gradient-btn');
      btn.textContent = 'Применено ✓';
      btn.style.background = 'rgba(100, 200, 100, 0.2)';
      btn.style.color = '#8f8';
      
      sendData();
      
      setTimeout(() => {
        btn.textContent = 'Применить свой градиент';
        btn.style.background = '';
        btn.style.color = '';
      }, 1500);
    }
    
    // Яркость - только обновление label
    function updateBrightnessLabel(val) {
      brightness = parseInt(val);
      const label = document.getElementById('brightnessLabel');
      if (brightness === 0) {
        label.textContent = 'Выключено';
      } else {
        label.textContent = brightness + '%';
      }
    }
    
    // Яркость - отправка на сервер (вызывается при отпускании)
    function sendBrightness() {
      sendLight();
    }
    
    function sendLight() {
      let url = '/light?p=' + selectedPreset + '&b=' + brightness;
      if (selectedPreset === 'custom') {
        const c = customColor.startsWith('#') ? customColor.slice(1) : customColor;
        url += '&c=' + c;
      }
      fetch(url);
    }
    
    function sendData() {
      const c1 = document.getElementById('color1').value;
      const c2 = document.getElementById('color2').value;
      const c3 = document.getElementById('color3').value;
      fetch('/save?h=' + alarmHours + '&m=' + alarmMinutes + '&en=' + (alarmEnabled?1:0) + '&wd=' + weekdaysBits + '&dur=' + duration + '&sp=' + selectedSunrisePreset + '&c1=' + c1.slice(1) + '&c2=' + c2.slice(1) + '&c3=' + c3.slice(1) + '&aa=' + afterAlarmValue + '&smooth=' + smoothness + '&aoff=' + autoOffMinutes);
    }
    
    function resetSettings() {
      if (confirm('Сбросить все настройки? Устройство перезагрузится.')) {
        document.body.innerHTML = '<div style="display:flex;justify-content:center;align-items:center;height:100vh;color:#fff;font-size:18px;text-align:center;padding:20px;">Сброс настроек...<br>Подождите</div>';
        fetch('/reset').finally(() => {
          // Ждём пока ESP перезагрузится
          document.body.innerHTML = '<div style="display:flex;justify-content:center;align-items:center;height:100vh;color:#fff;font-size:18px;text-align:center;padding:20px;">Перезагрузка устройства...<br>Страница обновится автоматически</div>';
          setTimeout(() => {
            // Полная перезагрузка страницы без кэша
            window.location.href = window.location.href.split('?')[0] + '?t=' + Date.now();
          }, 5000);
        });
      }
    }
    
    // Tooltip для слайдеров
    function showTooltip(tooltipId, slider) {
      const tooltip = document.getElementById(tooltipId);
      tooltip.classList.add('visible');
      moveTooltip(tooltipId, slider);
    }
    
    function moveTooltip(tooltipId, slider) {
      const tooltip = document.getElementById(tooltipId);
      const percent = (slider.value - slider.min) / (slider.max - slider.min);
      const sliderWidth = slider.offsetWidth - 28; // 28 = thumb width
      const left = 14 + percent * sliderWidth; // 14 = half thumb
      tooltip.style.left = left + 'px';
      
      // Обновляем текст в зависимости от слайдера
      if (tooltipId === 'brightnessTooltip') {
        tooltip.textContent = slider.value == 0 ? 'Выкл' : slider.value + '%';
      } else if (tooltipId === 'demoSpeedTooltip') {
        const sec = parseInt(slider.value);
        if (sec < 60) {
          tooltip.textContent = sec + ' сек';
        } else {
          const min = Math.floor(sec / 60);
          const s = sec % 60;
          tooltip.textContent = s > 0 ? min + ' мин ' + s + ' сек' : min + ' мин';
        }
      } else if (tooltipId === 'smoothnessTooltip') {
        tooltip.textContent = slider.value;
      } else if (tooltipId === 'autoOffTooltip') {
        const v = parseInt(slider.value);
        if (v == 0) {
          tooltip.textContent = 'Выкл';
        } else if (v < 60) {
          tooltip.textContent = v + ' мин';
        } else {
          const h = Math.floor(v / 60);
          const m = v % 60;
          tooltip.textContent = m > 0 ? h + ' ч ' + m + ' мин' : h + (h == 1 ? ' час' : ' часа');
        }
      }
    }
    
    function hideTooltip(tooltipId) {
      document.getElementById(tooltipId).classList.remove('visible');
    }
    
    // Настройки
    function openSettings() {
      document.getElementById('settingsScreen').classList.add('open');
    }
    
    function closeSettings() {
      document.getElementById('settingsScreen').classList.remove('open');
    }
    
    // Модалка времени
    function openTimeModal() {
      hourPicker.setValue(alarmHours);
      minutePicker.setValue(alarmMinutes);
      document.getElementById('timeModal').classList.add('open');
    }
    
    function closeTimeModal() {
      document.getElementById('timeModal').classList.remove('open');
    }
    
    function saveTime() {
      alarmHours = hourPicker.getValue();
      alarmMinutes = minutePicker.getValue();
      
      document.getElementById('timeDisplay').textContent = 
        String(alarmHours).padStart(2, '0') + ':' + String(alarmMinutes).padStart(2, '0');
      
      closeTimeModal();
      sendData();
    }
    
    // Закрытие по клику на фон
    document.getElementById('timeModal').addEventListener('click', function(e) {
      if (e.target === this) {
        closeTimeModal();
      }
    });
    
    // Длительность рассвета
    function changeDuration(delta) {
      duration += delta;
      if (duration < 5) duration = 5;
      if (duration > 60) duration = 60;
      document.getElementById('durationValue').textContent = duration + ' мин';
      sendData();
    }
    
    // Плавность
    function updateSmoothness(val) {
      smoothness = parseInt(val);
      document.getElementById('smoothnessValue').textContent = smoothness;
    }
    
    // Автовыключение
    function updateAutoOff(val) {
      autoOffMinutes = parseInt(val);
      document.getElementById('autoOffValue').textContent = autoOffText(autoOffMinutes);
    }

    function autoOffText(v) {
      if (v === 0) return 'Выкл';
      if (v < 60) return v + ' мин';
      const h = Math.floor(v / 60), m = v % 60;
      return m > 0 ? h + ' ч ' + m + ' мин' : h + (h == 1 ? ' час' : ' часа');
    }

    function pad(n) {
      return String(n).padStart(2, '0');
    }

    // Подсветить выбранный пресет свечения
    function markLightPreset(name) {
      document.querySelectorAll('.color-preset').forEach(el => el.classList.remove('selected'));
      const el = document.querySelector('.preset-' + name);
      if (el) el.classList.add('selected');
    }

    // Показать свой цвет на кнопке "✎"
    function showCustomColor(color) {
      customColor = color;
      const btn = document.querySelector('.preset-custom');
      btn.style.background = color;
      btn.style.border = 'none';
      btn.textContent = '';
    }

    // Раздел "Информация"
    function updateInfo(e) {
      document.getElementById('wifiName').textContent = e.wifi ? e.wifi + ' ✓ (' + e.rssi + ' dBm)' : 'Нет соединения';
      document.getElementById('ipAddr').textContent = e.ip || '—';
      document.getElementById('devTime').textContent = e.time || '—';
      const src = {ntp: 'NTP ✓', rtc: 'Часы DS3231 (NTP нет)', none: 'НЕТ ВРЕМЕНИ - будильник не сработает'};
      document.getElementById('timeSrc').textContent = src[e.tsrc] || e.tsrc;
      document.getElementById('rtcStatus').textContent = e.rtc ? 'Подключены ✓' : 'Не найдены - проверь D1/D2';
      document.getElementById('snoozeInfo').textContent = e.snz ? 'Отложено до ' + e.snz : '';
    }
    
    // Скорость демо
    function updateDemoSpeed(val) {
      const sec = parseInt(val);
      let text;
      if (sec < 60) {
        text = sec + ' сек';
      } else {
        const min = Math.floor(sec / 60);
        const s = sec % 60;
        text = s > 0 ? min + ' мин ' + s + ' сек' : min + ' мин';
      }
      document.getElementById('demoSpeedValue').textContent = text;
    }
    
    // Кастомный градиент
    function updateCustomGradient() {
      const c1 = document.getElementById('color1').value;
      const c2 = document.getElementById('color2').value;
      const c3 = document.getElementById('color3').value;
      
      document.getElementById('gradientPreview').style.background = 
        `linear-gradient(90deg, ${c1} 0%, ${c2} 50%, ${c3} 100%)`;
    }
    
    // Демо
    function toggleDemo() {
      demoRunning = !demoRunning;
      const btn = document.getElementById('demoBtn');
      const icon = document.getElementById('demoBtnIcon');
      const text = document.getElementById('demoBtnText');
      const speed = parseInt(document.getElementById('demoSpeed').value);
      
      if (demoRunning) {
        // Передаём текущий выбранный пресет и цвета
        let demoUrl = '/demo?start=1&speed=' + speed + '&preset=' + selectedSunrisePreset;
        if (selectedSunrisePreset === 'custom') {
          const c1 = document.getElementById('color1').value.slice(1);
          const c2 = document.getElementById('color2').value.slice(1);
          const c3 = document.getElementById('color3').value.slice(1);
          demoUrl += '&c1=' + c1 + '&c2=' + c2 + '&c3=' + c3;
        }
        fetch(demoUrl);
        btn.classList.add('running');
        icon.textContent = '⏹';
        text.textContent = 'Остановить';
        
        // Показываем экран рассвета
        document.getElementById('sunriseScreen').classList.add('active');
        updateSunriseTime();
        
        let progress = 0;
        const step = 100 / (speed * 10);
        
        demoInterval = setInterval(() => {
          progress += step;
          if (progress >= 100) {
            progress = 100;
            stopDemo();
          }
          document.getElementById('brightness').value = progress;
          updateBrightnessLabel(progress);
        }, 100);
      } else {
        fetch('/demo?start=0');
        stopDemo();
      }
    }
    
    function stopDemo() {
      demoRunning = false;
      clearInterval(demoInterval);
      
      const btn = document.getElementById('demoBtn');
      btn.classList.remove('running');
      document.getElementById('demoBtnIcon').textContent = '▶';
      document.getElementById('demoBtnText').textContent = 'Запустить рассвет';
      
      // Скрываем экран рассвета
      document.getElementById('sunriseScreen').classList.remove('active');
      
      // Сбрасываем яркость в UI
      brightness = 0;
      document.getElementById('brightness').value = 0;
      updateBrightnessLabel(0);
    }
    
    function turnOffSunrise() {
      document.getElementById('brightness').value = 0;
      updateBrightnessLabel(0);
      document.getElementById('sunriseScreen').classList.remove('active');
      window.sunriseScreenBlocked = true;
      setTimeout(() => { window.sunriseScreenBlocked = false; }, 6000);
      fetch('/demo?start=0');
      fetch('/light?b=0').then(() => {
        stopDemo();
      });
    }
    
    function snoozeSunrise() {
      document.getElementById('brightness').value = 0;
      updateBrightnessLabel(0);
      document.getElementById('sunriseScreen').classList.remove('active');
      window.sunriseScreenBlocked = true;
      setTimeout(() => { window.sunriseScreenBlocked = false; }, 6000);
      fetch('/demo?start=0');
      fetch('/snooze').then(() => {
        stopDemo();
      });
    }
    
    function updateSunriseTime() {
      document.getElementById('sunriseTime').textContent = 
        String(alarmHours).padStart(2, '0') + ':' + String(alarmMinutes).padStart(2, '0');
    }
    
    // После будильника
    function selectAfterAlarm(el, value) {
      document.querySelectorAll('#afterAlarm .radio-item').forEach(r => r.classList.remove('selected'));
      el.classList.add('selected');
      afterAlarmValue = value;
      sendData();
    }
    
    // Инициализация
    document.addEventListener('DOMContentLoaded', () => {
      initPickers();
      updateCustomGradient();
      loadStatus();
    });
    
    // Fallback если DOMContentLoaded уже прошёл
    if (document.readyState !== 'loading') {
      initPickers();
      updateCustomGradient();
      loadStatus();
    }
    
    // Загрузка статуса с сервера
    function loadStatus() {
      fetch('/status').then(r => r.json()).then(e => {
        alarmHours = e.h;
        alarmMinutes = e.m;
        alarmEnabled = e.en;
        weekdaysBits = e.wd;
        duration = e.dur;
        selectedSunrisePreset = e.sp;
        brightness = e.b;
        selectedPreset = e.lp;
        afterAlarmValue = e.aa;
        
        document.getElementById('timeDisplay').textContent = 
          String(alarmHours).padStart(2, '0') + ':' + String(alarmMinutes).padStart(2, '0');
        
        document.getElementById('alarmToggle').classList.toggle('active', alarmEnabled);
        
        document.querySelectorAll('.weekday').forEach((el, i) => {
          el.classList.toggle('active', (weekdaysBits & (1 << i)) !== 0);
        });
        
        document.getElementById('durationValue').textContent = duration + ' мин';
        document.getElementById('brightness').value = brightness;
        document.getElementById('brightnessLabel').textContent = brightness === 0 ? 'Выключено' : brightness + '%';
        
        document.querySelectorAll('.sunrise-preset').forEach(el => {
          el.classList.toggle('selected', el.dataset.preset === selectedSunrisePreset);
        });
        
        markLightPreset(selectedPreset);
        if (e.c) showCustomColor('#' + e.c.toLowerCase());

        if (e.c1) {
          document.getElementById('color1').value = '#' + e.c1;
          document.getElementById('color2').value = '#' + e.c2;
          document.getElementById('color3').value = '#' + e.c3;
          updateCustomGradient();
        }

        smoothness = e.smooth;
        document.getElementById('smoothness').value = smoothness;
        document.getElementById('smoothnessValue').textContent = smoothness;

        autoOffMinutes = e.aoff;
        document.getElementById('autoOff').value = autoOffMinutes;
        document.getElementById('autoOffValue').textContent = autoOffText(autoOffMinutes);

        updateInfo(e);
        document.getElementById('sunriseScreen').classList.toggle('active', e.sunrise);
        updateSunriseTime();
      }).catch(err => console.error('status:', err));
    }
    
    // Периодический опрос статуса (каждые 5 сек): состояние будильника и всё, что могло
    // измениться с другого устройства
    setInterval(() => {
      fetch('/status').then(r => r.json()).then(e => {
        alarmHours = e.h;
        alarmMinutes = e.m;
        document.getElementById('timeDisplay').textContent = pad(e.h) + ':' + pad(e.m);

        if (!window.sunriseScreenBlocked) {
          document.getElementById('sunriseScreen').classList.toggle('active', e.sunrise);
        }
        if (e.sunrise) updateSunriseTime();

        const slider = document.getElementById('brightness');
        if (Math.abs(slider.value - e.b) > 1) {
          slider.value = e.b;
          updateBrightnessLabel(e.b);
        }

        alarmEnabled = e.en;
        document.getElementById('alarmToggle').classList.toggle('active', e.en);

        weekdaysBits = e.wd;
        document.querySelectorAll('.weekday').forEach((el, i) => {
          el.classList.toggle('active', (e.wd & (1 << i)) !== 0);
        });

        if (e.lp !== selectedPreset) {
          selectedPreset = e.lp;
          markLightPreset(e.lp);
        }
        if (e.c && '#' + e.c.toLowerCase() !== customColor) showCustomColor('#' + e.c.toLowerCase());

        if (e.aoff !== autoOffMinutes) {
          autoOffMinutes = e.aoff;
          document.getElementById('autoOff').value = autoOffMinutes;
          document.getElementById('autoOffValue').textContent = autoOffText(autoOffMinutes);
        }

        updateInfo(e);
      }).catch(() => {});
    }, 5000);
  </script>
</body>
</html>
)rawliteral";

const char MANIFEST[] PROGMEM = R"({"name":"Рассвет","short_name":"Рассвет","start_url":"/","display":"standalone","background_color":"#1a1a2e","theme_color":"#1a1a2e","icons":[{"src":"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>☀️</text></svg>","sizes":"any","type":"image/svg+xml"}]})";

// ===== ВРЕМЯ =====
// Вся логика будильника работает с "локальным эпохом": секунды от 1970 в местном времени (UTC + tzOffset).
// Источник: системное время ESP (его держит NTP), если его нет - DS3231. RTC хранит локальное время, как и раньше.

bool rtcTimeValid() {
  if (!rtcFound) return false;
  DateTime t = rtc.now();
  return t.isValid() && t.year() >= 2024 && t.year() <= 2099;
}

bool getLocalEpoch(long& localEpoch) {
  time_t now = time(nullptr);
  if (now > MIN_VALID_EPOCH) {
    localEpoch = (long)now + tzOffset;
    return true;
  }
  if (rtcTimeValid()) {
    localEpoch = (long)rtc.now().unixtime();
    return true;
  }
  return false;
}

const char* timeSource() {
  if (time(nullptr) > MIN_VALID_EPOCH) return ntpSynced ? "ntp" : "rtc"; // системное время могло прийти из RTC при старте
  if (rtcTimeValid()) return "rtc";
  return "none";
}

void formatLocalTime(char* buf, size_t len) {
  long le;
  if (!getLocalEpoch(le)) { snprintf(buf, len, "--:--:--"); return; }
  long s = le % 86400;
  snprintf(buf, len, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
}

// Строка лога: в Serial и в кольцевой буфер для /log. Формат printf, время подставляется само
void logMsg(const char* fmt, ...) {
  char* line = logBuf[logHead];
  formatLocalTime(line, 10);
  line[8] = ' ';
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + 9, LOG_LINE_LEN - 9, fmt, ap);
  va_end(ap);
  Serial.println(line);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

// Колбэк settimeofday: вызывается и когда время пришло от NTP, и когда мы сами ставим его из RTC
void onTimeSet(bool fromSntp) {
  if (!fromSntp) return;
  ntpSynced = true;
  rtcWritePending = true;
}

void setupTime() {
  Wire.begin(D2, D1);
  rtcFound = rtc.begin();
  if (!rtcFound) {
    logMsg("RTC: DS3231 не найден - проверь D1 (SCL), D2 (SDA), питание. Будильник будет работать только при наличии NTP");
  } else if (rtc.lostPower()) {
    logMsg("RTC: часы сбрасывались (батарейка?), время в них недостоверно до синхронизации NTP");
  }

  // SNTP стартует сразу и сам дождётся сети, ждать WiFi не нужно
  configTime(tzOffset, 0, ntpServer1, ntpServer2, ntpServer3);
  settimeofday_cb(onTimeSet);

  // Пока NTP нет - берём время из RTC, чтобы будильник работал без сети
  if (rtcTimeValid()) {
    timeval tv = { (time_t)(rtc.now().unixtime() - tzOffset), 0 };
    settimeofday(&tv, nullptr);
    logMsg("RTC: время взято из DS3231");
  }
}

// ===== WiFi =====
// Ничего не ждём и не перезагружаем ESP: ядро переподключается само, будильник от сети не зависит

void setupWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname("sunrise");
  WiFi.setAutoReconnect(true);

  wifiGotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e) {
    wifiJustConnected = true;
  });
  wifiDisconnectedHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 30000) {
      lastLog = millis();
      logMsg("WiFi: нет соединения (причина %d), переподключение автоматическое", e.reason);
    }
  });

  WiFi.begin(ssid, password);
  logMsg("WiFi: подключаюсь к \"%s\" в фоне", ssid);
}

void handleWiFi() {
  if (wifiJustConnected) {
    wifiJustConnected = false;
    logMsg("WiFi: подключено, IP %s", WiFi.localIP().toString().c_str());
    if (!mdnsStarted) {
      mdnsStarted = MDNS.begin("sunrise");
      if (mdnsStarted) logMsg("mDNS: http://sunrise.local");
    } else {
      MDNS.notifyAPChange();
    }
    if (!otaStarted) {
      setupOTA();
      otaStarted = true;
    }
  }

  // Страховка: если ядро 5 минут не может подключиться - начинаем заново
  static unsigned long lastConnected = 0;
  if (WiFi.status() == WL_CONNECTED) {
    lastConnected = millis();
  } else if (millis() - lastConnected > 300000UL) {
    lastConnected = millis();
    logMsg("WiFi: 5 минут без сети, перезапуск подключения");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
}

// ===== OTA: прошивка по WiFi =====
// arduino-cli upload -p <ip или sunrise.local> --fqbn esp8266:esp8266:nodemcuv2 ~/Arduino/sunrise_alarm_v3
// Пароль - otaPassword из secrets.h. Во время прошивки лента гаснет, настройки в EEPROM сохраняются

void setupOTA() {
  ArduinoOTA.setHostname("sunrise");
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.onStart([]() {
    demoActive = false;
    phase = PHASE_IDLE;
    renderOff();
    if (settingsDirty) saveToEEPROM();
    logMsg("OTA: приём прошивки");
  });
  ArduinoOTA.onEnd([]() { logMsg("OTA: готово, перезагрузка"); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static unsigned int lastPct = 100;
    unsigned int pct = done * 100 / total;
    if (pct / 10 != lastPct / 10) { lastPct = pct; Serial.printf("OTA: %u%%\n", pct); }
  });
  ArduinoOTA.onError([](ota_error_t e) { logMsg("OTA: ошибка %u", e); });
  ArduinoOTA.begin();
  logMsg("OTA: включено, хост sunrise");
}

// ===== ВЫВОД НА ЛЕНТУ =====
// Вся лента одного цвета. Яркость считаем в 16 бит с гаммой 2.2, младший байт распределяем по ленте
// (часть диодов на один уровень выше) - рассвет без ступеней 1-2-3 в первые минуты.
// show() вызывается только когда кадр реально изменился: во время show() на 180 LED прерывания
// выключены ~5.4 мс, и лишние вызовы мешают WiFi.

uint16_t gamma16[256];
uint8_t ditherRank[LED_COUNT]; // порядок, в котором диоды получают "+1": равномерно по ленте, узор не движется
uint32_t lastR16 = 0xFFFFFFFF, lastG16 = 0xFFFFFFFF, lastB16 = 0xFFFFFFFF; // последний выведенный кадр
bool stripIsOff = false;

void buildGamma() {
  for (int i = 0; i < 256; i++) {
    gamma16[i] = (uint16_t)(pow(i / 255.0, 2.2) * 65535.0 + 0.5);
  }
  // Порядок дизеринга - перестановка индексов с обращением битов: соседние по рангу диоды
  // максимально далеко друг от друга, при росте яркости новые добавляются между уже горящими
  uint8_t rank = 0;
  for (int v = 0; v < 256; v++) {
    int idx = 0;
    for (int b = 0; b < 8; b++) if (v & (1 << b)) idx |= 1 << (7 - b);
    if (idx < LED_COUNT) ditherRank[idx] = rank++;
  }
}

void renderOff() {
  if (stripIsOff) return;
  strip.clear();
  strip.show();
  stripIsOff = true;
  lastR16 = lastG16 = lastB16 = 0xFFFFFFFF;
}

// r,g,b 0-255 - цвет; level 0.0-1.0 - воспринимаемая яркость
void renderColor(uint8_t r, uint8_t g, uint8_t b, float level) {
  if (level <= 0.0f) { renderOff(); return; }
  if (level > 1.0f) level = 1.0f;

  float lin = pow(level, 2.2) * (POWER_LIMIT / 255.0);

  // Пол: самый яркий канал не опускается ниже уровня 1 на всей ленте. Иначе в начале рассвета
  // горели бы отдельные диоды на голой ленте - выглядит как бегающие точки, а не как рассвет
  uint16_t maxG = max(gamma16[r], max(gamma16[g], gamma16[b]));
  if (maxG > 0 && lin * maxG < 256.0f) lin = 256.0f / maxG;

  uint32_t r16 = gamma16[r] * lin;
  uint32_t g16 = gamma16[g] * lin;
  uint32_t b16 = gamma16[b] * lin;
  if (r16 == lastR16 && g16 == lastG16 && b16 == lastB16) return;
  lastR16 = r16; lastG16 = g16; lastB16 = b16;
  stripIsOff = false;

  // Сколько диодов получают уровень +1 по каждому каналу (из младшего байта)
  uint16_t kr = ((r16 & 0xFF) * LED_COUNT) >> 8;
  uint16_t kg = ((g16 & 0xFF) * LED_COUNT) >> 8;
  uint16_t kb = ((b16 & 0xFF) * LED_COUNT) >> 8;
  uint8_t rb = r16 >> 8, gb = g16 >> 8, bb = b16 >> 8;

  for (uint16_t i = 0; i < LED_COUNT; i++) {
    uint8_t rank = ditherRank[i];
    uint8_t ro = rb + (rb < 255 && rank < kr ? 1 : 0);
    uint8_t go = gb + (gb < 255 && rank < kg ? 1 : 0);
    uint8_t bo = bb + (bb < 255 && rank < kb ? 1 : 0);
    strip.setPixelColor(i, ro, go, bo);
  }
  strip.show();
}

void renderRainbow(float level) {
  static uint16_t j = 0;
  j += 256;
  float lin = pow(level, 2.2) * (POWER_LIMIT / 255.0);
  for (uint16_t i = 0; i < LED_COUNT; i++) {
    uint32_t c = strip.gamma32(strip.ColorHSV(((uint32_t)i * 65536 / LED_COUNT + j) & 0xFFFF));
    uint8_t r = ((c >> 16) & 0xFF) * lin;
    uint8_t g = ((c >> 8) & 0xFF) * lin;
    uint8_t b = (c & 0xFF) * lin;
    strip.setPixelColor(i, r, g, b);
  }
  strip.show();
  stripIsOff = false;
  lastR16 = lastG16 = lastB16 = 0xFFFFFFFF; // следующий одноцветный кадр должен отрисоваться
}

// ===== ЦВЕТА =====

uint32_t lerpColor(uint32_t c1, uint32_t c2, float t) {
  int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
  int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
  uint8_t r = r1 + (r2 - r1) * t;
  uint8_t g = g1 + (g2 - g1) * t;
  uint8_t b = b1 + (b2 - b1) * t;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Градиенты рассвета: тёмно-красный -> оранжевый -> тёплый белый
void getSunriseGradient(const String& preset, const uint32_t* custom, uint32_t* colors) {
  if (preset == "warm") {
    colors[0] = 0x881100; colors[1] = 0xFF5500; colors[2] = 0xFFDAAA;
  } else if (preset == "soft") {
    colors[0] = 0x663300; colors[1] = 0xFF7722; colors[2] = 0xFFEEDD;
  } else if (preset == "custom") {
    colors[0] = custom[0]; colors[1] = custom[1]; colors[2] = custom[2];
  } else { // classic
    colors[0] = 0x990000; colors[1] = 0xFF4400; colors[2] = 0xFFE4C4;
  }
}

// Целевой цвет и яркость для прогресса 0..1 (плавный переход к ним делается в loop)
void setSunriseTarget(float progress, const String& preset, const uint32_t* custom) {
  uint32_t colors[3];
  getSunriseGradient(preset, custom, colors);
  uint32_t color = progress < 0.5
    ? lerpColor(colors[0], colors[1], progress * 2)
    : lerpColor(colors[1], colors[2], (progress - 0.5) * 2);
  tgtR = (color >> 16) & 0xFF;
  tgtG = (color >> 8) & 0xFF;
  tgtB = color & 0xFF;
  tgtBr = progress * 100.0f; // воспринимаемая яркость линейно по времени, гамму накладывает renderColor
}

// Начальный кадр рассвета: первый цвет градиента на нулевой яркости
void resetSunriseFrame(const String& preset, const uint32_t* custom) {
  uint32_t colors[3];
  getSunriseGradient(preset, custom, colors);
  curR = tgtR = (colors[0] >> 16) & 0xFF;
  curG = tgtG = (colors[0] >> 8) & 0xFF;
  curB = tgtB = colors[0] & 0xFF;
  curBr = 0;
  tgtBr = 1;
}

uint32_t lightPresetColor() {
  if (lightPreset == "orange") return 0xFF5500;
  if (lightPreset == "yellow") return 0xFFFF00;
  if (lightPreset == "white")  return 0xFFFFFF;
  if (lightPreset == "custom") return customColor;
  return 0xFF0000; // red
}

// ===== EEPROM =====

void writeColor(int addr, uint32_t c) {
  EEPROM.write(addr, (c >> 16) & 0xFF);
  EEPROM.write(addr + 1, (c >> 8) & 0xFF);
  EEPROM.write(addr + 2, c & 0xFF);
}

uint32_t readColor(int addr) {
  return ((uint32_t)EEPROM.read(addr) << 16) | ((uint32_t)EEPROM.read(addr + 1) << 8) | EEPROM.read(addr + 2);
}

void writeInt32(int addr, int32_t v) {
  for (int i = 0; i < 4; i++) EEPROM.write(addr + i, (v >> (8 * i)) & 0xFF);
}

int32_t readInt32(int addr) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= (uint32_t)EEPROM.read(addr + i) << (8 * i);
  return (int32_t)v;
}

void markSettingsDirty() {
  settingsDirty = true;
  settingsDirtySince = millis();
}

void saveToEEPROM() {
  EEPROM.write(ADDR_HOUR, alarmHour);
  EEPROM.write(ADDR_MINUTE, alarmMinute);
  EEPROM.write(ADDR_ENABLED, alarmEnabled ? 1 : 0);
  EEPROM.write(ADDR_WEEKDAYS, weekdays);
  EEPROM.write(ADDR_DURATION, sunriseDuration);

  uint8_t spIdx = 0;
  if (sunrisePreset == "warm") spIdx = 1;
  else if (sunrisePreset == "soft") spIdx = 2;
  else if (sunrisePreset == "custom") spIdx = 3;
  EEPROM.write(ADDR_SUNRISE_PRESET, spIdx);

  uint8_t lpIdx = 0;
  if (lightPreset == "orange") lpIdx = 1;
  else if (lightPreset == "yellow") lpIdx = 2;
  else if (lightPreset == "white") lpIdx = 3;
  else if (lightPreset == "rainbow") lpIdx = 4;
  else if (lightPreset == "custom") lpIdx = 5;
  EEPROM.write(ADDR_LIGHT_PRESET, lpIdx);
  writeColor(ADDR_CUSTOM_COLOR, customColor);

  uint8_t aaIdx = 1;
  if (afterAlarm == "keep") aaIdx = 0;
  else if (afterAlarm == "60") aaIdx = 2;
  EEPROM.write(ADDR_AFTER_ALARM, aaIdx);

  EEPROM.write(ADDR_SMOOTHNESS, smoothness);
  EEPROM.write(ADDR_AUTO_OFF, autoOffMinutes);

  writeColor(ADDR_SUNRISE_C1, sunriseColors[0]);
  writeColor(ADDR_SUNRISE_C2, sunriseColors[1]);
  writeColor(ADDR_SUNRISE_C3, sunriseColors[2]);

  writeInt32(ADDR_DISMISSED_KEY, dismissedKey);
  EEPROM.write(ADDR_MAGIC, MAGIC_VALUE);

  EEPROM.commit();
  settingsDirty = false;
}

// Отложенная запись: commit не чаще чем через EEPROM_COMMIT_DELAY после последнего изменения
void handleEepromCommit() {
  if (settingsDirty && millis() - settingsDirtySince > EEPROM_COMMIT_DELAY) {
    saveToEEPROM();
    logMsg("EEPROM: настройки сохранены");
  }
}

void resetToDefaults();

void loadFromEEPROM() {
  if (EEPROM.read(ADDR_MAGIC) != MAGIC_VALUE) {
    logMsg("EEPROM: первая загрузка, настройки по умолчанию");
    resetToDefaults();
    return;
  }

  alarmHour = EEPROM.read(ADDR_HOUR);
  alarmMinute = EEPROM.read(ADDR_MINUTE);
  alarmEnabled = EEPROM.read(ADDR_ENABLED) == 1;
  weekdays = EEPROM.read(ADDR_WEEKDAYS);
  sunriseDuration = EEPROM.read(ADDR_DURATION);

  uint8_t spIdx = EEPROM.read(ADDR_SUNRISE_PRESET);
  sunrisePreset = spIdx == 1 ? "warm" : spIdx == 2 ? "soft" : spIdx == 3 ? "custom" : "classic";

  uint8_t lpIdx = EEPROM.read(ADDR_LIGHT_PRESET);
  const char* lps[] = {"red", "orange", "yellow", "white", "rainbow", "custom"};
  lightPreset = lpIdx < 6 ? lps[lpIdx] : "red";
  customColor = readColor(ADDR_CUSTOM_COLOR);

  uint8_t aaIdx = EEPROM.read(ADDR_AFTER_ALARM);
  afterAlarm = aaIdx == 0 ? "keep" : aaIdx == 2 ? "60" : "30";

  smoothness = EEPROM.read(ADDR_SMOOTHNESS);
  autoOffMinutes = EEPROM.read(ADDR_AUTO_OFF);

  sunriseColors[0] = readColor(ADDR_SUNRISE_C1);
  sunriseColors[1] = readColor(ADDR_SUNRISE_C2);
  sunriseColors[2] = readColor(ADDR_SUNRISE_C3);

  dismissedKey = readInt32(ADDR_DISMISSED_KEY); // 0xFFFFFFFF на чистой флеш = -1 = ничего не выключено

  // Валидация (0 дней недели - допустимо, пользователь так решил)
  if (alarmHour > 23) alarmHour = 7;
  if (alarmMinute > 59) alarmMinute = 30;
  if (sunriseDuration < 5 || sunriseDuration > 60) sunriseDuration = 20;
  if (weekdays > 127) weekdays = 0b00011111;
  if (smoothness > 10) smoothness = 5;
  if (autoOffMinutes > 120) autoOffMinutes = 5;
}

void resetToDefaults() {
  phase = PHASE_IDLE;
  demoActive = false;
  snoozeUntil = 0;
  activeKey = -1;
  dismissedKey = -1;
  curR = curG = curB = curBr = 0;
  tgtR = tgtG = tgtB = tgtBr = 0;

  alarmHour = 7;
  alarmMinute = 30;
  alarmEnabled = true;
  weekdays = 0b00011111;
  sunriseDuration = 20;
  sunrisePreset = "classic";
  afterAlarm = "30";
  sunriseColors[0] = 0x990000;
  sunriseColors[1] = 0xFF4400;
  sunriseColors[2] = 0xFFE4C4;

  brightness = 0;
  currentBrightness = 0;
  lightPreset = "red";
  customColor = 0xFF5500;

  smoothness = 5;
  autoOffMinutes = 5;

  saveToEEPROM();
  renderOff();
}

// ===== БУДИЛЬНИК =====
// Запуск будильника = конкретная дата + время. Ключ: день от 1970 (локальный) * 1440 + минута будильника.
// По ключу помним, какой запуск пользователь уже выключил, чтобы он не включался снова в том же окне.

int32_t occurrenceKey(long day) {
  return (int32_t)(day * 1440L + alarmHour * 60L + alarmMinute);
}

bool weekdayAllowed(long day) {
  int dow = (day + 3) % 7; // 1970-01-01 был четверг: день 0 -> 3 (0=пн ... 6=вс)
  return weekdays & (1 << dow);
}

// Ищем запуск, в окно которого попадает "сейчас": вчерашний, сегодняшний или завтрашний
// (рассвет через полночь относится к дню будильника, и день недели проверяется по нему)
AlarmWindow findWindow(long nowLocal) {
  AlarmWindow w = {false, false, 0, -1};
  if (!alarmEnabled) return w;

  long today = nowLocal / 86400L;
  long alarmSec = alarmHour * 3600L + alarmMinute * 60L;
  long durSec = sunriseDuration * 60L;

  for (long day = today - 1; day <= today + 1; day++) {
    if (!weekdayAllowed(day)) continue;
    long alarmAt = day * 86400L + alarmSec;
    long start = alarmAt - durSec;
    long holdEnd = alarmAt + 5 * 60; // ещё 5 минут после времени будильника считаем его сработавшим

    if (nowLocal >= start && nowLocal < alarmAt) {
      w.active = true; w.inSunrise = true;
      w.progress = (float)(nowLocal - start) / durSec;
      w.key = occurrenceKey(day);
      return w;
    }
    if (nowLocal >= alarmAt && nowLocal < holdEnd) {
      w.active = true; w.inSunrise = false;
      w.progress = 1.0f;
      w.key = occurrenceKey(day);
      return w;
    }
  }
  return w;
}

void logTime(const char* msg) {
  logMsg("%s", msg);
}

void enterSunrise(const AlarmWindow& w) {
  phase = PHASE_SUNRISE;
  activeKey = w.key;
  sunriseProgress = w.progress;
  resetSunriseFrame(sunrisePreset, sunriseColors);
  logTime("Рассвет начался");
}

void enterHold(int32_t key) {
  if (phase != PHASE_SUNRISE) {
    // Пришли не из рассвета (загрузка в окне, конец snooze) - стартуем с нуля, яркость плавно поднимется
    resetSunriseFrame(sunrisePreset, sunriseColors);
  }
  phase = PHASE_HOLD;
  activeKey = key;
  sunriseProgress = 1.0f;
  holdStartMillis = millis();
  logTime("Будильник: время наступило, держим свет");
}

// Будильник погашен сам (таймаут, выключили тумблером, день стал неактивным)
void stopAlarm(const char* why) {
  if (phase == PHASE_IDLE) return;
  phase = PHASE_IDLE;
  activeKey = -1;
  curR = curG = curB = curBr = 0;
  brightness = 0;
  currentBrightness = 0;
  renderOff();
  logTime(why);
}

// Пользователь выключил будильник: этот запуск больше не включаем, помним это и после перезагрузки
void dismissAlarm() {
  if (phase == PHASE_IDLE && snoozeUntil == 0) return;
  if (activeKey >= 0 && activeKey != dismissedKey) {
    dismissedKey = activeKey;
    writeInt32(ADDR_DISMISSED_KEY, dismissedKey);
    EEPROM.commit(); // редкое событие, пишем сразу
  }
  snoozeUntil = 0;
  stopAlarm("Будильник выключен пользователем");
}

// Отложить на 5 минут: гасим, помечаем текущий запуск выключенным, через 5 минут включаем удержание
bool snoozeAlarm() {
  long nowLocal;
  if (!getLocalEpoch(nowLocal)) return false;
  if (phase == PHASE_IDLE && snoozeUntil == 0) return false;

  if (activeKey >= 0 && activeKey != dismissedKey) {
    dismissedKey = activeKey;
    writeInt32(ADDR_DISMISSED_KEY, dismissedKey);
    EEPROM.commit();
  }
  int32_t key = activeKey;
  stopAlarm("Будильник отложен на 5 минут");
  activeKey = key; // нужен, чтобы по окончании snooze удержание можно было выключить тем же ключом
  snoozeUntil = nowLocal + 5 * 60;
  return true;
}

void checkAlarm() {
  long nowLocal;
  if (!getLocalEpoch(nowLocal)) {
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 60000) {
      lastWarn = millis();
      logMsg("Будильник: нет источника времени (ни NTP, ни RTC) - ждём");
    }
    return;
  }

  // Отложенный будильник
  if (snoozeUntil > 0 && nowLocal >= snoozeUntil) {
    snoozeUntil = 0;
    if (!alarmEnabled) { activeKey = -1; return; }
    enterHold(activeKey);
    return;
  }

  if (phase == PHASE_HOLD) {
    if (!alarmEnabled) { stopAlarm("Будильник выключен тумблером"); return; }
    if (afterAlarm == "keep") return;
    unsigned long timeout = (afterAlarm == "60" ? 60UL : 30UL) * 60UL * 1000UL;
    if (millis() - holdStartMillis > timeout) stopAlarm("Свет выключен по таймеру после будильника");
    return;
  }

  AlarmWindow w = findWindow(nowLocal);

  if (phase == PHASE_SUNRISE) {
    if (!w.active || w.key != activeKey) { stopAlarm("Рассвет прерван: будильник выключен или время изменилось"); return; }
    if (!w.inSunrise) { enterHold(w.key); return; }
    sunriseProgress = w.progress; // длительность могла измениться - прогресс пересчитан
    return;
  }

  // PHASE_IDLE
  if (!w.active || w.key == dismissedKey || snoozeUntil > 0) return;

  if (brightness > 0 || currentBrightness > 0) {
    // Свет уже включён руками - человек не спит, этот запуск пропускаем целиком
    dismissedKey = w.key;
    writeInt32(ADDR_DISMISSED_KEY, dismissedKey);
    EEPROM.commit();
    logTime("Будильник пропущен: свет уже включён вручную");
    return;
  }

  if (w.inSunrise) enterSunrise(w);
  else enterHold(w.key);
}

// ===== HTTP HANDLERS =====

bool argInt(const char* name, int lo, int hi, int& out) {
  if (!server.hasArg(name)) return false;
  long v = server.arg(name).toInt();
  if (v < lo || v > hi) return false;
  out = (int)v;
  return true;
}

bool argColor(const char* name, uint32_t& out) {
  if (!server.hasArg(name)) return false;
  String s = server.arg(name);
  if (s.length() < 1 || s.length() > 6) return false;
  out = strtoul(s.c_str(), nullptr, 16) & 0xFFFFFF;
  return true;
}

bool argOneOf(const char* name, const char* const* allowed, int n, String& out) {
  if (!server.hasArg(name)) return false;
  String v = server.arg(name);
  for (int i = 0; i < n; i++) if (v == allowed[i]) { out = v; return true; }
  return false;
}

const char* const SUNRISE_PRESETS[] = {"classic", "warm", "soft", "custom"};
const char* const LIGHT_PRESETS[] = {"red", "orange", "yellow", "white", "rainbow", "custom"};
const char* const AFTER_ALARM_VALUES[] = {"keep", "30", "60"};

void handleRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleManifest() {
  server.send_P(200, "application/json", MANIFEST);
}

void handleStatus() {
  char timeBuf[12];
  formatLocalTime(timeBuf, sizeof timeBuf);

  char snoozeBuf[8] = "";
  if (snoozeUntil > 0) {
    long s = snoozeUntil % 86400;
    snprintf(snoozeBuf, sizeof snoozeBuf, "%02ld:%02ld", s / 3600, (s / 60) % 60);
  }

  const char* phaseName = demoActive ? "demo" : phase == PHASE_SUNRISE ? "sunrise" : phase == PHASE_HOLD ? "hold" : "idle";
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  static char json[640];
  snprintf(json, sizeof json,
    "{\"h\":%d,\"m\":%d,\"en\":%s,\"wd\":%u,\"dur\":%d,\"sp\":\"%s\","
    "\"b\":%d,\"lp\":\"%s\",\"c\":\"%06X\",\"aa\":\"%s\","
    "\"c1\":\"%06X\",\"c2\":\"%06X\",\"c3\":\"%06X\",\"smooth\":%d,\"aoff\":%d,"
    "\"sunrise\":%s,\"phase\":\"%s\",\"snz\":\"%s\","
    "\"wifi\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ntp\":%s,\"rtc\":%s,\"tsrc\":\"%s\",\"time\":\"%s\",\"up\":%lu}",
    alarmHour, alarmMinute, alarmEnabled ? "true" : "false", weekdays, sunriseDuration, sunrisePreset.c_str(),
    brightness, lightPreset.c_str(), customColor, afterAlarm.c_str(),
    sunriseColors[0], sunriseColors[1], sunriseColors[2], smoothness, autoOffMinutes,
    (phase != PHASE_IDLE || demoActive) ? "true" : "false", phaseName, snoozeBuf,
    wifiOk ? ssid : "", wifiOk ? WiFi.localIP().toString().c_str() : "", wifiOk ? WiFi.RSSI() : 0,
    ntpSynced ? "true" : "false", rtcFound ? "true" : "false", timeSource(), timeBuf, millis() / 1000);

  server.send(200, "application/json", json);
}

void handleSave() {
  argInt("h", 0, 23, alarmHour);
  argInt("m", 0, 59, alarmMinute);
  int en;
  if (argInt("en", 0, 1, en)) alarmEnabled = en == 1;
  int wd;
  if (argInt("wd", 0, 127, wd)) weekdays = wd;
  argInt("dur", 5, 60, sunriseDuration);
  argOneOf("sp", SUNRISE_PRESETS, 4, sunrisePreset);
  argOneOf("aa", AFTER_ALARM_VALUES, 3, afterAlarm);
  argInt("smooth", 0, 10, smoothness);
  argInt("aoff", 0, 120, autoOffMinutes);
  argColor("c1", sunriseColors[0]);
  argColor("c2", sunriseColors[1]);
  argColor("c3", sunriseColors[2]);

  markSettingsDirty();
  server.send(200, "text/plain", "OK");
}

void handleLight() {
  lastInteractionTime = millis();

  String newPreset;
  bool presetChanged = argOneOf("p", LIGHT_PRESETS, 6, newPreset) && newPreset != lightPreset;
  uint32_t newColor;
  bool colorChanged = argColor("c", newColor) && newColor != customColor;
  int newBrightness;
  bool hasBrightness = argInt("b", 0, 100, newBrightness);

  // Любое ручное действие со светом во время будильника - будильник выключен
  if (phase != PHASE_IDLE || snoozeUntil > 0) {
    if (hasBrightness || presetChanged) dismissAlarm();
  }
  if (demoActive && hasBrightness) {
    demoActive = false;
    curR = curG = curB = curBr = 0;
  }

  if (presetChanged) { lightPreset = newPreset; markSettingsDirty(); }
  if (colorChanged) { customColor = newColor; markSettingsDirty(); }
  if (hasBrightness) {
    brightness = newBrightness;
    if (brightness == 0) {
      currentBrightness = 0;
      renderOff();
    }
  }

  server.send(200, "text/plain", "OK");
}

void handleDemo() {
  int start;
  if (!argInt("start", 0, 1, start)) { server.send(400, "text/plain", "start=0|1"); return; }

  if (start == 1) {
    if (phase != PHASE_IDLE || snoozeUntil > 0) dismissAlarm();

    // Демо показывает переданный пресет/цвета, не трогая настройки будильника
    demoPreset = sunrisePreset;
    argOneOf("preset", SUNRISE_PRESETS, 4, demoPreset);
    demoColors[0] = sunriseColors[0];
    demoColors[1] = sunriseColors[1];
    demoColors[2] = sunriseColors[2];
    argColor("c1", demoColors[0]);
    argColor("c2", demoColors[1]);
    argColor("c3", demoColors[2]);
    if (!argInt("speed", 5, 600, demoSpeed)) demoSpeed = 30;

    brightness = 0;
    currentBrightness = 0;
    resetSunriseFrame(demoPreset, demoColors);
    demoActive = true;
    demoStartTime = millis();
  } else {
    // Только останавливает демо. Будильник выключается через /light?b=0, откладывается через /snooze -
    // UI шлёт эти запросы вместе с /demo?start=0, и их порядок не должен иметь значения
    if (demoActive) {
      demoActive = false;
      brightness = 0;
      currentBrightness = 0;
      curR = curG = curB = curBr = 0;
      renderOff();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSnooze() {
  server.send(200, "text/plain", snoozeAlarm() ? "OK" : "NOTHING_TO_SNOOZE");
}

void handleReset() {
  resetToDefaults();
  server.send(200, "text/plain", "OK");
  delay(500); // отдать ответ
  ESP.restart();
}

void handleLog() {
  String out;
  out.reserve(logCount * 60 + 160);
  out += "Рассвет-будильник v3\n";
  out += "Аптайм: " + String(millis() / 60000) + " мин, свободно памяти: " + String(ESP.getFreeHeap()) + " байт\n";
  out += "Фаза: " + String(phase == PHASE_IDLE ? "ожидание" : phase == PHASE_SUNRISE ? "рассвет" : "удержание");
  out += ", источник времени: " + String(timeSource()) + "\n\n";
  uint8_t start = (logHead + LOG_LINES - logCount) % LOG_LINES;
  for (uint8_t i = 0; i < logCount; i++) {
    out += logBuf[(start + i) % LOG_LINES];
    out += '\n';
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

// ===== SETUP =====

void setup() {
  Serial.begin(115200);
  Serial.println("\n");
  logMsg("Рассвет-будильник v3, причина перезагрузки: %s", ESP.getResetReason().c_str());

  EEPROM.begin(EEPROM_SIZE);
  loadFromEEPROM();

  buildGamma();
  strip.begin();
  // При загрузке ESP8266 выводит на GPIO2 (D4) служебные байты по UART, и первый светодиод
  // принимает их как цвет. Даём ленте паузу, чтобы она сбросила приём, и очищаем дважды
  delay(5);
  strip.clear();
  strip.show();
  delay(5);
  renderOff();

  setupTime();
  setupWiFi();

  server.on("/", handleRoot);
  server.on("/manifest.json", handleManifest);
  server.on("/status", handleStatus);
  server.on("/save", handleSave);
  server.on("/light", handleLight);
  server.on("/demo", handleDemo);
  server.on("/snooze", handleSnooze);
  server.on("/reset", handleReset);
  server.on("/log", handleLog);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  logMsg("Будильник %02d:%02d, %s, маска дней %u, рассвет %d мин, источник времени: %s",
    alarmHour, alarmMinute, alarmEnabled ? "включён" : "выключен",
    (unsigned)weekdays, sunriseDuration, timeSource());
  logMsg("Сервер запущен");
}

// ===== LOOP =====

// Шаг к целевому значению с ограничением скорости
inline void approach(float& cur, float tgt, float step) {
  if (cur < tgt) { cur += step; if (cur > tgt) cur = tgt; }
  else if (cur > tgt) { cur -= step; if (cur < tgt) cur = tgt; }
}

void loop() {
  server.handleClient();
  if (mdnsStarted) MDNS.update();
  if (otaStarted) ArduinoOTA.handle();
  handleWiFi();
  handleEepromCommit();

  if (rtcWritePending) {
    rtcWritePending = false;
    if (rtcFound) rtc.adjust(DateTime((uint32_t)(time(nullptr) + tzOffset)));
    logMsg("NTP: время получено%s", rtcFound ? ", записано в DS3231" : "");
  }

  // Проверка будильника - раз в секунду (во время демо не трогаем)
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    if (!demoActive) checkAlarm();
  }

  // Конец демо
  if (demoActive && millis() - demoStartTime >= (unsigned long)demoSpeed * 1000UL) {
    demoActive = false;
    curR = curG = curB = curBr = 0;
    renderOff();
  }

  // Рассвет / удержание / демо - 25 fps. С 16-битной яркостью этого достаточно, а прерывания
  // выключаются вдвое реже, чем при 50 fps
  static unsigned long lastSunriseFrame = 0;
  if ((phase != PHASE_IDLE || demoActive) && millis() - lastSunriseFrame > 40) {
    lastSunriseFrame = millis();

    if (demoActive) {
      float progress = (float)(millis() - demoStartTime) / (demoSpeed * 1000.0f);
      if (progress > 1.0f) progress = 1.0f;
      setSunriseTarget(progress, demoPreset, demoColors);
    } else {
      setSunriseTarget(sunriseProgress, sunrisePreset, sunriseColors);
    }

    // Цвет догоняет цель быстрее, яркость - медленнее (0.2%/кадр = от 0 до 100 за 20 с)
    approach(curR, tgtR, 0.8f);
    approach(curG, tgtG, 0.8f);
    approach(curB, tgtB, 0.8f);
    approach(curBr, tgtBr, 0.2f);

    renderColor((uint8_t)curR, (uint8_t)curG, (uint8_t)curB, curBr / 100.0f);
  }

  // Ручной свет - 50 fps
  static unsigned long lastManualFrame = 0;
  if (phase == PHASE_IDLE && !demoActive && millis() - lastManualFrame > 20) {
    lastManualFrame = millis();

    // Автовыключение по таймеру бездействия
    if (autoOffMinutes > 0 && brightness > 0 && millis() - lastInteractionTime > (unsigned long)autoOffMinutes * 60000UL) {
      brightness = 0;
      logTime("Свет выключен по таймеру автовыключения");
    }

    approach(currentBrightness, brightness, smoothness > 0 ? smoothness : 1);

    if (currentBrightness <= 0) {
      renderOff();
    } else if (lightPreset == "rainbow") {
      static unsigned long lastRainbow = 0;
      if (millis() - lastRainbow > 50) {
        lastRainbow = millis();
        renderRainbow(currentBrightness / 100.0f);
      }
    } else {
      uint32_t c = lightPresetColor();
      renderColor((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, currentBrightness / 100.0f);
    }
  }
}
