#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <esp_log.h>
#include <cJSON.h>
#include <esp_mac.h>
#include <esp_system.h>

static const char* TAG = "PROTOCOL";

// Forward declarations to silence -Wunused-function (all dispatched by
// protocol_process_bridge_message's switch statement)
static bool protocol_process_state_message(const cJSON* msg, global_state_t* state);
static bool protocol_process_session_update(const cJSON* msg, global_state_t* state);
static bool protocol_process_session_end(const cJSON* msg, global_state_t* state);
static bool protocol_process_notification(const cJSON* msg, global_state_t* state);
static bool protocol_process_notification_clear(const cJSON* msg, global_state_t* state);
static bool protocol_process_quota_update(const cJSON* msg, global_state_t* state);
static bool protocol_process_quota_windows(const cJSON* msg, global_state_t* state);
static bool protocol_process_permission_clear(global_state_t* state);
static bool protocol_process_time_sync(const cJSON* msg, global_state_t* state);
static void protocol_parse_session(const cJSON* session_obj, session_t* session);
static void protocol_parse_notification(const cJSON* notif_obj, notification_t* notification);
static bool protocol_parse_compact(const char* line, global_state_t* state);

// Initialize global state
void protocol_init_global_state(global_state_t* state) {
    if (!state) return;

    memset(state, 0, sizeof(global_state_t));
    state->timestamp = 0;
    state->agent_count = 0;
    state->notification_count = 0;
}

// Copy global state
void protocol_copy_global_state(const global_state_t* src, global_state_t* dest) {
    if (!src || !dest) return;

    memcpy(dest, src, sizeof(global_state_t));
}

// Parse agent type from string (case-insensitive: bridge.js sends "Claude" / "Kimi")
agent_type_t protocol_parse_agent_type(const char* type_str) {
    if (!type_str) return AGENT_TYPE_GENERIC;

    if (strcasecmp(type_str, "claude") == 0) return AGENT_TYPE_CLAUDE;
    if (strcasecmp(type_str, "kimi") == 0) return AGENT_TYPE_KIMI;
    if (strcasecmp(type_str, "codex") == 0) return AGENT_TYPE_CODEX;
    if (strcasecmp(type_str, "qwen") == 0) return AGENT_TYPE_QWEN;
    if (strcasecmp(type_str, "openclaw") == 0) return AGENT_TYPE_OPENCLAW;
    if (strcasecmp(type_str, "hermes") == 0) return AGENT_TYPE_HERMES;
    if (strcasecmp(type_str, "codebuddy") == 0) return AGENT_TYPE_CODEBUDDY;
    if (strcasecmp(type_str, "vecli") == 0) return AGENT_TYPE_VECLI;

    return AGENT_TYPE_GENERIC;
}

// Convert agent type to string
const char* protocol_agent_type_to_string(agent_type_t type) {
    switch (type) {
        case AGENT_TYPE_CLAUDE: return "claude";
        case AGENT_TYPE_KIMI: return "kimi";
        case AGENT_TYPE_CODEX: return "codex";
        case AGENT_TYPE_QWEN: return "qwen";
        case AGENT_TYPE_OPENCLAW: return "openclaw";
        case AGENT_TYPE_HERMES: return "hermes";
        case AGENT_TYPE_CODEBUDDY: return "codebuddy";
        case AGENT_TYPE_VECLI: return "vecli";
        case AGENT_TYPE_GENERIC: return "generic";
        default: return "unknown";
    }
}

// Parse session status from string
session_status_t protocol_parse_session_status(const char* status_str) {
    if (!status_str) return SESSION_STATUS_IDLE;

    if (strcmp(status_str, "active") == 0) return SESSION_STATUS_ACTIVE;
    if (strcmp(status_str, "idle") == 0) return SESSION_STATUS_IDLE;
    if (strcmp(status_str, "waiting_approval") == 0) return SESSION_STATUS_WAITING_APPROVAL;
    if (strcmp(status_str, "error") == 0) return SESSION_STATUS_ERROR;

    return SESSION_STATUS_IDLE;
}

// Convert session status to string
const char* protocol_session_status_to_string(session_status_t status) {
    switch (status) {
        case SESSION_STATUS_ACTIVE: return "active";
        case SESSION_STATUS_IDLE: return "idle";
        case SESSION_STATUS_WAITING_APPROVAL: return "waiting_approval";
        case SESSION_STATUS_ERROR: return "error";
        default: return "unknown";
    }
}

