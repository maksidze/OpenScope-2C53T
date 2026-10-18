/* OpenScope's pinned LVGL 9.5 configuration (Activity builds only). */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

/* No LVGL heap may land in .bss: the AT32 image has roughly 1.2 KiB static RAM
 * margin. LVGL asks this allocator once from lv_init(), after the Activity has
 * claimed SHMEM_OWNER_ESP_ACTIVITY. */
#define LV_MEM_SIZE (111104U)
#define LV_MEM_ADR 0
#define LV_MEM_POOL_INCLUDE "esp_activity.h"
#define LV_MEM_POOL_ALLOC(size) esp_activity_lvgl_heap()
#define LV_MEM_POOL_EXPAND_SIZE 0

#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_FLOAT 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
