#pragma once

/**
 * Initialize mDNS and advertise AirPlay 2 services
 *
 * This publishes:
 * - _airplay._tcp service (AirPlay 2)
 * - _raop._tcp service (Remote Audio Output Protocol)
 *
 * With all required TXT records for iOS to recognize the device
 */
void mdns_airplay_init(void);

/**
 * Refresh mDNS services after network reconnection
 *
 * Call this after WiFi reconnects to ensure mDNS records are up-to-date
 */
void mdns_airplay_refresh(void);
