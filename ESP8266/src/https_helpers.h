#ifndef HTTP_HELPERS_H_
#define HTTP_HELPERS_H_

#include <Arduino.h>

class HTTPClient;

extern bool post_data(const String &url, const char *key, const char *email, const String &payload, String *response = nullptr);

/**
 * @brief Проверка и получение валидного ответа от сервера
 * 
 * Проверяет наличие Content-Length заголовка и соответствие размера ответа лимиту
 * перед загрузкой данных в память. Защищает от переполнения памяти ESP8266.
 * 
 * @param httpClient HTTP клиент с активным соединением
 * @param response_code Код ответа от сервера
 * @param out_response Выходной параметр для тела ответа (заполняется при успехе)
 * @param log_prefix Префикс для сообщений лога (например "HTTP:" или "RCFG:")
 * @return true если ответ валидный и загружен в out_response
 * @return false если проверка не прошла (нет Content-Length или превышен лимит)
 */
extern bool validate_and_get_response(HTTPClient &httpClient, int response_code, String &out_response, const char* log_prefix = "HTTP");

#endif