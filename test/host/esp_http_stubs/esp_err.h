#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_HTTP_CONNECT 0x7002
#define ESP_ERR_HTTP_MAX_REDIRECT 0x7001
#define ESP_ERR_HTTP_WRITE_DATA 0x7003
#define ESP_ERR_HTTP_FETCH_HEADER 0x7004
const char *esp_err_to_name(esp_err_t error);
