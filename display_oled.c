#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "inc/ssd1306.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h" // PWM para o buzzer (beep/alerta sonoro)

/* ======================================================================
 * 1) CONFIGURAÇÕES GERAIS DE HARDWARE E PARÂMETROS DE INTERFACE
 * ====================================================================== */

// =============================
// Configurações de Hardware
// =============================

// Barramento I2C (Inter-Integrated Circuit = barramento serial de 2 fios)
const uint I2C_SDA = 14;
const uint I2C_SCL = 15;

// Botões da BitDogLab (GPIO = General-Purpose Input/Output = pino de uso geral)
#define BUTTON_A_PIN 5 // Avançar (next)
#define BUTTON_B_PIN 6 // Voltar  (previous)

// Buzzer (defina conforme seu hardware)
// - Para buzzer PASSIVO: gere tom (tone) via PWM
// - Para buzzer ATIVO: um simples "pwm_set_gpio_level" alto/baixo já emite som (o tom é interno)
#define BUZZER_PIN 10 // AJUSTE este pino para o seu buzzer

// =============================
// Parâmetros de Interface
// =============================

// Debounce (anti-repique) em milissegundos
#define DEBOUNCE_MS 180

// Altura de linha para fonte 5x7 (line height)
#define LINE_H 8

/* ======================================================================
 * 2) CONTEÚDO DE UI (PÁGINAS) E ESTADO DE PAGINAÇÃO
 * ====================================================================== */

// =============================
// Conteúdo das páginas (UI)
// =============================
// Use '\n' (newline = nova linha) para quebrar linhas no OLED.
static const char *PAGES[] = {
    "              \n"
    "|Bem vindo! |\n"
    "|            |\n"
    "|ALUNO    |\n"
    "|            |\n"
    "|TADS Info 2B|\n"
    "              \n",

    "Pagina 2\n\n"
    "Com programacao \n\n"
    "e robotica\n"
    "                \n",

    "Pagina 3\n\n"
    "O ceu e limite.",

    "Pagina 4\n\n"
    "Obrigado"};
static const int NUM_PAGES = (int)(sizeof(PAGES) / sizeof(PAGES[0]));

// Estado de paginação (page index = índice da página atual)
static int current_page = 0;

/* ======================================================================
 * 3) ÁUDIO / BUZZER (PWM)
 * ====================================================================== */

// =============================
// Buzzer / Som (PWM)
// =============================

static uint buzzer_slice = 0; // slice do PWM associado ao pino do buzzer

// Inicializa PWM no pino do buzzer.
// Observação: para buzzer ATIVO, qualquer frequência audível funciona (geralmente já emite som).
// Para buzzer PASSIVO, a frequência define o tom (pitch).
static void buzzer_init(void)
{
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    buzzer_slice = pwm_gpio_to_slice_num(BUZZER_PIN);

    // Deixe o PWM habilitado; ajustaremos TOP/nível na hora do beep.
    pwm_set_enabled(buzzer_slice, true);
}

// Reproduz um tom (tone) por "ms" milissegundos com "duty" (0.0 a 1.0).
// freq_hz: 400–4000 funciona bem para a maioria dos buzzers.
// duty: ~0.3 (30%) costuma ser audível sem distorcer.
static void play_tone(uint32_t freq_hz, uint16_t ms, float duty)
{
    if (freq_hz == 0)
    {
        sleep_ms(ms);
        return;
    }

    // clk_sys tipicamente 125 MHz no RP2040
    const uint32_t clk_sys = 125000000;
    // Escolhemos um divisor de clock (clock divider) moderado para manter TOP dentro de 16 bits
    float clk_div = 4.0f;
    // Calcula TOP: f_pwm = clk_sys / (clk_div * (TOP + 1))
    uint32_t top = (uint32_t)((float)clk_sys / (clk_div * (float)freq_hz)) - 1u;
    if (top > 65535)
    {
        // Se extrapolar 16 bits, aumente o divisor e recalcule
        clk_div = 16.0f;
        top = (uint32_t)((float)clk_sys / (clk_div * (float)freq_hz)) - 1u;
        if (top > 65535)
        {
            // Freq muito baixa — limita ao máximo possível
            top = 65535;
        }
    }

    pwm_set_clkdiv(buzzer_slice, clk_div);
    pwm_set_wrap(buzzer_slice, top);

    // Duty (nível) em counts
    if (duty < 0.0f)
        duty = 0.0f;
    if (duty > 1.0f)
        duty = 1.0f;
    uint32_t level = (uint32_t)((float)top * duty);
    pwm_set_gpio_level(BUZZER_PIN, level);

    sleep_ms(ms);

    // Silencia o buzzer
    pwm_set_gpio_level(BUZZER_PIN, 0);
}

