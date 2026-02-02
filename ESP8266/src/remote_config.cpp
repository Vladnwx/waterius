/**
 * @file remote_config.cpp
 * @brief Получение конфигурации устройства с удаленного сервера
 * @version 1.0
 * @date 2026-01-15
 * 
 * @copyright Copyright (c) 2026
 * 
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * 
 * This file is part of Waterius project.
 * Licensed under the GNU Lesser General Public License v3.0 or later.
 * See LICENSE file in the project root for full license information.
 * 
 */
#include "remote_config.h"
#include "Logging.h"
#include "utils.h"
#include "config.h"
#include "https_helpers.h"
#include <ESP8266WiFi.h>
#include "ESP8266HTTPClient.h"
#include <ArduinoJson.h>


/**
 * @brief Проверка авторизации по ключу устройства
 * 
 * Проверяет, что ключ в ответе сервера совпадает с ключом устройства.
 * Это защищает от подмены конфигурации злоумышленником.
 * 
 * @param config_json JSON документ с конфигурацией от сервера
 * @param key Ключ устройства для проверки
 * @return true если авторизация успешна, false если ключ не совпадает или отсутствует
 */
static bool validate_device_key(const JsonDocument &config_json, const char *key)
{
    if(!config_json["key"].isNull()) {
        String server_key = config_json["key"].as<String>();
        if(server_key != String(key)) {
            LOG_ERROR(F("RCFG: Authorization failed - key mismatch!"));
            LOG_ERROR(F("RCFG: Expected key: ") << key);
            LOG_ERROR(F("RCFG: Received key: ") << server_key);
            return false;
        }
        LOG_INFO(F("RCFG: Authorization successful - key verified"));
        return true;
    } else {
        LOG_ERROR(F("RCFG: Authorization failed - no key in server response"));
        return false;
    }
}

/**
 * @brief Получить конфигурацию с сервера через POST запрос к /cfg
 * 
 * Отправляет POST запрос с ключом устройства на эндпоинт {url}/cfg и получает
 * JSON конфигурацию от сервера. Выполняет проверку размера ответа для защиты
 * от переполнения памяти ESP8266.
 * 
 * @param url Базовый URL сервера (без /cfg), например "https://cloud.waterius.ru"
 * @param key Ключ устройства для авторизации (waterius_key)
 * @param response_json Выходной параметр - JSON документ с конфигурацией от сервера
 * 
 * @return true если конфигурация успешно получена и распарсена
 * @return false в случае ошибки (нет соединения, неверный ответ, ошибка парсинга)
 */
