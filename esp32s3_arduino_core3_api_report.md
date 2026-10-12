# ESP32-S3 + arduino-esp32 core 3.x (ESP-IDF v5.x) — verified API reference for the .ino port

All signatures below were read from the actual upstream source or official docs (URLs at the end of each
section). Anything I could not verify from a primary source is explicitly marked **UNVERIFIED**.

Note on core version: "core 3.x" (ESP-IDF 5.1+) is the 2.x→3.0 migration baseline; some items below (BLE
NimBLE backend, ESP_SR library, `rgbLedWrite`, MCPWM servo path, `ledcAttachChannel`) are additions from
later 3.x point releases and are present in the current master I inspected. Check `ESP_ARDUINO_VERSION_MAJOR`
/ `ESP_ARDUINO_VERSION` in your installed core if you need to gate features.

---

## 1. LEDC PWM (core 3.x)

### Removed vs. new (confirmed)
Core 3.0 **removed** `ledcSetup()` and `ledcAttachPin()`; `ledcDetachPin()` was renamed `ledcDetach()`, and
in **all** functions the `channel` parameter became a `pin` parameter.

### Exact signatures (from `cores/esp32/esp32-hal-ledc.h`)
```c
ledc_clk_cfg_t ledcGetClockSource(void);
bool     ledcSetClockSource(ledc_clk_cfg_t source);

bool     ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolution);
bool     ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, uint8_t channel);

bool     ledcWrite(uint8_t pin, uint32_t duty);
bool     ledcWriteChannel(uint8_t channel, uint32_t duty);
uint32_t ledcRead(uint8_t pin);
uint32_t ledcReadFreq(uint8_t pin);

uint32_t ledcWriteTone(uint8_t pin, uint32_t freq);          // 50 % duty tone
uint32_t ledcWriteNote(uint8_t pin, note_t note, uint8_t octave);
uint32_t ledcChangeFrequency(uint8_t pin, uint32_t freq, uint8_t resolution);

bool     ledcDetach(uint8_t pin);
bool     ledcOutputInvert(uint8_t pin, bool out_invert);

bool     ledcFade(uint8_t pin, uint32_t start_duty, uint32_t target_duty, int max_fade_time_ms);
bool     ledcFadeWithInterrupt(uint8_t pin, uint32_t s, uint32_t t, int ms, void (*userFunc)(void));
bool     ledcFadeWithInterruptArg(uint8_t pin, uint32_t s, uint32_t t, int ms, void (*userFunc)(void*), void* arg);

void     analogWrite(uint8_t pin, int value);                 // 0..255
void     analogWriteResolution(uint8_t pin, uint8_t resolution);
void     analogWriteFrequency(uint8_t pin, uint32_t freq);
```
`resolution` range: **1–14 bits** (1–20 bits on original ESP32 only; S3 = 1–14).

### How does `ledcWrite(pin, duty)` know the resolution?
The resolution is a property of the **timer**, not of the write call. `ledcAttach(pin, freq, resolution)`
allocates a channel + timer and stores `channel_resolution`, `timer_num`, `freq_hz` in an internal
`ledc_channel_handle_t` keyed by pin. `ledcWrite(pin, duty)` looks up that handle and applies `duty` at the
previously configured resolution. So duty is an **absolute raw compare value** in `0 .. 2^resolution - 1`
(not 0–255, not a percentage).

### Explicit resolution = yes, `ledcAttach(pin, freq, resolution)`
Also `analogWriteResolution(pin, bits)`. There is no separate "set resolution later" function other than
`ledcChangeFrequency(pin, freq, resolution)` (which re-programs the timer and affects **all** channels
sharing that timer).

### 50 Hz servos at 14-bit — is it achievable?
Yes. `freq ≈ ledc_clock / (divider × 2^resolution)`; the divider maxes out around 1021 (8-bit int +
fractional). The official docs state that at a 40 MHz XTAL clock with **8-bit** resolution the **minimum**
achievable frequency is ~153 Hz — i.e. you need more resolution for 50 Hz. Computed minima:
- 8-bit → ~153 Hz (documented)
- 12-bit → ~9.6 Hz
- 14-bit → ~2.4 Hz

So **`ledcAttach(pin, 50, 14)` is fine**, and even 12-bit works. Practical note: 50 Hz / 14-bit gives
20 ms / 16384 ≈ **1.22 µs per LSB**, far finer than an SG90 needs. Also remember: resolution *reduces*
maximum frequency; don't attach a 5 kHz LED channel to the same timer as the servo.

### Timer-sharing gotcha (new in the current docs/headers)
`ledcAttach()` reuses an existing timer when another channel in the same group already has the same
`freq`+`resolution`; different channel numbers do **not** guarantee independent frequencies, and
`ledcChangeFrequency()` then changes every channel on that timer. To keep two PWM outputs independent:
attach them with different freq/resolution from the start, or use `ledcAttachChannel()` with explicit
channels. ESP32-S3 has a **single group with 8 LEDC channels and 4 timers**.

### Raw-LEDC servo example
```cpp
#include <Arduino.h>
#define SERVO_PIN 4
// 50 Hz, 14-bit  ->  0..16383 over a 20000 us period
void servoUs(uint32_t us) {
  us = constrain(us, 500, 2500);
  uint32_t duty = (uint32_t)((uint64_t)us * 16383ULL / 20000ULL);  // 500us->409, 1500us->1228, 2500us->2047
  ledcWrite(SERVO_PIN, duty);
}
void setup() {
  ledcAttach(SERVO_PIN, 50, 14);
  servoUs(1500);
}
void loop() {}
```
Sources: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ledc.html ,
https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html ,
`cores/esp32/esp32-hal-ledc.h` (jsDelivr mirror of espressif/arduino-esp32@master).

---

## 2. Servo on ESP32-S3

### Is there a built-in `Servo`?
**No.** The bundled library list of arduino-esp32 contains no `Servo` and no `ESP32Servo`
(verified by listing `libraries/` of espressif/arduino-esp32: ArduinoOTA, AsyncUDP, BLE, BluetoothSerial,
Console, DNSServer, EEPROM, ESP32, ESP_SR, ESP_I2S, ESP_NOW, ESPmDNS, Ethernet, FFat, FS, HTTPClient,
HTTPUpdate, Hash, Insights, LittleFS, Matter, NetBIOS, Network, NetworkClientSecure, OpenThread, PPP,
Preferences, RainMaker, SD, SD_MMC, SPI, SPIFFS, SimpleBLE, Ticker, USB, Update, WebServer, WiFi, WiFiProv,
Wire, Zigbee).

You must install a third-party library. Recommended: **ESP32Servo** (`madhephaestus/ESP32Servo`), which
provides the header **`ESP32Servo.h`** and a class named `Servo` (so `#include <ESP32Servo.h>` + `Servo
myservo;`). (`ServoESP32` is an older alternative.)

