#!/usr/bin/env bash
set -euo pipefail

listing_script=$1
test_root=$(mktemp -d "${TEST_TMPDIR:-/tmp}/cuda-payload-test.XXXXXX")
trap 'rm -rf "${test_root}"' EXIT
inspector=${test_root}/cuobjdump
cat > "${inspector}" <<'INSPECTOR'
#!/usr/bin/env bash
mode=$1
elf=$2
case "${elf}" in
  host-only)
    echo "cuobjdump info    : File '${elf}' does not contain device code" >&2
    ;;
  host-only-fatal)
    echo "cuobjdump fatal   : File '${elf}' does not contain device code" >&2
    exit 255
    ;;
  ptx-only)
    if [[ ${mode} == --list-elf ]]; then
      echo "cuobjdump info    : No ELF file found to extract from '${elf}'. You may try with -all option." >&2
    else
      echo 'PTX file    1: ptx-only.1.sm_87.ptx'
    fi
    ;;
  native)
    echo 'ELF file    1: native.1.sm_87.cubin'
    ;;
  missing)
    echo "cuobjdump fatal   : Could not open input file '${elf}'" >&2
    exit 255
    ;;
  broken)
    echo "cuobjdump fatal   : Invalid ELF file '${elf}'" >&2
    exit 1
    ;;
  *) exit 2 ;;
esac
INSPECTOR
chmod +x "${inspector}"

for mode in --list-elf --list-ptx; do
  [[ -z $("${listing_script}" "${inspector}" "${mode}" host-only) ]]
  [[ -z $("${listing_script}" "${inspector}" "${mode}" host-only-fatal) ]]
  for bad in missing broken; do
    if "${listing_script}" "${inspector}" "${mode}" "${bad}" >"${test_root}/out" 2>"${test_root}/err"; then
      echo "Inspector failure was hidden: ${bad} ${mode}" >&2
      exit 1
    fi
    [[ -s ${test_root}/err ]]
  done
done
[[ -z $("${listing_script}" "${inspector}" --list-elf ptx-only) ]]
[[ $("${listing_script}" "${inspector}" --list-ptx ptx-only) == 'PTX file    1: ptx-only.1.sm_87.ptx' ]]
[[ $("${listing_script}" "${inspector}" --list-elf native) == 'ELF file    1: native.1.sm_87.cubin' ]]
echo 'CUDA payload listing handles host-only, PTX-only, native and failed inspections'
