#include <pthread.h>
#include <fcntl.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_vfs.h"

#include "dns_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "cJSON.h"
#include "global_state.h"
#include "nvs_config.h"
#include "vcore.h"
#include "power.h"
#include "connect.h"
#include "asic.h"
#include "TPS546.h"
#include "statistics_task.h"
#include "theme_api.h"  // Add theme API include
#include "axe-os/api/system/asic_settings.h"
#include "http_server.h"
#include "system.h"
#include "websocket.h"

#define JSON_ALL_STATS_ELEMENT_SIZE 120
#define JSON_DASHBOARD_STATS_ELEMENT_SIZE 60

static const char * TAG = "http_server";
static const char * CORS_TAG = "CORS";

static char axeOSVersion[32];

static GlobalState * GLOBAL_STATE;
static httpd_handle_t server = NULL;

/* Handler for WiFi scan endpoint */
static esp_err_t GET_wifi_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    
    // Give some time for the connected flag to take effect
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    wifi_ap_record_simple_t ap_records[20];
    uint16_t ap_count = 0;

    esp_err_t err = wifi_scan(ap_records, &ap_count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    for (int i = 0; i < ap_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(network, "authmode", ap_records[i].authmode);
        cJSON_AddItemToArray(networks, network);
    }

    cJSON_AddItemToObject(root, "networks", networks);

    const char *response = cJSON_Print(root);
    httpd_resp_sendstr(req, response);

    free((void *)response);
    cJSON_Delete(root);
    return ESP_OK;
}


#define REST_CHECK(a, str, goto_tag, ...)                                                                                          \
    do {                                                                                                                           \
        if (!(a)) {                                                                                                                \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__);                                                  \
            goto goto_tag;                                                                                                         \
        }                                                                                                                          \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)
#define MESSAGE_QUEUE_SIZE (128)

typedef struct rest_server_context
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

static esp_err_t ip_in_private_range(uint32_t address) {
    uint32_t ip_address = ntohl(address);

    // 10.0.0.0 - 10.255.255.255 (Class A)
    if ((ip_address >= 0x0A000000) && (ip_address <= 0x0AFFFFFF)) {
        return ESP_OK;
    }

    // 172.16.0.0 - 172.31.255.255 (Class B)
    if ((ip_address >= 0xAC100000) && (ip_address <= 0xAC1FFFFF)) {
        return ESP_OK;
    }

    // 192.168.0.0 - 192.168.255.255 (Class C)
    if ((ip_address >= 0xC0A80000) && (ip_address <= 0xC0A8FFFF)) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static uint32_t extract_origin_ip_addr(char *origin)
{
    char ip_str[16];
    uint32_t origin_ip_addr = 0;

    // Find the start of the IP address in the Origin header
    const char *prefix = "http://";
    char *ip_start = strstr(origin, prefix);
    if (ip_start) {
        ip_start += strlen(prefix); // Move past "http://"

        // Extract the IP address portion (up to the next '/')
        char *ip_end = strchr(ip_start, '/');
        size_t ip_len = ip_end ? (size_t)(ip_end - ip_start) : strlen(ip_start);
        if (ip_len < sizeof(ip_str)) {
            strncpy(ip_str, ip_start, ip_len);
            ip_str[ip_len] = '\0'; // Null-terminate the string

            // Convert the IP address string to uint32_t
            origin_ip_addr = inet_addr(ip_str);
            if (origin_ip_addr == INADDR_NONE) {
                ESP_LOGW(CORS_TAG, "Invalid IP address: %s", ip_str);
            } else {
                ESP_LOGD(CORS_TAG, "Extracted IP address %lu", origin_ip_addr);
            }
        } else {
            ESP_LOGW(CORS_TAG, "IP address string is too long: %s", ip_start);
        }
    }

    return origin_ip_addr;
}

esp_err_t is_network_allowed(httpd_req_t * req)
{
    if (GLOBAL_STATE->SYSTEM_MODULE.ap_enabled == true) {
        ESP_LOGI(CORS_TAG, "Device in AP mode. Allowing CORS.");
        return ESP_OK;
    }

    int sockfd = httpd_req_to_sockfd(req);
    char ipstr[INET6_ADDRSTRLEN];
    struct sockaddr_in6 addr;   // esp_http_server uses IPv6 addressing
    socklen_t addr_size = sizeof(addr);

    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) < 0) {
        ESP_LOGE(CORS_TAG, "Error getting client IP");
        return ESP_FAIL;
    }

    uint32_t request_ip_addr = addr.sin6_addr.un.u32_addr[3];

    // // Convert to IPv6 string
    // inet_ntop(AF_INET, &addr.sin6_addr, ipstr, sizeof(ipstr));

    // Convert to IPv4 string
    inet_ntop(AF_INET, &request_ip_addr, ipstr, sizeof(ipstr));

    // Attempt to get the Origin header.
    char origin[128];
    uint32_t origin_ip_addr;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK) {
        ESP_LOGD(CORS_TAG, "Origin header: %s", origin);
        origin_ip_addr = extract_origin_ip_addr(origin);
    } else {
        ESP_LOGD(CORS_TAG, "No origin header found.");
        origin_ip_addr = request_ip_addr;
    }

    if (ip_in_private_range(origin_ip_addr) == ESP_OK && ip_in_private_range(request_ip_addr) == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(CORS_TAG, "Client is NOT in the private ip ranges or same range as server.");
    return ESP_FAIL;
}

