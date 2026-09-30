#!/usr/bin/env python3
"""Re-apply the FAP_SCREENSHOT_V1 snapshot hook patch to esp_lvgl_port.

managed_components/ is git-ignored and re-resolved by the component manager,
which would wipe this patch.  Run from the repo root after any clean resolve:

    python3 scripts/patch-esp-lvgl-port.py

Idempotent: exits 0 quietly when the hook is already present.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DISP_C = ROOT / "managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c"
DISP_H = ROOT / "managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port_disp.h"

MARKER = "FAP capture hook"  # present in the patched .c

EDITS_C = [
    # 1) ctx struct: add callback fields after the flags bitfield
    (
        """        unsigned int sw_rotate: 1;    /* Use software rotation (slower) or PPA if available */
    } flags;
} lvgl_port_display_ctx_t;""",
        """        unsigned int sw_rotate: 1;    /* Use software rotation (slower) or PPA if available */
    } flags;
    lvgl_port_snapshot_cb_t   snapshot_cb;  /* Optional pre-swap frame hook */
    void                     *snapshot_ctx;
} lvgl_port_display_ctx_t;""",
    ),
    # 2) setter implementation right before the flush callback definition
    (
        """static void lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
    assert(drv != NULL);
    assert(area != NULL);
    assert(color_map != NULL);
    lvgl_port_display_ctx_t *disp_ctx = (lvgl_port_display_ctx_t *)lv_display_get_driver_data(drv);
    assert(disp_ctx != NULL);
    int offsetx1 = area->x1;""",
        """void lvgl_port_display_set_snapshot_cb(lv_display_t *drv, lvgl_port_snapshot_cb_t cb, void *user_ctx)
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
    assert(drv != NULL);
    assert(area != NULL);
    assert(color_map != NULL);
    lvgl_port_display_ctx_t *disp_ctx = (lvgl_port_display_ctx_t *)lv_display_get_driver_data(drv);
    assert(disp_ctx != NULL);
    int offsetx1 = area->x1;""",
    ),
    # 3) pre-swap hook call
    (
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
        """esp_err_t lvgl_port_remove_disp(lv_display_t *disp);

#ifdef __cplusplus
}
#endif""",
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
                                       void *user_ctx);

#ifdef __cplusplus
}
#endif""",
    ),
]


def apply(path: pathlib.Path, edits):
    text = path.read_text(encoding="utf-8")
    for old, new in edits:
        if old in text:
            text = text.replace(old, new, 1)
        elif MARKER not in text and "snapshot_cb" not in text:
            print(f"WARN: anchor not found in {path}; component version may differ", file=sys.stderr)
    path.write_text(text, encoding="utf-8")


def main() -> int:
    if not DISP_C.exists() or not DISP_H.exists():
        print("esp_lvgl_port component not found; nothing to patch", file=sys.stderr)
        return 1
    if MARKER in DISP_C.read_text(encoding="utf-8"):
        print("already patched")
        return 0
    apply(DISP_C, EDITS_C)
    apply(DISP_H, EDITS_H)
    print("patched esp_lvgl_port with FAP_SCREENSHOT_V1 snapshot hook")
    return 0


if __name__ == "__main__":
    sys.exit(main())
