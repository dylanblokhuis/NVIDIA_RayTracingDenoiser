#!/usr/bin/env bash
# Usage: gen_metal_shaders.sh <spirv-cross-executable> <shader-output-dir> <SpirvToMetallib-executable>
set -euo pipefail
SPIRV_CROSS="$1"
NRD_SHADERS_PATH="$2"
TOOL="$3"
shopt -s nullglob
for f in "${NRD_SHADERS_PATH}"/*.spirv.h; do
  m="${f%.spirv.h}.metal.h"
  echo "NRD: SpirvToMetallib: ${f} -> ${m}"
  "${TOOL}" "${SPIRV_CROSS}" "${f}" "${m}"
done