### ESP32Servo API (verified from `src/ESP32Servo.h`)
```cpp
class Servo {
public:
  Servo();
  int  attach(int pin);                       // returns channel number, 0 on failure
  int  attach(int pin, int min, int max);     // min/max in MICROSECONDS (clamped 500..2500)
  void detach();
  void write(int value);          // <500 => angle 0..180 ; >=500 => microseconds
  void writeMicroseconds(int value);
  void writeTicks(int value);
  void release();
  int  read();                    // last value as angle 0..180
  int  readMicroseconds();
  int  readTicks();
  bool attached();
  void setTimerWidth(int value);  // 10..20 (S3); side effect: recomputes pulse width
  int  readTimerWidth();
  void setPeriodHertz(int hertz); // NOTE: this is how you change the refresh rate
  // no 4-argument attach(pin,min,max,freq) exists
};
```
Defaults: `DEFAULT_uS_LOW 544`, `DEFAULT_uS_HIGH 2400`, `MIN_PULSE_WIDTH 500`,
`MAX_PULSE_WIDTH 2500`, `DEFAULT_PULSE_WIDTH 1500`, refresh = **50 Hz** (`int REFRESH_CPS = 50`,
`REFRESH_USEC 20000`), default timer width 10 bits.

`ESP32PWM` (in `ESP32PWM.h`) additionally exposes:
```cpp
static void allocateTimer(int timerNumber);   // explicit allocation mode, timers 0..3
static bool explicateAllocationMode;
ESP32PWM(bool variableFrequency = true);      // false = fixed frequency (servo mode)
void attachPin(uint8_t pin, double freq, uint8_t resolution_bits = 10);
void detachPin(int pin); void write(uint32_t duty); void writeScaled(double duty);
double adjustFrequency(double freq, double dutyScaled = -1);
static int channelsRemaining();
```

### Core 3.x compatibility: YES (verified in source)
`ESP32PWM.cpp` is guarded with `ESP_ARDUINO_VERSION`:
```cpp
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttachChannel(pin, freq, bit_num, channel);
    ledcWrite(getPin(), duty);
    ledcDetach(pin);
#else
    ledcSetup(channel, freq, bit_num); ledcWrite(getChannel(), duty); ledcDetachPin(pin);
#endif
```
So the current master compiles on core 3.x. **Important S3 detail:** on ESP32-S3 this library now prefers
**MCPWM** hardware for servos (`#include "driver/mcpwm.h"`, `mcpwm_init()`, `mcpwm_set_duty()`,
`mcpwm_gpio_init()`) and falls back to LEDC. Channel budget on S3: 8 LEDC + 2 MCPWM units × 3 timers ×
2 operators = up to 20 PWM outputs. `hasPwm()` on S3 accepts GPIO 1–21, 35–45, 47, 48.
Exact library release/version number: **UNVERIFIED** (I read master, not the tag history).

### Recommended way to drive one SG90 at 50 Hz, 500–2500 µs
```cpp
#include <ESP32Servo.h>
Servo sg90;

void setup() {
  sg90.setPeriodHertz(50);              // 50 Hz refresh (already the default)
  sg90.attach(4, 500, 2500);            // pin, min_us, max_us  -> full SG90 travel
  sg90.write(90);                       // 90 deg  (~1500 us)
}
void loop() {
  sg90.writeMicroseconds(1000);         // explicit pulse width is the most predictable
  delay(1000);
  sg90.writeMicroseconds(2000);
  delay(1000);
}
```
SG90 caveat: true spec is ~500–2400 µs for 0–180°; using `attach(pin, 500, 2400)` avoids pushing the
gears against the mechanical stop. Power the servo from a separate 5 V supply with a common ground —
an S3 GPIO cannot source servo current.
Sources: https://github.com/madhephaestus/ESP32Servo (`src/ESP32Servo.h`, `src/ESP32PWM.h`,
`src/ESP32PWM.cpp`), https://github.com/espressif/arduino-esp32/tree/master/libraries .

---

## 3. I2C / Wire on ESP32-S3

```cpp
bool     Wire.begin();                                            // default pins
bool     Wire.begin(int sdaPin, int sclPin, uint32_t frequency);  // master
bool     Wire.setPins(int sdaPin, int sclPin);                     // before begin()
bool     Wire.setClock(uint32_t frequency);
uint32_t Wire.getClock();
void     Wire.setTimeOut(uint16_t timeOutMillis);   // default 50 ms
uint16_t Wire.getTimeOut();
void     Wire.beginTransmission(uint16_t address);
uint8_t  Wire.endTransmission(bool sendStop);       // or endTransmission()
uint8_t  Wire.requestFrom(uint16_t address, uint8_t size, bool sendStop);
bool     Wire.end();
```
- Typical: `Wire.begin(8, 9, 400000);` on S3 (GPIO8 = SDA, GPIO9 = SCL) — **any** GPIO can be routed by
  the S3 GPIO matrix, so 8/9 are a convention, not a hardware limit. (The documented "generic ESP32"
  defaults 21/22 do **not** apply to the S3 variant.)
- `setTimeOut()` takes **milliseconds** (default 50 ms) — matches the ESP-IDF `i2c_master` timeout.
- Call `setPins()`/`setClock()` **before** `begin()` if you want defaults overridden.

### I2C scan pattern
```cpp
#include <Wire.h>
void setup() {
  Serial.begin(115200);
  Wire.begin(8, 9, 400000);
  Wire.setTimeOut(50);
}
void loop() {
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission(false) == 0)          // no STOP: keeps the bus for probing
      Serial.printf("I2C device at 0x%02X\n", addr);
  }
  delay(3000);
}
```
Source: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2c.html

---

## 4. ADC on ESP32-S3

```cpp
uint16_t analogRead(uint8_t pin);
uint32_t analogReadMilliVolts(uint8_t pin);          // calibrated, mV
void     analogReadResolution(uint8_t bits);         // default 12 (0..4095); range 1..16
void     analogSetAttenuation(adc_attenuation_t attenuation);
void     analogSetPinAttenuation(uint8_t pin, adc_attenuation_t attenuation);
void     analogSetWidth(uint8_t bits);               // ESP32 (original) only - NOT S3
```
- **Default resolution = 12 bits (0–4095) for all chips including S3.** `analogReadResolution(bits)` just
  shifts the returned value on S3 (hardware stays 12-bit); on original ESP32 it changes hardware width,
  range 9–12.
- **Default attenuation:** `analogRead` uses the IDF default; the ADC-continuous API documents
  "default is 11 dB". The attenuation enum values are `ADC_0db`, `ADC_2_5db`, `ADC_6db`, `ADC_11db`.
  Measurable range at `ADC_11db` on S3: **150 mV – 3100 mV** (approx; ~0–3.3 V in practice with the
  ESP32-S3 calibration). **Partially UNVERIFIED**: the docs table gives ranges per attenuation but does
  not state the exact default applied to a bare `analogRead()` call; treat "11 dB default" as
  near-certain but confirm on hardware.
- Practical S3 setup: `analogReadResolution(12); analogSetAttenuation(ADC_11db);` then
  `analogReadMilliVolts(pin)` for a calibrated reading (raw `analogRead` is non-calibrated).
- Removed in 3.0: `analogSetClockDiv`, `adcAttachPin`, `analogSetVRefPin`.

