#include "config.h"
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// --------------------
// Board: AI Thinker ESP32-CAM
// --------------------
#define AI_THINKER_ESP32CAM 1

static const char* DEVICE_ID = "esp32_fixed_01";

// SoftAP
static const char* AP_SSID = ESPCAM_AP_SSID;
static const char* AP_PASS = ESPCAM_AP_PASSWORD;

// IP ثابت برای اینکه گوشی همیشه 192.168.4.1 درست کار کنه
IPAddress AP_IP(192,168,4,1);
IPAddress AP_GW(192,168,4,1);
IPAddress AP_MASK(255,255,255,0);

// Camera pins
#if AI_THINKER_ESP32CAM
  #define PWDN_GPIO_NUM     32
  #define RESET_GPIO_NUM    -1
  #define XCLK_GPIO_NUM      0
  #define SIOD_GPIO_NUM     26
  #define SIOC_GPIO_NUM     27
  #define Y9_GPIO_NUM       35
  #define Y8_GPIO_NUM       34
  #define Y7_GPIO_NUM       39
  #define Y6_GPIO_NUM       36
  #define Y5_GPIO_NUM       21
  #define Y4_GPIO_NUM       19
  #define Y3_GPIO_NUM       18
  #define Y2_GPIO_NUM        5
  #define VSYNC_GPIO_NUM    25
  #define HREF_GPIO_NUM     23
  #define PCLK_GPIO_NUM     22
#endif

// --------------------
// Config (NVS)
// --------------------
Preferences prefs;
String cfg_ssid, cfg_pass, cfg_server;
int targetNode = 1;

// --------------------
// Web server
// --------------------
WebServer web(80);

// --------------------
// Shared state
// --------------------
SemaphoreHandle_t mtx;

uint32_t frameSeq = 0;
uint32_t lastFrameStarted = 0;
uint32_t lastFrameDone = 0;
int      lastServerFrame = -1;

String phase = "WAIT_PHONE";   // WAIT_PHONE / CAPTURE / POSTING / OK / ERR
String lastErr = "";

int   curNode = 1;
float curProb = 0.0f;
String cmd = "-";
String heading = "-";
String abs_dir = "-";
int   next_node = -1;

unsigned long lastOkMs = 0;

// Reconnect STA بعد از اینکه پاسخ HTTP کامل ارسال شد
volatile bool staReconnectPending = false;
unsigned long staReconnectAtMs = 0;

TaskHandle_t uploadTaskHandle = nullptr;

// --------------------
// Helpers
// --------------------
void lockS(){ xSemaphoreTake(mtx, portMAX_DELAY); }
void unlockS(){ xSemaphoreGive(mtx); }

bool phoneConnected() {
  return WiFi.softAPgetStationNum() > 0;
}

String esc(String s){
  s.replace("&","&amp;"); s.replace("'","&#39;"); s.replace("<","&lt;"); s.replace(">","&gt;"); s.replace("\"","&quot;");
  return s;
}

bool loadConfig() {
  prefs.begin("cfg", true);
  cfg_ssid   = prefs.getString("ssid", "");
  cfg_pass   = prefs.getString("pass", "");
  cfg_server = prefs.getString("server", "");
  targetNode = prefs.getInt("target", 1);
  prefs.end();
  return (cfg_ssid.length() > 0 && cfg_server.length() > 0);
}