// Parse severity from string
notification_severity_t protocol_parse_severity(const char* severity_str) {
    if (!severity_str) return SEVERITY_INFO;

    if (strcmp(severity_str, "critical") == 0) return SEVERITY_CRITICAL;
    if (strcmp(severity_str, "error") == 0) return SEVERITY_ERROR;
    if (strcmp(severity_str, "warning") == 0) return SEVERITY_WARNING;
    if (strcmp(severity_str, "info") == 0) return SEVERITY_INFO;

    return SEVERITY_INFO;
}

// Convert severity to string
const char* protocol_severity_to_string(notification_severity_t severity) {
    switch (severity) {
        case SEVERITY_CRITICAL: return "critical";
        case SEVERITY_ERROR: return "error";
        case SEVERITY_WARNING: return "warning";
        case SEVERITY_INFO: return "info";
        default: return "unknown";
    }
}

// Validate protocol version (lenient: missing "v" treated as compatible,
// since bridge.js does not send version field)
bool protocol_validate_version(const cJSON* msg) {
    const cJSON* v = cJSON_GetObjectItem(msg, "v");
    if (!v) return true;                  // missing → assume compatible
    return v->valueint == PROTOCOL_VERSION;
}

// Parse message from bridge
bool protocol_parse_message(const char* json_str, bridge_msg_type_t* type, cJSON** root) {
    if (!json_str || !type || !root) return false;

    *root = cJSON_Parse(json_str);
    if (!*root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return false;
    }

    if (!protocol_validate_version(*root)) {
        ESP_LOGE(TAG, "Invalid protocol version");
        cJSON_Delete(*root);
        return false;
    }

    const cJSON* type_obj = cJSON_GetObjectItem(*root, "type");
    if (!type_obj) {
        ESP_LOGE(TAG, "Missing type field");
        cJSON_Delete(*root);
        return false;
    }

    const char* type_str = type_obj->valuestring;

    // Accept both dot and dash notation (bridge.js uses "session.update")
    if (strcasecmp(type_str, "hello") == 0) {
        *type = BRIDGE_MSG_HELLO;
    } else if (strcasecmp(type_str, "state") == 0) {
        *type = BRIDGE_MSG_STATE;
    } else if (strcasecmp(type_str, "session-update") == 0 ||
               strcasecmp(type_str, "session.update") == 0) {
        *type = BRIDGE_MSG_SESSION_UPDATE;
    } else if (strcasecmp(type_str, "session-end") == 0 ||
               strcasecmp(type_str, "session.end") == 0) {
        *type = BRIDGE_MSG_SESSION_END;
    } else if (strcasecmp(type_str, "notification") == 0) {
        *type = BRIDGE_MSG_NOTIFICATION;
    } else if (strcasecmp(type_str, "notification-clear") == 0 ||
               strcasecmp(type_str, "notification.clear") == 0) {
        *type = BRIDGE_MSG_NOTIFICATION_CLEAR;
    } else if (strcasecmp(type_str, "quota-update") == 0 ||
               strcasecmp(type_str, "quota.update") == 0) {
        *type = BRIDGE_MSG_QUOTA_UPDATE;
    } else if (strcasecmp(type_str, "time-sync") == 0 ||
               strcasecmp(type_str, "time.sync") == 0) {
        *type = BRIDGE_MSG_TIME_SYNC;
    } else if (strcasecmp(type_str, "ping") == 0 ||
               strcasecmp(type_str, "heartbeat") == 0) {
        *type = BRIDGE_MSG_PING;
    } else if (strcasecmp(type_str, "quota_windows") == 0) {
        // bridge_usb.js: {"type":"quota_windows","q5":N,"q24":N,"q7":N}
        *type = BRIDGE_MSG_QUOTA_WINDOWS;
    } else if (strcasecmp(type_str, "permission_clear") == 0) {
        // bridge_usb.js: {"type":"permission_clear"}
        *type = BRIDGE_MSG_PERMISSION_CLEAR;
    } else {
        // Unknown type: be lenient, just ignore (treat as ping)
        *type = BRIDGE_MSG_PING;
    }

    return true;
}