### ESP32-S3 ADC pins — GPIO1/GPIO2 **are** valid ADC1 pins (confirmed)
From `soc/adc_channel.h` / ESP-IDF docs macros:
| Unit | GPIO → channel |
|---|---|
| **ADC1** | GPIO1=CH0, GPIO2=CH1, GPIO3=CH2, GPIO4=CH3, GPIO5=CH4, GPIO6=CH5, GPIO7=CH6, GPIO8=CH7, GPIO9=CH8, GPIO10=CH9 |
| **ADC2** | GPIO11=CH0 … GPIO20=CH9 |

So the user's assumption is correct: **ADC1 = GPIO1…GPIO10**, ADC2 = GPIO11…GPIO20. Caveats:
- Use **ADC1** while Wi-Fi is active (ADC2 is shared with the Wi-Fi driver on ESP32/S2/S3 — an IDF
  limitation; on S3 `analogRead` will generally work but ADC1 is the safe choice).
- `analogContinuous*` (DMA continuous mode) supports **only ADC1** pins.
- GPIO1/GPIO2 are also the default UART0 TX/RX on many S3 boards — if you use them for ADC, do not also
  expect `Serial` on those pins.
Sources: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html ,
https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc/index.html ,
https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html

---

## 5. FreeRTOS in an .ino sketch

- `xTaskCreatePinnedToCore` is available (it is a macro/alias for `xTaskCreatePinnedToCore` on dual-core
  targets; arduino-esp32 also provides `xTaskCreateUniversal(...)` which maps to
  `xTaskCreatePinnedToCore` when `CONFIG_FREERTOS_UNICORE` is not set — this is exactly what the core uses
  for `loopTask`).
- `vTaskDelay(pdMS_TO_TICKS(ms))`, `vTaskDelayUntil`, mutexes/queues/semaphores, `xTaskGetCurrentTaskHandle()`,
  `uxTaskGetStackHighWaterMark()` — all available; just `#include <Arduino.h>` (which pulls in FreeRTOS).
- **`loop()` runs in its own FreeRTOS task**, named **`loopTask`**, created in `cores/esp32/main.cpp`:
  ```cpp
  xTaskCreateUniversal(loopTask, "loopTask", getArduinoLoopTaskStackSize(), NULL, 1, &loopTaskHandle, ARDUINO_RUNNING_CORE);
  ```
- **Stack size:** `getArduinoLoopTaskStackSize()` returns `ARDUINO_LOOP_STACK_SIZE`, which is
  `CONFIG_ARDUINO_LOOP_STACK_SIZE` if defined, otherwise the hard-coded default **8192 bytes**.
  Override per-sketch with `-DARDUINO_LOOP_STACK_SIZE=16384` or define it before including Arduino.h
  (note the core defines it if absent, so the build flag is the clean route).
- **Core:** `ARDUINO_RUNNING_CORE` — selectable in the IDE via **Tools → Arduino Runs On → Core 0/Core 1**
  (`boards.txt`: `esp32s3.menu.LoopCore.1 = Core 1`, `.0 = Core 0`, implemented as
  `-DARDUINO_RUNNING_CORE=0/1`). Default is **Core 1** for ESP32/S3 boards. Task priority of `loopTask`
  is **1** (low). Wi-Fi/lwIP and the Arduino event task run on `ARDUINO_EVENT_RUNNING_CORE` (Tools →
  Events Run On).
- Consequence for the port: blocking in `loop()` does **not** block Wi-Fi, but it does block everything
  else you put in `loop()`. Prefer pinning your own tasks (`xTaskCreatePinnedToCore`) and use
  `vTaskDelay()` rather than `delay()` inside tasks (same thing, but clearer).
Sources: `cores/esp32/main.cpp`, `boards.txt` (esp32s3 section), FreeRTOS.h in the core.

---

## 6. WiFi + MQTT (PubSubClient)

### WiFi (`#include <WiFi.h>`)
```cpp
wl_status_t begin(const char* ssid, const char* passphrase = NULL, int32_t channel = 0,
                  const uint8_t* bssid = NULL, bool tryConnect = true);
wl_status_t begin();
bool connect(const char* ssid, const char* passphrase = NULL, ...);
bool disconnect(bool wifioff = false, bool eraseap = false);
bool reconnect();  bool isConnected();  bool setAutoReconnect(bool);  bool getAutoReconnect();
IPAddress localIP();  wl_status_t status();
bool setHostname(const char*);   // must be called BEFORE WiFi.begin()/mode()
```
`WiFi.onEvent()` — **three overloads** in core 3.x:
```cpp
typedef void (*WiFiEventCb)(arduino_event_id_t);
wifi_event_id_t onEvent(WiFiEventCb, arduino_event_id_t = ARDUINO_EVENT_MAX);

typedef void (*WiFiEventSysCb)(arduino_event_t*);   // struct { arduino_event_id_t event_id; arduino_event_info_t event_info; }
wifi_event_id_t onEvent(WiFiEventSysCb, arduino_event_id_t = ARDUINO_EVENT_MAX);

typedef std::function<void(arduino_event_id_t, arduino_event_info_t)> WiFiEventFuncCb;
wifi_event_id_t onEvent(WiFiEventFuncCb, arduino_event_id_t = ARDUINO_EVENT_MAX);
```
Event IDs changed in 3.x from `system_event_id_t` (`SYSTEM_EVENT_STA_GOT_IP`) to `arduino_event_id_t`
(`ARDUINO_EVENT_WIFI_STA_GOT_IP`, `ARDUINO_EVENT_WIFI_STA_DISCONNECTED`, …). **Callbacks run in a
separate task — they must be thread-safe, and you must never call `WiFi.onEvent()`/`removeEvent()` from
inside a callback.**
```cpp
WiFi.onEvent([](arduino_event_id_t id, arduino_event_info_t info) {
  switch (id) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("IP: %s\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      WiFi.reconnect();
      break;
    default: break;
  }
});
```