static void readAxeOSVersion(void) {
    FILE* f = fopen("/version.txt", "r");
    if (f != NULL) {
        size_t n = fread(axeOSVersion, 1, sizeof(axeOSVersion) - 1, f);
        axeOSVersion[n] = '\0';
        fclose(f);

        ESP_LOGI(TAG, "AxeOS version: %s", axeOSVersion);

        if (strcmp(axeOSVersion, esp_app_get_description()->version) != 0) {
            ESP_LOGE(TAG, "Firmware (%s) and AxeOS (%s) versions do not match. Please make sure to update both www.bin and esp-miner.bin.", esp_app_get_description()->version, axeOSVersion);
        }
    } else {
        strcpy(axeOSVersion, "unknown");
        ESP_LOGE(TAG, "Failed to open AxeOS version.txt");
    }
}

esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {.base_path = "", .partition_label = NULL, .max_files = 5, .format_if_mount_failed = false};
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    readAxeOSVersion();

    return ESP_OK;
}

/* Function for stopping the webserver */
void stop_webserver(httpd_handle_t server)
{
    if (server) {
        /* Stop the httpd server */
        httpd_stop(server);
    }
}

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t * req, const char * filepath)
{
    const char * type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "image/svg+xml";
    } else if (CHECK_FILE_EXTENSION(filepath, ".pdf")) {
        type = "application/pdf";
    } else if (CHECK_FILE_EXTENSION(filepath, ".woff2")) {
        type = "font/woff2";
    }
    return httpd_resp_set_type(req, type);
}

