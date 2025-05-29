// Add PDP context IP type enumeration and set_pdp_context function declaration
#ifndef MODEM_HANDLER_CMUX_H
#define MODEM_HANDLER_CMUX_H

#include <stdint.h>

// PDP context IP type enumeration
typedef enum {
    PDP_IP_TYPE_IPV4 = 0,    // IPv4 protocol type
    PDP_IP_TYPE_IPV6 = 1,    // IPv6 protocol type
    PDP_IP_TYPE_IPV4V6 = 2,  // Dual stack IPv4 and IPv6
} pdp_ip_type_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the modem with CMUX (multiplexed command/data mode)
 * 
 * @param tx_pin UART TX pin
 * @param rx_pin UART RX pin
 * @param rst_pin Reset pin
 * @param baudrate UART baudrate
 */
void init_modem_cmux(int tx_pin, int rx_pin, gpio_num_t rst_pin, int baudrate);

/**
 * @brief Set the PDP context with specific IP protocol type
 * 
 * This function creates a PDP context with the specified settings.
 * The PDP context defines how the modem connects to the cellular network,
 * including the protocol type (IPv4, IPv6, or dual stack).
 * 
 * Usage examples:
 * 
 * 1. For IPv4 connection:
 *    set_pdp_context(1, "internet", PDP_IP_TYPE_IPV4);
 * 
 * 2. For IPv6 connection:
 *    set_pdp_context(1, "internet", PDP_IP_TYPE_IPV6);
 * 
 * 3. For dual stack IPv4/IPv6 connection:
 *    set_pdp_context(1, "internet", PDP_IP_TYPE_IPV4V6);
 * 
 * @param pdp_context_id PDP context ID (typically 1)
 * @param apn_name APN name
 * @param ip_type IP protocol type (IPv4, IPv6, or dual stack)
 * @return true if successful, false otherwise
 */
bool set_pdp_context(int pdp_context_id, const char* apn_name, pdp_ip_type_t ip_type);

/**
 * @brief Reset the modem
 */
void modem_reset(void);

#ifdef __cplusplus
}
#endif

#endif // MODEM_HANDLER_CMUX_H
