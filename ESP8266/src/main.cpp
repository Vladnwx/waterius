#include <user_interface.h>
#include <umm_malloc/umm_heap_select.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include "Logging.h"
#include "config.h"
#include "master_i2c.h"
#include "senders/sender_waterius.h"
#include "senders/sender_http.h"
#include "senders/sender_mqtt.h"
#include "portal/active_point.h"
#include "remote_config.h"
#include "voltage.h"
#include "utils.h"
#include "porting.h"
#include "json.h"
#include "Ticker.h"
#include "sync_time.h"
#include "wifi_helpers.h"

MasterI2C masterI2C;  // Для общения с Attiny85 по i2c
AttinyData data;       // Данные от Attiny85
Settings sett;        // Настройки соединения и предыдущие показания из EEPROM
CalculatedData cdata; // вычисляемые данные
ADC_MODE(ADC_VCC);
Ticker voltage_ticker;

/*
Выполняется однократно при включении
*/
void setup()
{
    LOG_BEGIN(115200); // Включаем логгирование на пине TX, 115200 8N1
    LOG_INFO(F("Waterius\n========\n"));
    LOG_INFO(F("Build: ") << __DATE__ << F(" ") << __TIME__);

    static_assert((sizeof(Settings) == 960), "sizeof Settings != 960");

    masterI2C.begin(); // Включаем i2c master

    HeapSelectIram ephemeral;
    LOG_INFO(F("IRAM free: ") << ESP.getFreeHeap() << F(" bytes"));
    {
        HeapSelectDram ephemeral;
        LOG_INFO(F("DRAM free: ") << ESP.getFreeHeap() << F(" bytes"));
    }
    LOG_INFO(F("ChipId: ") << String(getChipId(), HEX));
    LOG_INFO(F("FlashChipId: ") << String(ESP.getFlashChipId(), HEX));

    get_voltage()->begin();
    voltage_ticker.attach_ms(300, []()
                             { get_voltage()->update(); }); // через каждые 300 мс будет измеряться напряжение
}