esp_err_t set_cors_headers(httpd_req_t * req)
{
    esp_err_t err;

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Recovery handler */
static esp_err_t rest_recovery_handler(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    extern const unsigned char recovery_page_start[] asm("_binary_recovery_page_html_start");
    extern const unsigned char recovery_page_end[] asm("_binary_recovery_page_html_end");
    const size_t recovery_page_size = (recovery_page_end - recovery_page_start);
    httpd_resp_send_chunk(req, (const char*)recovery_page_start, recovery_page_size);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Send a 404 as JSON for unhandled api routes */
static esp_err_t rest_api_common_handler(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "unknown route");

    const char * error_obj = cJSON_Print(root);
    httpd_resp_set_status(req, HTTPD_404);
    httpd_resp_sendstr(req, error_obj);
    free((char *)error_obj);
    cJSON_Delete(root);
    return ESP_OK;
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t * req)
{
    char filepath[FILE_PATH_MAX];
    uint8_t filePathLength = sizeof(filepath);

    rest_server_context_t * rest_context = (rest_server_context_t *) req->user_ctx;
    strlcpy(filepath, rest_context->base_path, filePathLength);
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", filePathLength);
    } else {
        strlcat(filepath, req->uri, filePathLength);
    }
    set_content_type_from_file(req, filepath);
    strcat(filepath, ".gz");
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        // Set status
        httpd_resp_set_status(req, "302 Temporary Redirect");
        // Redirect to the "/" root directory
        httpd_resp_set_hdr(req, "Location", "/");
        // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
        httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

        ESP_LOGI(TAG, "Redirecting to root");
        return ESP_OK;
    }
    if (req->uri[strlen(req->uri) - 1] != '/') {
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=2592000");
    }

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char * chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_OK;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_options_request(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers for OPTIONS request
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // Send a blank response for OPTIONS request
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t PATCH_update_settings(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int total_len = req->content_len;
    int cur_len = 0;
    char * buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_OK;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_OK;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON * root = cJSON_Parse(buf);
    cJSON * item;
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumURL"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_URL, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumURL"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_URL, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "stratumExtranonceSubscribe")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_EXTRANONCE_SUBSCRIBE, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "stratumSuggestedDifficulty")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_DIFFICULTY, item->valueint);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumUser"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_USER, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumPassword"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_PASS, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumExtranonceSubscribe")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumSuggestedDifficulty")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_DIFFICULTY, item->valueint);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumUser"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_USER, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumPassword"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "stratumPort")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_PORT, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumPort")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, item->valueint);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "ssid"))) {
        nvs_config_set_string(NVS_CONFIG_WIFI_SSID, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "wifiPass"))) {
        nvs_config_set_string(NVS_CONFIG_WIFI_PASS, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "hostname"))) {
        nvs_config_set_string(NVS_CONFIG_HOSTNAME, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "coreVoltage")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "frequency")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_ASIC_FREQ, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "overheat_mode")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "display"))) {
        nvs_config_set_string(NVS_CONFIG_DISPLAY, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "rotation")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_ROTATION, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "invertscreen")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_INVERT_SCREEN, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "displayTimeout")) != NULL) {
        nvs_config_set_i32(NVS_CONFIG_DISPLAY_TIMEOUT, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "autofanspeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "fanspeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "minFanSpeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_MIN_FAN_SPEED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "temptarget")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_TEMP_TARGET, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "statsFrequency")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_STATISTICS_FREQUENCY, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "overclockEnabled")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_OVERCLOCK_ENABLED, item->valueint);
    }

    cJSON_Delete(root);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t POST_restart(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Restarting System because of API Request");

    // Send HTTP response before restarting
    const char* resp_str = "System will restart shortly.";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // Delay to ensure the response is sent
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    // Restart the system
    esp_restart();

    // This return statement will never be reached, but it's good practice to include it
    return ESP_OK;
}

/* Simple handler for getting system handler */
static esp_err_t GET_system_info(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    char * ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, CONFIG_ESP_WIFI_SSID);
    char * hostname = nvs_config_get_string(NVS_CONFIG_HOSTNAME, CONFIG_LWIP_LOCAL_HOSTNAME);
    char * stratumURL = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
    char * fallbackStratumURL = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);
    char * stratumUser = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, CONFIG_STRATUM_USER);
    char * fallbackStratumUser = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER, CONFIG_FALLBACK_STRATUM_USER);
    char * display = nvs_config_get_string(NVS_CONFIG_DISPLAY, "SSD1306 (128x32)");
    uint16_t frequency = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);
    float expected_hashrate = frequency * GLOBAL_STATE->DEVICE_CONFIG.family.asic.small_core_count * GLOBAL_STATE->DEVICE_CONFIG.family.asic_count / 1000.0;

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char formattedMac[18];
    snprintf(formattedMac, sizeof(formattedMac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int8_t wifi_rssi = -90;
    get_wifi_current_rssi(&wifi_rssi);

    cJSON * root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "power", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.power);
    cJSON_AddNumberToObject(root, "voltage", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.voltage);
    cJSON_AddNumberToObject(root, "current", Power_get_current(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "temp", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.chip_temp_avg);
    cJSON_AddNumberToObject(root, "vrTemp", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.vr_temp);
    cJSON_AddNumberToObject(root, "maxPower", GLOBAL_STATE->DEVICE_CONFIG.family.max_power);
    cJSON_AddNumberToObject(root, "nominalVoltage", GLOBAL_STATE->DEVICE_CONFIG.family.nominal_voltage);
    cJSON_AddNumberToObject(root, "hashRate", GLOBAL_STATE->SYSTEM_MODULE.current_hashrate);
    cJSON_AddNumberToObject(root, "expectedHashrate", expected_hashrate);
    cJSON_AddStringToObject(root, "bestDiff", GLOBAL_STATE->SYSTEM_MODULE.best_diff_string);
    cJSON_AddStringToObject(root, "bestSessionDiff", GLOBAL_STATE->SYSTEM_MODULE.best_session_diff_string);
    cJSON_AddNumberToObject(root, "poolDifficulty", GLOBAL_STATE->pool_difficulty);

    cJSON_AddNumberToObject(root, "isUsingFallbackStratum", GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback);

    cJSON_AddNumberToObject(root, "isPSRAMAvailable", GLOBAL_STATE->psram_is_available);

    cJSON_AddNumberToObject(root, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "coreVoltage", nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE));
    cJSON_AddNumberToObject(root, "coreVoltageActual", VCORE_get_voltage_mv(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "frequency", frequency);
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "macAddr", formattedMac);
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddStringToObject(root, "wifiStatus", GLOBAL_STATE->SYSTEM_MODULE.wifi_status);
    cJSON_AddNumberToObject(root, "wifiRSSI", wifi_rssi);
    cJSON_AddNumberToObject(root, "apEnabled", GLOBAL_STATE->SYSTEM_MODULE.ap_enabled);
    cJSON_AddNumberToObject(root, "sharesAccepted", GLOBAL_STATE->SYSTEM_MODULE.shares_accepted);
    cJSON_AddNumberToObject(root, "sharesRejected", GLOBAL_STATE->SYSTEM_MODULE.shares_rejected);

    cJSON *error_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "sharesRejectedReasons", error_array);
    
    for (int i = 0; i < GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count; i++) {
        cJSON *error_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(error_obj, "message", GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].message);
        cJSON_AddNumberToObject(error_obj, "count", GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].count);
        cJSON_AddItemToArray(error_array, error_obj);
    }

    cJSON_AddNumberToObject(root, "uptimeSeconds", (esp_timer_get_time() - GLOBAL_STATE->SYSTEM_MODULE.start_time) / 1000000);
    cJSON_AddNumberToObject(root, "smallCoreCount", GLOBAL_STATE->DEVICE_CONFIG.family.asic.small_core_count);
    cJSON_AddStringToObject(root, "ASICModel", GLOBAL_STATE->DEVICE_CONFIG.family.asic.name);
    cJSON_AddStringToObject(root, "stratumURL", stratumURL);
    cJSON_AddNumberToObject(root, "stratumPort", nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT));
    cJSON_AddStringToObject(root, "stratumUser", stratumUser);
    cJSON_AddNumberToObject(root, "stratumSuggestedDifficulty", nvs_config_get_u16(NVS_CONFIG_STRATUM_DIFFICULTY, CONFIG_STRATUM_DIFFICULTY));
    cJSON_AddNumberToObject(root, "stratumExtranonceSubscribe", nvs_config_get_u16(NVS_CONFIG_STRATUM_EXTRANONCE_SUBSCRIBE, STRATUM_EXTRANONCE_SUBSCRIBE));
    cJSON_AddStringToObject(root, "fallbackStratumURL", fallbackStratumURL);
    cJSON_AddNumberToObject(root, "fallbackStratumPort", nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT));
    cJSON_AddStringToObject(root, "fallbackStratumUser", fallbackStratumUser);
    cJSON_AddNumberToObject(root, "fallbackStratumSuggestedDifficulty", nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_DIFFICULTY, CONFIG_FALLBACK_STRATUM_DIFFICULTY));
    cJSON_AddNumberToObject(root, "fallbackStratumExtranonceSubscribe", nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE, FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE));
    cJSON_AddNumberToObject(root, "responseTime", GLOBAL_STATE->SYSTEM_MODULE.response_time);

    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "axeOSVersion", axeOSVersion);

    cJSON_AddStringToObject(root, "idfVersion", esp_get_idf_version());
    cJSON_AddStringToObject(root, "boardVersion", GLOBAL_STATE->DEVICE_CONFIG.board_version);
    cJSON_AddStringToObject(root, "runningPartition", esp_ota_get_running_partition()->label);

    cJSON_AddNumberToObject(root, "overheat_mode", nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, 0));
    cJSON_AddNumberToObject(root, "overclockEnabled", nvs_config_get_u16(NVS_CONFIG_OVERCLOCK_ENABLED, 0));
    cJSON_AddStringToObject(root, "display", display);
    cJSON_AddNumberToObject(root, "rotation", nvs_config_get_u16(NVS_CONFIG_ROTATION, 0));
    cJSON_AddNumberToObject(root, "invertscreen", nvs_config_get_u16(NVS_CONFIG_INVERT_SCREEN, 0));
    cJSON_AddNumberToObject(root, "displayTimeout", nvs_config_get_i32(NVS_CONFIG_DISPLAY_TIMEOUT, -1));

    cJSON_AddNumberToObject(root, "autofanspeed", nvs_config_get_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1));

    cJSON_AddNumberToObject(root, "fanspeed", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_perc);
    cJSON_AddNumberToObject(root, "minFanSpeed", nvs_config_get_u16(NVS_CONFIG_MIN_FAN_SPEED, 25));
    cJSON_AddNumberToObject(root, "temptarget", nvs_config_get_u16(NVS_CONFIG_TEMP_TARGET, 60));
    cJSON_AddNumberToObject(root, "fanrpm", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_rpm);

    cJSON_AddNumberToObject(root, "statsFrequency", nvs_config_get_u16(NVS_CONFIG_STATISTICS_FREQUENCY, 0));

    if (GLOBAL_STATE->SYSTEM_MODULE.power_fault > 0) {
        cJSON_AddStringToObject(root, "power_fault", VCORE_get_fault_string(GLOBAL_STATE));
    }

    free(ssid);
    free(hostname);
    free(stratumURL);
    free(fallbackStratumURL);
    free(stratumUser);
    free(fallbackStratumUser);
    free(display);

    const char * sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((char *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

int create_json_statistics_all(cJSON * root)
{
    int prebuffer = 0;

    if (root) {
        // create array for all statistics
        const char *label[12] = {
            "hashRate", "temp", "vrTemp", "power", "voltage",
            "current", "coreVoltageActual", "fanspeed", "fanrpm",
            "wifiRSSI", "freeHeap", "timestamp"
        };

        cJSON * statsLabelArray = cJSON_CreateStringArray(label, 12);
        cJSON_AddItemToObject(root, "labels", statsLabelArray);
        prebuffer++;

        cJSON * statsArray = cJSON_AddArrayToObject(root, "statistics");

        if (NULL != GLOBAL_STATE->STATISTICS_MODULE.statisticsList) {
            StatisticsNodePtr node = *GLOBAL_STATE->STATISTICS_MODULE.statisticsList; // double pointer
            struct StatisticsData statsData;

            while (NULL != node) {
                node = statisticData(node, &statsData);

                cJSON *valueArray = cJSON_CreateArray();
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.hashrate));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.chipTemperature));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.vrTemperature));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.power));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.voltage));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.current));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.coreVoltageActual));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.fanSpeed));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.fanRPM));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.wifiRSSI));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.freeHeap));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.timestamp));

                cJSON_AddItemToArray(statsArray, valueArray);
                prebuffer++;
            }
        }
    }

    return prebuffer;
}