static bool fetch_config_from_server(const String &url, const char *key, JsonDocument &response_json)
{
    if (!key || key[0] == '\0')
    {
        LOG_ERROR(F("RCFG: Key is empty, skipping config fetch"));
        return false;
    }

    void *pClient = nullptr;
    HTTPClient httpClient;
    bool result = false;
    
    // Формируем URL для запроса конфигурации
    String cfg_url = url;
    if (!cfg_url.endsWith("/"))
    {
        cfg_url += "/";
    }
    cfg_url += "cfg";
    
    LOG_INFO(F("RCFG: Fetching configuration from server"));
    LOG_INFO(F("RCFG: URL: ") << cfg_url);
    
    // Формируем JSON с ключом устройства
    JsonDocument request_json;
    request_json["key"] = key;
    String payload;
    serializeJson(request_json, payload);
    LOG_INFO(F("RCFG: Request body: ") << payload);

    String proto = get_proto(url);
    LOG_INFO(F("RCFG: Protocol: ") << proto);

    // Создаем клиент
    if (proto == PROTO_HTTP)
    {
        LOG_INFO(F("RCFG: Create insecure client"));
        pClient = new WiFiClient;
    }
    else if (proto == PROTO_HTTPS)
    {
        LOG_INFO(F("RCFG: Create secure client"));
        pClient = new WiFiClientSecure;
        (*(WiFiClientSecure *)pClient).setInsecure();
    }

    // HTTP настройки
    httpClient.setTimeout(SERVER_TIMEOUT);
    httpClient.setReuse(false);

    if (httpClient.begin(*(WiFiClient *)pClient, cfg_url))
    {
        httpClient.addHeader(F("Content-Type"), F("application/json"));
        
        LOG_INFO(F("RCFG: Sending POST request"));
        int response_code = httpClient.POST(payload);
        LOG_INFO(F("RCFG: Response code: ") << response_code);
        
        // Используем общую функцию валидации и получения ответа
        String response_body;
        if (validate_and_get_response(httpClient, response_code, response_body, "RCFG"))
        {
            // Парсим JSON ответ
            DeserializationError error = deserializeJson(response_json, response_body);
            if (error)
            {
                LOG_ERROR(F("RCFG: Failed to parse JSON response: ") << error.c_str());
                result = false;
            }
            else
            {
                LOG_INFO(F("RCFG: Configuration successfully received and parsed"));
                result = true;
            }
        }
        else
        {
            LOG_ERROR(F("RCFG: Failed to get valid response from server"));
            result = false;
        }
        
        httpClient.end();
        (*(WiFiClient *)pClient).stop();
    }
    else
    {
        LOG_ERROR(F("RCFG: Failed to connect to server"));
    }

    // Освобождаем память клиента
    if (proto == PROTO_HTTP)
    {
        delete (WiFiClient *)pClient;
    }
    else if (proto == PROTO_HTTPS)
    {
        delete (WiFiClientSecure *)pClient;
    }

    return result;
}

/**
 * @brief Применение настроек полученных с сервера
 * 
 * Обрабатывает JSON конфигурацию от сервера и применяет настройки к устройству.
 * Поддерживает обновление показаний счетчиков, серийных номеров, имён счетчиков,
 * весов импульсов, типов счетчиков, Wi-Fi, MQTT, HTTP, NTP настроек.
 * 
 * @param sett Структура настроек устройства для обновления
 * @param config_json JSON документ с настройками от сервера
 * @param data Данные от Attiny85 (для получения текущих типов счетчиков)
 * @param masterI2C Ссылка на объект для обновления типов счетчиков на Attiny85
 * 
 * @return true если хотя бы одна настройка была изменена
 * @return false если изменений не было
 */
// Макрос для применения числового параметра из JSON с валидацией диапазона
#define APPLY_CFG_NUM(json, key, target, min_val, max_val, changed) \
    if(!json[key].isNull()) { \
        auto val = json[key].as<decltype(target)>(); \
        if(val >= (min_val) && val <= (max_val)) { \
            target = val; \
            changed = true; \
            LOG_INFO(F("RCFG: ") << F(key) << F("=") << target); \
        } \
    }

// Макрос для применения строкового параметра из JSON
#define APPLY_CFG_STR(json, key, target, max_len, changed) \
    if(!json[key].isNull()) { \
        String s = json[key].as<String>(); \
        if(s.length() <= (max_len) - 1) { \
            strncpy0(target, s.c_str(), max_len); \
            changed = true; \
            LOG_INFO(F("RCFG: ") << F(key) << F(" updated")); \
        } \
    }

