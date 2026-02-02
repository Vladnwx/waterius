/**
 * @file sender_http.h
 * @brief Функции отправки сведений по htpp/https
 * @version 0.1
 * @date 2023-02-11
 *
 * @copyright Copyright (c) 2023
 *
 */
#ifndef SENDERHTTP_h_
#define SENDERHTTP_h_
#ifndef HTTPS_DISABLED
#include <ESP8266WiFi.h>
#include "setup.h"
#include "master_i2c.h"
#include "Logging.h"
#include "json.h"
#include "https_helpers.h"
#include "remote_config.h"
#include "config.h"
#include "wifi_helpers.h"

#define HTTP_SEND_ATTEMPTS 3

bool send_http(Settings &sett, JsonDocument &jsonData, const AttinyData &data, MasterI2C &masterI2C)
{
    if (!(sett.http_on && sett.http_url[0]))
    {
        LOG_INFO(F("HTTP: SKIP"));
        return false;
    };

    uint32_t start_time = millis();

    LOG_INFO(F("-- START -- "));
    LOG_INFO(F("HTTP: Send new data"));

    String payload = "";
    serializeJson(jsonData, payload);
    String url = sett.http_url;

    int attempts = HTTP_SEND_ATTEMPTS;
    bool result = false;
    String response_body = "";
    
    do
    {
        LOG_INFO(F("HTTP: Attempt #") << HTTP_SEND_ATTEMPTS - attempts + 1 << F(" from ") << HTTP_SEND_ATTEMPTS);
        result = post_data(url, sett.waterius_key, sett.waterius_email, payload, &response_body);

    } while (!result && --attempts);

    if (result)
    {
        LOG_INFO(F("HTTP: Data sent. Time ") << millis() - start_time << F(" ms"));
        
        // Проверяем конфигурацию только если это НЕ перезагрузка после применения конфига
        // (защита от зацикливания)
        if (!sett.config_restart_pending)
        {
            LOG_INFO(F("HTTP: Checking response for configuration..."));
            bool config_changed = apply_config_from_response(response_body, sett.waterius_key, sett, data, masterI2C);
            
            // Если настройки изменились - перезагружаемся чтобы отправить актуальные данные
            if (config_changed)
            {
                LOG_INFO(F("HTTP: Config changed! Restarting to send updated data..."));
                sett.config_restart_pending = 1;  // Устанавливаем флаг защиты от зацикливания
                store_config(sett);
                wifi_shutdown();
                LOG_END();
                ESP.restart();
                // Сюда не дойдём
            }
        }
        else
        {
            LOG_INFO(F("HTTP: Skipping config check (restart after config change)"));
        }
    }
    else
    {
        LOG_ERROR(F("HTTP: Failed send data. Time ") << millis() - start_time << F(" ms"));
    }

    LOG_INFO(F("-- END --"));

    return result;
}

#endif
#endif