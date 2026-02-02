#include <ESP8266WiFi.h>
#include "ESP8266HTTPClient.h"
#include "Logging.h"
#include "utils.h"
#include "setup.h"
#include "https_helpers.h"

/**
 * @brief Проверка и получение валидного ответа от сервера
 * 
 * Выполняет трехуровневую проверку безопасности:
 * 1. Проверка кода ответа (должен быть 200)
 * 2. Проверка наличия Content-Length заголовка
 * 3. Проверка размера ответа (не более REMOTE_CONFIG_MAX_SIZE)
 * 
 * Только если все проверки пройдены - загружает данные в память.
 */
bool validate_and_get_response(HTTPClient &httpClient, int response_code, String &out_response, const char* log_prefix)
{
    // Проверка 1: Код ответа должен быть 200
    if (response_code != 200)
    {
        LOG_INFO(F("") << log_prefix << F(": Response code is not 200: ") << response_code);
        out_response = "";
        return false;
    }
    
    // Проверка 2: Получаем размер ответа из HTTP заголовка Content-Length ДО загрузки данных
    int content_length = httpClient.getSize();
    LOG_INFO(F("") << log_prefix << F(": Content-Length from server: ") << content_length);
    
    // Проверяем наличие Content-Length для безопасности
    if (content_length <= 0)
    {
        LOG_ERROR(F("") << log_prefix << F(": Server did not provide Content-Length header"));
        LOG_ERROR(F("") << log_prefix << F(": Rejecting response for security (unknown size could cause memory overflow)"));
        out_response = "";
        return false;
    }
    
    // Проверка 3: Проверяем размер ответа ДО загрузки в память
    // Приведение к unsigned long безопасно, т.к. выше проверили что content_length > 0
    if ((unsigned long)content_length > REMOTE_CONFIG_MAX_SIZE)
    {
        LOG_ERROR(F("") << log_prefix << F(": Response too large: ") << content_length << F(" bytes (max: ") << REMOTE_CONFIG_MAX_SIZE << F(" bytes)"));
        LOG_ERROR(F("") << log_prefix << F(": Rejecting response to prevent memory overflow"));
        out_response = "";
        return false;
    }
    
    // Все проверки пройдены - безопасно загружаем данные
    out_response = httpClient.getString();
    LOG_INFO(F("") << log_prefix << F(": Response body: ") << out_response);
    LOG_INFO(F("") << log_prefix << F(": Actual response size: ") << out_response.length() << F(" bytes"));
    
    return true;
}

bool post_data(const String &url, const char *key, const char *email, const String &payload, String *response)
{
    void *pClient = nullptr;
    HTTPClient httpClient;
    bool result = false;
    LOG_INFO(F("HTTP: Send JSON POST request"));
    LOG_INFO(F("HTTP: URL:") << url);
    LOG_INFO(F("HTTP: Body:") << payload);

    String proto = get_proto(url);
    LOG_INFO(F("HTTP: Protocol: ") << proto);

    // Set wc client
    if (proto == PROTO_HTTP)
    {
        LOG_INFO(F("HTTP: Create insecure client"));
        pClient = new WiFiClient;
    }
    else if (proto == PROTO_HTTPS)
    {
        LOG_INFO(F("HTTP: Create secure client"));
        pClient = new WiFiClientSecure;
        (*(WiFiClientSecure *)pClient).setInsecure(); // доверяем всем сертификатам
    }

    // HTTP settings
    httpClient.setTimeout(SERVER_TIMEOUT);
    httpClient.setReuse(false); // будет сразу закрывать подключение после отправки

    if (httpClient.begin(*(WiFiClient *)pClient, url))
    {
        httpClient.addHeader(F("Content-Type"), F("application/json"));
        if (key)
        {
            httpClient.addHeader(F("Waterius-Token"), key);
        }
        if (email)
        {
            httpClient.addHeader(F("Waterius-Email"), email);
        }
        LOG_INFO(F("HTTP: Post request"));

        int response_code = httpClient.POST(payload);
        LOG_INFO(F("HTTP: Response code: ") << response_code);
        result = response_code == 200;
        
        // Если ожидается ответ с возможной конфигурацией - используем валидацию
        if (response != nullptr)
        {
            validate_and_get_response(httpClient, response_code, *response, "HTTP");
        }
        else
        {
            // Ответ не ожидается - просто логируем
            String response_body = httpClient.getString();
            LOG_INFO(F("HTTP: Response body: ") << response_body);
        }
        
        httpClient.end();
        (*(WiFiClient *)pClient).stop();
    }

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
