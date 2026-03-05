#ifdef RG_ENABLE_NETPLAY

#include <freertos/FreeRTOS.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <nvs_flash.h>

#include "rg_system.h"
#include "rg_netplay.h"

#define NETPLAY_VERSION 0x01
#define MAX_PLAYERS     8

#define BROADCAST (inet_addr(WIFI_BROADCAST_ADDR))

// The SSID should be randomized to avoid conflicts
#define WIFI_SSID               "RETRO-GO"
#define WIFI_CHANNEL            6
#define WIFI_BROADCAST_ADDR     "192.168.4.255"
#define WIFI_NETPLAY_PORT       1234
#define NETPLAY_SYNC_TIMEOUT_MS 500

// Test to skip the network task and semaphores
#define NETPLAY_SYNCHRONOUS_TEST

static netplay_status_t netplay_status = NETPLAY_STATUS_NOT_INIT;
static netplay_mode_t netplay_mode = NETPLAY_MODE_NONE;
static netplay_callback_t netplay_callback = NULL;
static SemaphoreHandle_t netplay_sync;
// static bool netplay_available = false;

static netplay_player_t players[MAX_PLAYERS];
static netplay_player_t *local_player;
static netplay_player_t *remote_player; // This only works in 2 player mode

static esp_netif_t *netif_sta;
static esp_netif_t *netif_ap;

static QueueHandle_t espnow_rx_queue = NULL;
static QueueHandle_t netplay_queue = NULL;
static SemaphoreHandle_t netplay_send_mutex = NULL;
static uint8_t local_seq = 0;
static uint8_t remote_last_seq[MAX_PLAYERS] = {0};
static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct
{
    uint8_t mac_addr[6];
    netplay_packet_t packet;
} espnow_packet_t;


static void dummy_netplay_callback(netplay_event_t event, void *arg)
{
    RG_LOGI("...\n");
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (len < 4 || len > sizeof(netplay_packet_t) || !espnow_rx_queue)
        return;
    espnow_packet_t pkt;
    memcpy(pkt.mac_addr, recv_info->src_addr, 6);
    memcpy(&pkt.packet, data, len);
    xQueueSend(espnow_rx_queue, &pkt, 0);
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    // Optionally log send failures here if needed
}

static void set_status(netplay_status_t status)
{
    bool changed = status != netplay_status;

    netplay_status = status;

    if (changed)
    {
        (*netplay_callback)(RG_EVENT_TYPE_NETPLAY | NETPLAY_EVENT_STATUS_CHANGED, &netplay_status);
    }
}


static inline bool receive_packet(netplay_packet_t *packet, int timeout_ms)
{
    if (!netplay_queue)
        return false;

    int64_t start_time = rg_system_timer();
    TickType_t wait_ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);

    while (1)
    {
        if (xQueueReceive(netplay_queue, packet, wait_ticks) != pdPASS)
        {
            return false;
        }

        if (packet->player_id < MAX_PLAYERS)
        {
            players[packet->player_id].last_contact = rg_system_timer();
        }
        return true;
    }
}

static inline void send_packet(uint32_t dest, uint8_t cmd, uint8_t arg, void *data, uint8_t data_len)
{
    if (!local_player)
        return;
    netplay_packet_t packet = {.player_id = local_player->id, .cmd = cmd, .arg = arg, .data_len = data_len};
    size_t len = sizeof(packet) - sizeof(packet.data) + data_len;

    if (arg == 0 && cmd > NETPLAY_PACKET_READY)
    {
        packet.arg = ++local_seq;
        if (packet.arg == 0)
            packet.arg = 1;
    }

    if (data_len > 0)
        memcpy(packet.data, data, data_len);

    const uint8_t *target_mac = broadcast_mac;
    if (dest < MAX_PLAYERS)
    {
        target_mac = players[dest].mac_addr;
        if (!esp_now_is_peer_exist(target_mac))
            target_mac = broadcast_mac;
    }

    if (netplay_send_mutex)
        xSemaphoreTake(netplay_send_mutex, portMAX_DELAY);

    esp_now_send(target_mac, (uint8_t *)&packet, len);

    if (cmd <= NETPLAY_PACKET_SYNC_DONE)
        esp_now_send(target_mac, (uint8_t *)&packet, len);

    if (netplay_send_mutex)
        xSemaphoreGive(netplay_send_mutex);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && (event_id == WIFI_EVENT_STA_STOP || event_id == WIFI_EVENT_AP_STOP))
    {
        set_status(NETPLAY_STATUS_STOPPED);
    }
}

