/*
 * Token Quota Display — ESP32-2424S012C (ESP32-C3 + GC9A01 + CST816D)
 *
 * Cinco telas LVGL, distribuídas em torno do resumo:
 *   Grok ← Claude ← Resumo → Codex → Gemini
 *   Claude/Codex: sessão de 5h + limite semanal
 *   Gemini/Grok: cota semanal com reset e origem da leitura
 *
 * O resumo abre primeiro. Arraste um anel para cima (ou toque nele) para
 * abrir o modelo; arraste para baixo para retornar. O carrossel horizontal
 * continua disponível entre as telas detalhadas.
 * O coletor é consultado a cada 15s e os tempos de reset têm contagem
 * regressiva local entre polls.
 *
 * Pinos confirmados para ESP32-2424S012C-I:
 *   Display: SCLK=6 MOSI=7 CS=10 DC=2 RST=-1 BL=3
 *   Touch CST816D: SDA=4 SCL=5 INT=0 RST=1
 */

#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Wire.h>
#include <WiFi.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

#include "secrets.h"

// --------------------------------------------------------------------------
// Hardware
// --------------------------------------------------------------------------

#define PIN_LCD_SCLK 6
#define PIN_LCD_MOSI 7
#define PIN_LCD_CS   10
#define PIN_LCD_DC   2
#define PIN_LCD_RST  -1
#define PIN_LCD_BL   3

#define PIN_TOUCH_SDA 4
#define PIN_TOUCH_SCL 5
#define PIN_TOUCH_INT 0
#define PIN_TOUCH_RST 1
#define CST816_ADDR   0x15

#define SCREEN_W 240
#define SCREEN_H 240
#define LVGL_BUF_LINES 40

// Incrementar VERSION_CODE a cada firmware publicado via OTA.
static const uint32_t FIRMWARE_VERSION_CODE = 2026102447UL;
static const uint32_t OTA_CHECK_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
static const char *OTA_MANIFEST_PATH = "/firmware/manifest.json";
static const char *OTA_BINARY_PATH = "/firmware/token_quota_display.bin";

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, GFX_NOT_DEFINED);
// Rotação 180°: deixa o conteúdo na orientação correta com o USB para baixo.
Arduino_GFX *gfx = new Arduino_GC9A01(bus, PIN_LCD_RST, 2, true);

static lv_display_t *g_display;
static lv_indev_t *g_touch_indev;
static lv_color_t g_lvgl_buffer[SCREEN_W * LVGL_BUF_LINES];

// --------------------------------------------------------------------------
// Design tokens
// --------------------------------------------------------------------------

static const uint32_t COLOR_BG = 0x121C2E;
static const uint32_t COLOR_SURFACE = 0x111820;
static const uint32_t COLOR_SURFACE_ALT = 0x27313D;
static const uint32_t COLOR_TRACK = 0x1E2A40;
static const uint32_t COLOR_TEXT = 0xDCE4EC;
static const uint32_t COLOR_MUTED = 0xB7C0CA;
static const uint32_t COLOR_CLAUDE = 0xD98C5A;
static const uint32_t COLOR_CODEX = 0x23BBA8;
static const uint32_t COLOR_GEMINI = 0xC4A43F;
// O símbolo do Grok é monocromático; branco mantém a leitura sem competir
// com o azul do Gemini.
static const uint32_t COLOR_GROK = 0xDCE4EC;
static const uint32_t COLOR_CLAUDE_TRACK = 0x1E2A40;
static const uint32_t COLOR_CODEX_TRACK = 0x1B3944;
static const uint32_t COLOR_GEMINI_TRACK = 0x353125;
static const uint32_t COLOR_GROK_TRACK = 0x303946;
static const uint32_t COLOR_RESET_TEXT = 0xDCE4EC;
// Paleta dessaturada para reduzir brilho e halos no ceratocone.
static const uint32_t COLOR_USAGE_GREEN = 0x78B892;
static const uint32_t COLOR_USAGE_YELLOW = 0xC9B56A;
static const uint32_t COLOR_USAGE_RED = 0xC97878;

// --------------------------------------------------------------------------
// Dados e temporização
// --------------------------------------------------------------------------

struct ModelQuota {
  int session_pct = 0;
  int week_pct = 0;
  int session_reset_min = -1;
  int week_reset_min = -1;
  int projection_min = -1;
  int projected_pct_at_reset = -1;
  int trend[24];
  char source[20] = "";
  uint32_t observed_epoch = 0;
  uint64_t session_tokens = 0;
  uint64_t week_tokens = 0;
};

struct WeeklyQuota {
  int used_pct = 0;
  int remaining_pct = -1;
  int reset_min = -1;
  uint32_t reset_epoch = 0;
  uint32_t observed_epoch = 0;
  uint64_t week_tokens = 0;
  char source[28] = "";
  char plan_label[24] = "";
  bool available = false;
  bool stale = true;
  bool token_only = false;
};

struct QuotaState {
  ModelQuota claude;
  ModelQuota codex;
  WeeklyQuota gemini;
  WeeklyQuota grok;
  uint32_t updated_epoch = 0;
  bool valido = false;
};

static QuotaState g_quota;

static const uint32_t POLL_INTERVAL_MS = 15000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 20000;
static const uint32_t HTTP_TIMEOUT_MS = 5000;
static const uint32_t COUNTDOWN_REFRESH_MS = 1000;

static uint32_t g_last_poll_ms = 0;
static uint32_t g_last_wifi_attempt_ms = 0;
static uint32_t g_last_countdown_refresh_ms = 0;
static uint32_t g_last_ota_check_ms = 0;
static uint32_t g_quota_received_ms = 0;
static uint8_t g_http_failures = 0;
static bool g_ota_checked_once = false;
static bool g_mdns_started = false;
static IPAddress g_collector_ip;

// --------------------------------------------------------------------------
// Objetos de UI
// --------------------------------------------------------------------------

enum PageIndex : uint8_t {
  // A página principal fica no centro do carrossel: Grok ← Claude ←
  // Resumo → Codex → Gemini. Isso deixa a navegação simétrica no mostrador.
  PAGE_GROK = 0,
  PAGE_CLAUDE = 1,
  PAGE_SUMMARY = 2,
  PAGE_CODEX = 3,
  PAGE_GEMINI = 4,
};

struct SummaryUI {
  lv_obj_t *screen;
  lv_obj_t *arc_claude;
  lv_obj_t *arc_codex;
  lv_obj_t *arc_gemini;
  lv_obj_t *arc_grok;
  lv_obj_t *label_claude;
  lv_obj_t *label_claude_pct;
  lv_obj_t *label_claude_reset;
  lv_obj_t *label_codex;
  lv_obj_t *label_codex_pct;
  lv_obj_t *label_codex_reset;
  lv_obj_t *label_gemini_week;
  lv_obj_t *label_gemini_pct;
  lv_obj_t *label_gemini_reset;
  lv_obj_t *label_grok_week;
  lv_obj_t *label_grok_pct;
  lv_obj_t *label_grok_reset;
  lv_obj_t *label_status;
  lv_obj_t *mascot_claude;
  lv_obj_t *mascot_codex;
  lv_obj_t *label_reset_nw;
  lv_obj_t *label_reset_ne;
  lv_obj_t *label_reset_sw;
  lv_obj_t *label_reset_se;
  lv_obj_t *label_reset_nw_weight;
  lv_obj_t *label_reset_ne_weight;
  lv_obj_t *label_reset_sw_weight;
  lv_obj_t *label_reset_se_weight;
  lv_obj_t *label_active_name;
  lv_obj_t *label_active_name_weight;
  lv_obj_t *label_active_pct;
  lv_obj_t *model_dots[4];
};

struct DetailUI {
  lv_obj_t *screen;
  lv_obj_t *bar_session;
  lv_obj_t *bar_week;
  lv_obj_t *label_session_pct;
  lv_obj_t *label_week_pct;
  lv_obj_t *label_session_reset;
  lv_obj_t *label_week_reset;
  lv_obj_t *label_insight;
  lv_obj_t *label_status;
  lv_obj_t *mascot;
  lv_obj_t *blink_left;
  lv_obj_t *blink_right;
  lv_obj_t *sweat;
  int mascot_base_x;
  int mascot_base_y;
  int mascot_last_x;
  int mascot_last_y;
  bool blink_visible;
  uint32_t brand_color;
};

struct WeeklyUI {
  lv_obj_t *screen;
  lv_obj_t *mascot;
  lv_obj_t *bar_week;
  lv_obj_t *label_used;
  lv_obj_t *label_remaining;
  lv_obj_t *label_reset;
  lv_obj_t *label_source;
  lv_obj_t *label_status;
  uint32_t brand_color;
};

