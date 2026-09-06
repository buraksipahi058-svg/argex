# Jetson ↔ STM32 Haberleşme Protokolü — Otonom Ekip Rehberi

Bu doküman, otonom algoritmayı yazan ekibin STM32 (araç kontrolcüsü) ile
Jetson üzerinden sorunsuz haberleşmesi için gereken **her şeyi** içerir.

---

## 0. ÖNCE BUNU OKU (en kritik iki şey)

1. **TEK GERÇEK KAYNAK: [`needtocheck/jetson_parser.py`](needtocheck/jetson_parser.py).**
   (Tek istisna: ESP32'nin sonradan eklediği iki `durum` biti — `ST_ARMED 0x20`
   ve `ST_HW_ERROR 0x40` — bu dosyada isimlendirilmiyor; ham `durum` baytından
   kendiniz okuyun, §4'e bakın.)
   Protokolü elle yeniden yazmayın — bu dosyayı **import edin**. İçinde
   `Protocol.build_command()`, `build_heartbeat()`, `feed()` hazır. Çalışan tam
   örnek: [`sim/jetson_link_test.py`](sim/jetson_link_test.py).
   Doğrulama için: `python3 needtocheck/jetson_parser.py` → örnek COMMAND hex'i
   basar + CRC self-test yapar.

2. **⚠️ COMMAND MOTORA UYGULANIYOR — ARAÇ SİZİN KOMUTUNUZLA HAREKET EDER.**
   Kontrolcü **ESP32** (`ika_esp32`). Otonom yetkisini veren anahtar **CH8**'dir
   (CH5 değil) ve CH8 her zaman CH5'i ezer. CH8 en alttayken sürüş, taret ve
   lazerin **tamamı** sizin COMMAND'ınızla sürülür. Devreye girmesi için üç şart
   birden gerekir: `CMD_FLAG_AUTO_REQ` bayrağı + COMMAND'ın taze olması (<200 ms)
   + Jetson linkinin taze olması (<500 ms). Biri eksikse araç **güvenli durur**.
   **Hız tavanı firmware'de `AUTO_MAX_PCT` ile belirlenir ve şu an 100'dür**,
   yani gönderdiğiniz `±100` doğrudan tam güce gider — pratikte limit sizin
   tarafınızda. İlk saha testlerinde `±30`'u aşmayın.

---

## 1. Fiziksel bağlantı

- **Link:** ESP32 ↔ Jetson, **3.3 V USB-TTL dönüştürücü** üzerinden UART.
  ESP32 tarafı UART1 (`HardwareSerial JetsonSerial(1)`).
- **Kablolama:** USB-TTL **TX** → ESP32 **GPIO21** (`JETSON_RX_PIN`),
  USB-TTL **RX** ← ESP32 **GPIO22** (`JETSON_TX_PIN`), **GND** ortak.
  USB-TTL'in **VCC/3V3/5V pini bağlanmaz**.
- **Port (Jetson):** `/dev/ttyUSB0` — numara replug'da kayar, sabitlemek için
  `/dev/serial/by-id/...` yolunu kullanın.
  > ESP32'nin **kendi USB portu ayrı bir cihazdır**; sadece flash ve Seri Monitör
  > içindir, protokol oradan akmaz.
  > (Eski yollar: STM32F407 native USB-CDC ve 40-pin `/dev/ttyTHS1`; artık ikisi
  > de kullanılmıyor.)
- **Baud:** 115200 8N1.
- **Ham (raw) modda açın.** pyserial otomatik yapar. **`cat`/`head` ile OKUMAYIN**
  — terminal "cooked" modu ikili veriyi bozar (kontrol baytlarını yer).
- **⚠️ PORT SAHİPLİĞİ:** bu portu aynı anda **tek** bir process açabilir. Base
  station gateway'i de (`jetson/gateway.py`) aynı hattı dinliyor. İkisi birden
  çalışacaksa gateway'i `source.type: udp` yapın ve **sizin** process'iniz okuduğu
  ham çerçeveleri `127.0.0.1:9000`'e re-publish etsin (IPC tap).

**Kim ne gönderir:**
- ESP32 → Jetson: `STATUS` (20 Hz) + `HEARTBEAT` (10 Hz), sürekli.
- Jetson → ESP32: `COMMAND` + `HEARTBEAT` (siz gönderirsiniz).

---

## 2. Çerçeve (frame) formatı

```
AA 55 | VERSION | TYPE | LENGTH | SEQ | PAYLOAD (LENGTH bayt) | CRC_L CRC_H
```

| Alan | Boyut | Değer / Anlam |
|---|---|---|
| `AA 55` | 2 | Sabit başlık |
| `VERSION` | 1 | `0x01` |
| `TYPE` | 1 | `0x01`=STATUS, `0x02`=COMMAND, `0x03`=HEARTBEAT |
| `LENGTH` | 1 | payload uzunluğu (maks 32) |
| `SEQ` | 1 | 0..255 döngüsel (paket kaybı tespiti) |
| `PAYLOAD` | LENGTH | tipe göre (aşağıda) |
| `CRC` | 2 | CRC-16/CCITT-FALSE, **little-endian** (CRC_L önce) |

- **CRC-16/CCITT-FALSE**: poly `0x1021`, init `0xFFFF`, xorout `0x0000`.
- **CRC kapsamı: `VERSION`'dan `PAYLOAD` sonuna kadar** (yani `AA 55` ve CRC'nin
  kendisi HARİÇ).
