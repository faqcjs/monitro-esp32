/*
 * Monitor de actividad para adultos mayores
 * ESP32-C3 SuperMini + sensor PIR + Supabase + ntfy
 * ------------------------------------------------------------------------------
 * - Detecta movimiento con un PIR (HC-SR501) en GPIO 4.
 * - Registra cada evento (arranque, movimiento, alerta, recuperado) en Supabase y
 *   manda un "latido" cada HEARTBEAT_SEC para que la app sepa que el aparato está vivo.
 * - La configuración se edita desde la app web: el aparato la recibe en cada latido
 *   y guarda una copia en su memoria (sobrevive a cortes de luz y a caídas de internet).
 * - Si pasan "umbral" minutos sin movimiento manda una ALERTA push (app ntfy) y la
 *   repite cada "repetir" minutos hasta que vuelva a detectar movimiento.
 * - Horario activo: fuera de esa franja el reloj de inactividad no corre.
 * - Resumen diario por ntfy.
 *
 * Datos privados (WiFi, claves, token): en secrets.h (copiar de secrets.example.h).
 *
 * Conexiones:
 *   PIR VCC -> 5V   PIR GND -> GND   PIR OUT -> GPIO 4 (sale 3,3 V, va directo)
 *   GPIO 5 -> resistencia de 330 ohm -> base del 2N2222A (enciende el LED de la placa)
 *   ESP32-C3 SuperMini: 5V y GND a la placa
 *
 * Arduino IDE: placa "ESP32C3 Dev Module", "USB CDC On Boot: Enabled".
 * No necesita librerías extra (solo el core de ESP32).
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include "secrets.h"

// ======================= CONFIGURACIÓN =======================

const char* NTFY_SERVER = "https://ntfy.sh";  // base URL only; the topic is appended per request

const uint8_t PIR_PIN = 4;
const uint8_t LED_PIN = 5;   // maneja el transistor del LED de la placa

// Valores de fábrica: se usan hasta recibir la configuración de la app.
// Deben coincidir con los valores por defecto de supabase/schema.sql.
const uint32_t DEF_INACTIVITY_MIN   = 30;   // minutos sin movimiento -> alerta
const uint32_t DEF_REPEAT_MIN       = 30;   // repetir alerta cada tanto
const bool     DEF_NOTIFY_EACH      = true; // avisar cada movimiento
const uint32_t DEF_MOTION_GAP_MIN   = 1;    // minutos mínimos entre avisos de movimiento
const bool     DEF_USE_DAY_WINDOW   = false;
const int      DEF_DAY_START_H      = 8;
const int      DEF_DAY_END_H        = 22;

const int      DAILY_SUMMARY_H   = 21;        // hora del resumen diario (-1 = desactivado)
const uint32_t HEARTBEAT_SEC     = 20;        // latido a Supabase (y lectura de configuración)
const uint32_t MOTION_REPORT_SEC = 60;        // como máximo un evento "motion" por minuto
const char*    TZ_ARG            = "<-03>3";  // Argentina (UTC-3)
const uint32_t WARMUP_MS         = 60000;     // el PIR dispara cosas raras el primer minuto
// =============================================================

// Configuración en uso (se carga de la memoria al arrancar y se actualiza desde la app)
uint32_t inactivityMin = DEF_INACTIVITY_MIN;
uint32_t repeatMin     = DEF_REPEAT_MIN;
bool     notifyEach    = DEF_NOTIFY_EACH;
uint32_t motionGapMin  = DEF_MOTION_GAP_MIN;
bool     useDayWindow  = DEF_USE_DAY_WINDOW;
int      dayStartH     = DEF_DAY_START_H;
int      dayEndH       = DEF_DAY_END_H;

Preferences prefs;
volatile bool motionFlag = false;

uint32_t lastMotionMs       = 0;   // referencia del reloj de inactividad (millis)
time_t   lastMotionEpoch    = 0;   // último movimiento real (hora de pared)
uint32_t lastMotionMsgMs    = 0;
uint32_t lastMotionReportMs = 0;
uint32_t lastAlertMs        = 0;
uint32_t lastTryMs          = 0;
uint32_t lastHeartbeatMs    = 0;
bool     alertActive        = false;
bool     bootReported       = false;
uint32_t motionsToday       = 0;
int      counterDay         = -1;
int      summaryDay         = -1;
uint32_t lastWifiOkMs       = 0;
uint32_t lastWifiTryMs      = 0;
bool     wifiEverConnected  = false;

// Resultado del último intento de hablar con Supabase (para el LED)
enum CloudState { CLOUD_UNKNOWN, CLOUD_OK, CLOUD_FAIL };
CloudState cloudState = CLOUD_UNKNOWN;

void IRAM_ATTR onMotion() { motionFlag = true; }

// ----------------------- configuración guardada -----------------------
void loadSettings() {
  prefs.begin("monitor", true);
  inactivityMin = prefs.getUInt("inact", DEF_INACTIVITY_MIN);
  repeatMin     = prefs.getUInt("repeat", DEF_REPEAT_MIN);
  notifyEach    = prefs.getBool("notify", DEF_NOTIFY_EACH);
  motionGapMin  = prefs.getUInt("gap", DEF_MOTION_GAP_MIN);
  useDayWindow  = prefs.getBool("win", DEF_USE_DAY_WINDOW);
  dayStartH     = prefs.getInt("ds", DEF_DAY_START_H);
  dayEndH       = prefs.getInt("de", DEF_DAY_END_H);
  prefs.end();
}

void saveSettings() {
  prefs.begin("monitor", false);
  prefs.putUInt("inact", inactivityMin);
  prefs.putUInt("repeat", repeatMin);
  prefs.putBool("notify", notifyEach);
  prefs.putUInt("gap", motionGapMin);
  prefs.putBool("win", useDayWindow);
  prefs.putInt("ds", dayStartH);
  prefs.putInt("de", dayEndH);
  prefs.end();
}

// ----------------------- utilidades -----------------------
bool timeSynced() { return time(nullptr) > 1700000000; }

String fmtDuration(uint32_t minutes) {
  uint32_t h = minutes / 60, m = minutes % 60;
  if (h == 0) return String(m) + " min";
  return String(h) + " h " + String(m) + " min";
}

String fmtDateTime(time_t t) {
  if (t == 0) return "sin datos";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[20];
  strftime(buf, sizeof(buf), "%d/%m %H:%M", &tmv);
  return String(buf);
}

bool inDayWindow(int hour) {
  if (dayStartH <= dayEndH) return hour >= dayStartH && hour < dayEndH;
  return hour >= dayStartH || hour < dayEndH;  // franja que cruza medianoche
}

// Lectura mínima de JSON plano: {"clave" : valor, ...}. Tolera espacios alrededor de ':'.
int jsonValueStart(const String& body, const char* key) {
  String pat = String("\"") + key + "\"";
  int p = body.indexOf(pat);
  if (p < 0) return -1;
  p += pat.length();
  while (p < (int)body.length() && (body[p] == ' ' || body[p] == ':')) p++;
  return p < (int)body.length() ? p : -1;
}

bool jsonInt(const String& body, const char* key, long& out) {
  int p = jsonValueStart(body, key);
  if (p < 0) return false;
  char c = body[p];
  if (!(c == '-' || (c >= '0' && c <= '9'))) return false;
  out = body.substring(p).toInt();
  return true;
}

bool jsonBool(const String& body, const char* key, bool& out) {
  int p = jsonValueStart(body, key);
  if (p < 0) return false;
  if (body.startsWith("true", p))  { out = true;  return true; }
  if (body.startsWith("false", p)) { out = false; return true; }
  return false;
}

// ----------------------- WiFi -----------------------
void applyTxPower() {
  // Baja el pico de corriente y evita problemas de señal típicos de la SuperMini
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
}

void connectWifi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  applyTxPower();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    digitalWrite(LED_PIN, (millis() / 150) % 2);  // parpadeo rápido: conectando
    delay(50);
  }
  digitalWrite(LED_PIN, LOW);
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = millis();
    wifiEverConnected = true;
  }
}

void maintainWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = now;
    wifiEverConnected = true;
    return;
  }
  if (now - lastWifiTryMs >= 30000UL) {
    lastWifiTryMs = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    applyTxPower();
  }
  // Si lleva 30 min sin red, reiniciar
  if (now - lastWifiOkMs > 30UL * 60UL * 1000UL) ESP.restart();
}

// ----------------------- ntfy: notificaciones push -----------------------
// priority: "min", "low", "default", "high", "urgent"  |  tags: nombre de emoji de ntfy
bool sendNotify(const String& text, const char* title, const char* priority, const char* tags) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();  // sin verificar certificado (simple, uso doméstico)
  HTTPClient http;
  String url = String(NTFY_SERVER) + "/" + NTFY_TOPIC;
  if (!http.begin(client, url)) return false;
  http.setTimeout(10000);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  http.addHeader("Title", title);        // los encabezados van en ASCII, sin tildes
  http.addHeader("Priority", priority);
  http.addHeader("Tags", tags);
  int code = http.POST(text);
  http.end();
  Serial.printf("ntfy -> HTTP %d\n", code);
  return code == 200;
}

String statusText() {
  String s = "";
  if (lastMotionEpoch == 0 || !timeSynced()) {
    s += "Ultimo movimiento: ninguno desde el arranque\n";
  } else {
    uint32_t mins = (uint32_t)((time(nullptr) - lastMotionEpoch) / 60);
    s += "Ultimo movimiento: " + fmtDateTime(lastMotionEpoch) + " (hace " + fmtDuration(mins) + ")\n";
  }
  s += "Movimientos hoy: " + String(motionsToday) + "\n";
  s += "Alerta por inactividad: " + String(alertActive ? "ACTIVA" : "no") + "\n";
  s += "Umbral: " + fmtDuration(inactivityMin) + "\n";
  s += "WiFi: " + String(WiFi.RSSI()) + " dBm\n";
  s += "Encendido hace: " + fmtDuration(millis() / 60000UL);
  return s;
}

// ----------------------- Supabase -----------------------
// Aplica la configuración recibida. Solo escribe en la memoria si algo cambió.
void applyRemoteSettings(const String& body) {
  long inact, rep, gap, ds, de;
  bool notify, win;
  if (!jsonInt(body, "inactivity_min", inact) || !jsonInt(body, "repeat_min", rep) ||
      !jsonInt(body, "motion_gap_min", gap) || !jsonInt(body, "day_start_h", ds) ||
      !jsonInt(body, "day_end_h", de) || !jsonBool(body, "notify_each", notify) ||
      !jsonBool(body, "use_day_window", win)) {
    Serial.println("Supabase: respuesta sin configuracion valida");
    return;
  }
  if (inact < 1 || inact > 10080 || rep < 1 || rep > 1440 || gap < 0 || gap > 1440 ||
      ds < 0 || ds > 23 || de < 0 || de > 23) {
    Serial.println("Supabase: configuracion fuera de rango, se ignora");
    return;
  }
  bool changed = (uint32_t)inact != inactivityMin || (uint32_t)rep != repeatMin ||
                 (uint32_t)gap != motionGapMin || (int)ds != dayStartH || (int)de != dayEndH ||
                 notify != notifyEach || win != useDayWindow;
  if (!changed) return;
  inactivityMin = inact; repeatMin = rep; motionGapMin = gap;
  dayStartH = ds; dayEndH = de; notifyEach = notify; useDayWindow = win;
  saveSettings();
  Serial.println("Supabase: configuracion actualizada");
}

// Latido + evento opcional ("boot", "motion", "alert", "recovered"). Devuelve true si llegó.
bool cloudReport(const char* kind) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/rpc/device_report";
  if (!http.begin(client, url)) { cloudState = CLOUD_FAIL; return false; }
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  // Publishable keys (sb_publishable_...) are not JWTs: they go only in the apikey header.
  http.addHeader("apikey", SUPABASE_ANON_KEY);

  String body = String("{\"p_token\":\"") + DEVICE_TOKEN + "\"" +
                ",\"p_kind\":" + (kind ? String("\"") + kind + "\"" : String("null")) +
                ",\"p_rssi\":" + String(WiFi.RSSI()) +
                ",\"p_uptime_min\":" + String(millis() / 60000UL) +
                ",\"p_alert_active\":" + (alertActive ? "true" : "false") + "}";
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : "";
  http.end();
  Serial.printf("Supabase (%s) -> HTTP %d\n", kind ? kind : "latido", code);
  cloudState = code == 200 ? CLOUD_OK : CLOUD_FAIL;
  if (code != 200) {
    if (resp.length()) Serial.println(resp);
    return false;
  }
  applyRemoteSettings(resp);
  return true;
}

void handleHeartbeat(uint32_t now) {
  if (!bootReported) {
    // Se reintenta en cada latido hasta que el evento de arranque llegue
    if (lastHeartbeatMs != 0 && now - lastHeartbeatMs < HEARTBEAT_SEC * 1000UL) return;
    lastHeartbeatMs = now;
    bootReported = cloudReport("boot");
    return;
  }
  if (now - lastHeartbeatMs < HEARTBEAT_SEC * 1000UL) return;
  lastHeartbeatMs = now;
  cloudReport(nullptr);
}

// ----------------------- LED de estado -----------------------
// Conectando (nunca hubo WiFi): parpadeo rápido | WiFi perdido: parpadeo lento
// WiFi ok pero Supabase falla: doble destello cada 3 s | Todo ok: sigue al PIR
void updateLed(uint32_t now, int pir) {
  bool on;
  if (WiFi.status() != WL_CONNECTED) {
    on = wifiEverConnected ? (now / 1000) % 2 : (now / 150) % 2;
  } else if (cloudState == CLOUD_FAIL) {
    uint32_t t = now % 3000;
    on = t < 150 || (t >= 300 && t < 450);
  } else {
    on = pir == HIGH;
  }
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// ----------------------- lógica principal -----------------------
void handleMotion(uint32_t now) {
  if (!motionFlag) return;
  motionFlag = false;
  if (now < WARMUP_MS) return;  // ignorar el calentamiento del PIR

  uint32_t silentMin = (now - lastMotionMs) / 60000UL;
  lastMotionMs = now;
  lastMotionEpoch = timeSynced() ? time(nullptr) : 0;

  // Mientras el PIR siga en alto esto se repite: se cuenta un evento nuevo
  // solo si pasaron 10 s desde el anterior.
  static uint32_t lastEventMs = 0;
  if (lastEventMs == 0 || now - lastEventMs >= 10000UL) {
    lastEventMs = now;
    motionsToday++;
    Serial.println("Movimiento detectado");
  }

  if (alertActive) {
    alertActive = false;
    sendNotify("Se volvio a detectar movimiento (" + fmtDateTime(lastMotionEpoch) +
               ") despues de " + fmtDuration(silentMin) + " sin actividad.",
               "Movimiento recuperado", "high", "white_check_mark");
    lastMotionMsgMs = now;
    cloudReport("recovered");
    lastMotionReportMs = now;
    return;
  }

  if (lastMotionReportMs == 0 || now - lastMotionReportMs >= MOTION_REPORT_SEC * 1000UL) {
    lastMotionReportMs = now;  // also on failure, so a down server does not block the loop on every pass
    cloudReport("motion");
  }

  if (notifyEach && (lastMotionMsgMs == 0 || now - lastMotionMsgMs >= motionGapMin * 60000UL)) {
    lastMotionMsgMs = now;
    sendNotify("Movimiento detectado" + String(lastMotionEpoch ? " (" + fmtDateTime(lastMotionEpoch) + ")" : ""),
               "Movimiento", "default", "walking");
  }
}

void handleInactivity(uint32_t now) {
  static uint32_t lastCheck = 0;
  if (now - lastCheck < 1000) return;
  lastCheck = now;

  struct tm t;
  bool synced = getLocalTime(&t, 0);

  // Fuera del horario activo el reloj de inactividad no corre: arranca de cero a la mañana
  if (useDayWindow && synced && !inDayWindow(t.tm_hour)) {
    lastMotionMs = now;
    alertActive = false;
    return;
  }

  uint32_t silentMin = (now - lastMotionMs) / 60000UL;
  if (silentMin < inactivityMin) return;

  bool due = !alertActive || (now - lastAlertMs >= repeatMin * 60000UL);
  if (!due) return;
  if (lastTryMs != 0 && now - lastTryMs < 30000UL) return;  // reintento si falló el envío
  lastTryMs = now;

  String msg = "No se detecta movimiento hace " + fmtDuration(silentMin) +
               (useDayWindow ? " (en horario activo)" : "") + ".\n" +
               "Ultimo movimiento registrado: " + fmtDateTime(lastMotionEpoch) + "\n" +
               "Conviene llamar o pasar a verificar que este todo bien.";
  if (sendNotify(msg, "ALERTA: sin movimiento", "urgent", "rotating_light")) {
    alertActive = true;
    lastAlertMs = now;
    cloudReport("alert");
  }
}

void handleDaily(uint32_t now) {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  if (t.tm_yday != counterDay) {  // día nuevo
    counterDay = t.tm_yday;
    motionsToday = 0;
  }
  if (DAILY_SUMMARY_H < 0 || t.tm_hour < DAILY_SUMMARY_H || summaryDay == t.tm_yday) return;
  static uint32_t lastTry = 0;
  if (lastTry != 0 && now - lastTry < 30000UL) return;
  lastTry = now;
  if (sendNotify(statusText(), "Resumen del dia", "low", "clipboard")) summaryDay = t.tm_yday;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT_PULLDOWN);  // evita falsos disparos si se suelta el cable
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onMotion, RISING);

  loadSettings();
  connectWifi(20000);
  configTzTime(TZ_ARG, "pool.ntp.org", "time.google.com");

  lastMotionMs = millis();
  if (WiFi.status() == WL_CONNECTED) {
    sendNotify("Umbral de inactividad: " + fmtDuration(inactivityMin) +
               ". El sensor tarda ~1 min en calentar. La configuracion se cambia desde la app web.",
               "Monitor iniciado", "default", "electric_plug");
  }
}

void loop() {
  uint32_t now = millis();
  maintainWifi(now);

  int pir = digitalRead(PIR_PIN);
  updateLed(now, pir);
  if (pir == HIGH) motionFlag = true;  // mientras haya señal, cuenta como movimiento

  handleMotion(now);
  handleInactivity(now);
  handleDaily(now);
  handleHeartbeat(now);
  delay(50);
}
