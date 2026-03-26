#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Perform OTA update from HTTP POST request containing raw firmware binary.
 * Does not restart - caller should send response then call esp_restart().
 *
 * @param req HTTP request with firmware in body
 * @return ESP_OK on success
 */
esp_err_t ota_start_from_http(httpd_req_t *req);

/**
 * Initialize OTA manager.
 * Marks current firmware as valid if rollback protection is enabled.
 * Should be called during app startup.
 */
esp_err_t ota_manager_init(void);

/**
 * Mark current firmware as valid.
 * Required when rollback protection is enabled.
 * Call after successful boot to confirm the new firmware works.
 */
esp_err_t ota_mark_valid(void);

/**
 * Get current running partition label.
 * Returns "ota_0" or "ota_1".
 */
const char *ota_get_running_partition(void);

/**
 * Check if this is the first boot after OTA update.
 * When rollback protection is enabled, firmware starts in "pending verify" state.
 */
bool ota_is_pending_verify(void);
