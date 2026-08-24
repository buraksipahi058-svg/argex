# Reactor firmware + Base Station telemetri

Reactor V1.2 (UART) tank-drive firmware'inin, **STM32↔Jetson telemetri protokolü
eklenmiş** hâli. Base Station'ın okuduğu STATUS/HEARTBEAT çerçeveleri **birebir
aynı** olduğu için `jetson/`, `proto/`, `backend/`, `src/` tarafında **hiçbir
değişiklik gerekmez**.

## Klasör içeriği
- `reactor_telemetry.ino` — entegre firmware (eklenen satırlar `// [TELEMETRI]`).
- `haberlesme.h` / `haberlesme.cpp` — `needtocheck/`'ten **kopya**; protokol
  aynen korundu, **yalnızca** Jetson UART pini değişti (USART1, TX=PB6, RX=PB7).

> Arduino IDE: ana sketch adı klasör adıyla aynı olmalı → dosya
> `reactor_telemetry/reactor_telemetry.ino`. `haberlesme.*` aynı klasörde durur.

## Kablolama (yeni eklenen tek bağlantı)
| STM32 | → | Jetson |
|---|---|---|
| **PB6** (USART1 TX) | → | Jetson UART **RX** |
| **GND** | → | Jetson **GND** |

Baud **115200**. Tek yönlü: sadece PB6 bağlanır (PB7/RX kullanılmaz). Jetson
tarafında `jetson/config.yaml` → `source.type: serial`, `serial_port` = Jetson'ın
o UART cihazı (örn. `/dev/ttyTHS1`), `serial_baud: 115200`.

## Neden USART1/PB6
Reactor firmware'inde diğer UART'lar dolu: USART2(PA2)=sol motor,
USART6(PC6)=sağ motor, USART3(PB10/11)=CRSF. USART1 boş ve PB6/PB7 bu projede
başka hiçbir şeyle çakışmıyor (servo PB0/1, ateş PE7, LED PD12–15, buton PA0,
USB PA9–12 hepsi ayrı). **Timer kullanılmıyor** (UART + `millis()`), o yüzden
servo/motor ile timer çakışması yok.

## Telemetri alan eşlemesi
| STATUS alanı | Kaynak |
|---|---|
| `solMotor` / `sagMotor` | `surusModu()`'da uygulanan `sol`/`sag` (MAX_GUC sonrası) |
| `pan` / `tilt` | servo açıları (SERVO_AKTIF=0 iken 90 nötr) |
| `lazer` | atesleme çıkışı (SERVO_AKTIF=0 iken 0) |
| `aktifMod` | 0=sürüş / 1=lazer |
| `elrsLink` | `crsf.isLinkUp()` |
| `durum` | yalnızca `ST_FAILSAFE` (link yok → 1) |

## Base Station'da ne görünür / görünmez
- **Çalışır:** motorlar (L/R), mod, ELRS link, failsafe, seq/rate/STM uptime
  (+ servo açıksa pan/tilt/lazer).
- **Statik/0:** otonomi (AUTO_EN), CMD_TIMEOUT, STM↔Jetson link (durum biti),
  CRC_ERR — çünkü bu firmware Jetson'dan **komut ALMIYOR** (manuel-only). Panelde
  OPERATION hep **MANUAL** görünür; bu doğru davranıştır.

## Değişmeyen taahhüt
Tel üzerindeki protokol (çerçeve/CRC/STATUS/HEARTBEAT/zamanlama) **hiç
değişmedi**; sadece pin ve gönderim entegrasyonu eklendi. `needtocheck/`
orijinalleri de dokunulmadan durur.
