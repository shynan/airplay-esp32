#include "ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include <sys/param.h>

static const char *TAG = "ota";

esp_err_t ota_manager_init(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;

  if (running == NULL) {
    ESP_LOGW(TAG, "No running partition found");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Running from partition: %s", running->label);

  // Check if we need to mark firmware as valid (rollback protection)
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      ESP_LOGI(TAG, "First boot after OTA, marking firmware as valid");
      return ota_mark_valid();
    }
  }

  return ESP_OK;
}

esp_err_t ota_mark_valid(void) {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Firmware marked as valid");
  } else {
    ESP_LOGW(TAG, "Failed to mark firmware as valid: %s", esp_err_to_name(err));
  }
  return err;
}

const char *ota_get_running_partition(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running ? running->label : "unknown";
}

bool ota_is_pending_verify(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;

  if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    return ota_state == ESP_OTA_IMG_PENDING_VERIFY;
  }
  return false;
}

esp_err_t ota_start_from_http(httpd_req_t *req) {
  const esp_partition_t *ota_partition =
      esp_ota_get_next_update_partition(NULL);
  if (!ota_partition) {
    ESP_LOGE(TAG, "No OTA partition found");
    return ESP_ERR_NOT_FOUND;
  }

  esp_ota_handle_t ota_handle;
  esp_err_t err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    return err;
  }

  char buf[1024];
  size_t remaining = req->content_len;
  ESP_LOGI(TAG, "Receiving firmware (%zu bytes)...", remaining);

  while (remaining > 0) {
    int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

    if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    } else if (recv_len <= 0) {
      ESP_LOGE(TAG, "Receive error: %d", recv_len);
      esp_ota_abort(ota_handle);
      return ESP_FAIL;
    }

    if (esp_ota_write(ota_handle, buf, recv_len) != ESP_OK) {
      ESP_LOGE(TAG, "Flash write failed");
      esp_ota_abort(ota_handle);
      return ESP_FAIL;
    }

    remaining -= recv_len;
  }

  if (esp_ota_end(ota_handle) != ESP_OK) {
    ESP_LOGE(TAG, "Image validation failed");
    return ESP_FAIL;
  }

  if (esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set boot partition");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA update successful");
  return ESP_OK;
}
