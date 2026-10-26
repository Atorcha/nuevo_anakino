/*
 * Anakino Aquarium Controller v3.0 - Mono-fichero
 * Autor: Adaptado para Atorcha
 * 
 * Características:
 * - Control de 4 Relés (Luz, Aireador, Calentador, Extra)
 * - Sensor de temperatura DS18B20 con histeresis para el calentador
 * - Panel Web seguro con Login (JSON API)
 * - OTA (Actualización por WiFi)
 * - Guardado de configuración en NVS (Flash)
 * - Temporizadores automáticos (sin bloquear el bucle)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>

// ==========================================
// CONFIGURACIÓN DE PINES Y PARAMETROS
// ==========================================

#define FW_VERSION "3.0.0"

// Pines de los relés (Ajusta según tu placa de expansión)
#define PIN_RELAY_LUZ       25
#define PIN_RELAY_AIREADOR  26
#define PIN_RELAY_CALENT    27
#define PIN_RELAY_EXTRA     14

// Config del relé: true si se activa con LOW (común en módulos chinos)
#define RELAY_ACTIVE_LOW    true 

// Pin del sensor DS18B20
#define PIN_DS18B20         4

// Defaults
#define DEFAULT_USER        "Anakin"
#define DEFAULT_PASS        "1234"
#define DEFAULT_TEMP_TARGET 25.0f
#define DEFAULT_TEMP_HYST   0.5f
#define NVS_NAMESPACE       "anakino"

// ==========================================
// ESTRUCTURAS Y CLASES
// ==========================================

enum RelayId { R_LUZ = 0, R_AIREADOR, R_CALENT, R_EXTRA, R_COUNT };
enum RelayMode { MODE_OFF, MODE_ON, MODE_AUTO };

struct Params {
  char ssid[33], pass[64];
  char user[17], passHash[65]; // Guardamos el hash SHA256, nunca la contraseña en claro
  float tempTarget, tempHyst;
  bool  heaterEnabled;
};

// --- GESTOR DE CONFIGURACIÓN (NVS) ---
class ConfigStore {
public:
  Params p;
  
  void load() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    strlcpy(p.ssid, prefs.getString("ssid", "").c_str(), 33);
    strlcpy(p.pass, prefs.getString("pass", "").c_str(), 64);
    strlcpy(p.user, prefs.getString("user", DEFAULT_USER).c_str(), 17);
    
    String ph = prefs.getString("ph", "");
    if (ph.isEmpty()) { sha256hex(DEFAULT_PASS, p.passHash); }
    else strlcpy(p.passHash, ph.c_str(), 65);
    
    p.tempTarget   = prefs.getFloat("tt", DEFAULT_TEMP_TARGET);
    p.tempHyst     = prefs.getFloat("th", DEFAULT_TEMP_HYST);
    p.heaterEnabled= prefs.getBool("he", true);
    prefs.end();
  }

  void save() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("ssid", p.ssid);
    prefs.putString("pass", p.pass);
    prefs.putString("user", p.user);
    prefs.putString("ph",   p.passHash);
    prefs.putFloat("tt", p.tempTarget);
    prefs.putFloat("th", p.tempHyst);
    prefs.putBool ("he", p.heaterEnabled);
    prefs.end();
  }

  bool checkAuth(const String& u, const String& pw) {
    char h[65]; sha256hex(pw, h);
    return u == p.user && strcmp(h, p.passHash) == 0;
  }

  void setCredentials(const String& u, const String& pw) {
    strlcpy(p.user, u.c_str(), 17);
    sha256hex(pw, p.passHash);
    save();
  }

private:
  static void sha256hex(const String& in, char* out) {
    uint8_t h[32];
    mbedtls_sha256((const uint8_t*)in.c_str(), in.length(), h, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i*2, "%02x", h[i]);
    out[64] = 0;
  }
};

// --- GESTOR DE RELES ---
class RelayManager {
public:
  void begin() {
    for (int i = 0; i < R_COUNT; i++) {
      pinMode(_pins[i], OUTPUT);
      write((RelayId)i, false);
    }
  }

  void set(RelayId id, bool on) { 
    _mode[id] = on ? MODE_ON : MODE_OFF; 
    write(id, on); 
  }
  
  void setMode(RelayId id, RelayMode m) { _mode[id] = m; }
  bool state(RelayId id)      { return _on[id]; }
  RelayMode mode(RelayId id)  { return _mode[id]; }
  const char* name(RelayId id) { return NAMES[id]; }

private:
  const int _pins[R_COUNT] = {PIN_RELAY_LUZ, PIN_RELAY_AIREADOR, PIN_RELAY_CALENT, PIN_RELAY_EXTRA};
  bool _on[R_COUNT] = {};
  RelayMode _mode[R_COUNT] = {MODE_ON, MODE_ON, MODE_AUTO, MODE_OFF};
  static const char* NAMES[R_COUNT];
  
  void write(RelayId id, bool on) {
    _on[id] = on;
    digitalWrite(_pins[id], (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
  }
};
const char* RelayManager::NAMES[R_COUNT] = {"Luz", "Aireador", "Calentador", "Extra"};

// --- GESTOR DE TEMPORIZADORES ---
struct TimeSlot { uint16_t startMin, endMin; };

class Scheduler {
public:
  Scheduler(RelayManager& r) : _r(r) {}
  
  void handle() {
    struct tm t;
    if (!getLocalTime(&t, 5)) return; 
    uint16_t now = t.tm_hour * 60 + t.tm_min;
    
    for (int i = 0; i < R_COUNT; i++) {
      if (_r.mode((RelayId)i) != MODE_AUTO) continue;
      TimeSlot& sl = _slots[i];
      if (sl.startMin == 0 && sl.endMin == 0) continue;
      
      bool on = (sl.startMin <= sl.endMin) 
        ? (now >= sl.startMin && now < sl.endMin) 
        : (now >= sl.startMin || now < sl.endMin); 
      
      if (_r.state((RelayId)i) != on) _r.set((RelayId)i, on);
    }
  }

  void syncTime() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
  }

  void setSlot(RelayId id, uint16_t s, uint16_t e) { _slots[id] = {s, e}; _r.setMode(id, MODE_AUTO); }
  bool getSlot(RelayId id, uint16_t& s, uint16_t& e) { s = _slots[id].startMin; e = _slots[id].endMin; return !(s == 0 && e == 0); }

private:
  RelayManager& _r;
  TimeSlot _slots[R_COUNT] = {{480,1320},{0,1440},{0,0},{0,0}}; // Luz 08:00-22:00, Aireador 24h
};

// --- GESTOR DE TEMPERATURA (DS18B20) ---
class TempSensor {
public:
  TempSensor(ConfigStore& c, RelayManager& r) : _cfg(c), _r(r) {}
  
  void begin() { sensors.begin(); }
  
  void handle(unsigned long everyMs) {
    unsigned long now = millis();
    if (now - _last < everyMs) return;
    _last = now;

    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    _valid = (t != DEVICE_DISCONNECTED_C && t > -50 && t < 60);
    
    if (!_valid) { 
      if (_r.mode(R_CALENT) == MODE_AUTO) _r.set(R_CALENT, false);
      return;
    }
    _temp = t; 

    if (_r.mode(R_CALENT) != MODE_AUTO || !_cfg.p.heaterEnabled) return;
    
    bool on = _r.state(R_CALENT);
    if (t < _cfg.p.tempTarget - _cfg.p.tempHyst) on = true;
    else if (t > _cfg.p.tempTarget + _cfg.p.tempHyst) on = false;
    
    if (on != _r.state(R_CALENT)) _r.set(R_CALENT, on);
  }

  float read() { return _temp; }
  bool  valid(){ return _valid; }

private:
  ConfigStore& _cfg; RelayManager& _r;
  float _temp = 0; bool _valid = false;
  unsigned long _last = 0;
  OneWire oneWire(PIN_DS18B20);
  DallasTemperature sensors(&oneWire);
};

// --- GESTOR WIFI (AP + STA) ---
class WifiManager {
public:
  WifiManager(ConfigStore& c) : _cfg(c) {}
  
  void begin() {
    if (strlen(_cfg.p.ssid) == 0) { startAP(); return; }
    WiFi.mode(WIFI_STA);
    WiFi.hostname("anakino");
    WiFi.begin(_cfg.p.ssid, _cfg.p.pass);
  }
  
  void handle() {
    if (WiFi.status() == WL_CONNECTED) return;
    unsigned long now = millis();
    if (now - _retryAt > 15000) { 
      _retryAt = now; 
      WiFi.disconnect(); 
      WiFi.begin(_cfg.p.ssid, _cfg.p.pass); 
    }
  }

  bool isConnected() { return WiFi.status() == WL_CONNECTED; }

private:
  ConfigStore& _cfg;
  DNSServer _dns; WebServer _ap{80};
  unsigned long _retryAt = 0;

  void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Anakino_Config");
    _dns.start(53, "*", WiFi.softAPIP());
    _ap.on("/connect", [this]() {
      // Aquí se leerían los parámetros del captive portal
      // Por simplicidad en este fichero único, si entra aquí se asume conexión
      _cfg.save(); ESP.restart(); 
    });
    _ap.onNotFound([this]() { 
      _ap.sendHeader("Location", "http://192.168.4.1/connect", true); 
      _ap.send(302); 
    });
    _ap.begin();
  }
};

// --- GESTOR OTA ---
class OtaManager {
public:
  void begin(const char* hostname) {
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.onStart([]() { Serial.println(F("OTA start")); });
    ArduinoOTA.onEnd([]() { Serial.println(F("OTA OK")); });
    ArduinoOTA.begin();
  }
  void handle() { ArduinoOTA.handle(); }
};

// --- PANEL WEB Y API ---
class WebUI {
public:
  WebUI(ConfigStore& c, RelayManager& r, Scheduler& s, TempSensor& t, OtaManager& o)
    : _cfg(c), _r(r), _s(s), _t(t), _o(o), _srv(80) {}

  void begin() {
    uint32_t r = esp_random();
    char buf[17]; sprintf(buf, "%08x%08x", r, esp_random());
    _token = buf; 

    _srv.on("/",         HTTP_GET,  [this]{ handlePage(); });
    _srv.on("/login",    HTTP_GET,  [this]{ _srv.send_P(200, "text/html", LOGINPAGE); });
    _srv.on("/api/login",HTTP_POST, [this]{ handleLogin(); });
    _srv.on("/api/status",HTTP_GET, [this]{ handleStatus(); });
    _srv.on("/api/relay", HTTP_POST,[this]{ handleRelay(); });
    _srv.on("/api/slot",  HTTP_POST,[this]{ handleSlot(); });
    _srv.on("/api/config",HTTP_POST,[this]{ handleConfig(); });
    _srv.on("/api/ota",   HTTP_POST,[this]{ handlePage(); });
    _srv.begin();
    _act = true;
    Serial.print(F("Panel web: http://")); Serial.println(WiFi.localIP());
  }

  void handle() { if (_act) _srv.handleClient(); }

private:
  ConfigStore&  _cfg;
  RelayManager& _r;
  Scheduler&    _s;
  TempSensor&   _t;
  OtaManager&   _o;
  WebServer     _srv;
  bool          _act = false;
  String        _token;

  bool authed() { return _srv.hasHeader("X-Token") && _srv.arg("X-Token") == _token; }
  void sendJson(int code, const String& body) { _srv.send(code, "application/json", body); }

  void handleLogin() {
    JsonDocument d; deserializeJson(d, _srv.arg("plain"));
    if (_cfg.checkAuth(d["user"] | "", d["pass"] | "")) {
      char out[64]; snprintf(out, sizeof(out), "{\"token\":\"%s\"}", _token.c_str());
      sendJson(200, out);
    } else sendJson(401, "{\"error\":\"bad\"}");
  }

  void handlePage() {
    if (!authed()) { _srv.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    _srv.send_P(200, "text/html", PAGE);
  }

  void handleStatus() {
    if (!authed()) return sendJson(401, "{}");
    JsonDocument d;
    d["fw"]   = FW_VERSION;
    d["temp"] = _t.read();
    d["valid"]= _t.valid();
    d["heater"] = _r.state(R_CALENT);
    d["tt"]   = _cfg.p.tempTarget;
    d["th"]   = _cfg.p.tempHyst;
    d["he"]   = _cfg.p.heaterEnabled;
    d["user"] = _cfg.p.user;
    
    JsonArray rel = d["relays"].to<JsonArray>();
    for (int i = 0; i < R_COUNT; i++) {
      JsonObject o = rel.add<JsonObject>();
      o["n"] = _r.name((RelayId)i);
      o["s"] = _r.state((RelayId)i);
      o["auto"] = _r.mode((RelayId)i) == MODE_AUTO;
    }
    
    JsonArray sl = d["slots"].to<JsonArray>();
    for (int i = 0; i < R_COUNT; i++) {
      uint16_t s, e; if (!_s.getSlot((RelayId)i, s, e)) continue;
      JsonObject o = sl.add<JsonObject>();
      o["n"] = _r.name((RelayId)i);
      char a[6]; sprintf(a, "%02d:%02d", s / 60, s % 60); o["st"] = a;
      char b[6]; sprintf(b, "%02d:%02d", e / 60, e % 60); o["en"] = b;
    }
    
    String out; serializeJson(d, out); sendJson(200, out);
  }

  void handleRelay() {
    if (!authed()) return sendJson(401, "{}");
    JsonDocument d; deserializeJson(d, _srv.arg("plain"));
    int id = d["id"] | -1;
    if (id < 0 || id >= R_COUNT) return sendJson(400, "{\"error\":\"id\"}");
    _r.set((RelayId)id, d["on"] | false);
    sendJson(200, "{\"ok\":true}");
  }

  void handleSlot() {
    if (!authed()) return sendJson(401, "{}");
    JsonDocument d; deserializeJson(d, _srv.arg("plain"));
    int id = d["id"] | -1;
    if (id < 0 || id >= R_COUNT) return sendJson(400, "{\"error\":\"id\"}");
    _s.setSlot((RelayId)id, d["start"] | 0, d["end"] | 0);
    sendJson(200, "{\"ok\":true}");
  }

  void handleConfig() {
    if (!authed()) return sendJson(401, "{}");
    JsonDocument d; deserializeJson(d, _srv.arg("plain"));
    _cfg.p.tempTarget   = d["tt"] | _cfg.p.tempTarget;
    _cfg.p.tempHyst     = d["th"] | _cfg.p.tempHyst;
    _cfg.p.heaterEnabled= d["he"] | _cfg.p.heaterEnabled;
    if (d["user"].is<const char*>()) strlcpy(_cfg.p.user, d["user"], 17);
    const char* np = d["pass"] | "";
    if (strlen(np) > 0) _cfg.setCredentials(_cfg.p.user, np);
    else _cfg.save();
    sendJson(200, "{\"ok\":true}");
  }
};

// HTML del Panel (incrustado para simplificar)
static const char* PAGE = R"rawliteral(<!DOCTYPE html><html lang="es"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Anakino Aquarium</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--txt:#e2e8f0;--acc:#38bdf8;--off:#475569}
*{box-sizing:border-box;font-family:system-ui,sans-serif}
body{margin:0;background:var(--bg);color:var(--txt);padding:16px}
h1{font-size:1.2rem;margin:0 0 12px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(220px,1fr))}
.card{background:var(--card);border-radius:12px;padding:16px}
.temp{font-size:2.4rem;color:var(--acc)}
.sw{width:52px;height:28px;border-radius:14px;background:var(--off);position:relative;cursor:pointer;transition:.2s}
.sw.on{background:var(--acc)}.sw i{position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:.2s}
.sw.on i{left:27px}
.row{display:flex;justify-content:space-between;align-items:center;margin:8px 0}
input,button{padding:8px;border-radius:8px;border:none;background:#0b1220;color:var(--txt)}
button{background:var(--acc);color:#04263a;font-weight:600;cursor:pointer}
label{display:block;font-size:.8rem;opacity:.7;margin-top:8px}
</style></head><body>
<h1>🐠 Anakino Aquarium <small id="v" style="opacity:.5"></small></h1>
<div class="grid">
<div class="card"><div>Temperatura</div><div class="temp" id="tp">--</div><div id="th" style="opacity:.6"></div></div>
<div class="card" id="relays"></div>
<div class="card"><h3>Horarios (minutos)</h3><div id="slots"></div></div>
<div class="card"><h3>Ajustes</h3>
<label>Objetivo °C</label><input id="tt" type="number" step="0.1">
<label>Histeresis °C</label><input id="thh" type="number" step="0.1">
<label>Calentador activo</label><input id="he" type="checkbox">
<label>Usuario</label><input id="us">
<label>Nueva contraseña</label><input id="pw" type="password" placeholder="vacío = no cambiar">
<br><br><button onclick="saveCfg()">Guardar</button>
<button onclick="ota()">UPDATE (OTA)</button></div>
</div>
<script>
let T=localStorage.getItem('tk')||'';
async function api(p,o={}){o.headers={'X-Token':T,...(o.headers||{})};const r=await fetch(p,o);
if(r.status==401){location='/login';throw 0}return r.json()}
async function load(){const d=await api('/api/status');document.getElementById('v').textContent='v'+d.fw;
document.getElementById('tp').textContent=d.temp.toFixed(1)+'°';
document.getElementById('th').textContent=(d.heater?'Calentando':'Estable')+' | obj '+d.tt+'°';
document.getElementById('tt').value=d.tt;document.getElementById('thh').value=d.th;
document.getElementById('he').checked=d.he;document.getElementById('us').value=d.user;
document.getElementById('relays').innerHTML=d.relays.map((r,i)=>
'<div class=row><span>'+r.n+(r.auto?' 🕒':'')+'</span><div class="sw'+(r.s?' on':'')+'" onclick="tog('+i+')"><i></i></div></div>').join('');
document.getElementById('slots').innerHTML=d.slots.map((s,i)=>
'<label>'+s.n+'</label><input type="time" value="'+s.st+'" onchange="slot('+i+',this.value)">').join('')}
function m(v){const p=v.split(':');return +p[0]*60+ +p[1]}
async function tog(i){await api('/api/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:i,on:!document.querySelectorAll('.sw')[i].classList.contains('on')})});load()}
async function slot(i,v){await api('/api/slot',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:i,start:m(v)})});load()}
async function saveCfg(){await api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tt:+tt.value,th:+thh.value,he:he.checked,user:us.value,pass:pw.value})});alert('Guardado')}
async function ota(){if(!confirm('¿Actualizar firmware por OTA?'))return;await api('/api/ota',{method:'POST'});alert('Iniciando OTA desde Arduino IDE (puerto de red anakino)')}
load();setInterval(load,5000)
</script></body></html>)rawliteral";

static const char* LOGINPAGE = R"rawliteral(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Login</title>
<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;display:grid;place-items:center;height:100vh;margin:0}
.c{background:#1e293b;padding:24px;border-radius:12px;width:280px}input{width:100%;padding:10px;margin:6px 0;border-radius:8px;border:none;background:#0b1220;color:#e2e8f0}
button{width:100%;padding:10px;border:none;border-radius:8px;background:#38bdf8;font-weight:600}</style></head>
<body><form class="c" onsubmit="go(event)"><h2>🔒 Anakino</h2>
<input id="u" placeholder="Usuario"><input id="p" type="password" placeholder="Contraseña">
<button>Entrar</button></form>
<script>async function go(e){e.preventDefault();const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({user:u.value,pass:p.value})});const d=await r.json();if(d.token){localStorage.tk=d.token;location='/'}else alert('Credenciales incorrectas')}</script>
</body></html>)rawliteral";

// ==========================================
// VARIABLES GLOBALES
// ==========================================

ConfigStore   cfg;
WifiManager   wifi(cfg);
RelayManager  relays;
Scheduler     sched(relays);
TempSensor    temp(cfg, relays);
OtaManager    ota;
WebUI         ui(cfg, relays, sched, temp, ota);

// ==========================================
// SETUP Y LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  cfg.load();
  relays.begin();
  temp.begin();
  wifi.begin();
  
  if (wifi.isConnected()) {
    sched.syncTime();
    ota.begin("anakino");
    ui.begin();
  }
}

void loop() {
  wifi.handle();
  ota.handle();
  ui.handle();
  sched.handle();
  temp.handle(1000);
  delay(2); // Cede CPU al sistema (anti-Watchdog)
}
