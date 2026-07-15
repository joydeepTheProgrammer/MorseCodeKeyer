/* ============================================================================
 * ESP32 Morse Code — ESP-IDF Native API (Single File)
 * Target: ESP32 / ESP32-S3 / ESP32-C3
 * ============================================================================ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_err.h"

/* ───────────────────────────────────────────────────────────────────────────
   HARDWARE PINS
   ─────────────────────────────────────────────────────────────────────────── */
#define BTN_PIN         GPIO_NUM_4
#define LED_PIN         GPIO_NUM_2      /* Built-in LED on most dev boards */
#define BUZZER_PIN      GPIO_NUM_5
#define UART_PORT       UART_NUM_0
#define BUF_SIZE        1024

/* ───────────────────────────────────────────────────────────────────────────
   MORSE CODE TABLE
   ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    char letter;
    const char *code;
} morse_entry_t;

static const morse_entry_t morse_table[] = {
    {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},  {'E', "."},
    {'F', "..-."}, {'G', "--."},  {'H', "...."}, {'I', ".."},   {'J', ".---"},
    {'K', "-.-"},  {'L', ".-.."}, {'M', "--"},   {'N', "-."},   {'O', "---"},
    {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
    {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"}, {'Y', "-.--"},
    {'Z', "--.."},
    {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
    {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."},
    {'9', "----."}, {'0', "-----"},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'!', "-.-.--"},
    {' ', " "}
};

/* ───────────────────────────────────────────────────────────────────────────
   STATE
   ─────────────────────────────────────────────────────────────────────────── */
typedef enum {
    EVT_PRESS,
    EVT_RELEASE
} btn_event_type_t;

typedef struct {
    btn_event_type_t type;
    int64_t timestamp_us;
} btn_event_t;

/* Timing (set via set_wpm) */
static int dot_ms;
static int dash_ms;
static int element_gap_ms;
static int letter_gap_ms;
static int word_gap_ms;

/* Runtime state */
static QueueHandle_t btn_queue;
static SemaphoreHandle_t decode_mutex;
static char current_morse[16] = "";
static char decoded_msg[256] = "";
static int64_t last_event_us = 0;   /* debounce: last press OR release */

/* ───────────────────────────────────────────────────────────────────────────
   FORWARD DECLARATIONS
   ─────────────────────────────────────────────────────────────────────────── */
static void init_gpio(void);
static void init_uart(void);
static void init_buzzer(void);
static void set_wpm(int wpm);
static void send_morse(const char *code);
static void tone_out(int duration_ms);
static const char* char_to_morse(char c);
static char morse_to_char(const char *code);
static void decode_current(void);
static void process_cmd(const char *input);
static void print_msg(const char *msg);
static void IRAM_ATTR btn_isr(void *arg);
static void btn_task(void *pvParameters);
static void uart_task(void *pvParameters);

/* ============================================================================
   MAIN
   ============================================================================ */
void app_main(void)
{
    set_wpm(10);

    btn_queue = xQueueCreate(32, sizeof(btn_event_t));
    if (!btn_queue) {
        printf("Failed to create button queue\n");
        return;
    }

    decode_mutex = xSemaphoreCreateMutex();
    if (!decode_mutex) {
        printf("Failed to create mutex\n");
        return;
    }

    init_gpio();
    init_uart();
    init_buzzer();

    xTaskCreatePinnedToCore(btn_task, "btn_task", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(uart_task, "uart_task", 4096, NULL, 5, NULL, 0);

    print_msg("\n╔══════════════════════════════════════╗");
    print_msg("║     ESP32 MORSE CODE — ESP-IDF       ║");
    print_msg("╠══════════════════════════════════════╣");
    print_msg("║  SEND: Type text in Serial Monitor   ║");
    print_msg("║  READ: Tap button to input Morse     ║");
    print_msg("╚══════════════════════════════════════╝");
    print_msg("Commands: wpm=<5-40> | clear | help");
}

/* ============================================================================
   INITIALIZATION
   ============================================================================ */
static void init_gpio(void)
{
    gpio_config_t io_conf = {0};

    /* LED only — buzzer is managed by LEDC */
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LED_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(LED_PIN, 0);

    /* Button: input, pull-up, interrupt on any edge */
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_PIN, btn_isr, NULL));
}

