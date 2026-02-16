#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "[ERR] This test must run as root (use: sudo ./test_root_build.sh)"
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

RUN_ID="$(date +%s)"
TEST_ROOT="/tmp/zocker_e2e_${RUN_ID}"
STORE_DIR="/tmp/zocker_store_${RUN_ID}"
BIN="./zocker"

CTX_SIMPLE="$TEST_ROOT/simple"
CTX_MULTI="$TEST_ROOT/multi"
BASE_ROOT="$TEST_ROOT/base-rootfs"

IMAGE_SIMPLE_V1="cache-demo:${RUN_ID}-v1"
IMAGE_SIMPLE_V2="cache-demo:${RUN_ID}-v2"
IMAGE_MULTI="multi-demo:${RUN_ID}"
RUN_NAME_SIMPLE="simple-run-${RUN_ID}"
RUN_NAME_MULTI="multi-run-${RUN_ID}"

PAYLOAD_SIMPLE="hello-cache-${RUN_ID}"
PAYLOAD_MULTI="artifact-${RUN_ID}"

log() {
  echo "[TEST] $*"
}

fail() {
  echo "[FAIL] $*"
  exit 1
}

cleanup() {
  rm -rf "$TEST_ROOT"
  # keep STORE_DIR for debugging on failure
}
trap cleanup EXIT

rm -rf "$TEST_ROOT" "$STORE_DIR"
mkdir -p "$TEST_ROOT"

find_base_dir() {
  local c="${ZOCKER_TEST_BASEDIR:-}"
  if [[ -n "$c" && "$c" != "/" && -d "$c" && -x "$c/bin/sh" ]]; then
    if chroot "$c" /bin/sh -c ':' >/dev/null 2>&1; then
      echo "$c"
      return 0
    fi
  fi
  return 1
}

copy_bin_with_libs() {
  local bin="$1"
  local root="$2"
  local resolved interp lib

  resolved="$(readlink -f "$bin")"
  [[ -x "$resolved" ]] || return 1

  mkdir -p "$root$(dirname "$resolved")"
  cp -fL "$resolved" "$root$resolved"

  if [[ "$bin" != "$resolved" ]]; then
    mkdir -p "$root$(dirname "$bin")"
    ln -sfn "$resolved" "$root$bin"
  else
    cp -fL "$resolved" "$root$bin"
  fi

  interp="$(
    readelf -l "$resolved" 2>/dev/null \
      | sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p' \
      | head -n 1
  )"
  if [[ -n "$interp" && -f "$interp" ]]; then
    mkdir -p "$root$(dirname "$interp")"
    cp -fL "$interp" "$root$interp"
  fi

  while read -r lib; do
    [[ -n "$lib" ]] || continue
    [[ -f "$lib" ]] || continue
    mkdir -p "$root$(dirname "$lib")"
    cp -fL "$lib" "$root$lib"
  done < <(ldd "$resolved" | awk '/=> \// {print $3} /^\// {print $1}')
}

prepare_minimal_base_rootfs() {
  rm -rf "$BASE_ROOT"
  mkdir -p "$BASE_ROOT/bin" "$BASE_ROOT/tmp" "$BASE_ROOT/proc"

  copy_bin_with_libs /bin/sh "$BASE_ROOT"
  copy_bin_with_libs /bin/cat "$BASE_ROOT"

  if chroot "$BASE_ROOT" /bin/sh -c '/bin/cat /bin/sh >/tmp/preflight-copy && test -s /tmp/preflight-copy' >/dev/null 2>&1; then
    echo "$BASE_ROOT"
    return 0
  fi

  return 1
}

preflight_overlay() {
  local d lower upper work merged

  d="$(mktemp -d /tmp/zocker_overlay_check.XXXXXX)"
  lower="$d/lower"
  upper="$d/upper"
  work="$d/work"
  merged="$d/merged"

  mkdir -p "$lower" "$upper" "$work" "$merged"
  echo ok > "$lower/probe.txt"

  if ! mount -t overlay overlay -o "lowerdir=$lower,upperdir=$upper,workdir=$work" "$merged" >/dev/null 2>&1; then
    rm -rf "$d"
    return 1
  fi

  if [[ ! -f "$merged/probe.txt" ]]; then
    umount "$merged" || true
    rm -rf "$d"
    return 1
  fi

  umount "$merged"
  rm -rf "$d"
  return 0
}

log "Preflight: overlay support"
if ! preflight_overlay; then
  echo "[SKIP] overlay mount is not available in this environment (not a zocker logic error)."
  exit 2
fi

log "Preflight: locate a valid BASEDIR for chroot RUN"
BASE_DIR="$(find_base_dir || true)"
if [[ -z "$BASE_DIR" ]]; then
  log "No external BASEDIR supplied/usable; building minimal rootfs base"
  BASE_DIR="$(prepare_minimal_base_rootfs || true)"
fi
if [[ -z "$BASE_DIR" ]]; then
  echo "[SKIP] Could not create/find a chroot-able base dir."
  echo "       Try: export ZOCKER_TEST_BASEDIR=/path/to/rootfs"
  exit 2
