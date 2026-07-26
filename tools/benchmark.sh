#!/usr/bin/env bash
# benchmark.sh — run one source file through AetherCodec NL/HQ, FLAC, Opus and
# SBC, decode each back, and produce a comparison table + spectrogram grid.
#
#   ./tools/benchmark.sh <original.wav> [--keep-tmp] [--no-plot]
#                        [--shift-<codec> N] [--bitpool N] [--opus-bitrate N]
#
# Methodology: SBC and Opus cannot do 96 kHz, so the source is downsampled to
# 48 kHz ONCE and that single file is fed to all five codecs — AetherCodec
# included. Every SNR is measured against that same reference. A missing rival
# tool skips its row; it never aborts the run.
#
# Deliberately not set -e: a codec that fails must degrade to a skipped row.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${AETHER_BUILD_DIR:-$REPO_ROOT/build}"
OUT_DIR="${AETHER_BENCH_OUT:-$PWD}"

SBC_BITPOOL=53          # standard A2DP "high quality" bitpool
OPUS_BITRATE=510        # Opus hard ceiling
KEEP_TMP=0
NO_PLOT=0
SOURCE=""
PY_EXTRA=()

# ---------------------------------------------------------------- args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep-tmp)       KEEP_TMP=1; shift ;;
    --no-plot)        NO_PLOT=1; shift ;;
    --bitpool)        SBC_BITPOOL="$2"; shift 2 ;;
    --opus-bitrate)   OPUS_BITRATE="$2"; shift 2 ;;
    --shift-*)        PY_EXTRA+=("$1" "$2"); shift 2 ;;
    -h|--help)        sed -n '2,14p' "${BASH_SOURCE[0]}"; exit 0 ;;
    -*)               echo "unknown option: $1" >&2; exit 1 ;;
    *)                SOURCE="$1"; shift ;;
  esac
done

if [[ -z "$SOURCE" ]]; then
  echo "usage: $0 <original.wav> [--keep-tmp] [--no-plot] [--shift-<codec> N]" >&2
  exit 1
fi
if [[ ! -f "$SOURCE" ]]; then
  echo "error: no such file: $SOURCE" >&2
  exit 1
fi

# ---------------------------------------------------------------- deps
echo "== Dependency check =="

have() { command -v "$1" >/dev/null 2>&1; }

# Python must have numpy. Prefer an explicit interpreter, then a local venv.
PY=""
for cand in "${AETHER_PY:-}" "$REPO_ROOT/.venv-bench/bin/python" \
            "$REPO_ROOT/.venv/bin/python" python3; do
  [[ -z "$cand" ]] && continue
  if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import numpy' 2>/dev/null; then
    PY="$cand"; break
  fi
done

FATAL=0
if have sox; then
  echo "  sox              OK   ($(command -v sox))"
else
  echo "  sox              MISSING — required.  sudo apt install sox"
  FATAL=1
fi
if [[ -n "$PY" ]]; then
  echo "  python3 + numpy  OK   ($PY)"
else
  echo "  python3 + numpy  MISSING — required."
  echo "                   sudo apt install python3-numpy python3-matplotlib"
  echo "                   or: python3 -m venv .venv-bench && .venv-bench/bin/pip install numpy matplotlib"
  FATAL=1
fi
for t in aether_encode aether_decode; do
  if [[ -x "$BUILD_DIR/tools/$t" ]]; then
    echo "  $t     OK"
  else
    echo "  $t     MISSING at $BUILD_DIR/tools/$t — build first (see CLAUDE.md)"
    FATAL=1
  fi
done
[[ $FATAL -eq 1 ]] && { echo; echo "Cannot continue."; exit 1; }

HAVE_MPL=0
"$PY" -c 'import matplotlib' 2>/dev/null && HAVE_MPL=1
[[ $HAVE_MPL -eq 1 ]] && echo "  matplotlib       OK" \
                      || echo "  matplotlib       MISSING — table still prints, PNG skipped"

# Optional rivals. Each missing one prints its install line and skips its row.
declare -A SKIP_REASON
HAVE_FLAC=0; HAVE_OPUS=0; HAVE_SBC=0
if have flac; then HAVE_FLAC=1; echo "  flac             OK"
else echo "  flac             MISSING — sudo apt install flac"
     SKIP_REASON[flac]="\`flac\` not installed (\`sudo apt install flac\`)"; fi

