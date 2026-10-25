/**
 * lv_conf.h — configuração mínima do LVGL 9 para ESP32-C3 (4MB flash, sem PSRAM).
 *
 * Baseado no template oficial do LVGL, com os campos relevantes para este
 * projeto ajustados: profundidade de cor 16 bits (compatível com GC9A01 via
 * Arduino_GFX), fonte Montserrat 14 habilitada, e memória reduzida para
 * caber no heap livre do C3 (~200KB de RAM utilizável, sem PSRAM).
 *
 * Coloque este arquivo na raiz do sketch ou em Documents/Arduino/libraries/lv_conf.h
 * conforme a forma de instalação do LVGL usada na sua Arduino IDE.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/

// GC9A01 é RGB565 — 16 bits por pixel.
#define LV_COLOR_DEPTH 16

/*=========================
   MEMORY SETTINGS
 *=========================*/

// C3 sem PSRAM: usar alocador padrão do LVGL (não custom), heap pequeno.
#define LV_MEM_CUSTOM 0
#if LV_MEM_CUSTOM == 0
    // 48KB é suficiente para o buffer parcial de 240x40px (RGB565 = 19.200 bytes)
    // mais os objetos da UI (dois arcos, labels). Deixa folga no heap do C3
    // (~200KB livres) para Wi-Fi/HTTP/JSON, que também consomem heap.
    #define LV_MEM_SIZE (48U * 1024U)
#endif

// Buffer parcial: não alocar frame buffer completo (240*240*2 = 115.200 bytes
// estouraria a folga de RAM do C3 junto com pilha Wi-Fi). Usamos renderização
// parcial de 240x40px por chamada (ver firmware/token_quota_display.ino).
#define LV_MEM_POOL_EXPAND_SIZE 0

/*====================
   HAL SETTINGS
 *====================*/

#define LV_DEF_REFR_PERIOD 30 /* [ms] */
#define LV_DPI_DEF 130

/*=================
 * OPERATING SYSTEM
 *=================*/
#define LV_USE_OS LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1
    #define LV_DRAW_SW_SUPPORT_RGB565 1
    #define LV_DRAW_SW_SUPPORT_RGB888 0
    #define LV_DRAW_SW_SUPPORT_XRGB8888 0
    #define LV_DRAW_SW_SUPPORT_ARGB8888 0
    #define LV_DRAW_SW_SUPPORT_L8 0
    #define LV_DRAW_SW_SUPPORT_AL88 0
    #define LV_DRAW_SW_SUPPORT_A8 0

    #define LV_DRAW_SW_DRAW_UNIT_CNT 1
    #define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
    #define LV_USE_NEON 0
    #define LV_DRAW_SW_COMPLEX 1
    #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
    #define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
#endif

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

// Núcleos gráficos usados neste projeto: arco, barra, label e container.
#define LV_USE_ARC        1
#define LV_USE_LABEL      1
#define LV_USE_BAR        1
#define LV_USE_BTN        0
#define LV_USE_BTNMATRIX  0
#define LV_USE_CANVAS     0
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_IMG        0
#define LV_USE_LINE       0
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     0
#define LV_USE_SWITCH     0
#define LV_USE_TABLE      0
#define LV_USE_TEXTAREA   0

#define LV_USE_ANIMATION  1
#define LV_USE_SHADOW     0
#define LV_USE_BLEND_MODES 0
#define LV_USE_OPA_SCALE   1
#define LV_USE_IMG_TRANSFORM 0

// Widgets extras desligados para economizar flash (partição Minimal SPIFFS).
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_KEYBOARD   0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SCALE      0 /* depende de LV_USE_LINE, que está desligado — não usamos este widget */
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_QRCODE     0

/*==================
 * COMPILER SETTINGS
 *==================*/
#define LV_BIG_ENDIAN_SYSTEM 0
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_USE_LOG 0

/*-----------
 * Fonts
 *-----------*/

// Hierarquia tipográfica compacta para a tela circular de 240 px.
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*==================
 *   THEME USAGE
 *==================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 1
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif

#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO   0

/*==================
 * TEXT SETTINGS
 *==================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

#endif /* LV_CONF_H */
