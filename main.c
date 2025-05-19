#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "lib/ssd1306.h"
#include "lib/font.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/bootrom.h" // Correção: Incluído corretamente

#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define OLED_ADDRESS 0x3C
#define ADC_WATER_LEVEL 26 // ADC0 para nível da água
#define ADC_RAIN_VOLUME 27 // ADC1 para volume de chuva
#define LED_RED 13         // LED vermelho
#define LED_GREEN 11       // LED verde
#define LED_BLUE 12        // LED azul
#define BUZZER_PIN_1 10    // Buzzer principal (PWM)
#define BUZZER_PIN_2 21    // Buzzer secundário (PWM)
#define BUTTON_B 6
#define BUZZER_FREQ 3000   // Frequência de 3 kHz para o buzzer
#define JOYSTICK_NEUTRAL 2039 // Valor neutro do joystick
#define JOYSTICK_ERROR_MARGIN 20 // Margem de erro do joystick (em unidades do ADC)
#define ALERT_THRESHOLD_WATER 2866 // 70% de 4095 (nível da água para entrar em alerta)
#define ALERT_THRESHOLD_RAIN 3276  // 80% de 4095 (volume de chuva para entrar em alerta)
#define MATRIX_PIN 7       // GPIO para a matriz WS2812 5x5
#define MATRIX_WIDTH 5     // Largura da matriz
#define MATRIX_HEIGHT 5    // Altura da matriz
#define MATRIX_SIZE (MATRIX_WIDTH * MATRIX_HEIGHT)

typedef struct {
    uint16_t water_level; // Nível da água (0-4095)
    uint16_t rain_volume; // Volume de chuva (0-4095)
} joystick_data_t; // Correção: Fechado corretamente o typedef

QueueHandle_t xQueueDisplayData;
QueueHandle_t xQueueAlertData;

// Função para converter o valor do ADC em volume de chuva (m³)
// ADC 0-4095 mapeado para 0-200 mm de precipitação; área de coleta = 100 m²
float adc_to_rain_volume(uint16_t adc_value) {
    // Mapeia 0-4095 para 0-200 mm
    float precipitation_mm = (float)adc_value * 200.0f / 4095.0f;
    // Área de coleta de 100 m²: 1 mm = 0.1 m³ (100 litros)
    float volume_m3 = precipitation_mm * 0.1f;
    return volume_m3;
}

// Função para converter o valor do ADC em nível de água (metros)
// ADC 0-4095 mapeado para 0-50 metros
float adc_to_water_level(uint16_t adc_value) {
    // Mapeia 0-4095 para 0-50 metros
    float level_m = (float)adc_value * 50.0f / 4095.0f;
    return level_m;
}

// Função para converter RGB para uint32_t no formato GRB
static inline uint32_t rgb_to_uint32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b; // Ordem GRB
}

// Função para inicializar e enviar dados à matriz WS2812
void matrix_init_and_set(PIO pio, uint sm, uint offset, uint pin, float freq) {
    ws2812_program_init(pio, sm, offset, pin, freq, false); // rgbw = false, usa ordem GRB
}

// Função para atualizar a matriz WS2812 com um array de cores
void update_matrix(PIO pio, uint sm, uint32_t *colors, int size) {
    for (int i = 0; i < size; i++) {
        pio_sm_put_blocking(pio, sm, colors[i] << 8u); // Envia cada cor com deslocamento
    }
}

// Função para desenhar um triângulo invertido na matriz
void draw_triangle(PIO pio, uint sm, bool on) {
    uint32_t yellow = rgb_to_uint32(10, 10, 0); // Amarelo para o triângulo
    uint32_t off = rgb_to_uint32(0, 0, 0);     // Desligado
    uint32_t matrix_colors[MATRIX_SIZE];        // Buffer para armazenar as cores dos LEDs

    // Preenche o buffer com LEDs desligados
    for (int i = 0; i < MATRIX_SIZE; i++) {
        matrix_colors[i] = off;
    }

    if (on) {
        // Desenha o triângulo invertido conforme o novo formato
        // Linha 1 (índices 5 a 9): 5, 6, 7, 8, 9
        matrix_colors[5] = yellow; matrix_colors[6] = yellow; matrix_colors[7] = yellow;
        matrix_colors[8] = yellow; matrix_colors[9] = yellow;
        // Linha 2 (índices 11 a 13): 11, 12, 13
        matrix_colors[11] = yellow; matrix_colors[12] = yellow; matrix_colors[13] = yellow;
        // Linha 3 (índice 17): 17
        matrix_colors[17] = yellow;
    }

    // Atualiza a matriz com o buffer
    update_matrix(pio, sm, matrix_colors, MATRIX_SIZE);
}

