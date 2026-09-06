# İKA Base Station — Sıfırdan Başlatma Rehberi

Bu rehber, **her şey kapalıyken** sistemi baştan ayağa kaldırmak içindir. Hiç
bilmeyen biri bile takip edebilsin diye her komut ne yapıyor açıklanmıştır.
Bu oturumda gerçekten çalışan adımlara göre yazıldı.

> **En sık 3 hata (peşinen oku):**
> 1. Tarayıcıda **`127.0.0.1` yazma, `localhost` yaz** (Vite sadece onu dinliyor).
> 2. `.env.local` dosyasını **tırnaksız** yaz (`echo "..."` tırnağı dosyaya koyar).
> 3. Komutu **doğru makinede** çalıştır — `[JETSON]` mi `[LAPTOP]` mı, her adımda yazıyor.

---

## 0. Ne nerede çalışır? (önce bunu anla)

İki bilgisayar var:

| Bileşen | Çalıştığı yer | Ne işe yarar |
|---|---|---|
| **Kameralar** (MediaMTX + ffmpeg) | **JETSON** (araçta) | Kameralar fiziksel olarak Jetson'a takılı |
| **Gateway** (ESP telemetri okur) | **JETSON** | ESP seri kablosu Jetson'da *(şu an sorunlu, bkz. Bölüm 6)* |
| **Backend** (telemetri sunucusu) | **LAPTOP** | Jetson'dan gelen telemetriyi panele iletir |
| **Frontend / Panel** | **LAPTOP** | İzlediğin ekran (`http://localhost:5173`) |

Özet: **JETSON = araç tarafı** (kamera + veri kaynağı), **LAPTOP = izleme tarafı**.
Panel hep laptopta açılır. Video ile telemetri **ayrı** yollardan gider; biri
bozuksa diğeri çalışır (yani telemetri olmasa da kameraları görebilirsin).

---

## Güncel değerler (bu kurulum için)

> IP'ler ağdan otomatik (DHCP) alınır ve **değişebilir**. Aşağıdakiler şu anki
> değerler; her açılışta doğrula (nasıl olduğu Bölüm 1'de).

| Ne | Değer |
|---|---|
| Jetson IP | `192.168.0.200` |
| Laptop IP | `192.168.0.218` |
| Jetson'da repo | `/root/argex` (dal: `lazerson`) |
| GitHub repo | `https://github.com/buraksipahi058-svg/argex.git` |

---

## 1. Ağ ve IP'leri bul

1. Jetson ile laptop **aynı Wi-Fi / ağda** olmalı.
2. Laptoptan Jetson'a **SSH** ile bağlan (Jetson'da komut çalıştırmak için):

   Laptopta bir terminal (Git Bash veya PowerShell) aç:
   ```bash
   ssh root@192.168.0.200
   ```
   Parola sorarsa gir. Artık bu terminal **[JETSON]** oturumudur.

3. **[JETSON]** IP'sini öğren:
   ```bash
   hostname -I
   ```
   İlk adres Jetson'ın IP'si. (SSH için kullandığın adresle aynı olmalı.)

4. **[LAPTOP]** IP'sini öğren (laptopun kendi terminalinde):
   ```bash
   ipconfig
   ```
   `192.168.0.x` ile başlayan **IPv4 Address** satırı laptopun IP'si.
   (`192.168.56.x` görürsen o sanal adaptör, onu kullanma.)

> Jetson'a hiç bağlanamıyorsan (SSH takılıyor): aynı ağda değilsiniz ya da
> Jetson'ın IP'si değişmiş demektir. Jetson'a ekran/klavye takıp `hostname -I`
> ile yeni IP'yi öğren.

---

## 2. LAPTOP hazırlığı (repo bilgisayarda yoksa — sadece ilk sefer)

Bu bölüm **bir kez** yapılır. Sonraki açılışlarda atla.

1. Şunları kur (yoksa):
   - **Git** — https://git-scm.com
   - **Node.js 18+** — https://nodejs.org
   - **Python 3.10+** — https://python.org

