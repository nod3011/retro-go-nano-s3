#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { LINK_STATE_DISCONNECTED, LINK_STATE_CONNECTED } gb_link_state_t;

// Core setup
void gnuboy_set_link_cable_callback(uint8_t (*cb)(uint8_t));
void gnuboy_set_link_cable_poll_callback(bool (*cb)(uint8_t tx, uint8_t *rx));

// Ported from VMUPro foundations
gb_link_state_t link_cable_get_state(void);

// Master Mode API
void link_cable_master_transfer(uint8_t tx);
bool link_cable_master_poll(uint8_t *rx);

// Slave Mode API
void link_cable_slave_set_sb(uint8_t sb);
bool link_cable_slave_poll(uint8_t *rx);