- Tüm çok baytlı alanlar **little-endian**.
- Doğrulama sabiti: `crc16_ccitt(b"123456789") == 0x29B1`.

---

## 3. COMMAND (Jetson → STM) — `TYPE=0x02`, payload **7 bayt**

| Ofset | Alan | Tip | Aralık | Anlam |
|---|---|---|---|---|
| 0 | `sol`    | int8  | -100..100 | sol palet hız hedefi |
| 1 | `sag`    | int8  | -100..100 | sağ palet hız hedefi |
| 2 | `pan`    | uint8 | 0..180 | taret yatay açı (derece) |
| 3 | `tilt`   | uint8 | 0..180 | taret dikey açı (derece) |
| 4 | `lazer`  | uint8 | 0/1 | lazer ateşleme |
| 5 | `mod`    | uint8 | 0/1 veya `0xFF` | **bu firmware'de KULLANILMIYOR** — parse edilir, hiçbir etkisi yoktur. `0xFF` gönderin. |
| 6 | `bayrak` | uint8 | bit alanı | bit0 = `CMD_FLAG_AUTO_REQ` (0x01) = "otonom kontrol istiyorum" |

`build_command(sol, sag, pan, tilt, lazer, mod=None, auto_request=False)`
değerleri otomatik kırpar (clamp) ve `mod=None` → `0xFF` (mod değiştirme) yapar.

---

## 4. STATUS (STM → Jetson) — `TYPE=0x01`, payload **8 bayt**, **20 Hz**

| Ofset | Alan | Tip | Anlam |
|---|---|---|---|
| 0 | `sol_motor` | int8  | **UYGULANAN** sol palet hızı (-100..100) |
| 1 | `sag_motor` | int8  | **UYGULANAN** sağ palet hızı |
| 2 | `pan`       | uint8 | mevcut pan açısı (derece) |
| 3 | `tilt`      | uint8 | mevcut tilt açısı |
| 4 | `lazer`     | uint8 | lazer çıkış durumu 0/1 |
| 5 | `aktif_mod` | uint8 | **0=sürüş (CH5), 1=taret/lazer (CH5), 2=OTONOM (CH8)** |
| 6 | `elrs_link` | uint8 | RC (CRSF) linki 0/1 |
| 7 | `durum`     | uint8 | bit alanı (aşağıda) |

