#pragma once
#include <stdbool.h>
#include "util.h"

#define CONFIG_PATH "/picocalc_file_manager.cfg"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[80];
    bool split_view;
    int sort_mode;
    bool show_hidden;
    char path_a[MAX_PATH];
    char path_b[MAX_PATH];
} config_t;

extern config_t g_cfg;

void config_load(void);
bool config_save(void);
