#!/usr/bin/env bash
# Download and statically inspect Valve's official Windows Steam bootstrapper.
# The downloaded PE is held only in a temporary directory and deleted on exit.
set -euo pipefail

if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [output-dir]" >&2
  exit 2
fi
out=${1:-"$PWD/steamsetup-audit"}
mkdir -p -- "$out"
out=$(realpath -- "$out")
work=$(mktemp -d "${TMPDIR:-/tmp}/steamsetup-audit.XXXXXX")
trap 'rm -rf -- "$work"' EXIT

rea_bin=${REA_BIN:-$(command -v rea || true)}
if [[ -z "$rea_bin" ]]; then
  echo 'Set REA_BIN or put rea on PATH.' >&2
  exit 127
fi
rea() {
  if [[ "$rea_bin" == *.mjs ]]; then
    node "$rea_bin" "$@"
  else
    "$rea_bin" "$@"
  fi
}
curl --fail --location --silent --show-error \
  https://cdn.akamai.steamstatic.com/client/installer/SteamSetup.exe \
  -o "$work/SteamSetup.exe"
printf '%s  SteamSetup.exe\n' "$(sha256sum -- "$work/SteamSetup.exe" | awk '{print $1}')" \
  > "$out/steamsetup.sha256"
rea inspect-pe-imports "$work/SteamSetup.exe" --json \
  > "$out/steamsetup-imports.json"
rea inspect-pe-certificates "$work/SteamSetup.exe" --json \
  > "$out/steamsetup-certificates.json"
rea inspect-artifact "$work/SteamSetup.exe" --json \
  > "$out/steamsetup-artifact.json"
echo "Evidence written to $out; downloaded PE deleted from $work on exit."
