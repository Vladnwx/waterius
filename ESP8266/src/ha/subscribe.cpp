#include "subscribe.h"
#include "Logging.h"
#include "publish.h"
#include "publish_data.h"
#include "config.h"
#include "utils.h"

#define MQTT_MAX_TRIES 5
#define MQTT_CONNECT_DELAY 100
#define MQTT_SUBSCRIPTION_TOPIC "/#"

extern MasterI2C masterI2C;

// Контекст для отслеживания текущих типов счётчиков в MQTT сессии.
// Нужен т.к. data.counter_typeX не обновляется после setCountersType().
struct MqttCounterContext {
    int16_t ctype0 = -1;  // -1 = не инициализировано
    int16_t ctype1 = -1;
    
    void init(const AttinyData &data) {
        if (ctype0 < 0) ctype0 = data.counter_type0;
        if (ctype1 < 0) ctype1 = data.counter_type1;
    }
    
    void reset() {
        ctype0 = -1;
        ctype1 = -1;
    }
};

static MqttCounterContext mqtt_ctx;

/**
 * @brief Сброс контекста MQTT счётчиков (вызывать при новой MQTT сессии)
 */
void reset_mqtt_counter_context()
{
    mqtt_ctx.reset();
}

/**
 * @brief Обновление настроек по сообщению MQTT
 *
 * @param topic топик
 * @param payload данные из топика
 * @param sett настройки
 * @param json_data данные в JSON
 */
