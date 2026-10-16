# DoorLock-Arduino

Smart door lock berbasis **ESP32 + QR-Scanner** yang terhubung ke server
`project.sstech.id`. Pintu hanya terbuka ketika kode QR pemasangan **ada di
daftar lokal (cache)** **dan** **disetujui oleh server (online validate)**.
Dirancang agar tetap **berfungsi offline** — menggunakan cache SPIFFS — dan
kembali **menyinkronkan log serta daftar izin** begitu WiFi kembali tersedia.

> **Bukan** sistem berdasar keypad/RFID. Ini kunci pintu berbasis **QR code**.

---

## Fitur

- 🔓 Buka pintu lewat **QR code scanner** (UART, 9600 baud).
- ✅ Otorisasi **dua lapis**: cek cache lokal (`credent.json`) **dan** validasi
  server (`https://project.sstech.id/IOT/api/validate`).
- 📶 **Offline-first**: saat WiFi down, tetap pakai cache lokal; log & akses
  **disimpan sementara** di buffer (`/log_buffer.json`) dan **tersampaikan
  otomatis** ketika koneksi pulih.
- 🔑 **Device auth** berbasis *device ID* + *PSK* (login & refresh token),
  token disimpan di `/tokens.json` (cache).
- ⏱️ **Auto-lock**: pintu terbuka **5 detik**, lalu kunci otomatis.
- 🗓️ **NTP WIB** (UTC+7) untuk timestamp ISO-8601 pada log.
- 🔄 Sinkronisasi otomatis tiap **10 menit** (daftar izin) & **30 detik**
  (push offline log).
- 🛡️ **Secure HTTP** (HTTPS + CA pinning via `WiFiClientSecure`).
- 📝 **Log realtime** ke server, dengan **fallback ke buffer** bila gagal.

---

## Hardware

| Komponen                 | Spesifikasi                  | Pin ESP32      |
|--------------------------|------------------------------|----------------|
| ESP32 DevKit (contoh)    | ESP32-WROOM / Wemos / NodeMCU| —              |
| Solenoid Door Lock       | 5V/12V, 1 channel            | via Relay      |
| Relay Module (aktif LOW)| 1 channel, opto-isolated   | **GPIO26**     |
| QR Code Scanner          | UART (TTL) — contoh: F366    | **GPIO16 (RX2)** / **GPIO17 (TX2)** |
| (Opsional) OLED / RTC    | I2C                          | GPIO21 / GPIO22|

> Relay ditandai **active LOW** oleh kode (`digitalWrite(RELAY_PIN, LOW)` =
> buka). Pastikan tipe relay modulmu kompatibel; ubah logika bila perlu
> (lihat `unlockDoor()` / `lockDoor()`).

### Wiring

```
QR Scanner  ↔ ESP32
  TX  → GPIO16 (RX2)
  RX  → GPIO17 (TX2)   (opsional, untuk konfig scanner)
  VCC → 5V
  GND → GND
  (jika scanner 3.3V, hubungkan ke 3V3)

Relay       ↔ ESP32
  IN  → GPIO26
  VCC → 5V (atau 3V3 tergantung modul)
  GND → GND
  (solenoid dayainya terpisah — jangan dari pin ESP32!)
```

---

## Config / Environment Variables

Semua pengaturan berada di bagian atas `real-on-board/main.ino`.
Ubah **sebelum compile & flash**:

| Konstanta            | Default                                  | Keterangan                                  |
|----------------------|-------------------------------------------|---------------------------------------------|
| `WIFI_SSID`          | `"SSID"`                                  | SSID WiFi                                   |
| `WIFI_PASS`          | `"PASS"`                                  | Password WiFi                                |
| `SERVER_BASE`        | `https://project.sstech.id/IOT/api/val`  | Base URL API                                |
| `AUTH_LOGIN_URL`     | `…/api/auth/login`                        | Endpoint login                               |
| `AUTH_REFRESH_URL`   | `…/api/auth/refresh`                      | Endpoint refresh token                       |
| `DEVICE_PSK`         | `"super-secret-device-psk"`              | Pre-shared key perangkat                      |
| `ROOT_CA`            | *(placeholder PEM)*                      | CA chain — **wajib ganti** dengan milik server |
| `RELAY_PIN`          | `26`                                      | GPIO kendali relay                            |
| `QR_RX` / `QR_TX`    | `16` / `17`                               | GPIO UART QR scanner                          |
| `UNLOCK_MS`          | `5000`                                    | Durasi pintu terbuka (ms)                     |

> **`ROOT_CA` masih placeholder** (`MIIF…REPLACE_WITH_YOUR_CA_CHAIN…`).
> Ganti dengan *CA chain / sertifikat server* yang sebenarnya agar HTTPS
> terpercaya.

---

## Instalasi

### 1. Toolchain

Pasang **ESP32 board** di Arduino IDE / PlatformIO:

```bash
# Arduino IDE Board Manager URL
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

### 2. Library yang dibutuhkan (Arduino Library Manager)

- `WiFi` (builtin ESP32)
- `HTTPClient` (builtin)
- `WiFiClientSecure` (builtin)
- `SPIFFS` (builtin)
- `ArduinoJson`

### 3. Flash

1. Buka `real-on-board/main.ino` di Arduino IDE.
2. Edit nilai config (WiFi, PSK, CA, DLL) di bagian atas.
3. Pilih board ESP32 + port.
4. Upload.

> `TestFlowOnWokwi.ino` adalah **simulasi ringan** (mock server, tidak butuh
> board asli) — berguna untuk testing logika otorisasi tanpa koneksi fisik.

---

## Cara Kerja (Flow)

1. **Boot** — generate `deviceId` (`MYRP-<efuseMac>`), mount **SPIFFS**, load
   `tokens.json`, `credent.json`, `log_buffer.json`.
2. **Konek WiFi** — begitu terhubung: NTP sync → login (atau refresh token) →
   `syncAllowedList()`.
3. **Scan QR** — pada setiap data QR masuk via UART:
   - `localOk  = isAllowedLocal(qr)` — cek cache `credent.json`.
   - `serverOk = isAllowedServer(qr)` — validasi online; bila WiFi down,
     fallback `true` (biarkan keputusan lokal menjadi penentu).
   - **buka pintu** bila `localOk && serverOk`, kirim log `"OPEN"`/"DENIED".
4. **Log** — dikirim realtime ke `SERVER_BASE/log` bersertifikat Bearer token;
   bila gagal/offline, masuk buffer dan **push otomatis nanti**.
5. **Auto-lock** — setelah `UNLOCK_MS` (5s), relay kembali HIGH (kunci).

### Retry / Refresh Token

- `ensureAccessToken()`: bila token akan kadaluarsa (<60s), otomatis *refresh*.
- Bila refresh gagal → *login ulang* pakai PSK.
- Bila tidak ada token sama sekali → langsung *login* di `setup()`.

---

## Serial Monitor

Baud rate: **115200**.

Lihat log boot, QR yang terbaca, status akses (`Door unlocked` /
`Access DENIED`), dan proses sync token & log:

```
Device ID: MYRP-xxxxxxxx
WiFi connected: 192.168.1.x
NTP sync OK
Sync OK (3 items)
QR read: EMP001
Door unlocked
Log sent realtime
```

---

## Direktori

```
DoorLock-Arduino/
├── TestFlowOnWokwi.ino   # Simulasi/mock (logika otorisasi tanpa board asli)
└── real-on-board/
    └── main.ino          # Sketch utama untuk flash ke ESP32
```

---

## Troubleshooting

- **`WiFi not connected - offline mode`** — cek SSID/PASS & jangkauan. Di mode
  ini pintu tetap bisa terbuka lewat cache lokal (`credent.json`).
- **HTTPS / `httpBeginSecure` gagal** — `ROOT_CA` belum diganti dengan CA
  server yang benar.
- **QR tidak terbaca / `QR read` kosong** — cek baud rate scanner (9600), dan
  pastikan kabel TX scanner → RX ESP32 (GPIO16) benar.
- **`credent.json` kosong** — belum sinkron. Jalankan `wifi on` (di simulasi)
  atau biarkan ESP sinkron otomatis; file akan tersimpan via SPIFFS.
- **Pintu tidak kunci otomatis** — pastikan wiring relay active-LOW sesuai;
  ubah `digitalWrite` di `unlockDoor()`/`lockDoor()` bila tipe relay lain.
- **Token 401 persisten** — ganti `DEVICE_PSK` / daftarkan device ID di server.

---

## Security Notes

- `DEVICE_PSK` bersifat **rahasia perangkat** — jangan commit ke repo publik.
- `ROOT_CA` harus diisi dengan sertifikat *server* yang sesuai (CA pinning).
- Sertakan `.env` / `secrets.h` bila ingin memisahkan konfig rahasia.
- Token akses/refresh disimpan di SPIFFS (`/tokens.json`) — jika butuh
  keamanan lebih tinggi, gunakan **encrypted partition** atau **Secure
  Element**.

---

## Testing (Simulasi Wokwi)

`TestFlowOnWokwi.ino` mendukung CLI serial untuk mensimulasikan flow:

| Perintah            | Keterangan                                |
|----------------------|--------------------------------------------|
| `help`               | Tampilkan daftar perintah                  |
| `wifi on`            | Simulasi WiFi ON → sync daftar izin        |
| `wifi off`           | Simulasi WiFi OFF (offline mode)           |
| `scan <kode>`        | Simulasi scan QR → otorisasi & unlock      |
| `push UNLOCK`        | Paksa buka pintu                          |
| `push LOCK`          | Paksa kunci pintu                         |
| `status`             | Tampilkan device ID, WiFi, daftar, buffer  |

---

## License

Open Source — silakan fork, modifikasi, dan gunakan untuk keperluan edukasi
atau proyek pribadi. Sesuaikan `DEVICE_PSK`, `ROOT_CA`, dan SSID/PASS sebelum
deployment.