void set_volume_level(uint8_t level, uint slice1, uint slice2) {
    pwm_config config1 = pwm_get_default_config();
    pwm_config config2 = pwm_get_default_config();
    float divisor;

    switch (level) {
        case 1: // Normal (desligado)
            divisor = 300.0f; // Quase inaudível
            break;
        case 2: // Alerta
            divisor = 30.0f; // Volume alto
            break;
        default:
            divisor = 300.0f;
            break;
    }

    pwm_config_set_clkdiv(&config1, divisor);
    pwm_config_set_clkdiv(&config2, divisor);
    pwm_config_set_wrap(&config1, (125000000 / BUZZER_FREQ) - 1);
    pwm_config_set_wrap(&config2, (125000000 / BUZZER_FREQ) - 1);
    pwm_init(slice1, &config1, true);
    pwm_init(slice2, &config2, true);
}

void vJoystickTask(void *params) {
    adc_gpio_init(ADC_WATER_LEVEL);
    adc_gpio_init(ADC_RAIN_VOLUME);
    adc_init();

    joystick_data_t joydata;

    while (true) {
        adc_select_input(0); // ADC0 (GPIO 26)
        uint16_t raw_water_level = adc_read();
        // Aplica a margem de erro para water_level
        if (raw_water_level >= JOYSTICK_NEUTRAL - JOYSTICK_ERROR_MARGIN &&
            raw_water_level <= JOYSTICK_NEUTRAL + JOYSTICK_ERROR_MARGIN) {
            joydata.water_level = JOYSTICK_NEUTRAL; // Considera como neutro
        } else {
            joydata.water_level = raw_water_level;
        }

        adc_select_input(1); // ADC1 (GPIO 27)
        uint16_t raw_rain_volume = adc_read();
        // Aplica a margem de erro para rain_volume
        if (raw_rain_volume >= JOYSTICK_NEUTRAL - JOYSTICK_ERROR_MARGIN &&
            raw_rain_volume <= JOYSTICK_NEUTRAL + JOYSTICK_ERROR_MARGIN) {
            joydata.rain_volume = JOYSTICK_NEUTRAL; // Considera como neutro
        } else {
            joydata.rain_volume = raw_rain_volume;
        }

        // Envia os dados para ambas as filas
        xQueueSend(xQueueDisplayData, &joydata, 0); // Para vDisplayTask
        xQueueSend(xQueueAlertData, &joydata, 0);   // Para vAlertTask
        vTaskDelay(pdMS_TO_TICKS(50)); // 20 Hz
    }
}

void vDisplayTask(void *params) {
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    ssd1306_t ssd;
    ssd1306_init(&ssd, WIDTH, HEIGHT, false, OLED_ADDRESS, I2C_PORT);
    ssd1306_config(&ssd);
    ssd1306_send_data(&ssd);

    joystick_data_t joydata;

    while (true) {
        if (xQueueReceive(xQueueDisplayData, &joydata, portMAX_DELAY) == pdTRUE) {
            ssd1306_fill(&ssd, false); // Limpa a tela com cor fixa (false)
            // Desenha borda externa
            ssd1306_rect(&ssd, 3, 3, 122, 58, true, false);
            // Linha divisória horizontal
            ssd1306_hline(&ssd, 3, 125, 32, true);
            // Estado atual
            const char *estado;
            if (joydata.water_level >= ALERT_THRESHOLD_WATER || 
                joydata.rain_volume >= ALERT_THRESHOLD_RAIN) {
                estado = "Alerta";
            } else {
                estado = "Normal";
            }
            char estado_str[20];
            snprintf(estado_str, sizeof(estado_str), "ESTADO:");
            ssd1306_draw_string(&ssd, estado_str, 35, 10);
            snprintf(estado_str, sizeof(estado_str), "%s", estado);
            ssd1306_draw_string(&ssd, estado_str, 35, 22);
            // Valores dinâmicos convertidos
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "Agua: %.1f m", adc_to_water_level(joydata.water_level));
            ssd1306_draw_string(&ssd, buffer, 10, 38);
            snprintf(buffer, sizeof(buffer), "Chuva: %.1f m3", adc_to_rain_volume(joydata.rain_volume));
            ssd1306_draw_string(&ssd, buffer, 10, 48);
            ssd1306_send_data(&ssd);
        }
    }
}

