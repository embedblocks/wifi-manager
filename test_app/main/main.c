/* main.c — wifi_manager test application entry point
 *
 * Initialises the minimum ESP-IDF subsystems required by wifi_manager and
 * its stubs, then hands off to the Unity interactive test menu.
 *
 * Flash and run:
 *   idf.py -C test_app build flash monitor
 *
 * In the serial monitor, press ENTER to list tests, or send a test name to
 * run a specific case. Prefix with '*' to run all tests.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "unity.h"

/* Pull in test suites via Unity's test-registration macros.
 * Each test file calls TEST_CASE() which registers the case with the
 * Unity runner automatically when this app is built. */
#include "test_wifi_mgr_lifecycle.h"
#include "test_wifi_mgr_scan.h"
#include "test_wifi_mgr_connect.h"
#include "test_wifi_mgr_prov.h"
#include "test_wifi_mgr_state_machine.h"

static const char *TAG = "TEST_APP";

static void nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    ESP_LOGI(TAG, "wifi_manager test application starting");

    /* --- Subsystem init (must run before any test case) --- */
    nvs_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* --- Unity interactive menu --- */
    ESP_LOGI(TAG, "Starting Unity test runner");
    unity_run_menu();

    /* unity_run_menu() returns only when the user sends 'q'.
     * Tear down to allow a clean restart without power cycling. */
    ESP_ERROR_CHECK(esp_event_loop_delete_default());
    ESP_LOGI(TAG, "Test run complete");
}