static void netplay_task()
{
    espnow_packet_t rx_pkt;
    netplay_packet_t *packet = &rx_pkt.packet;
    int64_t last_handshake_broadcast = 0;

    RG_LOGI("netplay: Task started!\n");

    while (1)
    {
        if (netplay_status == NETPLAY_STATUS_HANDSHAKE || netplay_status == NETPLAY_STATUS_LISTENING)
        {
            if (rg_system_timer() - last_handshake_broadcast > 500 * 1000)
            {
                send_packet(MAX_PLAYERS, NETPLAY_PACKET_INFO, 0, (void *)local_player, sizeof(netplay_player_t));
                last_handshake_broadcast = rg_system_timer();
            }
        }

        if (netplay_status < NETPLAY_STATUS_LISTENING || !espnow_rx_queue)
        {
            rg_task_delay(50);
            continue;
        }

        if (xQueueReceive(espnow_rx_queue, &rx_pkt, pdMS_TO_TICKS(50)) != pdPASS)
        {
            continue;
        }

        if (packet->player_id >= MAX_PLAYERS || (local_player && packet->player_id == local_player->id))
        {
            continue;
        }

        netplay_player_t *packet_from = &players[packet->player_id];

        if (memcmp(packet_from->mac_addr, rx_pkt.mac_addr, 6) != 0 || !esp_now_is_peer_exist(rx_pkt.mac_addr))
        {
            memcpy(packet_from->mac_addr, rx_pkt.mac_addr, 6);
            if (!esp_now_is_peer_exist(rx_pkt.mac_addr))
            {
                esp_now_peer_info_t peer = {0};
                peer.channel = WIFI_CHANNEL;
                peer.ifidx = ESP_IF_WIFI_STA;
                peer.encrypt = false;
                memcpy(peer.peer_addr, rx_pkt.mac_addr, 6);
                esp_now_add_peer(&peer);
            }
        }
        packet_from->last_contact = rg_system_timer();

        if (packet->cmd >= NETPLAY_PACKET_SYNC_REQ && packet->cmd <= NETPLAY_PACKET_RAW_DATA)
        {
            if (netplay_queue)
            {
                if (xQueueSend(netplay_queue, packet, 0) != pdPASS)
                {
                    netplay_packet_t tmp;
                    xQueueReceive(netplay_queue, &tmp, 0);
                    xQueueSend(netplay_queue, packet, 0);
                }
            }
            continue;
        }

        switch (packet->cmd)
        {
        case NETPLAY_PACKET_INFO:
            if (packet->data_len != sizeof(netplay_player_t))
                break;

            memcpy(packet_from, packet->data, packet->data_len);
            memcpy(packet_from->mac_addr, rx_pkt.mac_addr, 6);
            remote_player = packet_from;

            RG_LOGI("netplay: Remote update id=%d game_id=%08lX\n", packet_from->id,
                    (unsigned long)packet_from->game_id);

            // If we receive a broadcast (arg=0), always reply with our info (arg=1)
            if (packet->arg == 0)
            {
                send_packet(packet_from->id, NETPLAY_PACKET_INFO, 1, (void *)local_player, sizeof(netplay_player_t));
            }

            if (packet_from->version != NETPLAY_VERSION)
            {
                RG_LOGE("netplay: protocol version mismatch!\n");
                break;
            }

            if (netplay_mode == NETPLAY_MODE_HOST)
            {
                // Host sends READY once it knows about the Guest.
                // Resend on every INFO to handle packet loss.
                send_packet(packet_from->id, NETPLAY_PACKET_READY, 0, 0, 0);
                set_status(NETPLAY_STATUS_CONNECTED);
            }
            break;

        case NETPLAY_PACKET_GAME_PAUSE:
            RG_LOGI("netplay: Received PAUSE from peer\n");
            packet_from->is_paused = true;
            break;

        case NETPLAY_PACKET_GAME_START:
            RG_LOGI("netplay: Received RESUME from peer\n");
            packet_from->is_paused = false;
            break;

        case NETPLAY_PACKET_READY:
            RG_LOGI("netplay: Received READY from Host\n");
            remote_player = packet_from; // ensure remote_player is set
            set_status(NETPLAY_STATUS_CONNECTED);
            break;

        case NETPLAY_PACKET_PING:
            send_packet(packet->player_id, NETPLAY_PACKET_PONG, 0, 0, 0);
            break;

        default:
            break;
        }
    }
}

