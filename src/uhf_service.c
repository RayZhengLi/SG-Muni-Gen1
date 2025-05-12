/* uhf_service.c - Event-driven UHF Reader implementation */

#include "uhf_service.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <stdio.h>

#define PACKET_HEADER 0xA0
#define PACKET_MAX_LEN 512
#define EPOLL_MAX_EVENTS 4

#define CMD_INVENTORY_ONCE 0x01
#define CMD_READ_TAG       0x02
#define CMD_WRITE_TAG      0x03
#define CMD_LOCK_TAG       0x04
#define CMD_KILL_TAG       0x05
#define CMD_SET_RSSI       0x06
#define CMD_SET_FILTER     0x07
#define CMD_GET_TRIGGER    0x08
#define CMD_START_AUTO     0x09
#define CMD_STOP_AUTO      0x0A
#define CMD_AUTO_PARAM     0x74

struct UHFClient {
    int sockfd;
    pthread_t thread;
    int running;
    UHFReportCallback callback;
    uint8_t last_cmd_sent;
};

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int verify_bcc(const uint8_t *buf, size_t len) {
    if (len < 4) return 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < len - 1; ++i) sum += buf[i];
    sum = ~sum + 1;
    return sum == buf[len - 1];
}


int uhf_client_init(UHFClient **handle, const char *ip, uint16_t port, UHFReportCallback callback) {
    if (!handle || !ip || !callback) return -1;

    UHFClient *cli = calloc(1, sizeof(UHFClient));
    if (!cli) return -1;

    cli->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cli->sockfd < 0) {
        free(cli);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(cli->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(cli->sockfd);
        free(cli);
        return -1;
    }

    set_nonblocking(cli->sockfd);
    cli->callback = callback;
    cli->running = 0;
    *handle = cli;
    return 0;
}

void uhf_client_deinit(UHFClient *handle) {
    if (!handle) return;
    uhf_client_stop(handle);
    close(handle->sockfd);
    free(handle);
}

int uhf_client_send_command(UHFClient *handle, uint8_t cmd, const uint8_t *payload, size_t len) {
    if (!handle || handle->sockfd < 0) return -1;
    uint8_t packet[PACKET_MAX_LEN] = {0};
    size_t total = 0;
    if (uhf_build_command_packet(cmd, payload, len, packet, &total) != 0) return -1;
    handle->last_cmd_sent = cmd;
    return send(handle->sockfd, packet, total, 0);
}

int uhf_build_command_packet(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out_buf, size_t *out_len) {
    if (!out_buf || !out_len || len > UHF_MAX_PAYLOAD_LEN) return -1;
    out_buf[0] = PACKET_HEADER;
    out_buf[1] = len + 2; // cmd + data
    out_buf[2] = cmd;
    memcpy(&out_buf[3], payload, len);
    uint8_t bcc = 0;
    for (size_t i = 0; i < len + 3; ++i) bcc += out_buf[i];
    out_buf[3 + len] = (~bcc) + 1;
    *out_len = 4 + len;
    return 0;
}

static void* recv_thread(void *arg) {
    UHFClient *cli = (UHFClient*)arg;
    int epfd = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = cli->sockfd};
    epoll_ctl(epfd, EPOLL_CTL_ADD, cli->sockfd, &ev);

    uint8_t buf[PACKET_MAX_LEN];
    struct epoll_event events[EPOLL_MAX_EVENTS];

    while (cli->running) {
        int nfds = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 1000);
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            int len = read(fd, buf, sizeof(buf));
            if (len > 4 && buf[0] == PACKET_HEADER && verify_bcc(buf, len)) {
                uint8_t cmd = buf[2];
                uint8_t *data = &buf[3];
                size_t datalen = len - 4;
                cli->callback(0x01, cmd, data, datalen);
            }
        }
    }
    close(epfd);
    return NULL;
}

int uhf_client_start(UHFClient *handle) {
    if (!handle || handle->running) return -1;
    handle->running = 1;
    return pthread_create(&handle->thread, NULL, recv_thread, handle);
}

void uhf_client_stop(UHFClient *handle) {
    if (!handle || !handle->running) return;
    handle->running = 0;
    pthread_join(handle->thread, NULL);
}

int uhf_client_inventory_once(UHFClient *handle) {
    return uhf_client_send_command(handle, CMD_INVENTORY_ONCE, NULL, 0);
}

int uhf_client_read_tag(UHFClient *handle, uint8_t bank, uint16_t offset, uint8_t length, const uint8_t *password) {
    if (!password) return -1;
    uint8_t payload[8] = {0};
    payload[0] = bank;
    payload[1] = (offset >> 8) & 0xFF;
    payload[2] = offset & 0xFF;
    payload[3] = length;
    memcpy(&payload[4], password, 4);
    return uhf_client_send_command(handle, CMD_READ_TAG, payload, 8);
}

int uhf_client_write_tag(UHFClient *handle, uint8_t bank, uint16_t offset, const uint8_t *data, uint8_t length, const uint8_t *password) {
    if (!data || !password || length == 0 || length > 32) return -1;
    uint8_t payload[40] = {0};
    payload[0] = bank;
    payload[1] = (offset >> 8) & 0xFF;
    payload[2] = offset & 0xFF;
    payload[3] = length;
    memcpy(&payload[4], password, 4);
    memcpy(&payload[8], data, length);
    return uhf_client_send_command(handle, CMD_WRITE_TAG, payload, 8 + length);
}

int uhf_client_lock_tag(UHFClient *handle, uint8_t bank, uint8_t lock_type, const uint8_t *password) {
    if (!password) return -1;
    uint8_t payload[6] = {0};
    payload[0] = bank;
    payload[1] = lock_type;
    memcpy(&payload[2], password, 4);
    return uhf_client_send_command(handle, CMD_LOCK_TAG, payload, 6);
}

int uhf_client_kill_tag(UHFClient *handle, const uint8_t *kill_password) {
    if (!kill_password) return -1;
    return uhf_client_send_command(handle, CMD_KILL_TAG, kill_password, 4);
}

int uhf_client_set_rssi_filter(UHFClient *handle, uint8_t min_rssi) {
    return uhf_client_send_command(handle, CMD_SET_RSSI, &min_rssi, 1);
}

int uhf_client_set_tag_filter(UHFClient *handle, const uint8_t *epc, uint8_t epc_len) {
    if (!epc || epc_len == 0 || epc_len > 32) return -1;
    return uhf_client_send_command(handle, CMD_SET_FILTER, epc, epc_len);
}

int uhf_client_get_trigger_mode_stop_delay(UHFClient *handle) {
    return uhf_client_send_command(handle, CMD_GET_TRIGGER, NULL, 0);
}

int uhf_client_start_auto_read(UHFClient *handle) {
    return uhf_client_send_command(handle, CMD_START_AUTO, NULL, 0);
}

int uhf_client_stop_auto_read(UHFClient *handle) {
    return uhf_client_send_command(handle, CMD_STOP_AUTO, NULL, 0);
}

int uhf_client_set_auto_read_params(UHFClient *handle, uint8_t reader_id, uint8_t bank, uint16_t offset, uint8_t length, const uint8_t *access_password, size_t access_password_len) {
    if (!handle || !access_password || access_password_len != 4) return -1;
    uint8_t payload[8] = {0};
    payload[0] = reader_id;
    payload[1] = bank;
    payload[2] = (offset >> 8) & 0xFF;
    payload[3] = offset & 0xFF;
    payload[4] = length;
    memcpy(&payload[5], access_password, 4);
    return uhf_client_send_command(handle, CMD_AUTO_PARAM, payload, 9);
}