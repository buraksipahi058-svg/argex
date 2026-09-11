#!/usr/bin/env bash
# ============================================================================
#  start_video.sh — JETSON'da kamera yayinini tek komutla baslatir.
#
#  Ne yapar:
#    1) Eski MediaMTX + ffmpeg process'lerini durdurur (temiz yeniden baslatma).
#    2) (opsiyonel) --temizle ile eski kayitlari siler (ONAY sorar).
#    3) MediaMTX'i baslatir (/root/mm.yml).
#    4) 3 kamerayi push eder (turret + front + rear).
#
#  Kameralar SABIT kimlikle bulunur (v4l2-ctl), /dev/videoN numarasi kaysa da calisir:
#    turret = C922            (isimden)
#    front  = USB portu 2.2   (fiziksel port)
#    rear   = USB portu 2.1   (fiziksel port)
#  Fiziksel on/arka ters ise asagidaki FRONT_PORT / REAR_PORT degerlerini degistir.
#
#  Kullanim:
#    /root/argex/scripts/start_video.sh              # kayitlari KORUR
#    /root/argex/scripts/start_video.sh --temizle    # eski kayitlari da siler (onayli)
#    /root/argex/scripts/start_video.sh --temizle -y # onaysiz siler
# ============================================================================
set -u

REC_DIR=/opt/ugv-recordings
MM_BIN=/root/mediamtx
MM_CFG=/root/mm.yml
FRONT_PORT='usb-2\.2'   # on kameranin USB portu
REAR_PORT='usb-2\.1'    # arka kameranin USB portu

# --- bayraklar ---
CLEAR=0; FORCE=0
for a in "$@"; do
  case "$a" in
    --temizle) CLEAR=1 ;;
    -y|--yes)  FORCE=1 ;;
    *) echo "bilinmeyen secenek: $a (--temizle / -y)"; exit 2 ;;
  esac
done

# --- kamera cihaz dugumlerini v4l2-ctl'den coz (numara kaysa da bulur) ---
node_for() {  # $1 = baslik satirinda aranacak desen
  v4l2-ctl --list-devices 2>/dev/null | awk -v pat="$1" '
    /^[^ \t]/       { keep = ($0 ~ pat) }
    /\/dev\/video/  { if (keep) { gsub(/[ \t]/,""); print; exit } }'
}
TURRET=$(node_for 'C922')
FRONT=$(node_for "$FRONT_PORT")
REAR=$(node_for "$REAR_PORT")

# --- 1) eski process'leri durdur ---
echo ">> eski MediaMTX/ffmpeg durduruluyor..."
pkill -f "$MM_BIN" 2>/dev/null
pkill -f 'ffmpeg.*rtsp://127.0.0.1:8554' 2>/dev/null
sleep 1

# --- 2) (opsiyonel) eski kayitlari sil ---
if [ "$CLEAR" = 1 ] && [ -d "$REC_DIR" ]; then
  n=$(find "$REC_DIR" -type f -name '*.mp4' 2>/dev/null | wc -l)
  sz=$(du -sh "$REC_DIR" 2>/dev/null | cut -f1)
  if [ "${n:-0}" -gt 0 ]; then
    echo "!! $REC_DIR icinde $n kayit ($sz) SILINECEK. Bu geri ALINAMAZ."
    if [ "$FORCE" != 1 ]; then
      read -r -p "   Silinsin mi? (e/h): " ans
      [ "$ans" = "e" ] || { echo "   iptal — kayitlar korundu."; CLEAR=0; }
    fi
    [ "$CLEAR" = 1 ] && rm -rf "${REC_DIR:?}/"* && echo "   eski kayitlar silindi."
  else
    echo ">> silinecek eski kayit yok."
  fi
fi

# --- 3) MediaMTX baslat ---
echo ">> MediaMTX baslatiliyor..."
"$MM_BIN" "$MM_CFG" >/tmp/mediamtx.log 2>&1 &
sleep 2
if ! pgrep -f "$MM_BIN" >/dev/null; then
  echo "!! MediaMTX BASLAMADI. Log:"; tail -8 /tmp/mediamtx.log; exit 1
