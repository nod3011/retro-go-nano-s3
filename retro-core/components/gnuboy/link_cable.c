#include "link_cable.h"
#include <stddef.h>

static uint8_t (*serial_callback)(uint8_t) = NULL;
static bool (*serial_poll)(uint8_t tx, uint8_t *rx) = NULL;

static uint8_t master_rx_buffer = 0xFF;
static bool master_has_data = false;

// Foundations for Slave Mode (to be implemented more fully later)
static uint8_t slave_sb = 0xFF;
static uint8_t slave_rx_buffer = 0xFF;
static bool slave_has_data = false;

void gnuboy_set_link_cable_callback(uint8_t (*cb)(uint8_t)) {
  serial_callback = cb;
}

void gnuboy_set_link_cable_poll_callback(bool (*cb)(uint8_t tx, uint8_t *rx)) {
  serial_poll = cb;
}

gb_link_state_t link_cable_get_state(void) {
  if (serial_callback) {
    return LINK_STATE_CONNECTED;
  }
  return LINK_STATE_DISCONNECTED;
}

void link_cable_master_transfer(uint8_t tx) {
  if (serial_callback) {
    serial_callback(tx);
    // Data is retrieved asynchronously via polling!
    master_has_data = false;
  } else {
    master_rx_buffer = 0xFF;
    master_has_data = true;
  }
}

bool link_cable_master_poll(uint8_t *rx) {
  if (master_has_data) {
    *rx = master_rx_buffer;
    master_has_data = false;
    return true;
  }
  if (serial_poll) {
    if (serial_poll(0xFF, rx)) {
      return true;
    }
  }
  return false;
}

void link_cable_slave_set_sb(uint8_t sb) { slave_sb = sb; }

bool link_cable_slave_poll(uint8_t *rx) {
  if (slave_has_data) {
    *rx = slave_rx_buffer;
    slave_has_data = false;
    return true;
  }
  if (serial_poll) {
    if (serial_poll(slave_sb, rx)) {
      return true;
    }
  }
  return false;
}
