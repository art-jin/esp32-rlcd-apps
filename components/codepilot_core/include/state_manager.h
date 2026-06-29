#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include "protocol.h"

// Initialize state manager
void state_manager_init(void);

// Get current global state
const global_state_t* state_manager_get_state(void);

// Update state from bridge message
bool state_manager_update_from_bridge(const char* json_msg);

// Find agent by type
agent_state_t* state_manager_find_agent(agent_type_t type);

// Find session by ID
session_t* state_manager_find_session(const char* session_id);

// Get active session count
uint8_t state_manager_get_active_session_count(void);

// Get notification count
uint8_t state_manager_get_notification_count(void);

// Get highest priority notification
const notification_t* state_manager_get_highest_priority_notification(void);

// Clear notification
bool state_manager_clear_notification(const char* id);

// Clear all notifications
void state_manager_clear_all_notifications(void);

// Get diagnostic data
void state_manager_get_diag_data(diag_data_t* diag);

// Get environment data
void state_manager_get_env_data(env_data_t* env);

// Phase B explicit locking for snapshot reads (e.g., rendering entire UI).
// Use as: state_manager_lock(); ...read state...; state_manager_unlock();
void state_manager_lock(void);
void state_manager_unlock(void);

#endif // STATE_MANAGER_H
