#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <vector>

const char *WIFI_SSID = "ASUS_A8_2G";
const char *WIFI_PASSWORD = "purple_6667";

const char *API_BASE_URL = "https://apiobi-1.onrender.com";
const char *API_USERNAME = "admin";
const char *API_PASSWORD = "123456";
const char *API_KEY = "";

#ifndef DEVICE_ID
#define DEVICE_ID "esp32-01"
#endif

#ifndef PIN_LED_RIEGO
#define PIN_LED_RIEGO 26
#endif

#ifndef PIN_LED_VENTILADOR
#define PIN_LED_VENTILADOR 27
#endif

const unsigned long INTERVALO_SENSORES_MS = 5000;
const unsigned long INTERVALO_COMANDOS_MS = 2500;

const char *ENDPOINT_REGISTER = "/auth/register";
const char *ENDPOINT_LOGIN = "/auth/token";
const char *ENDPOINT_DATOS = "/datos";
const char *ENDPOINT_CONTROL = "/control";
const char *ENDPOINT_PENDIENTES = "/comandos/pendientes";
const char *ENDPOINT_ESTADO = "/estado";
const char *ENDPOINT_HISTORIAL = "/historial";

class Actuador {
public:
  Actuador(const char *nombre, int pin)
      : _nombre(nombre), _pin(pin), _estado(false) {}

  void inicializar() {
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
  }

  void activar() {
    _estado = true;
    digitalWrite(_pin, HIGH);
    Serial.printf("[Actuador] %s -> ON\n", _nombre);
  }

  void desactivar() {
    _estado = false;
    digitalWrite(_pin, LOW);
    Serial.printf("[Actuador] %s -> OFF\n", _nombre);
  }

  void aplicarComando(const String &comando) {
    if (comando == "ON") {
      activar();
    } else if (comando == "OFF") {
      desactivar();
    } else if (comando.toInt() > 0) {
      activar();
    } else {
      desactivar();
    }
  }

  const char *nombre() const { return _nombre; }
  const char *estadoStr() const { return _estado ? "ON" : "OFF"; }
  bool estado() const { return _estado; }

private:
  const char *_nombre;
  int _pin;
  bool _estado;
};

class SensorAmbiental {
public:
  SensorAmbiental() {}

  float temperatura() { return 20.0f + (float)(random(0, 2000)) / 100.0f; }

  float humedad() { return 30.0f + (float)(random(0, 6000)) / 100.0f; }

  int co2() { return random(300, 901); }
};

class APIClient {
public:
  APIClient(Actuador &riego, Actuador &ventilador)
      : _riego(riego), _ventilador(ventilador) {
    _wifiClient.setInsecure();
  }

  bool asegurarSesion() {
    if (login()) {
      return true;
    }

    Serial.println("[API] Login fallo. Intentando registrar usuario...");
    if (!registrarUsuario()) {
      Serial.println("[API] No se pudo registrar usuario.");
      return false;
    }

    return login();
  }

  bool enviarDatos(float temp, float hum, int co2) {
    if (_token.length() == 0) {
      return false;
    }

    HTTPClient http;
    if (!http.begin(_wifiClient, construirUrl(ENDPOINT_DATOS))) {
      Serial.println("[API] Error iniciando conexion para /datos");
      return false;
    }

    prepararHeadersProtegidos(http);

    StaticJsonDocument<256> doc;
    doc["temp"] = temp;
    doc["hum"] = hum;
    doc["co2"] = co2;
    doc["dispositivo"] = DEVICE_ID;
    doc["riego"] = _riego.estadoStr();
    doc["ventilador"] = _ventilador.estadoStr();

    String payload;
    serializeJson(doc, payload);
    int code = http.POST(payload);

    if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED || code == HTTP_CODE_ACCEPTED) {
      Serial.printf("[API] /datos OK (%d): %s\n", code, payload.c_str());
      http.end();
      return true;
    }

