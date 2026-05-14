#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <vector>

const char *WIFI_SSID = "Alumno";
const char *WIFI_PASSWORD = "Mebe2ege";

const char *API_BASE_URL = "https://apiobi-1.onrender.com";
const char *API_USERNAME = "admin";
const char *API_PASSWORD = "123456";
const char *API_KEY = "";

const char *MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char *MQTT_USERNAME = "";
const char *MQTT_PASSWORD = "";

#ifndef DEVICE_ID
#define DEVICE_ID "esp32-01"
#endif

#ifndef PIN_LED_RIEGO
#define PIN_LED_RIEGO 26
#endif

#ifndef PIN_LED_VENTILADOR
#define PIN_LED_VENTILADOR 27
#endif

#ifndef PIN_SENSOR_TEMP
#ifdef A0
#define PIN_SENSOR_TEMP A0
#else
#define PIN_SENSOR_TEMP 0
#endif
#endif

#ifndef PIN_SENSOR_HUM
#ifdef A1
#define PIN_SENSOR_HUM A1
#else
#define PIN_SENSOR_HUM 1
#endif
#endif

#ifndef PIN_SENSOR_CO2
#ifdef A2
#define PIN_SENSOR_CO2 A2
#else
#define PIN_SENSOR_CO2 2
#endif
#endif

const unsigned long INTERVALO_SENSORES_MS = 5000;
const unsigned long INTERVALO_COMANDOS_MS = 2500;
const unsigned long INTERVALO_REINTENTO_MQTT_MS = 5000;
const unsigned long INTERVALO_REINTENTO_WIFI_MS = 15000;

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
  SensorAmbiental(int pinTemp, int pinHum, int pinCo2)
      : _pinTemp(pinTemp), _pinHum(pinHum), _pinCo2(pinCo2) {}

  void inicializar() {
    pinMode(_pinTemp, INPUT);
    pinMode(_pinHum, INPUT);
    pinMode(_pinCo2, INPUT);
    analogReadResolution(12);

    Serial.printf("[Sensores] Temp=A0(%d), Hum=A1(%d), CO2=A2(%d)\n", _pinTemp,
                  _pinHum, _pinCo2);
  }

  float temperatura() {
    int mv = leerMv(_pinTemp);
    return mapearFloat((float)mv, 0.0f, 3300.0f, -10.0f, 60.0f);
  }

  float humedad() {
    int mv = leerMv(_pinHum);
    return mapearFloat((float)mv, 0.0f, 3300.0f, 0.0f, 100.0f);
  }

  int co2() {
    int mv = leerMv(_pinCo2);
    return (int)mapearFloat((float)mv, 0.0f, 3300.0f, 300.0f, 2000.0f);
  }

  void imprimirLecturas() {
    Serial.printf("[Sensores] mV -> T:%d H:%d C:%d\n", leerMv(_pinTemp),
                  leerMv(_pinHum), leerMv(_pinCo2));
  }

private:
  int leerMv(int pin) {
    int mv = analogReadMilliVolts(pin);
    if (mv <= 0) {
      int raw = analogRead(pin);
      mv = (int)((raw * 3300.0f) / 4095.0f);
    }
    return constrain(mv, 0, 3300);
  }

  float mapearFloat(float x, float inMin, float inMax, float outMin,
                    float outMax) {
    if (x < inMin) {
      x = inMin;
    }
    if (x > inMax) {
      x = inMax;
    }

    return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
  }

  int _pinTemp;
  int _pinHum;
  int _pinCo2;
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
SensorAmbiental sensor(PIN_SENSOR_TEMP, PIN_SENSOR_HUM, PIN_SENSOR_CO2);
WiFiClient wifiClientMQTT;
PubSubClient mqttClient(wifiClientMQTT);

String topicDatos;
String topicEstado;
String topicCmdRiego;
String topicCmdVentilador;

unsigned long ultimaPublicacion = 0;
unsigned long ultimaRevisionComandos = 0;
unsigned long ultimoIntentoMQTT = 0;
unsigned long ultimoIntentoWiFi = 0;