void saveConfig(const String& ssid, const String& pass, const String& server) {
  prefs.begin("cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("server", server);
  prefs.end();
}

void saveTarget(int t) {
  prefs.begin("cfg", false);
  prefs.putInt("target", t);
  prefs.end();
  targetNode = t;
}

String staText() {
  if (WiFi.status() == WL_CONNECTED) return "connected (" + WiFi.localIP().toString() + ")";
  return "not connected";
}

// --------------------
// Camera
// --------------------
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0       = Y2_GPIO_NUM;
  c.pin_d1       = Y3_GPIO_NUM;
  c.pin_d2       = Y4_GPIO_NUM;
  c.pin_d3       = Y5_GPIO_NUM;
  c.pin_d4       = Y6_GPIO_NUM;
  c.pin_d5       = Y7_GPIO_NUM;
  c.pin_d6       = Y8_GPIO_NUM;
  c.pin_d7       = Y9_GPIO_NUM;
  c.pin_xclk     = XCLK_GPIO_NUM;
  c.pin_pclk     = PCLK_GPIO_NUM;
  c.pin_vsync    = VSYNC_GPIO_NUM;
  c.pin_href     = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn     = PWDN_GPIO_NUM;
  c.pin_reset    = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;

  // سبک برای اینکه AP/UI لگ نزنه
  c.frame_size   = FRAMESIZE_QVGA; // 320x240
  c.jpeg_quality = 14;
  c.fb_count     = psramFound() ? 2 : 1;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed 0x%x\n", err);
    return false;
  }
  Serial.println("[CAM] ready");
  return true;
}

// --------------------
// WiFi
// --------------------
void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
  WiFi.softAP(AP_SSID, AP_PASS, 1, false, 1);

  delay(150);
  Serial.printf("[AP] SSID=%s IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void connectSTA_now() {
  if (cfg_ssid.length() == 0) return;

  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);

  // فقط STA را نرم قطع کن که AP گوشی نپره
  WiFi.disconnect(false);
  delay(60);

  Serial.printf("[STA] begin ssid='%s'\n", cfg_ssid.c_str());
  WiFi.begin(cfg_ssid.c_str(), cfg_pass.c_str());
}

