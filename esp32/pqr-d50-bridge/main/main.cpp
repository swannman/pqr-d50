// PQR D50 -> Grafana Cloud bridge for ESP32-S3.
//
// Native USB (host) <-> D50's internal FT232 (VCP) ; WiFi -> InfluxDB push.
//
// The D50 protocol (command mode C1..C6, idle-terminated ASCII reports) is the
// same one validated against the device from Linux/macOS; here we drive it over
// the ESP32-S3 USB host VCP instead of a host serial port.
//
// NOTE: the USB-host + WiFi paths require on-hardware bring-up (no IDF in CI);
// the parsing / line-protocol cores are shared with test/host_test.c and are
// verified on a host.

#include <stdio.h>
#include <string.h>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include <time.h>

#include "usb/usb_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ftdi.hpp"
#include "usb/cdc_acm_host.h"

extern "C" {
#include "d50_parse.h"
#include "influx.h"
}
#include "config.h"

static const char *TAG = "pqr-bridge";
using namespace esp_usb;

// ---- RX accumulation (the VCP data callback feeds this) ----
static uint8_t  s_rx[8192];
static volatile size_t   s_rx_len = 0;
static volatile int64_t  s_rx_last_us = 0;
static std::unique_ptr<CdcAcmDevice> s_vcp;
static SemaphoreHandle_t s_dev_ready;

static bool rx_cb(const uint8_t *data, size_t len, void *arg) {
    if (s_rx_len + len <= sizeof(s_rx)) {
        memcpy(s_rx + s_rx_len, data, len);
        s_rx_len += len;
    }
    s_rx_last_us = esp_timer_get_time();
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *e, void *arg) {
    if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "D50 disconnected");
        s_vcp.reset();
    }
}

// ---- D50 command helpers over the VCP ----
static void d50_write(const char *s, size_t n) {
    if (s_vcp) s_vcp->tx_blocking((uint8_t *)s, n);
}

// Send `cmd`, then read until the line is idle for `idle_ms`. Returns bytes.
static size_t d50_xfer(const char *cmd, size_t cmdlen, uint32_t idle_ms,
                       uint32_t max_ms) {
    s_rx_len = 0;
    s_rx_last_us = esp_timer_get_time();
    d50_write(cmd, cmdlen);
    int64_t start = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int64_t now = esp_timer_get_time();
        if (s_rx_len && (now - s_rx_last_us) > (int64_t)idle_ms * 1000) break;
        if ((now - start) > (int64_t)max_ms * 1000) break;
    }
    return s_rx_len;
}

// Resync to command mode: spam CR (advance any menu sub-prompt), ESC (exit
// menu), then confirm with C1. Mirrors PQRClient.reset().
static bool d50_reset(void) {
    for (int i = 0; i < 30; i++) { d50_write("\r", 1); vTaskDelay(pdMS_TO_TICKS(8)); }
    d50_write("\x1b", 1); vTaskDelay(pdMS_TO_TICKS(200));
    size_t n = d50_xfer("C1", 2, 500, 3000);
    if (n >= sizeof(s_rx)) n = sizeof(s_rx) - 1;
    s_rx[n] = 0;   // NUL-terminate for strstr
    return strstr((char *)s_rx, "PowerTronics") != NULL;
}

// Set the D50 RTC from system time via the C6 setup menu (option 1).
// Prompt format: MM/DD/YY,HH:MM:SS  -> replies "Command OK".
static bool d50_set_clock(void) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%m/%d/%y,%H:%M:%S", &lt);

    d50_xfer("C6", 2, 800, 3000);      // enter setup menu
    d50_write("1\r", 2);               // option 1: date/time
    vTaskDelay(pdMS_TO_TICKS(300));
    s_rx_len = 0; s_rx_last_us = esp_timer_get_time();
    d50_write(stamp, strlen(stamp));
    d50_write("\r", 1);
    // wait for "Command OK"
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < 2000 * 1000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (s_rx_len && (esp_timer_get_time() - s_rx_last_us) > 500 * 1000) break;
    }
    size_t n = s_rx_len < sizeof(s_rx) ? s_rx_len : sizeof(s_rx) - 1;
    s_rx[n] = 0;
    bool ok = strstr((char *)s_rx, "Command OK") != NULL;
    d50_reset();
    ESP_LOGI(TAG, "clock set to %s -> %s", stamp, ok ? "OK" : "FAILED");
    return ok;
}

// ---- WiFi ----
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void) {
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---- Influx push ----
static esp_err_t influx_push(const char *body, size_t len) {
    esp_http_client_config_t cfg = {};
    cfg.url = INFLUX_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    cfg.username = INFLUX_USER;
    cfg.password = INFLUX_TOKEN;
    cfg.crt_bundle_attach = NULL;   // set esp_crt_bundle_attach in CMake for TLS
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "text/plain");
    esp_http_client_set_post_field(c, body, len);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "influx push -> HTTP %d", status);
    else
        ESP_LOGE(TAG, "influx push failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return (err == ESP_OK && status / 100 == 2) ? ESP_OK : ESP_FAIL;
}

