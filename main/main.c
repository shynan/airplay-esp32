#include "audio_output.h"
#include "audio_receiver.h"
#include "display.h"
#include "dns_server.h"
#include "ethernet.h"
#include "led.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "nvs_flash.h"
#include "ota.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "settings.h"
#include "web_server.h"
#include "wifi.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#include "rtsp_events.h"
#endif

#include "iot_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// AP mode IP address (192.168.4.1 in network byte order)
#define AP_IP_ADDR 0x0104A8C0

static bool s_airplay_started = false;
static bool s_airplay_infrastructure_ready = false;

static void start_airplay_services(void) {
  if (s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Starting AirPlay services...");

  // One-time infrastructure init (PTP, HAP, audio receiver/output)
  if (!s_airplay_infrastructure_ready) {
    esp_err_t err = ptp_clock_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to init PTP clock: %s", esp_err_to_name(err));
      s_airplay_started = false;
      return;
    }

    ESP_ERROR_CHECK(hap_init());
    ESP_ERROR_CHECK(audio_receiver_init());
    ESP_ERROR_CHECK(audio_output_init());
    mdns_airplay_init();
    s_airplay_infrastructure_ready = true;
  } else {
    // Infrastructure already initialized - flush and reset for clean restart
    // This is needed when switching from Bluetooth back to AirPlay
    ESP_LOGI(TAG, "Flushing audio buffers for AirPlay restart");
    audio_receiver_flush();
  }

  audio_output_start();

  ESP_ERROR_CHECK(rtsp_server_start());

  s_airplay_started = true;
  ESP_LOGI(TAG, "AirPlay ready");
}
#ifdef CONFIG_BT_A2DP_ENABLE
static void stop_airplay_services(void) {
  if (!s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Stopping AirPlay services...");

  rtsp_server_stop();
  audio_output_stop();

  s_airplay_started = false;
  ESP_LOGI(TAG, "AirPlay stopped");
}
#endif

static void network_monitor_task(void *pvParameters) {
  (void)pvParameters;
  bool had_network = ethernet_is_connected() || wifi_is_connected();
  bool dns_running = !had_network;
  bool wifi_started = wifi_is_connected() || !ethernet_is_connected();
  bool had_eth = ethernet_is_connected();

  // Start captive portal DNS if no network yet
  if (dns_running) {
    dns_server_start(AP_IP_ADDR);
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool eth_up = ethernet_is_connected();
    bool wifi_up = wifi_is_connected();
    bool has_network = eth_up || wifi_up;

    // Ethernet just came up — stop WiFi entirely
    if (eth_up && !had_eth && wifi_started) {
      ESP_LOGI(TAG, "Ethernet connected — stopping WiFi");
      wifi_stop();
      wifi_started = false;
      wifi_up = false;
    }

    // Ethernet dropped — bring up WiFi (AP + STA)
    if (!eth_up && had_eth) {
      ESP_LOGI(TAG, "Ethernet down — starting WiFi as fallback");
      wifi_init_apsta(NULL, NULL);
      wifi_started = true;
    }

    had_eth = eth_up;
    has_network = eth_up || wifi_is_connected();

    if (has_network == had_network) {
      continue;
    }

    if (has_network) {
      ESP_LOGI(TAG, "Network up (eth=%s, wifi=%s)", eth_up ? "yes" : "no",
               wifi_up ? "yes" : "no");
      start_airplay_services();
      if (dns_running) {
        dns_server_stop();
        dns_running = false;
      }
    } else {
      if (!dns_running) {
        dns_server_start(AP_IP_ADDR);
        dns_running = true;
      }
    }

    had_network = has_network;
  }
}

#ifdef CONFIG_BT_A2DP_ENABLE
static bool s_wifi_was_connected = false;  // Track WiFi state before BT

static void on_bt_state_changed(bool connected) {
  if (connected) {
    ESP_LOGI(TAG, "BT connected — stopping WiFi and AirPlay for clean coexistence");
    // Stop AirPlay services
    stop_airplay_services();
    // Stop WiFi to eliminate RF interference (BT and WiFi share same radio)
    s_wifi_was_connected = wifi_is_connected();
    if (s_wifi_was_connected) {
      ESP_LOGI(TAG, "Stopping WiFi to eliminate BT/WiFi interference");
      wifi_stop();
    }
  } else {
    ESP_LOGI(TAG, "BT disconnected — restarting WiFi and AirPlay");
    // Re-enable BT controller if it was disabled
    if (!bt_a2dp_sink_is_enabled()) {
      bt_a2dp_sink_enable();
    }
    // Restart WiFi if it was running before
    if (s_wifi_was_connected && !ethernet_is_connected()) {
      ESP_LOGI(TAG, "Restarting WiFi");
      wifi_init_apsta(NULL, NULL);
      // Wait for WiFi to reconnect
      if (settings_has_wifi_credentials()) {
        wifi_wait_connected(10000);
      }
    }
    // Re-enable AirPlay
    if (ethernet_is_connected() || wifi_is_connected()) {
      start_airplay_services();
      // Refresh mDNS after WiFi reconnects so iOS can discover the device
      mdns_airplay_refresh();
    }
    // Make BT discoverable again
    bt_a2dp_sink_set_discoverable(true);
  }
}

static void on_airplay_client_event(rtsp_event_t event,
                                    const rtsp_event_data_t *data,
                                    void *user_data) {
  (void)data;
  (void)user_data;
  if (bt_a2dp_sink_is_connected()) {
    ESP_LOGI(TAG, "BT connected, skipping AirPlay event handling");
    return;
  }
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    ESP_LOGI(TAG, "AirPlay client connected — disabling BT controller");
    // Disable BT controller completely to eliminate RF interference
    if (bt_a2dp_sink_is_enabled()) {
      bt_a2dp_sink_disable();
    } else {
      ESP_LOGI(TAG, "BT controller already disabled");
    }
    break;
  case RTSP_EVENT_DISCONNECTED:
    // This event may fire multiple times (once per client slot)
    // Only re-enable BT once
    if (!bt_a2dp_sink_is_enabled()) {
      ESP_LOGI(TAG, "AirPlay client disconnected — re-enabling BT controller");
      bt_a2dp_sink_enable();
    } else {
      ESP_LOGI(TAG, "AirPlay client disconnected — BT already enabled");
    }
    bt_a2dp_sink_set_discoverable(true);
    break;
  default:
    break;
  }
}
#endif

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(settings_init());
  ota_manager_init();  // Mark firmware as valid if rollback protection enabled
  led_init();
  display_init();

  // Initialize board-specific hardware (includes SPI bus for ethernet)
  ESP_LOGI(TAG, "Board: %s", iot_board_get_info());
  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
  }

  // Try ethernet first