// --------------------
// UI HTML (بدون reload) + fetch status + fetch save
// --------------------
String buildPage() {
  lockS();
  String ssid = cfg_ssid;
  String srv  = cfg_server;
  int tgt = targetNode;
  unlockS();

  String h;
  h.reserve(9000);

  h += "<!doctype html><html><head><meta charset='utf-8'/>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
  h += "<title>ESP NAV</title><style>";
  h += "body{font-family:Arial;margin:16px}.card{border:1px solid #ddd;border-radius:10px;padding:12px;margin:12px 0}";
  h += ".k{color:#666;font-size:13px}.v{font-size:18px}";
  h += "input{width:100%;padding:10px;font-size:16px;margin-top:6px}";
  h += "button{padding:12px 14px;font-size:16px;margin-top:10px}";
  h += "code{background:#f6f6f6;padding:2px 6px;border-radius:6px}";
  h += ".err{color:#b00020;font-weight:bold}";
  h += "</style></head><body>";

  h += "<h2>ESP Navigator</h2>";

  h += "<div class='card'>";
  h += "<div class='k'>Device</div><div class='v'><code>"; h += DEVICE_ID; h += "</code></div>";
  h += "<div class='k'>AP</div><div class='v'>";
  h += AP_SSID;
  h += " | clients=<span id='apc'>0</span>";
  h += " | UI: <code>http://192.168.4.1</code></div>";
  h += "<div class='k'>STA</div><div class='v' id='sta'>-</div>";
  h += "</div>";

  h += "<div class='card'>";
  h += "<div class='k'>Frames</div>";
  h += "<div class='v'>phase=<span id='ph'>-</span></div>";
  h += "<div class='v'>sent(start)=#<span id='fs'>-</span> | reply=#<span id='fd'>-</span> | server_frame=<span id='sf'>-</span></div>";
  h += "<div class='v err'>error: <span id='er'>-</span></div>";
  h += "<div class='k'>Last OK</div><div class='v'><span id='okago'>-</span></div>";
  h += "</div>";

  h += "<div class='card'>";
  h += "<div class='k'>Result</div>";
  h += "<div class='v'>current_node=<span id='cn'>-</span> | prob=<span id='pr'>-</span> | target=<span id='tg'>-</span></div>";
  h += "<div class='v'>next_node=<span id='nn'>-</span> | heading=<span id='hd'>-</span> | abs_dir=<span id='ab'>-</span></div>";
  h += "<div class='v'>command: <b><span id='cm'>-</span></b></div>";
  h += "</div>";

  // Target
  h += "<div class='card'>";
  h += "<div class='k'>Set Target (1..15)</div>";
  h += "<input id='tin' placeholder='مثلا 12' value='"; h += String(tgt); h += "'/>";
  h += "<button type='button' onclick='setTarget()'>Set</button>";
  h += "<div class='k' id='tmsg'></div>";
  h += "</div>";

  // WiFi
  h += "<div class='card'>";
  h += "<div class='k'>WiFi / Server Setup</div>";
  h += "<input id='ssid' placeholder='WiFi SSID' value='"; h += esc(ssid); h += "'/>";
  h += "<input id='pass' placeholder='WiFi Password' type='password' value=''/>"; // pass رو به دلایل امنیتی نمایش نمی‌دیم
  h += "<input id='srv' placeholder='Server URL مثل: http://192.168.1.10:8000' value='"; h += esc(srv); h += "'/>";
  h += "<button type='button' onclick='saveWifi()'>Save WiFi</button>";
  h += "<div class='k' id='wmsg'></div>";
  h += "</div>";

  // JS
  h += "<script>";
  h += "function setTxt(id,v){var e=document.getElementById(id); if(e) e.textContent=v;}";
  h += "function jget(url){return fetch(url+'?ts='+Date.now(),{cache:'no-store'}).then(r=>r.json());}";
  h += "function jpost(url,obj){return fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)}).then(r=>r.json());}";

  h += "function tick(){jget('/status.json').then(j=>{";
  h += "setTxt('apc', j.ap_clients);";
  h += "setTxt('sta', j.sta);";
  h += "setTxt('ph', j.phase);";
  h += "setTxt('fs', j.frame_started);";
  h += "setTxt('fd', j.frame_done);";
  h += "setTxt('sf', j.server_frame);";
  h += "setTxt('er', (j.err && j.err.length)? j.err : '-');";
  h += "setTxt('cn', j.current_node);";
  h += "setTxt('pr', Number(j.current_prob).toFixed(3));";
  h += "setTxt('tg', j.target_node);";
  h += "setTxt('nn', j.next_node);";
  h += "setTxt('cm', j.command);";
  h += "setTxt('hd', j.heading);";
  h += "setTxt('ab', j.abs_dir);";
  h += "setTxt('okago', j.last_ok_ago_s);";
  h += "}).catch(_=>{});}";

  h += "setInterval(tick,1000); tick();";

  h += "function setTarget(){";
  h += "var t=parseInt(document.getElementById('tin').value||'');";
  h += "setTxt('tmsg','setting...');";
  h += "jpost('/set_target',{target:t}).then(j=>{";
  h += "setTxt('tmsg', j.ok ? ('target set to '+j.target) : ('error: '+j.err));";
  h += "}).catch(_=>setTxt('tmsg','error: request failed'));";
  h += "}";

  h += "function saveWifi(){";
  h += "var ssid=document.getElementById('ssid').value||'';";
  h += "var pass=document.getElementById('pass').value||'';";
  h += "var srv=document.getElementById('srv').value||'';";
  h += "setTxt('wmsg','saving...');";
  h += "jpost('/save_wifi',{ssid:ssid,pass:pass,server:srv}).then(j=>{";
  h += "setTxt('wmsg', j.ok ? 'saved ✅  STA reconnecting...' : ('error: '+j.err));";
  h += "}).catch(_=>setTxt('wmsg','error: request failed'));";
  h += "}";

  h += "</script>";

  h += "</body></html>";
  return h;
}

