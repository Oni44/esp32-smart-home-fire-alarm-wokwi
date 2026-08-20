# ESP32 Smart Home Fire Alarm — IoT Final Project

Final Project Internet of Things (IoT) yang mengembangkan sistem **Smart Home Fire Alarm berbasis ESP32** menjadi sistem yang lebih **intelligent, secure, analytical, dan resilient**.

Project dijalankan menggunakan **Wokwi + PlatformIO**, terintegrasi dengan **ThingsBoard** melalui MQTT dan **Telegram Bot** untuk monitoring, notifikasi, serta remote control.

---

## Final Project Team

**Group 5 — BINUS Graduate Program, 2026**

- Andronikus Marintan
- Bobby Daniyal
- Naura Retyani Ajisasongko

---

## Final Project Improvements

Final Project menggunakan sistem IoT sebelumnya sebagai baseline dan menambahkan dua enhancement utama:

### 1. Adaptive Intelligence / Dynamic Threshold

Sistem tidak lagi hanya menggunakan static temperature threshold.

ESP32 membentuk **adaptive temperature baseline** menggunakan moving average dari data suhu normal, kemudian menghitung:

- Dynamic Warning Threshold
- Dynamic Fire Threshold
- Dynamic Clear Threshold
- Risk classification: `SAFE`, `WARNING`, `DANGER`

Konfigurasi utama:

```text
Adaptive Window          : 10 samples
Minimum Samples          : 5 samples
Warning Delta            : +4 °C
Fire Delta               : +8 °C
Clear Hysteresis         : 2 °C
Minimum Fire Threshold   : 30 °C
Critical Temperature     : 60 °C
```

Untuk menjaga keselamatan, adaptive model **tidak belajar dari kondisi abnormal**, yaitu ketika:

- Fire sedang aktif
- Gas/asap terdeteksi
- Temperatur telah mencapai warning threshold

Hard safety limit **60 °C** tetap dipertahankan sehingga temperatur kritis dapat memicu fire alarm meskipun gas belum terdeteksi.

---

### 2. IoT Security / Token-Based Authentication

Protected ThingsBoard Server-Side RPC menggunakan **application-level token authentication**.

Protected RPC meliputi:

```text
setLampBlue
setLampGreen
setLampRed
setWhitePwm
setFanPwm
openDoor
closeDoor
ringBell
muteAlarm
unmuteAlarm
```

Read-only RPC tetap dapat digunakan untuk monitoring:

```text
getLampBlue
getLampGreen
getLampRed
getWhitePwm
getFanPwm
getStatus
```

Security enhancement meliputi:

- RPC Security Token
- Missing token rejection
- Invalid token rejection
- Authentication failure counter
- Temporary security lock setelah 3 kegagalan
- Automatic unlock setelah 30 detik
- Security telemetry ke ThingsBoard
- `SECURITY_LOCKED` dan `SECURITY_UNLOCKED` event
- Read-only monitoring tetap tersedia ketika security lock aktif

Security lock hanya memblokir **remote control**, bukan local fire safety.

---

## Main Features

### Smart Home Monitoring

- DHT22 temperature and humidity monitoring
- Gas/smoke detection simulation
- Door/reed-switch monitoring
- LCD 16x2 local status display
- Lamp/LED control
- PWM lighting
- Fan PWM
- Servo door control
- Buzzer/bell

### Local Edge Fire Safety

Fire detection dan emergency action diproses langsung pada ESP32.

Ketika kondisi fire aktif:

- `fire_active = true`
- `risk_status = DANGER`
- Alarm buzzer aktif
- Alarm lamps aktif
- Fan effective PWM = `255`
- Servo membuka pintu
- LCD menampilkan fire warning
- Event `FIRE_STARTED` dibuat
- Telegram notification dikirim jika koneksi tersedia
- Telemetry dikirim ke ThingsBoard jika koneksi tersedia

### Safety Interlock

Remote command tidak dapat mengalahkan local safety rule.

Contoh:

```text
Valid RPC Token
      ↓
Authentication SUCCESS
      ↓
closeDoor
      ↓
Fire Active?
      ↓
YES
      ↓
Command REJECTED
```