**`durum` bitleri:**
| Bit | Ad | Anlam |
|---|---|---|
| `0x01` | `ST_JETSON_LINK` | Jetson paketi taze (sizi duyuyor) |
| `0x02` | `ST_CMD_TIMEOUT` | otonom modda COMMAND bayat (>200 ms) — **canlı** |
| `0x04` | `ST_AUTO_EN`     | otonom kontrol kabul edildi — **canlı** |
| `0x08` | `ST_FAILSAFE`    | RC yok / otonom şartı düştü / donanım hatası → motorlar durdu |
| `0x10` | `ST_CRC_ERR`     | son STATUS'tan beri en az bir CRC hatası — **canlı** |
| `0x20` | `ST_ARMED`       | **motor kilidi açık.** Taret modunda 0 olması normaldir |
| `0x40` | `ST_HW_ERROR`    | **mandallı donanım hatası** (RMT/LEDC kurulum veya TX hatası); `ST_FAILSAFE`'i de yakar |

`feed()` ilk beşini bool olarak da verir: `jetson_link`, `cmd_timeout`,
`auto_enabled`, `failsafe`, `crc_err`. **Son ikisini ham baytlan okuyun:**

```python
armed    = bool(s["durum"] & 0x20)
hw_error = bool(s["durum"] & 0x40)
```

`ST_AUTO_EN` tek başına motorun döneceğini garanti etmez; hareket için
`ST_ARMED` de gerekir (otonomda taze `AUTO_REQ` geldiği anda ikisi birlikte açılır).

---

## 5. HEARTBEAT (çift yön) — `TYPE=0x03`, payload **5 bayt**

| Ofset | Alan | Tip | Anlam |
|---|---|---|---|
| 0 | `kaynak`    | uint8  | `0x00`=ESP32, `0x01`=JETSON |
| 1..4 | `uptime_ms` | uint32 LE | açılıştan beri geçen ms |

---

## 6. Zamanlama ve canlılık kuralları

| Ne | Değer | Kaynak sabiti |
|---|---|---|
| ESP32 STATUS periyodu | 50 ms (20 Hz) | `STATUS_PERIOD_MS` |
| ESP32 HEARTBEAT periyodu | 100 ms (10 Hz) | `HB_PERIOD_MS` |
| COMMAND "taze" penceresi | < 200 ms | `CMD_TIMEOUT_MS` |
| Jetson linki "canlı" penceresi | < 500 ms | `JETSON_HB_TIMEOUT_MS` |

- **COMMAND'ı en az ~5 Hz** (tercihen 10–20 Hz) gönderin ki ESP32 taze saysın.
- **HEARTBEAT'i ~10 Hz** gönderin; COMMAND de linki tazeler. 500 ms paket gelmezse
  ESP32 sizi "kopuk" sayar (`ST_JETSON_LINK` düşer).
- **SEQ atlaması cezalıdır:** ESP32 aynı veya geri giden SEQ'li çerçeveyi atar
  (`delta == 0 || delta >= 128`). Tek bir artan sayaç kullanın — `Protocol`
  bunu zaten yapar.

---

## 7. Semantik / güvenlik notları

### Otorite: tek anahtar CH8

Kanal haritası (`ika_esp32/controller.cpp`): CH1 yatay, CH2 dikey, **CH5**
manuel sürüş / taret, **CH6** manuel hız kademesi (%30/%60/%100), **CH8 OTONOM**,
**CH10** ateş. Bu firmware'de **CH7 ve CH9 kullanılmıyor.**

| | CH8 yukarıda (MANUEL) | CH8 en altta (OTONOM) |
|---|---|---|
| Sürüş / taret seçimi | CH5 (operatör) | **ikisi de aynı anda sizde** |
| Paletler | CH1/CH2 + CH6 limiti | **`sol`/`sag`** (`AUTO_MAX_PCT`, şu an 100) |
| Pan / Tilt | CH1/CH2 (taret modunda) | **`pan`/`tilt`** |
| Lazer | CH10 tetiği | **`lazer`** |

