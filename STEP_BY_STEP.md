# STEP BY STEP — Gerçek ESP32 → Jetson → Base Station kurulumu

Bu dosya, **gerçek araçtan** (ESP32 kontrolcü firmware'i) telemetriyi Jetson
üzerinden Base Station paneline kadar getirmek için sıfırdan yapılacakları
anlatır.

```
ESP32-WROOM-32  (ika_esp32 firmware: CRSF + Reactor + servo + lazer + telemetri)
   │  UART  GPIO22 (TX) / GPIO21 (RX)  ──►  3.3V USB-TTL  ──►  Jetson USB
   │  115200 8N1, ÇİFT YÖNLÜ (STATUS/HEARTBEAT gelir, COMMAND/HEARTBEAT gider)
   ▼
Jetson  (jetson/gateway.py)  ──QUIC/Protobuf──►  Base Station backend  ──WebSocket──►  Frontend
   │
   └─ Kameralar ──RTSP──► MediaMTX ──WebRTC──► Frontend   (ayrı düzlem, bkz. Bölüm 6)
```

Nereye ne "kurulur" özeti:
| Cihaz | Ne yapılır |
|---|---|
| **ESP32** | Yazılım kurulmaz → **firmware FLASH'lanır** (ESP32 firmware paketi, Arduino IDE) |
| **Jetson** | Python + proje dosyaları + `jetson/requirements.txt` (gateway burada çalışır) |
| **Base Station (laptop/PC)** | Python (backend) + Node (frontend) |

> `argexika/` (STM32CubeIDE projesi) artık **kullanılmıyor**; repoda geçmiş
> referansı olarak duruyor. Araç kontrolcüsü ESP32'dir.

---

## BÖLÜM 1 — ESP32 firmware'ini yükle

Bu bölüm **firmware paketinin kendi README'sinin özetidir**; çelişki olursa
paketin README'si geçerlidir. Base station bu firmware'i değiştirmez.

### 1.1 Geliştirme makinesine kur
- **Arduino IDE**.
- *Tercihler > Ek Kart Yöneticisi URL'leri*:
  `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
- Kart Yöneticisi'nden **esp32 by Espressif Systems 3.3.0** (2.x desteklenmiyor).
- *Araçlar > Kart*: **ESP32 Dev Module**, Upload Speed **115200**.
- Ekstra kütüphane GEREKMEZ (CRSF, Reactor RMT, servo LEDC, telemetri paketin içinde).

### 1.2 Projeyi aç ve yükle
- `ika_esp32/ika_esp32.ino` aç; `controller.cpp`, `config.h`, `motion.h` aynı
  klasörde kalmalı (INO'nun kısa olması normal, uygulama `controller.cpp` içinde).
- Motor / servo / lazer güçleri **kapalıyken** ESP32'yi USB'den bağla ve Yükle.
- **GÜVENLİK:** ilk denemede **tekerlekler havada** ve araç sabitlenmiş olsun.

### 1.3 ESP32 pin haritası (`ika_esp32/config.h`)
| İşlev | ESP32 pini | Arayüz |
|---|---|---|
| CRSF alıcı (RC) | **GPIO16** (RX) | Serial2 @420000 |
| Sol Reactor sürücü | **GPIO25** | RMT-TX @38400 (SRL girişi) |
| Sağ Reactor sürücü | **GPIO26** | RMT-TX @38400 (SRL girişi) |
| Taret servo pan / tilt | GPIO18 / GPIO19 | LEDC 50 Hz PWM |
| Lazer rölesi | GPIO27 | GPIO |
| **JETSON telemetri RX** | **GPIO21** | UART1 @115200 ← USB-TTL TX buraya |
| **JETSON telemetri TX** | **GPIO22** | UART1 @115200 → USB-TTL RX buraya |

> ⚠️ Eski kurulumda telemetri STM32 PC6 / USART6 veya 40-pin `ttyTHS1` idi.
> ESP32'de **USB-TTL üzerinden GPIO21/22**. Kabloyu buna göre tak.

---

## BÖLÜM 2 — ESP32 ↔ Jetson KABLOLAMA (en önemli kısım)

Link **çift yönlü**: 3 kablo.

```
USB-TTL  TX   ───────────►  ESP32 GPIO21   (ESP32 RX)
USB-TTL  RX   ◄───────────  ESP32 GPIO22   (ESP32 TX)
USB-TTL  GND  ───────────   ESP32 GND
USB-TTL  USB  ───────────►  Jetson USB-A portu
```

| USB-TTL | → | ESP32 |
|---|---|---|
| **TX** | → | **GPIO21** |
| **RX** | ← | **GPIO22** |
| **GND** | — | **GND** |
| **VCC / 3V3 / 5V** | ✗ | **BAĞLANMAZ** |

- Baud **115200**, 8N1.
- ⚠️ **Voltaj:** dönüştürücü **3.3 V mantıklı** olmalı. Üzerindeki "3.3 V" besleme
  pini, TX mantığının da 3.3 V olduğunu tek başına kanıtlamaz — sinyal seviyesini
  doğrula. ESP32 pinine **5 V verme**.
- TX→RX çapraz bağla (TX-TX yaparsan çalışmaz).

### "Jetson'daki port hangisi?"
- USB-TTL Jetson'a takılınca **`/dev/ttyUSB0`** olarak görünür (numara replug'da
  kayabilir). Sabit yol için:
  ```bash
  ls -l /dev/serial/by-id/
  dmesg | grep -i ttyUSB
  ```
  Çıkan `/dev/serial/by-id/usb-...-port0` yolunu config'e yaz — numara kaysa da bulur.
- **ESP32'nin kendi USB portu ayrı bir cihazdır** (flash + Seri Monitör). Protokol
  oradan akmaz; gateway'i oraya bağlama.

---

## BÖLÜM 3 — Jetson tarafı (telemetri gateway)

### 3.1 Kur
- Python 3 + pip (JetPack'te var).
- Proje dosyalarını Jetson'a kopyala/clone et (en azından `jetson/`, `common/`,
  `gen/`, `proto/`, `needtocheck/`, `scripts/`).
- Bağımlılıklar:
  ```bash
  pip3 install -r jetson/requirements.txt
  python3 scripts/gen_proto.py
  ```

### 3.2 Seri porta erişim
Kullanıcıyı `dialout` grubuna ekle (sonra çıkış/giriş yap):
```bash
sudo usermod -aG dialout $USER
```
(`nvgetty` kapatmaya artık gerek yok — o, 40-pin `ttyTHS*` içindi. 40-pin UART'a
geri dönerseniz `sudo systemctl stop nvgetty && sudo systemctl disable nvgetty`.)

### 3.3 ⚠️ PORT SAHİPLİĞİ — önce buna karar ver
Bu portu aynı anda **tek** bir process açabilir. Otonom köprüsü (ESP32'ye COMMAND
yazan process) de aynı hatta oturur. İki senaryo:

| Durum | Ne yap |
|---|---|
| Jetson'da **sadece** gateway var | `source.type: serial` — aşağıdaki 3.5 |
| Otonom köprüsü de çalışacak | `source.type: udp` — köprü, okuduğu **ham çerçeveleri** `127.0.0.1:9000`'e re-publish etsin (IPC tap). Gateway portu hiç açmaz. |

### 3.4 Ham veri geliyor mu? (gateway'den ÖNCE bunu test et)
ESP32 açık + kablo takılıyken:
```bash
python3 sim/jetson_link_test.py /dev/ttyUSB0
```
Bu araç önce ~3 sn **sadece dinler** (STATUS akmalı, `jetson_link=False`), sonra
COMMAND + HEARTBEAT gönderir ve `jetson_link=True` olmalı — yani **iki yönü birden**
doğrular. Satırda `mod=` (DRIVE/LASER/AUTO) ve `arm=` de görürsün.

> `cat /dev/ttyUSB0` ile **okuma**: terminalin "cooked" modu ikili veriyi bozar.
> Şart olursa `stty -F /dev/ttyUSB0 115200 raw -echo && cat /dev/ttyUSB0 | xxd | head`
> → `aa 55 01 01 08 …` görmelisin (`aa 55` = paket başlığı).

### 3.5 Config'i gerçek seri porta çevir
`jetson/config.yaml`:
```yaml
source:
  type: serial
  serial_port: /dev/ttyUSB0     # veya /dev/serial/by-id/... (Bölüm 2)
  serial_baud: 115200
quic:
  host: <BASE_STATION_IP>       # 127.0.0.1 DEĞİL — base station'ın ağ IP'si
  port: 4433
```

### 3.6 Gateway'i çalıştır
```bash
python3 -m jetson.gateway
```
"connected; observing STM via serial source" görmelisin. Video açıksa
`video: mode-aware supervisor enabled (... DRIVE=front,rear; LASER=turret; AUTO=front,turret)`
satırı da gelir.

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

1. ESP32 flash'lı, **tekerlekler havada**, araç sabit.
2. USB-TTL ↔ ESP32 **GPIO21/22/GND** bağlı, USB-TTL Jetson'da.
3. Jetson: `python3 sim/jetson_link_test.py /dev/ttyUSB0` → STATUS akıyor, sonra
   `jetson_link=True`. ✅
4. Jetson: `python3 -m jetson.gateway` (serial).
5. Base station: `python -m backend.main` + `npm run dev`.
6. Panelde (`localhost:5173`):
   - Motor L/R ve ELRS link **gerçek araca göre oynamalı**; taret (pan/tilt)
     kumandayla döner.
   - **MODE**: CH5 yukarı → `DRIVE`, CH5 aşağı → `LASER`, **CH8 en altta → `AUTO`**
     (CH8 her zaman CH5'i ezer).
   - **MOTOR ARM**: manuel sürüşe geçip joystick'i 0,5 sn ortada tutunca `ARMED`.
     Taret modunda `DISARMED` **normaldir**.
   - Kumandayı **kapat** → 300 ms sonra **FAILSAFE** + ELRS link **DOWN** + `DISARMED`.
   - `OPERATION`: otonom komut akmıyorsa `MANUAL`. CH8 aşağıdayken henüz geçerli
     COMMAND yoksa **`AUTO PENDING`** (turuncu) gösterir — beklenen davranış.
   - **HW FAULT** `LATCHED` görürsen: firmware donanım hata mandalına düşmüş,
     ancak **reset** ile açılır.

---

## BÖLÜM 6 — Kameralar (ayrı düzlem, opsiyonel)

Telemetriden bağımsız. Gateway kameraları **moda göre** açıp kapatır
(`jetson/config.yaml` → `video.cameras[].modes`):

| Araç modu | Yayındaki kameralar |
|---|---|
| DRIVE | front + rear |
| LASER | turret |
| AUTO | turret + front |

- Aynı anda en fazla **2** kamera açık kalır (USB isochronous bütçesi + Wi-Fi bandı).
- Jetson: `./mediamtx &`, gateway ffmpeg push'ları kendi yönetir.
- Frontend: `.env.local` içinde `VITE_CAMERA_HOST=<JETSON_IP>`.
- Komutları çalıştırmadan görmek için: `python3 -m jetson.video_pipelines --print`.

---

## BÖLÜM 7 — Sık çıkan sorunlar

| Belirti | Sebep / çözüm |
|---|---|
| `/dev/ttyUSB0` yok veya izin hatası | Dönüştürücü takılı mı (`dmesg \| grep ttyUSB`), `dialout` grubuna ekle (3.2) |
| Port açılmıyor / "device busy" | Otonom köprüsü aynı portu tutuyor → `source.type: udp` + IPC tap (3.3) |
| `aa 55` yok, çöp/boş | Baud ≠ 115200, TX-RX ters, GND ortak değil, **ESP32'nin kendi USB portuna** bağlanmış |
| Panelde hiç telemetri yok | Gateway `quic.host` yanlış IP / 4433 kapalı / backend çalışmıyor |
| Panelde "BASE LINK LOST" | Jetson↔base ağ/firewall; gateway bağlanamıyor |
| MODE hep DRIVE, AUTO'ya geçmiyor | CH8 ters → kumandada kanalı ters çevir **veya** firmware `AUTO_INPUT_SIGN=-1` (ikisini birden değil) |
| AUTO PENDING'de kalıyor | `CMD_FLAG_AUTO_REQ` yok, COMMAND <5 Hz, ya da Jetson HEARTBEAT'i susmuş |
| Otonomda taret kamerası yok | `video.cameras[].modes` içinde turret'te `auto` yazmıyor |

---

## Hatırlatma
Tel üzerindeki protokol (`AA 55 | VER | TYPE | LEN | SEQ | PAYLOAD | CRC16`)
**değişmedi** — ESP32 firmware'i STM32'nin çerçevelerini birebir üretiyor, bu yüzden
`needtocheck/jetson_parser.py` olduğu gibi kullanılmaya devam ediyor. Base station
tarafında ESP32'ye uyarlanan şeyler:

- `aktifMod = 2` (**OTONOM**) artık tanınıyor → `ACTIVE_MODE_AUTONOMOUS`, panelde
  `AUTO`, video düzleminde turret+front.
- `durum` baytının **0x20 ARMED** ve **0x40 HW_ERROR** bitleri çözülüp panele ve
  event akışına bağlandı (donmuş parser'a dokunulmadan, ham bayttan).
- Seri port varsayılanı `/dev/ttyTHS1` → **`/dev/ttyUSB0`**.

Otonom ekibin protokol rehberi: [`otonomicingerekenler.md`](otonomicingerekenler.md).