Pintu tetap terbuka selama kondisi kebakaran aktif.

### ThingsBoard

- MQTT telemetry
- Advanced analytics dashboard
- Sensor monitoring
- Actuator status
- SAFE / WARNING / DANGER indicator
- Adaptive baseline
- Dynamic warning/fire threshold
- Time-series trend analytics
- Event history
- Alarm monitoring
- Server-side RPC remote control
- Security status monitoring

Security telemetry:

```text
auth_status
auth_failed_count
security_locked
last_auth_source
```

### Telegram

Telegram Bot digunakan untuk:

- Status monitoring
- Fire notification
- Gas/smoke notification
- Door notification
- Lamp control
- Fan control
- Servo door control
- Bell
- Alarm mute/unmute

### Persistent Event Buffer

Event penting disimpan menggunakan **ESP32 Preferences / NVS** ketika cloud tidak tersedia.

Event dapat dikirim kembali ke ThingsBoard setelah koneksi pulih.

Contoh event:

```text
SYSTEM_BOOT
GAS_DETECTED
GAS_CLEARED
DOOR_OPENED
DOOR_CLOSED
FIRE_STARTED
FIRE_CLEARED
ALARM_MUTED
ALARM_UNMUTED
WIFI_DISCONNECTED
WIFI_RECONNECTED
THINGSBOARD_DISCONNECTED
THINGSBOARD_RECONNECTED
SECURITY_LOCKED
SECURITY_UNLOCKED
```

---

## System Architecture

```text
                  SENSOR LAYER
       DHT22 / Gas / Door Sensor
                    |
                    v
            ESP32 EDGE PROCESSING
       +---------------------------+
       | Sensor Processing         |
       | Adaptive Intelligence     |
       | Fire Detection            |
       | Safety Interlock          |
       | Event Manager             |
       | Persistent Buffer         |
       +---------------------------+
          |                    |
          | Local Action       | Communication
          v                    v
 LED / Buzzer / Fan /      Wi-Fi / MQTT
 Servo / LCD                   |
                               v
                          ThingsBoard
                    Telemetry / Dashboard
                    Analytics / Event / RPC
                               |
                               v
                    RPC Token Authentication
                               |
                               v
                         Safety Interlock
                               |
                               v
                            Actuator
```

**Local safety decision memiliki prioritas lebih tinggi daripada remote command.**

---

## Hardware / Wokwi Pin Mapping

| Component            | ESP32 Pin | Function               |
| -------------------- | --------: | ---------------------- |
| DHT22                |    GPIO 4 | Temperature & humidity |
| Gas/MQ2 simulation   |   GPIO 16 | Gas/smoke detection    |
| Door/reed simulation |   GPIO 17 | Door status            |
| Blue lamp            |   GPIO 14 | Lamp                   |
| White PWM lamp       |   GPIO 27 | PWM lamp               |
| Red lamp             |   GPIO 26 | Lamp                   |
| Green lamp           |   GPIO 25 | Lamp                   |
| Fan PWM              |   GPIO 33 | Fan simulation         |
| Buzzer               |   GPIO 32 | Alarm / bell           |
| Servo                |   GPIO 13 | Door                   |
| LCD SDA              |   GPIO 21 | I2C                    |
| LCD SCL              |   GPIO 22 | I2C                    |

---

## Project Structure

```text
esp32-smart-home-fire-alarm-wokwi/
|
├── include/
│   ├── secrets.h
│   └── secrets.example.h
|
├── src/
│   └── main.cpp
|
├── thingsboard/
│   └── smart_home_dashboard_-_wokwi.json
|
├── diagram.json
├── platformio.ini
├── wokwi.toml
├── .gitignore
└── README.md
```

> `include/secrets.h` adalah file lokal yang berisi credential aktual dan **tidak boleh di-commit ke Git**.

---

## Configuration

### 1. Create Local Credential File

Copy:

```text
include/secrets.example.h
```

menjadi:

```text
include/secrets.h
```

Kemudian isi nilai konfigurasi lokal:

```cpp
WIFI_SSID
WIFI_PASSWORD

TELEGRAM_TOKEN
TELEGRAM_CHAT_ID

TB_HOST
TB_PORT
TB_ACCESS_TOKEN

RPC_SECURITY_TOKEN
```

Credential aktual tidak disertakan dalam repository.

---

### 2. ThingsBoard Dashboard

Dashboard export:

```text
thingsboard/smart_home_dashboard_-_wokwi.json
```

RPC Security Token pada dashboard yang dibagikan harus menggunakan placeholder, bukan token aktual.

Contoh parameter control:

```javascript
return {
  token: "RPC_SECURITY_TOKEN_PLACEHOLDER",
  value: value,
};
```

Untuk action tanpa value:

```javascript
return {
  token: "RPC_SECURITY_TOKEN_PLACEHOLDER",
};
```

Setelah dashboard di-import ke ThingsBoard, ganti placeholder tersebut dengan token lokal yang sesuai.

---

## Build and Run

### Requirements

- Visual Studio Code
- PlatformIO
- Wokwi extension / simulator
- ThingsBoard instance
- Telegram Bot configuration

### Build

```bash
pio run
```

### Run Simulation

Gunakan konfigurasi:

```text
diagram.json
wokwi.toml
```

kemudian jalankan Wokwi Simulator dari VS Code.

---

## Final Stable Version

Final implementation checkpoint:

```text
Branch : main
Tag    : v1.0-final
Commit : c7b1ff7
```

Tag description:

```text
Final stable IoT project with adaptive intelligence and token authentication
```

Final stable version mencakup:

- Edge fire safety
- Persistent event buffer
- Adaptive Intelligence
- Dynamic Threshold
- SAFE / WARNING / DANGER classification
- Advanced ThingsBoard analytics
- Token-Based Authentication
- Authentication failure counter
- Temporary Security Lock
- Automatic Unlock
- Security telemetry
- Authenticated ThingsBoard dashboard control

---

## Repository Access

Repository:

```text
https://github.com/Oni44/esp32-smart-home-fire-alarm-wokwi
```

**Repository saat ini bersifat private.**

Pengguna yang belum diberi akses GitHub tidak dapat membuka source code hanya dengan URL tersebut.

Untuk kebutuhan evaluasi/submission, source code dapat diberikan melalui salah satu cara berikut:

1. Memberikan akses collaborator/read access kepada dosen/penguji; atau
2. Menyertakan source-code export/ZIP bersama submission; atau
3. Mengubah repository menjadi public apabila kebijakan project mengizinkan.

Credential aktual tetap tidak boleh ikut dibagikan.

---

## Security Notice

Credential berikut tidak disimpan pada source repository:

- Wi-Fi credential
- Telegram Bot token
- Telegram Chat ID
- ThingsBoard device access token
- RPC Security Token

Gunakan:

```text
include/secrets.example.h
```

sebagai template konfigurasi.

Jangan commit:

```text
include/secrets.h
```

---

## Project Attribution

### Final Project Development and Major Enhancements

- Andronikus Marintan
- Bobby Daniyal
- Naura Retyani Ajisasongko
- Group 5 — BINUS Graduate Program, 2026

Project Final dikembangkan dari project ESP32/ThingsBoard sebelumnya dan kemudian diperluas secara signifikan dengan:

- Smart Home Fire Safety
- Edge Computing
- Telegram integration
- ThingsBoard telemetry and RPC
- Persistent NVS Event Buffer
- Safety Interlock
- Adaptive Intelligence / Dynamic Threshold
- Advanced Analytics
- Token-Based Authentication
- Temporary Security Lock
- Security Telemetry

### Original Project Attribution

Project ini dikembangkan dari contoh/project ESP32 ThingsBoard yang sebelumnya mencantumkan:

**Author: Mirutec - Roger Chung**

Attribution terhadap original author tetap dipertahankan. License asli pada repository, apabila ada, tetap harus dipertahankan sesuai ketentuannya.

---

## Final Project Scope

Project ini adalah **academic prototype / simulation**.

Sistem tidak dimaksudkan sebagai pengganti commercial certified fire alarm atau safety system.