### PubSubClient (verified from `src/PubSubClient.h`, master)
```cpp
PubSubClient();
PubSubClient(Client& client);
PubSubClient(IPAddress, uint16_t, Client&);
PubSubClient(const char* domain, uint16_t, Client&);       // + Stream&, + callback variants

PubSubClient& setServer(IPAddress ip, uint16_t port);
PubSubClient& setServer(uint8_t* ip, uint16_t port);
PubSubClient& setServer(const char* domain, uint16_t port);
PubSubClient& setCallback(MQTT_CALLBACK_SIGNATURE);        // std::function<void(char*,uint8_t*,unsigned int)> on ESP32
PubSubClient& setClient(Client&);
PubSubClient& setStream(Stream&);
PubSubClient& setKeepAlive(uint16_t keepAlive);            // seconds, default 15
PubSubClient& setSocketTimeout(uint16_t timeout);          // seconds, default 15

boolean  setBufferSize(uint16_t size);   // returns false on alloc failure
uint16_t getBufferSize();

boolean connect(const char* id);
boolean connect(const char* id, const char* user, const char* pass);
boolean connect(const char* id, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage);
boolean connect(const char* id, const char* user, const char* pass, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage);
boolean connect(const char* id, const char* user, const char* pass, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage, boolean cleanSession);

void    disconnect();
boolean publish(const char* topic, const char* payload);
boolean publish(const char* topic, const char* payload, boolean retained);
boolean publish(const char* topic, const uint8_t* payload, unsigned int plength);
boolean publish(const char* topic, const uint8_t* payload, unsigned int plength, boolean retained);
boolean publish_P(const char* topic, const char* payload, boolean retained);
boolean beginPublish(const char* topic, unsigned int plength, boolean retained);
int     endPublish();
boolean subscribe(const char* topic);
boolean subscribe(const char* topic, uint8_t qos);         // MQTTQOS0/1/2
boolean unsubscribe(const char* topic);
boolean loop();
boolean connected();
int     state();     // MQTT_CONNECTED 0, MQTT_DISCONNECTED -1, MQTT_CONNECT_FAILED -2, MQTT_CONNECTION_LOST -3,
                     // MQTT_CONNECTION_TIMEOUT -4, MQTT_CONNECT_BAD_CREDENTIALS 4, MQTT_CONNECT_UNAUTHORIZED 5, ...
```
Key defaults: **`MQTT_MAX_PACKET_SIZE 256`** (so ~256-byte limit until you raise it),
`MQTT_KEEPALIVE 15` s, `MQTT_SOCKET_TIMEOUT 15` s, `MQTT_MAX_HEADER_SIZE 5`. `MQTT_MAX_TRANSFER_SIZE` is
commented out by default (needed only for the old Arduino WiFi shield). Raise the buffer with
`mqtt.setBufferSize(1024);` **before** `connect()` (or define `-DMQTT_MAX_PACKET_SIZE=1024`), and note the
limit applies to the whole packet including topic + header — a payload of N bytes needs roughly N+~10.
`client.loop()` must be called regularly (it also handles keepalive pings and the callback dispatch).
Sources: https://github.com/knolleary/pubsubclient (`src/PubSubClient.h`),
https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html

---

## 7. BLE in arduino-esp32 3.x

