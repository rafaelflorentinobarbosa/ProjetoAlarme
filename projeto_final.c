#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "inc/ssd1306.h"
#include "hardware/i2c.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"

// Botoões A e B
#define BUTTON_A 5  // GPIO para ativar o alarme
#define BUTTON_B 6  // GPIO para desativar o alarme

// Leds RGB solitario
#define LED_PIN_AZUL 12 // LED Azul
#define LED_PIN_VERDE 11 // LED Verde
#define LED_PIN_VERMELHO 13 // LED Vermelho

// Matriz de leds 5X5 Ainda será implementada
// Pino e número de LEDs da matriz de LEDs.
#define LED_PIN 7
#define LED_COUNT 25

// Alarme A
#define BUZZER_PIN 21 // Buzzer (Disparado)
#define BUZZER_FREQUENCY 100

// Microfone para detectar som do ambiente
#define MIC_CHANNEL 2 // Pino e canal do microfone no ADC.
#define MIC_PIN (26 + MIC_CHANNEL)
#define SOUND_THRESHOLD 2000 // Limiar de som para disparo do alarme
#define SOUND_HIT_COUNT 5 // Número de leituras consecutivas acima do limite para disparar
bool waiting_for_sound = false; // Flag para esperar um tempo antes de monitorar o som
int sound_hit_counter = 0; // Contador de leituras acima do limiar

// Tela OLED 
const uint I2C_SDA = 14;
const uint I2C_SCL = 15;

// Joystick
const int VRX = 26; // Pino de leitura do eixo X do joystick (conectado ao ADC)
const int VRY = 27; // Pino de leitura do eixo Y do joystick (conectado ao ADC)
const int SW = 22;  // Pino de leitura do botão do joystick

// Status do alarme
bool alarm_activated = false;
bool alarm_triggered = false;
absolute_time_t activation_time; // Tempo de ativação do alarme

// Função para exibir uma mensagem na tela OLED
void display_message(char *lines[], int num_lines, uint8_t *ssd, struct render_area *frame_area) {
    memset(ssd, 0, ssd1306_buffer_length);
    int y = 0;
    for (int i = 0; i < num_lines; i++) {
        ssd1306_draw_string(ssd, 5, y, lines[i]);
        y += 8;
    }
    render_on_display(ssd, frame_area);
}

// Função para exibir uma mensagem Alarme Desativado na tela OLED
void show_alarm_deactivated(uint8_t *ssd, struct render_area *frame_area) {
    char *text[] = {
        "      ",
        "      ",
        "    Alarme    ",
        "      ",
        "  Desativado "
    };
    display_message(text, 5, ssd, frame_area);
}

// Função para exibir uma mensagem Alarme Ativado na tela OLED
void show_alarm_activated(uint8_t *ssd, struct render_area *frame_area) {
    char *text[] = {
        "      ",
        "      ",
        "    Alarme    ",
        "      ",
        "    Ativo "
    };
    display_message(text, 5, ssd, frame_area);
}

// Função para exibir uma mensagem Alarme Disparado na tela OLED
void show_alarm_triggered(uint8_t *ssd, struct render_area *frame_area) {
    char *text[] = {
        "      ",
        "      ",
        "    Alarme    ",
        "      ",
        "  Disparado "
    };
    display_message(text, 5, ssd, frame_area);
}

// Definição de uma função para configurar os LEDs RGB
void set_leds(bool verde, bool azul, bool vermelho) {
    gpio_put(LED_PIN_VERDE, verde);
    gpio_put(LED_PIN_AZUL, azul);
    gpio_put(LED_PIN_VERMELHO, vermelho);
}

// Definição de uma função para inicializar o PWM no pino do buzzer
void pwm_init_buzzer(uint pin) {
    // Configurar o pino como saída de PWM
    gpio_set_function(pin, GPIO_FUNC_PWM);

    // Obter o slice do PWM associado ao pino
    uint slice_num = pwm_gpio_to_slice_num(pin);

    // Configurar o PWM com frequência desejada
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, clock_get_hz(clk_sys) / (BUZZER_FREQUENCY * 4096)); // Divisor de clock
    pwm_init(slice_num, &config, true);

    // Iniciar o PWM no nível baixo
    pwm_set_gpio_level(pin, 0);
}

// Definição de uma função para emitir um beep com duração especificada
void beep(uint pin, uint duration_ms) {
    // Obter o slice do PWM associado ao pino
    uint slice_num = pwm_gpio_to_slice_num(pin);

    // Configurar o duty cycle para 50% (ativo)
    pwm_set_gpio_level(pin, 2048);

    // Temporização
    sleep_ms(duration_ms);

    // Desativar o sinal PWM (duty cycle 0)
    pwm_set_gpio_level(pin, 0);

    // Pausa entre os beeps
    sleep_ms(100); // Pausa de 100ms
}
void ativar_buzzer() { 
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_gpio_level(BUZZER_PIN, 2048); // Mantém o buzzer ligado
}

void desativar_buzzer() { 
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_gpio_level(BUZZER_PIN, 0); // Desliga o buzzer
}

// Função para configurar o ADC e os pinos de entrada analógica
void setup_joystick()
{
  // Inicializa o ADC e os pinos de entrada analógica
  adc_init();         // Inicializa o módulo ADC
  adc_gpio_init(VRX); // Configura o pino VRX (eixo X) para entrada ADC
  adc_gpio_init(VRY); // Configura o pino VRY (eixo Y) para entrada ADC

  // Inicializa o pino do botão do joystick
  gpio_init(SW);             // Inicializa o pino do botão
  gpio_set_dir(SW, GPIO_IN); // Configura o pino do botão como entrada
  gpio_pull_up(SW);          // Ativa o pull-up no pino do botão para evitar flutuações

}