static bool apply_config_from_server(Settings &sett, const JsonDocument &config_json, const AttinyData &data, MasterI2C &masterI2C)
{
    bool config_changed = false;
    
    LOG_INFO(F("RCFG: Applying config..."));
    
    // Показания счетчиков
    APPLY_CFG_NUM(config_json, "channel0", sett.channel0_start, 0.0f, 999999.0f, config_changed);
    APPLY_CFG_NUM(config_json, "channel1", sett.channel1_start, 0.0f, 999999.0f, config_changed);
    
    // Серийные номера
    APPLY_CFG_STR(config_json, "serial0", sett.serial0, SERIAL_LEN, config_changed);
    APPLY_CFG_STR(config_json, "serial1", sett.serial1, SERIAL_LEN, config_changed);
    
    // Названия счетчиков
    APPLY_CFG_NUM(config_json, "cname0", sett.counter0_name, (uint8_t)0, COUNTER_NAME_MAX, config_changed);
    APPLY_CFG_NUM(config_json, "cname1", sett.counter1_name, (uint8_t)0, COUNTER_NAME_MAX, config_changed);
    
    // Веса импульсов
    APPLY_CFG_NUM(config_json, "factor0", sett.factor0, (uint16_t)1, (uint16_t)10000, config_changed);
    APPLY_CFG_NUM(config_json, "factor1", sett.factor1, (uint16_t)1, (uint16_t)10000, config_changed);
    
    // Импульсы счетчиков
    if(!config_json["impulses0"].isNull()) {
        uint32_t imp = config_json["impulses0"];
        sett.impulses0_start = sett.impulses0_previous = imp;
        config_changed = true;
        LOG_INFO(F("RCFG: imp0=") << imp);
    }
    if(!config_json["impulses1"].isNull()) {
        uint32_t imp = config_json["impulses1"];
        sett.impulses1_start = sett.impulses1_previous = imp;
        config_changed = true;
        LOG_INFO(F("RCFG: imp1=") << imp);
    }
    
    // Типы счетчиков (требуют особой обработки - передаются на Attiny)
    bool ct0_present = !config_json["ctype0"].isNull();
    bool ct1_present = !config_json["ctype1"].isNull();
    
    if(ct0_present || ct1_present) {
        uint8_t ct0 = ct0_present ? config_json["ctype0"].as<uint8_t>() : data.counter_type0;
        uint8_t ct1 = ct1_present ? config_json["ctype1"].as<uint8_t>() : data.counter_type1;
        
        bool valid = (ct0 == NAMUR || ct0 == ELECTRONIC || ct0 == NONE) &&
                     (ct1 == NAMUR || ct1 == ELECTRONIC || ct1 == NONE);
        
        if(valid && masterI2C.setCountersType(ct0, ct1)) {
            config_changed = true;
            LOG_INFO(F("RCFG: ctype=") << ct0 << F(",") << ct1);
        } else if(!valid) {
            LOG_ERROR(F("RCFG: bad ctype"));
        }
    }
    
    // Период пробуждения
    if(!config_json["wakeup_per_min"].isNull()) {
        uint16_t period = config_json["wakeup_per_min"];
        if(period >= 1 && period <= 1440) {
            sett.wakeup_per_min = period;
            reset_period_min_tuned(sett);
            config_changed = true;
            LOG_INFO(F("RCFG: wakeup=") << period);
        }
    }
    
    // Режим "только при потреблении"
    APPLY_CFG_NUM(config_json, "wake_on_consumption_only", sett.wake_on_consumption_only, (uint8_t)0, (uint8_t)1, config_changed);
    
    // Wi-Fi настройки
    APPLY_CFG_STR(config_json, "ssid", sett.wifi_ssid, WIFI_SSID_LEN, config_changed);
    APPLY_CFG_STR(config_json, "password", sett.wifi_password, WIFI_PWD_LEN, config_changed);
    
    // MQTT настройки
    if(!config_json["mqtt_on"].isNull()) {
        sett.mqtt_on = config_json["mqtt_on"].as<bool>();
        config_changed = true;
        LOG_INFO(F("RCFG: mqtt_on=") << sett.mqtt_on);
    }
    
    if(sett.mqtt_on) {
        APPLY_CFG_STR(config_json, "mqtt_host", sett.mqtt_host, HOST_LEN, config_changed);
        APPLY_CFG_NUM(config_json, "mqtt_port", sett.mqtt_port, (uint16_t)1, (uint16_t)65535, config_changed);
        APPLY_CFG_STR(config_json, "mqtt_login", sett.mqtt_login, MQTT_LOGIN_LEN, config_changed);
        APPLY_CFG_STR(config_json, "mqtt_password", sett.mqtt_password, MQTT_PASSWORD_LEN, config_changed);
        APPLY_CFG_STR(config_json, "mqtt_topic", sett.mqtt_topic, MQTT_TOPIC_LEN, config_changed);
    }
    
    // HTTP настройки
    if(!config_json["http_on"].isNull()) {
        sett.http_on = config_json["http_on"].as<bool>();
        config_changed = true;
        LOG_INFO(F("RCFG: http_on=") << sett.http_on);
    }
    
    if(sett.http_on) {
        APPLY_CFG_STR(config_json, "http_url", sett.http_url, HOST_LEN, config_changed);
    }
    
    // NTP сервер
    APPLY_CFG_STR(config_json, "ntp_server", sett.ntp_server, HOST_LEN, config_changed);
    
    // Waterius настройки
    APPLY_CFG_STR(config_json, "waterius_host", sett.waterius_host, HOST_LEN, config_changed);
    APPLY_CFG_STR(config_json, "waterius_key", sett.waterius_key, WATERIUS_KEY_LEN, config_changed);
    APPLY_CFG_STR(config_json, "waterius_email", sett.waterius_email, EMAIL_LEN, config_changed);
    
    if(!config_json["waterius_on"].isNull()) {
        sett.waterius_on = config_json["waterius_on"].as<bool>();
        config_changed = true;
        LOG_INFO(F("RCFG: waterius_on=") << sett.waterius_on);
    }
    
    // Организация и место
    APPLY_CFG_STR(config_json, "company", sett.company, COMPANY_LEN, config_changed);
    APPLY_CFG_STR(config_json, "place", sett.place, PLACE_LEN, config_changed);
    
    // Home Assistant auto discovery
    if(!config_json["mqtt_auto_discovery"].isNull()) {
        sett.mqtt_auto_discovery = config_json["mqtt_auto_discovery"].as<bool>();
        config_changed = true;
        LOG_INFO(F("RCFG: mqtt_auto_discovery=") << sett.mqtt_auto_discovery);
    }
    APPLY_CFG_STR(config_json, "mqtt_discovery_topic", sett.mqtt_discovery_topic, MQTT_TOPIC_LEN, config_changed);
    
    // Сетевые настройки (DHCP/статический IP)
    if(!config_json["dhcp_off"].isNull()) {
        sett.dhcp_off = config_json["dhcp_off"].as<bool>();
        config_changed = true;
        LOG_INFO(F("RCFG: dhcp_off=") << sett.dhcp_off);
    }
    
    // Статический IP (применяется только если dhcp_off)
    if(sett.dhcp_off) {
        if(!config_json["static_ip"].isNull()) {
            IPAddress ip;
            if(ip.fromString(config_json["static_ip"].as<String>())) {
                sett.ip = ip;
                config_changed = true;
                LOG_INFO(F("RCFG: ip=") << ip.toString());
            }
        }
        if(!config_json["gateway"].isNull()) {
            IPAddress gw;
            if(gw.fromString(config_json["gateway"].as<String>())) {
                sett.gateway = gw;
                config_changed = true;
                LOG_INFO(F("RCFG: gw=") << gw.toString());
            }
        }
        if(!config_json["mask"].isNull()) {
            IPAddress mask;
            if(mask.fromString(config_json["mask"].as<String>())) {
                sett.mask = mask;
                config_changed = true;
                LOG_INFO(F("RCFG: mask=") << mask.toString());
            }
        }
    }
    
    // mDNS
    if(!config_json["mdns_on"].isNull()) {
        sett.mdns_on = config_json["mdns_on"].as<bool>();
        config_changed = true;
        LOG_INFO(F("RCFG: mdns_on=") << sett.mdns_on);
    }
    
    LOG_INFO(F("RCFG: changed=") << config_changed);
    
    return config_changed;
}

