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
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_we_cfg.c main/we_cfg.c \
        -o "${test_dir}/test_we_cfg"
    "${test_dir}/test_we_cfg"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_we_provider.c main/we_provider.c \
        -o "${test_dir}/test_we_provider"
    "${test_dir}/test_we_provider"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_we_diag.c main/we_diag.c \
        -o "${test_dir}/test_we_diag"
    "${test_dir}/test_we_diag"
    python3 tests/test_verify_firmware.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    # managed_components/ 不入库:组件管理器每次解析依赖都会重新解压 esp_lvgl_port,
    # 把 FAP_SCREENSHOT_V1 的取帧钩子补丁冲掉。所以顺序必须是
    # 先 reconfigure(拉依赖)→ 打补丁 → 再 build,否则 fap_screenshot.c 会因
    # lvgl_port_display_set_snapshot_cb() 未声明而编译失败。
    idf_py_build() {
        SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
            idf.py -B "${validation_build_dir}" \
            -D "SDKCONFIG=${validation_build_dir}/sdkconfig" "$@"
    }

    idf_py_build reconfigure
    python3 "${repo_root}/scripts/patch-esp-lvgl-port.py"
    idf_py_build build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
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
