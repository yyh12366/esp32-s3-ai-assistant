#!/usr/bin/env bash
set -euo pipefail

variant="${1:-kk_oled}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

target=esp32c3
app_variant="${variant}"
flash_mb=8
case "${variant}" in
    kk_oled) defaults="${repo_root}/sdkconfig.defaults" ;;
    astrbot_voice) defaults="${repo_root}/sdkconfig.astrbot-voice.defaults" ;;
    allinone) defaults="${repo_root}/sdkconfig.astrbot-voice.defaults" ;;
    esp32s3)
        defaults="${repo_root}/sdkconfig.astrbot-voice.defaults;${repo_root}/sdkconfig.esp32s3.defaults"
        target=esp32s3
        app_variant=allinone
        flash_mb=16
        ;;
    *)
        echo "Usage: $0 [allinone|kk_oled|astrbot_voice|esp32s3]" >&2
        exit 2
        ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: activate ESP-IDF 5.5.3 first." >&2
    exit 1
fi

build_dir="${repo_root}/build/${variant}"
SDKCONFIG_DEFAULTS="${defaults}" idf.py -B "${build_dir}" \
    -D "SDKCONFIG=${build_dir}/sdkconfig" -D "IDF_TARGET=${target}" -D "APP_VARIANT=${app_variant}" build
idf.py -B "${build_dir}" merge-bin \
    -o "${build_dir}/FoloToy-AI-Passport-full.bin"
python3 "${repo_root}/tools/verify_firmware.py" "${build_dir}" --flash-mb "${flash_mb}"
echo "Firmware build: ${build_dir}/FoloToy-AI-Passport-full.bin"