static void netplay_init()
{
    RG_LOGI("%s called.\n", __func__);

    if (netplay_status == NETPLAY_STATUS_NOT_INIT)
    {
        netplay_status = NETPLAY_STATUS_STOPPED;
        netplay_callback = netplay_callback ?: dummy_netplay_callback;
        netplay_mode = NETPLAY_MODE_NONE;
        netplay_sync = xSemaphoreCreateMutex();
        netplay_send_mutex = xSemaphoreCreateMutex();
        netplay_queue = xQueueCreate(64, sizeof(netplay_packet_t));
        espnow_rx_queue = xQueueCreate(32, sizeof(espnow_packet_t));

        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(err);

        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(err);

        netif_sta = esp_netif_create_default_wifi_sta();
        if (!netif_sta)
            netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(err);

        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // Improves latency a lot
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

        rg_task_create("rg_netplay", &netplay_task, NULL, 8192, RG_TASK_PRIORITY_4, 1);
    }
}


void rg_netplay_init(netplay_callback_t callback)
{
    RG_LOGI("%s called.\n", __func__);

    netplay_callback = callback;
}


bool rg_netplay_quick_start(void)
{
    const char *status_msg = _("Initializing...");
    const char *screen_msg = NULL;
    // int timeout = 100;

    rg_display_clear(0);

    const rg_gui_option_t options[] = {
        {1, _("Host Game (P1)"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, _("Find Game (P2)"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };

    int ret = rg_gui_dialog(_("Netplay"), options, 0);

    if (ret == 1)
        rg_netplay_start(NETPLAY_MODE_HOST);
    else if (ret == 2)
        rg_netplay_start(NETPLAY_MODE_GUEST);
    else
        return false;

    while (1)
    {
        switch (netplay_status)
        {
        case NETPLAY_STATUS_CONNECTED:
            if (!remote_player || !local_player)
            {
                set_status(NETPLAY_STATUS_HANDSHAKE);
                break;
            }
            return remote_player->game_id == local_player->game_id ||
                   rg_gui_confirm(_("Netplay"), _("ROMs not identical. Continue?"), 1);
            break;

        case NETPLAY_STATUS_HANDSHAKE:
            status_msg = _("Exchanging info...");
            break;

        case NETPLAY_STATUS_CONNECTING:
            status_msg = _("Connecting...");
            break;

        case NETPLAY_STATUS_DISCONNECTED:
            status_msg = _("Unable to find host!");
            break;

        case NETPLAY_STATUS_STOPPED:
            status_msg = _("Connection failed!");
            break;

        case NETPLAY_STATUS_LISTENING:
            status_msg = _("Waiting for peer...");
            break;

        default:
            status_msg = _("Unknown status...");
        }

        if (screen_msg != status_msg)
        {
            rg_display_clear(0);
            rg_gui_draw_dialog(status_msg, NULL, 0, 0);
            screen_msg = status_msg;
        }

        if (rg_input_key_is_pressed(RG_KEY_B))
            break;

        rg_task_delay(10);
    }

    rg_netplay_stop();

    return false;
}


bool rg_netplay_start(netplay_mode_t mode)
{
    RG_LOGI("%s called.\n", __func__);

    esp_err_t ret = ESP_FAIL;

    if (netplay_status == NETPLAY_STATUS_NOT_INIT)
    {
        netplay_init();
    }
    else if (netplay_mode != NETPLAY_MODE_NONE)
    {
        rg_netplay_stop();
    }

    memset(players, 0, sizeof(players));
    memset(remote_last_seq, 0, sizeof(remote_last_seq));
    local_seq = 0;
    local_player = NULL;
    remote_player = NULL;

    netplay_mode = mode;
    RG_LOGI("netplay: Starting ESP-NOW netplay. Mode: %d\n", mode);

    // Both Host and Guest use STA mode for ESP-NOW and agree on the channel
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

    esp_now_deinit();
    if (esp_now_init() == ESP_OK)
    {
        esp_now_register_recv_cb(espnow_recv_cb);
        esp_now_register_send_cb(espnow_send_cb);

        // Add broadcast peer for discovery
        esp_now_peer_info_t peer = {0};
        peer.channel = WIFI_CHANNEL;
        peer.ifidx = ESP_IF_WIFI_STA;
        peer.encrypt = false;
        memcpy(peer.peer_addr, broadcast_mac, 6);
        esp_now_add_peer(&peer);
    }
    else
    {
        RG_LOGE("netplay: Failed to init ESP-NOW");
        return false;
    }

    int player_id = (mode == NETPLAY_MODE_HOST) ? 0 : 1;
    local_player = &players[player_id];
    local_player->id = player_id;
    local_player->version = NETPLAY_VERSION;
    const char *rom_path = rg_system_get_app()->romPath ?: "";
    local_player->game_id = rg_crc32(0, (const uint8_t *)rom_path, strlen(rom_path));
    esp_read_mac(local_player->mac_addr, ESP_MAC_WIFI_STA);

    set_status(NETPLAY_STATUS_HANDSHAKE);
    ret = ESP_OK;

    return ret == ESP_OK;
}


bool rg_netplay_stop(void)
{
    RG_LOGI("%s called.\n", __func__);

    esp_err_t ret = ESP_FAIL;

    if (netplay_mode != NETPLAY_MODE_NONE)
    {
        esp_now_deinit();
        ret = esp_wifi_stop();
        netplay_status = NETPLAY_STATUS_STOPPED;
        netplay_mode = NETPLAY_MODE_NONE;
        if (espnow_rx_queue)
            xQueueReset(espnow_rx_queue);
        if (netplay_queue)
            xQueueReset(netplay_queue);
        if (netplay_sync)
            xSemaphoreGive(netplay_sync);
    }

    return ret == ESP_OK;
}


void rg_netplay_async(void *data_in, void *data_out, uint8_t data_len)
{
    if (netplay_status != NETPLAY_STATUS_CONNECTED || !remote_player)
    {
        return;
    }

    // Return the LAST received packet's data
    netplay_packet_t packet;
    if (xQueueReceive(netplay_queue, &packet, 0) == pdPASS)
    {
        if (data_out)
            memcpy(data_out, packet.data, data_len);
        players[packet.player_id].last_contact = rg_system_timer();
    }

    // Send our current byte for the Master to pick up
    send_packet(remote_player->id, NETPLAY_PACKET_SYNC_ACK, 0, data_in, data_len);
}

void rg_netplay_sync(void *data_in, void *data_out, uint8_t data_len)
{
    rg_netplay_sync_ex(data_in, data_out, data_len, 100);
}

bool rg_netplay_poll_sync(void *data_in, void *data_out, uint8_t data_len)
{
    if (netplay_status != NETPLAY_STATUS_CONNECTED || !remote_player)
        return false;

    netplay_packet_t packet;

    // Role-agnostic: handle incoming SYNC_REQ from either side.
    // This is the serial Slave path — we respond to whoever is the serial Master.
    if (receive_packet(&packet, 0))
    {
        if (packet.cmd == NETPLAY_PACKET_SYNC_REQ)
        {
            if (packet.arg == remote_last_seq[packet.player_id])
            {
                // Duplicate request — resend ACK but don't process again
                send_packet(remote_player->id, NETPLAY_PACKET_SYNC_ACK, packet.arg, data_in, data_len);
                return false;
            }

            if (data_out)
                memcpy(data_out, packet.data, data_len);

            // Respond immediately with our data
            send_packet(remote_player->id, NETPLAY_PACKET_SYNC_ACK, packet.arg, data_in, data_len);
            remote_last_seq[packet.player_id] = packet.arg;

            return true;
        }
    }

    return false;
}

void rg_netplay_sync_ex(void *data_in, void *data_out, uint8_t data_len, int timeout_ms)
{
    if (netplay_status != NETPLAY_STATUS_CONNECTED || !remote_player)
    {
        return;
    }

    netplay_packet_t packet;
    int64_t start_time = rg_system_timer();
    const int SYNC_TIMEOUT_MS = 2000;

    // Role-agnostic: The serial Master (caller) always sends SYNC_REQ.
    // This works regardless of whether we are netplay HOST or GUEST,
    // because GB serial Master is determined by who sets SC=0x81.
    uint8_t seq = ++local_seq;
    if (seq == 0)
        seq = ++local_seq;
    int64_t last_send = 0;
    bool dialog_shown = false;

    while ((rg_system_timer() - start_time) / 1000 < SYNC_TIMEOUT_MS)
    {
        if (remote_player->is_paused)
        {
            return; // Don't block if peer is paused
        }

        // (Re)send our SYNC_REQ every 40ms
        if ((rg_system_timer() - last_send) / 1000 >= 40)
        {
            send_packet(remote_player->id, NETPLAY_PACKET_SYNC_REQ, seq, data_in, data_len);
            last_send = rg_system_timer();
        }

        if (!dialog_shown && (rg_system_timer() - start_time) / 1000 > 1000)
        {
            RG_LOGI("netplay_sync_ex: waiting for peer to respond...\n");
            dialog_shown = true;
        }

        if (receive_packet(&packet, 2))
        {
            if (packet.cmd == NETPLAY_PACKET_SYNC_ACK && packet.arg == seq)
            {
                // Got our matching ACK — exchange complete
                if (data_out)
                    memcpy(data_out, packet.data, data_len);
                remote_last_seq[packet.player_id] = packet.arg;
                return;
            }
            else if (packet.cmd == NETPLAY_PACKET_GAME_PAUSE)
            {
                players[packet.player_id].is_paused = true;
            }
            else if (packet.cmd == NETPLAY_PACKET_GAME_START)
            {
                players[packet.player_id].is_paused = false;
            }
            else if (packet.cmd == NETPLAY_PACKET_SYNC_REQ)
            {
                // Peer is also a serial Master (both sides initiated).
                // Respond with ACK and use their data as our received byte.
                if (packet.arg != remote_last_seq[packet.player_id])
                {
                    if (data_out)
                        memcpy(data_out, packet.data, data_len);
                    send_packet(remote_player->id, NETPLAY_PACKET_SYNC_ACK, packet.arg, data_in, data_len);
                    remote_last_seq[packet.player_id] = packet.arg;
                    return;
                }
                else
                {
                    // Duplicate REQ — resend ACK
                    send_packet(remote_player->id, NETPLAY_PACKET_SYNC_ACK, packet.arg, data_in, data_len);
                }
            }
        }
    }
    RG_LOGW("netplay_sync_ex: timeout!\n");
}


netplay_mode_t rg_netplay_mode()
{
    return netplay_mode;
}


netplay_status_t rg_netplay_status()
{
    return netplay_status;
}

void rg_netplay_send_pause(bool paused)
{
    if (netplay_status != NETPLAY_STATUS_CONNECTED || !remote_player)
    {
        return;
    }
    RG_LOGI("netplay: Sending %s to peer\n", paused ? "PAUSE" : "RESUME");
    send_packet(remote_player->id, paused ? NETPLAY_PACKET_GAME_PAUSE : NETPLAY_PACKET_GAME_START, 0, NULL, 0);
}

#endif