    if (code == HTTP_CODE_UNAUTHORIZED) {
      Serial.println("[API] /datos no autorizado. Renovando sesion...");
      _token = "";
      _cookie = "";
    } else {
      Serial.printf("[API] Error /datos (%d): %s\n", code, http.getString().c_str());
    }
    http.end();
    return false;
  }

  bool enviarControl(const char *actuador, const char *estado) {
    if (_token.length() == 0) {
      return false;
    }

    HTTPClient http;
    if (!http.begin(_wifiClient, construirUrl(ENDPOINT_CONTROL))) {
      Serial.println("[API] Error iniciando conexion para /control");
      return false;
    }

    prepararHeadersProtegidos(http);

    StaticJsonDocument<128> doc;
    doc["actuador"] = actuador;
    doc["accion"] = estado;

    String payload;
    serializeJson(doc, payload);
    int code = http.POST(payload);

    bool ok = (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED || code == HTTP_CODE_ACCEPTED);
    if (!ok && code == HTTP_CODE_UNAUTHORIZED) {
      _token = "";
      _cookie = "";
    }

    Serial.printf("[API] /control (%d): %s\n", code, payload.c_str());
    http.end();
    return ok;
  }

  bool procesarComandosPendientes() {
    if (_token.length() == 0) {
      return false;
    }

    HTTPClient http;
    if (!http.begin(_wifiClient, construirUrl(ENDPOINT_PENDIENTES))) {
      Serial.println("[API] Error iniciando conexion para /comandos/pendientes");
      return false;
    }

    prepararHeadersProtegidos(http);
    int code = http.GET();
    String body = http.getString();
    http.end();

    if (code == HTTP_CODE_NO_CONTENT) {
      return true;
    }

    if (code == HTTP_CODE_UNAUTHORIZED) {
      Serial.println("[API] Pendientes no autorizado. Renovando sesion...");
      _token = "";
      _cookie = "";
      return false;
    }

    if (code != HTTP_CODE_OK) {
      Serial.printf("[API] Error /comandos/pendientes (%d): %s\n", code, body.c_str());
      return false;
    }

    if (body.length() == 0) {
      return true;
    }

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      Serial.printf("[API] JSON invalido en pendientes: %s\n", err.c_str());
      return false;
    }

    bool aplicado = false;

    if (doc.is<JsonArray>()) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonVariant item : arr) {
        aplicado |= aplicarDesdeJson(item);
      }
    } else if (doc.is<JsonObject>()) {
      JsonObject obj = doc.as<JsonObject>();
      if (obj.containsKey("comandos") && obj["comandos"].is<JsonArray>()) {
        JsonArray arr = obj["comandos"].as<JsonArray>();
        for (JsonVariant item : arr) {
          aplicado |= aplicarDesdeJson(item);
        }
      } else {
        aplicado |= aplicarDesdeJson(obj);
      }
    }

    return aplicado || true;
  }

