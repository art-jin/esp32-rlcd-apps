/*
 * XiaoZhi AI assistant display bridge
 * Routes callbacks to the appropriate display based on active app.
 *
 * Display updates are pushed to a FreeRTOS queue and processed by a
 * low-priority task. This keeps TTS text/state callbacks non-blocking,
 * so the audio main loop is not stalled by 15KB SPI framebuffer writes.
 */

#include "xiaozhi_bridge.h"
#include "app_manager.h"
#include "app_framework.h"
#include "xiaozhi_app_display.h"
#include "codepilot_app.h"  // for STT intercept when CodePilot active
#include "st7306.h"

#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

#define XZ_DISP_QUEUE_LEN   4
#define XZ_DISP_TASK_STACK  4096
#define XZ_DISP_TASK_PRIO   1   // same as xiaozhi_run, below audio tasks

typedef enum {
    DISP_MSG_TEXT,
    DISP_MSG_STATE,
} disp_msg_type_t;

typedef struct {
    disp_msg_type_t type;
    xiaozhi_state_t state;
    char text[256];
} disp_msg_t;

static QueueHandle_t s_disp_queue = NULL;
static TaskHandle_t  s_disp_task  = NULL;

static void display_task(void *arg)
{
    disp_msg_t msg;
    while (1) {
        if (xQueueReceive(s_disp_queue, &msg, portMAX_DELAY) != pdPASS) continue;

        // Drop stale messages queued before app switched away
        if (app_manager_current() != APP_ID_XIAOZHI) continue;

        switch (msg.type) {
        case DISP_MSG_TEXT:
            xiaozhi_app_update_text(msg.text);
            break;
        case DISP_MSG_STATE:
            xiaozhi_app_update_state(msg.state);
            break;
        }
        st7306_update_display();
    }
}

static void ensure_init(void)
{
    if (s_disp_queue) return;
    s_disp_queue = xQueueCreate(XZ_DISP_QUEUE_LEN, sizeof(disp_msg_t));
    xTaskCreate(display_task, "xz_disp", XZ_DISP_TASK_STACK,
                NULL, XZ_DISP_TASK_PRIO, &s_disp_task);
}

static void enqueue(disp_msg_t *msg)
{
    if (!s_disp_queue) return;
    // Non-blocking: if full, drop oldest and push newest so latest state wins
    if (xQueueSendToBack(s_disp_queue, msg, 0) != pdPASS) {
        disp_msg_t old;
        xQueueReceive(s_disp_queue, &old, 0);
        xQueueSendToBack(s_disp_queue, msg, 0);
    }
}

void xiaozhi_on_text(const char *text)
{
    // STT intercept: when CodePilot is active, divert text to its input area
    // instead of the XiaoZhi chat display. This captures both user speech
    // (STT) and AI responses — both are visible to the user in CodePilot.
    if (app_manager_current() == APP_ID_CODEPILOT) {
        codepilot_receive_stt(text);
        return;
    }
    if (app_manager_current() != APP_ID_XIAOZHI) return;
    ensure_init();

    disp_msg_t msg = {.type = DISP_MSG_TEXT};
    if (text) {
        strncpy(msg.text, text, sizeof(msg.text) - 1);
        msg.text[sizeof(msg.text) - 1] = '\0';
    } else {
        msg.text[0] = '\0';
    }
    enqueue(&msg);
}

void xiaozhi_on_state(xiaozhi_state_t state)
{
    if (app_manager_current() != APP_ID_XIAOZHI) return;
    ensure_init();

    disp_msg_t msg = {.type = DISP_MSG_STATE, .state = state};
    msg.text[0] = '\0';
    enqueue(&msg);
}