// Encode hello-ack message
char* protocol_encode_hello_ack(const device_info_t* info) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "hello-ack");
    cJSON_AddStringToObject(root, "fw", info->fw_version);
    cJSON_AddStringToObject(root, "ip", info->ip);
    cJSON_AddStringToObject(root, "mac", info->mac);

    if (info->capability_count > 0) {
        cJSON* capabilities = cJSON_CreateArray();
        for (uint8_t i = 0; i < info->capability_count; i++) {
            cJSON_AddItemToArray(capabilities, cJSON_CreateString(info->capabilities[i]));
        }
        cJSON_AddItemToObject(root, "capabilities", capabilities);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

// Encode pong message
char* protocol_encode_pong(uint32_t timestamp) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "pong");
    cJSON_AddNumberToObject(root, "timestamp", timestamp);

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

// Encode env message
char* protocol_encode_env(const env_data_t* env) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "env");
    cJSON_AddNumberToObject(root, "tempC", env->temp_c);
    cJSON_AddNumberToObject(root, "humidity", env->humidity);

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

// Encode diag message
char* protocol_encode_diag(const diag_data_t* diag) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "diag");
    cJSON_AddNumberToObject(root, "freeHeap", diag->free_heap);

    if (diag->rssi != 0) {
        cJSON_AddNumberToObject(root, "rssi", diag->rssi);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

// Encode error message
char* protocol_encode_error(const char* reason, const char* code) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "reason", reason);

    if (code) {
        cJSON_AddStringToObject(root, "code", code);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

// Encode button event message
char* protocol_encode_button(const button_event_t* event) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "button");
    cJSON_AddStringToObject(root, "button", event->button);
    cJSON_AddStringToObject(root, "event", event->event);
    cJSON_AddNumberToObject(root, "timestamp", event->timestamp);

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

// Process bridge message and update state.
// Accepts both formats emitted by the bridge family:
//   - JSON    : line starts with '{' → existing path (bridge.js legacy)
//   - compact : line starts with '0'/'1' → bridge_usb.js short format
bool protocol_process_bridge_message(const char* line, global_state_t* state) {
    if (!line || !state) return false;

    // Skip leading whitespace
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return false;

    // bridge_usb.js compact format: "1CC<task>|Q..|P..|C..|R..|L..,..|T.."
    if (*line == '0' || *line == '1') {
        return protocol_parse_compact(line, state);
    }

    // Otherwise treat as JSON
    bridge_msg_type_t type;
    cJSON* root = NULL;

    if (!protocol_parse_message(line, &type, &root)) {
        return false;
    }

    bool success = true;

    switch (type) {
        case BRIDGE_MSG_STATE:
            success = protocol_process_state_message(root, state);
            break;

        case BRIDGE_MSG_SESSION_UPDATE:
            success = protocol_process_session_update(root, state);
            break;

        case BRIDGE_MSG_SESSION_END:
            success = protocol_process_session_end(root, state);
            break;

        case BRIDGE_MSG_NOTIFICATION:
            success = protocol_process_notification(root, state);
            break;

        case BRIDGE_MSG_NOTIFICATION_CLEAR:
            success = protocol_process_notification_clear(root, state);
            break;

        case BRIDGE_MSG_QUOTA_UPDATE:
            success = protocol_process_quota_update(root, state);
            break;

        case BRIDGE_MSG_QUOTA_WINDOWS:
            success = protocol_process_quota_windows(root, state);
            break;

        case BRIDGE_MSG_PERMISSION_CLEAR:
            success = protocol_process_permission_clear(state);
            break;

        case BRIDGE_MSG_TIME_SYNC:
            success = protocol_process_time_sync(root, state);
            break;

        case BRIDGE_MSG_PING:
            // Ping - respond with pong (handled by WebSocket server)
            break;

        default:
            ESP_LOGW(TAG, "Unhandled bridge message type: %d", type);
            break;
    }

    cJSON_Delete(root);
    return success;
}