static void init_uart(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void init_buzzer(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 800,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

/* ============================================================================
   TIMING
   ============================================================================ */
static void set_wpm(int wpm)
{
    if (wpm < 5)  wpm = 5;
    if (wpm > 40) wpm = 40;
    dot_ms = 1200 / wpm;          /* standard Paris formula */
    dash_ms = dot_ms * 3;
    element_gap_ms = dot_ms;
    letter_gap_ms = dot_ms * 3;
    word_gap_ms = dot_ms * 7;
}

/* ============================================================================
   INTERRUPT — Button
   ============================================================================ */
static void IRAM_ATTR btn_isr(void *arg)
{
    btn_event_t evt;
    evt.timestamp_us = esp_timer_get_time();

    int level = gpio_get_level(BTN_PIN);
    evt.type = (level == 0) ? EVT_PRESS : EVT_RELEASE;

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(btn_queue, &evt, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* ============================================================================
   BUTTON TASK — Decode Morse from taps
   ============================================================================ */
static void btn_task(void *pvParameters)
{
    btn_event_t evt;
    int64_t press_start_us = 0;

    while (1) {
        if (xQueueReceive(btn_queue, &evt, portMAX_DELAY) != pdTRUE)
            continue;

        /* Debounce: ignore events < 40 ms apart */
        if (evt.timestamp_us - last_event_us < 40000)
            continue;
        last_event_us = evt.timestamp_us;

        if (evt.type == EVT_PRESS) {
            press_start_us = evt.timestamp_us;
            gpio_set_level(LED_PIN, 1);          /* visual feedback */
        }
        else if (evt.type == EVT_RELEASE && press_start_us > 0) {
            int duration_ms = (int)((evt.timestamp_us - press_start_us) / 1000);
            press_start_us = 0;
            gpio_set_level(LED_PIN, 0);

            /* Classify dot vs dash */
            if (duration_ms < (dot_ms * 2)) {
                if (strlen(current_morse) < sizeof(current_morse) - 1)
                    strcat(current_morse, ".");
                uart_write_bytes(UART_PORT, ".", 1);
            } else {
                if (strlen(current_morse) < sizeof(current_morse) - 1)
                    strcat(current_morse, "-");
                uart_write_bytes(UART_PORT, "-", 1);
            }

            /* Wait for letter gap or next press */
            TickType_t letter_ticks = pdMS_TO_TICKS(letter_gap_ms);
            TickType_t word_extra_ticks = pdMS_TO_TICKS(word_gap_ms - letter_gap_ms);
            btn_event_t next;

            if (xQueueReceive(btn_queue, &next, letter_ticks) == pdTRUE) {
                /* Next event arrived before letter gap expired */
                if (next.timestamp_us - last_event_us < 40000)
                    continue;  /* debounce */
                last_event_us = next.timestamp_us;

                if (next.type == EVT_PRESS) {
                    press_start_us = next.timestamp_us;
                    gpio_set_level(LED_PIN, 1);
                    continue;  /* loop back to catch the release */
                }
                /* Spurious release — ignore and keep waiting */
            } else {
                /* Letter gap expired → decode */
                xSemaphoreTake(decode_mutex, portMAX_DELAY);
                decode_current();
                xSemaphoreGive(decode_mutex);

                /* Wait extra time to see if it's a word gap */
                if (xQueueReceive(btn_queue, &next, word_extra_ticks) == pdTRUE) {
                    if (next.timestamp_us - last_event_us < 40000)
                        continue;
                    last_event_us = next.timestamp_us;

                    if (next.type == EVT_PRESS) {
                        press_start_us = next.timestamp_us;
                        gpio_set_level(LED_PIN, 1);
                        continue;
                    }
                } else {
                    /* Word gap expired → add space */
                    xSemaphoreTake(decode_mutex, portMAX_DELAY);
                    size_t len = strlen(decoded_msg);
                    if (len > 0 && len < sizeof(decoded_msg) - 1 &&
                        decoded_msg[len - 1] != ' ') {
                        decoded_msg[len] = ' ';
                        decoded_msg[len + 1] = '\0';
                    }
                    xSemaphoreGive(decode_mutex);

                    print_msg("  [WORD]");
                    char buf[280];
                    snprintf(buf, sizeof(buf), "Message: %s", decoded_msg);
                    print_msg(buf);
                }
            }
        }
    }
}

static void decode_current(void)
{
    if (strlen(current_morse) == 0) return;

    char decoded = morse_to_char(current_morse);
    char buf[280];

    if (decoded != '?') {
        size_t len = strlen(decoded_msg);
        if (len < sizeof(decoded_msg) - 1) {
            decoded_msg[len] = decoded;
            decoded_msg[len + 1] = '\0';
        }
        snprintf(buf, sizeof(buf), " → %c | Message: %s", decoded, decoded_msg);
    } else {
        snprintf(buf, sizeof(buf), " → [?] | Message: %s", decoded_msg);
    }
    print_msg(buf);

    current_morse[0] = '\0';
}

/* ============================================================================
   UART TASK — Send text as Morse + handle commands
   ============================================================================ */
static void uart_task(void *pvParameters)
{
    uint8_t data[BUF_SIZE];
    char line_buf[BUF_SIZE];
    int line_pos = 0;

    while (1) {
        int len = uart_read_bytes(UART_PORT, data, BUF_SIZE - 1, pdMS_TO_TICKS(50));
        if (len <= 0) continue;

        for (int i = 0; i < len; i++) {
            char c = data[i];
            if (c == '\r' || c == '\n') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    process_cmd(line_buf);
                    line_pos = 0;
                }
            } else if (line_pos < BUF_SIZE - 1) {
                line_buf[line_pos++] = c;
            }
        }
    }
}

static void process_cmd(const char *input)
{
    char cmd[BUF_SIZE];
    strncpy(cmd, input, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    /* Trim leading whitespace */
    char *p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    /* Uppercase */
    for (int i = 0; p[i]; i++) {
        if (p[i] >= 'a' && p[i] <= 'z') p[i] -= 32;
    }

    /* Command: WPM= */
    if (strncmp(p, "WPM=", 4) == 0) {
        int wpm = atoi(p + 4);
        if (wpm >= 5 && wpm <= 40) {
            set_wpm(wpm);
            char buf[64];
            snprintf(buf, sizeof(buf), "Speed set to %d WPM", wpm);
            print_msg(buf);
        } else {
            print_msg("WPM must be 5-40");
        }
        return;
    }

    if (strcmp(p, "CLEAR") == 0) {
        xSemaphoreTake(decode_mutex, portMAX_DELAY);
        decoded_msg[0] = '\0';
        xSemaphoreGive(decode_mutex);
        print_msg("Decoded message cleared.");
        return;
    }

    if (strcmp(p, "HELP") == 0) {
        print_msg("\n┌────── MORSE CHART ──────┐");
        for (int i = 0; i < 26; i += 2) {
            char buf[64];
            snprintf(buf, sizeof(buf), "│ %c=%-6s  %c=%-6s │",
                     morse_table[i].letter, morse_table[i].code,
                     morse_table[i+1].letter, morse_table[i+1].code);
            print_msg(buf);
        }
        print_msg("└─────────────────────────┘");
        return;
    }

    /* Otherwise: transmit as Morse */
    char buf[256];
    snprintf(buf, sizeof(buf), "Sending: %s", p);
    print_msg(buf);

    for (int i = 0; p[i]; i++) {
        char c = p[i];
        if (c == ' ') {
            vTaskDelay(pdMS_TO_TICKS(word_gap_ms));
            uart_write_bytes(UART_PORT, " / ", 3);
        } else {
            const char *code = char_to_morse(c);
            if (code) {
                char out[32];
                snprintf(out, sizeof(out), "%c=%s ", c, code);
                uart_write_bytes(UART_PORT, out, strlen(out));
                send_morse(code);
                vTaskDelay(pdMS_TO_TICKS(letter_gap_ms));
            }
        }
    }
    print_msg(" [DONE]");
}

/* ============================================================================
   OUTPUT — LED + Buzzer
   ============================================================================ */
static void send_morse(const char *code)
{
    for (int i = 0; code[i] != '\0'; i++) {
        if (code[i] == '.') {
            tone_out(dot_ms);
        } else if (code[i] == '-') {
            tone_out(dash_ms);
        }
        if (code[i + 1] != '\0')
            vTaskDelay(pdMS_TO_TICKS(element_gap_ms));
    }
}

static void tone_out(int duration_ms)
{
    gpio_set_level(LED_PIN, 1);
    /* 50% duty on 13-bit = 4096 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4096);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    gpio_set_level(LED_PIN, 0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ============================================================================
   LOOKUP HELPERS
   ============================================================================ */
static const char* char_to_morse(char c)
{
    for (int i = 0; i < sizeof(morse_table) / sizeof(morse_entry_t); i++) {
        if (morse_table[i].letter == c) return morse_table[i].code;
    }
    return NULL;
}

static char morse_to_char(const char *code)
{
    for (int i = 0; i < sizeof(morse_table) / sizeof(morse_entry_t); i++) {
        if (strcmp(morse_table[i].code, code) == 0) return morse_table[i].letter;
    }
    return '?';
}

static void print_msg(const char *msg)
{
    uart_write_bytes(UART_PORT, msg, strlen(msg));
    uart_write_bytes(UART_PORT, "\r\n", 2);
}
