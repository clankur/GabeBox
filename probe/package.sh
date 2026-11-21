#!/usr/bin/env bash
# Build and sign an Xbox test package. The development key stays in a temporary directory.
set -euo pipefail
if (( $# != 2 )); then
    echo 'Usage: package.sh /path/to/x64/WindowsApp.lib NEW_OUTPUT_DIR' >&2
    exit 2
fi

src=$(cd "$(dirname "$0")" && pwd)
library=$(realpath "$1")
output=$2
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
umask 077

bash "$src/build.sh" "$library" "$work/build"
"${MAKEMSIX:-makemsix}" pack -d "$work/build/package" -p "$work/unsigned.appx"
openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 365 \
    -subj '/CN=Xbox Portability Probe' -addext 'basicConstraints=critical,CA:FALSE' \
    -addext 'keyUsage=critical,digitalSignature' -addext 'extendedKeyUsage=codeSigning' \
    -keyout "$work/dev-key.pem" -out "$work/dev-cert.pem"
"${OSSLSIGNCODE:-osslsigncode}" sign -certs "$work/dev-cert.pem" -key "$work/dev-key.pem" \
    -in "$work/unsigned.appx" -out "$work/PackagedProcessProbe-x64.appx"
"${OSSLSIGNCODE:-osslsigncode}" verify -CAfile "$work/dev-cert.pem" \
    -in "$work/PackagedProcessProbe-x64.appx"
openssl x509 -in "$work/dev-cert.pem" -outform DER -out "$work/PackagedProcessProbe.cer"

mkdir "$output"
cp "$work/PackagedProcessProbe-x64.appx" "$work/PackagedProcessProbe.cer" "$output/"
cd "$output"
shasum -a 256 PackagedProcessProbe-x64.appx PackagedProcessProbe.cer > SHA256SUMS
