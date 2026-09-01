# Nucleo-F072RB firmware kurulumu

`argexika/` (STM32F407) firmware'inin Nucleo-F072RB portu. Uygulama mantığı
(`crsf`, `reactor`, `servo`, tank-mix, telemetri) **birebir aynı**; sadece donanım
kurulumu (clock, USART/pin/DMA/timer) F072'ye taşındı.

> ⚠️ Bu dosyalar **elde portlandı, henüz derlenip donanımda test EDİLMEDİ.**
> STM32CubeIDE'de derleyip araç üstünde (önce **motorlar havadayken**) doğrula.

---

## 0. Neden CubeMX gerekiyor?

Bu klasör yalnızca **uygulama kaynağını** içerir (`Core/Src`, `Core/Inc`). Bir
STM32 projesinin geri kalanını — F0xx HAL sürücü ağacı, `startup_stm32f072xb.s`,
linker script, `system_stm32f0xx.c`, `stm32f0xx_hal_conf.h` — **STM32CubeIDE üretir.**
Bu yüzden önce boş bir F072RB projesi oluşturup, üretilen `Core/`'un üstüne bu
dosyaları kopyalıyorsun.

---

## 1. Boş proje oluştur (STM32CubeIDE)

1. **File → New → STM32 Project**
2. Part Number: **STM32F072RB** (ya da Board Selector'dan **NUCLEO-F072RB**) → Next
3. İsim ver (örn. `nucleo_f072_ika`), **C** projesi → Finish
4. `.ioc` açılınca:
   - **Clock:** varsayılan bırakabilirsin; kodumuz `SystemClock_Config`'i HSI48
     48 MHz'e kendi ayarlıyor. (İstersen CubeMX'te de HSI48/48MHz seçip tutarlı yap.)
   - **Pinlere DOKUNMANA GEREK YOK** — tüm çevre birim/pin/DMA kurulumu kod içinde
     (`main.c` + `haberlesme.c`) yapılıyor. CubeMX'te USART/TIM/DMA açman **gerekmez.**
5. **Project → Generate Code** (Ctrl+S) → iskelet `Core/` üretilir.

> `stm32f0xx_hal_conf.h` içinde şu modüller **enabled** olmalı (CubeMX varsayılanı
> genelde açık): `HAL_UART_MODULE_ENABLED`, `HAL_TIM_MODULE_ENABLED`,
> `HAL_DMA_MODULE_ENABLED`, `HAL_GPIO_MODULE_ENABLED`, `HAL_RCC_MODULE_ENABLED`,
> `HAL_CORTEX_MODULE_ENABLED`.

---

## 2. Bu dosyaları projeye kopyala

Üretilen projenin `Core/` klasörünü **bu klasörün** dosyalarıyla değiştir/ekle:

| Kaynak (bu repo) | Hedef (CubeIDE projesi) | İşlem |
|---|---|---|
| `Core/Src/main.c` | `Core/Src/main.c` | **değiştir** |
| `Core/Src/stm32f0xx_it.c` | `Core/Src/stm32f0xx_it.c` | **değiştir** |
| `Core/Src/haberlesme.c` | `Core/Src/haberlesme.c` | **ekle** |
| `Core/Src/crsf.c` | `Core/Src/crsf.c` | **ekle** |
| `Core/Src/reactor.c` | `Core/Src/reactor.c` | **ekle** |
| `Core/Src/servo.c` | `Core/Src/servo.c` | **ekle** |
| `Core/Inc/main.h` | `Core/Inc/main.h` | **değiştir** |
| `Core/Inc/haberlesme.h` `crsf.h` `reactor.h` `servo.h` | `Core/Inc/` | **ekle** |

> `stm32f0xx_it.c`'yi değiştirmek istemezsen, sadece iki DMA handler'ını
> (`DMA1_Channel2_3_IRQHandler` ve `DMA1_Channel4_5_6_7_IRQHandler`) üretilen
> dosyanın USER CODE alanlarına yapıştır + `extern DMA_HandleTypeDef hdma_crsf_rx;`
> ekle. Ama SysTick vs. çakışmaması için **komple değiştirmek daha temiz.**

---

## 3. Derle ve yükle

- **Project → Build** (Ctrl+B). Hata yoksa `.elf` üretilir.
- Nucleo'yu USB ile PC'ye tak → **Run** (yeşil ok) ya da sürükle-bırak (`.bin`'i
  `NODE_F072RB` diskine kopyala).

### Derlemede çıkabilecek tek olası sorun: AF makro adları

HAL sürüm farkıyla bazı AF makrolarının adı değişebilir. Derleyici `GPIO_AF4_USART3`
/ `GPIO_AF0_USART4` bulamazsa, **datasheet AF numarasını** doğrudan yaz:

| Kod satırı | Anlamı | Bulunamazsa |
|---|---|---|
| `GPIO_AF1_USART1` (PA10) | CRSF RX | `1` |
| `GPIO_AF1_USART2` (PA2/PA3) | Jetson | `1` |
| `GPIO_AF4_USART3` (PB10) | SOL Reactor | `4` |
| `GPIO_AF0_USART4` (PC10) | SAG Reactor | `0` |
| `GPIO_AF1_TIM3` (PB0/PB1) | Servo | `1` |

---

## 4. Jetson tarafı (bu repo)

- [jetson/config.yaml](../jetson/config.yaml): `serial_port` zaten `/dev/ttyACM0`
  yapıldı. Nucleo takılınca sabit yol için:
  ```bash
  ls -l /dev/serial/by-id/
  ```
  çıkan `usb-STMicroelectronics_STM32_STLink_...-if02` yolunu yaz.
- ModemManager portu kapmasın:
  ```bash
  sudo systemctl disable --now ModemManager
  ```
- Kullanıcı `dialout` grubunda olsun.

---

## 5. Bring-up testi (sırayla)

1. **CRSF:** Kumandayı aç → mavi LED (PC2) yanmalı (link OK). Kapat → kırmızı (PC1).
2. **Servolar:** CH4/CH3 stickleri → PB0/PB1'de PWM, pan/tilt hareket.
3. **Motorlar (HAVADA!):** CH5 ARM (>1700µs) + stick → reactor'lar dönmeli.
   Ters dönerse `main.c`'deki `*_INV_*` işaretini çevir.
4. **Jetson linki:** Nucleo USB'sini Jetson'a al →
   ```bash
   python3 sim/jetson_link_test.py /dev/ttyACM0
   ```
   `jetson_link=True` ve STATUS akışı görülmeli.

---

## 6. Özet: F407 → F072 değişen donanım

| İşlev | F407 | F072 |
|---|---|---|
| Jetson | USART6 PC6/PC7, DMA2_Stream1 | **USART2 PA2/PA3 (VCP), DMA1_Ch5** |
| CRSF | USART2 PA3, DMA1_Stream5 | **USART1 PA10, DMA1_Ch3** |
| SOL Reactor | USART1 PB6 | **USART3 PB10** |
| SAG Reactor | USART3 PD8 | **USART4 PC10** |
| Servo | TIM4 PD12/PD13 | **TIM3 PB0/PB1** |
| EN/FIRE/LED'ler | PE0/PE1/PD14/PD15 | **PC0/PC3/PC1/PC2** |
| Clock | 168 MHz (HSE+PLL) | **48 MHz (HSI48)** |

Ayrıntılı pin haritası: [pinmodev2.md](../pinmodev2.md)
