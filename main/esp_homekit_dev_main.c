#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_event_base.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include <esp_http_server.h>

#include <nvs_flash.h>
#include "nvs_flash.h"

#include <sys/param.h>

#include <driver/gpio.h>

#define APP_WIFI_SSIS "WIFI_SSIS"
#define APP_WIFI_PASSWORD "WIFI_PASSWORD"

#define APP_EVENT 0x101

#define APP_EVENT_SWITCH_ON 0x011
#define APP_EVENT_SWITCH_OFF 0x012

#define SWITCH_ON_BIT 0x001
#define WIFI_CONNECTED_BIT 0x002
#define WIFI_FAILED_BIT 0x003

#define GREEN_LED_PIN 14
#define RED_LED_PIN 32
#define LED_PIN 15

#define LED_ACTIVE_LOW 1 // Inverts the output level if active low

static const char *TAG = "ESP32 Homekit Dev";

static esp_event_loop_handle_t app_event_loop_handle;

EventGroupHandle_t app_event_group;

/**
 * Event Handler for default loop
 */
static void event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t *) event_data;

    ESP_LOGI(TAG, "WiFi got IP" IPSTR, IP2STR(&event -> ip_info.ip));

    xEventGroupSetBits(app_event_group, WIFI_CONNECTED_BIT);
  }
}

/**
 * Event Handler for app events
 */
static void app_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
  if (base == APP_EVENT && id == APP_EVENT_SWITCH_ON) {
    xEventGroupSetBits(app_event_group, SWITCH_ON_BIT);
    gpio_set_level(LED_PIN, LED_ACTIVE_LOW ? 0 : 1);
  } else if (base == APP_EVENT && id == APP_EVENT_SWITCH_OFF) {
    xEventGroupClearBits(app_event_group, SWITCH_ON_BIT);
    gpio_set_level(LED_PIN, LED_ACTIVE_LOW ? 1 : 0);
  }
}

/**
 * Get the current status of the switch
 */
static esp_err_t switch_status_get_handler(httpd_req_t *req)
{
  EventBits_t bits = xEventGroupGetBits(app_event_group);
  httpd_resp_send(req, bits & SWITCH_ON_BIT ? "1" : "0", HTTPD_RESP_USE_STRLEN);

  return ESP_OK;
}

static const httpd_uri_t switch_get_status_uri = {
  .uri = "/status",
  .method = HTTP_GET,
  .handler = switch_status_get_handler,
  .user_ctx = "HomeKit Dev Main"
};

/**
 * Turn on the switch
 */
static esp_err_t switch_post_on_handler(httpd_req_t *req)
{
  esp_event_post_to(app_event_loop_handle, APP_EVENT, APP_EVENT_SWITCH_ON, NULL, NULL, 500 / portTICK_PERIOD_MS);
  httpd_resp_send(req, "1", HTTPD_RESP_USE_STRLEN);

  return ESP_OK;
}
static const httpd_uri_t switch_post_on_uri = {
  .uri = "/on",
  .method = HTTP_GET,
  .handler = switch_post_on_handler,
  .user_ctx = "HomeKit Dev Main"
};

/**
 * Turn off the switch
 */
static esp_err_t switch_post_off_handler(httpd_req_t *req)
{
  esp_event_post_to(app_event_loop_handle, APP_EVENT, APP_EVENT_SWITCH_OFF, NULL, NULL, 500 / portTICK_PERIOD_MS);
  httpd_resp_send(req, "0", HTTPD_RESP_USE_STRLEN);

  return ESP_OK;
}
static const httpd_uri_t switch_post_off_uri = {
  .uri = "/off",
  .method = HTTP_GET,
  .handler = switch_post_off_handler,
  .user_ctx = "HomeKit Dev Main"
};

/**
 * Initialize webserver and register handlers
 */
static httpd_handle_t webserver_init(void)
{
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  ESP_LOGI(TAG, "Starting server...");

  if (httpd_start(&server, &config) == ESP_OK) {
    ESP_LOGI(TAG, "Registering endpoint handlers...");

    httpd_register_uri_handler(server, &switch_get_status_uri);
    httpd_register_uri_handler(server, &switch_post_off_uri);
    httpd_register_uri_handler(server, &switch_post_on_uri);
    
    return server;
  }

  ESP_LOGI(TAG, "Error starting server");
  return NULL;
}

static void webserver_stop(httpd_handle_t server)
{
  httpd_stop(server);
}

/**
 * Initialize wireless network connection
 */
void wifi_init(void)
{
  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    ESP_EVENT_ANY_ID,
    &event_handler,
    NULL,
    &instance_any_id
  ));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    IP_EVENT,
    IP_EVENT_STA_GOT_IP,
    &event_handler,
    NULL,
    &instance_got_ip
  ));

  // Wifi config
  wifi_config_t wifi_config = {
    .sta = {
      .ssid = APP_WIFI_SSIS,
      .password = APP_WIFI_PASSWORD,
    },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "WiFi initialization finished");
}

/**
 * Initialize user event loop
 */
void user_event_loop_init(void)
{
  esp_event_loop_args_t app_event_loop_args = {
    .queue_size = 5,
    .task_name = "app_evt",
    .task_stack_size = 2048,
    .task_priority = uxTaskPriorityGet(NULL), // Priority of the calling task
    .task_core_id = tskNO_AFFINITY
  };

  esp_event_loop_create(&app_event_loop_args, &app_event_loop_handle);

  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
    app_event_loop_handle,
    APP_EVENT,
    APP_EVENT_SWITCH_ON,
    &app_event_handler,
    NULL,
    NULL
  ));

  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
    app_event_loop_handle,
    APP_EVENT,
    APP_EVENT_SWITCH_OFF,
    &app_event_handler,
    NULL,
    NULL
  ));
}

void app_main(void)
{
  //static httpd_handle_t server = NULL;
  app_event_group = xEventGroupCreate();

  gpio_reset_pin(GREEN_LED_PIN);
  gpio_set_direction(GREEN_LED_PIN, GPIO_MODE_OUTPUT);

  gpio_reset_pin(RED_LED_PIN);
  gpio_set_direction(RED_LED_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(RED_LED_PIN, 1);

  gpio_reset_pin(LED_PIN);
  gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  
  // Connect to network
  wifi_init();

  // Once network is connected, initialize endpoints
  EventBits_t eventBits = xEventGroupWaitBits(
    app_event_group,
    WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
    pdFALSE,
    pdFALSE,
    portMAX_DELAY
  );

  if (eventBits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "EventBit WiFi Connected");
    gpio_set_level(GREEN_LED_PIN, 1);
    gpio_set_level(RED_LED_PIN, 0);
  }

  webserver_init();

  user_event_loop_init();
}
