#!/usr/bin/env bash
# ============================================================================
#  dump_recordings.sh — yaris sonu tek-komut teslim scripti (JETSON'da calisir)
#
#  Ne yapar:
#    1. Dahili kayit klasorunu (MediaMTX recordPath) takili USB diske kopyalar
#       (rsync: kesintide kaldigi yerden devam eder, dosyalari dogrular).
#    2. `sync` ile tamponlari bosaltir (yazma bitmeden cikarma korumasi).
#    3. Diske bir manifest.txt yazar (dosya listesi + boyut + toplam).
#    4. Diski guvenle unmount eder -> "cikarabilirsin" der.
#
#  Kullanim:
#    bash scripts/dump_recordings.sh /media/ugv-usb
#    bash scripts/dump_recordings.sh /media/ugv-usb --keep-mounted
#    UGV_SRC=/mnt/nvme/ugv-recordings bash scripts/dump_recordings.sh /media/ugv-usb
#
#  Not: exFAT USB disk onerilir (Windows/Mac/Linux okur, 4GB+ dosya sorunu yok).
# ============================================================================
set -euo pipefail

SRC="${UGV_SRC:-/opt/ugv-recordings}"      # dahili kayit klasoru (recordPath tabani)
DEST="${1:-/media/ugv-usb}"                # takili USB diskin mount noktasi
KEEP_MOUNTED=0
[ "${2:-}" = "--keep-mounted" ] && KEEP_MOUNTED=1

say() { printf '\n\033[1m>>> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mHATA: %s\033[0m\n' "$*" >&2; exit 1; }

# --- On kontroller ----------------------------------------------------------
[ -d "$SRC" ]  || die "Kaynak klasor yok: $SRC (MediaMTX recordPath ile ayni mi?)"
if ! find "$SRC" -type f -print -quit | grep -q .; then
  die "Kaynakta hic kayit dosyasi yok: $SRC"
fi
is_mounted() {
  if command -v mountpoint >/dev/null 2>&1; then mountpoint -q "$1"
  else awk -v d="$1" '$2==d{f=1} END{exit !f}' /proc/mounts
  fi
}
is_mounted "$DEST" || die "USB disk $DEST'e mount edilmemis. Once tak/mount et:
  lsblk -f                       # cihazi bul (or. /dev/sda1, exFAT)
  sudo mkdir -p $DEST
  sudo mount /dev/sdXN $DEST     # veya UUID ile"

# --- Yer var mi? ------------------------------------------------------------
need_kb=$(du -sk "$SRC" | cut -f1)
free_kb=$(df -Pk "$DEST" | awk 'NR==2{print $4}')
say "Kaynak boyutu: $(du -sh "$SRC" | cut -f1)   USB bos alan: $(df -Ph "$DEST" | awk 'NR==2{print $4}')"
[ "$free_kb" -ge "$need_kb" ] || die "USB diskte yeterli yer yok."

# --- Kopyala (rsync varsa; yoksa cp) ---------------------------------------
DEST_DIR="$DEST/ugv-recordings"
mkdir -p "$DEST_DIR"
say "Kopyalaniyor: $SRC  ->  $DEST_DIR"
if command -v rsync >/dev/null 2>&1; then
  rsync -a --info=progress2 --partial "$SRC"/ "$DEST_DIR"/
else
  echo "(rsync yok, cp kullaniliyor)"
  cp -a "$SRC"/. "$DEST_DIR"/
fi

# --- Manifest + dogrulama ---------------------------------------------------
say "Manifest yaziliyor"
{
  echo "UGV kamera kayitlari — teslim dokumu"
  echo "Tarih   : $(date -Is)"
  echo "Kaynak  : $SRC"
  echo "Toplam  : $(du -sh "$DEST_DIR" | cut -f1)"
  echo "Dosya   : $(find "$DEST_DIR" -type f | wc -l) adet"
  echo "----------------------------------------"
  ( cd "$DEST_DIR" && find . -type f -printf '%10s  %p\n' | sort -k2 )
} > "$DEST_DIR/manifest.txt"

src_n=$(find "$SRC" -type f | wc -l)
dst_n=$(find "$DEST_DIR" -type f ! -name manifest.txt | wc -l)
[ "$src_n" -eq "$dst_n" ] || die "Dosya sayisi tutmuyor (kaynak $src_n, hedef $dst_n) — tekrar calistir."

# --- Tamponlari bosalt + (varsayilan) unmount ------------------------------
say "Diske yaziliyor (sync)..."
sync

if [ "$KEEP_MOUNTED" -eq 1 ]; then
  say "BITTI. $dst_n dosya kopyalandi. Disk hala mount ($DEST) — kontrol edip
      hazir olunca:  sync && sudo umount $DEST"
else
  say "Unmount ediliyor: $DEST"
  sudo umount "$DEST" && \
    printf '\n\033[1;32mTAMAM — %s dosya. Diski cikarabilirsin, teslime hazir.\033[0m\n' "$dst_n"
fi
