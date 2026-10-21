#include "irsyncdiag.h"
#include <Arduino.h>
#include <Preferences.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static RTC_NOINIT_ATTR SyncDiagnosticState s_current;
static SyncDiagnosticState s_previous;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;

static void allocation_failed(size_t bytes,uint32_t caps,const char *function) {
    // Called inside the allocator: never allocate, query the heap, or write
    // flash here. The preceding checkpoint supplies the memory measurements.
    portENTER_CRITICAL(&s_lock);
    syncdiag_allocation_failed(s_current,bytes,caps,function);
    portEXIT_CRITICAL(&s_lock);
}
void irsyncdiag_init() {
    auto reason=esp_reset_reason();
    bool retained=reason!=ESP_RST_POWERON && reason!=ESP_RST_BROWNOUT && syncdiag_valid(s_current) &&
        (s_current.active || s_current.failed || s_current.allocation_size);
    Preferences p;
    if (retained) {
        s_previous=s_current;
        // One bounded write after reboot, never from the crash path. Keep the
        // RTC evidence usable even when settings storage is full.
        if (p.begin("syncdiag",false)) { p.putBytes("previous",&s_previous,sizeof(s_previous)); p.end(); }
    } else if (p.begin("syncdiag",true)) {
        if (p.getBytesLength("previous")==sizeof(s_previous)) p.getBytes("previous",&s_previous,sizeof(s_previous));
        p.end();
    }
    syncdiag_begin(s_current); syncdiag_finish(s_current,"No HotSync since boot",false);
    heap_caps_register_failed_alloc_callback(allocation_failed);
}
void irsyncdiag_begin() {
    portENTER_CRITICAL(&s_lock); syncdiag_begin(s_current); portEXIT_CRITICAL(&s_lock);
    irsyncdiag_checkpoint("Creating HotSync task");
}
void irsyncdiag_checkpoint(const char *stage) {
    multi_heap_info_t heap={}; heap_caps_get_info(&heap,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    // ESP-IDF measures this in bytes, unlike upstream FreeRTOS stack words.
    uint32_t stack=uxTaskGetStackHighWaterMark(nullptr),ms=millis();
    portENTER_CRITICAL(&s_lock);
    syncdiag_event(s_current,stage,heap.total_free_bytes,heap.largest_free_block,stack,ms);
    portEXIT_CRITICAL(&s_lock);
}
void irsyncdiag_service(const char *service) {
    portENTER_CRITICAL(&s_lock); syncdiag_service(s_current,service); portEXIT_CRITICAL(&s_lock);
    irsyncdiag_checkpoint(service);
}
void irsyncdiag_finish(const char *outcome,bool failed) {
    irsyncdiag_checkpoint("HotSync finished");
    portENTER_CRITICAL(&s_lock); syncdiag_finish(s_current,outcome,failed); portEXIT_CRITICAL(&s_lock);
}
bool irsyncdiag_snapshot(SyncDiagnosticState &state,bool previous) {
    portENTER_CRITICAL(&s_lock); state=previous ? s_previous : s_current; portEXIT_CRITICAL(&s_lock);
    return syncdiag_valid(state);
}
bool irsyncdiag_memory_available(size_t allocation,size_t reserve) {
    multi_heap_info_t heap={}; heap_caps_get_info(&heap,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    return heap.total_free_bytes>=reserve && allocation<=heap.total_free_bytes-reserve &&
        heap.largest_free_block>=allocation;
}
