/*
  Рассвет-будильник v2
  - Веб-интерфейс с выбором цвета свечения и градиента рассвета
  - DS3231 RTC для хранения времени
  - WS2812B адресная лента
  - Дни недели
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <time.h>

// Forward declarations
void resetToDefaults();
void saveToEEPROM();

// NTP настройки
const char* ntpServer = "pool.ntp.org";
const long gmtOffset = 3 * 3600; // GMT+3 (Москва)
bool ntpSynced = false;
unsigned long lastNtpSync = 0;
const unsigned long ntpSyncInterval = 3600000; // Синхронизация каждый час
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ===== НАСТРОЙКИ WiFi =====
const char* ssid = "Lovit-479";
const char* password = "8977852Vrfc";

// ===== НАСТРОЙКИ ЖЕЛЕЗА =====
#define LED_PIN D4
#define LED_COUNT 180

// ===== ОБЪЕКТЫ =====
ESP8266WebServer server(80);
RTC_DS3231 rtc;
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ===== ПЕРЕМЕННЫЕ =====
// Будильник
int alarmHour = 7;
int alarmMinute = 30;
bool alarmEnabled = true;
uint8_t weekdays = 0b00011111; // пн-пт включены (биты 0-6 = пн-вс)

// Рассвет
int sunriseDuration = 20;
String sunrisePreset = "classic"; // classic, warm, soft, custom
uint32_t sunriseColors[3] = {0x990000, 0xFF4400, 0xFFE4C4}; // кастомный градиент

// Свечение
int brightness = 0;
int currentBrightness = 0; // Текущая яркость для плавного перехода
int targetBrightness = 0;  // Целевая яркость
String lightPreset = "red"; // red, orange, yellow, white, rainbow, custom
uint32_t customColor = 0xFF8844;
uint32_t currentColor = 0xFF0000; // Текущий цвет для плавного перехода
uint32_t targetColor = 0xFF0000;  // Целевой цвет

// После будильника
String afterAlarm = "30"; // keep, 30, 60

// Плавность (0-10, где 0 = самый плавный, 10 = резкий)
int smoothness = 5;
int colorSmoothness = 5;

// Автовыключение (0 = выкл, 5-120 минут)
int autoOffMinutes = 30;
unsigned long lastInteractionTime = 0;

// Состояние
bool sunriseActive = false;
unsigned long alarmTriggeredTime = 0;
bool alarmTriggered = false;
int alarmDismissedAt = -1; // Минута когда будильник был выключен (-1 = не выключен)
bool demoActive = false;
unsigned long demoStartTime = 0;
int demoSpeed = 30;

// Плавный рассвет - exponential blend
unsigned long sunriseStartMillis = 0;
unsigned long sunriseDurationMillis = 0;
float curR = 0, curG = 0, curB = 0, curBr = 0; // Текущие значения (float для плавности)
float tgtR = 0, tgtG = 0, tgtB = 0, tgtBr = 0; // Целевые значения

// ===== EEPROM =====
#define EEPROM_SIZE 64
#define ADDR_HOUR 0
#define ADDR_MINUTE 1
#define ADDR_ENABLED 2
#define ADDR_WEEKDAYS 3
#define ADDR_DURATION 4
#define ADDR_BRIGHTNESS 5
#define ADDR_SUNRISE_PRESET 6 // 1 byte: 0=classic, 1=warm, 2=soft, 3=custom
#define ADDR_LIGHT_PRESET 7   // 1 byte: 0=red, 1=orange, 2=yellow, 3=white, 4=rainbow, 5=custom
#define ADDR_CUSTOM_COLOR 8   // 3 bytes RGB
#define ADDR_SUNRISE_C1 11    // 3 bytes
#define ADDR_SUNRISE_C2 14    // 3 bytes
#define ADDR_SUNRISE_C3 17    // 3 bytes
#define ADDR_AFTER_ALARM 20   // 1 byte: 0=keep, 1=30, 2=60
#define ADDR_SMOOTHNESS 21    // 1 byte: 0-10
#define ADDR_COLOR_SMOOTHNESS 22 // 1 byte: 0-10
#define ADDR_AUTO_OFF 23      // 1 byte: 0=off, or minutes (5-120)
#define ADDR_MAGIC 24         // 1 byte: magic number для проверки инициализации
#define MAGIC_VALUE 0xAB      // Магическое число

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
          <div class="slider-tooltip" id="autoOffTooltip">30 мин</div>
          <input type="range" class="slider" id="autoOff" min="0" max="120" step="5" value="30" 
            oninput="updateAutoOff(this.value)"
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
        <div class="slider-value-center" id="autoOffValue">30 мин</div>
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
          <span class="value">HomeNetwork ✓</span>
        </div>
        <div class="info-row">
          <span class="label">IP</span>
          <span class="value">192.168.1.50</span>
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
    let colorSmoothness = 5;
    let autoOffMinutes = 30;
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
      customColor = color;
      selectedPreset = 'custom';
      
      // Снимаем выделение с пресетов
      document.querySelectorAll('.color-preset').forEach(p => p.classList.remove('selected'));
      
      // Выделяем кнопку своего цвета и меняем её фон
      const customBtn = document.querySelector('.preset-custom');
      customBtn.classList.add('selected');
      customBtn.style.background = color;
      customBtn.style.border = 'none';
      customBtn.textContent = '';
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
    
    // Для совместимости со старым кодом
    function updateBrightness(val) {
      updateBrightnessLabel(val);
    }
    
    function sendSettings() {
      sendData();
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
      fetch('/save?h=' + alarmHours + '&m=' + alarmMinutes + '&en=' + (alarmEnabled?1:0) + '&wd=' + weekdaysBits + '&dur=' + duration + '&sp=' + selectedSunrisePreset + '&c1=' + c1.slice(1) + '&c2=' + c2.slice(1) + '&c3=' + c3.slice(1) + '&aa=' + afterAlarmValue + '&smooth=' + smoothness + '&csmooth=' + colorSmoothness + '&aoff=' + autoOffMinutes);
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
      } else if (tooltipId === 'colorSmoothnessTooltip') {
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
      sendData();
    }
    
    // Плавность цвета
    function updateColorSmoothness(val) {
      colorSmoothness = parseInt(val);
      document.getElementById('colorSmoothnessValue').textContent = colorSmoothness;
      sendData();
    }
    
    // Автовыключение
    function updateAutoOff(val) {
      autoOffMinutes = parseInt(val);
      let text;
      if (autoOffMinutes === 0) {
        text = 'Выкл';
      } else if (autoOffMinutes < 60) {
        text = autoOffMinutes + ' мин';
      } else {
        const h = Math.floor(autoOffMinutes / 60);
        const m = autoOffMinutes % 60;
        text = m > 0 ? h + ' ч ' + m + ' мин' : h + (h == 1 ? ' час' : ' часа');
      }
      document.getElementById('autoOffValue').textContent = text;
      sendData();
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
        
        document.querySelectorAll('.color-preset').forEach(el => el.classList.remove('selected'));
        const presetEl = document.querySelector('.preset-' + selectedPreset);
        if (presetEl) presetEl.classList.add('selected');
        
        if (e.c1) {
          document.getElementById('color1').value = '#' + e.c1;
          document.getElementById('color2').value = '#' + e.c2;
          document.getElementById('color3').value = '#' + e.c3;
          updateCustomGradient();
        }
        
        if (e.wifi) document.getElementById('wifiName').textContent = e.wifi + ' ✓';
        if (e.ip) document.getElementById('ipAddr').textContent = e.ip;
        document.getElementById('ntpStatus').textContent = e.ntp ? 'Синхронизировано ✓' : 'Не синхронизировано';
        
        if (e.smooth !== undefined) {
          smoothness = e.smooth;
          document.getElementById('smoothness').value = smoothness;
          document.getElementById('smoothnessValue').textContent = smoothness;
        }
        
        if (e.csmooth !== undefined) {
          colorSmoothness = e.csmooth;
          document.getElementById('colorSmoothness').value = colorSmoothness;
          document.getElementById('colorSmoothnessValue').textContent = colorSmoothness;
        }
        
        if (e.aoff !== undefined) {
          autoOffMinutes = e.aoff;
          document.getElementById('autoOff').value = autoOffMinutes;
          let t;
          if (autoOffMinutes === 0) t = 'Выкл';
          else if (autoOffMinutes < 60) t = autoOffMinutes + ' мин';
          else {
            const h = Math.floor(autoOffMinutes / 60), m = autoOffMinutes % 60;
            t = m > 0 ? h + ' ч ' + m + ' мин' : h + (h == 1 ? ' час' : ' часа');
          }
          document.getElementById('autoOffValue').textContent = t;
        }
        
        document.getElementById('sunriseScreen').classList.toggle('active', e.sunrise);
        updateSunriseTime();
      });
    }
    
    // Периодический опрос статуса (каждые 5 сек)
    setInterval(() => {
      fetch('/status').then(r => r.json()).then(e => {
        alarmHours = e.h;
        alarmMinutes = e.m;
        if (!window.sunriseScreenBlocked) {
          document.getElementById('sunriseScreen').classList.toggle('active', e.sunrise);
        }
        if (e.sunrise) updateSunriseTime();
      });
    }, 5000);
  </script>
</body>
</html>
)rawliteral";

const char MANIFEST[] PROGMEM = R"({"name":"Рассвет","short_name":"Рассвет","start_url":"/","display":"standalone","background_color":"#1a1a2e","theme_color":"#1a1a2e","icons":[{"src":"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>☀️</text></svg>","sizes":"any","type":"image/svg+xml"}]})";

// ===== ФУНКЦИИ ЛЕНТЫ =====

void showStrip() {
  noInterrupts();
  strip.show();
  interrupts();
}

void setAllLeds(uint8_t r, uint8_t g, uint8_t b, int br) {
  float mult = br / 100.0;
  uint8_t rOut = (uint8_t)(r * mult);
  uint8_t gOut = (uint8_t)(g * mult);
  uint8_t bOut = (uint8_t)(b * mult);
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(rOut, gOut, bOut));
  }
  showStrip();
}

void turnOff() {
  strip.clear();
  showStrip();
}

// Интерполяция цвета
uint32_t lerpColor(uint32_t c1, uint32_t c2, float t) {
  int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
  int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
  uint8_t r = r1 + (r2 - r1) * t;
  uint8_t g = g1 + (g2 - g1) * t;
  uint8_t b = b1 + (b2 - b1) * t;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Градиенты рассвета (5 точек для более реалистичного перехода)
// Глубокий красный → Красный → Оранжевый → Янтарный → Тёплый белый
void getSunriseGradient(String preset, uint32_t* colors) {
  if (preset == "classic") {
    // Классический рассвет: тёмно-красный → оранжевый → тёплый белый
    colors[0] = 0x990000; // Глубокий тёмно-красный (начало)
    colors[1] = 0xFF4400; // Красно-оранжевый (середина)
    colors[2] = 0xFFE4C4; // Тёплый белый bisque (конец)
  } else if (preset == "warm") {
    // Тёплый: более оранжевые тона
    colors[0] = 0x881100; // Тёмный красно-коричневый
    colors[1] = 0xFF5500; // Оранжевый
    colors[2] = 0xFFDAAA; // Тёплый персиковый
  } else if (preset == "soft") {
    // Мягкий: менее контрастный
    colors[0] = 0x663300; // Тёмный янтарный
    colors[1] = 0xFF7722; // Мягкий оранжевый
    colors[2] = 0xFFEEDD; // Светлый тёплый белый
  } else { // custom
    colors[0] = sunriseColors[0];
    colors[1] = sunriseColors[1];
    colors[2] = sunriseColors[2];
  }
}

void setSunriseColor(float progress) {
  uint32_t colors[3];
  getSunriseGradient(sunrisePreset, colors);
  
  uint32_t color;
  if (progress < 0.5) {
    color = lerpColor(colors[0], colors[1], progress * 2);
  } else {
    color = lerpColor(colors[1], colors[2], (progress - 0.5) * 2);
  }
  
  // Устанавливаем целевые значения (плавный переход делается в loop)
  tgtR = (color >> 16) & 0xFF;
  tgtG = (color >> 8) & 0xFF;
  tgtB = color & 0xFF;
  
  // Яркость с квадратичной кривой
  float brightCurve = progress * progress;
  tgtBr = 1 + brightCurve * 99;
}

// Применить текущий цвет рассвета к ленте
void applySunriseToStrip() {
  float mult = curBr / 100.0;
  uint8_t r = (uint8_t)(curR * mult);
  uint8_t g = (uint8_t)(curG * mult);
  uint8_t b = (uint8_t)(curB * mult);
  
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  showStrip();
}

void applyLightPreset() {
  if (currentBrightness == 0) {
    turnOff();
    return;
  }
  
  if (lightPreset == "rainbow") {
    rainbowCycle();
  } else {
    uint8_t r = (currentColor >> 16) & 0xFF;
    uint8_t g = (currentColor >> 8) & 0xFF;
    uint8_t b = currentColor & 0xFF;
    setAllLeds(r, g, b, currentBrightness);
  }
}

void rainbowCycle() {
  static uint16_t j = 0;
  float mult = currentBrightness / 100.0;
  for (int i = 0; i < LED_COUNT; i++) {
    uint32_t color = strip.ColorHSV((i * 65536 / LED_COUNT + j) % 65536);
    uint8_t r = ((color >> 16) & 0xFF) * mult;
    uint8_t g = ((color >> 8) & 0xFF) * mult;
    uint8_t b = (color & 0xFF) * mult;
    strip.setPixelColor(i, r, g, b);
  }
  showStrip();
  j += 256;
}

// ===== HTTP HANDLERS =====

void handleRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleManifest() {
  server.send_P(200, "application/json", MANIFEST);
}

void handleStatus() {
  char c1[7], c2[7], c3[7];
  sprintf(c1, "%06X", sunriseColors[0]);
  sprintf(c2, "%06X", sunriseColors[1]);
  sprintf(c3, "%06X", sunriseColors[2]);
  
  String json = "{";
  json += "\"h\":" + String(alarmHour) + ",";
  json += "\"m\":" + String(alarmMinute) + ",";
  json += "\"en\":" + String(alarmEnabled ? "true" : "false") + ",";
  json += "\"wd\":" + String(weekdays) + ",";
  json += "\"dur\":" + String(sunriseDuration) + ",";
  json += "\"sp\":\"" + sunrisePreset + "\",";
  json += "\"b\":" + String(brightness) + ",";
  json += "\"lp\":\"" + lightPreset + "\",";
  json += "\"aa\":\"" + afterAlarm + "\",";
  json += "\"c1\":\"" + String(c1) + "\",";
  json += "\"c2\":\"" + String(c2) + "\",";
  json += "\"c3\":\"" + String(c3) + "\",";
  json += "\"wifi\":\"" + String(ssid) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"sunrise\":" + String((sunriseActive || alarmTriggered || demoActive) ? "true" : "false") + ",";
  json += "\"smooth\":" + String(smoothness) + ",";
  json += "\"csmooth\":" + String(colorSmoothness) + ",";
  json += "\"aoff\":" + String(autoOffMinutes) + ",";
  json += "\"ntp\":" + String(ntpSynced ? "true" : "false");
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleSave() {
  // Сбрасываем флаг выключения если меняется время будильника
  if (server.hasArg("h") || server.hasArg("m")) {
    alarmDismissedAt = -1;
  }
  
  if (server.hasArg("h")) alarmHour = server.arg("h").toInt();
  if (server.hasArg("m")) alarmMinute = server.arg("m").toInt();
  if (server.hasArg("en")) alarmEnabled = server.arg("en").toInt() == 1;
  if (server.hasArg("wd")) weekdays = server.arg("wd").toInt();
  if (server.hasArg("dur")) sunriseDuration = server.arg("dur").toInt();
  if (server.hasArg("sp")) sunrisePreset = server.arg("sp");
  if (server.hasArg("aa")) afterAlarm = server.arg("aa");
  if (server.hasArg("smooth")) smoothness = server.arg("smooth").toInt();
  if (server.hasArg("csmooth")) colorSmoothness = server.arg("csmooth").toInt();
  if (server.hasArg("aoff")) autoOffMinutes = server.arg("aoff").toInt();
  
  if (server.hasArg("c1")) {
    sunriseColors[0] = strtol(server.arg("c1").c_str(), NULL, 16);
  }
  if (server.hasArg("c2")) {
    sunriseColors[1] = strtol(server.arg("c2").c_str(), NULL, 16);
  }
  if (server.hasArg("c3")) {
    sunriseColors[2] = strtol(server.arg("c3").c_str(), NULL, 16);
  }
  
  saveToEEPROM();
  server.send(200, "text/plain", "OK");
}

void handleLight() {
  lastInteractionTime = millis(); // Сброс таймера автовыключения
  
  bool presetChanged = false;
  
  if (server.hasArg("p")) {
    String newPreset = server.arg("p");
    if (newPreset != lightPreset) {
      lightPreset = newPreset;
      presetChanged = true;
      
      // Если меняем пресет во время будильника — dismiss
      if (sunriseActive || alarmTriggered) {
        DateTime now = rtc.now();
        alarmDismissedAt = now.hour() * 60 + now.minute();
        sunriseActive = false;
        alarmTriggered = false;
        curR = 0; curG = 0; curB = 0; curBr = 0;
      }
    }
    // Устанавливаем целевой цвет по пресету
    if (lightPreset == "red") targetColor = 0xFF0000;
    else if (lightPreset == "orange") targetColor = 0xFF5500;
    else if (lightPreset == "yellow") targetColor = 0xFFFF00;
    else if (lightPreset == "white") targetColor = 0xFFFFFF;
    else if (lightPreset == "custom") targetColor = customColor;
  }
  
  if (server.hasArg("c")) {
    uint32_t newColor = strtol(server.arg("c").c_str(), NULL, 16);
    if (newColor != customColor) {
      customColor = newColor;
      if (lightPreset == "custom") {
        targetColor = customColor;
        presetChanged = true;
      }
    }
  }
  
  if (server.hasArg("b")) {
    brightness = server.arg("b").toInt();
    targetBrightness = brightness;
    
    // Если яркость 0 — выключаем всё
    if (brightness == 0) {
      if (sunriseActive || alarmTriggered) {
        DateTime now = rtc.now();
        alarmDismissedAt = now.hour() * 60 + now.minute();
        Serial.println("🛑 Будильник выключен (dismissed at " + String(alarmDismissedAt / 60) + ":" + String(alarmDismissedAt % 60) + ")");
      }
      demoActive = false;
      sunriseActive = false;
      alarmTriggered = false;
      currentBrightness = 0;
      turnOff();
    }
    // Если яркость > 0 — прерываем рассвет/демо
    else if (sunriseActive || alarmTriggered || demoActive) {
      // Если был активен будильник — помечаем как dismissed
      if (sunriseActive || alarmTriggered) {
        DateTime now = rtc.now();
        alarmDismissedAt = now.hour() * 60 + now.minute();
      }
      sunriseActive = false;
      alarmTriggered = false;
      demoActive = false;
      curR = 0; curG = 0; curB = 0; curBr = 0;
    }
  }
  
  // Если пресет/цвет изменился и свет включен — применить сразу
  if (presetChanged && currentBrightness > 0 && lightPreset != "rainbow") {
    currentColor = targetColor;
    applyLightPreset();
  }
  
  server.send(200, "text/plain", "OK");
}

void handleDemo() {
  if (server.hasArg("start")) {
    int start = server.arg("start").toInt();
    if (start == 1) {
      // Устанавливаем пресет если передан
      if (server.hasArg("preset")) {
        sunrisePreset = server.arg("preset");
      }
      // Устанавливаем кастомные цвета если переданы
      if (server.hasArg("c1")) {
        sunriseColors[0] = strtol(server.arg("c1").c_str(), NULL, 16);
      }
      if (server.hasArg("c2")) {
        sunriseColors[1] = strtol(server.arg("c2").c_str(), NULL, 16);
      }
      if (server.hasArg("c3")) {
        sunriseColors[2] = strtol(server.arg("c3").c_str(), NULL, 16);
      }
      // Сбрасываем текущее состояние перед запуском демо
      sunriseActive = false;
      alarmTriggered = false;
      currentBrightness = 0;
      targetBrightness = 0;
      brightness = 0;
      
      // Получаем начальный цвет градиента и сразу устанавливаем его
      uint32_t colors[3];
      getSunriseGradient(sunrisePreset, colors);
      curR = (colors[0] >> 16) & 0xFF;
      curG = (colors[0] >> 8) & 0xFF;
      curB = colors[0] & 0xFF;
      curBr = 0; // Яркость начинается с 0
      tgtR = curR; tgtG = curG; tgtB = curB;
      tgtBr = 1; // Начальная целевая яркость
      
      demoActive = true;
      demoStartTime = millis();
      if (server.hasArg("speed")) {
        demoSpeed = server.arg("speed").toInt();
        if (demoSpeed <= 0) demoSpeed = 30;
      }
    } else {
      // Если был активен будильник — помечаем как dismissed
      if (sunriseActive || alarmTriggered) {
        DateTime now = rtc.now();
        alarmDismissedAt = now.hour() * 60 + now.minute();
        Serial.println("🛑 Будильник выключен через OFF (dismissed at " + String(alarmDismissedAt / 60) + ":" + String(alarmDismissedAt % 60) + ")");
      }
      demoActive = false;
      sunriseActive = false;
      alarmTriggered = false;
      currentBrightness = 0;
      targetBrightness = 0;
      brightness = 0;
      curR = 0; curG = 0; curB = 0; curBr = 0;
      currentColor = targetColor;
      turnOff();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSnooze() {
  // Откладываем будильник на 5 минут
  sunriseActive = false;
  alarmTriggered = false;
  demoActive = false;
  alarmDismissedAt = -1; // Сбрасываем чтобы будильник мог сработать снова
  curR = 0; curG = 0; curB = 0; curBr = 0;
  turnOff();
  currentBrightness = 0;
  targetBrightness = 0;
  
  // Сдвигаем время будильника на 5 минут вперёд (только на сегодня)
  alarmMinute += 5;
  if (alarmMinute >= 60) {
    alarmMinute -= 60;
    alarmHour++;
    if (alarmHour >= 24) alarmHour = 0;
  }
  
  server.send(200, "text/plain", "OK");
}

// ===== EEPROM =====

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
  
  uint8_t aaIdx = 1;
  if (afterAlarm == "keep") aaIdx = 0;
  else if (afterAlarm == "60") aaIdx = 2;
  EEPROM.write(ADDR_AFTER_ALARM, aaIdx);
  
  EEPROM.write(ADDR_SMOOTHNESS, smoothness);
  EEPROM.write(ADDR_COLOR_SMOOTHNESS, colorSmoothness);
  EEPROM.write(ADDR_AUTO_OFF, autoOffMinutes);
  
  // Sunrise colors
  EEPROM.write(ADDR_SUNRISE_C1, (sunriseColors[0] >> 16) & 0xFF);
  EEPROM.write(ADDR_SUNRISE_C1 + 1, (sunriseColors[0] >> 8) & 0xFF);
  EEPROM.write(ADDR_SUNRISE_C1 + 2, sunriseColors[0] & 0xFF);
  
  EEPROM.write(ADDR_SUNRISE_C2, (sunriseColors[1] >> 16) & 0xFF);
  EEPROM.write(ADDR_SUNRISE_C2 + 1, (sunriseColors[1] >> 8) & 0xFF);
  EEPROM.write(ADDR_SUNRISE_C2 + 2, sunriseColors[1] & 0xFF);
  
  EEPROM.write(ADDR_SUNRISE_C3, (sunriseColors[2] >> 16) & 0xFF);
  EEPROM.write(ADDR_SUNRISE_C3 + 1, (sunriseColors[2] >> 8) & 0xFF);
  EEPROM.write(ADDR_SUNRISE_C3 + 2, sunriseColors[2] & 0xFF);
  
  EEPROM.write(ADDR_MAGIC, MAGIC_VALUE);
  
  EEPROM.commit();
  yield(); // Даём время завершить запись
}

void loadFromEEPROM() {
  // Проверяем магическое число - если не совпадает, сброс на дефолты
  if (EEPROM.read(ADDR_MAGIC) != MAGIC_VALUE) {
    resetToDefaults();
    return;
  }
  
  alarmHour = EEPROM.read(ADDR_HOUR);
  alarmMinute = EEPROM.read(ADDR_MINUTE);
  alarmEnabled = EEPROM.read(ADDR_ENABLED) == 1;
  weekdays = EEPROM.read(ADDR_WEEKDAYS);
  sunriseDuration = EEPROM.read(ADDR_DURATION);
  
  uint8_t spIdx = EEPROM.read(ADDR_SUNRISE_PRESET);
  if (spIdx == 1) sunrisePreset = "warm";
  else if (spIdx == 2) sunrisePreset = "soft";
  else if (spIdx == 3) sunrisePreset = "custom";
  else sunrisePreset = "classic";
  
  uint8_t aaIdx = EEPROM.read(ADDR_AFTER_ALARM);
  if (aaIdx == 0) afterAlarm = "keep";
  else if (aaIdx == 2) afterAlarm = "60";
  else afterAlarm = "30";
  
  smoothness = EEPROM.read(ADDR_SMOOTHNESS);
  colorSmoothness = EEPROM.read(ADDR_COLOR_SMOOTHNESS);
  autoOffMinutes = EEPROM.read(ADDR_AUTO_OFF);
  
  sunriseColors[0] = ((uint32_t)EEPROM.read(ADDR_SUNRISE_C1) << 16) | 
                     ((uint32_t)EEPROM.read(ADDR_SUNRISE_C1 + 1) << 8) | 
                     EEPROM.read(ADDR_SUNRISE_C1 + 2);
  sunriseColors[1] = ((uint32_t)EEPROM.read(ADDR_SUNRISE_C2) << 16) | 
                     ((uint32_t)EEPROM.read(ADDR_SUNRISE_C2 + 1) << 8) | 
                     EEPROM.read(ADDR_SUNRISE_C2 + 2);
  sunriseColors[2] = ((uint32_t)EEPROM.read(ADDR_SUNRISE_C3) << 16) | 
                     ((uint32_t)EEPROM.read(ADDR_SUNRISE_C3 + 1) << 8) | 
                     EEPROM.read(ADDR_SUNRISE_C3 + 2);
  
  // Валидация
  if (alarmHour > 23) alarmHour = 7;
  if (alarmMinute > 59) alarmMinute = 30;
  if (sunriseDuration < 5 || sunriseDuration > 60) sunriseDuration = 20;
  if (weekdays == 0 || weekdays == 255) weekdays = 0b00011111;
  if (smoothness < 0 || smoothness > 10) smoothness = 5;
  if (colorSmoothness < 0 || colorSmoothness > 10) colorSmoothness = 5;
  if (autoOffMinutes < 0 || autoOffMinutes > 120) autoOffMinutes = 30;
  
  // Если цвета не инициализированы или старые - ставим дефолтные
  bool colorsInvalid = (sunriseColors[0] == 0 && sunriseColors[1] == 0 && sunriseColors[2] == 0) ||
                       sunriseColors[0] == 0xFFFFFF || sunriseColors[1] == 0xFFFFFF || sunriseColors[2] == 0xFFFFFF ||
                       sunriseColors[0] == 0xFF4444 || sunriseColors[1] == 0xFF8844 || sunriseColors[2] == 0xFFDD44 ||
                       sunriseColors[0] == 0xFF0000; // Старые красные цвета тоже обновить
  if (colorsInvalid) {
    sunriseColors[0] = 0x990000;  // Глубокий тёмно-красный
    sunriseColors[1] = 0xFF4400;  // Красно-оранжевый
    sunriseColors[2] = 0xFFE4C4;  // Тёплый белый
    // Принудительно переключаем на classic
    sunrisePreset = "classic";
    // Сохраняем исправленные значения
    saveToEEPROM();
  }
}

void resetToDefaults() {
  // Сброс всех флагов состояния
  sunriseActive = false;
  alarmTriggered = false;
  demoActive = false;
  alarmDismissedAt = -1;
  
  // Сброс переменных рассвета
  curR = 0; curG = 0; curB = 0; curBr = 0;
  tgtR = 0; tgtG = 0; tgtB = 0; tgtBr = 0;
  sunriseStartMillis = 0;
  sunriseDurationMillis = 0;
  
  // Будильник
  alarmHour = 7;
  alarmMinute = 30;
  alarmEnabled = true;
  weekdays = 0b00011111; // пн-пт
  sunriseDuration = 20;
  sunrisePreset = "classic";
  afterAlarm = "30";
  
  // Цвета рассвета (реалистичные)
  sunriseColors[0] = 0x990000;  // Глубокий тёмно-красный
  sunriseColors[1] = 0xFF4400;  // Красно-оранжевый
  sunriseColors[2] = 0xFFE4C4;  // Тёплый белый
  
  // Свет
  brightness = 0;
  currentBrightness = 0;
  targetBrightness = 0;
  lightPreset = "red";
  customColor = 0xFF5500;
  currentColor = 0xFF0000;
  targetColor = 0xFF0000;
  
  // Настройки
  smoothness = 5;
  colorSmoothness = 5;
  autoOffMinutes = 30;
  
  // Сохраняем
  saveToEEPROM();
  
  // Выключаем свет
  turnOff();
}

void handleReset() {
  // Сначала сбрасываем и сохраняем
  resetToDefaults();
  
  // Потом отправляем ответ
  server.send(200, "text/plain", "OK");
  delay(500); // Даём время отправить ответ
  
  // Перезагружаем ESP для чистого старта
  ESP.restart();
}

// ===== ALARM CHECK =====

void checkAlarm() {
  if (!alarmEnabled) {
    if (sunriseActive) {
      sunriseActive = false;
      turnOff();
    }
    return;
  }
  
  DateTime now = rtc.now();
  
  // Проверяем день недели (RTC: 0=вс, 1=пн, ... 6=сб)
  // Наши биты: 0=пн, 1=вт, ... 6=вс
  int rtcDow = now.dayOfTheWeek(); // 0=Sunday
  int ourDow = (rtcDow + 6) % 7;   // Конвертируем: 0=пн, 6=вс
  
  if (!(weekdays & (1 << ourDow))) {
    // Сегодня будильник не активен
    if (sunriseActive) {
      sunriseActive = false;
      turnOff();
    }
    return;
  }
  
  // Используем секунды для плавного перехода
  long currentSeconds = (long)now.hour() * 3600 + now.minute() * 60 + now.second();
  long alarmSeconds = (long)alarmHour * 3600 + alarmMinute * 60;
  long sunriseStartSeconds = alarmSeconds - (long)sunriseDuration * 60;
  long sunriseDurationSeconds = (long)sunriseDuration * 60;
  
  // Проверка с учётом перехода через полночь
  bool inSunrise = false;
  long elapsedSeconds = 0;
  
  if (sunriseStartSeconds < 0) {
    // Рассвет начинается до полуночи
    sunriseStartSeconds += 86400;
    if (currentSeconds >= sunriseStartSeconds || currentSeconds < alarmSeconds) {
      inSunrise = true;
      if (currentSeconds >= sunriseStartSeconds) {
        elapsedSeconds = currentSeconds - sunriseStartSeconds;
      } else {
        elapsedSeconds = (86400 - sunriseStartSeconds) + currentSeconds;
      }
    }
  } else {
    // Обычный случай - рассвет в тот же день
    if (currentSeconds >= sunriseStartSeconds && currentSeconds < alarmSeconds) {
      inSunrise = true;
      elapsedSeconds = currentSeconds - sunriseStartSeconds;
    }
  }
  
  int currentMinutes = now.hour() * 60 + now.minute();
  int alarmMinutes = alarmHour * 60 + alarmMinute;
  
  // Рассвет
  if (inSunrise) {
    // Проверяем не был ли будильник уже выключен пользователем
    if (alarmDismissedAt >= 0) {
      // Debug: будильник заблокирован
      static unsigned long lastDebugPrint = 0;
      if (millis() - lastDebugPrint > 5000) { // Раз в 5 секунд
        Serial.println("⏸ Будильник заблокирован (dismissed at " + String(alarmDismissedAt / 60) + ":" + String(alarmDismissedAt % 60) + ")");
        lastDebugPrint = millis();
      }
      return; // Будильник уже выключен, не включаем снова
    }
    
    // Не запускаем рассвет если свет уже включен вручную
    if (!sunriseActive && currentBrightness > 0) {
      return; // Пользователь уже проснулся
    }
    
    if (!sunriseActive) {
      Serial.println("🌅 Запуск рассвета!");
      sunriseActive = true;
      alarmTriggered = false;
      
      // Получаем начальный цвет градиента и сразу устанавливаем его
      uint32_t colors[3];
      getSunriseGradient(sunrisePreset, colors);
      curR = (colors[0] >> 16) & 0xFF;
      curG = (colors[0] >> 8) & 0xFF;
      curB = colors[0] & 0xFF;
      curBr = 0; // Яркость начинается с 0
      tgtR = curR; tgtG = curG; tgtB = curB;
      tgtBr = 1; // Начальная целевая яркость
      
      // Вычисляем время старта рассвета на основе уже прошедшего времени
      sunriseDurationMillis = (unsigned long)sunriseDuration * 60 * 1000;
      sunriseStartMillis = millis() - (unsigned long)elapsedSeconds * 1000;
    }
    
    // Цвет обновляется в loop через millis
  }
  // Будильник сработал (после рассвета - держим максимальную яркость)
  else if (currentMinutes >= alarmMinutes && currentMinutes < alarmMinutes + 5) {
    // Проверяем не был ли будильник уже выключен пользователем
    if (alarmDismissedAt >= 0) {
      return; // Будильник уже выключен, не включаем снова
    }
    
    if (!alarmTriggered) {
      alarmTriggered = true;
      alarmTriggeredTime = millis();
      // Устанавливаем progress = 1.0
      sunriseStartMillis = millis() - sunriseDurationMillis;
    }
    // Цвет обновляется в loop
  }
  // После будильника — автовыключение
  else if (alarmTriggered) {
    unsigned long elapsed = millis() - alarmTriggeredTime;
    unsigned long timeout = 0;
    
    if (afterAlarm == "30") timeout = 30 * 60 * 1000UL;
    else if (afterAlarm == "60") timeout = 60 * 60 * 1000UL;
    
    if (afterAlarm != "keep" && elapsed > timeout) {
      turnOff();
      sunriseActive = false;
      alarmTriggered = false;
    }
  }
  else {
    if (sunriseActive) {
      sunriseActive = false;
    }
    // Сбрасываем флаг выключения когда время вышло за диапазон будильника
    if (alarmDismissedAt >= 0) {
      Serial.println("✅ Сброс dismiss флага (время вышло за диапазон будильника)");
    }
    alarmDismissedAt = -1;
  }
}

// ===== NTP SYNC =====

void syncNTP() {
  configTime(gmtOffset, 0, ntpServer, "time.google.com", "time.cloudflare.com");
  
  Serial.print("Синхронизация NTP");
  
  // Ждём получения времени (максимум 10 секунд)
  time_t now = 0;
  int attempts = 0;
  while (now < 1000000000 && attempts < 20) {
    delay(500);
    Serial.print(".");
    time(&now);
    attempts++;
  }
  
  if (now > 1000000000) {
    // Обновляем RTC
    struct tm* timeinfo = localtime(&now);
    rtc.adjust(DateTime(
      timeinfo->tm_year + 1900,
      timeinfo->tm_mon + 1,
      timeinfo->tm_mday,
      timeinfo->tm_hour,
      timeinfo->tm_min,
      timeinfo->tm_sec
    ));
    
    ntpSynced = true;
    lastNtpSync = millis();
    Serial.println("\nNTP синхронизировано!");
    Serial.printf("Время: %02d:%02d:%02d\n", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  } else {
    Serial.println("\nNTP ошибка, используем RTC");
  }
}

// ===== SETUP =====

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nРассвет-будильник v2");
  
  EEPROM.begin(EEPROM_SIZE);
  loadFromEEPROM();
  
  strip.begin();
  strip.setBrightness(200); // Ограничение ~80% для безопасности БП
  showStrip();
  
  Wire.begin(D2, D1);
  if (!rtc.begin()) {
    Serial.println("RTC не найден!");
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  WiFi.begin(ssid, password);
  Serial.print("Подключение к WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nПодключено!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  
  // Синхронизация времени через NTP
  syncNTP();
  
  if (MDNS.begin("sunrise")) {
    Serial.println("mDNS: http://sunrise.local");
  }
  
  server.on("/", handleRoot);
  server.on("/manifest.json", handleManifest);
  server.on("/status", handleStatus);
  server.on("/save", handleSave);
  server.on("/light", handleLight);
  server.on("/demo", handleDemo);
  server.on("/snooze", handleSnooze);
  server.on("/reset", handleReset);
  
  server.begin();
  Serial.println("Сервер запущен!");
}

// ===== LOOP =====

void loop() {
  server.handleClient();
  MDNS.update();
  
  // Проверка окончания демо
  if (demoActive) {
    if (demoSpeed <= 0) demoSpeed = 30; // Защита от деления на 0
    unsigned long elapsed = millis() - demoStartTime;
    float progress = (float)elapsed / (demoSpeed * 1000);
    if (progress >= 1.0) {
      demoActive = false;
      currentBrightness = 0;
      targetBrightness = 0;
      brightness = 0;
      // Сбросить переменные рассвета
      curR = 0; curG = 0; curB = 0; curBr = 0;
      tgtR = 0; tgtG = 0; tgtB = 0; tgtBr = 0;
      // Сбросить цвет на текущий пресет
      currentColor = targetColor;
      turnOff();
    }
  }
  
  // Проверка будильника - каждую секунду
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    if (!demoActive) {
      checkAlarm();
    }
  }
  
  // Обновление рассвета/демо - каждые 20мс (~50 fps)
  static unsigned long lastSunriseUpdate = 0;
  if ((sunriseActive || alarmTriggered || demoActive) && millis() - lastSunriseUpdate > 20) {
    lastSunriseUpdate = millis();
    
    // Вычисляем progress
    float progress;
    if (demoActive) {
      if (demoSpeed <= 0) demoSpeed = 30;
      progress = (float)(millis() - demoStartTime) / (demoSpeed * 1000);
    } else if (sunriseDurationMillis > 0) {
      progress = (float)(millis() - sunriseStartMillis) / sunriseDurationMillis;
    } else {
      progress = 1.0;
    }
    if (progress < 0) progress = 0;
    if (progress > 1.0) progress = 1.0;
    
    // Устанавливаем target из progress
    setSunriseColor(progress);
    
    // Линейный step для плавного перехода
    const float colorStep = 0.8;  // Шаг цвета
    const float brStep = 0.4;     // Шаг яркости
    
    // Линейное изменение R
    if (curR < tgtR) { curR += colorStep; if (curR > tgtR) curR = tgtR; }
    else if (curR > tgtR) { curR -= colorStep; if (curR < tgtR) curR = tgtR; }
    
    // Линейное изменение G
    if (curG < tgtG) { curG += colorStep; if (curG > tgtG) curG = tgtG; }
    else if (curG > tgtG) { curG -= colorStep; if (curG < tgtG) curG = tgtG; }
    
    // Линейное изменение B
    if (curB < tgtB) { curB += colorStep; if (curB > tgtB) curB = tgtB; }
    else if (curB > tgtB) { curB -= colorStep; if (curB < tgtB) curB = tgtB; }
    
    // Линейное изменение яркости
    if (curBr < tgtBr) { curBr += brStep; if (curBr > tgtBr) curBr = tgtBr; }
    else if (curBr > tgtBr) { curBr -= brStep; if (curBr < tgtBr) curBr = tgtBr; }
    
    applySunriseToStrip();
  }
  
  // Периодическая синхронизация NTP (каждый час)
  if (millis() - lastNtpSync > ntpSyncInterval) {
    syncNTP();
  }
  
  // Автовыключение по таймеру
  if (autoOffMinutes > 0 && currentBrightness > 0 && !sunriseActive && !alarmTriggered && !demoActive) {
    unsigned long autoOffMs = (unsigned long)autoOffMinutes * 60 * 1000;
    if (millis() - lastInteractionTime > autoOffMs) {
      targetBrightness = 0;
      brightness = 0;
    }
  }
  
  // Плавное изменение яркости и цвета
  static unsigned long lastBrightnessUpdate = 0;
  if (millis() - lastBrightnessUpdate > 20) { // ~50 fps
    lastBrightnessUpdate = millis();
    
    if (!demoActive && !sunriseActive && !alarmTriggered) {
      bool needUpdate = false;
      
      // Плавное изменение яркости
      if (currentBrightness != targetBrightness) {
        int step = smoothness > 0 ? smoothness : 1;
        
        if (currentBrightness < targetBrightness) {
          currentBrightness += step;
          if (currentBrightness > targetBrightness) currentBrightness = targetBrightness;
        } else {
          currentBrightness -= step;
          if (currentBrightness < targetBrightness) currentBrightness = targetBrightness;
        }
        needUpdate = true;
      }
      
      // Плавное изменение цвета отключено - мгновенное переключение
      if (currentColor != targetColor) {
        currentColor = targetColor;
        needUpdate = true;
      }
      
      if (needUpdate && lightPreset != "rainbow") {
        applyLightPreset();
      }
    }
  }
  
  // Обновление радуги
  static unsigned long lastRainbow = 0;
  if (millis() - lastRainbow > 50) {
    lastRainbow = millis();
    if (!demoActive && !sunriseActive && !alarmTriggered && lightPreset == "rainbow" && currentBrightness > 0) {
      rainbowCycle();
    }
  }
}
