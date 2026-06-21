// Non-secret configuration. Secrets (WiFi + Grafana) live in secrets.h
// (gitignored) — copy secrets.h.example to secrets.h and fill it in.
#ifndef CONFIG_H
#define CONFIG_H

#include "secrets.h"   // WIFI_SSID/WIFI_PASS, INFLUX_URL/INFLUX_USER/INFLUX_TOKEN

// ---- Diagnostics ----
// 1 = USB host self-test: open the D50 and log identity/datalog over the COM
// console in a loop, skipping WiFi/Grafana. 0 = normal operation.
#define PQR_USB_SELFTEST 0

// ---- Grafana measurements ----
#define INFLUX_MEASUREMENT       "pqr_d50"        // -> metrics pqr_d50_hot/_neu
#define INFLUX_EVENT_MEASUREMENT "pqr_d50_event"  // -> pqr_d50_event_magnitude

// ---- Time ----
#define NTP_SERVER      "pool.ntp.org"
#define POSIX_TZ        "PST8PDT,M3.2.0,M11.1.0"   // Seattle / US Pacific (DST-aware)
#define CLOCK_SYNC_HOURS 24                        // re-set the D50 RTC this often

// ---- D50 link ----
// The D50 is a standard FT232R (VID 0x0403, PID 0x6001) — already supported by
// the stock FTDI driver. Leave 0 unless a unit reports a custom PID.
#define D50_USB_PID     0x0000
#define D50_BAUD        19200      // D50 V20.55 default
#define D50_POLL_SEC    60         // how often to pull log + events

// ---- One-shot disturbance thresholds (C6 option 4), applied on boot ----
// 1 = write these once at startup then run normally; 0 = don't touch them.
// Order is the device's: CH1=Hot, CH2=Neutral, each Surge/Sag/PowerFail.
// Value: volts to trip at | 0 = device default (5%,10%) | -1 = keep current.
// Set to 1 to (re-)apply; device keeps them in battery-backed RAM, so 0 normally.
#define D50_SET_THRESHOLDS 0
#define TH_CH1_SURGE  123   // Hot: surge trips above 123 V
#define TH_CH1_SAG    115   // Hot: sag trips below 115 V
#define TH_CH1_PFAIL  (-1)  // keep
#define TH_CH2_SURGE  (-1)  // neutral channel: keep
#define TH_CH2_SAG    (-1)
#define TH_CH2_PFAIL  (-1)

// ---- USB ----
// Optional GPIO to enable 5V VBUS via a load switch. This board instead uses a
// "USB-OTG" solder jumper to route 5V to the host port, so leave -1.
#define USB_VBUS_EN_GPIO  (-1)

#endif
