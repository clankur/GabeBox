#!/usr/bin/env bash
# Build and sign an Xbox test package. The development key stays in a temporary directory.
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
umask 077

if (( $# == 3 )) && [[ $1 == --layout ]]; then
    layout=$(realpath "$2")
    output=$3
elif (( $# == 2 )); then
    library=$(realpath "$1")
    output=$2
    bash "$src/build.sh" "$library" "$work/build"
    layout=$work/build/package
else
    echo 'Usage: package.sh /path/to/x64/WindowsApp.lib NEW_OUTPUT_DIR' >&2
    echo '       package.sh --layout APPX_LAYOUT NEW_OUTPUT_DIR' >&2
    exit 2
fi

name=PackagedProcessProbe
if [[ -f "$layout/big-picture.html" && -f "$layout/BigPicture.Native.dll" ]]; then
    name=SteamBigPicture
fi

"${MAKEMSIX:-makemsix}" pack -d "$layout" -p "$work/unsigned.appx"
openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 365 \
    -subj '/CN=Xbox Portability Probe' -addext 'basicConstraints=critical,CA:FALSE' \
    -addext 'keyUsage=critical,digitalSignature' -addext 'extendedKeyUsage=codeSigning' \
    -keyout "$work/dev-key.pem" -out "$work/dev-cert.pem"
"${OSSLSIGNCODE:-osslsigncode}" sign -certs "$work/dev-cert.pem" -key "$work/dev-key.pem" \
    -in "$work/unsigned.appx" -out "$work/$name-x64.appx"
"${OSSLSIGNCODE:-osslsigncode}" verify -CAfile "$work/dev-cert.pem" \
    -in "$work/$name-x64.appx"
openssl x509 -in "$work/dev-cert.pem" -outform DER -out "$work/$name.cer"

mkdir "$output"
cp "$work/$name-x64.appx" "$work/$name.cer" "$output/"
cd "$output"
shasum -a 256 "$name-x64.appx" "$name.cer" > SHA256SUMS