int create_json_statistics_dashboard(cJSON * root)
{
    int prebuffer = 0;

    if (root) {
        // create array for dashboard statistics
        cJSON * statsArray = cJSON_AddArrayToObject(root, "statistics");

        if (NULL != GLOBAL_STATE->STATISTICS_MODULE.statisticsList) {
            StatisticsNodePtr node = *GLOBAL_STATE->STATISTICS_MODULE.statisticsList; // double pointer
            struct StatisticsData statsData;

            while (NULL != node) {
                node = statisticData(node, &statsData);

                cJSON *valueArray = cJSON_CreateArray();
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.hashrate));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.chipTemperature));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.power));
                cJSON_AddItemToArray(valueArray, cJSON_CreateNumber(statsData.timestamp));

                cJSON_AddItemToArray(statsArray, valueArray);
                prebuffer++;
            }
        }
    }

    return prebuffer;
}

static esp_err_t GET_system_statistics(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "currentTimestamp", (esp_timer_get_time() / 1000));
    int prebuffer = 1;

    prebuffer += create_json_statistics_all(root);

    const char * response = cJSON_PrintBuffered(root, (JSON_ALL_STATS_ELEMENT_SIZE * prebuffer), 0); // unformatted
    httpd_resp_sendstr(req, response);
    free((void *)response);

    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t GET_system_statistics_dashboard(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "currentTimestamp", (esp_timer_get_time() / 1000));
    int prebuffer = 1;

    prebuffer += create_json_statistics_dashboard(root);

    const char * response = cJSON_PrintBuffered(root, (JSON_DASHBOARD_STATS_ELEMENT_SIZE * prebuffer), 0); // unformatted
    httpd_resp_sendstr(req, response);
    free((void *)response);

    cJSON_Delete(root);

    return ESP_OK;
}