void vAlertTask(void *params) {
    // Configura GPIO 10 como PWM
    gpio_set_function(BUZZER_PIN_1, GPIO_FUNC_PWM);
    uint slice1 = pwm_gpio_to_slice_num(BUZZER_PIN_1);
    pwm_config config1 = pwm_get_default_config();
    pwm_config_set_clkdiv(&config1, 300.0f); // Inicialmente baixo
    pwm_config_set_wrap(&config1, (125000000 / BUZZER_FREQ) - 1);
    pwm_init(slice1, &config1, true);

    // Configura GPIO 21 como PWM
    gpio_set_function(BUZZER_PIN_2, GPIO_FUNC_PWM);
    uint slice2 = pwm_gpio_to_slice_num(BUZZER_PIN_2);
    pwm_config config2 = pwm_get_default_config();
    pwm_config_set_clkdiv(&config2, 300.0f); // Inicialmente baixo
    pwm_config_set_wrap(&config2, (125000000 / BUZZER_FREQ) - 1);
    pwm_init(slice2, &config2, true);

    // Configura LEDs como saídas digitais
    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_init(LED_GREEN);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_init(LED_BLUE);
    gpio_set_dir(LED_BLUE, GPIO_OUT);

    // Configura matriz WS2812
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    matrix_init_and_set(pio, sm, offset, MATRIX_PIN, 800000); // 800 kHz

    joystick_data_t joydata;
    bool blink_state = false;
    while (true) {
        if (xQueueReceive(xQueueAlertData, &joydata, pdMS_TO_TICKS(20)) == pdTRUE) {
            // Controle dos LEDs RGB, matriz WS2812 e Buzzer
            if (joydata.water_level >= ALERT_THRESHOLD_WATER || 
                joydata.rain_volume >= ALERT_THRESHOLD_RAIN) { // Alerta
                gpio_put(LED_RED, 1);    // Liga vermelho
                gpio_put(LED_GREEN, 0);  // Desliga verde
                gpio_put(LED_BLUE, 0);   // Desliga azul
                draw_triangle(pio, sm, blink_state); // Desenha triângulo piscante
                set_volume_level(2, slice1, slice2); // Volume alto
                pwm_set_chan_level(slice1, PWM_CHAN_A, (125000000 / BUZZER_FREQ) * 0.9);
                pwm_set_chan_level(slice2, PWM_CHAN_A, (125000000 / BUZZER_FREQ) * 0.9);
                vTaskDelay(pdMS_TO_TICKS(200)); // Piscar a cada 200 ms
                pwm_set_chan_level(slice1, PWM_CHAN_A, 0);
                pwm_set_chan_level(slice2, PWM_CHAN_A, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                blink_state = !blink_state; // Alterna estado do piscar
            } else { // Normal
                gpio_put(LED_RED, 0);    // Desliga vermelho
                gpio_put(LED_GREEN, 0);  // Desliga verde
                gpio_put(LED_BLUE, 1);   // Liga azul
                draw_triangle(pio, sm, false); // Desliga triângulo
                set_volume_level(1, slice1, slice2); // Volume mínimo
                pwm_set_chan_level(slice1, PWM_CHAN_A, 0);
                pwm_set_chan_level(slice2, PWM_CHAN_A, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Modo BOOTSEL com botão B
void gpio_irq_handler(uint gpio, uint32_t events) {
    reset_usb_boot(0, 0);
}

int main() {
    gpio_init(BUTTON_B);
    gpio_set_dir(BUTTON_B, GPIO_IN);
    gpio_pull_up(BUTTON_B);
    gpio_set_irq_enabled_with_callback(BUTTON_B, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);

    stdio_init_all();

    xQueueDisplayData = xQueueCreate(5, sizeof(joystick_data_t));
    xQueueAlertData = xQueueCreate(5, sizeof(joystick_data_t));

    xTaskCreate(vJoystickTask, "Joystick Task", 256, NULL, 3, NULL); // Prioridade 3
    xTaskCreate(vDisplayTask, "Display Task", 512, NULL, 2, NULL);   // Prioridade 2
    xTaskCreate(vAlertTask, "Alert Task", 256, NULL, 2, NULL);       // Prioridade 2

    vTaskStartScheduler();
    while (1);
}