// Beep distinto para "primeira página"
static void beep_first_page(void)
{
    // Tom mais grave (low) e curto
    play_tone(500, 90, 0.35f);
}

// Beep distinto para "última página"
static void beep_last_page(void)
{
    // Tom mais agudo (high) e curto
    play_tone(1200, 90, 0.35f);
}

/* ======================================================================
 * 4) DESENHO DE TEXTO NO OLED (QUEBRA DE LINHA) E RENDERIZAÇÃO DE PÁGINA
 * ====================================================================== */

// Desenha múltiplas linhas no buffer (buffer = memória temporária para renderização)
// Quebra o texto em '\n' e chama ssd1306_draw_string por linha.
static void oled_println_buf(uint8_t *ssd, int x, int y, const char *text)
{
    const char *start = text;
    const char *p = text;

    char linebuf[128] = {0}; // buffer temporário de linha

    while (*p)
    {
        if (*p == '\n')
        {
            int len = (int)(p - start);
            if (len > 0)
            {
                if (len >= (int)sizeof(linebuf))
                    len = (int)sizeof(linebuf) - 1;
                memcpy(linebuf, start, len);
                linebuf[len] = '\0';
                ssd1306_draw_string(ssd, x, y, linebuf);
            }
            // Próxima linha
            y += LINE_H;
            p++;
            start = p;
        }
        else
        {
            p++;
        }
    }

    // Última linha (se o texto não terminou com '\n')
    if (p > start)
    {
        int len = (int)(p - start);
        if (len >= (int)sizeof(linebuf))
            len = (int)sizeof(linebuf) - 1;
        memcpy(linebuf, start, len);
        linebuf[len] = '\0';
        ssd1306_draw_string(ssd, x, y, linebuf);
    }
}

// Renderiza a página atual:
// - Limpa o buffer (clear)
// - Desenha o corpo (body) do texto
// - Desenha rodapé (footer) com instruções e indicador "página atual/total"
// - Envia para o display (render_on_display)
static void render_page(uint8_t *ssd, struct render_area *area, int page_index)
{
    // Zera o display inteiro (limpa o buffer de vídeo)
    memset(ssd, 0, ssd1306_buffer_length);

    // Corpo da página (margem esquerda = 5 px, topo = 0)
    oled_println_buf(ssd, 5, 0, PAGES[page_index]);

    // Rodapé (footer) com instruções e indicador numérico
    char footer[32];
    snprintf(footer, sizeof(footer), "A=Prox B=Voltar  %d/%d", page_index + 1, NUM_PAGES);
    // Desenha rodapé na última linha útil (display 128x64 => y = 56)
    ssd1306_draw_string(ssd, 0, 56, footer);

    // Atualiza o display físico (show/update)
    render_on_display(ssd, area);
}

/* ======================================================================
 * 5) ENTRADA (BOTÕES) E UTILITÁRIOS
 * ====================================================================== */

// Retorna true (verdadeiro) se o botão (com pull-up) estiver pressionado (nível LOW/baixo).
static inline bool button_pressed(uint pin)
{
    return gpio_get(pin) == 0;
}

