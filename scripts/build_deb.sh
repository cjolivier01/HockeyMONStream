#!/usr/bin/env bash

set -euo pipefail
umask 022

fail() {
  echo "build_deb.sh: $*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage: scripts/build_deb.sh [--target=current|x86_64|jetson] [--bazel=PATH] [--jetson-sysroot=PATH]

Builds a Debian package in ./dist containing hstream runtime binaries, libraries,
GStreamer plugins, and shared configs.
EOF
}

TARGET="current"
BAZEL="${BAZEL:-bazelisk}"
JETSON_SYSROOT="${JETSON_SYSROOT:-/opt/jetson-sysroot}"
DIST_DIR="${DIST_DIR:-dist}"
PACKAGE_NAME="${HSTREAM_PACKAGE_NAME:-hstream}"
VERSION="${HSTREAM_DEB_VERSION:-}"

while (($#)); do
  case "$1" in
    --target=*)
      TARGET="${1#*=}"
      ;;
    --bazel=*)
      BAZEL="${1#*=}"
      ;;
    --jetson-sysroot=*)
      JETSON_SYSROOT="${1#*=}"
      ;;
    --version=*)
      VERSION="${1#*=}"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
  shift
done

case "$TARGET" in
  current)
    case "$(uname -m)" in
      x86_64)
        TARGET="x86_64"
        ;;
      aarch64|arm64)
        TARGET="jetson"
        ;;
      *)
        fail "unsupported host architecture for --target=current: $(uname -m)"
        ;;
    esac
    ;;
esac

declare -a BAZEL_ARGS
case "$TARGET" in
  x86_64|x86|amd64)
    TARGET="x86_64"
    DEB_ARCH="amd64"
    MULTIARCH_TRIPLET="x86_64-linux-gnu"
    BAZEL_ARGS=(--config=opt --cpu=k8)
    ;;
  jetson|aarch64|arm64)
    TARGET="jetson"
    DEB_ARCH="arm64"
    MULTIARCH_TRIPLET="aarch64-linux-gnu"
    BAZEL_ARGS=(--config=jetson --action_env=JETSON_SYSROOT="$JETSON_SYSROOT" --define=JETSON_SYSROOT="$JETSON_SYSROOT")
    ;;
  *)
    fail "unsupported target: $TARGET"
    ;;
esac

command -v "$BAZEL" >/dev/null 2>&1 || fail "bazel launcher not found: $BAZEL"
command -v dpkg-deb >/dev/null 2>&1 || fail "dpkg-deb is required"
command -v git >/dev/null 2>&1 || fail "git is required"

mkdir -p "$DIST_DIR"

"$BAZEL" build "${BAZEL_ARGS[@]}" //...
BAZEL_BIN="$("$BAZEL" info "${BAZEL_ARGS[@]}" bazel-bin)"
[ -d "$BAZEL_BIN/src" ] || fail "expected Bazel output tree under $BAZEL_BIN/src"

if [ -z "$VERSION" ]; then
  GIT_SHA="$(git rev-parse --short=12 HEAD)"
  GIT_DATE="$(git show -s --format=%cd --date=format:%Y%m%d%H%M HEAD)"
  DIRTY_SUFFIX=""
  if ! git diff --quiet --ignore-submodules=dirty HEAD --; then
    DIRTY_SUFFIX="+dirty"
  fi
  VERSION="0.0~git${GIT_DATE}.${GIT_SHA}${DIRTY_SUFFIX}"
fi

MAINTAINER_NAME="$(git config --get user.name || true)"
MAINTAINER_EMAIL="$(git config --get user.email || true)"
if [ -z "$MAINTAINER_NAME" ]; then
  MAINTAINER_NAME="Hstream Packaging"
fi
if [ -z "$MAINTAINER_EMAIL" ]; then
  MAINTAINER_EMAIL="packages@localhost"
fi

WORK_DIR="$(mktemp -d "$DIST_DIR/.${PACKAGE_NAME}.${DEB_ARCH}.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

PKGROOT="$WORK_DIR/pkgroot"
DEBIAN_DIR="$PKGROOT/DEBIAN"
ARCH_ROOT="$PKGROOT/usr/lib/$MULTIARCH_TRIPLET/$PACKAGE_NAME"
BIN_DIR="$ARCH_ROOT/bin"
LIB_DIR="$ARCH_ROOT/lib"
GST_DIR="$ARCH_ROOT/gst-plugins"
SHARE_DIR="$PKGROOT/usr/share/$PACKAGE_NAME"
DOC_DIR="$PKGROOT/usr/share/doc/$PACKAGE_NAME"
CONFIG_DIR="$SHARE_DIR/configs"
MANIFEST_FILE="$DIST_DIR/${PACKAGE_NAME}_${VERSION}_${DEB_ARCH}.files.txt"
OUTPUT_DEB="$DIST_DIR/${PACKAGE_NAME}_${VERSION}_${DEB_ARCH}.deb"

mkdir -p "$DEBIAN_DIR" "$BIN_DIR" "$LIB_DIR" "$GST_DIR" "$CONFIG_DIR" "$DOC_DIR"
cp -a configs/. "$CONFIG_DIR/"
find "$CONFIG_DIR" -type d -exec chmod 0755 {} +
find "$CONFIG_DIR" -type f -exec chmod 0644 {} +
install -m 0644 README.md "$DOC_DIR/README.md"
install -m 0644 LICENSE.md "$DOC_DIR/copyright"