// --------------------
// Web API
// --------------------
void setupWeb() {
  web.on("/", HTTP_GET, [](){
    web.send(200, "text/html; charset=utf-8", buildPage());
  });

  web.on("/status.json", HTTP_GET, [](){
    web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    web.sendHeader("Pragma", "no-cache");

    lockS();
    StaticJsonDocument<768> doc;
    doc["device"] = DEVICE_ID;
    doc["ap_clients"] = (int)WiFi.softAPgetStationNum();
    doc["sta"] = staText();

    doc["frame_started"] = lastFrameStarted;
    doc["frame_done"]    = lastFrameDone;
    doc["server_frame"]  = lastServerFrame;

    doc["phase"] = phase;
    doc["err"]   = lastErr;

    doc["current_node"] = curNode;
    doc["current_prob"] = curProb;
    doc["target_node"]  = targetNode;
    doc["next_node"]    = next_node;
    doc["command"]      = cmd;
    doc["heading"]      = heading;
    doc["abs_dir"]      = abs_dir;

    doc["last_ok_ago_s"] = (lastOkMs == 0) ? -1 : (int)((millis() - lastOkMs)/1000);
    unlockS();

    String out;
    serializeJson(doc, out);
    web.send(200, "application/json", out);
  });

  // JSON POST: set target
  web.on("/set_target", HTTP_POST, [](){
    if (!web.hasArg("plain")) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"no body\"}");
      return;
    }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, web.arg("plain"))) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}");
      return;
    }
    int t = doc["target"] | -1;
    if (t < 1 || t > 15) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"target must be 1..15\"}");
      return;
    }
    lockS(); saveTarget(t); unlockS();
    String resp = String("{\"ok\":true,\"target\":") + t + "}";
    web.send(200, "application/json", resp);
  });

  // JSON POST: save wifi
  web.on("/save_wifi", HTTP_POST, [](){
    String ssid, pass, server;

    // 1) preferred: JSON body
    if (web.hasArg("plain")) {
      StaticJsonDocument<384> doc;
      if (!deserializeJson(doc, web.arg("plain"))) {
        ssid = String((const char*)(doc["ssid"] | ""));
        pass = String((const char*)(doc["pass"] | ""));
        server = String((const char*)(doc["server"] | ""));
      }
    }

    // 2) fallback: form args (just in case)
    if (ssid.length() == 0 && web.hasArg("ssid")) ssid = web.arg("ssid");
    if (pass.length() == 0 && web.hasArg("pass")) pass = web.arg("pass");
    if (server.length() == 0 && web.hasArg("server")) server = web.arg("server");

    ssid.trim(); server.trim();
    while (server.endsWith("/")) server.remove(server.length()-1);

    if (ssid.length() == 0 || server.length() == 0 ||
        (!server.startsWith("http://") && !server.startsWith("https://"))) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"ssid/server required\"}");
      return;
    }

    // save
    lockS();
    cfg_ssid = ssid;
    cfg_pass = pass;
    cfg_server = server;
    saveConfig(cfg_ssid, cfg_pass, cfg_server);
    lastErr = "";
    unlockS();

    Serial.printf("[SAVE] ssid='%s' server='%s' pass_len=%d\n",
                  cfg_ssid.c_str(), cfg_server.c_str(), (int)cfg_pass.length());

    // respond first (so phone doesn't drop)
    web.send(200, "application/json", "{\"ok\":true}");

    // schedule STA reconnect shortly after response
    staReconnectPending = true;
    staReconnectAtMs = millis() + 400;
  });

  web.begin();
  Serial.println("[WEB] ready: http://192.168.4.1/");
}

