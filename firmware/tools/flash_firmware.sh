#!/usr/bin/env bash
# 烧录合并固件（默认 allinone）。
# 用法：./tools/flash_firmware.sh [allinone|kk_oled|astrbot_voice] [port]
set -euo pipefail

variant="${1:-allinone}"
port="${2:-}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
image="${repo_root}/build/${variant}/FoloToy-AI-Passport-full.bin"

if [[ ! -f "${image}" ]]; then
    echo "ERROR: ${image} 不存在，先运行 ./tools/build_firmware.sh ${variant}" >&2
    exit 1
fi

if [[ -z "${port}" ]]; then
    # macOS 上 ESP32-C3 原生 USB 通常是 cu.usbmodem*，CP210x 是 cu.SLAB_USBtoUART*
    for candidate in /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* /dev/ttyACM*; do
        [[ -e "${candidate}" ]] && port="${candidate}" && break
    done
fi
if [[ -z "${port}" ]]; then
    echo "ERROR: 找不到串口，插上设备或显式传入端口，例如 $0 ${variant} /dev/cu.usbmodem101" >&2
    exit 1
fi

esptool="esptool.py"
command -v esptool.py >/dev/null 2>&1 || esptool="esptool"
if ! command -v "${esptool}" >/dev/null 2>&1; then
    echo "ERROR: 先 source ESP-IDF export.sh（需要 esptool）" >&2
    exit 1
fi

# 合并镜像从 0x0 写入：bootloader + 分区表 + app 一次完成。
"${esptool}" -p "${port}" -b 460800 write_flash 0x0 "${image}"
echo "Flashed ${variant} -> ${port}"
echo "Image: ${image}"
