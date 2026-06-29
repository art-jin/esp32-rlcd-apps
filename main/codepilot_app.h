#ifndef CODEPILOT_APP_H
#define CODEPILOT_APP_H

#include "app_framework.h"

void codepilot_on_enter(void);
void codepilot_on_exit(void);
void codepilot_on_key(key_event_t key);
void codepilot_on_tick_1s(void);

/**
 * Receive an STT (speech-to-text) result from XiaoZhi pipeline.
 * Called by xiaozhi_display.c when current app is CodePilot and
 * XiaoZhi emits a text message (user speech or AI response).
 * Stores the text in the input area buffer + marks dirty.
 */
void codepilot_receive_stt(const char *text);

#endif // CODEPILOT_APP_H
