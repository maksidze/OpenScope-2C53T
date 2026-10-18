/* LVGL-backed ESP Activity lifecycle and native launcher boundary. */
#ifndef ESP_ACTIVITY_H
#define ESP_ACTIVITY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool esp_activity_init(void);
void esp_activity_deinit(void);
size_t esp_activity_heap_required(void);
void esp_activity_receive_app_list(const uint8_t *payload, uint16_t length);
void esp_activity_receive_start(const uint8_t *payload, uint16_t length);
void esp_activity_receive_state(const uint8_t *payload, uint16_t length, uint16_t transaction);
/* Link-task notification. It only dirties the native launcher model; all LCD
 * work and catalog requests remain in the display task. */
void esp_activity_link_ready(void);
void esp_activity_link_lost(void);
void esp_activity_stop_local(void);
bool esp_activity_active(void);
uint32_t esp_activity_token(void);

/* These are called only by the display task. */
void esp_activity_draw_launcher(void);
void esp_activity_display_service(void);
void esp_activity_display_leave(void);

/* These are called only by the input task. */
void esp_activity_launcher_move(int direction);
void esp_activity_launcher_start_selected(void);
void esp_activity_input_key(uint8_t button_id);

/* lv_conf.h points LVGL's built-in allocator at this lifecycle arena. */
void *esp_activity_lvgl_heap(void);
size_t esp_activity_lvgl_heap_size(void);

#endif
