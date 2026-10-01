#!/usr/bin/env bash
# Generates a self-signed TLS certificate + private key for the HD-WF2's
# HTTPS REST API (see main/Kconfig.projbuild's "HD-WF1 HTTPS API" menu).
#
# ECDSA (prime256v1), not RSA: a much lighter TLS handshake on an embedded
# server - RSA-2048 handshakes are both slower and more memory-hungry on
# this hardware, and the ESP32-S3's extra heap (vs. the HD-WF1) is the
# whole reason this feature exists in the first place, not something to
# spend carelessly.
#
# Not committed to git (certs/*.pem is gitignored, like sdkconfig) - run
# this once per physical board, same spirit as that board's own WiFi
# password: a leaked key should compromise one plaque, not the fleet.
#
# Usage: tools/gen_https_cert.sh [days] [common-name]
#   days: certificate validity, default 3650 (10 years - this is a LAN
#         device with no revocation/renewal infrastructure, so it's signed
#         to outlive the hardware rather than need periodic reissuing).
#   common-name: default "digital-address-plaque" - cosmetic only, since
#         clients pin the certificate itself (see README), not a hostname.
set -euo pipefail
cd "$(dirname "$0")/.."

DAYS="${1:-3650}"
CN="${2:-digital-address-plaque}"
OUT_DIR="certs"
CERT="$OUT_DIR/server_cert.pem"
KEY="$OUT_DIR/server_key.pem"

mkdir -p "$OUT_DIR"

if [[ -e "$CERT" || -e "$KEY" ]]; then
  echo "Refusing to overwrite existing $CERT / $KEY - delete them first if you really want a new cert/key." >&2
  echo "(Replacing a board's cert invalidates every client that already pinned the old one's fingerprint.)" >&2
  exit 1
fi

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -nodes -keyout "$KEY" -out "$CERT" -days "$DAYS" \
  -subj "/CN=$CN" -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"

chmod 600 "$KEY"

echo
echo "Wrote $CERT and $KEY (valid $DAYS days)."
echo "SHA-256 fingerprint (should match this board's /api/whoami 'tls_fingerprint' once flashed):"
openssl x509 -in "$CERT" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d ':' | tr 'A-F' 'a-f'