// ---- USB host plumbing ----
static void usb_lib_task(void *arg) {
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void open_d50(void) {
    cdc_acm_host_device_config_t dev_cfg = {};
    dev_cfg.connection_timeout_ms = 5000;
    dev_cfg.out_buffer_size = 512;
    dev_cfg.in_buffer_size = 512;
    dev_cfg.event_cb = handle_event;
    dev_cfg.data_cb = rx_cb;
    dev_cfg.user_arg = NULL;

    VCP::register_driver<FT23x>();          // FTDI VCP (D50's internal FT232)
    s_vcp.reset(VCP::open(&dev_cfg));
    if (!s_vcp) { ESP_LOGE(TAG, "VCP open failed"); return; }

    cdc_acm_line_coding_t lc = {};
    lc.dwDTERate = D50_BAUD;
    lc.bCharFormat = 0;   // 1 stop bit
    lc.bParityType = 0;   // none
    lc.bDataBits = 8;
    s_vcp->line_coding_set(&lc);
    s_vcp->set_control_line_state(true, true);  // DTR, RTS
    ESP_LOGI(TAG, "D50 VCP open @ %d 8N1", D50_BAUD);
    xSemaphoreGive(s_dev_ready);
}

// Pull the data log (C4), push samples newer than *watermark, advance it.
// If `send` is false, only advance the watermark (boot priming, no push).
static void process_datalog(int64_t *watermark, bool send) {
    static char body[6144];
    static d50_sample_t samples[256];
    size_t n = d50_xfer("C4", 2, 1500, 30000);
    size_t cnt = d50_parse_datalog((char *)s_rx, n, samples, 256);
    size_t bl = 0; int pushed = 0; int64_t newest = *watermark;
    for (size_t i = 0; i < cnt; i++) {
        int64_t ts = d50_timestamp_to_unix(samples[i].date, samples[i].time);
        if (ts <= *watermark) continue;
        if (ts > newest) newest = ts;
        if (!send) continue;
        char line[160];
        int w = influx_format_sample(line, sizeof(line), INFLUX_MEASUREMENT,
                                     INFLUX_USER, &samples[i], ts);
        if (w < 0 || bl + w + 1 >= sizeof(body)) break;
        memcpy(body + bl, line, w); bl += w; body[bl++] = '\n';
        pushed++;
    }
    if (send && bl && influx_push(body, bl) == ESP_OK) *watermark = newest;
    else if (!send) *watermark = newest;     // prime only
    if (send) ESP_LOGI(TAG, "datalog: %d new of %u", pushed, (unsigned)cnt);
}

// Pull the detail report (C3), push events newer than *watermark.
static void process_events(int64_t *watermark, bool send) {
    static char body[6144];
    static d50_event_t events[256];
    size_t n = d50_xfer("C3", 2, 1500, 30000);
    size_t cnt = d50_parse_detail((char *)s_rx, n, events, 256);
    size_t bl = 0; int pushed = 0; int64_t newest = *watermark;
    for (size_t i = 0; i < cnt; i++) {
        int64_t ts = d50_timestamp_to_unix(events[i].date, events[i].time);
        if (ts <= *watermark) continue;
        if (ts > newest) newest = ts;
        if (!send) continue;
        char line[160];
        int w = influx_format_event(line, sizeof(line), INFLUX_EVENT_MEASUREMENT,
                                    INFLUX_USER, &events[i], ts);
        if (w < 0 || bl + w + 1 >= sizeof(body)) break;
        memcpy(body + bl, line, w); bl += w; body[bl++] = '\n';
        pushed++;
    }
    if (send && bl && influx_push(body, bl) == ESP_OK) *watermark = newest;
    else if (!send) *watermark = newest;
    if (send && pushed) ESP_LOGI(TAG, "events: %d new of %u", pushed, (unsigned)cnt);
}

static void sntp_start(void) {
    setenv("TZ", POSIX_TZ, 1); tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();
    for (int i = 0; i < 30 && sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED; i++)
        vTaskDelay(pdMS_TO_TICKS(500));
    time_t now = time(NULL);
    ESP_LOGI(TAG, "SNTP time: %s", ctime(&now));
}

// ---- main loop ----
extern "C" void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    if (USB_VBUS_EN_GPIO >= 0) {
        gpio_set_direction((gpio_num_t)USB_VBUS_EN_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)USB_VBUS_EN_GPIO, 1);   // enable 5V to device
    }

    wifi_start();

    s_dev_ready = xSemaphoreCreateBinary();
    usb_host_config_t host_cfg = {};
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi up");
    sntp_start();

    open_d50();
    xSemaphoreTake(s_dev_ready, pdMS_TO_TICKS(10000));

    // Sync the D50 RTC first so its timestamps match our (NTP) clock, then
    // prime watermarks from the current logs so we don't backfill old samples
    // (Grafana Cloud/Mimir rejects samples older than ~1h).
    d50_reset();
    d50_set_clock();
    int64_t sample_wm = 0, event_wm = 0;
    process_datalog(&sample_wm, false);
    process_events(&event_wm, false);
    ESP_LOGI(TAG, "primed: sample_wm=%lld event_wm=%lld",
             (long long)sample_wm, (long long)event_wm);

    int64_t last_clock_sync = esp_timer_get_time();

    while (true) {
        if (!s_vcp) { open_d50(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        d50_reset();
        process_datalog(&sample_wm, true);
        process_events(&event_wm, true);

        // periodic RTC re-sync (drift correction)
        if ((esp_timer_get_time() - last_clock_sync) >
            (int64_t)CLOCK_SYNC_HOURS * 3600 * 1000000LL) {
            d50_reset();
            d50_set_clock();
            last_clock_sync = esp_timer_get_time();
        }

        vTaskDelay(pdMS_TO_TICKS(D50_POLL_SEC * 1000));
    }
}
