#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"

// ====== Wi-Fi ======
const char* WIFI_SSID = "xxxxxxxxx";
const char* WIFI_PASS = "xxxxxxxxx";

// ====== Pinos AI Thinker ESP32-CAM ======
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

#define FLASH_GPIO         4

// ====== HTTPD ======
static httpd_handle_t httpd = NULL;
static volatile bool streamingBusy = false;

// ====== HTML simples ======
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM</title>
<style>
  body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Helvetica,Arial,sans-serif;margin:0;background:#0b0e11;color:#e6e6e6}
  header{padding:12px 16px;background:#11161c;border-bottom:1px solid #222}
  .wrap{max-width:900px;margin:16px auto;padding:0 12px}
  .card{background:#141a21;border:1px solid #222;border-radius:12px;padding:16px}
  .row{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
  button,a.btn{padding:10px 16px;border-radius:10px;border:1px solid #2a3340;background:#1a2330;color:#e6e6e6;text-decoration:none;cursor:pointer}
  img{max-width:100%;border-radius:10px}
  small{opacity:.8}
</style></head><body>
<header><div class="wrap"><b>ESP32-CAM — Live</b></div></header>
<div class="wrap">
  <div class="card">
    <div class="row">
      <a class="btn" href="/jpg" target="_blank">📷 Snapshot</a>
      <button onclick="fetch('/flash?onoff=toggle')">💡 Flash</button>
      <button onclick="fetch('/restart')">♻️ Restart</button>
      <small id="ip"></small>
    </div>
  </div>
  <div class="card" style="margin-top:16px">
    <img id="stream" src="/stream">
  </div>
</div>
<script>
  document.getElementById('ip').textContent = 'IP: '+location.host;
</script>
</body></html>
)HTML";

// ====== Handlers ======
static esp_err_t index_handler(httpd_req_t *req){
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t jpg_handler(httpd_req_t *req){
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return httpd_resp_send_500(req);

  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t r = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return r;
}

// Flash simples on/off/toggle
static esp_err_t flash_handler(httpd_req_t *req){
  char buf[16]; String onoff;
  int n = httpd_req_get_url_query_len(req) + 1;
  if (n > 1) {
    std::unique_ptr<char[]> q(new char[n]);
    if (httpd_req_get_url_query_str(req, q.get(), n) == ESP_OK) {
      if (httpd_query_key_value(q.get(), "onoff", buf, sizeof(buf)) == ESP_OK) onoff = String(buf);
    }
  }
  static bool state = false;
  if (onoff == "on") state = true;
  else if (onoff == "off") state = false;
  else state = !state;
  digitalWrite(FLASH_GPIO, state ? HIGH : LOW);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, state ? "flash:on" : "flash:off", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t restart_handler(httpd_req_t *req){
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "restarting...", HTTPD_RESP_USE_STRLEN);
  delay(200);
  ESP.restart();
  return ESP_OK;
}

static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace; boundary=frame";
static const char* STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req){
  if (streamingBusy){
    httpd_resp_set_status(req, "503 Busy");
    return httpd_resp_send(req, "busy", HTTPD_RESP_USE_STRLEN);
  }
  streamingBusy = true;

  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  uint32_t lastLog = millis();
  int fail = 0;

  while(true){
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      fail++;
      if (fail >= 5) {
        // fallback: reduzir resolução em runtime
        sensor_t *s = esp_camera_sensor_get();
        if (s) s->set_framesize(s, FRAMESIZE_QVGA); // 320x240
        fail = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    fail = 0;

    char part[64];
    int len = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    if (httpd_resp_send_chunk(req, part, len) != ESP_OK ||
        httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len) != ESP_OK ||
        httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) {
      esp_camera_fb_return(fb);
      break; // cliente desconectou
    }
    esp_camera_fb_return(fb);

    // log periódico de memória p/ diagnosticar
    if (millis() - lastLog > 5000) {
      lastLog = millis();
      size_t dram = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      size_t ps   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      Serial.printf("[MEM] DRAM=%u  PSRAM=%u\n", (unsigned)dram, (unsigned)ps);
    }

    vTaskDelay(pdMS_TO_TICKS(5)); // alívio p/ watchdog e TCP
  }

  streamingBusy = false;
  return ESP_OK;
}

void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = 2;            // baixo = mais estável
  config.stack_size = 8192;               // mais folga
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;

  if (httpd_start(&httpd, &config) == ESP_OK) {
    httpd_uri_t index_uri  = { .uri="/",      .method=HTTP_GET, .handler=index_handler, .user_ctx=NULL };
    httpd_uri_t jpg_uri    = { .uri="/jpg",   .method=HTTP_GET, .handler=jpg_handler,   .user_ctx=NULL };
    httpd_uri_t stream_uri = { .uri="/stream",.method=HTTP_GET, .handler=stream_handler,.user_ctx=NULL };
    httpd_uri_t flash_uri  = { .uri="/flash", .method=HTTP_GET, .handler=flash_handler, .user_ctx=NULL };
    httpd_uri_t rst_uri    = { .uri="/restart",.method=HTTP_GET,.handler=restart_handler,.user_ctx=NULL };
    httpd_register_uri_handler(httpd, &index_uri);
    httpd_register_uri_handler(httpd, &jpg_uri);
    httpd_register_uri_handler(httpd, &stream_uri);
    httpd_register_uri_handler(httpd, &flash_uri);
    httpd_register_uri_handler(httpd, &rst_uri);
  }
}

// ====== Câmera ======
bool initCamera(){
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;          // 20 MHz
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST; // ↓ latência, ↓ fila

  bool hasPsram = psramFound();

  // Perfil mais estável (começa em VGA)
  config.frame_size   = FRAMESIZE_VGA;     // 640x480 (bom p/ estabilidade)
  config.jpeg_quality = 15;                // 10~20 (maior = mais leve)
  config.fb_count     = 1;                 // 1 = menos pressão na RAM

  // Local do framebuffer
  config.fb_location  = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("camera init fail: 0x%x\n", err);
    return false;
  }

  // Ajustes leves do sensor
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    if (s->set_lenc) s->set_lenc(s, 1);
  }
  return true;
}

// ====== Wi-Fi ======
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // sem power-save
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Também força PS off via IDF
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.printf("Conectando em %s", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi OK. IP: "); Serial.println(WiFi.localIP());
  } else {
    // Fallback AP
    uint64_t chipid = ESP.getEfuseMac();
    char apName[32];
    sprintf(apName, "ESP32CAM-%04X", (uint16_t)(chipid & 0xFFFF));
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName);
    Serial.print("AP ativo: "); Serial.println(apName);
    Serial.print("Acesse em: http://"); Serial.println(WiFi.softAPIP());
  }
}

void setup(){
  pinMode(FLASH_GPIO, OUTPUT);
  digitalWrite(FLASH_GPIO, LOW);

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32-CAM: Streaming (Stable) ===");

  if (!initCamera()){
    Serial.println("Falha ao inicializar câmera. Verifique alimentação/pinos.");
    while(true){ delay(1000); }
  }

  connectWiFi();
  startCameraServer();

  // Log inicial de memória
  Serial.printf("[MEM] DRAM=%u  PSRAM=%u\n",
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  Serial.println("Servidor ativo. Abra o IP no navegador (rota /).");
}

void loop(){
  delay(1000);
}
