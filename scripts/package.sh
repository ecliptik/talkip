#!/bin/sh
# package.sh: the release files, after build.sh. Output: build/talkip-<version>.dsk (800K HFS) and
# build/talkip-<version>.sit (StuffIt 1.5), each with the five tools, the two INITs
# and a Read Me.
# Needs hfsutils (hformat, hcopy) and the sit archiver with its macbinary_split.py helper
# (EMULATORS_DIR, default ~/emulators: tools/sit/sit and scripts/macbinary_split.py).
set -eu
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^project(talkip VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
EMULATORS_DIR=${EMULATORS_DIR:-$HOME/emulators}
SIT_BIN=$EMULATORS_DIR/tools/sit/sit
MACBIN_SPLIT=$EMULATORS_DIR/scripts/macbinary_split.py
B=build
NAME="talkip $VERSION"
OUT=$B/talkip-$VERSION

# build output -> name on the Mac
FILES="TalkIPStatus:TalkIP Status
TalkIPPing:TalkIP Ping
TalkIPLookup:TalkIP Lookup
TalkIPFetch:TalkIP Fetch
TalkIPWeb:TalkIP Web
TalkIPDriver:TalkIP Driver
AppleTalkOn:AppleTalk On"

STAGE=$B/.release
rm -rf "$STAGE" "$OUT.dsk" "$OUT.sit"
mkdir -p "$STAGE/sit"

# MacBinary copies under their Mac names (header name and CRC rewritten), for hcopy and sit
echo "$FILES" | while IFS=: read -r src mac; do
    python3 - "$B/$src.bin" "$STAGE/$mac.bin" "$mac" <<'EOF'
import sys
src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3].encode('mac_roman')
mb = bytearray(open(src, 'rb').read())
mb[1] = len(name)
mb[2:65] = name.ljust(63, b'\0')
crc = 0
for b in mb[:124]:
    crc ^= b << 8
    for _ in range(8):
        crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
mb[124:126] = crc.to_bytes(2, 'big')
open(dst, 'wb').write(mb)
EOF
done
sed "s/@VERSION@/$VERSION/" resources/ReadMe.txt > "$STAGE/Read Me"

# 800K disk
dd if=/dev/zero of="$OUT.dsk" bs=1024 count=800 2>/dev/null
hformat -l "$NAME" "$OUT.dsk" >/dev/null
hmount "$OUT.dsk" >/dev/null
hcopy -t "$STAGE/Read Me" ":Read Me"
hattrib -t TEXT -c ttxt ":Read Me"
echo "$FILES" | while IFS=: read -r src mac; do
    hcopy -m "$STAGE/$mac.bin" ":$mac"
done
hls -l
humount >/dev/null

# StuffIt archive of the same files. Not of a folder: sit would take the .rsrc/.info sidecars in it for files.
tr '\n' '\r' < "$STAGE/Read Me" > "$STAGE/sit/Read Me"
python3 "$MACBIN_SPLIT" make-info "$STAGE/sit/Read Me" TEXT ttxt
echo "$FILES" | while IFS=: read -r src mac; do
    python3 "$MACBIN_SPLIT" split "$STAGE/$mac.bin" "$STAGE/sit/$mac"
done
set -- "Read Me"
while IFS=: read -r src mac; do
    set -- "$@" "$mac"
done <<EOF
$FILES
EOF
(cd "$STAGE/sit" && "$SIT_BIN" -o "../../talkip-$VERSION.sit" "$@" >/dev/null)

ls -l "$OUT.dsk" "$OUT.sit"