// Process state message (full update)
static bool protocol_process_state_message(const cJSON* msg, global_state_t* state) {
    const cJSON* agents_obj = cJSON_GetObjectItem(msg, "agents");
    const cJSON* notifications_obj = cJSON_GetObjectItem(msg, "notifications");
    const cJSON* timestamp_obj = cJSON_GetObjectItem(msg, "timestamp");

    if (!agents_obj || !cJSON_IsArray(agents_obj)) {
        ESP_LOGE(TAG, "Invalid agents in state message");
        return false;
    }

    // Clear current state
    protocol_init_global_state(state);

    // Process agents
    int agent_count = cJSON_GetArraySize(agents_obj);
    state->agent_count = (agent_count > MAX_AGENTS) ? MAX_AGENTS : agent_count;

    for (int i = 0; i < state->agent_count; i++) {
        const cJSON* agent_obj = cJSON_GetArrayItem(agents_obj, i);
        agent_state_t* agent = &state->agents[i];

        const cJSON* type_obj = cJSON_GetObjectItem(agent_obj, "type");
        const cJSON* sessions_obj = cJSON_GetObjectItem(agent_obj, "sessions");

        if (type_obj) {
            agent->type = protocol_parse_agent_type(type_obj->valuestring);
        }

        if (sessions_obj && cJSON_IsArray(sessions_obj)) {
            int session_count = cJSON_GetArraySize(sessions_obj);
            agent->session_count = (session_count > MAX_SESSIONS) ? MAX_SESSIONS : session_count;

            for (int j = 0; j < agent->session_count; j++) {
                const cJSON* session_obj = cJSON_GetArrayItem(sessions_obj, j);
                protocol_parse_session(session_obj, &agent->sessions[j]);
            }
        }
    }

    // Process notifications
    if (notifications_obj && cJSON_IsArray(notifications_obj)) {
        int notif_count = cJSON_GetArraySize(notifications_obj);
        state->notification_count = (notif_count > MAX_NOTIFICATIONS) ? MAX_NOTIFICATIONS : notif_count;

        for (int i = 0; i < state->notification_count; i++) {
            const cJSON* notif_obj = cJSON_GetArrayItem(notifications_obj, i);
            protocol_parse_notification(notif_obj, &state->notifications[i]);
        }
    }

    // Update timestamp
    if (timestamp_obj) {
        state->timestamp = timestamp_obj->valueint;
    }

    return true;
}

// Parse session from JSON
static void protocol_parse_session(const cJSON* session_obj, session_t* session) {
    const cJSON* id = cJSON_GetObjectItem(session_obj, "id");
    const cJSON* name = cJSON_GetObjectItem(session_obj, "name");
    const cJSON* status = cJSON_GetObjectItem(session_obj, "status");
    const cJSON* last_tool = cJSON_GetObjectItem(session_obj, "lastTool");
    const cJSON* last_target = cJSON_GetObjectItem(session_obj, "lastTarget");
    const cJSON* tool_count = cJSON_GetObjectItem(session_obj, "toolCount");
    const cJSON* start_time = cJSON_GetObjectItem(session_obj, "startTime");
    const cJSON* last_time = cJSON_GetObjectItem(session_obj, "lastTime");

    if (id) strncpy(session->id, id->valuestring, sizeof(session->id) - 1);
    if (name) strncpy(session->name, name->valuestring, sizeof(session->name) - 1);
    if (status) session->status = protocol_parse_session_status(status->valuestring);
    if (last_tool) strncpy(session->last_tool, last_tool->valuestring, sizeof(session->last_tool) - 1);
    if (last_target) strncpy(session->last_target, last_target->valuestring, sizeof(session->last_target) - 1);
    if (tool_count) session->tool_count = tool_count->valueint;
    if (start_time) session->start_time = start_time->valueint;
    if (last_time) session->last_time = last_time->valueint;
}

// Parse notification from JSON
static void protocol_parse_notification(const cJSON* notif_obj, notification_t* notification) {
    const cJSON* id = cJSON_GetObjectItem(notif_obj, "id");
    const cJSON* agent_type = cJSON_GetObjectItem(notif_obj, "agentType");
    const cJSON* session_id = cJSON_GetObjectItem(notif_obj, "sessionId");
    const cJSON* severity = cJSON_GetObjectItem(notif_obj, "severity");
    const cJSON* message = cJSON_GetObjectItem(notif_obj, "message");
    const cJSON* timestamp = cJSON_GetObjectItem(notif_obj, "timestamp");

    if (id) strncpy(notification->id, id->valuestring, sizeof(notification->id) - 1);
    if (agent_type) notification->agent_type = protocol_parse_agent_type(agent_type->valuestring);
    if (session_id) strncpy(notification->session_id, session_id->valuestring, sizeof(notification->session_id) - 1);
    if (severity) notification->severity = protocol_parse_severity(severity->valuestring);
    if (message) strncpy(notification->message, message->valuestring, sizeof(notification->message) - 1);
    if (timestamp) notification->timestamp = timestamp->valueint;
}