bool update_settings(String &topic, String &payload, Settings &sett, const AttinyData &data, JsonDocument &json_data)
{
    bool updated = false;
    
    // Инициализация контекста при первом вызове в сессии
    mqtt_ctx.init(data);
    
    if (topic.endsWith(F("/set"))) // пришла команда на изменение
    {
        // извлекаем имя параметра
        int endslash = topic.lastIndexOf('/');
        int prevslash = topic.lastIndexOf('/', endslash - 1);
        String param = topic.substring(prevslash + 1, endslash);
        LOG_INFO(F("MQTT: param=") << param);

        // period_min
        if (param.equals(F("period_min")))
        {
            int period_min = payload.toInt();
            if (period_min > 0)
            {
                // обновили в настройках
                if (sett.wakeup_per_min != period_min)
                {
                    LOG_INFO(F("MQTT: old wakeup_per_min=") << sett.wakeup_per_min);
                    sett.wakeup_per_min = period_min;
                    reset_period_min_tuned(sett);

                    // если есть ключ то время уже получено и json уже сформирован, можно отправлять
                    if (json_data["period_min"].is<int>())   //todo добавить F("")
                    {
                        json_data[F("period_min")] = period_min;
                        updated = true;
                    }
                    LOG_INFO(F("MQTT: new wakeup_per_min=") << sett.wakeup_per_min);
                }
            }
        } else if (param.equals(F("f0")))
        {
            int f0 = payload.toInt();
            if (f0 > 0)
            {
                if (sett.factor0 != f0)
                {
                    LOG_INFO(F("MQTT: old f0=") << sett.factor0);
                    sett.factor0 = f0;
                    if (json_data["f0"].is<int>())
                    {
                        json_data[F("f0")] = f0;
                        updated = true;
                    }
                    LOG_INFO(F("MQTT: CALLBACK: New Settings.factor0: ") << sett.factor0);
                    
                    sett.setup_time = 0;
                    LOG_INFO(F("MQTT: CALLBACK: reset Settings.setup_time: ") << sett.setup_time);
                }
            }
        } else if (param.equals(F("f1")))
        {
            int f1 = payload.toInt();
            if (f1 > 0)
            {
                if (sett.factor1!= f1)
                {
                    LOG_INFO(F("MQTT: old f1=") << sett.factor1);
                    sett.factor1 = f1;
                    if (json_data["f1"].is<int>())
                    {
                        json_data[F("f1")] = f1;
                        updated = true;
                    }
                    LOG_INFO(F("MQTT: new f1=") << sett.factor1);
                    sett.setup_time = 0;
                }
            }
        } else if (param.equals(F("ch0")))
        {
            float ch0 = payload.toFloat(); // Преобразовали во флоат просто для проверки на условие в следующей строке
            if (ch0 >= 0)
            {
                updated = true;
                LOG_INFO(F("MQTT: ch0 ") << sett.channel0_start << F("->") << ch0);
                sett.channel0_start = ch0;
                sett.impulses0_start = data.impulses0;
                if (json_data["ch0"].is<float>())
                {
                    json_data[F("ch0")] = (int)(ch0 * 1000 + 5) / 1000.0;
                }
                sett.setup_time = 0;
            }
        } else if (param.equals(F("ch1")))
        {
            float ch1 = payload.toFloat();
            if (ch1 >= 0)
            {
                updated = true;
                LOG_INFO(F("MQTT: ch1 ") << sett.channel1_start << F("->") << ch1);
                sett.channel1_start = ch1;
                sett.impulses1_start = data.impulses1;
                if (json_data["ch1"].is<float>())
                {
                    json_data[F("ch1")] = (int)(ch1 * 1000 + 5) / 1000.0;
                }
                sett.setup_time = 0;
            }
        } else if (param.equals(F("cname0")))
        {
            int cname0 = payload.toInt();
            if (sett.counter0_name != cname0)
            {
                LOG_INFO(F("MQTT: cname0 ") << sett.counter0_name << F("->") << cname0);
                sett.counter0_name = cname0;
                if (json_data["cname0"].is<int>())
                {
                    json_data[F("cname0")] = cname0;
                    updated = true;
                }
                if (json_data["data_type0"].is<int>())
                {
                    json_data[F("data_type0")] = (uint8_t)data_type_by_name(cname0);
                    updated = true;
                }
                sett.setup_time = 0;
            }
        } else if (param.equals(F("cname1")))
        {
            int cname1 = payload.toInt();
            if (sett.counter1_name != cname1)
            {
                LOG_INFO(F("MQTT: cname1 ") << sett.counter1_name << F("->") << cname1);
                sett.counter1_name = cname1;
                if (json_data["cname1"].is<int>())
                {
                    json_data[F("cname1")] = cname1;
                    updated = true;
                }
                if (json_data["data_type1"].is<int>())
                {
                    json_data[F("data_type1")] = (uint8_t)data_type_by_name(cname1);
                    updated = true;
                }
                sett.setup_time = 0;
            }
        } else if (param.equals(F("ctype0")))
        {
            int ctype0 = payload.toInt();
            if (mqtt_ctx.ctype0 != ctype0)
            {
                LOG_INFO(F("MQTT: ctype0 ") << mqtt_ctx.ctype0 << F("->") << ctype0);
                if (masterI2C.setCountersType(ctype0, mqtt_ctx.ctype1))
                {
                    mqtt_ctx.ctype0 = ctype0;
                    updated = true;
                    if (json_data["ctype0"].is<int>())
                    {
                        json_data[F("ctype0")] = ctype0;
                    }
                }
                sett.setup_time = 0;
            }
        } else if (param.equals(F("ctype1")))
        {
            int ctype1 = payload.toInt();
            if (mqtt_ctx.ctype1 != ctype1)
            {
                LOG_INFO(F("MQTT: ctype1 ") << mqtt_ctx.ctype1 << F("->") << ctype1);
                if (masterI2C.setCountersType(mqtt_ctx.ctype0, ctype1))
                {
                    mqtt_ctx.ctype1 = ctype1;
                    updated = true;
                    if (json_data["ctype1"].is<int>())
                    {
                        json_data[F("ctype1")] = ctype1;
                    }
                }
                sett.setup_time = 0;
            }
        }
    }
    return updated;
}