/**
 * @brief Получить и применить конфигурацию с удаленного сервера
 * 
 * Основная функция для получения конфигурации устройства с удаленного сервера.
 * Отправляет POST запрос на эндпоинт {url}/cfg, проверяет авторизацию по ключу,
 * применяет полученные настройки и сохраняет их в EEPROM.
 * 
 * Используется при ручной отправке данных (нажатие кнопки) для обновления
 * настроек устройства с сервера.
 * 
 * @param url Базовый URL сервера (без /cfg)
 * @param key Ключ устройства для авторизации
 * @param sett Структура настроек устройства для обновления
 * @param data Данные от Attiny85 (для получения текущих типов счетчиков)
 * @param masterI2C Ссылка на объект для работы с Attiny85
 * 
 * @return true если конфигурация успешно получена, применена и сохранена
 * @return false в случае ошибки на любом этапе
 */
bool fetch_and_apply_remote_config(const String &url, const char *key, Settings &sett, const AttinyData &data, MasterI2C &masterI2C)
{
    LOG_INFO(F("RCFG: fetch cfg..."));
    
    JsonDocument config_json;
    
    if (!fetch_config_from_server(url, key, config_json))
    {
        LOG_ERROR(F("RCFG: fetch failed"));
        return false;
    }
    
    if (!validate_device_key(config_json, key)) {
        return false;
    }
    
    if (apply_config_from_server(sett, config_json, data, masterI2C))
    {
        LOG_INFO(F("RCFG: saving..."));
        store_config(sett);
        LOG_INFO(F("RCFG: saved OK"));
        return true;
    }
    
    LOG_INFO(F("RCFG: no changes"));
    return false;
}


