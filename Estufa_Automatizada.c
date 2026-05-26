#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "aht20.h"
#include "bh1750_light_sensor.h"

// ═══════════════════════════════════════════════════════════════════════════
//  PINOS
// ═══════════════════════════════════════════════════════════════════════════
#define SOIL_PIN    28      // GP28 = ADC2 — sensor de umidade do solo
#define BOMBA_PIN   18      // GP18 = Bomba d'água (relé)

// ═══════════════════════════════════════════════════════════════════════════
//  I2C
// ═══════════════════════════════════════════════════════════════════════════
#define I2C_PORT    i2c0
#define I2C_SDA     0
#define I2C_SCL     1

// ═══════════════════════════════════════════════════════════════════════════
//  LIMITES DO SENSOR DE SOLO
// ═══════════════════════════════════════════════════════════════════════════
#define SOLO_SECO_RAW   2400    // Acima deste valor = solo seco - No ar foi medido 2200

// ═══════════════════════════════════════════════════════════════════════════
//  LIMITES DE LUMINOSIDADE (lux)
// ═══════════════════════════════════════════════════════════════════════════
#define LUX_ALTA        54600
#define LUX_MEDIA       600
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
#define IRRIGA_TEMP_BAIXA   2
#define IRRIGA_TEMP_MEDIA   5
#define IRRIGA_TEMP_ALTA    10
#define IRRIGA_BONUS_LUX    2

#define BOMBA_LIGADA    true
#define BOMBA_DESLIGADA false

// ═══════════════════════════════════════════════════════════════════════════
//  PAINEL DE EXIBIÇÃO
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

static void acionar_bomba(int segundos) {
    printf("\n  [BOMBA] Ligando por %d segundo(s)...\n", segundos);
    gpio_put(BOMBA_PIN, BOMBA_LIGADA);    // Ativa o relé (assumindo que LOW liga a bomba)
    sleep_ms(segundos * 1000);
    gpio_put(BOMBA_PIN, BOMBA_DESLIGADA);  // Desativa o relé (assumindo que LOW liga a bomba)
    printf("  [BOMBA] Ciclo concluido.\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════════════
int main(void) {
    stdio_init_all();

    // — ADC ————————————————————————————————————————————————————————————————
    adc_init();
    adc_gpio_init(SOIL_PIN);

    // — Relé ———————————————————————————————————————————————————————————————
    gpio_init(BOMBA_PIN);
    gpio_set_dir(BOMBA_PIN, GPIO_OUT);
    gpio_put(BOMBA_PIN, BOMBA_DESLIGADA);

    // — I2C ————————————————————————————————————————————————————————————————
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

    printf("\nSistema iniciado. Aguardando leituras...\n\n");

    while (true) {

        // ── Leituras ──────────────────────────────────────────────────────
        adc_select_input(2);
        uint16_t soil_raw = adc_read();
        bool solo_seco    = (soil_raw > SOLO_SECO_RAW);

        bool temp_ok = aht20_read(I2C_PORT, &aht_data);
        float temperatura = temp_ok ? aht_data.temperature : 0.0f;

        uint16_t lux = bh1750_read_measurement(I2C_PORT);

        // ── Exibição ──────────────────────────────────────────────────────
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

        if (tempo > 0) {
            printf("  Solo seco detectado.\n");
            if (lux >= LUX_ALTA)
                printf("  Alta luminosidade: +%ds de bonus aplicado.\n",
                       IRRIGA_BONUS_LUX);
            acionar_bomba(tempo);
        } else {
            printf("  Solo umido — irrigacao nao necessaria.\n");
        }

        print_separador();
        printf("\n");

        sleep_ms(1000);
    }
}