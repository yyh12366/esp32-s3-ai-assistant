#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local dead_strip_flag

    # Apple ld does not accept --gc-sections; -dead_strip is the equivalent.
    dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    # AstrBot voice application: protocol, WebSocket, state machine, text, ring buffer.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_av_ws.c main/av_ws.c main/av_url.c \
        -o "${test_dir}/test_av_ws"
    "${test_dir}/test_av_ws"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_av_proto.c main/av_proto.c main/av_text.c main/av_ring.c \
        -o "${test_dir}/test_av_proto"
    "${test_dir}/test_av_proto"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_av_adpcm.c main/av_adpcm.c \
        -o "${test_dir}/test_av_adpcm"
    "${test_dir}/test_av_adpcm"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_av_ota_manifest.c main/av_ota_manifest.c \
        -o "${test_dir}/test_av_ota_manifest"
    "${test_dir}/test_av_ota_manifest"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_av_session.c main/av_session.c main/av_ipedit.c main/av_anim.c main/av_url.c \
        -lm -o "${test_dir}/test_av_session"
    "${test_dir}/test_av_session"
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_av_font.py

    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_kk_core.c main/kk_core.c main/kk_anim.c \
        -o "${test_dir}/test_kk_core"
    "${test_dir}/test_kk_core"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_kk_fb.c main/kk_fb.c -o "${test_dir}/test_kk_fb"
    "${test_dir}/test_kk_fb"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_kk_render.c main/kk_render.c main/kk_render_home.c \
        main/kk_render_pages.c main/kk_render_modal.c main/kk_render_settings.c \
        main/kk_render_targets.c main/kk_render_texts.c main/kk_icons.c \
        main/kk_font.c main/kk_fb.c main/kk_anim.c main/kk_core.c \
        -o "${test_dir}/test_kk_render"
    "${test_dir}/test_kk_render"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/kk_store_stubs -Imain tests/test_kk_store.c main/kk_store.c \
        -o "${test_dir}/test_kk_store"
    "${test_dir}/test_kk_store"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_mono.c components/bsp/src/bsp_display_mono_math.c \
        -o "${test_dir}/test_bsp_display_mono"
    "${test_dir}/test_bsp_display_mono"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/battery_stubs -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_battery.c -o "${test_dir}/test_bsp_battery"
    "${test_dir}/test_bsp_battery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_profile.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_root
    local variant
    local defaults
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_root="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_root}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_root}" ;; esac' EXIT

    for variant in allinone kk_oled astrbot_voice; do
        case "${variant}" in
            kk_oled) defaults="${repo_root}/sdkconfig.defaults" ;;
            allinone|astrbot_voice) defaults="${repo_root}/sdkconfig.astrbot-voice.defaults" ;;
        esac
        validation_build_dir="${validation_root}/${variant}"
        SDKCONFIG_DEFAULTS="${defaults}" \
            idf.py -B "${validation_build_dir}" \
            -D "SDKCONFIG=${validation_build_dir}/sdkconfig" \
            -D "APP_VARIANT=${variant}" build
        idf.py -B "${validation_build_dir}" merge-bin \
            -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
        python3 tools/verify_firmware.py "${validation_build_dir}"
        PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
            "${validation_build_dir}" \
            --archive-root "${repo_root}/build/firmware/${variant}"
        mkdir -p "${repo_root}/build/${variant}"
        install -m 0644 \
            "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
            "${repo_root}/build/${variant}/FoloToy-AI-Passport-full.bin"
        echo "Firmware build (${variant}): PASS"
    done
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