/**
 * @brief Применить конфигурацию из ответа сервера
 * 
 * Эта функция используется когда сервер возвращает настройки в ответе
 * на обычную отправку данных (вместо отдельного запроса /cfg)
 */
bool apply_config_from_response(const String &response_body, const char *key, Settings &sett, const AttinyData &data, MasterI2C &masterI2C)
{
    LOG_INFO(F("RCFG: Checking response for configuration..."));
    
    // Если ответ пустой или слишком короткий - пропускаем
    if (response_body.length() < 10)
    {
        LOG_INFO(F("RCFG: Response too short, no configuration"));
        return false;
    }
    
    // Резервная проверка размера (основная проверка в https_helpers.cpp)
    // Эта проверка на случай если response_body пришел из другого источника
    if (response_body.length() > REMOTE_CONFIG_MAX_SIZE)
    {
        LOG_ERROR(F("RCFG: Response too large: ") << response_body.length() << F(" bytes (max: ") << REMOTE_CONFIG_MAX_SIZE << F(" bytes)"));
        return false;
    }
    
    // Проверяем, содержит ли ответ JSON
    if (!response_body.startsWith("{") && !response_body.startsWith("["))
    {
        LOG_INFO(F("RCFG: Response is not JSON, no configuration"));
        return false;
    }
    
    // Парсим JSON
    JsonDocument config_json;
    DeserializationError error = deserializeJson(config_json, response_body);
    
    if (error)
    {
        LOG_ERROR(F("RCFG: parse err: ") << error.c_str());
        return false;
    }
    
    if (!validate_device_key(config_json, key)) {
        if (config_json["key"].isNull()) {
            LOG_INFO(F("RCFG: no cfg in response"));
        }
        return false;
    }
    
    LOG_INFO(F("RCFG: cfg in response"));
    
    if (apply_config_from_server(sett, config_json, data, masterI2C))
    {
        LOG_INFO(F("RCFG: applied, saving..."));
        store_config(sett);
        return true;
    }
    
    LOG_INFO(F("RCFG: no changes"));
    return false;
}