// Process bridge.js session.update message
// Wire format: {"type":"session.update","provider":"Claude","status":"Connected",
//               "current_task":"...","active":true,
//               "quota_used":N,"quota_total":N,"timestamp":N}
static bool protocol_process_session_update(const cJSON* msg, global_state_t* state) {
    const cJSON* provider    = cJSON_GetObjectItem(msg, "provider");
    const cJSON* status      = cJSON_GetObjectItem(msg, "status");
    const cJSON* current_task = cJSON_GetObjectItem(msg, "current_task");
    const cJSON* active      = cJSON_GetObjectItem(msg, "active");
    const cJSON* quota_used  = cJSON_GetObjectItem(msg, "quota_used");
    const cJSON* quota_total = cJSON_GetObjectItem(msg, "quota_total");
    const cJSON* timestamp   = cJSON_GetObjectItem(msg, "timestamp");

    if (!provider || !cJSON_IsString(provider)) {
        ESP_LOGW(TAG, "session.update missing provider");
        return false;
    }

    agent_type_t type = protocol_parse_agent_type(provider->valuestring);

    // Find or create agent slot
    agent_state_t* agent = NULL;
    for (uint8_t i = 0; i < state->agent_count; i++) {
        if (state->agents[i].type == type) {
            agent = &state->agents[i];
            break;
        }
    }
    if (!agent) {
        if (state->agent_count >= MAX_AGENTS) {
            ESP_LOGW(TAG, "MAX_AGENTS reached, ignoring new provider %s", provider->valuestring);
            return false;
        }
        agent = &state->agents[state->agent_count++];
        memset(agent, 0, sizeof(*agent));
        agent->type = type;
    }

    if (status && cJSON_IsString(status)) {
        strncpy(agent->status, status->valuestring, sizeof(agent->status) - 1);
        agent->status[sizeof(agent->status) - 1] = '\0';
    }
    if (current_task && cJSON_IsString(current_task)) {
        strncpy(agent->current_task, current_task->valuestring, sizeof(agent->current_task) - 1);
        agent->current_task[sizeof(agent->current_task) - 1] = '\0';
    }
    if (active) agent->active = cJSON_IsTrue(active);
    if (quota_used && cJSON_IsNumber(quota_used)) {
        agent->quota_used = (uint32_t)cJSON_GetNumberValue(quota_used);
    }
    if (quota_total && cJSON_IsNumber(quota_total)) {
        agent->quota_total = (uint32_t)cJSON_GetNumberValue(quota_total);
    }
    if (timestamp && cJSON_IsNumber(timestamp)) {
        agent->last_update = (uint32_t)cJSON_GetNumberValue(timestamp);
    }
    agent->online = true;
    state->timestamp = agent->last_update;

    ESP_LOGI(TAG, "agent[%d] %s: status=%s active=%d quota=%u/%u",
             (int)(agent - state->agents),
             protocol_agent_type_to_string(type),
             agent->status, agent->active,
             agent->quota_used, agent->quota_total);
    return true;
}

static bool protocol_process_session_end(const cJSON* msg, global_state_t* state) {
    // Implementation for session end
    return true;
}

static bool protocol_process_notification(const cJSON* msg, global_state_t* state) {
    // Implementation for new notification
    return true;
}

static bool protocol_process_notification_clear(const cJSON* msg, global_state_t* state) {
    // Implementation for clearing notification
    return true;
}

static bool protocol_process_quota_update(const cJSON* msg, global_state_t* state) {
    // Implementation for quota update
    return true;
}

// {"type":"quota_windows","q5":N,"q24":N,"q7":N} from bridge_usb.js
static bool protocol_process_quota_windows(const cJSON* msg, global_state_t* state) {
    const cJSON* q5  = cJSON_GetObjectItem(msg, "q5");
    const cJSON* q24 = cJSON_GetObjectItem(msg, "q24");
    const cJSON* q7  = cJSON_GetObjectItem(msg, "q7");
    if (q5 && cJSON_IsNumber(q5))  state->quota_windows.q5  = (uint8_t)q5->valueint;
    if (q24 && cJSON_IsNumber(q24)) state->quota_windows.q24 = (uint8_t)q24->valueint;
    if (q7 && cJSON_IsNumber(q7))  state->quota_windows.q7  = (uint8_t)q7->valueint;
    return true;
}

// {"type":"permission_clear"} — bridge_usb.js after user grants permission in Claude Code
static bool protocol_process_permission_clear(global_state_t* state) {
    state->permission_alert = false;
    return true;
}

static bool protocol_process_time_sync(const cJSON* msg, global_state_t* state) {
    // Implementation for time sync
    return true;
}