esp_err_t POST_WWW_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }

    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "www.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[1000];
    int remaining = req->content_len;

    const esp_partition_t * www_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
    if (www_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WWW partition not found");
        return ESP_OK;
    }

    // Don't attempt to write more than what can be stored in the partition
    if (remaining > www_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File provided is too large for device");
        return ESP_OK;
    }

    // Erase the entire www partition before writing
    ESP_ERROR_CHECK(esp_partition_erase_range(www_partition, 0, www_partition->size));

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        } else if (recv_len <= 0) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Protocol Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            return ESP_OK;
        }

        if (esp_partition_write(www_partition, www_partition->size - remaining, (const void *) buf, recv_len) != ESP_OK) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
            return ESP_OK;
        }


        uint8_t percentage = 100 - ((remaining * 100 / req->content_len));
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);

        remaining -= recv_len;
    }

    httpd_resp_sendstr(req, "WWW update complete\n");

    readAxeOSVersion();

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Finished...");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;

    return ESP_OK;
}

/*
 * Handle OTA file upload
 */
esp_err_t POST_OTA_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }
    
    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "esp-miner.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[1000];
    esp_ota_handle_t ota_handle;
    int remaining = req->content_len;

    const esp_partition_t * ota_partition = esp_ota_get_next_update_partition(NULL);
    ESP_ERROR_CHECK(esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle));

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

        // Timeout Error: Just retry
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;

            // Serious Error: Abort OTA
        } else if (recv_len <= 0) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Protocol Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            return ESP_OK;
        }

        // Successful Upload: Flash firmware chunk
        if (esp_ota_write(ota_handle, (const void *) buf, recv_len) != ESP_OK) {
            esp_ota_abort(ota_handle);
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
            return ESP_OK;
        }

        uint8_t percentage = 100 - ((remaining * 100 / req->content_len));

        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);

        remaining -= recv_len;
    }

    // Validate and switch to new OTA image and reboot
    if (esp_ota_end(ota_handle) != ESP_OK || esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Validation Error");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation / Activation Error");
        return ESP_OK;
    }

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Rebooting...");

    httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
    ESP_LOGI(TAG, "Restarting System because of Firmware update complete");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();

    return ESP_OK;
}