static SummaryUI g_summary;
static DetailUI g_claude_ui;
static DetailUI g_codex_ui;
static WeeklyUI g_gemini_ui;
static WeeklyUI g_grok_ui;
static PageIndex g_current_page = PAGE_SUMMARY;
static int8_t g_pending_page_delta = 0;
static int8_t g_pending_page_target = -1;
static uint32_t g_navigation_locked_until = 0;
static bool g_thresholds_initialized = false;
static int g_previous_claude_peak = 0;
static int g_previous_codex_peak = 0;
static uint32_t g_alert_until = 0;
static volatile bool g_sync_requested = false;
static uint8_t g_active_reset_model = 0;
static uint32_t g_last_reset_model_switch_ms = 0;
static const uint32_t SUMMARY_MODEL_SWITCH_MS = 4000;

static void set_status(const char *message);
static int countdown_minutes(int base_minutes);
static void format_duration(int minutes, char *buffer, size_t size);
static void format_tokens(uint64_t tokens, char *buffer, size_t size);
struct NetworkResult { char body[6144]; char status[48]; uint32_t received; };
static QueueHandle_t g_network_results, g_network_requests;
static NetworkResult g_network_result;
static uint32_t g_server_epoch = 0, g_last_contact_ms = 0;
static lv_obj_t *g_extra_screen, *g_extra_title, *g_extra_text;
static lv_obj_t *g_trend_columns[24];
static int g_extra_mode = 0; // 1 Claude, 2 Codex, 3 conexão
static void refresh_extra();
static void open_extra(lv_event_t *event);
static void network_status(const char *message) {
  snprintf(g_network_result.status, sizeof(g_network_result.status), "%s", message);
}

// --------------------------------------------------------------------------
// Touch CST816D
// --------------------------------------------------------------------------

static volatile bool g_touch_irq = false;
static bool g_touch_pressed = false;
static uint16_t g_touch_x = SCREEN_W / 2;
static uint16_t g_touch_y = SCREEN_H / 2;
static uint16_t g_touch_start_x = 0;
static uint16_t g_touch_start_y = 0;
static uint32_t g_touch_last_sample_ms = 0;
static uint32_t g_touch_start_ms = 0;

void IRAM_ATTR touch_irq_cb() {
  g_touch_irq = true;
}

static bool cst816_read_point(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x02); // FingerNum, XposH/L, YposH/L
  if (Wire.endTransmission(false) != 0) return false;

  const uint8_t bytes = Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)5);
  if (bytes != 5) {
    while (Wire.available()) Wire.read();
    return false;
  }

  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if (fingers == 0) return false;

  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);
  // O touch permanece fisicamente orientado a 0°; alinhe-o ao display girado.
  x = SCREEN_W - 1 - x;
  y = SCREEN_H - 1 - y;
  return true;
}

static int8_t summary_target_for_point(uint16_t x, uint16_t y) {
  const int radii[] = {86, 70, 54, 38};
  const PageIndex pages[] = {
    PAGE_CLAUDE, PAGE_CODEX, PAGE_GROK, PAGE_GEMINI
  };
  int dx = (int)x - SCREEN_W / 2;
  int dy = (int)y - 120;
  int distance_squared = dx * dx + dy * dy;
  for (int i = 0; i < 4; ++i) {
    int min_radius = radii[i] - 9;
    int max_radius = radii[i] + 9;
    if (distance_squared >= min_radius * min_radius &&
        distance_squared <= max_radius * max_radius) return pages[i];
  }
  return -1;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  uint16_t x;
  uint16_t y;
  bool new_point = false;

  // Polling contínuo evita perder o primeiro toque quando o INT do CST816D
  // fica preso após wake/sleep; o pacote I2C é pequeno e mantém resposta rápida.
  g_touch_irq = false;
  new_point = cst816_read_point(x, y);

  if (new_point) {
    g_touch_x = x;
    g_touch_y = y;
    g_touch_last_sample_ms = millis();
    if (!g_touch_pressed) {
      g_touch_pressed = true;
      g_touch_start_x = x;
      g_touch_start_y = y;
      g_touch_start_ms = millis();
    }
  }

  // Encerra o toque rapidamente após a soltura para deixar swipes responsivos.
  if (g_touch_pressed && (millis() - g_touch_last_sample_ms > 45)) {
    int dx = (int)g_touch_x - (int)g_touch_start_x;
    int dy = (int)g_touch_y - (int)g_touch_start_y;
    if (abs(dy) >= 24 && abs(dy) > abs(dx)) {
      if (dy < 0 && g_current_page == PAGE_SUMMARY) {
        g_pending_page_target = summary_target_for_point(
          g_touch_start_x, g_touch_start_y);
      } else if (dy > 0 && g_current_page != PAGE_SUMMARY) {
        g_pending_page_target = PAGE_SUMMARY;
      }
      Serial.print("Swipe touch dy=");
      Serial.println(dy);
    } else if (abs(dx) >= 28 && abs(dx) > abs(dy)) {
      g_pending_page_delta = dx < 0 ? 1 : -1;
      Serial.print("Swipe touch dx=");
      Serial.println(dx);
    } else if (millis() - g_touch_start_ms >= 80 &&
               abs(dx) < 20 && abs(dy) < 20) {
      // O toque no anel oferece uma alternativa acessível ao gesto vertical.
      if (g_current_page == PAGE_SUMMARY) {
        int8_t target = summary_target_for_point(
          g_touch_start_x, g_touch_start_y);
        if (target >= 0) {
          g_pending_page_target = target;
        } else if (g_touch_start_x <= 76) {
          // Área lateral esquerda: Claude.
          g_pending_page_delta = -1;
        } else if (g_touch_start_x >= SCREEN_W - 76) {
          // Área lateral direita: Codex.
          g_pending_page_delta = 1;
        }
      } else if (g_touch_x < 70) {
        g_pending_page_delta = -1;
      } else if (g_touch_x > SCREEN_W - 70) {
        g_pending_page_delta = 1;
      }
    }
    g_touch_pressed = false;
  }

  data->point.x = g_touch_x;
  data->point.y = g_touch_y;
  data->state = g_touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void init_touch() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(10);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(50);

  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touch_irq_cb, FALLING);

  g_touch_indev = lv_indev_create();
  lv_indev_set_type(g_touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(g_touch_indev, touch_read_cb);
  lv_indev_set_display(g_touch_indev, g_display);
  Serial.println("Touch CST816D pronto");
}

// --------------------------------------------------------------------------
// Helpers visuais
// --------------------------------------------------------------------------

static lv_color_t color_hex(uint32_t hex) {
  return lv_color_hex(hex);
}

static uint32_t soften_color(uint32_t color) {
  // Mantém os anéis inativos legíveis, mas reduz o brilho percebido para
  // evitar halos no display circular.
  uint8_t r = (uint8_t)((color >> 16) & 0xFF);
  uint8_t g = (uint8_t)((color >> 8) & 0xFF);
  uint8_t b = (uint8_t)(color & 0xFF);
  r = (uint8_t)((r * 60U) / 100U);
  g = (uint8_t)((g * 60U) / 100U);
  b = (uint8_t)((b * 60U) / 100U);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static lv_color_t quota_color(int pct) {
  // Semáforo de consumo: verde até 65%, amarelo acima de 65%,
  // vermelho a partir de 80%.
  if (pct >= 80) return color_hex(COLOR_USAGE_RED);
  if (pct > 65) return color_hex(COLOR_USAGE_YELLOW);
  return color_hex(COLOR_USAGE_GREEN);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color,
                            const lv_font_t *font, lv_align_t align, int x, int y) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color_hex(color), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_align(label, align, x, y);
  return label;
}