cat > "$SHARE_DIR/setup-env.sh" <<'EOF'
#!/usr/bin/env bash

set -euo pipefail

case "$(uname -m)" in
  x86_64)
    hstream_triplet="x86_64-linux-gnu"
    ;;
  aarch64|arm64)
    hstream_triplet="aarch64-linux-gnu"
    ;;
  *)
    echo "Unsupported architecture for hstream runtime: $(uname -m)" >&2
    return 1 2>/dev/null || exit 1
    ;;
esac

prepend_path() {
  local var="$1"
  local entry="$2"
  local current="${!var:-}"
  case ":$current:" in
    *":$entry:"*)
      ;;
    *)
      if [ -n "$current" ]; then
        export "$var=$entry:$current"
      else
        export "$var=$entry"
      fi
      ;;
  esac
}

export HSTREAM_ROOT="/usr/lib/${hstream_triplet}/hstream"
prepend_path PATH "$HSTREAM_ROOT/bin"
prepend_path LD_LIBRARY_PATH "$HSTREAM_ROOT/lib"
prepend_path GST_PLUGIN_PATH "$HSTREAM_ROOT/gst-plugins"
prepend_path GST_PLUGIN_PATH_1_0 "$HSTREAM_ROOT/gst-plugins"
EOF
chmod 0755 "$SHARE_DIR/setup-env.sh"

cat > "$DOC_DIR/README.Debian" <<'EOF'
This package installs architecture-specific hstream binaries, shared libraries,
and GStreamer plugins under:

  /usr/lib/<multiarch>/hstream/

Shared configs are installed under:

  /usr/share/hstream/configs

To use the packaged binaries and plugins, source:

  /usr/share/hstream/setup-env.sh
EOF
chmod 0644 "$DOC_DIR/README.Debian"

declare -A INSTALLED_DESTS=()
PACKAGED_COUNT=0
TMP_MANIFEST="$WORK_DIR/packaged-files.txt"
: > "$TMP_MANIFEST"

install_runtime_file() {
  local src="$1"
  local dest="$2"
  if [[ -n "${INSTALLED_DESTS[$dest]:-}" ]]; then
    fail "duplicate packaged destination: $dest (from $src and ${INSTALLED_DESTS[$dest]})"
  fi
  INSTALLED_DESTS["$dest"]="$src"
  install -m 0755 "$src" "$dest"
  echo "${dest#$PKGROOT/}" >> "$TMP_MANIFEST"
  PACKAGED_COUNT=$((PACKAGED_COUNT + 1))
}

while IFS= read -r -d '' SRC; do
  REL="${SRC#$BAZEL_BIN/}"
  BASE="${REL##*/}"

  case "$REL" in
    *.params|*.a|*.pic.a|*.o|*.d|*.runfiles_manifest|*/MANIFEST|*.manifest|*runfiles/*)
      continue
      ;;
  esac

  case "$REL" in
    *_test|*/test/*)
      continue
      ;;
  esac

  DEST=""
  case "$BASE" in
    *.so)
      case "$REL" in
        src/gst-plugins/*/libnvdsgst_*.so|src/gst-plugins/*/libgst*.so)
          DEST="$GST_DIR/$BASE"
          ;;
        *)
          DEST="$LIB_DIR/$BASE"
          ;;
      esac
      ;;
    *)
      if [ -x "$SRC" ] && [[ "$BASE" != lib* ]]; then
        DEST="$BIN_DIR/$BASE"
      fi
      ;;
  esac

  if [ -n "$DEST" ]; then
    install_runtime_file "$SRC" "$DEST"
  fi
done < <(find "$BAZEL_BIN/src" -type f -print0 | sort -z)

[ "$PACKAGED_COUNT" -gt 0 ] || fail "no runtime files were selected from $BAZEL_BIN/src"

{
  cat "$TMP_MANIFEST"
  echo "usr/share/$PACKAGE_NAME/setup-env.sh"
  echo "usr/share/doc/$PACKAGE_NAME/README.md"
  echo "usr/share/doc/$PACKAGE_NAME/README.Debian"
  echo "usr/share/doc/$PACKAGE_NAME/copyright"
  find "$CONFIG_DIR" -type f | sed "s#^$PKGROOT/##"
} | sort > "$MANIFEST_FILE"

INSTALLED_SIZE="$(du -sk "$PKGROOT" | awk '{print $1}')"
cat > "$DEBIAN_DIR/control" <<EOF
Package: $PACKAGE_NAME
Version: $VERSION
Section: misc
Priority: optional
Architecture: $DEB_ARCH
Multi-Arch: same
Maintainer: $MAINTAINER_NAME <$MAINTAINER_EMAIL>
Installed-Size: $INSTALLED_SIZE
Description: Hstream runtime applications, libraries, and DeepStream plugins
 GPU-accelerated hstream applications, support libraries, and GStreamer plugins
 staged under a multiarch-friendly Debian filesystem layout.
EOF

(cd "$PKGROOT" && find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums)
find "$PKGROOT" -type d -exec chmod 0755 {} +
rm -f "$OUTPUT_DEB"
dpkg-deb --root-owner-group --build "$PKGROOT" "$OUTPUT_DEB" >/dev/null

echo "Built $OUTPUT_DEB"
echo "Manifest written to $MANIFEST_FILE"
