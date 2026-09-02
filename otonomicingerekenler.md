# Jetson ↔ STM32 Haberleşme Protokolü — Otonom Ekip Rehberi

Bu doküman, otonom algoritmayı yazan ekibin STM32 (araç kontrolcüsü) ile
Jetson üzerinden sorunsuz haberleşmesi için gereken **her şeyi** içerir.

---

## 0. ÖNCE BUNU OKU (en kritik iki şey)

1. **TEK GERÇEK KAYNAK: [`needtocheck/jetson_parser.py`](needtocheck/jetson_parser.py).**
   Protokolü elle yeniden yazmayın — bu dosyayı **import edin**. İçinde
   `Protocol.build_command()`, `build_heartbeat()`, `feed()` hazır. Çalışan tam
   örnek: [`sim/jetson_link_test.py`](sim/jetson_link_test.py).
   Doğrulama için: `python3 needtocheck/jetson_parser.py` → örnek COMMAND hex'i
   basar + CRC self-test yapar.

2. **⚠️ ŞU ANKİ FIRMWARE COMMAND'I MOTORA UYGULAMIYOR.**
   STM, gelen COMMAND'ı **alır, CRC'sini doğrular, `jetson_link` bitini True
   yapar ve tazeliğini izler** — ama **motorlara/servolara HENÜZ bağlamaz.**
   Araç şu an tamamen RC (ELRS/CRSF kumanda) ile sürülüyor. RC↔AI arbitrasyonu
   (`main.c::Drive_Update`) sonraki adımdır. Yani: komut gönderip linkin
   canlandığını (`jetson_link=True`) görebilirsiniz, **ama araç sizin komutunuzla
   arbitrasyon bağlanana kadar HAREKET ETMEZ.** Bunu birlikte planlayın.

---

## 1. Fiziksel bağlantı

- **Link:** STM32F407 ↔ Jetson, **native USB Full-Speed CDC** (Virtual COM Port).
- **Port (Jetson):** `/dev/ttyACM*` — numara replug'da kayar, bu yüzden **by-id**
  yolunu kullanın (sabit):
  `/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_367D34503235-if00`
  > Not: `367D34503235` **o çipe özel** seri numarasıdır. Başka bir F407'de
  > farklı olur → o kartın by-id yolunu `ls /dev/serial/by-id/` ile bulun.
  > ST-Link'in kendi VCP'si (`...STLink...-if02`) BAŞKA bir cihazdır, karıştırmayın.
