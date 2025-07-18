/* ESP32 OPC UA Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include <nvs_flash.h>
#include "esp_spiffs.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

#include <stdio.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_netif.h"
#include "ethernet_helper.h"
#include "driver/gpio.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#define CHIP_NAME "ESP32"
#endif

#ifdef CONFIG_IDF_TARGET_ESP32S2BETA
#define CHIP_NAME "ESP32-S2 Beta"
#endif

#ifndef UA_ARCHITECTURE_FREERTOSLWIP
#error UA_ARCHITECTURE_FREERTOSLWIP needs to be defined
#endif

#include <open62541.h>

#include <esp_task_wdt.h>
#include <esp_sntp.h>

#define LED_GPIO 5
#define POT_ADC ADC1_CHANNEL_6
#define TEMP_ADC ADC1_CHANNEL_7

static const char *TAG = "MAIN";
static const char *TAG_OPC = "OPC UA";

static bool serverCreated = false;

/* Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;

static void
beforeReadLed(UA_Server *server,
               const UA_NodeId *sessionId, void *sessionContext,
               const UA_NodeId *nodeid, void *nodeContext,
               const UA_NumericRange *range, const UA_DataValue *data) {
    UA_Boolean level = gpio_get_level(LED_GPIO);
    UA_Variant value;
    UA_Variant_setScalar(&value, &level, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_NodeId nodeId = UA_NODEID_STRING(1, "led");
    UA_Server_writeValue(server, nodeId, value);
}

static void
afterWriteLed(UA_Server *server,
               const UA_NodeId *sessionId, void *sessionContext,
               const UA_NodeId *nodeId, void *nodeContext,
               const UA_NumericRange *range, const UA_DataValue *data) {
    int value = (int)*(UA_Boolean *)(data->value.data);
    gpio_set_level(LED_GPIO, value);               
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                "The variable led was updated to %d - current value = %d", value, gpio_get_level(LED_GPIO));
}

static void configLed() {
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_INPUT_OUTPUT);
}

static void
beforeReadPot(UA_Server *server,
               const UA_NodeId *sessionId, void *sessionContext,
               const UA_NodeId *nodeid, void *nodeContext,
               const UA_NumericRange *range, const UA_DataValue *data) {
    UA_Int32 readADC = adc1_get_raw(POT_ADC);
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                "Reading variable pot = %d", readADC);
    UA_Variant value;
    UA_Variant_setScalar(&value, &readADC, &UA_TYPES[UA_TYPES_INT32]);
    UA_NodeId nodeId = UA_NODEID_STRING(1, "pot");
    UA_Server_writeValue(server, nodeId, value);
}

static void configPot() {
    adc1_config_width(ADC_WIDTH_BIT_10);
    adc1_config_channel_atten(POT_ADC, ADC_ATTEN_DB_0);
}

static void
beforeRead(UA_Server *server,
               const UA_NodeId *sessionId, void *sessionContext,
               const UA_NodeId *nodeid, void *nodeContext,
               const UA_NumericRange *range, const UA_DataValue *data) {
    UA_Int32 randomValue =  rand() % 1000;
    UA_Variant value;
    UA_Variant_setScalar(&value, &randomValue, &UA_TYPES[UA_TYPES_INT32]);
    UA_Server_writeValue(server, *nodeid, value);
}

static void
addVariables(UA_Server *server) {
    //Add variable led
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Boolean led = false;
    attr.displayName = UA_LOCALIZEDTEXT("en-US", "led");
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Variant_setScalar(&attr.value, &led, &UA_TYPES[UA_TYPES_BOOLEAN]);

    UA_NodeId nodeId = UA_NODEID_STRING(1, "led");
    UA_QualifiedName name = UA_QUALIFIEDNAME(1, "led");
    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_NodeId variableTypeNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    UA_Server_addVariableNode(server, nodeId, parentNodeId,
                              parentReferenceNodeId, name,
                              variableTypeNodeId, attr, NULL, NULL);

    UA_ValueCallback ledCallback;
    ledCallback.onRead = beforeReadLed;
    ledCallback.onWrite = afterWriteLed;
    UA_Server_setVariableNode_valueCallback(server, nodeId, ledCallback);

    configLed();

    //Add variable pot
    UA_Int32 pot = 0;
    attr.displayName = UA_LOCALIZEDTEXT("en-US", "pot");
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    UA_Variant_setScalar(&attr.value, &pot, &UA_TYPES[UA_TYPES_INT32]);

    nodeId = UA_NODEID_STRING(1, "pot");
    name = UA_QUALIFIEDNAME(1, "pot");
    UA_Server_addVariableNode(server, nodeId, parentNodeId,
                              parentReferenceNodeId, name,
                              variableTypeNodeId, attr, NULL, NULL);

    UA_ValueCallback potCallback;
    potCallback.onRead = beforeReadPot;
    potCallback.onWrite = NULL;
    UA_Server_setVariableNode_valueCallback(server, nodeId, potCallback);

    configPot();

    UA_Int32 var = 0;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    UA_Variant_setScalar(&attr.value, &var, &UA_TYPES[UA_TYPES_INT32]);
    UA_ValueCallback varCallback;
    varCallback.onRead = beforeRead;
    varCallback.onWrite = NULL;
    for (int i = 1; i <= 10; i++) {
        char nodeName[15];
        sprintf(nodeName,"var%d", i);
        attr.displayName = UA_LOCALIZEDTEXT("en-US", nodeName);
        nodeId = UA_NODEID_STRING(1, nodeName);
        name = UA_QUALIFIEDNAME(1, nodeName);

        UA_Server_addVariableNode(server, nodeId, parentNodeId,
                              parentReferenceNodeId, name,
                              variableTypeNodeId, attr, NULL, NULL);
        UA_Server_setVariableNode_valueCallback(server, nodeId, varCallback);
    }
}

static UA_StatusCode
UA_ServerConfig_setUriName(UA_ServerConfig *uaServerConfig, const char *uri, const char *name) {
    // delete pre-initialized values
    UA_String_deleteMembers(&uaServerConfig->applicationDescription.applicationUri);
    UA_LocalizedText_deleteMembers(&uaServerConfig->applicationDescription.applicationName);

    uaServerConfig->applicationDescription.applicationUri = UA_String_fromChars(uri);
    uaServerConfig->applicationDescription.applicationName.locale = UA_STRING_NULL;
    uaServerConfig->applicationDescription.applicationName.text = UA_String_fromChars(name);

    for (size_t i = 0; i < uaServerConfig->endpointsSize; i++) {
        UA_String_deleteMembers(&uaServerConfig->endpoints[i].server.applicationUri);
        UA_LocalizedText_deleteMembers(
                &uaServerConfig->endpoints[i].server.applicationName);

        UA_String_copy(&uaServerConfig->applicationDescription.applicationUri,
                       &uaServerConfig->endpoints[i].server.applicationUri);

        UA_LocalizedText_copy(&uaServerConfig->applicationDescription.applicationName,
                              &uaServerConfig->endpoints[i].server.applicationName);
    }

    return UA_STATUSCODE_GOOD;
}

static void initSPIFFS() {
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

static void clearSPIFFS() {
    esp_vfs_spiffs_unregister(NULL);
    ESP_LOGI(TAG, "SPIFFS unmounted");
}

static UA_ByteString loadFile(const char * filePath) {
    UA_ByteString fileContents = UA_STRING_NULL;

    FILE *fp = fopen(filePath, "rb");
    if(!fp) {
        ESP_LOGE(TAG, "Failed to open %s", filePath);
        return fileContents;
    }

    fseek(fp, 0, SEEK_END);
    fileContents.length = (size_t)ftell(fp);
    fileContents.data = (UA_Byte *)UA_malloc(fileContents.length * sizeof(UA_Byte));
    if(fileContents.data) {
        fseek(fp, 0, SEEK_SET);
        size_t read = fread(fileContents.data, sizeof(UA_Byte), fileContents.length, fp);
        if(read != fileContents.length)
            UA_ByteString_clear(&fileContents);
    } else {
        fileContents.length = 0;
    }
    fclose(fp);
    
    return fileContents;
}

static void addSecurityPolicy(UA_ServerConfig *config, const UA_ByteString *certificate, const UA_ByteString *privateKey) {
    UA_StatusCode retval = UA_ServerConfig_addSecurityPolicyBasic128Rsa15(config, certificate, privateKey);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                       "Could not add SecurityPolicy#Basic128Rsa15 with error code %s",
                       UA_StatusCode_name(retval));
        return;
    }

    retval = UA_ServerConfig_addEndpoint(config, UA_BYTESTRING("http://opcfoundation.org/UA/SecurityPolicy#Basic128Rsa15"),
                                         UA_MESSAGESECURITYMODE_SIGN);

    retval = UA_ServerConfig_addEndpoint(config, UA_BYTESTRING("http://opcfoundation.org/UA/SecurityPolicy#Basic128Rsa15"),
                                         UA_MESSAGESECURITYMODE_SIGNANDENCRYPT);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                       "Could not add endpoint with error code %s",
                       UA_StatusCode_name(retval));
        return;
    }

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                "Added SecurityPolicy#Basic128Rsa15");
}


static void opcua_task(void *arg) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    //The default 64KB of memory for sending and receicing buffer caused problems to many users. With the code below, they are reduced to ~16KB
    UA_UInt32 sendBufferSize = 16000;       //64 KB was too much for my platform
    UA_UInt32 recvBufferSize = 16000;       //64 KB was too much for my platform

    ESP_LOGI(TAG_OPC, "Initializing OPC UA. Free Heap: %d bytes", xPortGetFreeHeapSize());

    initSPIFFS();
    UA_ByteString certificate = loadFile("/spiffs/server_cert.der");
    if(certificate.length == 0) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Unable to load file server certificate.");
    }

    UA_ByteString privateKey = loadFile("/spiffs/server_key.der");
    if(privateKey.length == 0) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Unable to load file server private key.");
    }
    clearSPIFFS();

    UA_Server *server = UA_Server_new();

    UA_ServerConfig *config = UA_Server_getConfig(server);

    UA_StatusCode res;
    if(certificate.length == 0) {
        res = UA_ServerConfig_setMinimalCustomBuffer(config, 4840, 0, sendBufferSize, recvBufferSize);
    } else {
        res = UA_ServerConfig_setMinimalCustomBuffer(config, 4840, &certificate, sendBufferSize, recvBufferSize);

        if(privateKey.length != 0 && res == UA_STATUSCODE_GOOD) {
            addSecurityPolicy(config, &certificate, &privateKey);
        }
    }
            
    if(res != UA_STATUSCODE_GOOD) {
        UA_ServerConfig_clean(config);

        UA_ByteString_clear(&certificate);
        UA_ByteString_clear(&privateKey);

        return;
    }

    config->maxSessions = 2;
    config->maxMonitoredItemsPerSubscription = 2;
    config->queueSizeLimits = (UA_UInt32Range) {1, 1};
    config->publishingIntervalLimits = (UA_DurationRange) {10.0, 3600.0 * 1000.0};
    config->samplingIntervalLimits = (UA_DurationRange) {10.0, 24.0 * 3600.0 * 1000.0};

    UA_ServerConfig_setUriName(config, "urn:esp.server.application", "ESPServer");

    UA_String str = UA_STRING(CONFIG_ETHERNET_HELPER_STATIC_IP4_ADDRESS);
    UA_String_clear(&config->customHostname);
    UA_String_copy(&str, &config->customHostname);

    printf("xPortGetFreeHeapSize before create = %d bytes\n", xPortGetFreeHeapSize());

    addVariables(server);

    UA_StatusCode retval = UA_Server_run_startup(server);
    if (retval != UA_STATUSCODE_GOOD) {
        ESP_LOGE(TAG_OPC, "Starting up the server failed with %s", UA_StatusCode_name(retval));
        return;
    }

    ESP_LOGI(TAG_OPC, "Starting server loop. Free Heap: %d bytes", xPortGetFreeHeapSize());

    while (true) {
        UA_Server_run_iterate(server, false);
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        taskYIELD();
    }
    UA_Server_run_shutdown(server);

    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
}


static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    /*UA_Server *server = *(UA_Server**) arg;
    if (server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }*/
}

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    sntp_init();
}

