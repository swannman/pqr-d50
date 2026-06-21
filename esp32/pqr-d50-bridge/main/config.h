// Edit these for your network + Grafana Cloud, then build/flash.
// (For real deployments prefer NVS/menuconfig over hard-coding secrets.)
#ifndef CONFIG_H
#define CONFIG_H

// ---- WiFi ----
#define WIFI_SSID       "your-ssid"
#define WIFI_PASS       "your-password"

// ---- Grafana Cloud (InfluxDB line protocol push) ----
// Find these in Grafana Cloud -> your stack -> InfluxDB "Send Metrics":
//   URL pattern:  https://influx-prod-XX-region.grafana.net/api/v1/push/influx/write
//   User: your numeric instance ID    Token: a Cloud Access Policy token (write)
// Auth is HTTP Basic (user:token).
#define INFLUX_URL      "https://influx-prod-XX-region.grafana.net/api/v1/push/influx/write"
#define INFLUX_USER     "123456"
#define INFLUX_TOKEN    "glc_xxxxxxxxxxxxxxxxxxxxxxxx"

#define INFLUX_MEASUREMENT "pqr_d50"

// ---- D50 link ----
#define D50_BAUD        19200      // D50 V20.55 default
#define D50_POLL_SEC    60         // how often to pull the data log

// ---- USB ----
// Optional: a GPIO that enables 5V VBUS to the device port via a load switch.
// Set to -1 if your board always provides VBUS on the native port.
#define USB_VBUS_EN_GPIO  (-1)

#endif
