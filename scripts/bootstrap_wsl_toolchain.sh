#!/usr/bin/env bash
# Install a user-local C++ toolchain (no root) so the project can be built in WSL.
set -euo pipefail
PREFIX="${HOME}/.local/pp-toolchain"
BIN="${PREFIX}/bin"
mkdir -p "${BIN}" "${PREFIX}/tmp"
export TMPDIR="${PREFIX}/tmp"

if [[ -x "${BIN}/g++" && -x "${BIN}/cmake" && -x "${BIN}/make" ]]; then
  echo "Toolchain already present at ${PREFIX}"
  "${BIN}/g++" --version | head -1
  "${BIN}/cmake" --version | head -1
  exit 0
fi

echo "Fetching micromamba..."
curl -fsSL https://micro.mamba.pm/api/micromamba/linux-64/latest -o "${PREFIX}/tmp/micromamba.tar.bz2"
tar -xjf "${PREFIX}/tmp/micromamba.tar.bz2" -C "${PREFIX}/tmp"
MM="${PREFIX}/tmp/bin/micromamba"
chmod +x "${MM}"

echo "Installing compilers + cmake into ${PREFIX}..."
"${MM}" create -y -p "${PREFIX}" -c conda-forge \
  "cxx-compiler" "cmake" "make" "ninja" "binutils"

echo "Done."
"${BIN}/g++" --version | head -1
"${BIN}/cmake" --version | head -1
