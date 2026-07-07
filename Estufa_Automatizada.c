#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "aht20.h"
#include "bh1750_light_sensor.h"

// Display OLED — mesmo padrão usado no projeto do reservatório
#include "lib/config_display.h"

// ═══════════════════════════════════════════════════════════════════════════
//  PINOS
// ═══════════════════════════════════════════════════════════════════════════
#define SOIL_PIN    28      // GP28 = ADC2 — sensor de umidade do solo
#define BOMBA_PIN   18      // GP18 = Bomba d'água (relé)
#define BOTAO_A_PIN 5       // GP5  = Botão A — liga/desliga o sistema

#define Luz_led 16          // GP16 = LED indicador de luminosidade

// ═══════════════════════════════════════════════════════════════════════════
//  I2C (sensores AHT20 / BH1750)
// ═══════════════════════════════════════════════════════════════════════════
#define I2C_PORT    i2c0
#define I2C_SDA     0
#define I2C_SCL     1

// ═══════════════════════════════════════════════════════════════════════════
//  LIMITES DO SENSOR DE SOLO
// ═══════════════════════════════════════════════════════════════════════════
#define SOLO_SECO_RAW   3000    // Acima deste valor = solo seco - No ar foi medido 2200

// ═══════════════════════════════════════════════════════════════════════════
//  LIMITES DE LUMINOSIDADE (lux)
// ═══════════════════════════════════════════════════════════════════════════
#define LUX_ALTA        54600
#define LUX_MEDIA       300
#define LUX_BAIXA       10

// ═══════════════════════════════════════════════════════════════════════════
//  LIMITES DE TEMPERATURA (°C)
// ═══════════════════════════════════════════════════════════════════════════
#define TEMP_ALTA       33.0f
#define TEMP_MEDIA      28.0f
#define TEMP_BAIXA      22.0f

// ═══════════════════════════════════════════════════════════════════════════
//  TEMPOS DE IRRIGAÇÃO (segundos)
// ═══════════════════════════════════════════════════════════════════════════
#define IRRIGA_TEMP_BAIXA   1
#define IRRIGA_TEMP_MEDIA   2
#define IRRIGA_TEMP_ALTA    5
#define IRRIGA_BONUS_LUX    1

// Intervalo entre reteste do solo após um ciclo de irrigação (ms)
#define RETESTE_SOLO_MS     5000

// Debounce do botão A (us)
#define DEBOUNCE_BOTAO_US   200000

#define BOMBA_LIGADA    true
#define BOMBA_DESLIGADA false

// ═══════════════════════════════════════════════════════════════════════════
//  ESTADO GLOBAL
// ═══════════════════════════════════════════════════════════════════════════
static volatile bool sistema_ligado   = true;
static volatile uint32_t ultimo_botao_us = 0;

// ═══════════════════════════════════════════════════════════════════════════
//  PAINEL DE EXIBIÇÃO (SERIAL)
// ═══════════════════════════════════════════════════════════════════════════
static void print_separador(void) {
    printf("══════════════════════════════════════════\n");
}

static void print_cabecalho(void) {
    print_separador();
    printf("         SISTEMA DE IRRIGAÇÃO\n");
    print_separador();
}

static const char *classificar_luminosidade(uint16_t lux) {
    if (lux >= LUX_ALTA)  return "Alta (sol direto)";
    if (lux >= LUX_MEDIA) return "Media (ambiente)";
    return                       "Baixa (escuro)";
}

static const char *classificar_temperatura(float temp) {
    if (temp >= TEMP_ALTA)  return "Alta";
    if (temp >= TEMP_MEDIA) return "Media";
    return                         "Baixa";
}

// Versões curtas (3 letras) — usadas no display, que tem pouco espaço
static const char *classificar_temperatura_curta(float temp) {
    if (temp >= TEMP_ALTA)  return "ALT";
    if (temp >= TEMP_MEDIA) return "MED";
    return                         "BAI";
}

static const char *classificar_luminosidade_curta(uint16_t lux) {
    if (lux >= LUX_ALTA)  return "ALT";
    if (lux >= LUX_MEDIA) return "MED";
    return                       "BAI";
}

// ═══════════════════════════════════════════════════════════════════════════
//  BOTÃO A — LIGA/DESLIGA O SISTEMA
// ═══════════════════════════════════════════════════════════════════════════
static void gpio_callback(uint gpio, uint32_t events) {
    if (gpio != BOTAO_A_PIN) return;

    uint32_t agora = time_us_32();
    // Debounce simples: só aceita novo toque após DEBOUNCE_BOTAO_US
    if (agora - ultimo_botao_us > DEBOUNCE_BOTAO_US) {
        sistema_ligado = !sistema_ligado;
        ultimo_botao_us = agora;
    }
}

