/* Console example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "linenoise/linenoise.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "dhcpserver/dhcpserver.h"

#include "cmd_decl.h"
#include <esp_http_server.h>

#if !IP_NAPT
#error "IP_NAPT must be defined"
#endif
#include "lwip/lwip_napt.h"

#include "router_globals.h"
#include "modem_handler_cmux.h"

// On board LED
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define BLINK_GPIO 44
#else
#define BLINK_GPIO 2
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

#define DEFAULT_AP_IP "192.168.4.1"
#define DEFAULT_DNS "8.8.8.8"

/* Global vars */
uint16_t connect_count = 0;
bool ap_connect = false;
bool has_static_ip = false;

uint32_t my_ip;
uint32_t my_ap_ip;

struct portmap_table_entry {
  u32_t daddr;
  u16_t mport;
  u16_t dport;
  u8_t proto;
  u8_t valid;
};
struct portmap_table_entry portmap_tab[IP_PORTMAP_MAX];

esp_netif_t* wifiAP;
esp_netif_t* wifiSTA;

httpd_handle_t start_webserver(void);

static const char *TAG = "ESP32 NAT router";

/* Console command history can be stored to and loaded from a file.
 * The easiest way to do this is to use FATFS filesystem on top of
 * wear_levelling library.
 */
#if CONFIG_STORE_HISTORY

#define MOUNT_PATH "/data"
#define HISTORY_PATH MOUNT_PATH "/history.txt"

static void initialize_filesystem(void)
{
    static wl_handle_t wl_handle;
    const esp_vfs_fat_mount_config_t mount_config = {
            .max_files = 4,
            .format_if_mount_failed = true
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_PATH, "storage", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
        return;
    }
}
#endif // CONFIG_STORE_HISTORY

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

esp_err_t apply_portmap_tab() {
    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid) {
            ip_portmap_add(portmap_tab[i].proto, my_ip, portmap_tab[i].mport, portmap_tab[i].daddr, portmap_tab[i].dport);
        }
    }
    return ESP_OK;
}

esp_err_t delete_portmap_tab() {
    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid) {
            ip_portmap_remove(portmap_tab[i].proto, portmap_tab[i].mport);
        }
    }
    return ESP_OK;
}

void print_portmap_tab() {
    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid) {
            printf ("%s", portmap_tab[i].proto == PROTO_TCP?"TCP ":"UDP ");
            ip4_addr_t addr;
            addr.addr = my_ip;
            printf (IPSTR":%d -> ", IP2STR(&addr), portmap_tab[i].mport);
            addr.addr = portmap_tab[i].daddr;
            printf (IPSTR":%d\n", IP2STR(&addr), portmap_tab[i].dport);
        }
    }
}

esp_err_t get_portmap_tab() {
    esp_err_t err;
    nvs_handle_t nvs;
    size_t len;

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(nvs, "portmap_tab", NULL, &len);
    if (err == ESP_OK) {
        if (len != sizeof(portmap_tab)) {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            err = nvs_get_blob(nvs, "portmap_tab", portmap_tab, &len);
            if (err != ESP_OK) {
                memset(portmap_tab, 0, sizeof(portmap_tab));
            }
        }
    }
    nvs_close(nvs);

    return err;
}

esp_err_t add_portmap(u8_t proto, u16_t mport, u32_t daddr, u16_t dport) {
    esp_err_t err;
    nvs_handle_t nvs;

    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (!portmap_tab[i].valid) {
            portmap_tab[i].proto = proto;
            portmap_tab[i].mport = mport;
            portmap_tab[i].daddr = daddr;
            portmap_tab[i].dport = dport;
            portmap_tab[i].valid = 1;

            err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                return err;
            }
            err = nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab));
            if (err == ESP_OK) {
                err = nvs_commit(nvs);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "New portmap table stored.");
                }
            }
            nvs_close(nvs);

            ip_portmap_add(proto, my_ip, mport, daddr, dport);

            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t del_portmap(u8_t proto, u16_t mport) {
    esp_err_t err;
    nvs_handle_t nvs;

    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid && portmap_tab[i].mport == mport && portmap_tab[i].proto == proto) {
            portmap_tab[i].valid = 0;

            err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                return err;
            }
            err = nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab));
            if (err == ESP_OK) {
                err = nvs_commit(nvs);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "New portmap table stored.");
                }
            }
            nvs_close(nvs);

            ip_portmap_remove(proto, mport);
            return ESP_OK;
        }
    }
    return ESP_OK;
}


void * led_status_thread(void * p)
{
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while (true)
    {
        gpio_set_level(BLINK_GPIO, ap_connect);

        for (int i = 0; i < connect_count; i++)
        {
            gpio_set_level(BLINK_GPIO, 1 - ap_connect);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, ap_connect);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    esp_netif_dns_info_t dns;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG,"disconnected - retry to connect to the AP");
        ap_connect = false;
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        ap_connect = true;
        my_ip = event->ip_info.ip.addr;
        delete_portmap_tab();
        apply_portmap_tab();
        if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        {
            esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
            ESP_LOGI(TAG, "set dns to:" IPSTR, IP2STR(&(dns.ip.u_addr.ip4)));
        }
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        connect_count++;
        ESP_LOGI(TAG,"%d. station connected", connect_count);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        connect_count--;
        ESP_LOGI(TAG,"station disconnected - %d remain", connect_count);
    }
}

