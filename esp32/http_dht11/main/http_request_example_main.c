/* HTTP GET Example using plain POSIX sockets

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "dht.h"
#include "esp_mac.h"

#define ONE_WIRE_GPIO 4
#define USER_AGENT     "esp-idf/1.0 esp32c3"
#define API_IP        "192.168.0.3"
#define API_PORT      "9090"
#define API_IP_PORT   API_IP ":" API_PORT
#define DEVICE_ID     "ESP32"

static const gpio_num_t dht_gpio = ONE_WIRE_GPIO;
static const dht_sensor_type_t sensor_type = DHT_TYPE_DHT11;
static const char *TAG = "temp_collector";


//
// path /measurement API service
//
static char *BODY_MEASUREMENT = "id="DEVICE_ID"&t=%0.2f&h=%0.2f&timestamp=%s&key=%s";

static char *REQUEST_POST_MEASUREMENT = "POST /measurement HTTP/1.0\r\n"
    "Host: "API_IP_PORT"\r\n"
    "User-Agent: "USER_AGENT"\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: %d\r\n"
    "\r\n"
    "%s";


//
// path /device API service
//
static char *BODY_DEVICE = "id="DEVICE_ID"&n=%s&k=%s";

static char *REQUEST_POST_REGISTER_DEVICE = "POST /device HTTP/1.0\r\n"
    "Host: "API_IP_PORT"\r\n"
    "User-Agent: "USER_AGENT"\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: %d\r\n"
    "\r\n"
    "%s";

   
void http_post(char * send_buf){

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;

    char recv_buf[64];

    int err = getaddrinfo(API_IP, API_PORT, &hints, &res);

    if(err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return;
    }

    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    s = socket(res->ai_family, res->ai_socktype, 0);
    if(s < 0) {
        ESP_LOGE(TAG, "... Failed to allocate socket.");
        freeaddrinfo(res);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return;
    }
    ESP_LOGI(TAG, "... allocated socket");

    if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return;
    }

    ESP_LOGI(TAG, "... connected");
    freeaddrinfo(res);

    if (write(s, send_buf, strlen(send_buf)) < 0) {
        ESP_LOGE(TAG, "... socket send failed");
        close(s);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return;
    }
    ESP_LOGI(TAG, "... socket send success 1");

    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;

    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, sizeof(receiving_timeout)) < 0) {

        ESP_LOGE(TAG, "... failed to set socket receiving timeout");
        close(s);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return;
    }
    ESP_LOGI(TAG, "... set socket receiving timeout success");

    do {
        bzero(recv_buf, sizeof(recv_buf));
        r = read(s, recv_buf, sizeof(recv_buf)-1);
        for(int i = 0; i < r; i++) {
            putchar(recv_buf[i]);
        }
    } while(r > 0);

    ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);

    close(s);
}

void get_mac_address(char * mac_address){

    unsigned char mac_base[6] = {0};
    esp_efuse_mac_get_default(mac_base);
    esp_read_mac(mac_base, ESP_MAC_WIFI_STA);
    unsigned char mac_local_base[6] = {0};
    unsigned char mac_uni_base[6] = {0};
    esp_derive_local_mac(mac_local_base, mac_uni_base);

    // char mac_address_str_3[22];
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X"
        , mac_base[0]
        , mac_base[1]
        , mac_base[2]
        , mac_base[3]
        , mac_base[4]
        , mac_base[5] );
}

void get_timestamp(char * timestamp){
    time_t rawtime;
    struct tm *info;
    time( &rawtime );
    info = localtime( &rawtime );
    strftime(timestamp,16,"%Y%m%d%H%M%S", info);
}

static void http_get_task(void *pvParameters) {
    
    int16_t temperature = 0;
    int16_t humidity = 0;

    //
    // determinando la mac address
    //
    char mac_address_str_3[22];
    get_mac_address(mac_address_str_3);

    char body[100];
    char send_buf[256];

    //
    // registramos el dispositivo con la mac address
    //
    sprintf(body, BODY_DEVICE, DEVICE_ID , mac_address_str_3);
    sprintf(send_buf, REQUEST_POST_REGISTER_DEVICE, (int)strlen(body),body );
    http_post(send_buf);

    while(1) {

        //
        // determinando el timestamp
        //
        char buffer[16];
        get_timestamp(buffer);

        if (dht_read_data(sensor_type, dht_gpio, &humidity, &temperature) == ESP_OK) {
         
            ESP_LOGI(TAG,"MAC: %s\n", mac_address_str_3);
            
            ESP_LOGI(TAG,"Humidity: %d%% Temp: %dC\n", humidity / 10, temperature / 10);
            
            sprintf(body, BODY_MEASUREMENT, (float) temperature/10  , (float) humidity/10 , buffer , mac_address_str_3);
            sprintf(send_buf, REQUEST_POST_MEASUREMENT, (int)strlen(body),body );
	        ESP_LOGI(TAG,"sending: \n%s\n",send_buf);
        } else {
            ESP_LOGE(TAG,"Could not read data from sensor\n");
        }

        http_post(send_buf);

        for(int countdown = 10; countdown >= 0; countdown--) {
            ESP_LOGI(TAG, "%d... ", countdown);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "Starting again!");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    xTaskCreate(&http_get_task, "http_get_task", 4096, NULL, 5, NULL);
}
