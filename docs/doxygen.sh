#!/usr/bin/env bash
set -euo pipefail
# If doxygen is not installed, generate a minimal stub so Bazel builds succeed.
if ! command -v doxygen >/dev/null 2>&1; then
  cfg="${1:-}"
  if [[ -z "$cfg" || ! -f "$cfg" ]]; then
    echo "doxygen.sh: missing Doxyfile path and doxygen not installed" >&2
    exit 127
  fi
  out_dir=$(grep -E '^OUTPUT_DIRECTORY[[:space:]]*=' "$cfg" | sed -E 's/^[^=]+=\s*//')
  html_dir=$(grep -E '^HTML_OUTPUT[[:space:]]*=' "$cfg" | sed -E 's/^[^=]+=\s*//')
  [[ -z "$out_dir" ]] && { echo "doxygen.sh: could not infer OUTPUT_DIRECTORY" >&2; exit 2; }
  [[ -z "$html_dir" ]] && html_dir=html
  mkdir -p "$out_dir/$html_dir"
  cat >"$out_dir/$html_dir/index.html" <<'EOF'
<!doctype html><html><head><meta charset="utf-8"><title>Docs unavailable</title></head>
<body><h1>Documentation not built</h1><p>Doxygen is not installed on this machine. Install doxygen and rebuild.</p></body></html>
EOF
  exit 0
fi
exec doxygen "$@"
