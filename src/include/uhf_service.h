/* uhf_service.h - Public API for UHF Reader (event-driven, Ethernet based) */

#ifndef UHF_SERVICE_H
#define UHF_SERVICE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UHF_MAX_PAYLOAD_LEN 256

// Report callback function type
typedef void (*UHFReportCallback)(uint8_t reader_id, uint8_t cmd, const uint8_t *data, size_t len);

// UHF Reader handle structure (opaque to user)
typedef struct UHFClient UHFClient;

// Initialize the system (start TCP client connection to UHF reader)
int uhf_client_init(UHFClient **handle, const char *ip, uint16_t port, UHFReportCallback callback);

// Clean up and disconnect
void uhf_client_deinit(UHFClient *handle);

// Send command to reader
int uhf_client_send_command(UHFClient *handle, uint8_t cmd, const uint8_t *payload, size_t len);

// Optional helper to build packets (add CRC, header etc.)
int uhf_build_command_packet(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out_buf, size_t *out_len);

// Start background thread to receive and dispatch reports
int uhf_client_start(UHFClient *handle);

// Stop background thread
void uhf_client_stop(UHFClient *handle);

// High-level UHF operations
int uhf_client_inventory_once(UHFClient *handle);
int uhf_client_read_tag(UHFClient *handle, uint8_t bank, uint16_t offset, uint8_t length, const uint8_t *password);
int uhf_client_write_tag(UHFClient *handle, uint8_t bank, uint16_t offset, const uint8_t *data, uint8_t length, const uint8_t *password);
int uhf_client_lock_tag(UHFClient *handle, uint8_t bank, uint8_t lock_type, const uint8_t *password);
int uhf_client_kill_tag(UHFClient *handle, const uint8_t *kill_password);

int uhf_client_set_rssi_filter(UHFClient *handle, uint8_t min_rssi);
int uhf_client_set_tag_filter(UHFClient *handle, const uint8_t *epc, uint8_t epc_len);
int uhf_client_get_trigger_mode_stop_delay(UHFClient *handle);
int uhf_client_start_auto_read(UHFClient *handle);
int uhf_client_stop_auto_read(UHFClient *handle);


#ifdef __cplusplus
}
#endif

#endif // UHF_SERVICE_H
