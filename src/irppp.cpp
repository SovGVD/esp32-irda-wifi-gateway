#include "irppp.h"
#include "irportal.h"
#include "irdebug.h"
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

extern "C" {
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "lwip/lwip_napt.h"
}

static ppp_pcb *s_ppp = nullptr;
static struct netif s_ppp_netif;

// PPP bytes for the Palm. Written by the network stack task, read by loop().
static StreamBufferHandle_t s_to_palm = nullptr;

// The Palm is waiting for PPP, but the previous session is still shutting down
static bool s_start_pending = false;
static uint32_t s_last_start_attempt_ms = 0;

// Link status reported by the network stack task, logged from loop()
static volatile int s_ppp_status = PPPERR_NONE;
static volatile bool s_ppp_status_changed = false;
static volatile uint32_t s_palm_ip = 0;
static volatile uint32_t s_dropped_bytes = 0;

// Called by lwIP (network stack task) with PPP bytes to transmit
static u32_t on_ppp_output(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx) {
    // The IR link is far slower than Wi-Fi: when the buffer is full the packet
    // is dropped and TCP on the far end slows down and retransmits.
    if (xStreamBufferSpacesAvailable(s_to_palm) < len) {
        s_dropped_bytes += len;
        return 0;
    }
    return xStreamBufferSend(s_to_palm, data, len, 0);
}

// Called by lwIP (network stack task) when the PPP link comes up or goes down
static void on_ppp_status(ppp_pcb *pcb, int err_code, void *ctx) {
    if (err_code == PPPERR_NONE) {
        // Translate the Palm's private address onto the Wi-Fi interface
        ip_napt_enable(ip4_addr_get_u32(netif_ip4_addr(&s_ppp_netif)), 1);
        s_palm_ip = ip4_addr_get_u32(netif_ip4_gw(&s_ppp_netif));
    } else {
        s_palm_ip = 0;
    }
    s_ppp_status = err_code;
    s_ppp_status_changed = true;
}

void irppp_init() {
    if (!ENABLE_WIFI_GATEWAY) {
        return;
    }

    // Wi-Fi, plus the settings portal and DNS the Palm reaches over PPP
    irportal_init();

    s_to_palm = xStreamBufferCreate(PPP_TO_IR_BUF_SIZE, 1);
    if (s_to_palm != nullptr) {
        s_ppp = pppapi_pppos_create(&s_ppp_netif, on_ppp_output, on_ppp_status, nullptr);
    }
}

bool irppp_enabled() {
    return s_ppp != nullptr;
}

void irppp_start() {
    if (s_ppp == nullptr) {
        return;
    }

    // We are the "provider" end: fixed address for us, and one to hand to the Palm
    ip4_addr_t addr;
    ip4addr_aton(PPP_GATEWAY_IP, &addr);
    ppp_set_ipcp_ouraddr(s_ppp, &addr);
    ip4addr_aton(PPP_PALM_IP, &addr);
    ppp_set_ipcp_hisaddr(s_ppp, &addr);

    // No Van Jacobson TCP header compression: it gains little at this speed, and
    // it is one less stateful layer between two very different TCP/IP stacks
    s_ppp->ipcp_wantoptions.neg_vj = 0;
    s_ppp->ipcp_allowoptions.neg_vj = 0;

    // The Palm asks us for names: we answer the portal's and relay the rest
    ip4addr_aton(PPP_GATEWAY_IP, &addr);
    ppp_set_ipcp_dnsaddr(s_ppp, 0, &addr);

    // Drop leftovers of the previous session before PPP queues its first packet.
    // Connecting fails while that session is still terminating; irppp_poll() retries.
    xStreamBufferReset(s_to_palm);
    s_last_start_attempt_ms = millis();
    s_start_pending = (pppapi_connect(s_ppp, 0) != ERR_OK);
}

uint32_t irppp_palm_ip() {
    return s_palm_ip;
}

size_t irppp_pending_output() {
    return (s_to_palm != nullptr) ? xStreamBufferBytesAvailable(s_to_palm) : 0;
}

uint32_t irppp_dropped_bytes() {
    return s_dropped_bytes;
}

void irppp_stop() {
    if (s_ppp == nullptr) {
        return;
    }
    s_start_pending = false;
    pppapi_close(s_ppp, 1); // The IR link is already gone: no goodbye handshake
}

void irppp_input(const uint8_t *data, size_t len) {
    if (s_ppp == nullptr || len == 0) {
        return;
    }
    pppos_input_tcpip(s_ppp, (u8_t *)data, len);
}

size_t irppp_read_output(uint8_t *dest, size_t max_len) {
    if (s_to_palm == nullptr) {
        return 0;
    }
    return xStreamBufferReceive(s_to_palm, dest, max_len, 0);
}

void irppp_poll() {
    if (s_ppp == nullptr) {
        return;
    }

    if (s_start_pending && millis() - s_last_start_attempt_ms > 250) {
        irppp_start();
    }

#if ENABLE_CDC_DEBUG
    static bool wifi_was_up = false;
    bool wifi_up = WiFi.status() == WL_CONNECTED;
    if (wifi_up != wifi_was_up) {
        wifi_was_up = wifi_up;
        if (wifi_up) {
            irdbg_printf("Wi-Fi connected, IP %s", WiFi.localIP().toString().c_str());
        } else {
            irdbg_printf("Wi-Fi disconnected");
        }
    }

    if (s_ppp_status_changed) {
        s_ppp_status_changed = false;
        if (s_ppp_status == PPPERR_NONE) {
            irdbg_printf("PPP up: Palm is %s, gateway %s",
                         IPAddress(s_palm_ip).toString().c_str(), PPP_GATEWAY_IP);
        } else {
            irdbg_printf("PPP down (code %d)", s_ppp_status);
        }
    }
#endif
}