**Yes — `BLEDevice.h`, `BLEServer`, `BLECharacteristic`, `BLEScan`, `BLEAdvertising`, `BLE2902`,
notifications, etc. all still exist** in the bundled `BLE` library (S3 is ✅ in the core's feature matrix).
The classic Kolban API is preserved; the 2.x→3.x breaking changes are:

1. Return/parameter types changed from `std::string` → Arduino **`String`**.
2. UUID data type changed from `uint16_t` → the **`BLEUUID`** class.
3. `BLEScan::start()` and `BLEScan::getResults()` return type changed from `BLEScanResults` →
   **`BLEScanResults*`** (pointer) — e.g. `BLEScanResults* foundDevices = pBLEScan->start(5, false);`
   then `foundDevices->getCount()` and `pBLEScan->clearResults()`.
4. `BLEDevice::init()` now returns **`bool`** (`static bool init(String deviceName = "")`), not `void`.
5. **Big one:** the same `BLE` library now has a **NimBLE backend**. `BLEDevice.h` is guarded by
   `#if defined(CONFIG_BLUEDROID_ENABLED) || defined(CONFIG_NIMBLE_ENABLED)` and exposes
   `BLEDevice::getBLEStack()` / `getBLEStackString()` returning `BLEStack::BLUEDROID | NIMBLE | UNKNOWN`.
   Many callbacks gained *additional* overloads for the NimBLE path, while the classic signatures remain:
   ```cpp
   class BLEServerCallbacks {
     virtual void onConnect(BLEServer* pServer);
     virtual void onDisconnect(BLEServer* pServer);
     // Bluedroid-only extras:
     virtual void onConnect(BLEServer*, esp_ble_gatts_cb_param_t*);
     virtual void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t*);
     virtual void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t*);
     // NimBLE-only extras:
     virtual void onConnect(BLEServer*, ble_gap_conn_desc*);
     virtual void onDisconnect(BLEServer*, ble_gap_conn_desc*);
   };
   class BLECharacteristicCallbacks {
     virtual void onRead(BLECharacteristic*);
     virtual void onWrite(BLECharacteristic*);
     virtual void onNotify(BLECharacteristic*);
     virtual void onStatus(BLECharacteristic*, Status s, uint32_t code);
   };
   ```
   → **The single-argument `onConnect(BLEServer*)` / `onDisconnect(BLEServer*)` / `onWrite(BLECharacteristic*)`
   forms still compile and are the portable choice.** If you override the two-argument forms you are
   pinning yourself to a specific stack.
   Also new: `BLEServer::advertiseOnDisconnect(bool enable)` (so you don't have to restart advertising
   manually) — availability is conditional on the stack (compiled out for NimBLE extended advertising).
6. `BLECharacteristic` API (unchanged names, exact signatures):
   ```cpp
   void    setValue(const uint8_t* data, size_t size);
   void    setValue(const String& value);
   void    setValue(uint16_t / uint32_t / int / float / double);
   String  getValue() const;
   uint8_t* getData();
   size_t  getLength() const;
   void    notify(bool is_notification = true);
   void    indicate();
   void    setCallbacks(BLECharacteristicCallbacks*);
   void    addDescriptor(BLEDescriptor*);
   uint16_t getHandle() const;
   ```
7. Practical notes: the `BLE2902` descriptor is **not required when the NimBLE stack is used** (NimBLE adds
   the CCCD automatically from the characteristic properties) but is harmless/needed for Bluedroid.
   The BLE library reserves Bluetooth memory automatically; if you call ESP-IDF BLE APIs directly you must
   `#include "esp32-hal-alloc-ble-mem.h"` in at least one file. `BLEDevice::deinit(bool release_memory = false)`
   exists to free the memory again. Do **not** mix BLE with Matter in the same sketch.

### NimBLE-Arduino as an alternative
The Community library `h2zero/NimBLE-Arduino` still exists and is the usual choice when you want a much
smaller/flash-friendlier BLE stack with a very similar API. In core 3.x you have a second option: the
**bundled** BLE library can be built against NimBLE, so you can keep `BLEDevice.h`/`BLEServer` source
compatibility and still change stacks. Exact minimum NimBLE-Arduino release tested against core 3.x:
**UNVERIFIED** (I did not fetch its release notes). If you use the bundled library you avoid that
dependency question entirely.

### Working BLE notify skeleton (core 3.x, portable form)
```cpp
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // write
#define CHAR_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // notify

BLECharacteristic* pTx = nullptr;
BLEServer*         pServer = nullptr;
volatile bool      connected = false;

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override    { connected = true;  }
  void onDisconnect(BLEServer* s) override { connected = false; s->startAdvertising(); }
};
class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    Serial.printf("RX: %s\n", v.c_str());
  }
};

void setup() {
  BLEDevice::init("SmartHome-S3");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new SrvCb());
  BLEService* svc = pServer->createService(SERVICE_UUID);

  pTx = svc->createCharacteristic(CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pTx->addDescriptor(new BLE2902());                 // required for Bluedroid, optional for NimBLE
  BLECharacteristic* pRx = svc->createCharacteristic(CHAR_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  pRx->setCallbacks(new RxCb());

  svc->start();
  pServer->getAdvertising()->start();
}
void loop() {
  static uint32_t n = 0;
  if (connected) { uint32_t v = n++; pTx->setValue((uint8_t*)&v, sizeof(v)); pTx->notify(); }
  delay(1000);
}
```
Sources: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ble.html ,
https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html ,
`libraries/BLE/src/BLEDevice.h`, `BLEServer.h`, `BLECharacteristic.h` (master).

---

## 8. U8g2 + SSD1306 128×64 I2C

### Constructor
`U8G2_SSD1306_128X64_NONAME_F_HW_I2C` **exists** and is the full-framebuffer (F) hardware-I2C variant.
Its constructor takes `(rotation, reset, clock, data)`:
```cpp
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ U8X8_PIN_NONE, /* data=*/ U8X8_PIN_NONE);
```
**Clock (SCL) comes BEFORE data (SDA)** — this is the classic mistake. With `U8X8_PIN_NONE` for clock/data,
u8g2 uses the Arduino `Wire` default pins (`Wire.begin()` / your `Wire.begin(sda,scl)`).
To force pins through u8g2 instead: `U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 9 /*SCL*/, 8 /*SDA*/);`
Read/verify with `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` for non-default I2C ports: `..._F_2ND_HW_I2C` (second
hardware I2C = `Wire1`); `..._F_SW_I2C` for bit-banged I2C.

Suffix meaning: `_F_` = full RAM framebuffer (1024 bytes for 128×64, fastest, needs `clearBuffer()` +
`sendBuffer()`); `_1_` = page buffer of 1 tile row (~128 bytes); `_2_` = 2 tile rows. **For the S3 use the
`_F_` variant** — 1 KB of RAM is nothing.

### API (exact signatures)
```cpp
bool begin();                                   // init display + clear
void clearBuffer();
void sendBuffer();
void setFont(const uint8_t* font);
u8g2_uint_t drawStr (u8g2_uint_t x, u8g2_uint_t y, const char* s);
u8g2_uint_t drawUTF8(u8g2_uint_t x, u8g2_uint_t y, const char* s);   // required for Chinese
void setI2CAddress(uint8_t adr);                // forwards to u8x8_SetI2CAddress(); 8-bit form!
void setPowerSave(uint8_t is_enable);
void setContrast(uint8_t value);
void setBitmapMode(uint8_t is_transparent);
```
`u8g2_uint_t` is `uint16_t` on 32-bit targets (U8G2_16BIT is force-enabled on `ESP_PLATFORM`), so the
"240-pixel" 8-bit limitation does not apply here.

**`setI2CAddress` gotcha (verified from source):**
```c
#define u8g2_SetI2CAddress(u8g2, address)  ((u8g2_GetU8x8(u8g2))->i2c_address = (address))
#define u8x8_SetI2CAddress(u8x8, address)  ((u8x8)->i2c_address = (address))
```
and the `u8x8_t` field comment says: *"i2c_address is the address for writing data to the display; usually,
the lowest bit must be zero for a valid address"*. So the value is the **8-bit (already shifted) address**:
for a 0x3C module use `u8g2.setI2CAddress(0x3C << 1);  // = 0x78`. Passing the raw 7-bit 0x3C makes the
display stay blank. Also call it **before `begin()`**, because the address field is (re)assigned during
display init. Reported upstream: https://github.com/olikraus/u8g2/issues/821 . The exact "does init
overwrite a previously set address" semantics: **UNVERIFIED** — but calling it before `begin()` is the
safe, universally-working order.

### Chinese fonts — exact names (verified against u8g2's own generated font list)
All of these exist in `tools/font/build/u8g2_font_list.c` (the authoritative list of shipped fonts):
- **WenQuanYi bitmaps (recommended for 128×64 Chinese: 12 px):**
  `u8g2_font_wqy12_t_gb2312`, `u8g2_font_wqy12_t_gb2312a`, `u8g2_font_wqy12_t_gb2312b`
  also `wqy12_t_chinese1`, `wqy12_t_chinese2`, `wqy12_t_chinese3`
  and the same families at **13, 14, 15, 16 px**: `u8g2_font_wqy13_t_gb2312[…]`, `wqy14_…`, `wqy15_…`, `wqy16_…`
  (each with `_gb2312`, `_gb2312a`, `_gb2312b`, `_chinese1`, `_chinese2`, `_chinese3`).
  → **There is no `u8g2_font_wqy9_t_gb2312`** — 12 px is the smallest wqy font. **UNVERIFIED for wqy10/11**
  (not present in the list I read).
- **unifont (16 px CJK, larger coverage):** `u8g2_font_unifont_t_chinese1`, `_chinese2`, `_chinese3`,
  `u8g2_font_unifont_t_gb2312`, `u8g2_font_unifont_t_gb2312a`, `u8g2_font_unifont_t_gb2312b`.
- **Fixed-size GB fonts:** `u8g2_font_gb16st_t_1/2/3` (16 px), `u8g2_font_gb24st_t_1/2/3` (24 px).
- **Boutique bitmap CJK:** `u8g2_font_boutique_bitmap_7x7_t_gb2312[ab]`,
  `u8g2_font_boutique_bitmap_9x9_t_gb2312[ab]`, `u8g2_font_boutique_bitmap_9x9_bold_t_gb2312[ab]`
  (plus `_chinese1/2/3` variants of each).

Naming rules: `_t_` = **transparent** background (what you want for `drawUTF8`), `_h_`/`_m_` = different
pixel modes; the trailing `_gb2312` covers the GB2312 charset (6763 hanzi × 2 variants), `_gb2312a` and
`_gb2312b` are the two GB2312 halves/planes, `_chinese1/2/3` are unifont/wqy coverage subsets. Use
`drawUTF8()` (not `drawStr()`) and keep the source file **UTF-8** encoded (u8g2's `U8G2_WITH_UNICODE` is
enabled by default and expects UTF-8 C strings). A GB2312 font costs flash: wqy12 is roughly a few hundred
KB, unifont_chinese* well over a megabyte — with 16 MB flash and a 3 MB+ app partition that is fine.

### Complete working snippet
```cpp
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ U8X8_PIN_NONE, /* data=*/ U8X8_PIN_NONE);

void setup() {
  Wire.begin(8, 9, 400000);          // SDA=8, SCL=9 (before u8g2.begin())
  u8g2.setI2CAddress(0x3C << 1);     // 8-bit form! (default is already 0x3C<<1 = 0x78)
  u8g2.begin();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
}
void loop() {
  u8g2.clearBuffer();
  u8g2.drawUTF8(0, 14, "温度: 25.3C");     // file must be saved as UTF-8
  u8g2.drawUTF8(0, 30, "湿度: 48%");
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 46, "MQTT: connected");
  u8g2.sendBuffer();
  delay(1000);
}
```
Sources: https://github.com/olikraus/u8g2 (`csrc/u8g2.h`, `csrc/u8x8.h`,
`tools/font/build/u8g2_font_list.c`), https://github.com/olikraus/u8g2/issues/821

---

## 9. ESP-SR / offline wake word + command words in an .ino — **yes, this is now officially possible**

**This is the biggest correction to the usual "you must stay in ESP-IDF" answer.** arduino-esp32 ships a
bundled **`ESP_SR` library** with a working `Basic.ino` example:

```
libraries/ESP_SR/
├── examples/Basic/Basic.ino
├── keywords.txt
├── library.properties
└── src/  ESP_SR.h, ESP_SR.cpp, esp32-hal-sr.h, esp32-hal-sr.c
```
The public API (verbatim from `libraries/ESP_SR/src/ESP_SR.h`):
```cpp
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4) && (CONFIG_MODEL_IN_FLASH || CONFIG_MODEL_IN_SDCARD)
typedef void (*sr_cb)(sr_event_t event, int command_id, int phrase_id);

class ESP_SR_Class {
public:
  void onEvent(sr_cb cb);
  bool begin(I2SClass &i2s, const sr_cmd_t *sr_commands, size_t sr_commands_len,
             sr_channels_t rx_chan = SR_CHANNELS_STEREO, sr_mode_t mode = SR_MODE_WAKEWORD,
             const char *input_format = "MN");
  bool end(void);
  bool setMode(sr_mode_t mode);
  bool pause(void);
  bool resume(void);
};
extern ESP_SR_Class ESP_SR;
#endif
```
So: `#include <ESP_SR.h>`, give it an `I2SClass` (INMP441 / I2S MEMS mic) and a `sr_cmd_t` table, and you
get **WakeNet wake-word + MultiNet command-word recognition inside a normal .ino sketch**.

Conditions/limitations (all verified):
- **ESP32-S3 (or P4) only**, and only when the core was built with `CONFIG_MODEL_IN_FLASH` or
  `CONFIG_MODEL_IN_SDCARD`.
- It requires a **model partition**: `boards.txt` has a dedicated scheme for it —
  `esp32s3.menu.PartitionScheme.esp_sr_16 = ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)` with
  `upload.extra_flags=0xC10000 {build.path}/srmodels.bin` (i.e. the model blob is flashed at 0xC10000).
  **Tools → Partition Scheme → "ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"** is therefore mandatory.
  That scheme caps the app at 3 MB — factor that into your flash budget.
- Mode is selected with `SR_MODE_WAKEWORD` / the equivalent command mode via `setMode()`; both engines are
  the same proprietary ESP-SR models used in ESP-IDF (WakeNet + MultiNet). Model licensing is Espressif's
  ESP-SR license (models are **not** open source even though the glue code is) — see
  https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/index.html for the AFE / WakeNet / MultiNet
  guides, and the repo https://github.com/espressif/esp-sr .
- ESP-SR needs a decent I2S mic and the AFE pipeline; expect to spend real time on mic gain/AEC tuning.

**Definitive recommendation:** if your voice features are "one wake word + a handful of command phrases",
do them **in the Arduino sketch** with the bundled `ESP_SR` library — it works and is officially supported.
Only drop back to ESP-IDF if you need (a) a chip other than S3/P4, (b) the ESP-IDF `voice_assistant`
example's full pipeline, or (c) custom-trained models. The old community "ESP-SR Arduino port" is a 2.x-era
dead end — don't use it.
Caveat: the `ESP_SR` library is a recent addition; if your installed core predates it,
`libraries/ESP_SR` will be missing. Check your core's `libraries` folder (or update the core).
Sources: `libraries/ESP_SR/src/ESP_SR.h`, `libraries/ESP_SR/examples/Basic/`, `boards.txt`
(`esp32s3.menu.PartitionScheme.esp_sr_16`), https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/index.html

---

## 10. NVS / Preferences

