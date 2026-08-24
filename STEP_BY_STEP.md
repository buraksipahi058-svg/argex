# STEP BY STEP — Gerçek STM32 → Jetson → Base Station kurulumu

Bu dosya, **gerçek araçtan** (STM32 + Reactor firmware) telemetriyi Jetson üzerinden
Base Station paneline kadar getirmek için sıfırdan yapılacakları anlatır.

```
STM32 (Reactor fw + telemetri)
   │  UART  PB6 -> Jetson RX  (115200, 3.3V, tek yön)
   ▼
Jetson  (jetson/gateway.py)  ──QUIC/Protobuf──►  Base Station backend  ──WebSocket──►  Frontend
   │
   └─ Kameralar ──RTP──► MediaMTX ──WebRTC──► Frontend   (ayrı düzlem, bkz. Bölüm 6)
```

Nereye ne "kurulur" özeti:
| Cihaz | Ne yapılır |
|---|---|
| **STM32** | Yazılım kurulmaz → **firmware FLASH'lanır** (`firmware/reactor_telemetry/`) |
| **Jetson** | Python + proje dosyaları + `jetson/requirements.txt` (gateway burada çalışır) |
| **Base Station (laptop/PC)** | Python (backend) + Node (frontend) |

---

## BÖLÜM 1 — STM32 firmware'ini yükle