static void style_screen(lv_obj_t *screen) {
  lv_obj_set_style_bg_color(screen, color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_card(lv_obj_t *parent, int y) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 188, 53);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_bg_color(card, color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, color_hex(COLOR_SURFACE_ALT), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
  return card;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int width) {
  lv_obj_t *bar = lv_bar_create(parent);
  lv_obj_set_size(bar, width, 6);
  lv_obj_set_pos(bar, x, y);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, color_hex(COLOR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
  return bar;
}

static lv_obj_t *make_pixel(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t color) {
  lv_obj_t *pixel = lv_obj_create(parent);
  lv_obj_remove_style_all(pixel);
  lv_obj_set_size(pixel, w, h);
  lv_obj_set_pos(pixel, x, y);
  lv_obj_set_style_bg_color(pixel, color_hex(color), 0);
  lv_obj_set_style_bg_opa(pixel, LV_OPA_COVER, 0);
  lv_obj_add_flag(pixel, LV_OBJ_FLAG_HIDDEN);
  return pixel;
}

static lv_obj_t *make_visible_pixel(lv_obj_t *parent, int x, int y, int w, int h,
                                    uint32_t color) {
  lv_obj_t *pixel = make_pixel(parent, x, y, w, h, color);
  lv_obj_remove_flag(pixel, LV_OBJ_FLAG_HIDDEN);
  return pixel;
}

static lv_obj_t *make_visible_round(lv_obj_t *parent, int x, int y, int w, int h,
                                    int radius, uint32_t color) {
  lv_obj_t *shape = make_visible_pixel(parent, x, y, w, h, color);
  lv_obj_set_style_radius(shape, radius, 0);
  return shape;
}

static lv_obj_t *make_weekly_mascot(lv_obj_t *parent, bool is_gemini,
                                     int x, int y) {
  lv_obj_t *root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 46, 38);
  lv_obj_set_pos(root, x, y);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);

  if (is_gemini) {
    // Gemini: estrela facetada de quatro pontas com expressão discreta.
    const uint32_t gem_dark = 0x604B91;
    const uint32_t gem_light = 0xC7B6F2;
    const uint32_t face = 0x171225;
    make_visible_round(root, 16, 1, 14, 34, 7, COLOR_GEMINI);
    make_visible_round(root, 6, 11, 34, 14, 7, COLOR_GEMINI);
    make_visible_round(root, 12, 7, 22, 22, 10, gem_dark);
    make_visible_pixel(root, 17, 14, 4, 5, face);
    make_visible_pixel(root, 27, 14, 4, 5, face);
    make_visible_pixel(root, 21, 23, 7, 2, gem_light);
    make_visible_round(root, 20, 2, 7, 7, 4, gem_light);
  } else {
    // Grok: núcleo monocromático com órbita diagonal, derivado do símbolo xAI.
    const uint32_t shell = 0xD9DEE4;
    const uint32_t visor = 0x161B22;
    const uint32_t eye = 0x8DE7DD;
    make_visible_round(root, 7, 2, 32, 32, 16, shell);
    make_visible_round(root, 11, 6, 24, 24, 12, visor);
    make_visible_pixel(root, 5, 27, 9, 3, shell);
    make_visible_pixel(root, 11, 23, 10, 3, shell);
    make_visible_pixel(root, 18, 19, 10, 3, shell);
    make_visible_pixel(root, 25, 15, 10, 3, shell);
    make_visible_pixel(root, 32, 11, 9, 3, shell);
    make_visible_pixel(root, 15, 12, 4, 4, eye);
    make_visible_pixel(root, 27, 12, 4, 4, eye);
    make_visible_round(root, 16, 29, 16, 7, 4, visor);
  }
  return root;
}

static lv_obj_t *make_brand_mascot(lv_obj_t *parent, bool is_claude,
                                    int x, int y, lv_obj_t **blink_left,
                                    lv_obj_t **blink_right, lv_obj_t **sweat) {
  lv_obj_t *root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 48, 38);
  lv_obj_set_pos(root, x, y);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);

  uint32_t brand = is_claude ? COLOR_CLAUDE : COLOR_CODEX;
  if (is_claude) {
    // Clawd reduzido para a tela circular: corpo, braços e olhos apenas.
    // A leitura da silhueta é priorizada em vez dos detalhes pequenos.
    const uint32_t clawd_shadow = 0xB95640;
    const uint32_t clawd_eye = 0x1B1110;
    make_visible_pixel(root, 7, 8, 34, 18, brand);
    make_visible_pixel(root, 0, 15, 7, 8, brand);
    make_visible_pixel(root, 41, 15, 7, 8, brand);
    make_visible_pixel(root, 7, 24, 34, 2, clawd_shadow);
    // Quatro pés de apenas quatro pixels sugerem as pernas sem alongar o ícone.
    make_visible_pixel(root, 9, 26, 4, 4, brand);
    make_visible_pixel(root, 18, 26, 4, 4, brand);
    make_visible_pixel(root, 27, 26, 4, 4, brand);
    make_visible_pixel(root, 36, 26, 4, 4, brand);
    make_visible_pixel(root, 14, 15, 4, 5, clawd_eye);
    make_visible_pixel(root, 30, 15, 4, 5, clawd_eye);
    *blink_left = make_pixel(root, 14, 15, 4, 5, brand);
    *blink_right = make_pixel(root, 30, 15, 4, 5, brand);
  } else {
    // Companion Codex: capuz azul em forma de nuvem, visor escuro e
    // expressão de terminal, seguindo a referência fornecida.
    const uint32_t hood = 0x3F6FE5;
    const uint32_t hood_light = 0x5A86F4;
    const uint32_t visor = 0x101A3A;
    const uint32_t terminal = 0x69F0F2;
    make_visible_round(root, 6, 3, 36, 24, 12, hood);
    make_visible_round(root, 2, 8, 13, 18, 7, hood);
    make_visible_round(root, 34, 7, 12, 19, 7, hood);
    make_visible_round(root, 11, 0, 15, 10, 7, hood_light);
    make_visible_round(root, 24, 1, 14, 9, 6, hood_light);
    make_visible_round(root, 9, 8, 30, 15, 5, visor);
    // Prompt >_ no visor.
    make_visible_pixel(root, 15, 12, 3, 3, terminal);
    make_visible_pixel(root, 18, 14, 3, 3, terminal);
    make_visible_pixel(root, 15, 16, 3, 3, terminal);
    make_visible_pixel(root, 27, 17, 7, 2, terminal);
    make_visible_round(root, 14, 24, 21, 10, 5, hood);
    make_visible_round(root, 8, 25, 8, 9, 4, hood);
    make_visible_round(root, 33, 25, 8, 9, 4, hood);
    make_visible_round(root, 15, 32, 8, 6, 3, hood);
    make_visible_round(root, 27, 32, 8, 6, 3, hood);
    make_visible_pixel(root, 20, 27, 3, 2, terminal);
    make_visible_pixel(root, 23, 29, 3, 2, terminal);
    make_visible_pixel(root, 20, 31, 3, 2, terminal);
    *blink_left = make_pixel(root, 14, 11, 8, 9, visor);
    *blink_right = make_pixel(root, 26, 15, 9, 5, visor);
  }
  *sweat = make_pixel(root, 42, 2, 4, 8, 0x62C7FF);
  lv_obj_set_style_radius(*sweat, 3, 0);
  return root;
}

static void sync_button_event_cb(lv_event_t *event) {
  (void)event;
  g_sync_requested = true;
  set_status("Sincronizando");
}

static void make_page_dots(lv_obj_t *screen, PageIndex active) {
  for (int i = 0; i < 5; ++i) {
    lv_obj_t *dot = lv_obj_create(screen);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, i == active ? 12 : 5, 5);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot,
      i == active ? color_hex(COLOR_TEXT) : color_hex(COLOR_TRACK), 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, (i - 2) * 11, -7);
  }
}

static void configure_arc(lv_obj_t *arc, int size) {
  lv_obj_set_size(arc, size, size);
  lv_arc_set_rotation(arc, 270);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(arc);
  lv_obj_set_style_arc_width(arc, 7, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, color_hex(COLOR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
}

static lv_obj_t *make_model_ring(lv_obj_t *parent, int size,
                                 uint32_t track_color) {
  lv_obj_t *arc = lv_arc_create(parent);
  configure_arc(arc, size);
  lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, color_hex(track_color), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
  return arc;
}

static lv_obj_t *make_diagonal_reset_label(lv_obj_t *parent) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_remove_style_all(label);
  lv_label_set_text(label, "");
  // 20px aumenta o peso aparente com a fonte compacta disponível no C3,
  // sem usar outline/glow que agravaria o halo visual.
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label, color_hex(COLOR_TEXT), 0);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}

static void set_diagonal_reset_label_one(lv_obj_t *label, const char *text,
                                         uint32_t color, float angle,
                                         int x_bias) {
  constexpr float R_DIAG = 92.0f;
  constexpr float ANGLE_TO_RADIANS = 0.01745329252f;
  float radians = angle * ANGLE_TO_RADIANS;
  int offset_x = (int)lroundf(cosf(radians) * R_DIAG) + x_bias;
  int offset_y = (int)lroundf(sinf(radians) * R_DIAG);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color_hex(color), 0);
  lv_obj_align(label, LV_ALIGN_CENTER, offset_x, offset_y);
  if (text[0]) lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
}

static void set_diagonal_reset_label(lv_obj_t *label, lv_obj_t *weight,
                                     const char *text, uint32_t color,
                                     float angle) {
  set_diagonal_reset_label_one(label, text, color, angle, 0);
  // O deslocamento de 1px na horizontal engrossa os glyphs sem sombra difusa.
  set_diagonal_reset_label_one(weight, text, color, angle, 1);
}

static void make_summary_model_dots(lv_obj_t *screen) {
  const int offsets[] = {-14, -5, 5, 14};
  for (int i = 0; i < 4; ++i) {
    lv_obj_t *dot = lv_obj_create(screen);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, i == 0 ? 8 : 4, 4);
    lv_obj_set_style_radius(dot, 2, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, offsets[i], -2);
    g_summary.model_dots[i] = dot;
  }
}

