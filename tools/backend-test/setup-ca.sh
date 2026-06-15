#!/bin/bash
# setup-ca.sh — Generate CA and broker certificates for MQTT TLS
# Run once on the backend server before deploying the MCU provisioning flow.
#
# Output:
#   data/ca/ca-cert.pem     CA root certificate (paste this into MCU web UI)
#   data/ca/ca-key.pem      CA private key (keep secret, do NOT share)
#   data/ca/broker-cert.pem Broker server certificate (for mosquitto/aedes)
#   data/ca/broker-key.pem  Broker private key
#
# MCU embeds ca-cert.pem to verify the broker on every MQTT TLS connect.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CA_DIR="${SCRIPT_DIR}/data/ca"
CA_CERT="${CA_DIR}/ca-cert.pem"
CA_KEY="${CA_DIR}/ca-key.pem"
BROKER_CERT="${CA_DIR}/broker-cert.pem"
BROKER_KEY="${CA_DIR}/broker-key.pem"
BROKER_CSR="${CA_DIR}/broker.csr"
OPENSSL_CNF="${CA_DIR}/openssl.cnf"

mkdir -p "${CA_DIR}"

# ── OpenSSL config for the CA ────────────────────────────────────────
cat > "${OPENSSL_CNF}" << 'EOF'
[req]
default_bits       = 2048
prompt             = no
distinguished_name = dn
x509_extensions    = v3_ca

[dn]
C  = TW
O  = autoControl
CN = autoControl Cluster CA

[v3_ca]
basicConstraints       = critical, CA:TRUE
keyUsage               = critical, keyCertSign, cRLSign
subjectKeyIdentifier   = hash

[v3_broker]
basicConstraints       = critical, CA:FALSE
keyUsage               = critical, digitalSignature, keyEncipherment
extendedKeyUsage       = serverAuth
subjectAltName         = @alt_names

[alt_names]
DNS.1 = localhost
IP.1  = 127.0.0.1
EOF

echo "[setup-ca] Generating CA root certificate…"

# ── CA root key + self-signed certificate (valid 10 years) ───────────
openssl genrsa -out "${CA_KEY}" 2048 2>/dev/null
openssl req -x509 -new -nodes \
  -key "${CA_KEY}" \
  -sha256 -days 3650 \
  -out "${CA_CERT}" \
  -config "${OPENSSL_CNF}" \
  -extensions v3_ca 2>/dev/null

echo "[setup-ca] CA certificate: ${CA_CERT}"
echo "[setup-ca] CA fingerprint (SHA-256):"
openssl x509 -in "${CA_CERT}" -noout -fingerprint -sha256 | sed 's/.*=//'

echo ""
echo "[setup-ca] Generating broker server certificate…"

# ── Broker key + CSR ─────────────────────────────────────────────────
openssl genrsa -out "${BROKER_KEY}" 2048 2>/dev/null
openssl req -new \
  -key "${BROKER_KEY}" \
  -out "${BROKER_CSR}" \
  -subj "/C=TW/O=autoControl/CN=autocontrol-mqtt-broker" 2>/dev/null

# ── CA signs broker cert (valid 2 years) ─────────────────────────────
openssl x509 -req \
  -in "${BROKER_CSR}" \
  -CA "${CA_CERT}" -CAkey "${CA_KEY}" \
  -CAcreateserial \
  -out "${BROKER_CERT}" \
  -days 730 -sha256 \
  -extfile "${OPENSSL_CNF}" \
  -extensions v3_broker 2>/dev/null

rm -f "${BROKER_CSR}" "${OPENSSL_CNF}"

echo "[setup-ca] Broker certificate: ${BROKER_CERT}"
echo "[setup-ca] Broker key: ${BROKER_KEY}"
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  CA cert for MCU (copy this into the MCU web settings page):"
echo "═══════════════════════════════════════════════════════════════"
cat "${CA_CERT}"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "  Mosquitto config snippet (add to mosquitto.conf):"
echo "═══════════════════════════════════════════════════════════════"
echo "listener 8883"
echo "cafile   ${CA_CERT}"
echo "certfile ${BROKER_CERT}"
echo "keyfile  ${BROKER_KEY}"
echo "require_certificate false"
echo "tls_version tlsv1.2"
echo "═══════════════════════════════════════════════════════════════"
