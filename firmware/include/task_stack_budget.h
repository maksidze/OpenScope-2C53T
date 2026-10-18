/* FreeRTOS stack depths are StackType_t WORDS (4 bytes on Cortex-M4F).
 * Keep the LVGL budget distinct from the original native renderer budget. */
#ifndef TASK_STACK_BUDGET_H
#define TASK_STACK_BUDGET_H

#define DISPLAY_NATIVE_STACK_WORDS 768u
#define DISPLAY_LVGL_STACK_WORDS  1536u

#ifdef ESP_LINK_LVGL
/* LV_OS_NONE executes nested object events, text layout and software drawing
 * on the display task. The audited screen->label->glyph path is ~3 KiB before
 * the Cortex-M4F exception/FPU context, so the old 768-word stack had no margin.
 * 6 KiB is charged to the existing RTOS heap BEFORE optional ESP preflight;
 * neither .bss nor configTOTAL_HEAP_SIZE grows. Verify on-target low-water
 * marks after Wi-Fi status changes; a .su audit is not a whole-program bound. */
#define DISPLAY_TASK_STACK_WORDS DISPLAY_LVGL_STACK_WORDS
#else
#define DISPLAY_TASK_STACK_WORDS DISPLAY_NATIVE_STACK_WORDS
#endif

#endif
