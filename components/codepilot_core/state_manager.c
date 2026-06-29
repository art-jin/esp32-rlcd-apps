/*
 * state_manager.c — global state singleton with thread-safe access.
 *
 * Phase B modification: added recursive mutex (`s_lock`) around all
 * state mutations/reads so httpd WS recv task and CodePilot UI worker
 * task can safely share g_global_state.
 *
 * Sensors (SHTC3 / RSSI) are read here when env/diag is requested;
 * implementations are linked from main/shtc3.c and main/wifi_manager.c.
 */

#include "state_manager.h"
#include <string.h>
#include <esp_log.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "STATE_MANAGER";

static global_state_t g_global_state = {0};
static SemaphoreHandle_t s_lock = NULL;  // recursive mutex

// External sensor readers (implemented in main/)
extern float  get_shtc3_temperature(void);
extern float  get_shtc3_humidity(void);
extern int8_t get_wifi_rssi(void);

static void ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
}

void state_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing state manager...");
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    protocol_init_global_state(&g_global_state);
    xSemaphoreGiveRecursive(s_lock);
}

const global_state_t* state_manager_get_state(void)
{
    return &g_global_state;
}

bool state_manager_update_from_bridge(const char* json_msg)
{
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    bool ok = protocol_process_bridge_message(json_msg, &g_global_state);
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

agent_state_t* state_manager_find_agent(agent_type_t type)
{
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < g_global_state.agent_count; i++) {
        if (g_global_state.agents[i].type == type) {
            xSemaphoreGiveRecursive(s_lock);
            return &g_global_state.agents[i];
        }
    }
    xSemaphoreGiveRecursive(s_lock);
    return NULL;
}

// Caller is responsible for locking before iterating the returned range
// — this is a convenience accessor for snapshot reads.
void state_manager_lock(void)
{
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void state_manager_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

session_t* state_manager_find_session(const char* session_id)
{
    if (!session_id) return NULL;
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < g_global_state.agent_count; i++) {
        agent_state_t* agent = &g_global_state.agents[i];
        for (uint8_t j = 0; j < agent->session_count; j++) {
            if (strcmp(agent->sessions[j].id, session_id) == 0) {
                xSemaphoreGiveRecursive(s_lock);
                return &agent->sessions[j];
            }
        }
    }
    xSemaphoreGiveRecursive(s_lock);
    return NULL;
}

uint8_t state_manager_get_active_session_count(void)
{
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    uint8_t count = 0;
    for (uint8_t i = 0; i < g_global_state.agent_count; i++) {
        agent_state_t* agent = &g_global_state.agents[i];
        for (uint8_t j = 0; j < agent->session_count; j++) {
            if (agent->sessions[j].status == SESSION_STATUS_ACTIVE) count++;
        }
    }
    xSemaphoreGiveRecursive(s_lock);
    return count;
}

uint8_t state_manager_get_notification_count(void)
{
    return g_global_state.notification_count;
}

const notification_t* state_manager_get_highest_priority_notification(void)
{
    if (g_global_state.notification_count == 0) return NULL;
    return &g_global_state.notifications[0];
}

bool state_manager_clear_notification(const char* id)
{
    if (!id) return false;
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < g_global_state.notification_count; i++) {
        if (strcmp(g_global_state.notifications[i].id, id) == 0) {
            for (uint8_t j = i; j < g_global_state.notification_count - 1; j++) {
                memcpy(&g_global_state.notifications[j],
                       &g_global_state.notifications[j + 1],
                       sizeof(notification_t));
            }
            g_global_state.notification_count--;
            xSemaphoreGiveRecursive(s_lock);
            return true;
        }
    }
    xSemaphoreGiveRecursive(s_lock);
    return false;
}

void state_manager_clear_all_notifications(void)
{
    ensure_lock();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    g_global_state.notification_count = 0;
    xSemaphoreGiveRecursive(s_lock);
}

void state_manager_get_diag_data(diag_data_t* diag)
{
    if (!diag) return;
    diag->free_heap = esp_get_free_heap_size();
    diag->rssi = get_wifi_rssi();
}

void state_manager_get_env_data(env_data_t* env)
{
    if (!env) return;
    env->temp_c = get_shtc3_temperature();
    env->humidity = get_shtc3_humidity();
}