### 1.1 Geliştirme makinesine kur
- **Arduino IDE** + **STM32 core** (STM32duino / "STM32 MCU based boards", kart: *STM32F407G-DISC1* veya *Generic STM32F4 / DISCOVERY F407VG*).
- Kütüphane: **AlfredoCRSF** (Library Manager'dan).

### 1.2 Sketch'i aç ve yükle
- Klasör: `firmware/reactor_telemetry/` (içinde `reactor_telemetry.ino` + `haberlesme.h` + `haberlesme.cpp`).
- Kartı ST-Link/USB ile bağla → Derle → Yükle.
- **GÜVENLİK:** ilk denemede **tekerlekler havada** olsun.

### 1.3 STM32 pin haritası (bu firmware'de)
Pinleri firmware başındaki `#define`'lardan ve STM32F407G-DISC1 kılavuzundan (UM1472) bulursun.
| İşlev | STM32 pini | UART |
|---|---|---|
| CRSF alıcı (RC) | PB11=RX, PB10=TX | USART3 @420000 |
| Sol Reactor sürücü | **PA2** (TX) | USART2 @38400 |
| Sağ Reactor sürücü | **PC6** (TX) | USART6 @38400 |
| Servo (opsiyonel) | PB0 / PB1, ateş PE7 | — |
| LED'ler | PD12–15 | — |
| **JETSON telemetri** | **PB6** (TX) | **USART1 @115200** ← bize lazım olan |

---

## BÖLÜM 2 — STM32 ↔ Jetson KABLOLAMA (en önemli kısım)

Sadece **2 kablo** yeterli (tek yönlü telemetri):

```
STM32  PB6 (USART1 TX)  ───────────►  Jetson UART RX
STM32  GND              ───────────►  Jetson GND
```

| STM32 | → | Jetson 40-pin header |
|---|---|---|
| **PB6** (USART1 TX) | → | **Pin 10 (UART RXD)** |
| **GND** | → | **Pin 6** (veya 9/14/…GND) |

- Baud **115200**, 8N1.
- ⚠️ **Voltaj:** STM32 UART = 3.3V, Jetson 40-pin UART = 3.3V → **doğrudan bağlanır, level shifter gerekmez.** Jetson pinine **ASLA 5V verme** (kartı yakar).
- STM PB6 = **çıkış (TX)**, Jetson pini = **giriş (RX)**. TX→RX olacak şekilde bağla (TX-TX yaparsan çalışmaz).

### "Jetson'ın RX pini / UART cihazı nerede?"
- Fiziksel: Jetson dev kit **40-pin header**'da **Pin 8 = TXD, Pin 10 = RXD, Pin 6 = GND**. (Nano / Xavier NX / Orin Nano/NX dev kit hepsi bu düzeni paylaşır.)
- Cihaz adı (`/dev/ttyTHS*`) **modele göre değişir** (Nano'da genelde `ttyTHS1`, Orin/Xavier'de `ttyTHS0`). Doğrulama:
  ```bash
  ls -l /dev/ttyTHS*
  dmesg | grep -i tty
  ```
- Kaynak: NVIDIA "Jetson … Developer Kit — 40-pin Expansion Header" dokümanı veya `jetson.gpio` pinout.

---

## BÖLÜM 3 — Jetson tarafı (telemetri gateway)

### 3.1 Kur
- Python 3 + pip (JetPack'te var).
- Proje dosyalarını Jetson'a kopyala/clone et (en azından `jetson/`, `common/`, `gen/`, `proto/`, `needtocheck/`, `scripts/`).
- Bağımlılıklar:
  ```bash
  pip3 install -r jetson/requirements.txt
  python3 scripts/gen_proto.py
  ```

### 3.2 UART'ı kullanıma aç (Jetson'a özel, atlanırsa çalışmaz!)
Seri konsol servisi UART'ı işgal ediyor olabilir; kapat:
```bash
sudo systemctl stop nvgetty
sudo systemctl disable nvgetty
```
Kullanıcıyı `dialout` grubuna ekle (sonra çıkış/giriş yap):
```bash
sudo usermod -aG dialout $USER
```

### 3.3 Ham veri geliyor mu? (gateway'den ÖNCE bunu test et)
STM açık + kablo takılıyken:
```bash
cat /dev/ttyTHS1 | xxd | head
```
Ekranda `aa 55 01 01 08 …` gibi baytlar akıyorsa **fiziksel hat çalışıyor** demektir. (aa 55 = paket başlığı.) Akmıyorsa: baud/kablo/GND/port kontrol et.

### 3.4 Config'i gerçek seri porta çevir
`jetson/config.yaml`:
```yaml
source:
  type: serial
  serial_port: /dev/ttyTHS1     # kendi portunu yaz (Bölüm 2'den)
  serial_baud: 115200
quic:
  host: <BASE_STATION_IP>       # 127.0.0.1 DEĞİL — base station'ın ağ IP'si
  port: 4433
```

### 3.5 Gateway'i çalıştır
```bash
python3 -m jetson.gateway
```
"connected; observing STM via serial source" görmelisin.

---

## BÖLÜM 4 — Base Station tarafı (backend + frontend)

Bu, paneli açtığın makinede (laptop/PC) çalışır.

### 4.1 Kur
```bash
pip install -r backend/requirements.txt
python scripts/gen_proto.py
npm install
```

### 4.2 Çalıştır (sırayla)
```bash
python -m backend.main      # QUIC :4433, WebSocket/REST :8080
npm run dev                 # http://localhost:5173
```

### 4.3 Ağ eşleştirme
- Jetson ile base station **aynı ağda** olmalı.
- Jetson'daki `quic.host` = **base station'ın IP'si** (base station'da `hostname -I` / `ipconfig`).
- Base station güvenlik duvarında **4433/UDP** açık olmalı (QUIC).

---

## BÖLÜM 5 — Uçtan uca doğrulama sırası

1. STM32 flash'lı, **tekerlekler havada**.
2. STM **PB6 → Jetson Pin 10 (RX)**, **GND → GND**.
3. Jetson: `cat /dev/ttyTHS1 | xxd` → `aa 55 …` akıyor. ✅
4. Jetson: `python3 -m jetson.gateway` (serial).
5. Base station: `python -m backend.main` + `npm run dev`.
6. Panelde (`localhost:5173`):
   - Motor L/R, MODE, ELRS link **gerçek araca göre oynamalı**.
   - Kumandayı **kapat** → **FAILSAFE** + ELRS link **DOWN** düşmeli.
   - `OPERATION` **hep MANUAL** (bu firmware otonomi göndermiyor — normal).

---

## BÖLÜM 6 — Kameralar (ayrı düzlem, opsiyonel)

Telemetriden bağımsız. Zaten kurduğun akış:
- Jetson: `./mediamtx &` + `gst-launch-1.0 v4l2src device=/dev/video0 ! videoconvert ! x264enc tune=zerolatency … ! rtspclientsink location=rtsp://127.0.0.1:8554/cam_front`
- Frontend: `.env.local` içinde `VITE_CAMERA_HOST=<JETSON_IP>`.
- Panelde "ÖN" karosu canlı olur.

---

## BÖLÜM 7 — Sık çıkan sorunlar

| Belirti | Sebep / çözüm |
|---|---|
| `/dev/ttyTHS1` yok veya izin hatası | `nvgetty` kapat, `dialout` grubuna ekle (3.2) |
| `xxd`'de `aa 55` yok, çöp/boş | Baud ≠ 115200, TX-RX ters, GND ortak değil, yanlış port |
| Panelde hiç telemetri yok | Gateway `quic.host` yanlış IP / 4433 kapalı / backend çalışmıyor |
| Panelde "BASE LINK LOST" | Jetson↔base ağ/firewall; gateway bağlanamıyor |
| STM/Jetson bozuldu | Jetson pinine **5V verilmiş** olabilir — sadece 3.3V! |
| OPERATION hep MANUAL | Doğru: Reactor firmware Jetson'dan komut almıyor |

---

## Hatırlatma
Tel üzerindeki protokol (`haberlesme`) **değişmedi**; bu yüzden `jetson/`, `proto/`,
`backend/`, `src/` aynı kaldı. Gerçek veriye geçiş sadece **STM firmware'ine telemetri
eklemek (yapıldı)** + **jetson/config.yaml'ı `serial` yapmak** + **2 kablo** meselesi.
