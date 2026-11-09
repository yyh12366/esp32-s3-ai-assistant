#!/usr/bin/env bash
# 发布固件 OTA 到 la 的媒体目录：构建（可选）→ 计算清单 → 上传 firmware.bin → 最后替换 version.json。
#
# 用法：tools/publish_ota.sh [--no-build]
#   默认先执行 firmware/tools/build_firmware.sh allinone（需要已激活 ESP-IDF 5.5.3）。
#   --no-build 直接发布 firmware/build/allinone 里已有的 FoloToy-AI-Passport.bin。
#
# 版本标识 = esp_app_desc 的 version + " " + date + " " + time，与固件 av_ota_current_version() 一致。
# 远端路径 /opt/astrbot-relay/media/ota 由 la 上的 relay 容器只读挂载为 /app/media/ota。
set -euo pipefail

usage() {
    echo "Usage: $0 [--no-build]" >&2
}

build=1
case "${1:-}" in
    "") ;;
    --no-build) build=0 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
esac

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
remote="${OTA_REMOTE:-la}"
remote_dir="${OTA_REMOTE_DIR:-/opt/astrbot-relay/media/ota}"
app_bin="${repo_root}/firmware/build/allinone/FoloToy-AI-Passport.bin"

if [[ "${build}" == "1" ]]; then
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: activate ESP-IDF 5.5.3 first (source ~/esp/esp-idf-v5.5.3/export.sh), or pass --no-build." >&2
        exit 1
    fi
    "${repo_root}/firmware/tools/build_firmware.sh" allinone
fi

if [[ ! -f "${app_bin}" ]]; then
    echo "ERROR: ${app_bin} is missing; build allinone first." >&2
    exit 1
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/publish-ota.XXXXXX")"
trap 'rm -rf -- "${work}"' EXIT

# 从镜像的 esp_app_desc_t（偏移 32 起）读取版本标识，生成 version.json。
# 布局：magic(4) secure_version(4) reserv1(8) version[32] project_name[32] time[16] date[16]
PYTHONDONTWRITEBYTECODE=1 python3 -I - "${app_bin}" "${work}/version.json" <<'PY'
import hashlib
import json
import os
import struct
import sys

path, out = sys.argv[1], sys.argv[2]
raw = open(path, "rb").read()
magic, = struct.unpack_from("<I", raw, 32)
if magic != 0xABCD5432:
    raise SystemExit("ERROR: not an ESP application image (bad app descriptor magic)")


def field(offset: int, size: int) -> str:
    return raw[offset:offset + size].split(b"\0", 1)[0].decode("ascii")


version = field(48, 32)
time_text = field(112, 16)
date_text = field(128, 16)
identity = f"{version} {date_text} {time_text}"
manifest = {
    "version": identity,
    "size": os.path.getsize(path),
    "sha256": hashlib.sha256(raw).hexdigest(),
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(manifest, handle)
print(identity)
PY
manifest="${work}/version.json"
identity="$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["version"])' "${manifest}")"
size="$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["size"])' "${manifest}")"
sha="$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["sha256"])' "${manifest}")"

ssh -o BatchMode=yes "${remote}" "mkdir -p '${remote_dir}' && chmod 0755 '${remote_dir}'"

# 先传固件（临时名 + mv 原子替换），最后替换清单：设备只会在清单指向的固件就绪后才看到新版本。
scp -q -o BatchMode=yes "${app_bin}" "${remote}:${remote_dir}/firmware.bin.tmp"
ssh -o BatchMode=yes "${remote}" \
    "chmod 0644 '${remote_dir}/firmware.bin.tmp' && mv -f '${remote_dir}/firmware.bin.tmp' '${remote_dir}/firmware.bin'"
scp -q -o BatchMode=yes "${manifest}" "${remote}:${remote_dir}/version.json.tmp"
ssh -o BatchMode=yes "${remote}" \
    "chmod 0644 '${remote_dir}/version.json.tmp' && mv -f '${remote_dir}/version.json.tmp' '${remote_dir}/version.json'"

# 远端回读校验：大小与 sha256 必须和本地一致
remote_line="$(ssh -o BatchMode=yes "${remote}" \
    "stat -c %s '${remote_dir}/firmware.bin' && sha256sum '${remote_dir}/firmware.bin' | cut -d' ' -f1")"
remote_size="$(printf '%s\n' "${remote_line}" | sed -n 1p)"
remote_sha="$(printf '%s\n' "${remote_line}" | sed -n 2p)"
if [[ "${remote_size}" != "${size}" || "${remote_sha}" != "${sha}" ]]; then
    echo "ERROR: remote firmware.bin does not match the local image (size ${remote_size} vs ${size})." >&2
    exit 1
fi

echo "Published OTA version: ${identity}"
echo "  size: ${size} bytes"
echo "  sha256: ${sha}"
echo "  remote: ${remote}:${remote_dir}/{firmware.bin,version.json}"
