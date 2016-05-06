#!/bin/sh
# Generates a self-signed certificate for Dracon's HTTPS port.
# The certificate is valid for localhost only and is meant for testing.

openssl req -x509 -newkey rsa:2048 -noenc \
    -keyout server.key -out server.crt -days 3650 \
    -subj "/O=Dracon/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,DNS:localhost.localdomain,IP:127.0.0.1,IP:::1"
