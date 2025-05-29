#include <cstring>
#include <iostream>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_modem_config.h"
#include "cxx_include/esp_modem_dte.hpp"
#include "cxx_include/esp_modem_api.hpp"
#include "cxx_include/esp_modem_dce.hpp"
#include "esp_vfs_dev.h"
#include "freertos/idf_additions.h"
#include "vfs_resource/vfs_create.hpp"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_netif_ppp.h"
#include <inttypes.h>
#include "modem_handler_cmux.h"

using namespace esp_modem;

static const char *TAG = "modem_handler_cmux";
#define DEFAULT_APN "MOUSAL" /* STATIC APN*/

#define MODEM_UART_RX_BUFFER 1024 * 32
#define MODEM_UART_TX_BUFFER 1024 * 32
#define MODEM_UART_EVENT_QUEUE_SIZE 30
#define MODEM_UART_EVENT_TASK_STACK 1024 * 5
#define MODEM_UART_EVENT_TASK_PRIORITY 9

// Event group and event bits
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int DISCONNECTION_BIT = BIT3;
static gpio_num_t reset_pin = GPIO_NUM_NC;

// Struct to hold network APN information
typedef struct
{
    char provider_name[50];
    char apn_name[100];
} NetworkAPN;

// Array of network APNs
NetworkAPN networkAPNs[] = {
    {"airtel", "airtelgprs.com"},
    {"jio", "JioNet"},
    {"ASIACELL", "net.asiacell.com"},
    {"kotek", "net.kotek.com"},
    {"zainIQ", "MOUSAL"}, /*STATIC IP APN */
};

// Function to get APN name for given provider name (case-insensitive comparison)
const char *get_apn_for_provider(const char *provider_name)
{
    // Search for the APN name by provider name (case-insensitive)
    for (size_t i = 0; i < sizeof(networkAPNs) / sizeof(networkAPNs[0]); ++i)
    {
        const char *current_provider = networkAPNs[i].provider_name;
        // Compare ignoring case
        if (strcasestr(provider_name, current_provider) != NULL)
        {
            return networkAPNs[i].apn_name;
        }
    }

    return DEFAULT_APN; // Provider name not found
}

typedef enum
{
    MODEM_STATE_RESET_MODEM,
    MODEM_STATE_CHECK_MODEM,
    MODEM_STATE_CHECK_SIM,
    MODEM_STATE_CHECK_NETWORK,
    MODEM_STATE_CHECK_SIGNAL,
    MODEM_STATE_SET_APN,
    MODEM_STATE_SET_MODE,
    MODEM_STATE_WAIT_FOR_IP,
    MODEM_STATE_CONNECTED,
    MODEM_STATE_DISCONNECTED,
} modem_state_t;

// Forward declare the task function
static void connect_ppp_task(void *pvParameters);

