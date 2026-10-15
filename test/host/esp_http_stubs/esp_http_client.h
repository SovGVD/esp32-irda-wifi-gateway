#pragma once
#include "esp_err.h"
enum esp_http_client_method_t { HTTP_METHOD_GET,HTTP_METHOD_POST,HTTP_METHOD_PUT,HTTP_METHOD_PATCH,HTTP_METHOD_DELETE };
enum esp_http_client_event_id_t { HTTP_EVENT_ON_DATA,HTTP_EVENT_ON_HEADER };
struct esp_http_client_event_t { esp_http_client_event_id_t event_id; void *user_data; void *data; int data_len;
    const char *header_key=nullptr,*header_value=nullptr; };
struct esp_http_client_config_t {
    const char *url;
    esp_err_t (*crt_bundle_attach)(void *);
    int timeout_ms,buffer_size_tx,buffer_size;
    bool disable_auto_redirect;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
};
struct MockHttpClient;
typedef MockHttpClient *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,esp_http_client_method_t method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,const char *key,const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,const char *data,int length);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
esp_err_t esp_http_client_open(esp_http_client_handle_t client,int write_len);
int esp_http_client_write(esp_http_client_handle_t client,const char *data,int len);
int esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client,char *data,int len);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_get_errno(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