static bool obtain_time(void)
{
    initialize_sntp();

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(struct tm));
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry <= retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        ESP_ERROR_CHECK(esp_task_wdt_reset());
    }
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    return timeinfo.tm_year > (2016 - 1900);
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "WIFI Connected");

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        if (!obtain_time()) {
            ESP_LOGE(TAG, "Could not get time from NTP. Using default timestamp.");
        }
        // update 'now' variable with current time
        time(&now);
    }
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Current time: %d-%02d-%02d %02d:%02d:%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon+1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    if (!serverCreated) {
        ESP_LOGI(TAG, "Starting OPC UA Task");
        // We need a big stack depth here. You may adapt it if necessary
        xTaskCreate(opcua_task, "opcua_task", 24336, NULL, 10, NULL);
        serverCreated = true;
    }
}

void app_main(void)
{
    ++boot_count;
    ESP_LOGI(TAG, "Boot count: %d", boot_count);

    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU cores, WiFi%s%s, ",
           CHIP_NAME,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    spi_flash_init();

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Heap Info:\n");
    printf("\tInternal free: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("\tSPI free: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("\tDefault free: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    printf("\tAll free: %d bytes\n", xPortGetFreeHeapSize());


    //static UA_Server *server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_task_wdt_init(10, true));
    // Remove idle tasks from watchdog
    ESP_ERROR_CHECK(esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0)));
    ESP_ERROR_CHECK(esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(1)));

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
#ifdef CONFIG_ETHERNET_HELPER_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, NULL));
#endif // CONFIG_ETHERNET_HELPER_WIFI

    ESP_LOGI(TAG, "Waiting for wifi connection. OnConnect will start OPC UA...");

    ESP_ERROR_CHECK(ethernet_helper_connect());

}