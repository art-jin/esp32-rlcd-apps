#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <cJSON.h>

#define PROTOCOL_VERSION 1
#define MAX_AGENTS 8
#define MAX_SESSIONS 16
#define MAX_NOTIFICATIONS 10
#define MAX_MESSAGE_SIZE 4096

// Agent types
typedef enum {
    AGENT_TYPE_CLAUDE = 0,
    AGENT_TYPE_KIMI,
    AGENT_TYPE_CODEX,
    AGENT_TYPE_QWEN,
    AGENT_TYPE_OPENCLAW,
    AGENT_TYPE_HERMES,
    AGENT_TYPE_CODEBUDDY,
    AGENT_TYPE_VECLI,
    AGENT_TYPE_GENERIC,
    AGENT_TYPE_COUNT
} agent_type_t;

// Session status
typedef enum {
    SESSION_STATUS_IDLE = 0,
    SESSION_STATUS_ACTIVE,
    SESSION_STATUS_WAITING_APPROVAL,
    SESSION_STATUS_ERROR,
    SESSION_STATUS_COUNT
} session_status_t;

// Notification severity
typedef enum {
    SEVERITY_INFO = 0,
    SEVERITY_WARNING,
    SEVERITY_ERROR,
    SEVERITY_CRITICAL,
    SEVERITY_COUNT
} notification_severity_t;

// Message types (device → bridge)
typedef enum {
    MSG_HELLO_ACK = 0,
    MSG_PONG,
    MSG_ENV,
    MSG_DIAG,
    MSG_ERROR,
    MSG_BUTTON,
    MSG_COUNT
} device_msg_type_t;

// Message types (bridge → device)
typedef enum {
    BRIDGE_MSG_HELLO = 0,
    BRIDGE_MSG_STATE,
    BRIDGE_MSG_SESSION_UPDATE,
    BRIDGE_MSG_SESSION_END,
    BRIDGE_MSG_NOTIFICATION,
    BRIDGE_MSG_NOTIFICATION_CLEAR,
    BRIDGE_MSG_QUOTA_UPDATE,
    BRIDGE_MSG_TIME_SYNC,
    BRIDGE_MSG_PING,
    // bridge_usb.js JSON side-channel:
    BRIDGE_MSG_QUOTA_WINDOWS,    // {"type":"quota_windows","q5","q24","q7"}
    BRIDGE_MSG_PERMISSION_CLEAR, // {"type":"permission_clear"}
    BRIDGE_MSG_COUNT
} bridge_msg_type_t;

// Session data
typedef struct {
    char id[64];
    char name[64];
    session_status_t status;
    char last_tool[32];
    char last_target[128];
    uint32_t tool_count;
    uint32_t start_time;
    uint32_t last_time;
} session_t;

// Agent state
typedef struct {
    agent_type_t type;
    session_t sessions[MAX_SESSIONS];
    uint8_t session_count;
    void* quota_data;  // Type-specific quota data (unused, kept for ABI compat)
    uint32_t last_update;
    // Fields populated by bridge.js "session.update" messages
    char status[32];           // "Connected", "Error", "Idle", ...
    char current_task[128];    // task description
    bool active;               // CPU > 5% threshold (per bridge.js)
    uint32_t quota_used;
    uint32_t quota_total;
    bool online;               // false if heartbeat stale > 30s
    // Fields populated by bridge_usb.js compact format (1CC<task>|Q..|P..|..).
    // Additive — older JSON-only callers leave these at 0.
    char     project[32];           // e.g. "esp32-rlcd-ap"
    uint32_t cost_cents;            // total session cost in cents
    uint32_t budget_cents;          // session budget cap in cents (for "remaining")
    uint32_t rate_cents_per_hour;   // burn rate, cents per hour
    uint32_t lines_added;
    uint32_t lines_removed;
    uint16_t session_minutes;
    uint32_t session_start_sec;     // unix epoch seconds the session began
} agent_state_t;

// Notification
typedef struct {
    char id[128];
    agent_type_t agent_type;
    char session_id[64];
    notification_severity_t severity;
    char message[256];
    uint32_t timestamp;
} notification_t;

// Global state
typedef struct {
    agent_state_t agents[MAX_AGENTS];
    uint8_t agent_count;
    notification_t notifications[MAX_NOTIFICATIONS];
    uint8_t notification_count;
    uint32_t timestamp;
    // From bridge_usb.js JSON side-channel:
    //   {"type":"quota_windows","q5":N,"q24":N,"q7":N}
    //   {"type":"permission_clear"}
    struct {
        uint8_t q5, q24, q7;
    } quota_windows;
    bool permission_alert;  // set by notification, cleared by permission_clear
} global_state_t;

// Device info
typedef struct {
    char fw_version[32];
    char ip[16];
    char mac[18];
    char* capabilities[8];
    uint8_t capability_count;
} device_info_t;

// Environment data
typedef struct {
    float temp_c;
    float humidity;
} env_data_t;

// Diagnostic data
typedef struct {
    uint32_t free_heap;
    int8_t rssi;
} diag_data_t;

// Button event
typedef struct {
    char button[8];  // "key" or "boot"
    char event[16];  // "single-click", "double-click", "long-press"
    uint32_t timestamp;
} button_event_t;

// Protocol API
bool protocol_parse_message(const char* json_str, bridge_msg_type_t* type, cJSON** root);
char* protocol_encode_device_msg(device_msg_type_t type, const void* data);
bool protocol_process_bridge_message(const char* json_str, global_state_t* state);
bool protocol_validate_version(const cJSON* msg);

// Message encoding helpers
char* protocol_encode_hello_ack(const device_info_t* info);
char* protocol_encode_pong(uint32_t timestamp);
char* protocol_encode_env(const env_data_t* env);
char* protocol_encode_diag(const diag_data_t* diag);
char* protocol_encode_error(const char* reason, const char* code);
char* protocol_encode_button(const button_event_t* event);

// State management helpers
void protocol_init_global_state(global_state_t* state);
void protocol_copy_global_state(const global_state_t* src, global_state_t* dest);
agent_type_t protocol_parse_agent_type(const char* type_str);
const char* protocol_agent_type_to_string(agent_type_t type);
session_status_t protocol_parse_session_status(const char* status_str);
const char* protocol_session_status_to_string(session_status_t status);
notification_severity_t protocol_parse_severity(const char* severity_str);
const char* protocol_severity_to_string(notification_severity_t severity);

#endif // PROTOCOL_H
