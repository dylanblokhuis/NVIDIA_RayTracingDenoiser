#!/usr/bin/env bash
# Usage: gen_metal_shaders.sh <spirv-cross-executable> <shader-output-dir> <SpirvToMetallib-executable>
# Avoid `pipefail` / `shopt` so this works when invoked as `sh` or bash --posix (CMake/Make sometimes do).
set -eu
SPIRV_CROSS="$1"
NRD_SHADERS_PATH="$2"
TOOL="$3"
for f in "${NRD_SHADERS_PATH}"/*.spirv.h; do
  [ -e "$f" ] || continue
  m="${f%.spirv.h}.metal.h"
  echo "NRD: SpirvToMetallib: ${f} -> ${m}"
  "${TOOL}" "${SPIRV_CROSS}" "${f}" "${m}"
done
