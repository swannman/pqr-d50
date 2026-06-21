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
#include <exception>
#include <array>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
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

// Custom FTDI VCP driver for the D50's non-standard PID. The stock FT23x only
// recognizes FT232(0x6001)/FT231(0x6015); the D50's FT chip reports D50_USB_PID.
// We reuse the component's ftdi_vcp_open(), just with our PID in the list.
#if D50_USB_PID
class D50Ftdi : public CdcAcmDevice {
public:
    D50Ftdi(uint16_t pid, const cdc_acm_host_device_config_t *cfg, uint8_t idx = 0) {
        const esp_err_t err = ftdi_vcp_open(pid, idx, cfg, &this->cdc_hdl);
        if (err != ESP_OK) throw (err);
    }
    static constexpr uint16_t vid = FTDI_VID;
    static constexpr std::array<uint16_t, 1> pids = { D50_USB_PID };
};
#endif

// ---- RX accumulation (the VCP data callback feeds this) ----
static uint8_t  s_rx[8192];
static volatile size_t   s_rx_len = 0;
static volatile int64_t  s_rx_last_us = 0;
static std::unique_ptr<CdcAcmDevice> s_vcp;
static SemaphoreHandle_t s_dev_ready;
static char s_unit_id[16] = "unknown";   // the D50's own ID, used as the metric tag
static double s_last_hot = 120.0;        // latest Hot RMS volts, baseline for impulse peaks

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

#if D50_SET_THRESHOLDS
// Apply CH1/CH2 surge/sag/power-fail thresholds via the C6 setup menu (option 4).
// The flow is a fixed 6-prompt walk that ignores ESC; we send a value, "0" for
// default, or blank (CR) to keep each. Then we read the values back and log them.
static void d50_apply_thresholds(void) {
    const int vals[6] = { TH_CH1_SURGE, TH_CH1_SAG, TH_CH1_PFAIL,
                          TH_CH2_SURGE, TH_CH2_SAG, TH_CH2_PFAIL };
    const char *names[6] = { "CH1 Surge", "CH1 Sag", "CH1 PowerFail",
                             "CH2 Surge", "CH2 Sag", "CH2 PowerFail" };
    ESP_LOGI(TAG, "applying disturbance thresholds...");
    d50_xfer("C6", 2, 800, 3000);          // setup menu
    d50_xfer("4\r", 2, 1200, 3000);        // option 4 -> first prompt (CH1 Surge)
    for (int i = 0; i < 6; i++) {
        char buf[16];
        if (vals[i] < 0) { buf[0] = '\r'; buf[1] = 0; }      // keep current
        else snprintf(buf, sizeof(buf), "%d\r", vals[i]);    // set volts (0=default)
        ESP_LOGI(TAG, "  %s <- %s", names[i], vals[i] < 0 ? "keep" : buf);
        d50_xfer(buf, strlen(buf), 1000, 3000);              // send, read next prompt
    }
    d50_reset();

    // Read back: walk option 4 again with blanks; each prompt echoes the stored value.
    d50_xfer("C6", 2, 800, 3000);
    d50_xfer("4\r", 2, 1200, 3000);
    for (int i = 0; i < 6; i++) {
        size_t n = s_rx_len < sizeof(s_rx) ? s_rx_len : sizeof(s_rx) - 1;
        s_rx[n] = 0;
        ESP_LOGI(TAG, "  readback %s: %.70s", names[i], (char *)s_rx);
        d50_xfer("\r", 1, 1000, 3000);     // advance to next prompt
    }
    d50_reset();
    ESP_LOGI(TAG, "thresholds applied");
}
#endif

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
// Build "Basic base64(user:token)" once, so we send auth preemptively (avoids a
// guaranteed 401 + POST-body-resend dance every minute).
static void make_basic_auth(char *hdr, size_t hdrsz, const char *user) {
    char creds[200];
    int n = snprintf(creds, sizeof(creds), "%s:%s", user, INFLUX_TOKEN);
    unsigned char b64[300]; size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)creds, n);
    b64[olen] = 0;
    snprintf(hdr, hdrsz, "Basic %s", (char *)b64);
}

