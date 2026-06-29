#ifndef OTA_CLIENT_H
#define OTA_CLIENT_H

#include <string>
#include "esp_err.h"

class OtaClient {
public:
    OtaClient();
    ~OtaClient();

    /**
     * Perform OTA activation: POST system info to server, get WebSocket URL + token.
     * Results are cached in NVS for subsequent calls.
     */
    esp_err_t Activate();

    /**
     * Load previously cached credentials from NVS.
     * Returns ESP_ERR_NOT_FOUND if no credentials stored.
     */
    esp_err_t LoadCredentials(std::string &url, std::string &token, int &version);

private:
    esp_err_t BuildPostBody(std::string &json);
    esp_err_t ParseResponse(const char *json, int len);
    esp_err_t StoreCredentials();
    std::string GetMacAddress();
};

#endif // OTA_CLIENT_H