fi
echo "[INFO] Using BASEDIR=$BASE_DIR"

log "Compile zocker with isolated ZOCKER_PREFIX"
rm -f "$BIN"
gcc -w -std=c11 -O2  *.c -o "$BIN"

rm -rf "$STORE_DIR"
if [[ ! -d "$BASE_DIR" ]]; then
  fail "Selected BASEDIR disappeared: $BASE_DIR"
fi
mkdir -p "$CTX_SIMPLE" "$CTX_MULTI"

# -----------------------------
# Test 1: cache behavior
# -----------------------------
cat > "$CTX_SIMPLE/payload.txt" <<ZEOF
$PAYLOAD_SIMPLE
ZEOF

cat > "$CTX_SIMPLE/Zockerfile" <<ZEOF
BASEDIR $BASE_DIR
WORKDIR /tmp/zocker-simple-$RUN_ID
COPY payload.txt payload.txt
RUN /bin/sh -c 'cat /tmp/zocker-simple-$RUN_ID/payload.txt > /tmp/zocker-simple-$RUN_ID/out.txt'
CMD cat /tmp/zocker-simple-$RUN_ID/out.txt
ZEOF

log "Simple build #1"
out1="$TEST_ROOT/build1.log"
start1=$(date +%s%N)
"$BIN" build -f "$CTX_SIMPLE/Zockerfile" -t "$IMAGE_SIMPLE_V1" | tee "$out1"
end1=$(date +%s%N)
ms1=$(( (end1 - start1) / 1000000 ))

log "Simple build #2 (same inputs, different tag -> should reuse cache)"
out2="$TEST_ROOT/build2.log"
start2=$(date +%s%N)
"$BIN" build -f "$CTX_SIMPLE/Zockerfile" -t "$IMAGE_SIMPLE_V2" | tee "$out2"
end2=$(date +%s%N)
ms2=$(( (end2 - start2) / 1000000 ))

if ! grep -q "\[CACHE HIT\]" "$out2"; then
  fail "Second build has no cache hit marker"
fi

layer1=$("$BIN" history "$IMAGE_SIMPLE_V1" | awk 'NR==2 {print $1}')
layer2=$("$BIN" history "$IMAGE_SIMPLE_V2" | awk 'NR==2 {print $1}')

if [[ -z "$layer1" || -z "$layer2" ]]; then
  fail "Could not parse top layers from history"
fi

if [[ "$layer1" != "$layer2" ]]; then
  fail "Top layer differs between identical builds (cache mismatch): $layer1 vs $layer2"
fi

echo "[PASS] Cache hit detected and top layer reused"
echo "[INFO] Build#1=${ms1}ms, Build#2=${ms2}ms"

log "Run cached image and verify output"
run_simple_log="$TEST_ROOT/run_simple.log"
"$BIN" run --name "$RUN_NAME_SIMPLE" --base-image "$IMAGE_SIMPLE_V2" "cat /tmp/zocker-simple-$RUN_ID/out.txt" | tee "$run_simple_log"
if ! grep -q "$PAYLOAD_SIMPLE" "$run_simple_log"; then
  fail "Simple run output mismatch"
fi

# -----------------------------
# Test 2: multi-stage build
# -----------------------------
cat > "$CTX_MULTI/payload.txt" <<ZEOF
$PAYLOAD_MULTI
ZEOF

cat > "$CTX_MULTI/Zockerfile" <<ZEOF
BASEDIR $BASE_DIR AS deps
WORKDIR /tmp/zocker-multi-$RUN_ID
COPY payload.txt /tmp/zocker-multi-$RUN_ID/in.txt
RUN /bin/sh -c 'cat /tmp/zocker-multi-$RUN_ID/in.txt > /tmp/zocker-multi-$RUN_ID/artifact.txt'

BASEDIR $BASE_DIR
COPY --from=deps /tmp/zocker-multi-$RUN_ID/artifact.txt /tmp/zocker-multi-$RUN_ID/final.txt
CMD cat /tmp/zocker-multi-$RUN_ID/final.txt
ZEOF

log "Multi-stage build"
"$BIN" build -f "$CTX_MULTI/Zockerfile" -t "$IMAGE_MULTI" | tee "$TEST_ROOT/multi_build.log"

log "Run multi-stage image and verify copied artifact"
run_multi_log="$TEST_ROOT/run_multi.log"
"$BIN" run --name "$RUN_NAME_MULTI" --base-image "$IMAGE_MULTI" "cat /tmp/zocker-multi-$RUN_ID/final.txt" | tee "$run_multi_log"
if ! grep -q "$PAYLOAD_MULTI" "$run_multi_log"; then
  fail "Multi-stage output mismatch"
fi

if ! "$BIN" history "$IMAGE_MULTI" | grep -q "COPY --from=deps"; then
  fail "Multi-stage history missing COPY --from instruction"
fi

echo "[PASS] Multi-stage build and runtime verification passed"

log "List images"
"$BIN" images

log "Done"
echo "[DONE] All logic checks passed"
echo "[INFO] Binary: $BIN"
echo "[INFO] Store: $STORE_DIR"
echo "[INFO] Logs: $TEST_ROOT"
