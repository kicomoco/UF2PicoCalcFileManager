#pragma once

#define UPDATE_DIR  "/pico2-apps"
#define UPDATE_FILE "picocalc_file_manager.uf2"

/* Interactive: connects to Wi-Fi, checks GitHub releases, offers to download. */
void updater_check(void);
