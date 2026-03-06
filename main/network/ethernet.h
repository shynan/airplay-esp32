#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize W5500 Ethernet interface.
 * Requires the board's SPI bus to already be initialized.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not configured
 */
esp_err_t ethernet_init(void);

/**
 * Check if ethernet has link and an IP address.
 */
bool ethernet_is_connected(void);

/**
 * Check if ethernet link layer is up (cable connected).
 */
bool ethernet_is_link_up(void);

/**
 * Get current ethernet IP address as string.
 * @param ip_str Output buffer
 * @param len Buffer size
 * @return ESP_OK on success
 */
esp_err_t ethernet_get_ip_str(char *ip_str, size_t len);

/**
 * Get the ethernet MAC address as a string (XX:XX:XX:XX:XX:XX)
 */
void ethernet_get_mac_str(char *mac_str, size_t len);