bool conectarWiFi() {
  ultimoIntentoWiFi = millis();
  Serial.printf("\n[WiFi] Conectando a '%s'", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - inicio > 15000) {
      Serial.println("\n[WiFi] Timeout. Seguire sin red por ahora.");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\n[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void mantenerWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - ultimoIntentoWiFi < INTERVALO_REINTENTO_WIFI_MS) {
    return;
  }

  conectarWiFi();
}

void asegurarSesionAPI() {
  while (!apiClient.asegurarSesion()) {
    Serial.println("[API] Reintentando sesion en 3 segundos...");
    delay(3000);
  }
}

void configurarTopicosMQTT() {
  String base = String("obi/") + DEVICE_ID;
  topicDatos = base + "/datos";
  topicEstado = base + "/estado";
  topicCmdRiego = base + "/cmd/riego";
  topicCmdVentilador = base + "/cmd/ventilador";
}

void publicarEstadoMQTT() {
  if (!mqttClient.connected()) {
    return;
  }

  StaticJsonDocument<128> doc;
  doc["dispositivo"] = DEVICE_ID;
  doc["riego"] = riego.estadoStr();
  doc["ventilador"] = ventilador.estadoStr();

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(topicEstado.c_str(), payload.c_str(), true);
}

void callbackMQTT(char *topic, byte *payload, unsigned int length) {
  String mensaje;
  mensaje.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }

  String topicRx = String(topic);
  mensaje.trim();

  if (topicRx == topicCmdRiego) {
    riego.aplicarComando(mensaje);
    apiClient.enviarControl(riego.nombre(), riego.estadoStr());
    publicarEstadoMQTT();
    return;
  }

  if (topicRx == topicCmdVentilador) {
    ventilador.aplicarComando(mensaje);
    apiClient.enviarControl(ventilador.nombre(), ventilador.estadoStr());
    publicarEstadoMQTT();
    return;
  }
}

void conectarMQTT() {
  if (mqttClient.connected()) {
    return;
  }

  if (millis() - ultimoIntentoMQTT < INTERVALO_REINTENTO_MQTT_MS) {
    return;
  }
  ultimoIntentoMQTT = millis();

  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = false;

  if (strlen(MQTT_USERNAME) > 0) {
    ok = mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD);
  } else {
    ok = mqttClient.connect(clientId.c_str());
  }

  if (!ok) {
    Serial.printf("[MQTT] Error conectando (%d)\n", mqttClient.state());
    return;
  }

  mqttClient.subscribe(topicCmdRiego.c_str());
  mqttClient.subscribe(topicCmdVentilador.c_str());
  Serial.printf("[MQTT] Conectado. Subs: %s, %s\n", topicCmdRiego.c_str(),
                topicCmdVentilador.c_str());
  publicarEstadoMQTT();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("[System] Arrancando firmware...");

  riego.inicializar();
  ventilador.inicializar();
  sensor.inicializar();

  conectarWiFi();
  if (!apiClient.asegurarSesion()) {
    Serial.println("[API] Sesion no disponible. El firmware seguira mostrando datos por serial.");
  }

  configurarTopicosMQTT();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(callbackMQTT);
  conectarMQTT();

  Serial.println("[System] Firmware iniciado correctamente.");
}

void loop() {
  unsigned long ahora = millis();

  mantenerWiFi();
  conectarMQTT();
  mqttClient.loop();

  if (ahora - ultimaRevisionComandos >= INTERVALO_COMANDOS_MS) {
    ultimaRevisionComandos = ahora;
    if (!apiClient.procesarComandosPendientes()) {
      Serial.println("[API] No se pudieron procesar comandos pendientes en este ciclo.");
    }
  }

  if (ahora - ultimaPublicacion >= INTERVALO_SENSORES_MS) {
    ultimaPublicacion = ahora;
    float temp = sensor.temperatura();
    float hum = sensor.humedad();
    int co2 = sensor.co2();

    Serial.printf("[Datos] Temp=%.2f C | Hum=%.2f %% | CO2=%d ppm | Riego=%s | Ventilador=%s | MQTT=%s\n",
                  temp, hum, co2, riego.estadoStr(), ventilador.estadoStr(),
                  mqttClient.connected() ? "ON" : "OFF");

    if (!apiClient.enviarDatos(temp, hum, co2)) {
      Serial.println("[API] No se pudo enviar /datos en este ciclo.");
    }

    if (mqttClient.connected()) {
      StaticJsonDocument<256> doc;
      doc["dispositivo"] = DEVICE_ID;
      doc["temp"] = temp;
      doc["hum"] = hum;
      doc["co2"] = co2;
      doc["riego"] = riego.estadoStr();
      doc["ventilador"] = ventilador.estadoStr();

      String payload;
      serializeJson(doc, payload);
      mqttClient.publish(topicDatos.c_str(), payload.c_str(), false);
    }
  }

  delay(25);
}
