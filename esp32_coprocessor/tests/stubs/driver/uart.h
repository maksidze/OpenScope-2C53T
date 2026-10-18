#pragma once
constexpr int UART_NUM_1=1,ESP_OK=0;
inline int mockHostBusy=0;
inline int uart_wait_tx_done(int,unsigned) { return mockHostBusy?-1:ESP_OK; }