if have opusenc && have opusdec; then HAVE_OPUS=1; echo "  opusenc/opusdec  OK"
else echo "  opusenc/opusdec  MISSING — sudo apt install opus-tools"
     SKIP_REASON[opus]="\`opusenc\`/\`opusdec\` not installed (\`sudo apt install opus-tools\`)"; fi

if have sbcenc && have sbcdec; then HAVE_SBC=1; echo "  sbcenc/sbcdec    OK"
else
  echo "  sbcenc/sbcdec    MISSING — sudo apt install sbc-tools"
  echo "                   (not packaged on some distros; then build from the"
  echo "                    BlueZ 'sbc' source: git clone git://git.kernel.org/pub/scm/bluetooth/sbc.git"
  echo "                    && ./bootstrap && ./configure && make)"
  SKIP_REASON[sbc]="\`sbcenc\`/\`sbcdec\` not installed (\`sudo apt install sbc-tools\`, or build from the BlueZ \`sbc\` source)"
fi
echo

# ---------------------------------------------------------------- tmpdir
TMP="$(mktemp -d "${TMPDIR:-/tmp}/aether-bench.XXXXXX")"
cleanup() {
  if [[ $KEEP_TMP -eq 1 ]]; then echo "Temp files kept in $TMP"
  else rm -rf "$TMP"; fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- reference
REF="$TMP/ref48.wav"
echo "== Building the 48 kHz reference (single ground truth for all codecs) =="
if ! sox "$SOURCE" -b 24 -c 2 -r 48000 "$REF" 2>"$TMP/sox.err"; then
  echo "error: sox failed to produce the 48 kHz reference:" >&2
  cat "$TMP/sox.err" >&2
  exit 1
fi
REF_FRAMES=$(soxi -s "$REF")
DURATION=$(soxi -D "$REF")
RAW_BYTES=$(( REF_FRAMES * 2 * 3 ))       # frames * channels * 3 bytes (24-bit)
echo "  $SOURCE -> $REF"
echo "  48000 Hz / 24-bit / 2 ch, ${REF_FRAMES} frames, ${DURATION}s, ${RAW_BYTES} raw PCM bytes"
echo

# ---------------------------------------------------------------- helpers
ENTRIES=()   # JSON fragments, one per codec that produced a decoded WAV

fsize() { stat -c %s "$1" 2>/dev/null || echo 0; }

# record <key> <label> <encoded_file> <decoded_wav>
record() {
  local key="$1" label="$2" enc="$3" dec="$4"
  local bytes kbps
  bytes=$(fsize "$enc")
  if [[ "$bytes" -le 0 || ! -s "$dec" ]]; then
    echo "  !! $label produced no usable output — skipping"
    SKIP_REASON[$key]="encode/decode produced no usable output"
    return 1
  fi
  kbps=$("$PY" -c "print(f'{$bytes*8/$DURATION/1000:.1f}')")
  echo "  $label: ${bytes} bytes, ${kbps} kbps"
  ENTRIES+=("{\"key\":\"$key\",\"label\":\"$label\",\"bytes\":$bytes,\"bitrate_kbps\":$kbps,\"decoded\":\"$dec\"}")
  return 0
}

echo "== Encoding / decoding =="

# ---- 1 & 2. AetherCodec NL and HQ ------------------------------------------
for mode in nl hq; do
  up=$(echo "$mode" | tr '[:lower:]' '[:upper:]')
  enc="$TMP/aether_$mode.aether"
  dec="$TMP/decoded_aether_$mode.wav"
  if "$BUILD_DIR/tools/aether_encode" "$REF" "$enc" --mode "$mode" >"$TMP/ae_$mode.log" 2>&1 \
     && "$BUILD_DIR/tools/aether_decode" "$enc" "$dec" >>"$TMP/ae_$mode.log" 2>&1; then
    record "aether_$mode" "AetherCodec $up" "$enc" "$dec"
  else
    echo "  !! AetherCodec $up failed:"; sed 's/^/     /' "$TMP/ae_$mode.log"
    SKIP_REASON[aether_$mode]="aether_encode/aether_decode failed (see run log)"
  fi
done

# ---- 3. FLAC ---------------------------------------------------------------
if [[ $HAVE_FLAC -eq 1 ]]; then
  enc="$TMP/x.flac"; dec="$TMP/decoded_flac.wav"
  if flac --best --silent -f -o "$enc" "$REF" 2>"$TMP/flac.log" \
     && flac -d --silent -f -o "$dec" "$enc" 2>>"$TMP/flac.log"; then
    record flac "FLAC (--best)" "$enc" "$dec"
  else
    echo "  !! FLAC failed:"; sed 's/^/     /' "$TMP/flac.log"
    SKIP_REASON[flac]="flac encode/decode failed"
  fi
fi

# ---- 4. Opus ---------------------------------------------------------------
if [[ $HAVE_OPUS -eq 1 ]]; then
  enc="$TMP/x.opus"; dec="$TMP/decoded_opus.wav"
  if opusenc --quiet --bitrate "$OPUS_BITRATE" "$REF" "$enc" 2>"$TMP/opus.log" \
     && opusdec --quiet --force-wav "$enc" "$dec" 2>>"$TMP/opus.log"; then
    record opus "Opus (${OPUS_BITRATE} kbps)" "$enc" "$dec"
  else
    echo "  !! Opus failed:"; sed 's/^/     /' "$TMP/opus.log"
    SKIP_REASON[opus]="opusenc/opusdec failed"
  fi
fi

# ---- 5. SBC ----------------------------------------------------------------
# SBC's tools are the fiddly ones: sbcenc reads ONLY Sun/NeXT .au (S16_BE) and
# writes the stream to stdout; sbcdec writes .au back. sox converts at both
# ends. SBC is 16-bit only — that is its own limitation, not a handicap we
# impose, and it is measured against the same 24-bit reference as everyone else.
if [[ $HAVE_SBC -eq 1 ]]; then
  sin="$TMP/sbc_in.au"; enc="$TMP/x.sbc"; raw="$TMP/decoded_sbc.au"
  dec="$TMP/decoded_sbc.wav"
  ok=1
  sox "$REF" -t au -e signed-integer -b 16 -c 2 -r 48000 "$sin" 2>"$TMP/sbc.log" || ok=0
  [[ $ok -eq 1 ]] && { sbcenc -b "$SBC_BITPOOL" -j -s 8 -B 16 "$sin" >"$enc" \
                       2>>"$TMP/sbc.log" || ok=0; }
  [[ $ok -eq 1 && ! -s "$enc" ]] && { echo "sbcenc produced an empty stream" >>"$TMP/sbc.log"; ok=0; }
  [[ $ok -eq 1 ]] && { sbcdec -f "$raw" "$enc" >>"$TMP/sbc.log" 2>&1 || ok=0; }
  [[ $ok -eq 1 ]] && { sox "$raw" -b 24 -c 2 -r 48000 "$dec" >>"$TMP/sbc.log" 2>&1 || ok=0; }
  if [[ $ok -eq 1 ]]; then
    record sbc "SBC (bitpool $SBC_BITPOOL)" "$enc" "$dec"
  else
    echo "  !! SBC failed:"; sed 's/^/     /' "$TMP/sbc.log"
    SKIP_REASON[sbc]="sbcenc/sbcdec pipeline failed"
  fi
fi
echo

if [[ ${#ENTRIES[@]} -eq 0 ]]; then
  echo "No codec produced output — nothing to report." >&2
  exit 1
fi

# ---------------------------------------------------------------- manifest
declare -A LABELS=( [flac]="FLAC" [opus]="Opus" [sbc]="SBC"
                    [aether_nl]="AetherCodec NL" [aether_hq]="AetherCodec HQ" )
SKIPPED=()
for k in "${!SKIP_REASON[@]}"; do
  SKIPPED+=("{\"label\":\"${LABELS[$k]:-$k}\",\"reason\":\"${SKIP_REASON[$k]}\"}")
done

MANIFEST="$TMP/manifest.json"
{
  echo "{"
  echo "  \"source\": \"$SOURCE\","
  echo "  \"reference\": \"$REF\","
  echo "  \"rate\": 48000, \"bits\": 24, \"channels\": 2,"
  echo "  \"duration_s\": $DURATION,"
  echo "  \"raw_ref_bytes\": $RAW_BYTES,"
  echo "  \"codecs\": [$(IFS=,; echo "${ENTRIES[*]}")],"
  echo "  \"skipped\": [$(IFS=,; echo "${SKIPPED[*]:-}")]"
  echo "}"
} > "$MANIFEST"

# ---------------------------------------------------------------- report
PLOT_ARG=()
[[ $NO_PLOT -eq 1 || $HAVE_MPL -eq 0 ]] && PLOT_ARG=(--no-plot)
"$PY" "$REPO_ROOT/tools/benchmark_report.py" "$MANIFEST" \
      --out-dir "$OUT_DIR" "${PLOT_ARG[@]}" "${PY_EXTRA[@]+"${PY_EXTRA[@]}"}"
