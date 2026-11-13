#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p components .tools
# Download URL published by AgoraIO-Conversational-AI/esp32-client.
curl --fail --location --proto '=https' \
  https://rte-store.s3.amazonaws.com/agora_iot_sdk.tar \
  --output .tools/agora_iot_sdk.tar
mkdir -p components/agora_iot_sdk
tar -xf .tools/agora_iot_sdk.tar -C components/agora_iot_sdk
test -f components/agora_iot_sdk/include/agora_rtc_api.h
echo 'Agora SDK extracted. Review its license before redistribution.'