static void update_summary_reset_labels() {
  static const uint32_t model_colors[] = {
    COLOR_CLAUDE, COLOR_CODEX, COLOR_GROK, COLOR_GEMINI
  };
  static const char *window_text[] = {
    "5H",
    "5H",
    "7D",
    "7D"
  };
  static const char *model_names[] = {"CLAUDE", "CODEX", "GROK", "GEMINI"};

  uint32_t now = millis();
  if (!g_last_reset_model_switch_ms)
    g_last_reset_model_switch_ms = now;
  else if (now - g_last_reset_model_switch_ms >= SUMMARY_MODEL_SWITCH_MS) {
    g_active_reset_model = (g_active_reset_model + 1) % 4;
    g_last_reset_model_switch_ms = now;
  }

  int reset_min = -1;
  int active_pct = 0;
  switch (g_active_reset_model) {
    case 0:
      reset_min = g_quota.claude.session_reset_min;
      active_pct = g_quota.claude.session_pct;
      break;
    case 1:
      reset_min = g_quota.codex.session_reset_min;
      active_pct = g_quota.codex.session_pct;
      break;
    case 2:
      reset_min = g_quota.grok.reset_min;
      active_pct = g_quota.grok.used_pct;
      break;
    case 3:
      reset_min = g_quota.gemini.reset_min;
      active_pct = g_quota.gemini.used_pct;
      break;
  }
  char percent[12];
  snprintf(percent, sizeof(percent), "%d%%", active_pct);
  lv_label_set_text(g_summary.label_active_name, model_names[g_active_reset_model]);
  lv_label_set_text(g_summary.label_active_name_weight,
                    model_names[g_active_reset_model]);
  lv_label_set_text(g_summary.label_active_pct, percent);
  lv_obj_set_style_text_color(g_summary.label_active_name,
                              color_hex(model_colors[g_active_reset_model]), 0);
  lv_obj_set_style_text_color(g_summary.label_active_name_weight,
                              color_hex(model_colors[g_active_reset_model]), 0);
  char duration[20];
  format_duration(countdown_minutes(reset_min), duration, sizeof(duration));
  set_diagonal_reset_label(g_summary.label_reset_nw,
                           g_summary.label_reset_nw_weight,
                           window_text[g_active_reset_model],
                           model_colors[g_active_reset_model], -135.0f);
  set_diagonal_reset_label(g_summary.label_reset_ne,
                           g_summary.label_reset_ne_weight, duration,
                           COLOR_RESET_TEXT, -45.0f);

  // Claude e Codex também têm a janela semanal; Grok e Gemini deixam os
  // dois vãos inferiores livres para não criar ruído visual.
  const char *week_text = "";
  int week_reset_min = -1;
  if (g_active_reset_model == 0 || g_active_reset_model == 1) {
    week_text = "7 DIAS";
    week_reset_min = g_active_reset_model == 0
      ? g_quota.claude.week_reset_min : g_quota.codex.week_reset_min;
  }
  format_duration(countdown_minutes(week_reset_min), duration, sizeof(duration));
  set_diagonal_reset_label(g_summary.label_reset_sw,
                           g_summary.label_reset_sw_weight, week_text,
                           model_colors[g_active_reset_model], 135.0f);
  set_diagonal_reset_label(g_summary.label_reset_se,
                           g_summary.label_reset_se_weight,
                           week_text[0] ? duration : "",
                           COLOR_RESET_TEXT, 45.0f);

  const int dot_offsets[] = {-14, -5, 5, 14};
  for (int i = 0; i < 4; ++i) {
    bool active = i == g_active_reset_model;
    lv_obj_set_size(g_summary.model_dots[i], active ? 8 : 4, 4);
    lv_obj_align(g_summary.model_dots[i], LV_ALIGN_BOTTOM_MID,
                 dot_offsets[i], -2);
    lv_obj_set_style_bg_color(g_summary.model_dots[i],
      active ? color_hex(model_colors[i]) : color_hex(COLOR_TRACK), 0);
  }

  // O anel do modelo em foco permanece cheio; os demais recuam para 60%
  // de intensidade sem alterar os dados nem a espessura das trilhas.
  lv_obj_t *rings[] = {
    g_summary.arc_claude, g_summary.arc_codex,
    g_summary.arc_grok, g_summary.arc_gemini
  };
  for (int i = 0; i < 4; ++i) {
    uint32_t ring_color = i == g_active_reset_model
      ? model_colors[i] : soften_color(model_colors[i]);
    lv_obj_set_style_arc_color(rings[i], color_hex(ring_color),
                               LV_PART_INDICATOR);
  }
}

static void build_summary_screen() {
  g_summary.screen = lv_obj_create(NULL);
  style_screen(g_summary.screen);

  // Tela de atividade pura: quatro anéis concêntricos, sem números ou rótulos.
  // A identidade vem da cor; os dados ficam nas telas detalhadas.
  // Oito pixels a menos preservam uma faixa externa limpa para rótulos e
  // resets, sem sacrificar a leitura dos quatro anéis.
  g_summary.arc_claude = make_model_ring(
    g_summary.screen, 176, COLOR_CLAUDE_TRACK);
  g_summary.arc_codex = make_model_ring(
    g_summary.screen, 144, COLOR_CODEX_TRACK);
  g_summary.arc_grok = make_model_ring(
    g_summary.screen, 112, COLOR_GROK_TRACK);
  g_summary.arc_gemini = make_model_ring(
    g_summary.screen, 80, COLOR_GEMINI_TRACK);

  g_summary.mascot_claude = NULL;
  g_summary.mascot_codex = NULL;
  g_summary.label_claude = NULL;
  g_summary.label_claude_pct = NULL;
  g_summary.label_claude_reset = NULL;
  g_summary.label_codex = NULL;
  g_summary.label_codex_pct = NULL;
  g_summary.label_codex_reset = NULL;
  g_summary.label_gemini_week = NULL;
  g_summary.label_gemini_pct = make_label(
    g_summary.screen, "75%", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_LEFT_MID, 2, 0);
  g_summary.label_gemini_reset = NULL;
  g_summary.label_grok_week = NULL;
  g_summary.label_grok_pct = make_label(
    g_summary.screen, "50%", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_BOTTOM_MID, 0, -12);
  g_summary.label_grok_reset = NULL;
  g_summary.label_claude_pct = make_label(
    g_summary.screen, "100%", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_TOP_MID, 0, 2);
  g_summary.label_codex_pct = make_label(
    g_summary.screen, "25%", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_RIGHT_MID, -2, 0);
  g_summary.label_status = NULL;
  g_summary.label_reset_nw = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_reset_ne = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_reset_sw = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_reset_se = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_reset_nw_weight = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_reset_ne_weight = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_reset_sw_weight = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_reset_se_weight = make_diagonal_reset_label(g_summary.screen);
  g_summary.label_active_name = make_label(
    g_summary.screen, "CLAUDE", COLOR_CLAUDE, &lv_font_montserrat_20,
    LV_ALIGN_CENTER, 0, -10);
  g_summary.label_active_name_weight = make_label(
    g_summary.screen, "CLAUDE", COLOR_CLAUDE, &lv_font_montserrat_20,
    LV_ALIGN_CENTER, 1, -10);
  g_summary.label_active_pct = make_label(
    g_summary.screen, "0%", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_CENTER, 0, 12);
  make_summary_model_dots(g_summary.screen);
  update_summary_reset_labels();
}

