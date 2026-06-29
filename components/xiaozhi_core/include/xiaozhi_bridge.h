#ifndef XIAOZHI_BRIDGE_H
#define XIAOZHI_BRIDGE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XZ_IDLE,
    XZ_LISTENING,
    XZ_SPEAKING,
    XZ_CONNECTING,
    XZ_ERROR
} xiaozhi_state_t;

typedef void (*xiaozhi_text_cb)(const char *text);
typedef void (*xiaozhi_state_cb)(xiaozhi_state_t state);

/**
 * Initialize audio hardware and protocol modules (I2C, I2S, ES8311, ES7210, OTA, Opus).
 * WiFi must be connected before calling this.
 */
esp_err_t xiaozhi_init(void);

/**
 * Register callback for AI text output (replies, STT results).
 */
void xiaozhi_register_text_cb(xiaozhi_text_cb cb);

/**
 * Register callback for state changes.
 */
void xiaozhi_register_state_cb(xiaozhi_state_cb cb);

/**
 * Blocking event loop. Runs on the calling task.
 * Handles WebSocket I/O, JSON message routing, and state machine.
 * Press BOOT button (GPIO 0) to trigger listening.
 */
void xiaozhi_run(void);

/**
 * Thread-safe state query.
 */
xiaozhi_state_t xiaozhi_get_state(void);

/**
 * Thread-safe: trigger XiaoZhi to start listening.
 * Equivalent to pressing the BOOT button.
 */
void xiaozhi_start_listening(void);

/**
 * Thread-safe: request XiaoZhi to stop listening.
 */
void xiaozhi_stop_listening(void);

/**
 * Disable I2S audio channels (mute mic and speaker).
 * Call when switching away from XiaoZhi app.
 */
void xiaozhi_disable_audio(void);

/**
 * Clear stale event bits and reset state for reconnection.
 * Call before creating a new xiaozhi_run task on subsequent switches.
 */
void xiaozhi_prepare_reconnect(void);

/**
 * Force kill signal: causes xiaozhi_run() to exit its main loop cleanly.
 * Also disconnects from server (closes WS, stops pipeline).
 * Caller should wait for the xiaozhi task to actually exit before reusing resources.
 */
void xiaozhi_force_disconnect(void);

/**
 * Expose the I2C master bus used for ES8311/ES7210 codec control.
 * Allows other drivers (e.g., SHTC3 sensor) to add their own devices
 * on the same bus (GPIO13/14).
 * Returns NULL if xiaozhi_init() has not been called yet.
 */
void *xiaozhi_get_i2c_bus(void);  // returns i2c_master_bus_handle_t

/**
 * Hardware-mute the speaker (used by CodePilot STT-only mode).
 * Does NOT touch the XiaoZhi protocol — purely I2S TX channel on/off.
 */
void xiaozhi_set_speaker_mute(bool mute);

#ifdef __cplusplus
}
#endif

#endif // XIAOZHI_BRIDGE_H