void loop()
{
    uint8_t mode = TRANSMIT_MODE; // TRANSMIT_MODE;
    bool config_loaded = false;
    bool skip_transmission = false;  // Флаг пропуска передачи (для режима "только при потреблении")

    // спрашиваем у Attiny85 повод пробуждения и данные true) 
    if (masterI2C.getMode(mode) && masterI2C.getAttinyData(data))
    {
        // Загружаем конфигурацию из EEPROM
        config_loaded = load_config(sett);
        sett.mode = mode;
        LOG_INFO(F("Startup mode: ") << mode);
        
        // Проверяем флаг перезагрузки после применения конфигурации
        // Если флаг установлен - значит мы перезагрузились после изменения настроек
        // и теперь отправим актуальные данные
        if (sett.config_restart_pending)
        {
            LOG_INFO(F("Restart after config change detected - will send updated data"));
            // Флаг сбросится после успешной отправки данных (перед sleep)
        }

        // Вычисляем текущие показания
        calculate_values(sett, data, cdata);

        if (mode == SETUP_MODE)
        {
            LOG_INFO(F("Entering in setup mode..."));
            // Режим настройки - запускаем точку доступа на 192.168.4.1
            // Запускаем точку доступа с вебсервером

            start_active_point(sett, cdata);

            sett.setup_time = millis();
            sett.setup_finished_counter++;

            store_config(sett);

            wifi_shutdown();

            LOG_INFO(F("Set mode MANUAL_TRANSMIT to attiny"));
            masterI2C.setTransmitMode(); // Режим "Передача"

            LOG_INFO(F("Restart ESP"));
            LOG_END();

            LOG_INFO(F("Finish setup mode..."));
            ESP.restart();

            return; // сюда не должно дойти никогда
        }

        // Режим "только при потреблении воды" - проверяем нужна ли передача
        // Работает только в автоматическом режиме (TRANSMIT_MODE)
        // При ручном пробуждении (MANUAL_TRANSMIT_MODE) всегда отправляем
        LOG_INFO(F("WOC check: mode=") << mode << F(" wake_on_consumption_only=") << sett.wake_on_consumption_only);
        
        if (config_loaded && sett.wake_on_consumption_only && mode == TRANSMIT_MODE)
        {
            bool has_consumption = (cdata.delta0 > 0 || cdata.delta1 > 0);
            
            // Проверяем, нужен ли heartbeat (прошло ли ~24ч с последней отправки)
            // Используем счётчик пробуждений, т.к. time(nullptr) невалидно до NTP синхронизации
            // Максимум пробуждений до heartbeat = 24ч * 60мин / период_пробуждения
            uint16_t max_wakeups_before_heartbeat = (24 * 60) / sett.wakeup_per_min;
            if (max_wakeups_before_heartbeat == 0) max_wakeups_before_heartbeat = 1;  // защита от деления на 0
            
            bool heartbeat_needed = (sett.wakeups_without_send >= max_wakeups_before_heartbeat);
            
            LOG_INFO(F("WOC: delta0=") << cdata.delta0 << F(" delta1=") << cdata.delta1);
            LOG_INFO(F("WOC: wakeups_without_send=") << sett.wakeups_without_send 
                << F(" max=") << max_wakeups_before_heartbeat);
            LOG_INFO(F("WOC: consumption=") << has_consumption << F(" heartbeat=") << heartbeat_needed);
            
            if (!has_consumption && !heartbeat_needed)
            {
                LOG_INFO(F("WOC: No consumption and no heartbeat needed - skipping transmission"));
                skip_transmission = true;
                
                // Обновляем impulses_previous для корректного расчёта delta в следующий раз
                sett.impulses0_previous = data.impulses0;
                sett.impulses1_previous = data.impulses1;
                
                // Увеличиваем счётчик пробуждений без отправки
                if (sett.wakeups_without_send < UINT16_MAX) {
                    sett.wakeups_without_send++;
                }
            }
            else
            {
                LOG_INFO(F("WOC: Proceeding with transmission"));
                // Счётчик сбросится после успешной отправки (в update_config или после wifi)
            }
        }

        if (config_loaded)
        {
            if (!skip_transmission && wifi_connect(sett))
            {
                log_system_info();

                JsonDocument json_data;

#ifndef MQTT_DISABLED
                // Подключаемся и подписываемся на мктт
                if (is_mqtt(sett))
                {
                    connect_and_subscribe_mqtt(sett, data, cdata, json_data);
                }
#endif
                // Синхронизация времени NTP раз в неделю (или при необходимости)
                // Нужно для HTTPS (проверка сертификатов) и MQTT
                if (is_mqtt(sett) || is_https(sett.waterius_host) || is_https(sett.http_url))
                {
                    // Интервал синхронизации: 7 дней = 604800 секунд
                    const time_t NTP_SYNC_INTERVAL_SEC = 7 * 24 * 60 * 60;
                    
                    time_t now = time(nullptr);
                    bool need_sync = false;
                    
                    // Синхронизировать если:
                    // 1. Текущее время невалидно (первый запуск или сбой)
                    if (!is_valid_time(now)) {
                        need_sync = true;
                        LOG_INFO(F("NTP: Sync needed - invalid current time"));
                    }
                    // 2. Никогда не синхронизировались
                    else if (!is_valid_time(sett.last_ntp_sync)) {
                        need_sync = true;
                        LOG_INFO(F("NTP: Sync needed - never synced before"));
                    }
                    // 3. Прошло больше недели с последней синхронизации
                    else if (difftime(now, sett.last_ntp_sync) > NTP_SYNC_INTERVAL_SEC) {
                        need_sync = true;
                        LOG_INFO(F("NTP: Sync needed - interval exceeded (") 
                            << (int)(difftime(now, sett.last_ntp_sync) / 86400) << F(" days)"));
                    }
                    // 4. Ручное пробуждение - синхронизируем для актуальности
                    else if (mode == MANUAL_TRANSMIT_MODE) {
                        need_sync = true;
                        LOG_INFO(F("NTP: Sync needed - manual mode"));
                    }
                    else {
                        LOG_INFO(F("NTP: Skipping sync - last sync ") 
                            << (int)(difftime(now, sett.last_ntp_sync) / 86400) << F(" days ago"));
                    }
                    
                    if (need_sync) {
                        if (sync_ntp_time(sett)) {
                            sett.last_ntp_sync = time(nullptr);
                            LOG_INFO(F("NTP: Sync successful, saved timestamp"));
                        } else {
                            sett.ntp_error_counter++;
                        }
                    }
                }

                voltage_ticker.detach(); // перестаем обновлять перед созданием объекта с данными
                LOG_INFO(F("Free memory: ") << ESP.getFreeHeap());

                // Формироуем JSON
                get_json_data(sett, data, cdata, json_data);

                LOG_INFO(F("Free memory: ") << ESP.getFreeHeap());

#ifndef WATERIUS_RU_DISABLED
                if (send_waterius(sett, json_data, data, masterI2C))
                {
                    LOG_INFO(F("HTTP: Send OK"));
                }
#endif

#ifndef HTTPS_DISABLED
                if (send_http(sett, json_data, data, masterI2C))
                {
                    LOG_INFO(F("HTTP: Send OK"));
                }
#endif

#ifndef MQTT_DISABLED
                if (is_mqtt(sett))
                {
                    if (send_mqtt(sett, data, cdata, json_data))
                    {
                        LOG_INFO(F("MQTT: Send OK"));
                    }
                }
                else
                {
                    LOG_INFO(F("MQTT: SKIP"));
                }
#endif
                
                // В режиме ручного пробуждения (кнопкой) получаем конфигурацию с сервера
                // Сначала пытаемся получить настройки из ответа сервера (уже обработано в send_xxx)
                // Запрашиваем через отдельный эндпоинт /cfg
                // Пропускаем если это перезагрузка после применения конфига (защита от зацикливания)
                if (mode == MANUAL_TRANSMIT_MODE && !sett.config_restart_pending)
                {
                    LOG_INFO(F("Manual mode: Trying to fetch configuration via /cfg endpoint..."));
                    
                    bool config_changed = false;
                    
                    // Пытаемся получить конфигурацию с основного сервера waterius
                    if (is_waterius_site(sett))
                    {
                        config_changed = fetch_and_apply_remote_config(sett.waterius_host, sett.waterius_key, sett, data, masterI2C);
                    }
                    // Если основной сервер не настроен, пробуем HTTP сервер
                    else if (sett.http_on && sett.http_url[0])
                    {
                        config_changed = fetch_and_apply_remote_config(sett.http_url, sett.waterius_key, sett, data, masterI2C);
                    }
                    
                    // Если настройки изменились - перезагружаемся чтобы отправить актуальные данные
                    if (config_changed)
                    {
                        LOG_INFO(F("Config changed via /cfg! Restarting to send updated data..."));
                        sett.config_restart_pending = 1;
                        store_config(sett);
                        wifi_shutdown();
                        LOG_END();
                        ESP.restart();
                        // Сюда не дойдём
                    }
                }
                else if (sett.config_restart_pending)
                {
                    LOG_INFO(F("Skipping /cfg fetch (restart after config change)"));
                }
                
                // Все уже отправили,  wifi не нужен - выключаем
                wifi_shutdown();

                update_config(sett, data, cdata);
                
                // Сбрасываем счётчик пробуждений без отправки после успешной передачи
                if (sett.wake_on_consumption_only && sett.wakeups_without_send > 0)
                {
                    LOG_INFO(F("WOC: Resetting wakeups_without_send counter"));
                    sett.wakeups_without_send = 0;
                }

                if (!masterI2C.setWakeUpPeriod(sett.period_min_tuned))
                {
                    LOG_ERROR(F("Wakeup period wasn't set"));
                }
            }
            // Сбрасываем флаг перезагрузки после конфига перед засыпанием
            // чтобы следующее пробуждение было нормальным
            if (sett.config_restart_pending)
            {
                LOG_INFO(F("Clearing config_restart_pending flag"));
                sett.config_restart_pending = 0;
            }
            
            store_config(sett);  // т.к. сохраняем число ошибок подключения
        }
    }
    
    if (!config_loaded)
    {
        delay(500);
        blink_led(3, 1000, 500);
    }

    LOG_INFO(F("Going to sleep"));
    LOG_END();

    uint8_t vendor_id = ESP.getFlashChipVendorId();

    masterI2C.setSleep(); // через 20мс attiny отключит EN

    // { 0xC4, "Giantec Semiconductor, Inc." }, https://github.com/elitak/freeipmi/blob/master/libfreeipmi/spec/ipmi-jedec-manufacturer-identification-code-spec.c
    if (vendor_id != 0xC4) 
    {
        ESP.deepSleepInstant(0, RF_DEFAULT); // Спим до следущего включения EN. (выключили Instant не ждет 92мс)
    } 
    
    while(true) yield();
}