static void build_detail_screen(DetailUI &ui, PageIndex page, const char *name,
                                bool is_claude,
                                uint32_t brand_color) {
  (void)is_claude;
  ui.screen = lv_obj_create(NULL);
  style_screen(ui.screen);
  ui.brand_color = brand_color;
  ui.mascot = NULL;
  ui.blink_left = NULL;
  ui.blink_right = NULL;
  ui.sweat = NULL;
  ui.bar_session = NULL;
  ui.bar_week = NULL;
  ui.label_insight = NULL;
  ui.label_status = NULL;

  make_label(ui.screen, name, brand_color, &lv_font_montserrat_20,
             LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *session_panel = make_card(ui.screen, 55);
  lv_obj_set_size(session_panel, 190, 66);
  make_label(session_panel, "5 HORAS", brand_color, &lv_font_montserrat_12,
             LV_ALIGN_TOP_LEFT, 14, 9);
  ui.label_session_pct = make_label(
    session_panel, "--%", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_TOP_LEFT, 14, 28);
  make_label(session_panel, "REINICIA", COLOR_MUTED, &lv_font_montserrat_12,
             LV_ALIGN_TOP_RIGHT, -14, 9);
  ui.label_session_reset = make_label(
    session_panel, "--", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_TOP_RIGHT, -14, 28);

  lv_obj_t *week_panel = make_card(ui.screen, 128);
  lv_obj_set_size(week_panel, 190, 66);
  make_label(week_panel, "7 DIAS", brand_color, &lv_font_montserrat_12,
             LV_ALIGN_TOP_LEFT, 14, 9);
  ui.label_week_pct = make_label(
    week_panel, "--%", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_TOP_LEFT, 14, 28);
  make_label(week_panel, "REINICIA", COLOR_MUTED, &lv_font_montserrat_12,
             LV_ALIGN_TOP_RIGHT, -14, 9);
  ui.label_week_reset = make_label(
    week_panel, "--", COLOR_TEXT, &lv_font_montserrat_20,
    LV_ALIGN_TOP_RIGHT, -14, 28);
  make_page_dots(ui.screen, page);
}

static void build_weekly_screen(WeeklyUI &ui, PageIndex page, const char *name,
                                uint32_t brand_color);

static void build_ui() {
  build_detail_screen(g_claude_ui, PAGE_CLAUDE, "CLAUDE", true, COLOR_CLAUDE);
  build_summary_screen();
  build_detail_screen(g_codex_ui, PAGE_CODEX, "CODEX", false, COLOR_CODEX);
  build_weekly_screen(g_gemini_ui, PAGE_GEMINI, "GEMINI", COLOR_GEMINI);
  build_weekly_screen(g_grok_ui, PAGE_GROK, "GROK", COLOR_GROK);
  lv_screen_load(g_summary.screen);
}

static lv_obj_t *screen_for_page(PageIndex page);

static void extra_back(lv_event_t *) {
  g_extra_mode = 0;
  lv_screen_load(screen_for_page(g_current_page));
}

static void add_action(lv_obj_t *screen, const char *text, int x, int y, int width, intptr_t mode) {
  lv_obj_t *button = lv_button_create(screen);
  lv_obj_set_size(button, width, 30);
  lv_obj_align(button, LV_ALIGN_TOP_MID, x, y);
  lv_obj_set_style_bg_color(button, color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_border_color(button, color_hex(COLOR_SURFACE_ALT), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_radius(button, 15, 0);
  make_label(button, text, COLOR_TEXT, &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(button, open_extra, LV_EVENT_CLICKED, (void *)mode);
}

static void build_extra() {
  g_extra_screen = lv_obj_create(NULL);
  style_screen(g_extra_screen);
  g_extra_title = make_label(g_extra_screen, "", COLOR_TEXT,
    &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 28);
  g_extra_text = make_label(g_extra_screen, "", COLOR_TEXT,
    &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 112);
  lv_obj_set_width(g_extra_text, 180);
  lv_obj_set_style_text_align(g_extra_text, LV_TEXT_ALIGN_CENTER, 0);
  for (int i = 0; i < 24; ++i) {
    g_trend_columns[i] = make_visible_pixel(g_extra_screen, 36 + i * 7, 100, 5, 1, COLOR_MUTED);
  }
  lv_obj_t *back = lv_button_create(g_extra_screen);
  lv_obj_set_size(back, 100, 32);
  lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 193);
  make_label(back, "Voltar", COLOR_TEXT, &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(back, extra_back, LV_EVENT_CLICKED, NULL);
}

static void build_weekly_screen(WeeklyUI &ui, PageIndex page, const char *name,
                                uint32_t brand_color) {
  ui.screen = lv_obj_create(NULL);
  ui.brand_color = brand_color;
  style_screen(ui.screen);
  ui.mascot = NULL;
  ui.bar_week = NULL;
  ui.label_remaining = NULL;
  ui.label_source = NULL;
  ui.label_status = NULL;

  make_label(ui.screen, name, brand_color, &lv_font_montserrat_20,
             LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *panel = make_card(ui.screen, 72);
  lv_obj_set_size(panel, 190, 98);
  make_label(panel, "SEMANAL", brand_color, &lv_font_montserrat_12,
             LV_ALIGN_TOP_LEFT, 16, 12);
  ui.label_used = make_label(panel, "--%", COLOR_TEXT, &lv_font_montserrat_20,
                             LV_ALIGN_TOP_LEFT, 16, 37);
  make_label(panel, "REINICIA", COLOR_MUTED, &lv_font_montserrat_12,
             LV_ALIGN_TOP_RIGHT, -16, 12);
  ui.label_reset = make_label(panel, "--", COLOR_TEXT, &lv_font_montserrat_20,
                              LV_ALIGN_TOP_RIGHT, -16, 37);
  make_page_dots(ui.screen, page);
}

static void open_extra(lv_event_t *event) {
  g_extra_mode = (intptr_t)lv_event_get_user_data(event);
  refresh_extra();
  lv_screen_load(g_extra_screen);
}

static void refresh_extra() {
  if (!g_extra_mode) return;
  char text[256];
  for (int i = 0; i < 24; ++i)
    lv_obj_add_flag(g_trend_columns[i], LV_OBJ_FLAG_HIDDEN);
  if (g_extra_mode == 3) {
    lv_label_set_text(g_extra_title, "CONEXAO");
    uint32_t epoch = g_server_epoch + (millis() - g_quota_received_ms) / 1000;
    snprintf(text, sizeof(text), "Wi-Fi: %s\nColetor: %s\nClaude: %s (%lus)\nCodex: %s (%lus)",
      WiFi.status() == WL_CONNECTED ? "conectado" : "reconectando",
      !g_last_contact_ms ? "aguardando" : millis() - g_last_contact_ms > 45000 ? "offline" : "online",
      g_quota.claude.source, (unsigned long)(epoch >= g_quota.claude.observed_epoch ? epoch - g_quota.claude.observed_epoch : 0),
      g_quota.codex.source, (unsigned long)(epoch >= g_quota.codex.observed_epoch ? epoch - g_quota.codex.observed_epoch : 0));
    lv_obj_align(g_extra_text, LV_ALIGN_TOP_MID, 0, 65);
  } else {
    auto &q = g_extra_mode == 1 ? g_quota.claude : g_quota.codex;
    lv_label_set_text(g_extra_title, g_extra_mode == 1 ? "RITMO CLAUDE - 5H" : "RITMO CODEX - 5H");
    bool has_points = false;
    for (int i = 0; i < 24; ++i) {
      if (!g_quota.valido || q.trend[i] < 0) continue;
      has_points = true;
      int h = max(1, q.trend[i] * 48 / 100);
      lv_obj_set_size(g_trend_columns[i], 5, h);
      lv_obj_set_pos(g_trend_columns[i], 36 + i * 7, 103 - h);
      lv_obj_remove_flag(g_trend_columns[i], LV_OBJ_FLAG_HIDDEN);
    }
    char duration[20];
    format_duration(countdown_minutes(q.session_reset_min), duration, sizeof(duration));
    if (!g_quota.valido || !has_points)
      snprintf(text, sizeof(text), "Aprendendo o ritmo\nReinicia: %s", duration);
    else if (q.projection_min >= 0)
      snprintf(text, sizeof(text), "Estimativa: limite em %dm\nReinicia: %s", countdown_minutes(q.projection_min), duration);
    else if (q.projected_pct_at_reset >= 0)
      snprintf(text, sizeof(text), "Estimativa no reset: %d%%\nReinicia: %s", q.projected_pct_at_reset, duration);
    else snprintf(text, sizeof(text), "Historico observado\nReinicia: %s\nProjecao aprendendo", duration);
    lv_obj_align(g_extra_text, LV_ALIGN_TOP_MID, 0, 116);
  }
  lv_label_set_text(g_extra_text, text);
}

// --------------------------------------------------------------------------
// Formatação e atualização da UI
// --------------------------------------------------------------------------

static int countdown_minutes(int base_minutes) {
  if (base_minutes < 0 || !g_quota.valido) return -1;
  uint32_t elapsed_minutes = (millis() - g_quota_received_ms) / 60000UL;
  return max(0, base_minutes - (int)elapsed_minutes);
}

static void format_duration(int minutes, char *buffer, size_t size) {
  if (minutes < 0) {
    snprintf(buffer, size, "--");
  } else if (minutes < 60) {
    snprintf(buffer, size, "%dm", minutes);
  } else if (minutes < 1440) {
    snprintf(buffer, size, "%dh%02dm", minutes / 60, minutes % 60);
  } else {
    snprintf(buffer, size, "%dd%02dh", minutes / 1440, (minutes % 1440) / 60);
  }
}

static void update_summary_weekly_row(lv_obj_t *value_label,
                                      lv_obj_t *reset_label,
                                      const WeeklyQuota &quota) {
  char value[16];
  char reset[24];
  if (!quota.available) {
    snprintf(value, sizeof(value), "--");
    snprintf(reset, sizeof(reset), "aguarda");
  } else if (quota.token_only) {
    char tokens[24];
    format_tokens(quota.week_tokens, tokens, sizeof(tokens));
    snprintf(value, sizeof(value), "--");
    snprintf(reset, sizeof(reset), "%s", tokens);
  } else if (quota.remaining_pct < 0) {
    snprintf(value, sizeof(value), "--");
    snprintf(reset, sizeof(reset), "aguarda");
  } else {
    snprintf(value, sizeof(value), "%d%%", quota.used_pct);
    if (quota.stale) {
      snprintf(reset, sizeof(reset), "antigo");
    } else {
      format_duration(countdown_minutes(quota.reset_min), reset, sizeof(reset));
    }
  }
  lv_label_set_text(value_label, value);
  lv_label_set_text(reset_label, reset);
}

static void update_weekly_summary() {
  if (!g_summary.label_gemini_pct || !g_summary.label_gemini_reset ||
      !g_summary.label_grok_pct || !g_summary.label_grok_reset) return;
  update_summary_weekly_row(
    g_summary.label_gemini_pct, g_summary.label_gemini_reset,
    g_quota.gemini);
  update_summary_weekly_row(
    g_summary.label_grok_pct, g_summary.label_grok_reset,
    g_quota.grok);
}

static void update_weekly_detail(WeeklyUI &ui, const WeeklyQuota &quota,
                                 const char *provider) {
  (void)provider;
  char text[32];
  if (!quota.available || quota.token_only || quota.remaining_pct < 0) {
    lv_label_set_text(ui.label_used, "--");
    lv_label_set_text(ui.label_reset, "--");
    return;
  }

  snprintf(text, sizeof(text), "%d%%", quota.used_pct);
  lv_label_set_text(ui.label_used, text);
  if (quota.reset_min < 0) {
    lv_label_set_text(ui.label_reset, "--");
  } else {
    char duration[20];
    format_duration(countdown_minutes(quota.reset_min), duration, sizeof(duration));
    lv_label_set_text(ui.label_reset, duration);
  }
}

static void format_tokens(uint64_t tokens, char *buffer, size_t size) {
  if (tokens >= 1000000ULL) {
    snprintf(buffer, size, "%llu.%lluM tok",
      tokens / 1000000ULL, (tokens % 1000000ULL) / 100000ULL);
  } else if (tokens >= 1000ULL) {
    snprintf(buffer, size, "%lluk tok", tokens / 1000ULL);
  } else if (tokens > 0) {
    snprintf(buffer, size, "%llu tok", tokens);
  } else {
    snprintf(buffer, size, "tokens --");
  }
}

static void set_reset_label(lv_obj_t *label, int base_minutes) {
  if (base_minutes < 0) {
    lv_label_set_text(label, "--");
    return;
  }
  if (countdown_minutes(base_minutes) == 0) {
    lv_label_set_text(label, "aguarda");
    return;
  }
  char duration[20];
  format_duration(countdown_minutes(base_minutes), duration, sizeof(duration));
  lv_label_set_text(label, duration);
}

static void update_detail(DetailUI &ui, const ModelQuota &quota) {
  char pct[12];
  snprintf(pct, sizeof(pct), "%d%%", quota.session_pct);
  lv_label_set_text(ui.label_session_pct, pct);
  snprintf(pct, sizeof(pct), "%d%%", quota.week_pct);
  lv_label_set_text(ui.label_week_pct, pct);

  if (ui.bar_session) {
    lv_bar_set_value(ui.bar_session, quota.session_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(ui.bar_session, color_hex(ui.brand_color), LV_PART_INDICATOR);
  }
  if (ui.bar_week) {
    lv_bar_set_value(ui.bar_week, quota.week_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(ui.bar_week, color_hex(ui.brand_color), LV_PART_INDICATOR);
  }

  if (quota.session_pct == 0 && quota.session_reset_min < 0) {
    lv_label_set_text(ui.label_session_reset, "livre");
  } else {
    set_reset_label(ui.label_session_reset, quota.session_reset_min);
  }
  set_reset_label(ui.label_week_reset, quota.week_reset_min);

  char tokens[24];
  char insight[64];
  format_tokens(quota.session_tokens, tokens, sizeof(tokens));
  if (quota.projection_min >= 0) {
    char duration[20];
    format_duration(countdown_minutes(quota.projection_min), duration, sizeof(duration));
    snprintf(insight, sizeof(insight), "%s | esgota %s", tokens, duration);
  } else if (quota.projected_pct_at_reset >= 0) {
    snprintf(insight, sizeof(insight), "%s | fecha ~%d%%",
             tokens, quota.projected_pct_at_reset);
  } else {
    snprintf(insight, sizeof(insight), "%s | projecao aprendendo", tokens);
  }
  if (ui.label_insight) lv_label_set_text(ui.label_insight, insight);

  if (ui.sweat && max(quota.session_pct, quota.week_pct) > 65) {
    lv_obj_remove_flag(ui.sweat, LV_OBJ_FLAG_HIDDEN);
  } else if (ui.sweat) {
    lv_obj_add_flag(ui.sweat, LV_OBJ_FLAG_HIDDEN);
  }
}

static void update_countdowns() {
  if (!g_quota.valido) return;

  char text[32];
  char duration[20];

  format_duration(countdown_minutes(g_quota.claude.session_reset_min), duration, sizeof(duration));
  if (g_quota.claude.session_pct == 0 && g_quota.claude.session_reset_min < 0)
    snprintf(text, sizeof(text), "livre");
  else if (g_quota.claude.session_reset_min < 0)
    snprintf(text, sizeof(text), "--");
  else
    snprintf(text, sizeof(text), "%s", duration);
  if (g_summary.label_claude_reset)
    lv_label_set_text(g_summary.label_claude_reset, text);

  format_duration(countdown_minutes(g_quota.codex.session_reset_min), duration, sizeof(duration));
  if (g_quota.codex.session_pct == 0 && g_quota.codex.session_reset_min < 0)
    snprintf(text, sizeof(text), "livre");
  else if (g_quota.codex.session_reset_min < 0)
    snprintf(text, sizeof(text), "--");
  else
    snprintf(text, sizeof(text), "%s", duration);
  if (g_summary.label_codex_reset)
    lv_label_set_text(g_summary.label_codex_reset, text);

  set_reset_label(g_claude_ui.label_session_reset, g_quota.claude.session_reset_min);
  set_reset_label(g_claude_ui.label_week_reset, g_quota.claude.week_reset_min);
  set_reset_label(g_codex_ui.label_session_reset, g_quota.codex.session_reset_min);
  set_reset_label(g_codex_ui.label_week_reset, g_quota.codex.week_reset_min);
  update_weekly_detail(g_gemini_ui, g_quota.gemini, "Gemini");
  update_weekly_detail(g_grok_ui, g_quota.grok, "Grok");
  update_weekly_summary();
}

static void update_ui() {
  if (!g_quota.valido) return;

  lv_arc_set_value(g_summary.arc_claude, g_quota.claude.session_pct);
  lv_obj_set_style_arc_color(g_summary.arc_claude,
                             color_hex(COLOR_CLAUDE), LV_PART_INDICATOR);
  lv_arc_set_value(g_summary.arc_codex, g_quota.codex.session_pct);
  lv_obj_set_style_arc_color(g_summary.arc_codex,
                             color_hex(COLOR_CODEX), LV_PART_INDICATOR);
  lv_arc_set_value(g_summary.arc_gemini,
                   g_quota.gemini.available ? g_quota.gemini.used_pct : 0);
  lv_obj_set_style_arc_color(g_summary.arc_gemini,
                             g_quota.gemini.available
                               ? color_hex(COLOR_GEMINI)
                               : color_hex(COLOR_TRACK),
                             LV_PART_INDICATOR);
  lv_arc_set_value(g_summary.arc_grok,
                   g_quota.grok.available ? g_quota.grok.used_pct : 0);
  lv_obj_set_style_arc_color(g_summary.arc_grok,
                             g_quota.grok.available
                               ? color_hex(COLOR_GROK)
                               : color_hex(COLOR_TRACK),
                             LV_PART_INDICATOR);
  update_detail(g_claude_ui, g_quota.claude);
  update_detail(g_codex_ui, g_quota.codex);
  update_weekly_detail(g_gemini_ui, g_quota.gemini, "Gemini");
  update_weekly_detail(g_grok_ui, g_quota.grok, "Grok");
  update_countdowns();
  update_summary_reset_labels();
}

static void set_status(const char *message) {
  if (g_summary.label_status)
    lv_label_set_text(g_summary.label_status, message);
  if (g_claude_ui.label_status)
    lv_label_set_text(g_claude_ui.label_status, message);
  if (g_codex_ui.label_status)
    lv_label_set_text(g_codex_ui.label_status, message);
  if (g_gemini_ui.label_status)
    lv_label_set_text(g_gemini_ui.label_status, message);
  if (g_grok_ui.label_status)
    lv_label_set_text(g_grok_ui.label_status, message);
}

static int highest_crossed_threshold(int before, int after) {
  static const int thresholds[] = {25, 50, 66, 80, 100};
  int crossed = 0;
  for (int threshold : thresholds) {
    if (before < threshold && after >= threshold) crossed = threshold;
  }
  return crossed;
}

static void trigger_threshold_alert(const char *model, int threshold) {
  char message[40];
  const char *state = threshold >= 80 ? "CRITICO" :
                      threshold >= 66 ? "ATENCAO" :
                      threshold >= 50 ? "ACOMPANHE" : "ATIVO";
  snprintf(message, sizeof(message), "%s %d%% | %s", model, threshold, state);
  set_status(message);
  g_alert_until = millis() + 3500;
  Serial.print("Animacao: ");
  Serial.println(message);
}

static void detect_threshold_events() {
  int claude_peak = max(g_quota.claude.session_pct, g_quota.claude.week_pct);
  int codex_peak = max(g_quota.codex.session_pct, g_quota.codex.week_pct);
  if (!g_thresholds_initialized) {
    g_previous_claude_peak = claude_peak;
    g_previous_codex_peak = codex_peak;
    g_thresholds_initialized = true;
    return;
  }
  int claude_crossed = highest_crossed_threshold(g_previous_claude_peak, claude_peak);
  int codex_crossed = highest_crossed_threshold(g_previous_codex_peak, codex_peak);
  if (claude_crossed) trigger_threshold_alert("Claude", claude_crossed);
  if (codex_crossed) trigger_threshold_alert("Codex", codex_crossed);
  g_previous_claude_peak = claude_peak;
  g_previous_codex_peak = codex_peak;
}

static void animate_mascot(DetailUI &ui, const ModelQuota &quota, uint32_t now) {
  if (!ui.mascot) return;
  int peak = max(quota.session_pct, quota.week_pct);
  int x = ui.mascot_base_x;
  int y = ui.mascot_base_y;
  if (peak >= 100) {
    x += ((now / 70) % 3) - 1;
  } else if (peak >= 80) {
    x += ((now / 180) % 3) - 1;
  } else {
    y += ((now / 500) % 2);
  }
  if (x != ui.mascot_last_x || y != ui.mascot_last_y) {
    lv_obj_align(ui.mascot, LV_ALIGN_TOP_LEFT, x, y);
    ui.mascot_last_x = x;
    ui.mascot_last_y = y;
  }

  bool blink = peak < 66 && (now % 4000) < 130;
  if (blink != ui.blink_visible && ui.blink_left && ui.blink_right) {
    if (blink) {
      lv_obj_remove_flag(ui.blink_left, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(ui.blink_right, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui.blink_left, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui.blink_right, LV_OBJ_FLAG_HIDDEN);
    }
    ui.blink_visible = blink;
  }
}

static lv_obj_t *screen_for_page(PageIndex page) {
  if (page == PAGE_CLAUDE) return g_claude_ui.screen;
  if (page == PAGE_CODEX) return g_codex_ui.screen;
  if (page == PAGE_GEMINI) return g_gemini_ui.screen;
  if (page == PAGE_GROK) return g_grok_ui.screen;
  return g_summary.screen;
}

static void process_navigation() {
  if (g_extra_mode) {
    g_pending_page_delta = 0;
    g_pending_page_target = -1;
    return;
  }
  if (g_pending_page_target >= 0) {
    PageIndex target = (PageIndex)g_pending_page_target;
    g_pending_page_target = -1;
    g_pending_page_delta = 0;
    if ((int32_t)(millis() - g_navigation_locked_until) < 0 ||
        target == g_current_page) return;
    int delta = (int)target - (int)g_current_page;
    lv_screen_load_anim(
      screen_for_page(target),
      delta > 0 ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
      120, 0, false);
    g_current_page = target;
    g_navigation_locked_until = millis() + 170;
    return;
  }
  if (g_pending_page_delta == 0) return;
  if ((int32_t)(millis() - g_navigation_locked_until) < 0) {
    g_pending_page_delta = 0;
    return;
  }

  int next = constrain((int)g_current_page + g_pending_page_delta,
                       (int)PAGE_GROK, (int)PAGE_GEMINI);
  int delta = g_pending_page_delta;
  g_pending_page_delta = 0;
  if (next == g_current_page) return;

  lv_screen_load_anim(
    screen_for_page((PageIndex)next),
    delta > 0 ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
    90, 0, false);
  g_current_page = (PageIndex)next;
  g_navigation_locked_until = millis() + 150;
}

// --------------------------------------------------------------------------
// Display, Wi-Fi e HTTP
// --------------------------------------------------------------------------

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t width = area->x2 - area->x1 + 1;
  uint32_t height = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, width, height);
  lv_display_flush_ready(disp);
}

static void manage_wifi() {
  static bool was_connected = false;

  if (WiFi.status() == WL_CONNECTED) {
    if (!was_connected) {
      Serial.print("Wi-Fi conectado, IP: ");
      Serial.println(WiFi.localIP());
      was_connected = true;
      g_collector_ip = IPAddress();
      if (!g_mdns_started) {
        g_mdns_started = MDNS.begin("token-quota-display");
      }
      network_status("");
    }
    return;
  }

  if (was_connected) {
    g_collector_ip = IPAddress();
    if (g_mdns_started) MDNS.end();
    g_mdns_started = false;
  }
  was_connected = false;
  uint32_t now = millis();
  if (now - g_last_wifi_attempt_ms < WIFI_RETRY_INTERVAL_MS) return;
  g_last_wifi_attempt_ms = now;

  network_status("Reconectando Wi-Fi...");
  // Encerra a tentativa anterior antes de alternar de rede; sem isso o
  // driver pode ignorar um novo SSID enquanto ainda está em AUTH.
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static int json_int_or(JsonVariantConst value, int fallback) {
  return value.isNull() ? fallback : value.as<int>();
}

static bool fetch_quota_network(bool force_refresh) {
  if (WiFi.status() != WL_CONNECTED) {
    network_status("Sem Wi-Fi");
    return false;
  }

  if ((uint32_t)g_collector_ip == 0 && g_mdns_started) {
    g_collector_ip = MDNS.queryHost(QUOTA_SERVER_MDNS_HOST, 1500);
    if ((uint32_t)g_collector_ip != 0) {
      Serial.print("Coletor encontrado via mDNS: ");
      Serial.println(g_collector_ip);
    }
  }
  if ((uint32_t)g_collector_ip == 0) {
    g_collector_ip.fromString(QUOTA_SERVER_FALLBACK_IP);
    Serial.print("Usando IP fallback do coletor: ");
    Serial.println(g_collector_ip);
  }

  String quota_url = "http://" + g_collector_ip.toString() + ":8787/quota";
  if (force_refresh) quota_url += "?refresh=1";
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(quota_url)) {
    network_status("Erro HTTP");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("Erro HTTP: ");
    Serial.println(code);
    g_collector_ip = IPAddress();
    if (++g_http_failures >= 3) network_status("Coletor offline");
    http.end();
    return false;
  }

  // Evita String/getString a cada poll: em uso contínuo isso fragmenta a heap
  // do C3 e era uma fonte provável das desconexões após várias horas.
  int expected = http.getSize();
  if (expected <= 0 || expected >= (int)sizeof(g_network_result.body)) {
    http.end();
    network_status("Resposta invalida");
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  size_t used = 0;
  uint32_t last_byte_ms = millis();
  while (used < (size_t)expected && http.connected()) {
    size_t available = stream->available();
    if (!available) {
      if (millis() - last_byte_ms >= HTTP_TIMEOUT_MS) break;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    size_t space = sizeof(g_network_result.body) - 1 - used;
    size_t wanted = min(available, space);
    int read = stream->readBytes(g_network_result.body + used, wanted);
    if (read <= 0) break;
    used += (size_t)read;
    last_byte_ms = millis();
  }
  http.end();
  if (used != (size_t)expected) {
    network_status("Resposta incompleta");
    return false;
  }
  g_network_result.body[used] = 0;
  g_network_result.received = millis();
  return true;
}

static bool check_for_ota() {
  if (WiFi.status() != WL_CONNECTED || (uint32_t)g_collector_ip == 0) return false;

  String base_url = "http://" + g_collector_ip.toString();
  HTTPClient manifest_http;
  manifest_http.setTimeout(HTTP_TIMEOUT_MS);
  if (!manifest_http.begin(base_url + OTA_MANIFEST_PATH)) return false;
  int manifest_code = manifest_http.GET();
  if (manifest_code != HTTP_CODE_OK) {
    manifest_http.end();
    return false;
  }

  JsonDocument manifest;
  DeserializationError parse_error = deserializeJson(manifest, *manifest_http.getStreamPtr());
  manifest_http.end();
  if (parse_error) return false;

  uint32_t remote_version = manifest["version_code"] | 0UL;
  size_t expected_size = manifest["size"] | (size_t)0;
  const char *expected_md5 = manifest["md5"] | "";
  if (remote_version <= FIRMWARE_VERSION_CODE || expected_size == 0) return false;
  if (expected_size > 2U * 1024U * 1024U) return false;

  Serial.printf("OTA disponivel: %lu -> %lu (%u bytes)\n",
                (unsigned long)FIRMWARE_VERSION_CODE,
                (unsigned long)remote_version,
                (unsigned)expected_size);
  network_status("Atualizando firmware...");

  HTTPClient binary_http;
  binary_http.setTimeout(HTTP_TIMEOUT_MS);
  if (!binary_http.begin(base_url + OTA_BINARY_PATH)) return false;
  int binary_code = binary_http.GET();
  if (binary_code != HTTP_CODE_OK) {
    binary_http.end();
    return false;
  }
  int content_length = binary_http.getSize();
  if (content_length > 0 && (size_t)content_length != expected_size) {
    binary_http.end();
    return false;
  }
  if (!Update.begin(expected_size)) {
    binary_http.end();
    return false;
  }
  if (expected_md5[0] && strlen(expected_md5) == 32) {
    Update.setMD5(expected_md5);
  }
  WiFiClient *stream = binary_http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  bool finished = written == expected_size && Update.end(true) && Update.isFinished();
  binary_http.end();
  if (!finished) {
    Update.abort();
    network_status("OTA falhou");
    return false;
  }

  Serial.println("OTA concluido; reiniciando");
  delay(500);
  ESP.restart();
  return true;
}

static void network_task(void *) {
  for (;;) {
    uint8_t request = 0;
    bool manual = xQueueReceive(g_network_requests, &request, pdMS_TO_TICKS(100)) == pdTRUE;
    manage_wifi();
    if (manual || millis() - g_last_poll_ms >= POLL_INTERVAL_MS) {
      g_last_poll_ms = millis();
      g_network_result.body[0] = 0;
      g_network_result.status[0] = 0;
      bool received = fetch_quota_network(request > 1);
      xQueueOverwrite(g_network_results, &g_network_result);
      if (received && (!g_ota_checked_once ||
                       millis() - g_last_ota_check_ms >= OTA_CHECK_INTERVAL_MS)) {
        g_ota_checked_once = true;
        g_last_ota_check_ms = millis();
        check_for_ota();
      }
    }
  }
}

static bool apply_quota(const char *body, uint32_t received) {

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error || !doc["claude"].is<JsonObjectConst>() || !doc["codex"].is<JsonObjectConst>()) {
    set_status("JSON invalido");
    return false;
  }

  JsonObjectConst claude = doc["claude"];
  JsonObjectConst codex = doc["codex"];
  g_quota.claude.session_pct = constrain(json_int_or(claude["session_pct"], 0), 0, 100);
  g_quota.claude.week_pct = constrain(json_int_or(claude["week_pct"], 0), 0, 100);
  g_quota.claude.session_reset_min = json_int_or(
    claude["session_reset_min"], json_int_or(claude["reset_min"], -1));
  g_quota.claude.week_reset_min = json_int_or(claude["week_reset_min"], -1);
  g_quota.claude.projection_min = json_int_or(claude["projection_min"], -1);
  g_quota.claude.projected_pct_at_reset = json_int_or(
    claude["projected_pct_at_reset"], -1);
  g_quota.claude.session_tokens = claude["session_tokens"] | (uint64_t)0;
  g_quota.claude.week_tokens = claude["week_tokens"] | (uint64_t)0;

  g_quota.codex.session_pct = constrain(json_int_or(codex["session_pct"], 0), 0, 100);
  g_quota.codex.week_pct = constrain(json_int_or(codex["week_pct"], 0), 0, 100);
  g_quota.codex.session_reset_min = json_int_or(
    codex["session_reset_min"], json_int_or(codex["reset_min"], -1));
  g_quota.codex.week_reset_min = json_int_or(codex["week_reset_min"], -1);
  g_quota.codex.projection_min = json_int_or(codex["projection_min"], -1);
  g_quota.codex.projected_pct_at_reset = json_int_or(
    codex["projected_pct_at_reset"], -1);
  g_quota.codex.session_tokens = codex["session_tokens"] | (uint64_t)0;
  g_quota.codex.week_tokens = codex["week_tokens"] | (uint64_t)0;
  g_quota.updated_epoch = json_int_or(doc["updated"], 0);
  g_quota.valido = true;
  g_http_failures = 0;
  g_quota_received_ms = received;
  g_last_contact_ms = received;
  g_server_epoch = doc["server_epoch"] | g_quota.updated_epoch;
  JsonObjectConst models[] = {claude, codex};
  ModelQuota *targets[] = {&g_quota.claude, &g_quota.codex};
  for (int i = 0; i < 2; ++i) {
    auto &q = *targets[i];
    auto j = models[i];
    auto remaining = [&](const char *key, int fallback) {
      int64_t epoch = j[key] | (int64_t)0;
      return epoch > 0 ? (int)max((int64_t)0, (epoch - g_server_epoch + 59) / 60) : fallback;
    };
    q.session_reset_min = remaining("session_reset_epoch", q.session_reset_min);
    q.week_reset_min = remaining("week_reset_epoch", q.week_reset_min);
    q.observed_epoch = j["observed_epoch"] | g_quota.updated_epoch;
    snprintf(q.source, sizeof(q.source), "%s", j["source"] | "indisponivel");
    for (int k = 0; k < 24; ++k) q.trend[k] = j["trend"][k] | -1;
  }

  auto parse_weekly = [&](JsonVariantConst value, WeeklyQuota &target) {
    target = WeeklyQuota();
    if (!value.is<JsonObjectConst>()) return;
    JsonObjectConst weekly = value.as<JsonObjectConst>();
    target.used_pct = constrain(json_int_or(weekly["week_used_pct"],
                                            json_int_or(weekly["week_pct"], 0)), 0, 100);
    target.remaining_pct = json_int_or(weekly["week_remaining_pct"], -1);
    if (target.remaining_pct >= 0) target.remaining_pct = constrain(target.remaining_pct, 0, 100);
    target.reset_min = json_int_or(weekly["week_reset_min"], -1);
    target.reset_epoch = weekly["week_reset_epoch"] | (uint32_t)0;
    target.observed_epoch = weekly["observed_epoch"] | g_quota.updated_epoch;
    target.week_tokens = weekly["week_tokens"] | (uint64_t)0;
    target.available = weekly["available"] | false;
    target.stale = weekly["stale"] | true;
    target.token_only = weekly["token_only"] | false;
    snprintf(target.source, sizeof(target.source), "%s", weekly["source"] | "indisponivel");
    snprintf(target.plan_label, sizeof(target.plan_label), "%s", weekly["plan_label"] | "");
    int64_t epoch = weekly["week_reset_epoch"] | (int64_t)0;
    if (epoch > 0 && g_server_epoch > 0) {
      target.reset_min = (int)max((int64_t)0, (epoch - g_server_epoch + 59) / 60);
    }
  };
  parse_weekly(doc["gemini"], g_quota.gemini);
  parse_weekly(doc["grok"], g_quota.grok);

  set_status("");
  detect_threshold_events();
  Serial.printf("Quota -> Claude %d%%/%d%% | Codex %d%%/%d%% | Gemini %d%% | Grok %d%%\n",
    g_quota.claude.session_pct, g_quota.claude.week_pct,
    g_quota.codex.session_pct, g_quota.codex.week_pct,
    g_quota.gemini.used_pct, g_quota.grok.used_pct);
  return true;
}
// --------------------------------------------------------------------------
// Arduino lifecycle
// --------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);

  lv_init();
  lv_tick_set_cb(millis);
  g_display = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(g_display, lvgl_flush_cb);
  lv_display_set_buffers(g_display, g_lvgl_buffer, NULL,
                         sizeof(g_lvgl_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

  build_ui();
  build_extra();
  init_touch();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  g_network_results = xQueueCreate(1, sizeof(NetworkResult));
  g_network_requests = xQueueCreate(1, sizeof(uint8_t));
  if (!g_network_results || !g_network_requests ||
      xTaskCreate(network_task, "quota-network", 12288, NULL, 1, NULL) != pdPASS)
    set_status("Falha tarefa rede");
}

void loop() {
  lv_timer_handler();
  process_navigation();
  uint32_t now = millis();
  animate_mascot(g_claude_ui, g_quota.claude, now);
  animate_mascot(g_codex_ui, g_quota.codex, now);
  if (g_alert_until && (int32_t)(now - g_alert_until) >= 0) {
    g_alert_until = 0;
    if (WiFi.status() == WL_CONNECTED) set_status("");
  }
  if (now - g_last_countdown_refresh_ms >= COUNTDOWN_REFRESH_MS) {
    g_last_countdown_refresh_ms = now;
    update_countdowns();
    update_summary_reset_labels();
  }

  if (g_sync_requested) {
    g_sync_requested = false;
    // O valor 2 pede ao coletor uma atualização oficial do Antigravity antes
    // de devolver os dados; o toque continua leve e não bloqueia o LVGL.
    uint8_t request = 2;
    if (g_network_requests) xQueueOverwrite(g_network_requests, &request);
  }
  static NetworkResult result;
  if (g_network_results && xQueueReceive(g_network_results, &result, 0) == pdTRUE) {
    if (result.body[0] && apply_quota(result.body, result.received)) update_ui();
    else if (result.status[0]) set_status(result.status);
  }
  static uint32_t last_extra = 0;
  if (now - last_extra >= 1000) {
    last_extra = now;
    refresh_extra();
    if (g_last_contact_ms && now - g_last_contact_ms > 45000)
      set_status("Dados antigos");
  }

  delay(1);
}