private:
  String construirUrl(const char *path) {
    return String(API_BASE_URL) + String(path);
  }

  void agregarHeaderApiKey(HTTPClient &http) {
    if (strlen(API_KEY) > 0) {
      http.addHeader("X-API-Key", API_KEY);
    }
  }

  void prepararHeadersProtegidos(HTTPClient &http) {
    http.addHeader("Content-Type", "application/json");
    agregarHeaderApiKey(http);
    if (_token.length() > 0) {
      http.addHeader("Authorization", String("Bearer ") + _token);
    }
    if (_cookie.length() > 0) {
      http.addHeader("Cookie", _cookie);
    }
  }

  bool registrarUsuario() {
    HTTPClient http;
    if (!http.begin(_wifiClient, construirUrl(ENDPOINT_REGISTER))) {
      return false;
    }

    http.addHeader("Content-Type", "application/json");
    agregarHeaderApiKey(http);

    StaticJsonDocument<128> doc;
    doc["username"] = API_USERNAME;
    doc["password"] = API_PASSWORD;

    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    String body = http.getString();
    http.end();

    if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED || code == 400 || code == 409) {
      Serial.printf("[API] Registro respuesta (%d): %s\n", code, body.c_str());
      return true;
    }

    Serial.printf("[API] Error registrando (%d): %s\n", code, body.c_str());
    return false;
  }

  bool login() {
    HTTPClient http;
    if (!http.begin(_wifiClient, construirUrl(ENDPOINT_LOGIN))) {
      return false;
    }

    const char *headers[] = {"Set-Cookie"};
    http.collectHeaders(headers, 1);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    agregarHeaderApiKey(http);

    String form = String("username=") + API_USERNAME + "&password=" + API_PASSWORD;
    int code = http.POST(form);
    String body = http.getString();

    if (code != HTTP_CODE_OK) {
      Serial.printf("[API] Login fallo (%d): %s\n", code, body.c_str());
      http.end();
      return false;
    }

    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc.containsKey("access_token")) {
      Serial.printf("[API] Login sin token valido: %s\n", body.c_str());
      http.end();
      return false;
    }

    _token = String((const char *)doc["access_token"]);
    _cookie = http.header("Set-Cookie");
    http.end();

    Serial.println("[API] Sesion iniciada correctamente.");
    return true;
  }

  bool aplicarDesdeJson(JsonVariant v) {
    String actuador;
    String comando;

    if (v.is<JsonObject>()) {
      JsonObject o = v.as<JsonObject>();
      if (o.containsKey("actuador") && o.containsKey("accion")) {
        actuador = String((const char *)o["actuador"]);
        comando = String((const char *)o["accion"]);
      } else {
        if (o.containsKey("riego")) {
          _riego.aplicarComando(String((const char *)o["riego"]));
          enviarControl(_riego.nombre(), _riego.estadoStr());
          return true;
        }
        if (o.containsKey("ventilador")) {
          _ventilador.aplicarComando(String((const char *)o["ventilador"]));
          enviarControl(_ventilador.nombre(), _ventilador.estadoStr());
          return true;
        }
      }
    }

    if (actuador.length() == 0) {
      return false;
    }

    if (actuador == "riego") {
      _riego.aplicarComando(comando);
      enviarControl(_riego.nombre(), _riego.estadoStr());
      return true;
    }

    if (actuador == "ventilador") {
      _ventilador.aplicarComando(comando);
      enviarControl(_ventilador.nombre(), _ventilador.estadoStr());
      return true;
    }

    return false;
  }

  WiFiClientSecure _wifiClient;
  Actuador &_riego;
  Actuador &_ventilador;
  String _token;
  String _cookie;
};

Actuador riego("riego", PIN_LED_RIEGO);
Actuador ventilador("ventilador", PIN_LED_VENTILADOR);
APIClient apiClient(riego, ventilador);
SensorAmbiental sensor;

unsigned long ultimaPublicacion = 0;
unsigned long ultimaRevisionComandos = 0;

void conectarWiFi() {
  Serial.printf("\n[WiFi] Conectando a '%s'", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - inicio > 15000) {
      Serial.println("\n[WiFi] Timeout. Reiniciando...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\n[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

void asegurarSesionAPI() {
  while (!apiClient.asegurarSesion()) {
    Serial.println("[API] Reintentando sesion en 3 segundos...");
    delay(3000);
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed((uint32_t)micros());

  riego.inicializar();
  ventilador.inicializar();

  conectarWiFi();
  asegurarSesionAPI();
}

void loop() {
  unsigned long ahora = millis();

  if (ahora - ultimaRevisionComandos >= INTERVALO_COMANDOS_MS) {
    ultimaRevisionComandos = ahora;
    if (!apiClient.procesarComandosPendientes()) {
      asegurarSesionAPI();
    }
  }

  if (ahora - ultimaPublicacion >= INTERVALO_SENSORES_MS) {
    ultimaPublicacion = ahora;

    float temp = sensor.temperatura();
    float hum = sensor.humedad();
    int co2 = sensor.co2();

    if (!apiClient.enviarDatos(temp, hum, co2)) {
      asegurarSesionAPI();
    }
  }

  delay(25);
}