fi

# --- 4) kameralari push et ---
push() {
  local dev="$1" name="$2" size="${3:-640x480}" br="${4:-1000k}" fps="${5:-30}"
  if [ -z "$dev" ]; then
    echo "!! $name: kamera BULUNAMADI (takili mi? dogru portta mi?)"; return
  fi
  echo ">> $name <- $dev  (${size} @ ${br} @ ${fps}fps)"
  ffmpeg -hide_banner -nostdin -loglevel warning \
    -f v4l2 -input_format mjpeg -framerate "$fps" -video_size "$size" -i "$dev" \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -b:v "$br" -maxrate "$br" -bufsize 500k -g "$fps" \
    -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:8554/$name" >"/tmp/$name.log" 2>&1 &
}
# TARET pozlama kilidi: guneste lazer noktasinin kaybolmamasi icin otomatik
# pozlamayi kapatip pozu kisiyoruz (bkz. jetson/config.yaml v4l2_controls).
# Sahada ayarla: cok parlaksa EXPO'yu dusur, karanlikta yukselt (100us birimi).
EXPO=${EXPO:-100}
lock_exposure() {  # $1 = /dev/videoN
  [ -n "$1" ] || return
  for ctl in "auto_exposure=1" "exposure_auto=1" "exposure_time_absolute=$EXPO"              "exposure_absolute=$EXPO" "gain=0" "white_balance_automatic=0"              "white_balance_temperature_auto=0" "backlight_compensation=0"; do
    v4l2-ctl -d "$1" --set-ctrl "$ctl" >/dev/null 2>&1   # bilinmeyen ad = sessiz gec
  done
  echo ">> taret pozlama kilidi: exposure=$EXPO, gain=0 (EXPO=<deger> ile degistir)"
}
lock_exposure "$TURRET"

# turret tam kalite; on/arka 240p + dusuk bitrate + 15fps (CPU, USB ve bant tasarrufu)
push "$TURRET" cam_turret
push "$FRONT"  cam_front 320x240 400k 15
push "$REAR"   cam_rear  320x240 400k 15

# --- 5) USB dokum servisi (base station'daki "USB'YE AT" dugmesi icin) ---
# Tek isli kucuk HTTP ucu: POST /dump -> scripts/dump_recordings.sh calisir.
# Zaten ayaktaysa dokunmaz. Kapatmak icin: pkill -f dump_service.py
if pgrep -f 'dump_service\.py' >/dev/null; then
  echo ">> USB dokum servisi zaten calisiyor (:8099)"
else
  # Script kopyasi /root/start_video.sh olarak da durabiliyor; her iki yerleside dene.
  SVC="${UGV_DUMP_SERVICE:-}"
  if [ -z "$SVC" ]; then
    for c in "$(dirname "$0")/../jetson/dump_service.py" /root/argex/jetson/dump_service.py; do
      [ -f "$c" ] && SVC="$c" && break
    done
  fi
  if [ -z "$SVC" ]; then
    echo "!! dump_service.py bulunamadi (UGV_DUMP_SERVICE ile yolunu verebilirsin)"
  else
    python3 "$SVC" >/tmp/dump_service.log 2>&1 &
    sleep 1
    if pgrep -f 'dump_service\.py' >/dev/null; then
      echo ">> USB dokum servisi ayakta (:8099)"
    else
      echo "!! USB dokum servisi baslamadi. Log:"; tail -3 /tmp/dump_service.log
    fi
  fi
fi

# --- durum ---
sleep 3
echo "--- durum (log bos = iyi) ---"
for c in cam_turret cam_front cam_rear; do
  echo "== $c =="; tail -2 "/tmp/$c.log" 2>/dev/null
done
echo ">> bitti. Kayit klasoru: $REC_DIR  (mm.yml'da record: yes ise kayit acik)"