void modem_reset(void)
{
    if (reset_pin != GPIO_NUM_NC)
    {
        ESP_LOGI(TAG, "Resetting modem using GPIO %d", reset_pin);
        gpio_set_level(reset_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGE(TAG, "Modem Reset");
}

void modem_reconnect(void)
{
    ESP_LOGI(TAG, "Modem Disconnect");
    xEventGroupSetBits(event_group, DISCONNECTION_BIT);
    xEventGroupClearBits(event_group, CONNECT_BIT);
}

// PPP state change event handler
static void on_ppp_changed(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGE(TAG, "PPP state changed event %" PRIu32, event_id);
    if (event_id == NETIF_PPP_ERRORUSER)
    {
        // User interrupted event from esp-netif
        auto p_netif = static_cast<esp_netif_t **>(event_data);
        ESP_LOGI(TAG, "User interrupted event from netif:%p", *p_netif);
    }
}

// IP event handler
static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "IP event! %" PRIu32, event_id);
    if (event_id == IP_EVENT_PPP_GOT_IP)
    {
        esp_netif_dns_info_t dns_info;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(TAG, "Modem Connect to PPP Server");
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
        ESP_LOGI(TAG, "DNS1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info);
        ESP_LOGI(TAG, "DNS2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");

        xEventGroupSetBits(event_group, CONNECT_BIT);

        ESP_LOGI(TAG, "GOT IP event!!!");

        // operationsOnIPv4Reception(&event->ip_info.ip, &event->ip_info.gw, NT_GSM);
        // manage_uart_gpio_mode(SMART_METER_UART, 9600, SMART_METER_TX_PIN, SMART_METER_RX_PIN, NULL);
    }
    else if (event_id == IP_EVENT_PPP_LOST_IP)
    {
        ESP_LOGI(TAG, "Modem Disconnect from PPP Server");
        modem_reconnect();
        // operationsOnIPv4Lost(NT_GSM);
        // manage_uart_gpio_mode(SMART_METER_UART, 9600, SMART_METER_TX_PIN, SMART_METER_RX_PIN, NULL);
    }
    else if (event_id == IP_EVENT_GOT_IP6)
    {
        ESP_LOGI(TAG, "GOT IPv6 event!");
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

// Task to handle PPP connection
static void connect_ppp_task(void *pvParameters)
{
    auto *dce = static_cast<DCE *>(pvParameters);
    const TickType_t retry_delay = pdMS_TO_TICKS(2000);
    const TickType_t keep_alive_interval = pdMS_TO_TICKS(5000);
    modem_state_t state = MODEM_STATE_RESET_MODEM;
    int retry_count = 0;
    const int max_retries = 6;
    std::string response_buffer;
    std::string operator_name;
    std::string apn_name = DEFAULT_APN; // Initialize with the default APN

    // Create a PdpContext object with default APN
    std::unique_ptr<PdpContext> new_pdp = std::make_unique<PdpContext>(DEFAULT_APN);
    new_pdp->context_id = 1;
    new_pdp->protocol_type = "IP"; // Default to IPv4

    // Variable declarations for signal quality
    int signal = 0;
    int ber = 0;

    // Null check to ensure dce isn't null
    if (!dce)
    {
        ESP_LOGE(TAG, "DCE object is null, exiting task");
        vTaskDelete(NULL);
        return;
    }

    while (true)
    {
        // Add null check at the start of each loop
        if (!dce)
        {
            ESP_LOGE(TAG, "DCE object became null, exiting task");
            vTaskDelete(NULL);
            return;
        }

        switch (state)
        {
        case MODEM_STATE_RESET_MODEM:
            // Reset modem
            ESP_LOGI(TAG, "Resetting modem...");
            modem_reset();
            // manage_uart_gpio_mode(SMART_METER_UART, 9600, SMART_METER_TX_PIN, SMART_METER_RX_PIN, NULL);

            // vTaskDelay(pdMS_TO_TICKS(2000)); // Wait for modem to reboot
            retry_count = 0;
            xEventGroupClearBits(event_group, DISCONNECTION_BIT);
            xEventGroupClearBits(event_group, CONNECT_BIT);
            state = MODEM_STATE_CHECK_MODEM;
            break;

        case MODEM_STATE_CHECK_MODEM:
            ESP_LOGI(TAG, "Checking modem connection...");
            if (dce->at_raw("AT\r\n", response_buffer, "OK", "ERROR", 500) == command_result::OK)
            {
                ESP_LOGI(TAG, "Modem Connected");
                retry_count = 0;
                state = MODEM_STATE_CHECK_SIM;
            }
            else
            {
                retry_count++;
                ESP_LOGE(TAG, "Modem not connected");
                vTaskDelay(retry_delay);
            }
            break;
        case MODEM_STATE_CHECK_SIM:
            // Check SIM PIN status
            ESP_LOGI(TAG, "Checking SIM status...");

            if (dce->at_raw("AT+CPIN?\r\n", response_buffer, "OK", "ERROR", 1000) == command_result::OK)
            {
                if (response_buffer.find("SIM PIN") != std::string::npos || response_buffer.find("SIM PUK") != std::string::npos)
                {
                    ESP_LOGI(TAG, "SIM PIN required");
                    // Handle SIM PIN entry here if needed
                }
                else if (response_buffer.find("READY") != std::string::npos)
                {
                    ESP_LOGI(TAG, "SIM is ready");
                    retry_count = 0;
                    state = MODEM_STATE_CHECK_NETWORK;
                }
                else
                {
                    retry_count++;
                    ESP_LOGE(TAG, "Unknown SIM status: %s", response_buffer.c_str());
                    vTaskDelay(retry_delay);
                }
            }
            else
            {
                retry_count++;
                ESP_LOGE(TAG, "Failed to check SIM status");
                vTaskDelay(retry_delay);
            }
            break;
        case MODEM_STATE_CHECK_NETWORK:

            // Query network mode
            ESP_LOGI(TAG, "Querying network mode...");
            if (dce->at_raw("AT$MYNETINFO?\r\n", response_buffer, "OK", "ERROR", 5000) == command_result::OK)
            {
                ESP_LOGI(TAG, "Network Mode Information: %s", response_buffer.c_str());

                if (response_buffer.find("MYNETINFO: 1") != std::string::npos)
                {
                    ESP_LOGI(TAG, "Network Mode : Auto");
                }
                else if (response_buffer.find("MYNETINFO: 2") != std::string::npos)
                {
                    ESP_LOGI(TAG, "Network Mode : 2G");
                }
                else if (response_buffer.find("MYNETINFO: 3") != std::string::npos)
                {
                    ESP_LOGI(TAG, "Network Mode : 3G");
                }
                else if (response_buffer.find("MYNETINFO: 4") != std::string::npos)
                {
                    ESP_LOGI(TAG, "Network Mode : LTE");
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to query network mode");
            }

            // Check network registration status with "AT+CREG?" command
            ESP_LOGI(TAG, "Checking network registration...");
            if (dce->at_raw("AT+CREG?\r\n", response_buffer, "OK", "ERROR", 1000) == command_result::OK)
            {
                ESP_LOGI(TAG, "Network Registration Response: %s", response_buffer.c_str());
                // Parse CREG response format: +CREG: <n>,<stat>[,<lac>,<ci>,<AcT>]
                size_t start = response_buffer.find("+CREG: ");
                if (start != std::string::npos)
                {
                    std::string creg_params = response_buffer.substr(start + 7);
                    int n, stat;
                    if (sscanf(creg_params.c_str(), "%d,%d", &n, &stat) >= 2)
                    {
                        ESP_LOGI(TAG, "Network registration mode: %d, status: %d", n, stat);
                        // stat values:
                        // 0 = Not registered, not searching
                        // 1 = Registered, home network
                        // 2 = Not registered, searching
                        // 3 = Registration denied
                        // 4 = Unknown
                        // 5 = Registered, roaming
                        if (stat == 1 || stat == 5)
                        {
                            ESP_LOGI(TAG, "Network Registered");
                            retry_count = 0;
                            state = MODEM_STATE_CHECK_SIGNAL;
                        }else{
                            retry_count++;
                            vTaskDelay(retry_delay);
                        }
                    }
                }
                else
                {
                    dce->at_raw("AT+CREG=1\r\n", response_buffer, "OK", "ERROR", 1000);
                    ESP_LOGE(TAG, "Failed to check network registration");
                    vTaskDelay(retry_delay);
                }
            }
            else
            {
                retry_count++;
                ESP_LOGE(TAG, "Failed to check network registration");
                vTaskDelay(retry_delay);
            }
            break;
        case MODEM_STATE_CHECK_SIGNAL:
            // Check signal quality
            ESP_LOGI(TAG, "Checking signal quality...");
            if (dce->get_signal_quality(signal, ber) == command_result::OK)
            {
                ESP_LOGI(TAG, "Signal quality: signal=%d, ber=%d", signal, ber);
                state = MODEM_STATE_SET_APN;
            }
            else
            {
                retry_count++;
                ESP_LOGE(TAG, "Signal quality too poor or failed to get signal quality: rssi=%d, ber=%d", signal, ber);
                vTaskDelay(retry_delay);
            }
            break;
        case MODEM_STATE_SET_APN:
            // Get operator name
            ESP_LOGI(TAG, "Getting operator name...");

            if (dce->get_operator_name(operator_name) == command_result::OK)
            {
                ESP_LOGI(TAG, "Operator Name: %s", operator_name.c_str());
                ESP_LOGI(TAG, "Getting APN for provider: %s", operator_name.c_str());

                apn_name = get_apn_for_provider(operator_name.c_str());
                ESP_LOGI(TAG, "APN: %s", apn_name.c_str());
            }
            else
            {
                // Default to static APN if operator name retrieval fails
                apn_name = DEFAULT_APN;
                ESP_LOGW(TAG, "Failed to get operator name, using default APN: %s", apn_name.c_str());
            }

            // Set APN with IPv4 protocol by default
            ESP_LOGI(TAG, "Setting APN...");

            // Update the PDP context with new APN
            new_pdp = std::make_unique<PdpContext>(apn_name);
            if (!new_pdp)
            {
                ESP_LOGE(TAG, "Failed to create PDP context");
                retry_count++;
                vTaskDelay(retry_delay);
                break;
            }

            new_pdp->context_id = 1;
            new_pdp->protocol_type = "IP"; // Use IP for IPv4

            // Configure the DCE with the new PDP context
            if (dce->set_pdp_context(*new_pdp) == command_result::OK)
            {
                ESP_LOGI(TAG, "APN Set Successfully");
                retry_count = 0;
                state = MODEM_STATE_SET_MODE;
            }
            else
            {
                retry_count++;
                ESP_LOGE(TAG, "Failed to set APN");
                vTaskDelay(retry_delay);
            }
            break;
        case MODEM_STATE_SET_MODE:
            ESP_LOGI(TAG, "Setting mode...");
            if (dce->set_mode(esp_modem::modem_mode::CMUX_MODE))
            {
                ESP_LOGI(TAG, "Mode set successfully");
                retry_count = 0;
                state = MODEM_STATE_WAIT_FOR_IP;
            }
            else
            {
                retry_count++;
                ESP_LOGE(TAG, "Failed to set mode");
                vTaskDelay(retry_delay);
            }
            break;
        case MODEM_STATE_WAIT_FOR_IP:
            ESP_LOGI(TAG, "Waiting for IP...");
            xEventGroupWaitBits(event_group, CONNECT_BIT | DISCONNECTION_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
            if (xEventGroupGetBits(event_group) & CONNECT_BIT)
            {
                ESP_LOGI(TAG, "IP received, continuing...");
                retry_count = 0;
                state = MODEM_STATE_CONNECTED;
            }
            else
            {
                ESP_LOGE(TAG, "IP not received after 10 seconds");
                state = MODEM_STATE_DISCONNECTED;
            }
            break;
        case MODEM_STATE_CONNECTED:
            // ESP_LOGI(TAG, "Connected to PPP Server");

            // Check if the connection is still active
            if (dce->at_raw("AT\r\n", response_buffer, "OK", "ERROR", 2000) == command_result::OK)
            {
                ESP_LOGI(TAG, "Modem connection is still active");
                if (dce->get_signal_quality(signal, ber) == command_result::OK)
                {
                    // Convert signal quality to dBm
                    int dBm = signal * 2 - 113;
                    ESP_LOGI(TAG, "Signal quality: rssi=%d dBm, ber=%d", dBm, ber);
                }
                retry_count = 0;
                if (xEventGroupGetBits(event_group) & DISCONNECTION_BIT)
                {
                    ESP_LOGI(TAG, "PPP disconnected");
                    state = MODEM_STATE_DISCONNECTED;
                }
            }
            else
            {

                retry_count++;
                ESP_LOGE(TAG, "Modem not responding. Probably disconnected");
                if (retry_count >= max_retries)
                {
                    ESP_LOGE(TAG, "Max retries reached...");
                    state = MODEM_STATE_DISCONNECTED;
                }
            }

            vTaskDelay(keep_alive_interval);
            break;
        case MODEM_STATE_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from PPP Server");
            ESP_LOGI(TAG, "Restarting esp to recover from modem disconnection...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            break;
        default:
            ESP_LOGE(TAG, "Unknown state: %d", state);
            state = MODEM_STATE_RESET_MODEM;
            break;
        }

        if (retry_count >= max_retries)
        {
            ESP_LOGE(TAG, "Max retries reached. Resetting modem...");
            state = MODEM_STATE_RESET_MODEM;
        }
    }
}

/**
 * @brief Set the PDP context with specific IP protocol type
 */
bool set_pdp_context(int pdp_context_id, const char *apn_name, pdp_ip_type_t ip_type)
{
    ESP_LOGI(TAG, "Setting PDP context with ID: %d, APN: %s", pdp_context_id, apn_name);

    // Select protocol type string based on the requested IP type
    const char *protocol_type = "IP"; // Default to IPv4

    switch (ip_type)
    {
    case PDP_IP_TYPE_IPV4:
        ESP_LOGI(TAG, "Setting PDP context with IPv4 protocol");
        protocol_type = "IP";
        break;
    case PDP_IP_TYPE_IPV6:
        ESP_LOGI(TAG, "Setting PDP context with IPv6 protocol");
        protocol_type = "IPV6";
        break;
    case PDP_IP_TYPE_IPV4V6:
        ESP_LOGI(TAG, "Setting PDP context with dual stack IPv4/IPv6 protocol");
        protocol_type = "IPV4V6";
        break;
    default:
        ESP_LOGW(TAG, "Unknown IP type, defaulting to IPv4");
        protocol_type = "IP";
        break;
    }

    // Create a PdpContext with the appropriate settings
    std::unique_ptr<esp_modem::PdpContext> new_pdp = std::make_unique<esp_modem::PdpContext>(apn_name);
    new_pdp->context_id = pdp_context_id;
    new_pdp->protocol_type = protocol_type;

    // Return success - the actual AT command will be sent by the caller with DCE access
    return true;
}

void init_modem_cmux(int tx_pin, int rx_pin, gpio_num_t rst_pin, int baudrate)
{
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    assert(esp_netif);

    event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Initializing modem with CMUX...");

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << rst_pin);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    reset_pin = rst_pin;
    // modem_reset();

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = tx_pin;
    dte_config.uart_config.rx_io_num = rx_pin;
    dte_config.uart_config.baud_rate = baudrate;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config.uart_config.rx_buffer_size = MODEM_UART_RX_BUFFER;
    dte_config.uart_config.tx_buffer_size = MODEM_UART_TX_BUFFER;
    dte_config.uart_config.event_queue_size = MODEM_UART_EVENT_QUEUE_SIZE;
    dte_config.task_stack_size = MODEM_UART_EVENT_TASK_STACK;
    dte_config.task_priority = MODEM_UART_EVENT_TASK_PRIORITY;
    dte_config.dte_buffer_size = MODEM_UART_RX_BUFFER / 2;

    // Create static DTE and DCE objects that will persist beyond this function
    static std::shared_ptr<DTE> dte = create_uart_dte(&dte_config);
    assert(dte);

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(DEFAULT_APN);
    static std::shared_ptr<DCE> dce = create_generic_dce(&dce_config, dte, esp_netif);
    assert(dce);

    // Create a task to handle the modem connection, passing the DCE
    xTaskCreate(connect_ppp_task, "modem_task", 1024 * 5, dce.get(), 8, NULL);
    ESP_LOGI(TAG, "Modem initialized and ready for CMUX communication");
}
