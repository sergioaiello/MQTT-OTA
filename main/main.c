#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "protocol_examples_common.h"
#include "cJSON.h"
#include "connect.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
// #include "wifi_provisioning/wifi_config.h"
// #include "wifi_provisioning/manager.h"

#define TAG1 "OTA"
#define TAG2 "MQTT"

xSemaphoreHandle mutexHandle;

// ********* OTA ************
xSemaphoreHandle ota_semaphore;

// ********* MQTT ************
xQueueHandle readingQueue;
TaskHandle_t taskHandle;
TaskHandle_t taskOTAHandle;
extern int ota_incourse = 0;
int reading_shot = 0;


const uint32_t WIFI_CONNECTED = BIT1;
const uint32_t MQTT_CONNECTED = BIT2;
const uint32_t MQTT_PUBLISHED = BIT3;

// ********* OTA ************
extern const uint8_t server_cert_pem_start[] asm("_binary_google_cer_start");

esp_err_t  client_event_handler(esp_http_client_event_t *evt)
{
  return ESP_OK;
}

// ********* MQTT ************
void mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
  switch (event->event_id)
  {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG2, "MQTT_EVENT_CONNECTED");
    xTaskNotify(taskHandle, MQTT_CONNECTED, eSetValueWithOverwrite);
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG2, "MQTT_EVENT_DISCONNECTED");
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG2, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(TAG2, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(TAG2, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    xTaskNotify(taskHandle, MQTT_PUBLISHED, eSetValueWithOverwrite);
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG2, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG2, "MQTT_EVENT_ERROR");
    break;
  default:
    ESP_LOGI(TAG2, "Other event id:%d", event->event_id);
    break;
  }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  mqtt_event_handler_cb(event_data);
}

// ********* OTA ************
esp_err_t validate_image_header(esp_app_desc_t *incoming_ota_desc)
{
  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  esp_app_desc_t running_partition_description;
  esp_ota_get_partition_description(running_partition, &running_partition_description);

  ESP_LOGI(TAG1, "current version is %s\n", running_partition_description.version);
  ESP_LOGI(TAG1, "new version is %s\n", incoming_ota_desc->version);

  if (strcmp(running_partition_description.version, incoming_ota_desc->version) == 0)
  {
    ESP_LOGW(TAG1, "NEW VERSION IS THE SAME AS CURRENT VERSION. ABORTING");
    return ESP_FAIL;
  }
  return ESP_OK;
}