`#include <Preferences.h>`
```cpp
bool begin(const char* name, bool readOnly = false, const char* partition_label = NULL);
void end();
bool clear();
bool remove(const char* key);
bool isKey(const char* key);

size_t putChar (const char* key, int8_t   value);
size_t putUChar(const char* key, uint8_t  value);       // returns 1 on success, 0 on failure
size_t putShort(const char* key, int16_t  value);
size_t putUShort(const char* key, uint16_t value);      // returns 2
size_t putInt  (const char* key, int32_t  value);
size_t putUInt (const char* key, uint32_t value);
size_t putLong (const char* key, int32_t  value);
size_t putULong(const char* key, uint32_t value);
size_t putFloat(const char* key, float    value);       // returns 4
size_t putLong64(const char* key, int64_t  value);
size_t putULong64(const char* key, uint64_t value);
size_t putDouble(const char* key, double   value);      // returns 8
size_t putBool (const char* key, bool      value);
size_t putString(const char* key, const char* value);
size_t putString(const char* key, String    value);     // returns bytes stored
size_t putBytes (const char* key, const void* value, size_t len);

int8_t   getChar (const char* key, int8_t   defaultValue = 0);
uint8_t  getUChar(const char* key, uint8_t  defaultValue = 0);
int16_t  getShort(const char* key, int16_t  defaultValue = 0);
uint16_t getUShort(const char* key, uint16_t defaultValue = 0);
int32_t  getInt  (const char* key, int32_t  defaultValue = 0);
uint32_t getUInt (const char* key, uint32_t defaultValue = 0);
int32_t  getLong (const char* key, int32_t  defaultValue = 0);
uint32_t getULong(const char* key, uint32_t defaultValue = 0);
int64_t  getLong64(const char* key, int64_t defaultValue = 0);
uint64_t getULong64(const char* key, uint64_t defaultValue = 0);
float    getFloat (const char* key, float  defaultValue = NAN);
double   getDouble(const char* key, double defaultValue = NAN);
bool     getBool (const char* key, bool   defaultValue = false);
size_t   getString(const char* key, char* value, size_t maxLen);   // -> bytes incl. NUL, 0 on failure
String   getString(const char* key, String defaultValue = String());
size_t   getStringLength(const char* key);
size_t   getBytes(const char* key, void* buf, size_t maxLen);
size_t   getBytesLength(const char* key);
PreferenceType getType(const char* key);     // 10 == PT_INVALID on failure
size_t   freeEntries();
```
Confirmed: **namespaces and keys are limited to 15 characters**; `getFloat` default is `NAN`; `putFloat`
returns 4; `getString(key, char*, maxLen)` needs `maxLen` ≥ stored length *including* the NUL (use
`getStringLength()` first). Float/double keys consume 3 key-table entries, strings ≥ 2 (relevant to
`freeEntries()`). Always `end()` (or at least don't keep the namespace open) before deep sleep — NVS
writes are committed on `end()`/per put depending on the API. Example:
```cpp
Preferences prefs;
prefs.begin("smarthome", false);           // namespace, read-write
float t = prefs.getFloat("t_set", 21.0f);
prefs.putFloat("t_set", 22.5f);
String ssid = prefs.getString("ssid", "");
bool first = !prefs.isKey("ssid");
prefs.remove("old_key");
prefs.end();
```
Source: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html

---

## 11. Board settings and partitions (ESP32-S3, 16 MB)

Board: **Tools → Board → esp32 → "ESP32S3 Dev Module"** (`boards.txt` id `esp32s3`). Menus (exact labels
from `boards.txt`):
| Menu | Options (esp32s3) | Recommended for N16R8 |
|---|---|---|
| USB CDC On Boot | `Disabled` / `Enabled` | **Enabled** if you want `Serial` over the native USB port; leave Disabled and use the UART bridge if you see nothing |
| USB Mode | `Hardware CDC and JTAG` / `USB-OTG (TinyUSB)` | Hardware CDC and JTAG (default) |
| USB Firmware MSC On Boot / USB DFU On Boot | Disabled/Enabled | Disabled |
| Upload Mode | `UART0 / Hardware CDC` / `USB-OTG CDC (TinyUSB)` | UART0 / Hardware CDC |
| CPU Frequency | 240/160/80/40/20/10 MHz | 240 MHz (WiFi) |
| Flash Mode | QIO / DIO | QIO 80 MHz (typical) |
| Flash Size | 2/4/8/**16MB (128Mb)** | **16MB (128Mb)** |
| **PSRAM** | `Disabled` / `QSPI PSRAM` / `OPI PSRAM` | **OPI PSRAM** for N16**R8** (octal). Wrong choice ⇒ PSRAM not detected / boot loop |
| **Partition Scheme** | see below | depends (see below) |
| Arduino Runs On | Core 0 / Core 1 | Core 1 (default) |
| Events Run On | Core 0 / Core 1 | Core 0 |
| Core Debug Level | None…Verbose | None |
| Erase All Flash Before Sketch Upload | Disabled/Enabled | Disabled |

`Flash Size` only tells the toolchain/esptool the real size (and validates the partition table against it);
it does **not** by itself enlarge the app partition — that is the Partition Scheme's job.

**Default partition scheme on S3:** `esp32s3.build.partitions=default` →
**"Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)"**. That is the trap: on a 16 MB module you get a
1.2 MB app partition unless you change it.

Relevant S3 schemes (exact labels + max app size from boards.txt):
| Label | App size |
|---|---|
| Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS) | 1 310 720 |
| Default 4MB with ffat (1.2MB APP/1.5MB FATFS) | 1 310 720 |
| 8M with spiffs (3MB APP/1.5MB SPIFFS) | 3 342 336 |
| Minimal (1.3MB APP/700KB SPIFFS) | ~1.3 MB |
| No OTA (2MB APP/2MB SPIFFS) | 2 097 152 |
| **Huge APP (3MB No OTA/1MB SPIFFS)** | **3 145 728** |
| Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS) | 1 966 080 |
| 16M Flash (2MB APP/12.5MB FATFS) | 2 097 152 |
| **16M Flash (3MB APP/9.9MB FATFS)** | **3 145 728** |
| **ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)** | 3 145 728 (flashes `srmodels.bin` @0xC10000) |
| RainMaker 4MB / 4MB No OTA / 8MB | 1 966 080 / 4 038 656 / 4 096 000 |
| Zigbee ZCZR 4MB / 8MB | 1 310 720 / 3 407 872 |
| **Custom** | `build.partitions=` (empty), `upload.maximum_size=16777216` |

**How to get more app space with 16 MB flash:**
1. If you don't need OTA and want the largest *predefined* app: **"Huge APP (3MB No OTA/1MB SPIFFS)"** — but
   note it is laid out for 4 MB flash, so with `Flash Size = 16MB` you simply leave the upper 12 MB unused;
   **"16M Flash (3MB APP/9.9MB FATFS)"** is the same 3 MB app *plus* a 9.9 MB FATFS data partition, which is
   usually the better 16 MB choice.
2. For **more than 3 MB of app**, select **Partition Scheme → Custom** and supply your own table (place a
   `partitions.csv` in the sketch folder; the `custom` entry sets `build.partitions=` empty, which makes the
   build look for the sketch-provided CSV). Known wart: Arduino IDE has/had a bug where a sketch `partitions.csv`
   is ignored (https://github.com/arduino/arduino-ide/issues/2526) — if that bites, use `arduino-cli compile`
   with `--build-property build.partitions=...`, or use PlatformIO with `board_build.partitions`.
   **The exact sketch-folder CSV lookup rule for core 3.x is UNVERIFIED** (I verified the boards.txt
   `custom` entry, not the docs for the lookup).
   A 16 MB layout that works well (single app, no OTA):
   ```csv
   # Name,   Type, SubType, Offset,  Size, Flags
   nvs,      data, nvs,     0x9000,  0x5000,
   otadata,  data, ota,     0xe000,  0x2000,
   app0,     app,  ota_0,   0x10000, 0x600000,
   spiffs,   data, spiffs,  0x610000,0x9E0000,
   coredump, data, coredump,0xFF0000,0x10000,
   ```
3. Remember the OTA tradeoff: with a single 6 MB app partition there is **no** OTA (OTA needs two equal app
   slots). With OTA you are limited to ~half the flash, e.g. two 3 MB slots.
4. If you enable ESP-SR you must use the `esp_sr_16` scheme and accept a 3 MB app cap.

Sources: `boards.txt` (esp32s3 section, read in full),
https://docs.espressif.com/projects/arduino-esp32/en/latest/libraries.html ,
https://github.com/arduino/arduino-ide/issues/2526

---

## 12. WS2812 / NeoPixel

### Adafruit_NeoPixel (verified from `Adafruit_NeoPixel.h`)
```cpp
Adafruit_NeoPixel(uint16_t n, int16_t pin = 6, neoPixelType type = NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel(void);
bool     begin(void);                                  // returns bool
void     show(void);
void     setPin(int16_t p);
void     setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b);
void     setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void     setPixelColor(uint16_t n, uint32_t c);
void     fill(uint32_t c = 0, uint16_t first = 0, uint16_t count = 0);
void     setBrightness(uint8_t);        // 0..255, applied inside show(); NOT persisted across show ordering
uint8_t  getBrightness(void) const;
void     clear(void);
void     updateLength(uint16_t n);
void     updateType(neoPixelType t);
uint16_t numPixels(void) const;
uint32_t getPixelColor(uint16_t n) const;
uint8_t* getPixels(void) const;
bool     canShow(void);                 // true if show() won't have to wait out the 300us latch
static uint32_t Color(uint8_t r, uint8_t g, uint8_t b);
static uint32_t Color(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
static uint32_t ColorHSV(uint16_t hue, uint8_t sat = 255, uint8_t val = 255);
static uint8_t  gamma8(uint8_t x);   static uint32_t gamma32(uint32_t x);
```
For a standard WS2812B: `Adafruit_NeoPixel strip(NUM_LEDS, PIN, NEO_GRB + NEO_KHZ800);` then
`strip.begin(); strip.setBrightness(64); strip.setPixelColor(i, strip.Color(r,g,b)); strip.show();`
`setBrightness()` scales the values used by `show()` but does **not** modify the stored pixel data.

### ESP32-S3 / core 3.x caveats (verified from `esp.c`)
- The library has an explicit **ESP-IDF 5 path** (`#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)`), so
  it is core-3.x aware and uses the **new RMT API**:
  `rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000)` → build `rmt_data_t[]` → `rmtWrite(pin, data, numBytes*8, RMT_WAIT_FOR_EVER)` → `rmtDeinit(pin)`.
  It also declares `extern "C" void espInit();` for `ESP_IDF_VERSION >= 5.0.0`, called from `begin()` to
  create the internal mutex.
- **One shared RMT TX channel for all NeoPixel instances.** `esp.c` keeps a single static `rmtPin`; if you
  call `show()` on a strip bound to a *different* pin, it `rmtDeinit()`s and re-inits the channel for that
  pin — legal but slow and it means multiple strips on different pins are serialized. Also **all
  `Adafruit_NeoPixel` objects must be constructed before starting other tasks** (mutex race).
- To release the RMT channel: `strip.updateLength(0); strip.show();` (documented in the source comment).
- ESP32-S3 has **4 RMT TX channels** (vs 8 on the original ESP32); the NeoPixel path uses one. Any
  output-capable GPIO works for RMT TX on the S3.
- Library version required: needs a release containing the `HAS_ESP_IDF_5` branch. **Exact minimum version
  number UNVERIFIED** — just use the latest from Library Manager (1.12+ era); if you see
  `driver/rmt.h`-style errors on core 3.x, your installed version is too old.
- **Built-in alternative (verified):** core 3.x provides
  ```c
  void rgbLedWrite(uint8_t pin, uint8_t red_val, uint8_t green_val, uint8_t blue_val);   // uses RGB_BUILTIN_LED_COLOR_ORDER (default GRB)
  void rgbLedWriteOrdered(uint8_t pin, rgb_led_color_order_t order, uint8_t r, uint8_t g, uint8_t b);
  [[deprecated("Use rgbLedWrite() instead.")]] void neopixelWrite(uint8_t p, uint8_t r, uint8_t g, uint8_t b);
  ```
  `neopixelWrite()` still exists but is **deprecated** — use `rgbLedWrite()`. These drive **one** LED only
  (no strip support); use Adafruit_NeoPixel / FastLED for strips.
Sources: https://github.com/adafruit/Adafruit_NeoPixel (`Adafruit_NeoPixel.h`, `esp.c`),
`cores/esp32/esp32-hal-rgb-led.h`, https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html (RMT changes)

---

## 13. Multi-file sketch structure (Arduino build rules)

Verified from the official arduino-cli build-process docs:
1. **All `.ino` and `.pde` files in the sketch folder are concatenated** into a single translation unit,
   **starting with the file whose name matches the folder name, then the others in alphabetical order**;
   the result is compiled as `<folder>.cpp`.
2. `#include <Arduino.h>` is added automatically to that concatenated file.
3. **Prototypes are generated only for functions defined in `.ino`/`.pde` files** — not for `.cpp`/`.c`.
4. **No pre-processing at all is done to any other extension.** `.h` files in the sketch folder are **not**
   auto-included — you must `#include "myfile.h"` yourself.
5. `.c` and `.cpp` files in the sketch folder are compiled as **separate translation units** (source files
   taken: `.S`, `.c`, `.cpp`). If a `.cpp` (or the generated sketch `.cpp`) calls a function defined in a
   `.c` file, wrap the declarations in `extern "C" { }` inside the C++ file.
6. Your own `.cpp` files **must** `#include <Arduino.h>` themselves if they use `String`, `Serial`, `pinMode`,
   etc. — the auto-include only applies to the concatenated sketch.
7. **Folder name must match the main `.ino` name.** The IDE enforces/creates this; the build looks for
   `<folder>/<folder>.ino` as the first file (that's how item 1 is ordered). A folder whose `.ino` doesn't
   match will not open/compile correctly in the IDE.
8. **Subfolders:** only a `src/` subfolder is compiled recursively (Arduino library-style layout);
   arbitrary subfolders are ignored. Keep flat or use `src/`. (The `src/` rule is from the Arduino library
   specification; the flat-sketch behavior is from the sketch build process doc.)

**Recommended layout for this port**
```
SmartHomeS3/                 <- folder name must equal main .ino name
├── SmartHomeS3.ino          <- main file: setup(), loop(), task creation
├── config.h                 <- pins, WiFi/MQTT credentials, feature flags (no code!)
├── ledc_pwm.h  / ledc_pwm.cpp
├── servo_ctrl.h / servo_ctrl.cpp
├── oled_ui.h   / oled_ui.cpp
├── net_mqtt.h  / net_mqtt.cpp
├── ble_srv.h   / ble_srv.cpp
├── storage_prefs.h / storage_prefs.cpp
└── partitions.csv           <- only if Partition Scheme = Custom
```
Gotchas:
- Put **only** `#include`s and `config.h` at the top of the main `.ino`, **before** any function that uses
  types from your headers. The auto-generated prototypes are inserted before your first function
  definition; if a prototype references a type that isn't declared yet, you get
  `'XxxType' does not name a type`. Fix: include the header in the `.ino` (and/or add explicit prototypes).
- Every `.cpp` must include its own header first, then `<Arduino.h>`.
- Don't sprinkle `static` globals with the same name across `.cpp` files; keep state in one owner and expose
  accessors (or `extern` in the header).
Source: https://github.com/arduino/arduino-cli/blob/master/docs/sketch-build-process.md ,
https://github.com/arduino/arduino-cli/blob/master/docs/sketch-specification.md

---

## Quick "what breaks when porting an ESP-IDF project" checklist

| ESP-IDF idiom | Arduino 3.x equivalent |
|---|---|
| `ledc_timer_config` + `ledc_channel_config` | `ledcAttach(pin, freq, bits)` + `ledcWrite(pin, duty)` |
| `ledc_set_freq` | `ledcChangeFrequency(pin, freq, bits)` (warning: shared timer) |
| `adc_oneshot_*` | `analogRead()` / `analogReadMilliVolts()`, `analogSetAttenuation()` |
| `i2c_master_*` | `Wire.begin(sda, scl, hz)`, `Wire.setTimeOut(ms)` |
| `nvs_open/nvs_set_*` | `Preferences` |
| `esp_wifi_*` + `esp_event_handler_register` | `WiFi.begin()` + `WiFi.onEvent()` |
| `esp_mqtt_client_*` | PubSubClient + `WiFiClient` |
| `esp_bt_*` / NimBLE C API | `BLEDevice` / `BLEServer` / `BLECharacteristic` (String + BLEUUID in 3.x) |
| `nvs_flash_init()` | automatic (`initArduino()`) |
| `xTaskCreatePinnedToCore` | identical, still available |
| `rmt_*` (IDF4 style) | changed in 3.x: `rmtInit/rmtWrite/rmtDeinit`, `rmtWrite` is now blocking, `rmtSetTick` removed (`rmtInit` takes a frequency) |
| ESP-SR (`esp_srmodel_init`, `afe_*`, `multinet_*`) | bundled `ESP_SR` class (`ESP_SR.begin(i2s, cmds, n, …)`) + `esp_sr_16` partition scheme, S3/P4 only |

## Items explicitly UNVERIFIED
- Exact default attenuation actually applied by a bare `analogRead()` on S3 (docs give the ranges and the
  continuous-mode default of 11 dB, not the oneshot default).
- Whether `u8g2.setI2CAddress()` called *after* `begin()` is overwritten by display init (calling it before
  `begin()` is safe and standard).
- Whether `u8g2_font_wqy10/wqy11_t_gb2312` exist (not in the font list I read; wqy12 is the smallest wqy).
- The precise minimum Adafruit_NeoPixel release number that added the IDF-5 RMT path.
- The precise minimum NimBLE-Arduino release number validated against core 3.x.
- The exact sketch-folder `partitions.csv` lookup rule under core 3.x (the `Custom` menu entry and its
  16 MB max are verified from boards.txt; IDE support has a known bug).
- ESP32Servo's exact release version/tag (I verified current master source).
- Whether the `ESP_SR` bundled library is present in *every* 3.x release (it is present in current master;
  verify in your installed core's `libraries/` folder).