// --------------------
// Upload
// --------------------
bool uploadOnce(uint32_t seq) {
  // اگر گوشی وصل نیست: هیچ کاری نکن
  if (!phoneConnected()) {
    lockS(); phase = "WAIT_PHONE"; lastErr = ""; unlockS();
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lockS(); phase = "ERR"; lastErr = "STA not connected"; unlockS();
    return false;
  }

  String server;
  int target;
  int hint;
  lockS(); server = cfg_server; target = targetNode; hint = curNode; unlockS();

  if (server.length() == 0) {
    lockS(); phase = "ERR"; lastErr = "Server not set"; unlockS();
    return false;
  }

  lockS(); phase = "CAPTURE"; lastErr = ""; unlockS();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    lockS(); phase = "ERR"; lastErr = "Camera capture failed"; unlockS();
    return false;
  }

  lockS(); phase = "POSTING"; unlockS();

  String url = server + "/upload?device=" + String(DEVICE_ID)
             + "&cur=" + String(hint)
             + "&target=" + String(target)
             + "&frame=" + String(seq);

  HTTPClient http;
  http.setTimeout(2000);
  if (!http.begin(url)) {
    esp_camera_fb_return(fb);
    lockS(); phase = "ERR"; lastErr = "Invalid server URL"; unlockS();
    return false;
  }
  http.addHeader("Content-Type", "image/jpeg");
  int code = http.POST(fb->buf, fb->len);
  esp_camera_fb_return(fb);

  if (code < 200 || code >= 300) {
    String e = http.errorToString(code);
    http.end();
    lockS(); phase = "ERR"; lastErr = "HTTP failed: " + e; unlockS();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(1800);
  if (deserializeJson(doc, body)) {
    lockS(); phase = "ERR"; lastErr = "Bad JSON from server"; unlockS();
    return false;
  }
  if (!(bool)(doc["ok"] | false)) {
    lockS(); phase = "ERR"; lastErr = "Server ok=false"; unlockS();
    return false;
  }

  lockS();
  lastServerFrame = (int)(doc["frame"] | -1);
  curNode   = doc["current_node"] | curNode;
  curProb   = (float)(doc["current_prob"] | 0.0f);
  cmd       = String((const char*)(doc["command"] | "-"));
  heading   = String((const char*)(doc["heading"] | "-"));
  abs_dir   = String((const char*)(doc["abs_dir"] | "-"));
  next_node = doc["next_node"].isNull() ? -1 : (int)(doc["next_node"] | -1);

  phase = "OK";
  lastErr = "";
  lastOkMs = millis();
  unlockS();

  return true;
}

void uploadTask(void* pv) {
  unsigned long last = 0;

  for (;;) {
    if (!phoneConnected()) {
      lockS(); phase = "WAIT_PHONE"; lastErr = ""; unlockS();
      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }

    if (millis() - last >= 5000) {
      last = millis();

      uint32_t seq;
      lockS();
      frameSeq++;
      seq = frameSeq;
      lastFrameStarted = seq;
      phase = "CAPTURE";
      lastErr = "";
      unlockS();

      bool ok = uploadOnce(seq);

      lockS();
      lastFrameDone = seq;
      if (!ok && phase != "WAIT_PHONE" && phase != "ERR") {
        phase = "ERR";
        if (lastErr.length() == 0) lastErr = "Unknown error";
      }
      unlockS();
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// --------------------
// main
// --------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  mtx = xSemaphoreCreateMutex();

  if (!mtx) { Serial.println("Mutex allocation failed"); return; }
  bool cameraReady = initCamera();
  startAP();
  setupWeb();

  loadConfig();
  if (cfg_ssid.length()) connectSTA_now();

  if (cameraReady && xTaskCreatePinnedToCore(uploadTask, "uploadTask", 12288, nullptr, 1, &uploadTaskHandle, 0) != pdPASS) {
    lockS(); phase = "ERR"; lastErr = "Upload task allocation failed"; unlockS();
  } else if (!cameraReady) {
    lockS(); phase = "ERR"; lastErr = "Camera initialization failed"; unlockS();
  }
}

void loop() {
  web.handleClient();
  delay(1);

  // reconnect STA بعد از پاسخ HTTP
  if (staReconnectPending && millis() > staReconnectAtMs) {
    staReconnectPending = false;
    connectSTA_now();
  }
}