// ── bridge_usb.js compact format ─────────────────────────────────────────────
// Wire shape (see ESP32_screen/bridge/bridge_usb.js:79 toShortMessage):
//   {active '0'/'1'}{status 'C'/'I'}{name[0]}{task≤20}|Q{0-100}|P{project≤12}|C{cents}|R{cents/h}|L{+lines},{-lines}|T{minutes}\n
// 'task' has '|' replaced with '!' upstream. Fields may be missing if the
// bridge hasn't filled them yet — leave the previous value alone in that case.
//
// Example:  1CCesp32-rlcd-apps!!|Q43|Pesp32-rlcd-ap|C4319|R55|L3344,784|T777
static bool protocol_parse_compact(const char* line, global_state_t* state) {
    // Min length: 3 prefix chars + at least 1 task char = 4
    if (strlen(line) < 4) return false;

    const char active_ch  = line[0];
    const char status_ch  = line[1];
    const char name_ch    = line[2];

    if (active_ch != '0' && active_ch != '1') return false;
    if (status_ch != 'C' && status_ch != 'I') return false;

    // Map name[0] → agent type. Bridge currently emits only 'C' (Claude).
    // Unknown initials route to GENERIC.
    agent_type_t type = AGENT_TYPE_GENERIC;
    switch (name_ch) {
        case 'C': type = AGENT_TYPE_CLAUDE; break;
        case 'K': type = AGENT_TYPE_KIMI; break;
        default: break;
    }

    // Find-or-create agent slot (mirrors protocol_process_session_update)
    agent_state_t* agent = NULL;
    for (uint8_t i = 0; i < state->agent_count; i++) {
        if (state->agents[i].type == type) {
            agent = &state->agents[i];
            break;
        }
    }
    if (!agent) {
        if (state->agent_count >= MAX_AGENTS) return false;
        agent = &state->agents[state->agent_count++];
        memset(agent, 0, sizeof(*agent));
        agent->type = type;
    }

    agent->active = (active_ch == '1');
    snprintf(agent->status, sizeof(agent->status), "%s",
             (status_ch == 'C') ? "Connected" : "Idle");

    // Task is the substring [3 .. first '|')
    const char* task_start = line + 3;
    const char* pipe = strchr(task_start, '|');
    if (!pipe) {
        // No pipe = no other fields; just task
        size_t tlen = strlen(task_start);
        if (tlen >= sizeof(agent->current_task)) tlen = sizeof(agent->current_task) - 1;
        memcpy(agent->current_task, task_start, tlen);
        agent->current_task[tlen] = '\0';
        agent->online = true;
        return true;
    }

    size_t tlen = (size_t)(pipe - task_start);
    if (tlen >= sizeof(agent->current_task)) tlen = sizeof(agent->current_task) - 1;
    memcpy(agent->current_task, task_start, tlen);
    agent->current_task[tlen] = '\0';

    // Parse the |X<value> tokens by single-char tag
    const char* p = pipe;
    while (p && *p == '|') {
        char tag = p[1];
        const char* val = p + 2;
        // Find next '|' to bound the value
        const char* next_pipe = strchr(val, '|');
        size_t vlen = next_pipe ? (size_t)(next_pipe - val) : strlen(val);

        char vbuf[32];
        if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf) - 1;
        memcpy(vbuf, val, vlen);
        vbuf[vlen] = '\0';

        switch (tag) {
            case 'Q': agent->quota_used  = (uint32_t)atoi(vbuf);
                      agent->quota_total = 100;             // it's a percentage
                      break;
            case 'P': {
                size_t pl = strlen(vbuf);
                if (pl >= sizeof(agent->project)) pl = sizeof(agent->project) - 1;
                memcpy(agent->project, vbuf, pl);
                agent->project[pl] = '\0';
                break;
            }
            case 'C': agent->cost_cents          = (uint32_t)strtoul(vbuf, NULL, 10); break;
            case 'R': agent->rate_cents_per_hour = (uint32_t)strtoul(vbuf, NULL, 10); break;
            case 'L': {
                // L<added>,<removed>
                char* comma = strchr(vbuf, ',');
                if (comma) {
                    *comma = '\0';
                    agent->lines_added   = (uint32_t)strtoul(vbuf,   NULL, 10);
                    agent->lines_removed = (uint32_t)strtoul(comma+1, NULL, 10);
                }
                break;
            }
            case 'T': agent->session_minutes     = (uint16_t)strtoul(vbuf, NULL, 10); break;
            default: break;  // unknown tag — skip silently
        }

        p = next_pipe;
    }

    agent->online = true;
    agent->last_update = state->timestamp = (uint32_t)time(NULL);
    return true;
}
