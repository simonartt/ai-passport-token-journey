#!/usr/bin/env python3
"""Re-apply the FAP_SCREENSHOT_V1 snapshot hook patch to esp_lvgl_port.

managed_components/ is git-ignored and re-resolved by the component manager,
which would wipe this patch.  Run from the repo root after any clean resolve:

    python3 scripts/patch-esp-lvgl-port.py

Idempotent: exits 0 quietly when the hook is already present.

Failure policy: every edit must land. If an anchor is missing (upstream changed
the surrounding code), this exits non-zero with a explicit message instead of
writing a half-patched file. A silent half-patch is nasty -- the component still
compiles, so the error only shows up as an "undefined reference to
lvgl_port_display_set_snapshot_cb" at link time.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DISP_C = ROOT / "managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c"
DISP_H = ROOT / "managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port_disp.h"

MARKER = "FAP capture hook"  # present in the patched .c

SNAPSHOT_FIELDS = """    lvgl_port_snapshot_cb_t   snapshot_cb;  /* Optional pre-swap frame hook */
    void                     *snapshot_ctx;
} lvgl_port_display_ctx_t;"""

SNAPSHOT_SETTER = """void lvgl_port_display_set_snapshot_cb(lv_display_t *drv, lvgl_port_snapshot_cb_t cb, void *user_ctx)
{
    lvgl_port_display_ctx_t *disp_ctx = (lvgl_port_display_ctx_t *)lv_display_get_driver_data(drv);
    if (disp_ctx == NULL) {
        return;
    }
    disp_ctx->snapshot_cb = cb;
    disp_ctx->snapshot_ctx = user_ctx;
}

static void lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
"""

# (name, mode, pattern, replacement)
#   mode "text"  -> literal substring, must appear exactly once
#   mode "regex" -> first match of the pattern, must appear at least once
# Anchors are deliberately small: match the *shape* of the code, not a whole
# function body, so an upstream blank line or comment does not break the patch.
EDITS_C = [
    (
        "ctx struct fields",
        "regex",
        re.compile(r"(?m)^    \} flags;\n\} lvgl_port_display_ctx_t;\n"),
        "    } flags;\n" + SNAPSHOT_FIELDS + "\n",
    ),
    (
        "setter definition",
        "regex",
        re.compile(
            r"(?m)^static void lvgl_port_flush_callback\(lv_display_t \*drv, "
            r"const lv_area_t \*area, uint8_t \*color_map\)\n\{\n"
        ),
        SNAPSHOT_SETTER,
    ),
    (
        "pre-swap hook call",
        "text",
        """    if (disp_ctx->flags.swap_bytes) {
        size_t len = lv_area_get_size(area);
        lv_draw_sw_rgb565_swap(color_map, len);
    }""",
        """    /* FAP capture hook: raw native LE RGB565 pixels, before byte swap */
    if (disp_ctx->snapshot_cb) {
        disp_ctx->snapshot_cb(drv, area, color_map, disp_ctx->snapshot_ctx);
    }

    if (disp_ctx->flags.swap_bytes) {
        size_t len = lv_area_get_size(area);
        lv_draw_sw_rgb565_swap(color_map, len);
    }""",
    ),
]

EDITS_H = [
    (
        "snapshot API declarations",
        "text",
        """esp_err_t lvgl_port_remove_disp(lv_display_t *disp);""",
        """esp_err_t lvgl_port_remove_disp(lv_display_t *disp);

/**
 * @brief Snapshot callback type (FAP_SCREENSHOT_V1 capture support)
 *
 * @note Invoked from the LVGL flush callback BEFORE byte-swapping, so
 *       color_map holds native little-endian RGB565 pixels.
 */
typedef void (*lvgl_port_snapshot_cb_t)(lv_display_t *drv, const lv_area_t *area,
                                        uint8_t *color_map, void *user_ctx);

/**
 * @brief Register an optional pre-swap frame hook on the display.
 *
 * The hook fires for every flushed area with the raw LE RGB565 pixels.
 * Pass NULL to unregister. Observational use only.
 */
void lvgl_port_display_set_snapshot_cb(lv_display_t *drv,
                                       lvgl_port_snapshot_cb_t cb,
                                       void *user_ctx);""",
    ),
]


def read_text(path: pathlib.Path) -> str:
    # newline="" -> 不做行尾翻译:上游文件是什么行尾就保持什么行尾,
    # 否则在 Windows 上跑一次补丁会把整个文件重写成 CRLF(CI 是 Linux,
    # 但本地开发时会污染 diff)。
    with open(path, "r", encoding="utf-8", newline="") as fh:
        return fh.read()


def write_text(path: pathlib.Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)


def apply(path: pathlib.Path, edits) -> list:
    """Apply every edit or report failure. Returns the list of failed labels."""
    text = read_text(path)
    failed = []
    for label, mode, pattern, replacement in edits:
        if mode == "text":
            hits = text.count(pattern)
            if hits == 0:
                failed.append(label)
                continue
            if hits > 1:
                print(f"ERROR: {path.name}: anchor '{label}' is ambiguous "
                      f"({hits} matches)", file=sys.stderr)
                failed.append(label)
                continue
            text = text.replace(pattern, replacement, 1)
        else:
            match = pattern.search(text)
            if match is None:
                failed.append(label)
                continue
            text = text[:match.start()] + replacement + text[match.end():]
    write_text(path, text)
    return failed


def main() -> int:
    if not DISP_C.exists() or not DISP_H.exists():
        print(f"ERROR: esp_lvgl_port not found under {DISP_C.parent.parent}", file=sys.stderr)
        return 1

    if MARKER in read_text(DISP_C):
        print("already patched")
        return 0

    failed = apply(DISP_C, EDITS_C) + apply(DISP_H, EDITS_H)
    if failed:
        print("ERROR: could not patch esp_lvgl_port; anchors missing: "
              + ", ".join(failed), file=sys.stderr)
        print("       upstream likely changed the surrounding code -- update "
              "scripts/patch-esp-lvgl-port.py", file=sys.stderr)
        return 1

    # Post-conditions: the linker needs the definition, the compiler the declaration.
    src = read_text(DISP_C)
    hdr = read_text(DISP_H)
    if "void lvgl_port_display_set_snapshot_cb(lv_display_t *drv" not in src \
            or MARKER not in src:
        print("ERROR: setter definition did not land in esp_lvgl_port_disp.c", file=sys.stderr)
        return 1
    if "lvgl_port_snapshot_cb_t cb," not in hdr:
        print("ERROR: declaration did not land in esp_lvgl_port_disp.h", file=sys.stderr)
        return 1

    print("patched esp_lvgl_port with FAP_SCREENSHOT_V1 snapshot hook")
    return 0


if __name__ == "__main__":
    sys.exit(main())