2. Repoyu indir (laptopun terminali):
   ```bash
   cd /c/Users/burak/Desktop
   ```
   ```bash
   git clone https://github.com/buraksipahi058-svg/argex.git ugv-base-station
   ```
   ```bash
   cd ugv-base-station && git checkout lazerson
   ```

3. Frontend paketlerini kur:
   ```bash
   npm install
   ```

4. Backend için Python ortamı kur:
   ```bash
   python -m venv .venv
   ```
   ```bash
   .venv/Scripts/python -m pip install -r backend/requirements.txt
   ```
   ```bash
   .venv/Scripts/python scripts/gen_proto.py
   ```

> Not: Kamera izlemek için backend şart değil. Backend'i sadece telemetri
> (ESP) çalışınca kullanacaksın.

---

## 3. JETSON hazırlığı (kod güncel mi)

**[JETSON]** oturumunda (Bölüm 1'deki SSH):

```bash
cd /root/argex
```
```bash
git pull origin lazerson
```
> "local changes" hatası verirse (Jetson kopyası sadece çalıştırma için):
> ```bash
> git fetch origin && git checkout -f -B lazerson origin/lazerson && git reset --hard origin/lazerson
> ```

Bağımlılıklar + protobuf (Jetson'da hazır venv var, `python3` değil `.venv/bin/python` kullan):
```bash
.venv/bin/pip install -r jetson/requirements.txt && .venv/bin/python scripts/gen_proto.py
```

---

## 4. JETSON: Kameraları başlat

### 4.1 MediaMTX kurulu mu? (bir kez indir)
Kontrol et:
```bash
ls -l /root/mediamtx
```
**Yoksa** indir (Jetson internete bağlı olmalı):
```bash
cd /root && url=$(wget -qO- https://api.github.com/repos/bluenviron/mediamtx/releases/latest | grep -o 'https://[^"]*linux_arm64\.tar\.gz' | head -1) && wget -q "$url" -O mediamtx.tar.gz && tar xzf mediamtx.tar.gz mediamtx && ls -l /root/mediamtx
```

### 4.2 Küçük config (bir kez oluştur)
Tüm yayın yollarına izin veren minik ayar:
```bash
printf 'paths:\n  all_others:\n' > /root/mm.yml
```

### 4.3 Kamera kimlikleri (neden numara kullanmıyoruz)
`/dev/video0`, `video2` gibi numaralar reboot/çıkar-tak sonrası **kayar**. O yüzden
script kameraları **sabit kimlikle** bulur; sen bir şey ayarlamazsın:

- **turret (C922):** benzersiz seri numarası var → `/dev/v4l/by-id/...C922...`
- **ön/arka (iki C270):** ikisinin **seri numarası aynı** olduğu için ancak
  **fiziksel USB portundan** ayırt edilir → `/dev/v4l/by-path/...:2.2:...` (ön),
  `...:2.1:...` (arka).

> **Önemli:** Ön ve arka C270'leri **hep aynı USB portunda tut.** Portlarını
> değiştirirsen ön/arka yer değiştirir. Fiziksel olarak ön kameran arka portta
> takılıysa, aşağıdaki script'te `2.2` ve `2.1`'i değiştir.

İstersen kameraların görünüp görünmediğini kontrol et:
```bash
v4l2-ctl --list-devices
```
(C922 + iki C270 görünmeli. Görünmüyorsa kamera takılı değil / USB sorunudur.)

### 4.4 Kamera başlatma script'i (bir kez oluştur)
Kameraları sabit kimlikle bulup MediaMTX + 3 yayını başlatan yardımcı:
```bash
cat > /root/start_video.sh <<'EOF'
#!/usr/bin/env bash
# Kameralar SABIT kimlikle bulunur; video-numarasi kaysa da calisir.
#   turret = C922 (benzersiz seri -> by-id)
#   front  = USB fiziksel port 2.2 (by-path)
#   rear   = USB fiziksel port 2.1 (by-path)
# Fiziksel on/arka ters ise asagidaki 2.2 / 2.1'i degistir.
TURRET=$(ls /dev/v4l/by-id/usb-046d_C922_*-video-index0 2>/dev/null | head -1)
FRONT=$(ls /dev/v4l/by-path/*:2.2:1.0-video-index0 2>/dev/null | head -1)
REAR=$(ls /dev/v4l/by-path/*:2.1:1.0-video-index0 2>/dev/null | head -1)

pkill -f '/root/mediamtx' 2>/dev/null
pkill -f 'ffmpeg.*rtsp://127.0.0.1:8554' 2>/dev/null
sleep 1

/root/mediamtx /root/mm.yml >/tmp/mediamtx.log 2>&1 &
sleep 2

push() {
  local dev="$1" name="$2"
  if [ -z "$dev" ]; then echo "!! $name: kamera BULUNAMADI (takili mi? dogru portta mi?)"; return; fi
  echo ">> $name <- $dev"
  ffmpeg -hide_banner -nostdin -loglevel warning \
    -f v4l2 -input_format mjpeg -framerate 30 -video_size 640x480 -i "$dev" \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -b:v 1000k -maxrate 1000k -bufsize 500k -g 30 \
    -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:8554/$name" >/tmp/$name.log 2>&1 &
}
push "$TURRET" cam_turret
push "$FRONT"  cam_front
push "$REAR"   cam_rear
sleep 3
echo "--- durum (log bos = iyi) ---"
for c in cam_turret cam_front cam_rear; do echo "== $c =="; tail -2 /tmp/$c.log 2>/dev/null; done
EOF
chmod +x /root/start_video.sh
```

### 4.5 Kameraları başlat (her açılışta)
```bash
/root/start_video.sh
```
Her kamera için log **boşsa** = iyi. Hata görürsen Bölüm 7'ye bak.

---

## 5. LAPTOP: Paneli aç

Laptopun **kendi terminalinde** (SSH değil), repo klasöründe:

1. Kamera host'unu ayarla (Jetson IP'si — **tırnaksız!**):
   ```bash
   cd /c/Users/burak/Desktop/ugv-base-station
   ```
   ```bash
   printf 'VITE_CAMERA_HOST=192.168.0.200\n' > .env.local
   ```
   > Jetson IP'si değiştiyse `192.168.0.200` yerine yeni IP'yi yaz.

2. Paneli başlat (ağdaki herkese açık olsun diye `--host`):
   ```bash
   npm run dev -- --host
   ```
   Terminal iki adres yazar; **`http://192.168.0.218:5173`** (laptop LAN IP) paylaşılabilir olandır.

3. **İlk seferde güvenlik duvarı** — başka cihazdan açılması için (Yönetici PowerShell'de bir kez):
   > Başlat → "PowerShell" → sağ tık → **Yönetici olarak çalıştır**:
   ```
   New-NetFirewallRule -DisplayName "Vite Dev 5173" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 5173
   ```

4. Tarayıcıda aç:
   - Laptopta: **`http://localhost:5173`**  ← `127.0.0.1` DEĞİL!
   - Başka cihaz/telefon (aynı ağda): **`http://192.168.0.218:5173`**

**Beklenen:** ÖN / ARKA / TARET karoları canlı. Telemetri "offline" görünür —
normal (backend/ESP çalışmıyor, bkz. Bölüm 6).

---

## 6. Telemetri / ESP (ŞU AN SORUNLU — kamera için gerekmez)

ESP↔Jetson seri hattı şu an çalışmıyor (USB hub `-110` hatası veriyor; kanıt:
iki seri cihaz da aynı hub'da birlikte düşüyor, kameralar ayrı portta sağlam).
**Muhtemel kalıcı çözüm: beslemeli (powered) USB hub** veya seri adaptörleri
doğrudan Jetson portuna almak. Hat düzelince telemetri şöyle açılır:

1. **[JETSON]** ESP portunu bul:
   ```bash
   ls -l /dev/serial/by-id/
   ```
2. Hattı test et (STATUS akmalı, `jetson_link=True` olmalı):
   ```bash
   .venv/bin/python sim/jetson_link_test.py /dev/serial/by-id/<ESP-yolu>
   ```
3. Çalışırsa `jetson/config.yaml`: `source.type: serial`, `serial_port: <ESP-yolu>`,
   `quic.host: 192.168.0.218` (laptop IP).
4. **[JETSON]** gateway: `.venv/bin/python -m jetson.gateway`
5. **[LAPTOP]** backend: `.venv/Scripts/python -m backend.main` → panelde telemetri canlanır.

Detay ve sorun giderme: [otonomicingerekenler.md](otonomicingerekenler.md).

---

## 7. Sorun giderme (bu kurulumda yaşadıklarımız)

| Belirti | Sebep / Çözüm |
|---|---|
| Tarayıcı `localhost:5173`'e girmiyor | `127.0.0.1` yazmışsındır → **`localhost`** yaz. Ya da dev server durmuş → laptopta `npm run dev -- --host` tekrar çalıştır. |
| Kameralar gelmiyor, telemetri offline | Normal olabilir. Kamera için: `.env.local` tırnaksız + doğru Jetson IP mi? Jetson'da `/root/start_video.sh` çalışıyor mu? |
| `start_video.sh` "kamera BULUNAMADI" diyor | O kamera takılı değil ya da beklenen portta değil. `v4l2-ctl --list-devices` ile bak; C270 portu farklıysa script'teki `2.2`/`2.1`'i güncelle. Numaralar (`videoN`) kayması sorun değil — script kimlikle bulur. |
| `.env.local` işe yaramıyor | İçinde tırnak vardır. `cat .env.local` → `VITE_CAMERA_HOST=192.168.0.200` (tırnaksız) olmalı. Düzeltince `npm run dev`'i **yeniden başlat**. |
| Başka cihazdan "unreachable" | (a) Güvenlik duvarı kuralı eklenmemiş (Bölüm 5.3). (b) Öbür cihaz farklı ağda — `192.168.0.x` olduğundan emin ol. |
| ffmpeg `Exit 8` / `400 Bad Request` | MediaMTX o yola izin vermiyor → `/root/mm.yml` (`paths: all_others:`) ile başlatıldığından emin ol. |
| `which mediamtx` boş | MediaMTX kurulu değil → Bölüm 4.1'deki indirme. |
| Kamera `No space left on device` | USB bandı doldu (aynı bus'ta çok kamera). Bir kamerayı kapat ya da çözünürlüğü düşür (`-video_size 424x240`). |
| Seri port `Errno 5` / `-110`, iki port birden ölü | USB hub sorunu. Fişe dokunmadan resetle: <br>`echo '1-2.4' \| sudo tee /sys/bus/usb/drivers/usb/unbind; sleep 2; echo '1-2.4' \| sudo tee /sys/bus/usb/drivers/usb/bind` <br>(`1-2.4` = hub'ın adresi, `lsusb -t` ile doğrula). Tekrar ediyorsa **powered hub** şart. |
| ESP'den 0 bayt | ESP beslenmiyor / firmware yok / GPIO22→adaptör TX-RX ters. Bkz. Bölüm 6. |

---

## 8. Kapatma / yeniden başlatma

**Kapatma:**
- Laptopta `npm run dev` terminalinde **Ctrl+C**.
- Jetson'da: `pkill -f mediamtx; pkill -f ffmpeg`

**Tekrar açarken** (her şey kurulduktan sonra kısa yol):
1. **[JETSON]** SSH → `/root/start_video.sh`
2. **[LAPTOP]** repo klasörü → `npm run dev -- --host`
3. Tarayıcı → `http://localhost:5173`

(IP'ler değişmediyse `.env.local`'a dokunmana gerek yok. Değiştiyse Bölüm 5.1.)

---

*Bu rehber çalışan kamera akışına göre yazıldı. Telemetri (ESP) hattı düzelince
Bölüm 6 devreye girer; kalıcı kamera/kayıt kurulumu için ayrıca
[STEP_BY_STEP.md](STEP_BY_STEP.md) ve [README-basestation.md](README-basestation.md).*