- **CH8 CH5'i ezer.** `Mode_ReadFromRadio()` önce CH8'e bakar; otonomdayken
  CH5'in nerede olduğunun hiçbir önemi yoktur.
- **⚠️ `mod` baytı bu firmware'de ÖLÜDÜR.** Parse edilip saklanır ama hiçbir yerde
  kullanılmaz. Sonuçları:
  - Otonomda **sürüş ve taret aynı anda açıktır**; `mod=1` göndererek paletleri
    kilitleyemezsiniz. Hareket halinde ateş etmek **yazılımla engellenmiyor** —
    bu kısıt artık sizin sorumluluğunuzda.
  - Kamera düzlemini `mod` baytı **seçmez**. Base station telemetrideki
    `aktif_mod`'a bakar: 0 → ön+arka, 1 → taret, **2 (otonom) → taret + ön**.
  - `0xFF` gönderin (`build_command`'ın `mod=None` varsayılanı zaten bunu yapar).
- **Ateş mandalı otonomda YOK.** CH10'un bırakılıp tekrar basılmasını isteyen
  mandal yalnızca manuel taret modundadır. Otonomda `lazer=1`, `AUTO_REQ` taze ve
  `ST_ARMED` açık olduğu ilk çevrimde ateş eder. Moda girmeden **önce** `lazer=1`
  göndermiş olmayın.
- **Servo slew limiti:** `pan`/`tilt` hedefleriniz doğrudan yazılmaz; **pan
  400 µs/s, tilt 250 µs/s** hızla hedefe yürür (`PAN/TILT_RATE_US_PER_S`).
  Uçtan uca pan aralığı 700–2300 µs, tilt 1050–1900 µs; yani 0→180 bir pan
  taraması **~4 saniye** sürer. Kontrol döngünüzü buna göre kurun — servo
  hedefe oturmadan bir sonraki hedefi göndermek taramayı yavaşlatır.

### Firmware'in her koşulda uyguladığı kesmeler (siz ezemezsiniz)

- **CRSF link kaybı:** 300 ms CRSF çerçevesi gelmezse (veya LQ=0 bildirilirse)
  otonom dahil her şey durur. Yani otonom menzili = ELRS menzili. Operatörün
  fiziksel override'ı her koşulda elindedir: **CH8'i yukarı almak** otonomu keser.
- **Sizin sessizliğiniz:** COMMAND >200 ms bayatlarsa, Jetson HEARTBEAT'i >500 ms
  susarsa veya `AUTO_REQ` düşerse araç güvenli durur (`ST_CMD_TIMEOUT` +
  `ST_FAILSAFE` yanar, `ST_AUTO_EN` ve `ST_ARMED` söner).
- **Donanım hata mandalı:** RMT/LEDC kurulumu veya gönderimi hata verirse
  `ST_HW_ERROR` mandallanır, her şey durur ve **ancak reset ile açılır**.
- **Boot koruması:** açılıştan sonra `STARTUP_HOLD_MS = 1200 ms` boyunca motorlar
  kilitlidir; ayrıca hiç CRSF çerçevesi gelmemişken link "yok" sayılır, bu yüzden
  araç **asla otonom modda boot edemez**. Otonoma girmek için CH8'in gerçek RC ile
  en alta indirilmesi şarttır.
- **Manuel arm kilidi (sizi ilgilendirmez ama telemetride görünür):** manuel
  sürüşe geçerken joystick 500 ms ortada tutulmadan `ST_ARMED` açılmaz. Otonomda
  bu ikinci bekleme **kaldırılmıştır**; taze `AUTO_REQ` motor yetkisini anında verir.

### Sizde kalan açık madde

- **Abort zinciri:** Base station'dan Jetson'a bir "dur" kanalı bugün **yok**
  (`jetson/gateway.py` bilinçli olarak kontrol trafiği üretmiyor). Otonom
  sırasında yazılımsal durdurma tek yoldan geçer: sizin COMMAND göndermeyi
  kesmeniz. Base station bağlantınız koparsa kendi kendine `AUTO_REQ`'i düşüren
  bir watchdog koymanızı öneririz.
- **Hareket halinde ateş kilidi:** eski firmware'de `mod=1` paletleri kilitliyordu;
  bu firmware'de o kilit yok. Ateş etmeden önce `sol=sag=0` göndermeyi ve aracın
  gerçekten durduğunu telemetriden (`sol_motor`/`sag_motor`) doğrulamayı kendi
  kodunuza koyun.
- **Bağımsız fiziksel acil stop:** bu firmware'de **CH9 E-STOP yoktur** ve UART
  stop baytları ESP32 enerjisiz kalırsa sürücüye ulaşmaz. Donanımsal acil stop
  devresi şart.

---

## 8. Örnek (Python) — yeniden yazmayın, import edin

```python
import serial, importlib.util
from pathlib import Path

# needtocheck/jetson_parser.py'yi yukle (TEK GERCEK KAYNAK)
spec = importlib.util.spec_from_file_location("jp", "needtocheck/jetson_parser.py")
jp = importlib.util.module_from_spec(spec); spec.loader.exec_module(jp)

PORT = "/dev/ttyUSB0"                          # USB-TTL (ESP32 GPIO21/22)
ser = serial.Serial(PORT, 115200, timeout=0)   # raw
proto = jp.Protocol()

import time
t0 = time.time()
while True:
    # 1) ESP32'den geleni coz
    for pkt in proto.feed(ser.read(256)):
        if pkt["type"] == jp.TYPE_STATUS:
            s = pkt["status"]
            # ornek: s["sol_motor"], s["pan"], s["failsafe"], s["jetson_link"], ...
            otonom = s["aktif_mod"] == 2            # CH8 en altta mi
            armed  = bool(s["durum"] & 0x20)        # motor kilidi acik mi
            hw_err = bool(s["durum"] & 0x40)        # mandalli donanim hatasi

    # 2) ~10-20 Hz COMMAND + HEARTBEAT gonder
    up = int((time.time() - t0) * 1000)
    ser.write(proto.build_command(sol=0, sag=0, pan=90, tilt=90,
                                  lazer=0, mod=None,  # mod kullanilmiyor -> 0xFF
                                  auto_request=True))
    ser.write(proto.build_heartbeat(uptime_ms=up))
    time.sleep(0.05)
```

Çalışan, iki yönü de test eden tam örnek:
[`sim/jetson_link_test.py`](sim/jetson_link_test.py) — `python3 sim/jetson_link_test.py <port>`.

---

## 9. Hızlı doğrulama listesi (otonom ekip için)

- [ ] `python3 needtocheck/jetson_parser.py` → CRC self-test OK.
- [ ] `python3 sim/jetson_link_test.py /dev/ttyUSB0` → önce STATUS gelir
      (`jetson_link=False`), sonra gönderince **`jetson_link=True`**, `kayip=0`.
      Rapor satırı `mod=` ve `arm=` de basar.
- [ ] Kendi kodunuzda `feed()` STATUS'ları çözüyor; `build_command`/`build_heartbeat`
      ile gönderiyorsunuz; COMMAND'ı ≥5 Hz, HEARTBEAT'i ~10 Hz.
- [ ] **Araç hareket ETMELİ.** Otonom yolu bağlı (bkz. §0.2). Hareket yoksa sırayla
      bakın: `aktif_mod == 2` mi (CH8 en altta mı), `CMD_FLAG_AUTO_REQ` set mi,
      COMMAND ≥5 Hz akıyor mu, CRSF linki ayakta mı, `ST_HW_ERROR` mandallanmış mı.
      Telemetrideki `auto_enabled` / `cmd_timeout` / `failsafe` / `ST_ARMED`
      bitleri sebebi söyler.
- [ ] Portu ikinci bir process açmıyor. Base station gateway'i de aynı hattaysa
      `source.type: udp` + IPC tap kurulu (bkz. §1).