// Espera "ms" milissegundos em pequenos passos, verificando o botão a cada
// passo. Retorna false se o sistema for desligado no meio da espera
// (permite interromper irrigação/reteste imediatamente).
static bool esperar_com_verificacao(int ms) {
    const int passo_ms = 100;
    int passos = ms / passo_ms;
    for (int i = 0; i < passos; i++) {
        if (!sistema_ligado) return false;
        sleep_ms(passo_ms);
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  LÓGICA DE IRRIGAÇÃO
// ═══════════════════════════════════════════════════════════════════════════
static int calcular_tempo_irrigacao(bool solo_seco, float temperatura, uint16_t lux) {
    if (!solo_seco) return 0;

    int tempo = 0;

    if (temperatura >= TEMP_ALTA)       tempo = IRRIGA_TEMP_ALTA;
    else if (temperatura >= TEMP_MEDIA) tempo = IRRIGA_TEMP_MEDIA;
    else                                tempo = IRRIGA_TEMP_BAIXA;

    if (lux >= LUX_ALTA) tempo += IRRIGA_BONUS_LUX;

    return tempo;
}

// Aciona a bomba por "segundos", mas em passos de 100ms verificando o botão
// A — se o sistema for desligado durante o ciclo, a bomba é cortada na hora.
static void acionar_bomba(int segundos) {
    printf("\n  [BOMBA] Ligando por %d segundo(s)...\n", segundos);
    gpio_put(BOMBA_PIN, BOMBA_LIGADA);      // Ativa o relé
    esperar_com_verificacao(segundos * 1000);
    gpio_put(BOMBA_PIN, BOMBA_DESLIGADA);   // Desativa o relé
    printf("  [BOMBA] Ciclo concluido.\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAINEL DE EXIBIÇÃO (DISPLAY OLED)
// ═══════════════════════════════════════════════════════════════════════════
static void exibir_display(uint16_t soil_raw, bool solo_seco,
                            float temperatura, bool temp_ok,
                            uint16_t lux, bool bomba_ligada, int tempo_irrigacao)
{
    char str_solo[16];
    char str_temp[16];
    char str_lux[16];
    char str_bomba[12];

    snprintf(str_solo, sizeof(str_solo), "%4u %s", soil_raw, solo_seco ? "Seco" : "Umid");

    if (temp_ok)
        snprintf(str_temp, sizeof(str_temp), "%.1fC %s", temperatura, classificar_temperatura_curta(temperatura));
    else
        snprintf(str_temp, sizeof(str_temp), "ERRO");

    snprintf(str_lux, sizeof(str_lux), "%u %s", lux, classificar_luminosidade_curta(lux));
    snprintf(str_bomba, sizeof(str_bomba), "%s", bomba_ligada ? "LIGADA" : "OFF");

    ssd1306_fill(&ssd, 0);
    ssd1306_rect(&ssd, 3, 3, 122, 60, true, false);
    ssd1306_line(&ssd, 3, 15, 123, 15, true);
    ssd1306_line(&ssd, 3, 27, 123, 27, true);
    ssd1306_line(&ssd, 3, 39, 123, 39, true);
    ssd1306_line(&ssd, 3, 51, 123, 51, true);

    ssd1306_draw_string(&ssd, "ESTUFA AUTO", 16, 5);

    ssd1306_draw_string(&ssd, "Solo:", 8, 18);
    ssd1306_draw_string(&ssd, str_solo, 55, 18);

    ssd1306_draw_string(&ssd, "Temp:", 8, 30);
    ssd1306_draw_string(&ssd, str_temp, 55, 30);

    ssd1306_draw_string(&ssd, "Luz:", 8, 42);
    ssd1306_draw_string(&ssd, str_lux, 55, 42);

    ssd1306_draw_string(&ssd, "Bomba:", 8, 54);
    ssd1306_draw_string(&ssd, str_bomba, 60, 54);

    // Enquanto a bomba está ativa, mostra o tempo configurado no ciclo
    if (bomba_ligada && tempo_irrigacao > 0) {
        char str_tempo[16];
        snprintf(str_tempo, sizeof(str_tempo), "%ds", tempo_irrigacao);
        ssd1306_draw_string(&ssd, str_tempo, 100, 54);
    }

    ssd1306_send_data(&ssd);
}

// Tela simples exibida quando o sistema está desligado pelo botão A
static void exibir_sistema_desligado(void) {
    ssd1306_fill(&ssd, 0);
    ssd1306_rect(&ssd, 3, 3, 122, 60, true, false);
    ssd1306_draw_string(&ssd, "SISTEMA", 34, 20);
    ssd1306_draw_string(&ssd, "DESLIGADO", 22, 34);
    ssd1306_send_data(&ssd);
}

// ═══════════════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════════════
int main(void) {
    stdio_init_all();
    sleep_ms(10000); // Aguarda a inicialização do terminal serial

    // — Display ———————————————————————————————————————————————————————————
    display_init();

    // — ADC ————————————————————————————————————————————————————————————————
    adc_init();
    adc_gpio_init(SOIL_PIN);

    // — Relé ———————————————————————————————————————————————————————————————
    gpio_init(BOMBA_PIN);
    gpio_set_dir(BOMBA_PIN, GPIO_OUT);
    gpio_put(BOMBA_PIN, BOMBA_DESLIGADA);

    // — LED indicador de luminosidade —————————————————————————————————————
    gpio_init(Luz_led);
    gpio_set_dir(Luz_led, GPIO_OUT);

    // — Botão A (liga/desliga) ———————————————————————————————————————————
    gpio_init(BOTAO_A_PIN);
    gpio_set_dir(BOTAO_A_PIN, GPIO_IN);
    gpio_pull_up(BOTAO_A_PIN);
    gpio_set_irq_enabled_with_callback(BOTAO_A_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);

    // — I2C (sensores) ————————————————————————————————————————————————————
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    // — AHT20 ——————————————————————————————————————————————————————————————
    aht20_reset(I2C_PORT);
    aht20_init(I2C_PORT);
    AHT20_Data aht_data;

    // — BH1750 —————————————————————————————————————————————————————————————
    bh1750_power_on(I2C_PORT);
    sleep_ms(200);

    ssd1306_fill(&ssd, 0);
    ssd1306_draw_string(&ssd, "Sistema iniciado", 0, 0);
    ssd1306_draw_string(&ssd, "Aguardando...", 0, 12);
    ssd1306_send_data(&ssd);

    printf("\nSistema iniciado. Aguardando leituras...\n\n");
    printf("Pressione o botao A (GP%d) para ligar/desligar o sistema.\n\n", BOTAO_A_PIN);

    while (true) {

        // ── Sistema desligado pelo botão A ──────────────────────────────────
        if (!sistema_ligado) {
            gpio_put(BOMBA_PIN, BOMBA_DESLIGADA);
            gpio_put(Luz_led, false);
            exibir_sistema_desligado();
            printf("  [SISTEMA DESLIGADO] Pressione o botao A para ligar.\n");
            sleep_ms(500);
            continue;
        }

        // ── Leituras ──────────────────────────────────────────────────────
        adc_select_input(2);
        uint16_t soil_raw = adc_read();
        bool solo_seco    = (soil_raw > SOLO_SECO_RAW);

        bool temp_ok = aht20_read(I2C_PORT, &aht_data);
        float temperatura = temp_ok ? aht_data.temperature : 0.0f;

        uint16_t lux = bh1750_read_measurement(I2C_PORT);

        // ── Exibição (serial) ────────────────────────────────────────────
        print_cabecalho();

        printf("  Solo        : %4u raw  →  %s\n",
               soil_raw,
               solo_seco ? "Seco" : "Umido");

        if (temp_ok)
            printf("  Temperatura : %.1f C      →  %s\n",
                   temperatura,
                   classificar_temperatura(temperatura));
        else
            printf("  Temperatura : ERRO DE LEITURA\n");

        printf("  Luminosidade: %5u lux  →  %s\n",
               lux,
               classificar_luminosidade(lux));

        // ── Decisão de irrigação ──────────────────────────────────────────
        print_separador();
        printf("  IRRIGAÇÃO\n");
        print_separador();

        int tempo = calcular_tempo_irrigacao(solo_seco, temperatura, lux);

        // Atualiza o display com o estado atual (antes de acionar a bomba)
        exibir_display(soil_raw, solo_seco, temperatura, temp_ok, lux, false, tempo);

        if (tempo > 0) {
            printf("  Solo seco detectado.\n");
            if (lux >= LUX_ALTA)
                printf("  Alta luminosidade: +%ds de bonus aplicado.\n",
                       IRRIGA_BONUS_LUX);

            // ── Loop de irrigação: irriga, espera, reteste — repete até o
            //    solo ficar úmido (ou o sistema ser desligado no meio) ─────
            while (solo_seco && sistema_ligado) {
                exibir_display(soil_raw, solo_seco, temperatura, temp_ok, lux, true, tempo);
                acionar_bomba(tempo);
                exibir_display(soil_raw, solo_seco, temperatura, temp_ok, lux, false, tempo);

                if (!sistema_ligado) break;

                printf("  Aguardando %d s antes do reteste do solo...\n", RETESTE_SOLO_MS / 1000);
                if (!esperar_com_verificacao(RETESTE_SOLO_MS)) break;

                adc_select_input(2);
                soil_raw  = adc_read();
                solo_seco = (soil_raw > SOLO_SECO_RAW);

                printf("  [RETESTE] Solo: %4u raw  →  %s\n",
                       soil_raw, solo_seco ? "Seco" : "Umido");
            }

            if (!solo_seco)
                printf("  Solo umido — irrigacao concluida.\n");
        } else {
            printf("  Solo umido — irrigacao nao necessaria.\n");
        }

        if (lux <= LUX_BAIXA) {
            printf("  Luminosidade muito baixa — acendendo LED indicador.\n");
            gpio_put(Luz_led, true);
        } else {
            gpio_put(Luz_led, false);
        }

        print_separador();
        printf("\n");

        esperar_com_verificacao(5000);
    }
}