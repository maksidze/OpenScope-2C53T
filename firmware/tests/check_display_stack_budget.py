#!/usr/bin/env python3
"""Audit synchronous LVGL label and keyboard text call chains using GCC .su.

This is a regression budget, NOT a global worst-case stack analysis. Indirect
callbacks and the screen->child recursion are resolved for the current shallow
Activity UI in pinned LVGL 9.5.0. No text/UI input can create deeper containers.
Run after the LVGL build. Missing reports or an undersized stack fail loudly.
"""
import argparse
from pathlib import Path
import re


# Source-reviewed active frames for an invalidated transparent label under its
# screen. lv_label_event includes its inlined draw_main; dispatch includes its
# inlined execute_drawing. The same lv_obj_refr/lv_obj_redraw pair appears twice
# because the screen's draw frame remains alive while its child is redrawn.
CHAIN = (
    ("main", "vDisplayTask"), ("esp_activity", "esp_activity_display_service"),
    ("lv_timer", "lv_timer_handler"), ("lv_refr", "lv_display_refr_timer"),
    ("lv_refr", "refr_area"), ("lv_refr", "refr_configured_layer"),
    ("lv_refr", "refr_obj_and_children"), ("lv_refr", "lv_obj_refr"),
    ("lv_refr", "lv_obj_redraw"), ("lv_refr", "lv_obj_refr"),
    ("lv_refr", "lv_obj_redraw"), ("lv_obj_event", "lv_obj_send_event"),
    ("lv_obj_event", "event_send_core"), ("lv_obj_event", "lv_obj_event_base"),
    ("lv_label", "lv_label_event"), ("lv_draw_label", "lv_draw_label"),
    ("lv_draw", "lv_draw_finalize_task_creation"), ("lv_draw", "lv_draw_dispatch"),
    ("lv_draw", "lv_draw_dispatch_layer"), ("lv_draw_sw", "dispatch"),
    ("lv_draw_label", "lv_draw_label_iterate_characters"),
    ("lv_draw_label", "lv_draw_unit_draw_letter"), ("lv_draw_sw_letter", "draw_letter_cb"),
    ("lv_draw_sw_blend", "lv_draw_sw_blend"),
    ("lv_draw_sw_blend_to_rgb565", "lv_draw_sw_blend_color_to_rgb565"),
)
# Buttonmatrix dispatch calls draw_main after its base-object handler has
# returned; unlike lv_label_event, draw_main is not inlined by this compiler.
KEYBOARD_CHAIN = tuple(entry for entry in CHAIN[:13]) + (
    ("lv_buttonmatrix", "lv_buttonmatrix_event"), ("lv_buttonmatrix", "draw_main"),
) + CHAIN[15:]


def audit(build, header):
    frames = {}
    for module in {module for module, _ in CHAIN + KEYBOARD_CHAIN}:
        report = build / (module + ".su")
        if not report.is_file():
            raise SystemExit(f"Missing {report}; build FFT ESP_LINK_LVGL ESP_LINK_BRIDGE first")
        for line in report.read_text(encoding="utf-8").splitlines():
            symbol, size, kind = line.split("\t")
            if kind != "static":
                continue
            frames[(module, symbol.rsplit(":", 1)[-1])] = int(size)
    try:
        label_active = sum(frames[entry] for entry in CHAIN)
        keyboard_active = sum(frames[entry] for entry in KEYBOARD_CHAIN)
        active = max(label_active, keyboard_active)
    except KeyError as error:
        raise SystemExit(f"Missing static frame {error}; review compiler/LVGL changes")
    source = header.read_text(encoding="utf-8")
    words = int(re.search(r"#define\s+DISPLAY_LVGL_STACK_WORDS\s+(\d+)u", source)[1])
    budget = words * 4
    # ARM_CM4F port saves nine core registers plus hardware core/FPU frames;
    # reserve 208 bytes including stack alignment, then a separate 512-B margin.
    required = active + 208 + 512
    if budget < required:
        raise SystemExit(f"FAIL: display {budget} B < audited path/context/margin {required} B")
    assert 768 * 4 < required, "The old 3072-B negative control no longer exposes the regression; review audit"
    print(f"Display stack: {budget} B; label path {label_active} B, keyboard text {keyboard_active} B")
    print(f"Audited maximum {active} B + context/margin 720 B = {required} B")
    print("PASS: configured budget fits; old 3072-B budget fails the same audit")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    args = parser.parse_args()
    audit(args.build, Path(__file__).resolve().parents[1] / "include/task_stack_budget.h")