/* ======================================================================
 * 6) SETUP (INICIALIZAÇÃO) E LOOP PRINCIPAL
 * ====================================================================== */

// =============================
// Programa principal (main)
// =============================
int main(void)
{
    // stdio_init_all: prepara E/S padrão (standard I/O)
    stdio_init_all();

    // --- I2C + OLED ---
    // i2c_init: inicializa controladora I2C1 em ssd1306_i2c_clock (kHz)
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    // Inicializa o display SSD1306
    ssd1306_init();

    // Área de renderização (render area = região da tela a atualizar)
    struct render_area frame_area = {
        .start_column = 0,
        .end_column = ssd1306_width - 1,
        .start_page = 0,
        .end_page = ssd1306_n_pages - 1};
    calculate_render_area_buffer_length(&frame_area);

    // Buffer de vídeo (framebuffer = imagem em memória)
    static uint8_t ssd[ssd1306_buffer_length];
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);

    // --- Botões A (avança) e B (volta) ---
    gpio_init(BUTTON_A_PIN);
    gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_A_PIN);

    gpio_init(BUTTON_B_PIN);
    gpio_set_dir(BUTTON_B_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_B_PIN);

    // --- Buzzer ---
    buzzer_init();

    // Primeiro desenho (render) na tela
    render_page(ssd, &frame_area, current_page);
    // Se iniciar já na primeira/última, pode tocar um beep informativo (opcional):
    beep_first_page();

    // Controle de debounce por tempo
    absolute_time_t last_change = get_absolute_time();

    while (true)
    {
        bool updated = false;

        // Avançar (A / next)
        if (button_pressed(BUTTON_A_PIN))
        {
            if (absolute_time_diff_us(last_change, get_absolute_time()) / 1000 > DEBOUNCE_MS)
            {
                if (current_page < NUM_PAGES - 1)
                {
                    // Vai avançar de fato
                    current_page++;
                    updated = true;

                    // *** REQUISITO: tocar SOM AO CHEGAR NA ÚLTIMA PÁGINA ***
                    // if (current_page == (NUM_PAGES - 1))
                    // {
                    //     beep_last_page(); // chegou agora na última
                    // }
                }
                else
                {
                    // *** REQUISITO: se JÁ ESTIVER na ÚLTIMA e apertar A, tocar som ***
                    beep_last_page();
                }
                last_change = get_absolute_time();
            }
        }
        // Voltar (B / previous)
        else if (button_pressed(BUTTON_B_PIN))
        {
            if (absolute_time_diff_us(last_change, get_absolute_time()) / 1000 > DEBOUNCE_MS)
            {
                if (current_page > 0)
                {
                    // Vai voltar de fato
                    current_page--;
                    updated = true;

                    // (Observação): o requisito NÃO pede som ao CHEGAR na primeira.
                    // Se você quiser som ao chegar na primeira, descomente:
                    // if (current_page == 0) { beep_first_page(); }
                }
                else
                {
                    // *** REQUISITO: se JÁ ESTIVER na PRIMEIRA e apertar B, tocar som ***
                    beep_first_page();
                }
                last_change = get_absolute_time();
            }
        }

        // Se houve mudança de página, renderiza (desenha) a página atual
        if (updated)
        {
            render_page(ssd, &frame_area, current_page);

            // Observação importante:
            // Antes, havia um beep aqui ao "chegar" nas extremidades (incluindo a primeira).
            // Para atender exatamente ao novo requisito, mantivemos:
            //  - Beep ao chegar na ÚLTIMA (feito logo após o incremento, acima).
            //  - Beep ao pressionar nas bordas (feito nos blocos 'else' acima).
            //  - NÃO tocar beep automaticamente ao chegar na PRIMEIRA (a menos que você queira).
        }

        // Pequena pausa (sleep) para aliviar CPU (loop = laço)
        sleep_ms(10);
    }

    return 0;
}