- **Baud:** 115200 (USB-CDC baud'u yok sayar ama pyserial için değer verin).
- **Ham (raw) modda açın.** pyserial otomatik yapar. **`cat`/`head` ile OKUMAYIN**
  — terminal "cooked" modu ikili veriyi bozar (kontrol baytlarını yer).

**Kim ne gönderir:**
- STM → Jetson: `STATUS` (20 Hz) + `HEARTBEAT` (10 Hz), sürekli.
- Jetson → STM: `COMMAND` + `HEARTBEAT` (siz gönderirsiniz).

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
| 5 | `mod`    | uint8 | 0/1 veya `0xFF` | 0=sürüş, 1=lazer, **`0xFF`=modu değiştirme** |
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
| 5 | `aktif_mod` | uint8 | 0=sürüş, 1=lazer |
| 6 | `elrs_link` | uint8 | RC (CRSF) linki 0/1 |
| 7 | `durum`     | uint8 | bit alanı (aşağıda) |

**`durum` bitleri:**
| Bit | Ad | Anlam |
|---|---|---|
| `0x01` | `ST_JETSON_LINK` | Jetson paketi taze (sizi duyuyor) |
| `0x02` | `ST_CMD_TIMEOUT` | otonom komut bayat *(bu firmware'de şimdilik 0)* |
| `0x04` | `ST_AUTO_EN`     | otonom kontrol aktif *(şimdilik 0)* |
| `0x08` | `ST_FAILSAFE`    | RC yok → güvenlik motorları durdurdu |
| `0x10` | `ST_CRC_ERR`     | yakında CRC hatası *(şimdilik 0)* |

`feed()` bunları ayrıca bool olarak da verir: `jetson_link`, `cmd_timeout`,
`auto_enabled`, `failsafe`, `crc_err`.

---

## 5. HEARTBEAT (çift yön) — `TYPE=0x03`, payload **5 bayt**

| Ofset | Alan | Tip | Anlam |
|---|---|---|---|
| 0 | `kaynak`    | uint8  | `0x00`=STM, `0x01`=JETSON |
| 1..4 | `uptime_ms` | uint32 LE | açılıştan beri geçen ms |

---

## 6. Zamanlama ve canlılık kuralları

| Ne | Değer | Kaynak sabiti |
|---|---|---|
| STM STATUS periyodu | 50 ms (20 Hz) | `STATUS_PERIOD_MS` |
| STM HEARTBEAT periyodu | 100 ms (10 Hz) | `HB_PERIOD_MS` |
| COMMAND "taze" penceresi | < 200 ms | `CMD_TIMEOUT_MS` |
| Jetson linki "canlı" penceresi | < 500 ms | `JETSON_HB_TIMEOUT_MS` |

- **COMMAND'ı en az ~5 Hz** (tercihen 10–20 Hz) gönderin ki STM taze saysın.
- **HEARTBEAT'i ~10 Hz** gönderin; COMMAND de linki tazeler. 500 ms paket gelmezse
  STM sizi "kopuk" sayar (`ST_JETSON_LINK` düşer).

---

## 7. Semantik / güvenlik notları

- **FAILSAFE:** RC (CRSF) linki koparsa STM motorları durdurur (`ST_FAILSAFE`).
  Bu, Jetson linkinden bağımsızdır — RC ayrı bir link.
- **Mod:** Şu an modu RC'deki CH5 belirler. COMMAND'da `mod=0xFF` gönderirseniz
  STM modu değiştirmez (önerilen: arbitrasyon netleşene kadar `0xFF`).
- **ARM:** RC'deki ARM anahtarı sürüşü açar; disarmed'de motorlar durur.
- Otonom komut devreye girdiğinde (arbitrasyon yazılınca) büyük olasılıkla
  `CMD_FLAG_AUTO_REQ` + geçerli RC link + bir "AI izin" anahtarı gerekecek.
  Bu kural henüz tanımlı değil — **birlikte kararlaştırın.**

---

## 8. Örnek (Python) — yeniden yazmayın, import edin

```python
import serial, importlib.util
from pathlib import Path

# needtocheck/jetson_parser.py'yi yukle (TEK GERCEK KAYNAK)
spec = importlib.util.spec_from_file_location("jp", "needtocheck/jetson_parser.py")
jp = importlib.util.module_from_spec(spec); spec.loader.exec_module(jp)

PORT = "/dev/serial/by-id/usb-STMicroelectronics_STM32_Virtual_ComPort_367D34503235-if00"
ser = serial.Serial(PORT, 115200, timeout=0)   # raw
proto = jp.Protocol()

import time
t0 = time.time()
while True:
    # 1) STM'den geleni coz
    for pkt in proto.feed(ser.read(256)):
        if pkt["type"] == jp.TYPE_STATUS:
            s = pkt["status"]
            # ornek: s["sol_motor"], s["pan"], s["failsafe"], s["jetson_link"], ...
            pass

    # 2) ~10-20 Hz COMMAND + HEARTBEAT gonder
    up = int((time.time() - t0) * 1000)
    ser.write(proto.build_command(sol=0, sag=0, pan=90, tilt=90,
                                  lazer=0, mod=None, auto_request=True))
    ser.write(proto.build_heartbeat(uptime_ms=up))
    time.sleep(0.05)
```

Çalışan, iki yönü de test eden tam örnek:
[`sim/jetson_link_test.py`](sim/jetson_link_test.py) — `python3 sim/jetson_link_test.py <port>`.

---

## 9. Hızlı doğrulama listesi (otonom ekip için)

- [ ] `python3 needtocheck/jetson_parser.py` → CRC self-test OK.
- [ ] `python3 sim/jetson_link_test.py <by-id-port>` → önce STATUS gelir
      (`jetson_link=False`), sonra gönderince **`jetson_link=True`**, `kayip=0`.
- [ ] Kendi kodunuzda `feed()` STATUS'ları çözüyor; `build_command`/`build_heartbeat`
      ile gönderiyorsunuz; COMMAND'ı ≥5 Hz, HEARTBEAT'i ~10 Hz.
- [ ] **Araç hareket etmiyorsa panik yok** — arbitrasyon (`Drive_Update`) henüz
      bağlı değil (bkz. §0.2). Link/telemetri çalışıyorsa haberleşme tamamdır.
