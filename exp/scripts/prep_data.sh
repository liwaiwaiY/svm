#!/bin/bash
# prep_data.sh <als|xgb> - 准备 IO 密集应用数据集并写入 fio.raw + data_iscsi.raw（两盘内容一致）
#   als : exp/data/ml-25.zip/ml-25m.zip -> ratings_big.csv（放大 8 份带 id 偏移，~5GB 超 4GB 内存）
#   xgb : exp/data/higgs.zip   -> HIGGS.csv（7.5GB 超 4GB 内存）
# 两个 raw（各 9.2G 可用）每次 prep 前 mkfs 清空，只放当前应用的数据。
# 用法: prep_data.sh <als|xgb> [als_factor]
set -u
H=/home/waiai/svm
APP=$1
FACTOR=${2:-8}
sudoc() { echo dxeqqghk | sudo -S bash -c "$1"; }
RAWS="$H/exp/remote/fio.raw $H/exp/iscsi/data_iscsi.raw"

prep_als() {
  local Z=$H/exp/data/ml-25.zip/ml-25m.zip
  [ -f $Z ] || { echo "MISSING $Z"; exit 1; }
  local DIR=$H/exp/data/ml-25m
  [ -f $DIR/ratings.csv ] || (rm -rf $DIR && mkdir -p $DIR && cd $DIR && unzip -q -o -j $Z && echo "  unzipped ml-25m")
  [ -f $DIR/ratings.csv ] || { echo "  no ratings.csv in $DIR"; exit 1; }
  echo "  generating ratings_big.csv (x$FACTOR, ~4.5GB) ..."
  python3 - "$DIR/ratings.csv" "$H/exp/data/ratings_big.csv" "$FACTOR" <<'PYEOF'
import sys
src, dst, fac = sys.argv[1], sys.argv[2], int(sys.argv[3])
# ml-25m: userId 1..162541, movieId 1..62423, 25000095 rows
MAXU, MAXI = 162541, 62423
out = open(dst, "w")
out.write("userId,movieId,rating,timestamp\n")
import itertools
n = 0
with open(src) as f:
    next(f)
    rows = list(f)  # 2 千万行驻留内存 ~600MB，host 内存足够
for k in range(fac):
    off_u, off_i = k * (MAXU + 1), k * (MAXI + 1)
    for line in rows:
        u, i, r, t = line.rstrip("\n").split(",")
        out.write(f"{int(u)+off_u},{int(i)+off_i},{r},{t}\n")
        n += 1
        if n % 20_000_000 == 0:
            print(f"  {n/1e6:.0f}M rows", flush=True)
out.close()
print(f"  done: {n} rows")
PYEOF
  ls -lah $H/exp/data/ratings_big.csv
}

prep_xgb() {
  local Z=$H/exp/data/higgs.zip
  [ -f $Z ] || { echo "MISSING $Z"; exit 1; }
  local CSV=$H/exp/data/HIGGS.csv
  if [ ! -f $CSV ] && [ -f $H/exp/data/HIGGS.csv.gz ]; then
    echo "  gunzip HIGGS.csv.gz ..."; gunzip -k $H/exp/data/HIGGS.csv.gz
  fi
  if [ ! -f $CSV ]; then
    echo "  unzip higgs.zip -> HIGGS.csv ..."; cd $H/exp/data && unzip -q -o $Z
  fi
  [ -f $CSV ] || { echo "  no HIGGS.csv"; exit 1; }
  ls -lah $CSV
}

write_raw() {  # write_raw <raw> <file>
  local RAW=$1 SRC=$2 LOOP
  sudoc "losetup -d \$(losetup -j $RAW 2>/dev/null | cut -d: -f1) 2>/dev/null"
  LOOP=$(echo dxeqqghk | sudo -S losetup -f 2>/dev/null | tr -d '\r')
  echo "  [$RAW] loop=$LOOP mkfs.ext4 ..."
  sudoc "losetup $LOOP $RAW && mkfs.ext4 -F -q $LOOP"
  sudoc "mkdir -p /mnt/rawtmp && mount $LOOP /mnt/rawtmp"
  sudoc "chmod 777 /mnt/rawtmp"
  local SZ=$(du -sh $SRC | cut -f1)
  echo "  copying $SZ -> $(basename $RAW) ..."
  sudoc "cp $SRC /mnt/rawtmp/ && sync"
  sudoc "ls -lah /mnt/rawtmp/"
  sudoc "umount /mnt/rawtmp && losetup -d $LOOP"
  echo "  [$(basename $RAW)] done"
}

echo "=== [$(date +%T)] PREP $APP ==="
if [ "$APP" = "als" ]; then
  prep_als; FILE=$H/exp/data/ratings_big.csv
elif [ "$APP" = "xgb" ]; then
  prep_xgb; FILE=$H/exp/data/HIGGS.csv
else
  echo "usage: prep_data.sh <als|xgb>"; exit 1
fi
for R in $RAWS; do write_raw $R $FILE; done
echo "PREP_${APP}_OK [$(date +%T)]"