/**
 * @brief Обработка пришедшего сообщения по подписке
 *
 * @param sett настройки
 * @param mqtt_client клиент MQTT
 * @param json_data данные JSON
 * @param raw_topic топик
 * @param raw_payload  данные из топика
 * @param length длина сообщения
 */
void mqtt_callback(Settings &sett, const AttinyData &data, JsonDocument &json_data, PubSubClient &mqtt_client, String &mqtt_topic, char *raw_topic, byte *raw_payload, unsigned int length)
{
    String topic = raw_topic;
    String payload;
    String zero_payload("");
    payload.reserve(length);

    LOG_INFO(F("MQTT: CB topic=") << topic << F(" len=") << length);

    // Эффективное копирование payload без посимвольного добавления
    payload.concat((const char*)raw_payload, length);
    
    LOG_INFO(F("MQTT: CB payload=") << payload);
    if (update_settings(topic, payload, sett, data, json_data))
    {
        // если данные изменились то переопубликуем их сразу не ожидая следующего сеанса связи
        publish_data(mqtt_client, mqtt_topic, json_data, true);
    }
    LOG_INFO(F("MQTT: rm retain=") << topic);
    publish(mqtt_client, topic, zero_payload, PUBLISH_MODE_SIMPLE);
}

/**
 * @brief Подключается к серверу MQTT c таймаутом и несколькими попытками
 *
 * @param sett настройки
 * @param mqtt_client клиент MQTT
 * @param mqtt_topic строка с топиком
 * @return true Удалось подключиться,
 * @return false Не удалось подключиться
 */
bool mqtt_connect(Settings &sett, PubSubClient &mqtt_client)
{
    String client_id = get_device_name();
    const char *login = sett.mqtt_login[0] ? sett.mqtt_login : NULL;
    const char *pass = sett.mqtt_password[0] ? sett.mqtt_password : NULL;
    LOG_INFO(F("MQTT: Connecting..."));
    int attempts = MQTT_MAX_TRIES;
    do
    {
        LOG_INFO(F("MQTT: Attempt #") << MQTT_MAX_TRIES - attempts + 1 << F(" from ") << MQTT_MAX_TRIES);
        if (mqtt_client.connect(client_id.c_str(), login, pass))
        {
            LOG_INFO(F("MQTT: Connected."));
            return true;
        }
        LOG_ERROR(F("MQTT: Connect failed with state ") << mqtt_client.state());
        delay(MQTT_CONNECT_DELAY);
    } while (--attempts);
    LOG_ERROR(F("MQTT: All connection attempts failed"));
    return false;
}

/**
 * @brief Подписка на все субтопики устройства
 *
 * @param mqtt_client клиент MQTT
 * @param mqtt_topic строка с топиком
 * @return true удалось подключиться,
 * @return false не удалось подключиться
 */
bool mqtt_subscribe(PubSubClient &mqtt_client, String &mqtt_topic)
{
    String subscribe_topic = mqtt_topic + F(MQTT_SUBSCRIPTION_TOPIC);
    if (!mqtt_client.subscribe(subscribe_topic.c_str(), 1))
    {
        LOG_ERROR(F("MQTT: Failed Subscribe to ") << subscribe_topic);
        return false;
    }

    LOG_INFO(F("MQTT: Subscribed to ") << subscribe_topic);

    return true;
}

/**
 * @brief Отписка от сообщений на все субтопики устройства
 *
 * @param mqtt_client
 * @param mqtt_topic
 * @return true
 * @return false
 */
bool mqtt_unsubscribe(PubSubClient &mqtt_client, String &mqtt_topic)
{
    String subscribe_topic = mqtt_topic + F(MQTT_SUBSCRIPTION_TOPIC);
    if (!mqtt_client.unsubscribe(subscribe_topic.c_str()))
    {
        LOG_ERROR(F("MQTT: Failed Unsubscribe from ") << subscribe_topic);
        return false;
    }

    LOG_INFO(F("MQTT: Unsubscribed from ") << subscribe_topic);

    return true;
}
