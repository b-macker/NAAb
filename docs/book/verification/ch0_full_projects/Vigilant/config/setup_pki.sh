#!/bin/bash
# Vigilant/config/setup_pki.sh
# Generates a Mutual TLS (mTLS) Chain of Trust

DIR="verification/ch0_full_projects/Vigilant/config"
mkdir -p $DIR
rm -f $DIR/*.pem $DIR/*.srl

# 1. Create the TRUSTED ROOT CA
echo "[PKI] Generating Root CA..."
openssl req -x509 -newkey rsa:4096 -keyout $DIR/ca_key.pem -out $DIR/ca_cert.pem -days 365 -nodes -subj "/C=US/O=Vigilant Sovereign/CN=Vigilant Root CA"

# 2. Create the SERVER Cert (The Gateway)
echo "[PKI] Generating Server Cert..."
openssl req -newkey rsa:4096 -keyout $DIR/server_key.pem -out $DIR/server.csr -nodes -subj "/C=US/O=Vigilant/CN=localhost"
openssl x509 -req -in $DIR/server.csr -CA $DIR/ca_cert.pem -CAkey $DIR/ca_key.pem -CAcreateserial -out $DIR/server_cert.pem -days 365

# 3. Create the CLIENT Cert (The Authorized App)
echo "[PKI] Generating Client Cert..."
openssl req -newkey rsa:4096 -keyout $DIR/client_key.pem -out $DIR/client.csr -nodes -subj "/C=US/O=Vigilant/CN=AuthorizedClient"
openssl x509 -req -in $DIR/client.csr -CA $DIR/ca_cert.pem -CAkey $DIR/ca_key.pem -out $DIR/client_cert.pem -days 365

echo "[PKI] mTLS Infrastructure Ready."
