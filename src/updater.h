#pragma once

#define UPDATE_DIR  "/pico2-apps"
#define UPDATE_FILE "picocalc_file_manager.uf2"

#include <stdbool.h>

/* Interactive: connects to Wi-Fi, checks GitHub releases, offers to download.
 * wifi_setup is called when Wi-Fi is not configured or fails to connect; it
 * should return true only if it left Wi-Fi connected. */
void updater_check(bool (*wifi_setup)(void));
