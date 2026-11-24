/* Host test of settings.ini's online keys (runtime/src/settings.c): net_dns and net_server written and read back,
 * net_server saved empty when it is the default and read back as the default. Usage: run_net_native_test.sh */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "runtime.h"
#include "settings.h"

static char g_path[256];
const char* platform_settings_path(void) { return g_path; }
void platform_set_cpu_clock(int mhz) { (void)mhz; }
void platform_set_handheld_gpu(int on) { (void)on; }
void game_mods_apply(void) {}
void rt_log(const char* fmt, ...) { (void)fmt; }

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int file_has(const char* line) {
    char b[4096] = {0};
    FILE* f = fopen(g_path, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strstr(b, line) != NULL;
}

int main(void) {
    snprintf(g_path, sizeof g_path, "build/tests/net/settings.ini");
    remove(g_path);
    CHECK(!strcmp(g_settings.net_server, SETTINGS_NET_SERVER_DEFAULT) && !g_settings.net_dns[0], "defaults");
    settings_apply();
    CHECK(file_has("\nnet_server=\n") && file_has("\nnet_dns=\n"), "the default net_server is saved empty");
    snprintf(g_settings.net_server, sizeof g_settings.net_server, "other.example");
    settings_apply();
    snprintf(g_settings.net_server, sizeof g_settings.net_server, "x");
    settings_load();
    CHECK(!strcmp(g_settings.net_server, "other.example"), "net_server read back (%s)", g_settings.net_server);
    snprintf(g_settings.net_server, sizeof g_settings.net_server, "%s", SETTINGS_NET_SERVER_DEFAULT);
    snprintf(g_settings.net_dns, sizeof g_settings.net_dns, "10.0.0.2:5353");
    settings_apply();
    snprintf(g_settings.net_server, sizeof g_settings.net_server, "x");
    g_settings.net_dns[0] = 0;
    settings_load();
    CHECK(!strcmp(g_settings.net_server, SETTINGS_NET_SERVER_DEFAULT) && !strcmp(g_settings.net_dns, "10.0.0.2:5353"),
          "empty net_server read as the default, net_dns kept (%s, %s)", g_settings.net_server, g_settings.net_dns);
    printf("settings_net_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