void OTALogic()
{
  uint32_t command = 0;
  while (true)
  {
    if (ota_incourse == 1 )
    {
      xTaskNotifyWait(0, 0, &command, portMAX_DELAY);
      switch (command)
      {
      case WIFI_CONNECTED:
        while (true)
        {
          ESP_LOGI(TAG1, "Invoking OTA");

          esp_http_client_config_t clientConfig = {
            .url = "https://drive.google.com/u/0/uc?id=1zqMt6QZwwLAWnsK19ZY9C-wH93NBZHd_&export=download",
            .event_handler = client_event_handler,
            .cert_pem = (char *)server_cert_pem_start};
      
          esp_https_ota_config_t ota_config = {
            .http_config = &clientConfig};

          esp_https_ota_handle_t ota_handle = NULL;
  
          if (esp_https_ota_begin(&ota_config, &ota_handle) != ESP_OK)
          {
            ESP_LOGE(TAG1, "esp_https_ota_begin failed");
            continue;
          }

          esp_app_desc_t incoming_ota_desc;
          if (esp_https_ota_get_img_desc(ota_handle, &incoming_ota_desc) != ESP_OK)
          {
            ESP_LOGE(TAG1, "esp_https_ota_get_img_desc failed");
            esp_https_ota_finish(ota_handle);
            ota_incourse = 0;
            esp_wifi_stop();
            xSemaphoreGive(ota_semaphore);
            continue;
          }
          if (validate_image_header(&incoming_ota_desc) != ESP_OK)
          {
            ESP_LOGE(TAG1, "validate_image_header failed");
            esp_https_ota_finish(ota_handle);
            ota_incourse = 0;
            esp_wifi_stop();
            xSemaphoreGive(ota_semaphore);
            continue;
          }

          while (true)
          {
            esp_err_t ota_result = esp_https_ota_perform(ota_handle);
            if (ota_result != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
              break;
          }

          if (esp_https_ota_finish(ota_handle) != ESP_OK)
          {
            ESP_LOGE(TAG1, "esp_https_ota_finish failed");
            ota_incourse = 0;
            esp_wifi_stop();
            xSemaphoreGive(ota_semaphore);
            continue;
          }
          else
          {
            printf("restarting in 5 seconds\n");
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
          }
          ESP_LOGE(TAG1, "Failed to update firmware");
        }
        break;
      default:
        break;
      }
    }
    else
    {
      printf(" dentro OTA logic dentro else-- ota_incourse=%d\n", ota_incourse);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    
// prova 1 linea 
    printf("OTA logic -- fuori if ota_inservice \n");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ********* MQTT ************
void MQTTLogic(int sensorReading)
{
  uint32_t command = 0;
  esp_mqtt_client_config_t mqttConfig = {
      .uri = "mqtt://mqtt.eclipse.org:1883"};
  esp_mqtt_client_handle_t client = NULL;

  while (true)
  {
    printf("dentro MQTT logic prima if\n");
    // if (xSemaphoreTake(ota_semaphore, portMAX_DELAY) && ota_incourse == 0 )
    if (ota_incourse == 0 )
    {
      printf("dentro MQTT logic dopo if -- ota_incourse=%d\n", ota_incourse);
      xTaskNotifyWait(0, 0, &command, 3000 / portTICK_PERIOD_MS);
      // xTaskNotifyWait(0, 0, &command, portMAX_DELAY);
      switch (command)
      {
      case WIFI_CONNECTED:
        client = esp_mqtt_client_init(&mqttConfig);
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
        printf("MQTT logic - sending data: %d\n", sensorReading);
        esp_mqtt_client_start(client);
        break;
      case MQTT_CONNECTED:
        esp_mqtt_client_subscribe(client, "/pano/mymgnt/general", 2);
        char data[50];
        sprintf(data, "%d", sensorReading);
        printf("MQTT logic - sending data: %d\n", sensorReading);
        esp_mqtt_client_publish(client, "/pano/power/main", data, strlen(data), 2, false);
        break;
      case MQTT_PUBLISHED:
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        printf("MQTT logic prima wifiStop - sending data : %d\n", sensorReading);
        esp_wifi_stop();
        return;
      default:
        break;
      }
    }
    else
    {
      printf(" dentro MQTT logic dentro else-- ota_incourse=%d\n", ota_incourse);
    }
    
// prova 1 linea 
    printf("fuori if ota_inservice MQTT logic\n");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void OnConnected(void *para)
{
  printf("dentro On Connected - prima di while -- ota_incourse = %d \n", ota_incourse);
  if (ota_incourse == 0 )
  {
    int sensorReading;
    while (true)
    {
      printf("dentro On Connected - dentro while prima if xQueueReceive \n");
      if (xQueueReceive(readingQueue, &sensorReading, portMAX_DELAY))
      {
        printf("dentro On Connected - dentro while dentro if xQueueReceive \n");
        ESP_ERROR_CHECK(esp_wifi_start());
        MQTTLogic(sensorReading);
        // xSemaphoreGive(ota_semaphore);
      }
    printf("dentro On Connected - dentro while dopo if xQueueReceive \n");  
    }
  }
}

void OnConnectedOta(void *para)
{
  printf("dentro On ConnectedOta ota_incourse = %d \n", ota_incourse);
  if (ota_incourse == 1 )
  {
    // ESP_ERROR_CHECK(esp_wifi_start());
    printf("dentro On Connected dopo else\n");
    ESP_ERROR_CHECK(esp_wifi_start());
    OTALogic();
    printf("dentro On Connected dopo OTALogic - ota_incourse = %d \n", ota_incourse);
  }
}


void generateReading(void *params)
{
  printf("dentro generatingReading  - prima di while\n");
  while (true)
  {
    if (ota_incourse == 0 )
    {
      printf("dentro generatingReading  - dentro di while prima di if reading_shot: %d\n", reading_shot);
      int random = esp_random();
      if (reading_shot == 0)
      {
        xQueueSend(readingQueue, &random, 2000 / portTICK_PERIOD_MS);
        printf("dentro generatingReading  - dentro di while dopo xQueueSend\n");
        reading_shot = 1;
        printf("dentro generatingReading  - dentro while cambio variabile reading_shot: %d\n", reading_shot);
      }
      else
      {
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }
  }
}

void taskMQTT(void * params)
{
  printf("dentro TaskMQTT prima di while\n");
  while (true)
  {
    printf("dentro TaskMQTT -- while prima if\n");
    if (xSemaphoreTake(ota_semaphore, portMAX_DELAY) && ota_incourse == 0 )
    {
      reading_shot = 0;
      ESP_LOGI(TAG2, "MQTT Taken token");
      printf("dentro TaskMQTT -- while dopo if --> preso token\n");
      readingQueue = xQueueCreate(sizeof(int), 10);
      xTaskCreate(OnConnected, "handle comms", 1024 * 5, NULL, 5, &taskHandle);
      printf("dentro TaskMQTT -- dopo xTask OnConnected\n");
      xTaskCreate(generateReading, "handle comms", 1024 * 5, NULL, 5, NULL);
// 8 seconds is for wifi setup+connection and publish. Needs improvement
      vTaskDelay(8000 / portTICK_PERIOD_MS);
      ota_incourse = 1;
      xSemaphoreGive(ota_semaphore);
   }
    else
    {
      printf("TaskMQTT timed out \n");
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void taskOTA(void * params) 
{
  printf("dentro TaskOTA prima while \n");
  // while (ota_incourse == 1)
  while (true)
  {
    // if (xSemaphoreTake(mutexHandle, 5000 / portTICK_PERIOD_MS))
    if (xSemaphoreTake(ota_semaphore, portMAX_DELAY) && ota_incourse == 1)
    // if (xSemaphoreTake(ota_semaphore, 5000 / portTICK_PERIOD_MS))
    {
      ESP_LOGI(TAG1, "OTA Taken token");
      const esp_partition_t *running_partition = esp_ota_get_running_partition();
      esp_app_desc_t running_partition_description;
      esp_ota_get_partition_description(running_partition, &running_partition_description);
      printf("current firmware versionnn is: %s\n", running_partition_description.version);
        
      printf("dentro taskOTA dopo def ISR\n");

      xTaskCreate(OnConnectedOta, "run_OTALogic", 1024 * 5, NULL, 5, &taskOTAHandle);
    }
    else
    {
      printf("TaskOTA timed out \n");
    }
    printf("dentro TaskOTA dopo if \n");
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  printf("dentro TaskOTA dopo while --> NON preso token \n");
}
void app_main(void)
{
  ota_semaphore = xSemaphoreCreateBinary();
  wifiInit();

  printf("dentro main prima task MQTT \n");
  xTaskCreate(&taskMQTT, "sensRead & DataRece", 1024*20, "taskMQTT", 2, NULL);
  printf("dentro main prima task OTA \n");
  xTaskCreate(&taskOTA, "Ve&Down NewSW Ver", 1024*10, "taskOTA", 2, NULL);
  ota_incourse = 0;

  xSemaphoreGive(ota_semaphore);
  printf("dentro main dopo semaphore give \n");
}