// HTTP Error (404) Handler - Redirects all requests to the root page
esp_err_t http_404_error_handler(httpd_req_t * req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "302 Temporary Redirect");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting to root");
    return ESP_OK;
}

esp_err_t start_rest_server(void * pvParameters)
{
    GLOBAL_STATE = (GlobalState *) pvParameters;
    
    // Initialize the ASIC API with the global state
    asic_api_init(GLOBAL_STATE);
    const char * base_path = "";

    bool enter_recovery = false;
    if (init_fs() != ESP_OK) {
        // Unable to initialize the web app filesystem.
        // Enter recovery mode
        enter_recovery = true;
    }

    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t * rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_open_sockets = 20;
    config.max_uri_handlers = 20;
    config.close_fn = websocket_close_fn;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    httpd_uri_t recovery_explicit_get_uri = {
        .uri = "/recovery", 
        .method = HTTP_GET, 
        .handler = rest_recovery_handler, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &recovery_explicit_get_uri);
    
    // Register theme API endpoints
    ESP_ERROR_CHECK(register_theme_api_endpoints(server, rest_context));

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/system/info", 
        .method = HTTP_GET, 
        .handler = GET_system_info, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for fetching system asic values */
    httpd_uri_t system_asic_get_uri = {
        .uri = "/api/system/asic", 
        .method = HTTP_GET, 
        .handler = GET_system_asic, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_asic_get_uri);

    /* URI handler for fetching system statistic values */
    httpd_uri_t system_statistics_get_uri = {
        .uri = "/api/system/statistics", 
        .method = HTTP_GET, 
        .handler = GET_system_statistics, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_statistics_get_uri);

    /* URI handler for fetching system statistic values for dashboard */
    httpd_uri_t system_statistics_dashboard_get_uri = {
        .uri = "/api/system/statistics/dashboard", 
        .method = HTTP_GET, 
        .handler = GET_system_statistics_dashboard, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_statistics_dashboard_get_uri);

    /* URI handler for WiFi scan */
    httpd_uri_t wifi_scan_get_uri = {
        .uri = "/api/system/wifi/scan",
        .method = HTTP_GET,
        .handler = GET_wifi_scan,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &wifi_scan_get_uri);

    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart", .method = HTTP_POST, 
        .handler = POST_restart, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_restart_uri);

    httpd_uri_t system_restart_options_uri = {
        .uri = "/api/system/restart", 
        .method = HTTP_OPTIONS, 
        .handler = handle_options_request, 
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_restart_options_uri);

    httpd_uri_t update_system_settings_uri = {
        .uri = "/api/system", 
        .method = HTTP_PATCH, 
        .handler = PATCH_update_settings, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &update_system_settings_uri);

    httpd_uri_t system_options_uri = {
        .uri = "/api/system",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &system_options_uri);

    httpd_uri_t update_post_ota_firmware = {
        .uri = "/api/system/OTA", 
        .method = HTTP_POST, 
        .handler = POST_OTA_update, 
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &update_post_ota_firmware);

    httpd_uri_t update_post_ota_www = {
        .uri = "/api/system/OTAWWW", 
        .method = HTTP_POST, 
        .handler = POST_WWW_update, 
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &update_post_ota_www);

    httpd_uri_t ws = {
        .uri = "/api/ws", 
        .method = HTTP_GET, 
        .handler = websocket_handler, 
        .user_ctx = NULL, 
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws);

    if (enter_recovery) {
        /* Make default route serve Recovery */
        httpd_uri_t recovery_implicit_get_uri = {
            .uri = "/*", .method = HTTP_GET, 
            .handler = rest_recovery_handler, 
            .user_ctx = rest_context
        };
        httpd_register_uri_handler(server, &recovery_implicit_get_uri);

    } else {
        httpd_uri_t api_common_uri = {
            .uri = "/api/*",
            .method = HTTP_ANY,
            .handler = rest_api_common_handler,
            .user_ctx = rest_context
        };
        httpd_register_uri_handler(server, &api_common_uri);
        /* URI handler for getting web server files */
        httpd_uri_t common_get_uri = {
            .uri = "/*", 
            .method = HTTP_GET, 
            .handler = rest_common_get_handler, 
            .user_ctx = rest_context
        };
        httpd_register_uri_handler(server, &common_get_uri);
    }

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    // Start websocket log handler thread
    xTaskCreate(websocket_task, "websocket_task", 4096, server, 2, NULL);

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    start_dns_server(&dns_config);

    return ESP_OK;
err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}
