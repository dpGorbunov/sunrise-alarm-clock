# Sunrise Alarm Clock

Умный будильник-рассвет на ESP8266 с веб-интерфейсом.

## Скриншоты

| Главный экран | Будильник | Настройки | Настройки | Настройки |
|:---:|:---:|:---:|:---:|:---:|
| <img src="screenshots/main.jpg" width="180"> | <img src="screenshots/alarm.jpg" width="180"> | <img src="screenshots/settings1.jpg" width="180"> | <img src="screenshots/settings2.jpg" width="180"> | <img src="screenshots/settings3.jpg" width="180"> |

## Возможности

- Симуляция рассвета перед будильником
- Веб-интерфейс (PWA) — `http://sunrise.local`
- Выбор дней недели, пресеты цветов, радуга
- Отложить будильник (+5 мин), автовыключение, синхронизация NTP

## Железо

- ESP8266 (NodeMCU/Wemos D1)
- WS2812B адресная лента (176 шт)
- DS3231 RTC модуль

### Подключение

```mermaid
graph LR
    subgraph БП 5V
        PSU_5V[+5V]
        PSU_GND[GND]
    end

    subgraph NodeMCU
        VIN
        D1[D1/SCL]
        D2[D2/SDA]
        D4[D4]
        V3[3.3V]
        GND[GND]
    end

    subgraph DS3231
        SCL
        SDA
        VCC
        RTC_GND[GND]
    end

    subgraph WS2812B лента
        DIN
        LED_5V[5V]
        LED_GND[GND]
    end

    D1 --> SCL
    D2 --> SDA
    V3 --> VCC
    GND --> RTC_GND

    D4 --> DIN
    PSU_5V --> LED_5V
    PSU_5V --> VIN
    PSU_GND --> LED_GND
    PSU_GND --> GND
```

## Установка

1. Arduino IDE + ESP8266 Board
2. Библиотеки: `Adafruit NeoPixel`, `RTClib`
3. Изменить WiFi в коде:
   ```cpp
   const char* ssid = "YOUR_WIFI";
   const char* password = "YOUR_PASSWORD";
   ```
4. Загрузить на ESP8266

## API

```bash
# Свет
curl "http://sunrise.local/light?b=50&p=orange"

# Будильник
curl "http://sunrise.local/save?h=7&m=0&en=1"

# Статус
curl "http://sunrise.local/status"
```

| Endpoint | Описание |
|----------|----------|
| `/status` | JSON настроек |
| `/save` | Сохранить будильник |
| `/light` | Управление светом |
| `/demo` | Демо рассвета |
| `/snooze` | Отложить (+5 мин) |
| `/reset` | Сброс настроек |