static esp_err_t http_post(const char *url, const char *auth, const char *ctype,
                           const char *body, size_t len, const char *tag) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // verify Grafana Cloud TLS
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", ctype);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_post_field(c, body, len);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    if (err == ESP_OK) ESP_LOGI(TAG, "%s -> HTTP %d", tag, status);
    else ESP_LOGE(TAG, "%s failed: %s", tag, esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return (err == ESP_OK && status / 100 == 2) ? ESP_OK : ESP_FAIL;
}

static esp_err_t influx_push(const char *body, size_t len) {
    static char auth[320];
    if (!auth[0]) make_basic_auth(auth, sizeof(auth), INFLUX_USER);
    return http_post(INFLUX_URL, auth, "text/plain", body, len, "influx");
}

static esp_err_t loki_push(const char *body, size_t len) {
    static char auth[320];
    if (!auth[0]) make_basic_auth(auth, sizeof(auth), LOKI_USER);
    return http_post(LOKI_URL, auth, "application/json", body, len, "loki");
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
    dev_cfg.connection_timeout_ms = 2000;
    dev_cfg.out_buffer_size = 512;
    dev_cfg.in_buffer_size = 0;             // FTDI driver requires 0 (it warns otherwise)
    dev_cfg.event_cb = handle_event;
    dev_cfg.data_cb = rx_cb;
    dev_cfg.user_arg = NULL;

    try {
        s_vcp.reset(VCP::open(&dev_cfg));   // returns null (no throw) if no match
    } catch (const std::exception &e) {
        ESP_LOGW(TAG, "VCP open threw: %s", e.what());
        s_vcp.reset();
    }
    if (!s_vcp) { ESP_LOGW(TAG, "D50 not opened yet; will retry"); return; }

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

// Read the C1 identity and store the D50's unit ID for use as the metric tag.
static void fetch_unit_id(void) {
    size_t n = d50_xfer("C1", 2, 600, 3000);
    if (n >= sizeof(s_rx)) n = sizeof(s_rx) - 1;
    s_rx[n] = 0;
    d50_ident_t id;
    if (d50_parse_ident((char *)s_rx, n, &id) && id.unit_id[0]) {
        strncpy(s_unit_id, id.unit_id, sizeof(s_unit_id) - 1);
        s_unit_id[sizeof(s_unit_id) - 1] = 0;
    }
    ESP_LOGI(TAG, "D50 unit=%s model=%s fw=%s", s_unit_id, id.model, id.firmware);
}

// Pull the data log (C4), push samples newer than *watermark, advance it.
// If `send` is false, only advance the watermark (boot priming, no push).
static int process_datalog(int64_t *watermark, bool send) {
    static char body[6144];
    static d50_sample_t samples[256];
    size_t n = d50_xfer("C4", 2, 1500, 30000);
    size_t cnt = d50_parse_datalog((char *)s_rx, n, samples, 256);
    if (cnt) s_last_hot = samples[cnt - 1].ch1_value;   // baseline for impulse peaks
    size_t bl = 0; int pushed = 0; int64_t newest = *watermark;
    for (size_t i = 0; i < cnt; i++) {
        int64_t ts = d50_timestamp_to_unix(samples[i].date, samples[i].time);
        if (ts <= *watermark) continue;
        if (ts > newest) newest = ts;
        if (!send) continue;
        char line[160];
        int w = influx_format_sample(line, sizeof(line), INFLUX_MEASUREMENT,
                                     s_unit_id, &samples[i], ts);
        if (w < 0 || bl + w + 1 >= sizeof(body)) break;
        memcpy(body + bl, line, w); bl += w; body[bl++] = '\n';
        pushed++;
    }
    if (send && bl && influx_push(body, bl) == ESP_OK) *watermark = newest;
    else if (!send) *watermark = newest;     // prime only
    if (send) ESP_LOGI(TAG, "datalog: %d new of %u", pushed, (unsigned)cnt);
    return pushed;
}

// Clear the D50's data log + events (C5). Everything has already been pushed to
// Grafana; this lets a full/stalled log resume recording. Confirms with "Y".
static void d50_clear_data(void) {
    ESP_LOGW(TAG, "data log stalled (full) -> clearing device to resume logging");
    d50_xfer("C5", 2, 800, 3000);     // -> "Are You Sure ... ?"
    d50_xfer("Y", 1, 2000, 20000);    // confirm -> FLASH erase -> "Ram has been cleared"
    d50_reset();
    ESP_LOGI(TAG, "device cleared; logging resumes");
}

// Pull the detail report (C3), push events newer than *watermark.
// Convert a D50 event date/time ("Mon/DD/YY","HH:MM:SS[.cc]") to Loki ns.
static int64_t d50_event_ns(const d50_event_t *e) {
    int64_t sec = d50_timestamp_to_unix(e->date, e->time);
    int cc = 0;
    const char *dot = strchr(e->time, '.');
    if (dot) cc = atoi(dot + 1);                 // hundredths of a second
    return sec * 1000000000LL + (int64_t)cc * 10000000LL;
}

// Push every disturbance in the C3 detail report to Loki as a structured line.
// Loki de-dupes identical (timestamp, line) entries, so re-sending the whole
// report each poll is safe and reboot-proof (no watermark needed).
// plot_v = the value to draw on the voltage axis:
//   impulse -> live voltage + magnitude (the transient's peak, "how much higher")
//   sag     -> the absolute RMS volts (the dip); sag_complete reuses the dip level
//              so a sag's start+complete connect as a flat line across its duration.
static void push_events_loki(void) {
    static char body[8192];
    static d50_event_t evs[128];
    size_t n = d50_xfer("C3", 2, 1500, 30000);
    size_t cnt = d50_parse_detail((char *)s_rx, n, evs, 128);
    if (!cnt) return;

    int bl = snprintf(body, sizeof(body),
        "{\"streams\":[{\"stream\":{\"service_name\":\"pqr_d50\",\"unit\":\"%s\"},"
        "\"values\":[", s_unit_id);
    int added = 0;
    double sag_low = 0;
    for (size_t i = 0; i < cnt; i++) {
        char type[24];
        strncpy(type, evs[i].event_type, sizeof(type) - 1); type[sizeof(type) - 1] = 0;
        influx_normalize_label(type);            // "Sag Start" -> "sag_start"

        double plot_v;
        if (strstr(type, "impulse"))           plot_v = s_last_hot + evs[i].magnitude;
        else if (strstr(type, "sag_start"))    { plot_v = evs[i].magnitude; sag_low = plot_v; }
        else if (strstr(type, "sag_complete")) plot_v = sag_low > 0 ? sag_low : evs[i].magnitude;
        else                                   plot_v = evs[i].magnitude;

        int64_t ts = d50_event_ns(&evs[i]);
        char line[128];
        // plot_v rounded to whole volts so a drifting baseline doesn't change the
        // line each poll (keeps Loki's identical-entry de-dup working).
        snprintf(line, sizeof(line), "type=%s channel=%s magnitude=%.1f plot_v=%.0f",
                 type, evs[i].channel, evs[i].magnitude, plot_v);
        int w = snprintf(body + bl, sizeof(body) - bl, "%s[\"%lld\",\"%s\"]",
                         added ? "," : "", (long long)ts, line);
        if (w < 0 || bl + w + 8 >= (int)sizeof(body)) break;
        bl += w; added++;
    }
    bl += snprintf(body + bl, sizeof(body) - bl, "]}]}");
    if (added && loki_push(body, bl) == ESP_OK)
        ESP_LOGI(TAG, "loki: pushed %d events", added);
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

// How many USB devices are currently enumerated on the host bus. 0 => nothing
// powered/enumerated (suspect VBUS/OTG); >=1 => a device is present.
static int usb_device_count(void) {
    uint8_t addrs[16]; int n = 0;
    usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n);
    return n;
}

// Diagnostic: prove the USB-host link to the D50 without WiFi/Grafana.
static void usb_selftest(void) {
    ESP_LOGI(TAG, "=== USB SELF-TEST: connect the D50 to the native USB port ===");
    while (!s_vcp) {
        int n = usb_device_count();
        ESP_LOGI(TAG, "USB devices on bus: %d %s", n,
                 n == 0 ? "(none - check VBUS/OTG power to the D50)" : "");
        open_d50();
        if (!s_vcp) vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "*** D50 ENUMERATED on USB host ***");
    while (true) {
        d50_reset();
        size_t n = d50_xfer("C1", 2, 600, 3000);
        if (n >= sizeof(s_rx)) n = sizeof(s_rx) - 1;
        s_rx[n] = 0;
        ESP_LOGI(TAG, "C1 identity: %.110s", (char *)s_rx);

        static d50_sample_t samples[64];
        n = d50_xfer("C4", 2, 1500, 30000);
        size_t cnt = d50_parse_datalog((char *)s_rx, n, samples, 64);
        ESP_LOGI(TAG, "C4 data log: %u samples", (unsigned)cnt);
        if (cnt) ESP_LOGI(TAG, "  latest: %s %s  hot=%.1f V  neu=%.1f V",
                          samples[cnt - 1].date, samples[cnt - 1].time,
                          samples[cnt - 1].ch1_value, samples[cnt - 1].ch2_value);

        // Detail report (C3) — every disturbance the D50 has recorded
        n = d50_xfer("C3", 2, 1500, 30000);
        static d50_event_t evs[64];
        size_t ec = d50_parse_detail((char *)s_rx, n, evs, 64);
        ESP_LOGI(TAG, "C3 detail: %u event(s)", (unsigned)ec);
        for (size_t i = 0; i < ec; i++)
            ESP_LOGI(TAG, "  EVENT %s %s  %s  %s  %.1f V", evs[i].date, evs[i].time,
                     evs[i].channel, evs[i].event_type, evs[i].magnitude);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ---- main loop ----
extern "C" void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (USB_VBUS_EN_GPIO >= 0) {
        gpio_set_direction((gpio_num_t)USB_VBUS_EN_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)USB_VBUS_EN_GPIO, 1);   // enable 5V to device
    }

#if !PQR_USB_SELFTEST
    wifi_start();
#endif

    s_dev_ready = xSemaphoreCreateBinary();
    usb_host_config_t host_cfg = {};
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    VCP::register_driver<FT23x>();          // stock FTDI PIDs (0x6001/0x6015)
#if D50_USB_PID
    VCP::register_driver<D50Ftdi>();        // the D50's actual PID
#endif

#if PQR_USB_SELFTEST
    usb_selftest();   // never returns; diagnostic only
#endif

    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi up");
    sntp_start();

    open_d50();
    xSemaphoreTake(s_dev_ready, pdMS_TO_TICKS(10000));

    // Sync the D50 RTC first so its timestamps match our (NTP) clock, then
    // prime watermarks from the current logs so we don't backfill old samples
    // (Grafana Cloud/Mimir rejects samples older than ~1h).
    d50_reset();
    fetch_unit_id();          // tag metrics with the D50's own ID
    d50_set_clock();
#if D50_SET_THRESHOLDS
    d50_apply_thresholds();   // one-shot: write configured surge/sag thresholds
#endif
    int64_t sample_wm = 0;
    process_datalog(&sample_wm, false);   // prime voltage watermark (no backfill)
    ESP_LOGI(TAG, "primed: sample_wm=%lld", (long long)sample_wm);

    int64_t last_clock_sync = esp_timer_get_time();
    int64_t last_clear = esp_timer_get_time();
    int stall_polls = 0;

    while (true) {
        if (!s_vcp) { open_d50(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        d50_reset();
        int newsamples = process_datalog(&sample_wm, true);   // voltage -> Prometheus
        push_events_loki();                   // disturbances -> Loki (impulse/sag series)

#if D50_AUTOCLEAR
        // Clear the device log (after the above pushes) so it keeps logging:
        //  - primary: every D50_CLEAR_HOURS, well before the ~3h fill
        //  - backstop: if it stalls (0 new samples) for D50_AUTOCLEAR_POLLS
        stall_polls = (newsamples == 0) ? stall_polls + 1 : 0;
        bool clear_due =
            (esp_timer_get_time() - last_clear)
                >= (int64_t)D50_CLEAR_HOURS * 3600 * 1000000LL ||
            stall_polls >= D50_AUTOCLEAR_POLLS;
        if (clear_due) {
            d50_clear_data();
            sample_wm = 0;            // log emptied; accept fresh samples
            stall_polls = 0;
            last_clear = esp_timer_get_time();
        }
#endif

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
