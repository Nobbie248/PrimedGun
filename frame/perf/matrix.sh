#!/bin/bash
# Runs mock runs for the GPU/resolution table, one after another, each from the same GPU temperature.
#
#   frame/perf/matrix.sh [--seconds N] [--cool C] [--exe PATH] SPEC... [-- extra mockrun options]
#
# SPEC is state:eye:ir:tu, e.g. 02:1728:2:sysmem (tu: sysmem, gmem or - for Turnip's default).
# Each run: mockrun.sh --state S --seconds N --warmup 20 --eye E --set GM8E01:Video_Settings.InternalResolution=IR
# [--tudebug TU] --clocklog --cool C --tag tbl-S-E-irIR-TU. Prints the run dirs; frame/perf/summary.py
# makes the rows. A run marked INVALID (headset session) is repeated.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SECS=120; COOL=55; EXE=(); SPECS=(); EXTRA=()
while [ $# -gt 0 ]; do
  case $1 in
    --seconds) SECS=$2; shift ;;
    --cool) COOL=$2; shift ;;
    --exe) EXE=(--exe "$2"); shift ;;
    --) shift; EXTRA=("$@"); break ;;
    *) SPECS+=("$1") ;;
  esac
  shift
done
for SPEC in "${SPECS[@]}"; do
  IFS=: read -r S E IR TU <<<"$SPEC"
  TUA=(); [ "$TU" != - ] && TUA=(--tudebug "$TU")
  for TRY in 1 2 3; do
    "$HERE/../tools/mockrun.sh" --state "$S" --seconds "$SECS" --warmup 20 --eye "$E" \
      --set "GM8E01:Video_Settings.InternalResolution=$IR" "${TUA[@]}" --clocklog --cool "$COOL" \
      --tag "tbl-s$S-$E-ir$IR-$TU" ${EXE[@]+"${EXE[@]}"} ${EXTRA[@]+"${EXTRA[@]}"} > /dev/null 2>&1
    RC=$?
    D=$(ls -td "${PF_MOCKRUN_HOME:-$HOME/.cache/pf-mockrun}"/runs/*-tbl-s$S-$E-ir$IR-$TU | head -1)
    echo "$SPEC rc=$RC $D"
    [ $RC = 3 ] || break
  done
done