#ifndef CONFIG_BT_ONLY_MODE
  bool eth_available = false;
  err = ethernet_init();
  if (err == ESP_OK) {
    // Wait for ethernet link + DHCP (up to 5s for link, then 10s more for DHCP)
    ESP_LOGI(TAG, "Waiting for ethernet...");
    for (int i = 0; i < 25 && !ethernet_is_link_up(); i++) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ethernet_is_link_up() && !ethernet_is_connected()) {
      ESP_LOGI(TAG, "Ethernet link up, waiting for DHCP...");
      for (int i = 0; i < 50 && !ethernet_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
    eth_available = ethernet_is_connected();
    if (eth_available) {
      ESP_LOGI(TAG, "Ethernet connected");
    } else {
      ESP_LOGI(TAG, "Ethernet not connected (cable?), will use WiFi");
    }
  } else if (err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
  }

  // Start WiFi only if ethernet is not available
  if (!eth_available) {
    wifi_init_apsta(NULL, NULL);

    // Wait for initial WiFi connection if credentials exist
    if (settings_has_wifi_credentials()) {
      if (!wifi_wait_connected(30000)) {
        ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
      }
    } else {
      ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
    }
  } else {
    ESP_LOGI(TAG, "Ethernet connected — skipping WiFi");
  }
#endif

  // Start services that work on any interface
#ifndef CONFIG_BT_ONLY_MODE
  web_server_start(80);
  xTaskCreate(network_monitor_task, "net_mon", 4096, NULL, 5, NULL);
#endif

#ifndef CONFIG_BT_ONLY_MODE
  bool connected = eth_available || wifi_is_connected();
  if (connected) {
    start_airplay_services();
  }
#else
  ESP_LOGI(TAG, "Bluetooth-only mode - AirPlay disabled");
#endif

#ifdef CONFIG_BT_A2DP_ENABLE
  // Initialize Bluetooth A2DP Sink
  {
    char bt_name[65];
    settings_get_device_name(bt_name, sizeof(bt_name));
    esp_err_t bt_err = bt_a2dp_sink_init(bt_name, on_bt_state_changed);
    if (bt_err != ESP_OK) {
      ESP_LOGE(TAG, "BT A2DP init failed: %s", esp_err_to_name(bt_err));
    } else {
#ifdef CONFIG_BT_ONLY_MODE
      // In BT-only mode, initialize audio output (I2S) for Bluetooth
      // Do NOT start playback task - BT has its own I2S writer task
      ESP_LOGI(TAG, "Initializing audio output for Bluetooth-only mode");
      audio_output_init();
#else
      rtsp_events_register(on_airplay_client_event, NULL);
#endif
    }
  }
#endif

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