const int CONNECTED_BIT = BIT0;
#define JOIN_TIMEOUT_MS (2000)


void wifi_init(const uint8_t* mac, const char* ssid, const char* ent_username, const char* ent_identity, const char* passwd, const char* static_ip, const char* subnet_mask, const char* gateway_addr, const uint8_t* ap_mac, const char* ap_ssid, const char* ap_passwd, const char* ap_ip)
{
    esp_netif_dns_info_t dnsserver;
    // esp_netif_dns_info_t dnsinfo;

    wifi_event_group = xEventGroupCreate();
  
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();


    my_ap_ip = esp_ip4addr_aton(ap_ip);

    esp_netif_ip_info_t ipInfo_ap;
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;
    esp_netif_set_ip4_addr(&ipInfo_ap.netmask, 255,255,255,0);
    esp_netif_dhcps_stop(wifiAP); // stop before setting ip WifiAP
    esp_netif_set_ip_info(wifiAP, &ipInfo_ap);
    esp_netif_dhcps_start(wifiAP);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* ESP WIFI CONFIG */
    wifi_config_t wifi_config = { 0 };
        wifi_config_t ap_config = {
        .ap = {
            .channel = 0,
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .ssid_hidden = 0,
            .max_connection = 8,
            .beacon_interval = 100,
        }
    };

    strlcpy((char*)ap_config.sta.ssid, ap_ssid, sizeof(ap_config.sta.ssid));
    if (strlen(ap_passwd) < 8) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
	    strlcpy((char*)ap_config.sta.password, ap_passwd, sizeof(ap_config.sta.password));
    }

    if (strlen(ssid) > 0) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA) );

        //Set SSID
        strlcpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        //Set passwprd
        if(strlen(ent_username) == 0) {
            ESP_LOGI(TAG, "STA regular connection");
            strlcpy((char*)wifi_config.sta.password, passwd, sizeof(wifi_config.sta.password));
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
        if(strlen(ent_username) != 0 && strlen(ent_identity) != 0) {
            ESP_LOGI(TAG, "STA enterprise connection");
            if(strlen(ent_username) != 0 && strlen(ent_identity) != 0) {
                esp_eap_client_set_identity((uint8_t *)ent_identity, strlen(ent_identity)); //provide identity
            } else {
                esp_eap_client_set_identity((uint8_t *)ent_username, strlen(ent_username));
            }
            esp_eap_client_set_username((uint8_t *)ent_username, strlen(ent_username)); //provide username
            esp_eap_client_set_password((uint8_t *)passwd, strlen(passwd)); //provide password
            esp_wifi_sta_enterprise_enable();
        }

        if (mac != NULL) {
            ESP_ERROR_CHECK(esp_wifi_set_mac(ESP_IF_WIFI_STA, mac));
        }
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP) );
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config) );

    if (ap_mac != NULL) {
        ESP_ERROR_CHECK(esp_wifi_set_mac(ESP_IF_WIFI_AP, ap_mac));
    }


    // Enable DNS (offer) for dhcp server
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_option(wifiAP,ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value));

    // // Set custom dns server address for dhcp server
    dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton(DEFAULT_DNS);
    dnsserver.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dnsserver);

    // esp_netif_get_dns_info(ESP_IF_WIFI_AP, ESP_NETIF_DNS_MAIN, &dnsinfo);
    // ESP_LOGI(TAG, "DNS IP:" IPSTR, IP2STR(&dnsinfo.ip.u_addr.ip4));

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
        pdFALSE, pdTRUE, JOIN_TIMEOUT_MS / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(esp_wifi_start());

    if (strlen(ssid) > 0) {
        ESP_LOGI(TAG, "wifi_init_apsta finished.");
        ESP_LOGI(TAG, "connect to ap SSID: %s ", ssid);
    } else {
        ESP_LOGI(TAG, "wifi_init_ap with default finished.");      
    }
}

uint8_t* mac = NULL;
char* ssid = "";
char* ent_username = "";
char* ent_identity = "";
char* passwd = "";
char* static_ip = "";
char* subnet_mask = "";
char* gateway_addr = "";
uint8_t* ap_mac = NULL;
char* ap_ssid = "ESP32-AP";
char* ap_passwd = "esp32ap123";
char* ap_ip = "192.168.4.1";

char* param_set_default(const char* def_val) {
    char * retval = malloc(strlen(def_val)+1);
    strcpy(retval, def_val);
    return retval;
}

#define GSM_UART_TX 5 // 12
#define GSM_UART_RX  34
#define GSM_RST_PIN GPIO_NUM_14
#define GSM_BAUDRATE 115200

void app_main(void)
{
    initialize_nvs();

    // get_portmap_tab();

    // Setup WIFI
    wifi_init(mac, ssid, ent_username, ent_identity, passwd, static_ip, subnet_mask, gateway_addr, ap_mac, ap_ssid, ap_passwd, ap_ip);

    init_modem_cmux(GSM_UART_TX, GSM_UART_RX, GSM_RST_PIN, GSM_BAUDRATE);

    ip_napt_enable(my_ap_ip, 1);
    ESP_LOGI(TAG, "NAT is enabled");

}