// Função para monitorar o som ambiente pelo microfone
void check_microphone(uint8_t *ssd, struct render_area *frame_area) {
    if (!alarm_activated || alarm_triggered) return;
    
    if (waiting_for_sound && absolute_time_diff_us(activation_time, get_absolute_time()) < 6000000) {
        return; // Espera 3 segundos antes de monitorar o som
    }
    waiting_for_sound = false;

    sleep_ms(10);
    uint16_t mic_value = adc_read();
    printf("Leitura do microfone: %d\n", mic_value); // Debug da leitura

    if (mic_value > SOUND_THRESHOLD) {
        sound_hit_counter++;
        if (sound_hit_counter >= SOUND_HIT_COUNT) {
            printf("Som detectado! Alarme disparado.\n");
            alarm_triggered = true;
            set_leds(0, 0, 1);
            show_alarm_triggered(ssd, frame_area);
            ativar_buzzer(); // Agora o buzzer permanece ligado
            sound_hit_counter = 0; // Reseta o contador apenas quando o alarme dispara
        } 
    } else if (sound_hit_counter > 0) {
        sound_hit_counter--; // Em vez de resetar, diminui gradualmente
    }
}

// Função para debouncing de botões
bool debounce_button(uint gpio) {
    if (gpio_get(gpio) == 0) { // Se o botão foi pressionado
        sleep_ms(50); // Aguarda um tempo para estabilizar
        if (gpio_get(gpio) == 0) { // Confirma se ainda está pressionado
            while (gpio_get(gpio) == 0); // Espera até o botão ser solto
            return true;
        }
    }
    return false;
}

// Função para verificar se o joystick foi movido
bool joystick_moved() {
    adc_select_input(0); // Eixo X
    uint16_t x_value = adc_read();
    
    adc_select_input(1); // Eixo Y
    uint16_t y_value = adc_read();
    
    // Definição de um limiar para considerar que o joystick foi movido
    const int threshold = 1000; 

    return (x_value < (2048 - threshold) || x_value > (2048 + threshold) ||
            y_value < (2048 - threshold) || y_value > (2048 + threshold));
}

// Função para ativar ou desativar o alarme
void toggle_alarme(uint8_t *ssd, struct render_area *frame_area) {
    static absolute_time_t last_press_time;
    static int press_count = 0;
    static bool joystick_moved_recently = false;

    if (joystick_moved()) {
        joystick_moved_recently = true; // Marca que o joystick foi movido
    }

    if (debounce_button(BUTTON_A) && !alarm_activated) {
        sleep_ms(300);
        alarm_activated = true;
        alarm_triggered = false;
        waiting_for_sound = true;
        activation_time = get_absolute_time();
        joystick_moved_recently = false; // Reseta o flag ao ativar o alarme
        printf("Alarme ativado\n");
        set_leds(0, 1, 0);
        show_alarm_activated(ssd, frame_area);
        sleep_ms(300); 

    } else if (debounce_button(BUTTON_B) && (alarm_activated || alarm_triggered)) {
        if (!joystick_moved_recently) {
            printf("Joystik não foi movido! Não pode desativar o alarme.\n");
            return; // Impede a desativação do alarme
        }

        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last_press_time, now) < 500000) { // 500ms
            press_count++;
        } else {
            press_count = 1; // Reinicia o contador se a segunda pressão não ocorrer dentro do tempo limite
        }
        last_press_time = now;

        if (press_count == 2) {
            sleep_ms(300);
            alarm_activated = false;
            alarm_triggered = false;
            waiting_for_sound = false;
            desativar_buzzer();
            joystick_moved_recently = false; // Reseta o flag após desativar o alarme
            printf("Alarme desativado\n");
            set_leds(1, 0, 0);
            show_alarm_deactivated(ssd, frame_area);
            press_count = 0; // Reinicia o contador após a desativação
        }
    }
}

int main() {
    stdio_init_all();

    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init();

    struct render_area frame_area = {
        .start_column = 0,
        .end_column = ssd1306_width - 1,
        .start_page = 0,
        .end_page = ssd1306_n_pages - 1
    };

    calculate_render_area_buffer_length(&frame_area);
    uint8_t ssd[ssd1306_buffer_length];

    // Inicialização dos pinos dos botoes A e B
    gpio_init(BUTTON_A);
    gpio_set_dir(BUTTON_A, GPIO_IN);
    gpio_pull_up(BUTTON_A);

    gpio_init(BUTTON_B);
    gpio_set_dir(BUTTON_B, GPIO_IN);
    gpio_pull_up(BUTTON_B);

    // Inicialização dos pinos dos LEDs RGB
    gpio_init(LED_PIN_AZUL);
    gpio_set_dir(LED_PIN_AZUL, GPIO_OUT);
    gpio_init(LED_PIN_VERDE);
    gpio_set_dir(LED_PIN_VERDE, GPIO_OUT);
    gpio_init(LED_PIN_VERMELHO);
    gpio_set_dir(LED_PIN_VERMELHO, GPIO_OUT);

    // Inicialização do pino do buzzer
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);

    // Inicialização do pino do microfone
    adc_init();
    adc_gpio_init(MIC_PIN);
    adc_select_input(MIC_CHANNEL);

    set_leds(1, 0, 0);
    show_alarm_deactivated(ssd, &frame_area);

    setup_joystick();

    pwm_init_buzzer(BUZZER_PIN);

    while (true) {
        toggle_alarme(ssd, &frame_area);
        if (alarm_activated) {
            check_microphone(ssd, &frame_area);
        }
        sleep_ms(100);
    }
}
