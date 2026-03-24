/**
 * @file board.c
 * @brief ESP32 Generic board implementation
 *
 * Minimal implementation for generic ESP32 dev boards with external I2S DAC.
 * Supports TAS58xx DAC when CONFIG_DAC_TAS58XX is enabled.
 */

#include "iot_board.h"

#include "dac.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtsp_events.h"
#include "settings.h"

#ifdef CONFIG_DAC_TAS58XX
#include "dac_tas58xx.h"
#endif

static const char TAG[] = "ESP32-Generic";

static bool s_board_initialized = false;
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
  case BOARD_I2C0_ID:
    return (board_res_handle_t)s_i2c_bus_handle;
  default:
    return NULL;
  }
}

static esp_err_t init_mute_gpio(void) {
#if BOARD_MUTE_GPIO >= 0
  // Configure mute/pdn GPIO - start with DAC in reset (low)
  gpio_config_t mute_cfg = {
      .pin_bit_mask = (1ULL << BOARD_MUTE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&mute_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure mute GPIO: %s", esp_err_to_name(err));
    return err;
  }

  // Set initial state - unmute (high for PDN active-high, or adjust logic)
  // PDN pin: low = power down, high = normal operation
  gpio_set_level(BOARD_MUTE_GPIO, 1);

  ESP_LOGI(TAG, "Mute/PDN GPIO initialized on GPIO %d", BOARD_MUTE_GPIO);
#endif
  return ESP_OK;
}

#ifdef CONFIG_DAC_TAS58XX
static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PAUSED:
    dac_set_power_mode(DAC_POWER_STANDBY);
    break;
  case RTSP_EVENT_PLAYING:
    dac_set_power_mode(DAC_POWER_ON);
    break;
  case RTSP_EVENT_DISCONNECTED:
    dac_set_power_mode(DAC_POWER_OFF);
    break;
  case RTSP_EVENT_METADATA:
    break;
  }
}
#endif

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

#ifdef CONFIG_DAC_TAS58XX
  // Initialize mute/PDN GPIO first (pull high to enable DAC)
  esp_err_t err = init_mute_gpio();
  if (err != ESP_OK) {
    return err;
  }

  // Wait for DAC to power up (TAS5825 needs ~10ms after PDN high)
  vTaskDelay(pdMS_TO_TICKS(20));

  // Register and initialize TAS58xx DAC
  dac_register(&dac_tas58xx_ops);

  // Initialize I2C bus for DAC control
  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = BOARD_I2C_PORT,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  err = i2c_new_master_bus(&i2c_cfg, &s_i2c_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "I2C bus %d initialized: sda=%d, scl=%d", BOARD_I2C_PORT,
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);

  err = dac_init(s_i2c_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize DAC: %s", esp_err_to_name(err));
    return err;
  }

  // Register for RTSP events to control DAC power
  rtsp_events_register(on_rtsp_event, NULL);

  // Start in standby
  dac_set_power_mode(DAC_POWER_OFF);

  // Restore saved volume
  float vol_db;
  if (ESP_OK == settings_get_volume(&vol_db)) {
    dac_set_volume(vol_db);
  }

  ESP_LOGI(TAG, "TAS58xx DAC initialized");
#endif

  s_board_initialized = true;
  ESP_LOGI(TAG, "Generic board initialized");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  if (!s_board_initialized) {
    return ESP_OK;
  }

#ifdef CONFIG_DAC_TAS58XX
  rtsp_events_unregister(on_rtsp_event);
  dac_set_power_mode(DAC_POWER_OFF);

#if BOARD_MUTE_GPIO >= 0
  // Power down DAC
  gpio_set_level(BOARD_MUTE_GPIO, 0);
#endif

  if (s_i2c_bus_handle != NULL) {
    i2c_del_master_bus(s_i2c_bus_handle);
    s_i2c_bus_handle = NULL;
  }
#endif

  s_board_initialized = false;
  return ESP_OK;
}