#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "pico/pdm_microphone.h"

// --- Microphone Pins ---
#define MIC_CLK_PIN 22
#define MIC_DAT_PIN 23

// --- Audio Settings ---
#define SAMPLE_RATE 16000
#define FFT_SIZE 256  // 16ms of audio
#define PI 3.14159265358979323846

int16_t sample_buffer[FFT_SIZE];
volatile bool new_audio_ready = false;

// --- DMA Audio Callback ---
void on_pdm_samples_ready() {
    pdm_microphone_read(sample_buffer, FFT_SIZE);
    new_audio_ready = true;
}

// ==========================================
// FIX: DC OFFSET REMOVAL
// ==========================================
// PDM microphones often have a floating zero-point. This calculates the 
// actual resting point and subtracts it so the wave centers perfectly on 0.
void remove_dc_offset(int16_t *buffer, int num_samples) {
    long sum = 0;
    for (int i = 0; i < num_samples; i++) {
        sum += buffer[i];
    }
    int16_t mean = (int16_t)(sum / num_samples);
    for (int i = 0; i < num_samples; i++) {
        buffer[i] -= mean;
    }
}

// --- Lightweight Radix-2 FFT ---
void compute_fft_magnitude(int16_t* pcm_data, float* magnitudes, int n) {
    float data_re[FFT_SIZE];
    float data_im[FFT_SIZE];

    // Load data and apply a simple Hanning window
    for (int i = 0; i < n; i++) {
        float multiplier = 0.5f * (1.0f - cos(2.0f * PI * i / (n - 1)));
        data_re[i] = pcm_data[i] * multiplier;
        data_im[i] = 0.0f;
    }

    // Bit-reversal permutation
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            float temp_re = data_re[i];
            float temp_im = data_im[i];
            data_re[i] = data_re[j];
            data_im[i] = data_im[j];
            data_re[j] = temp_re;
            data_im[j] = temp_im;
        }
        int m = n / 2;
        while (m >= 1 && j >= m) {
            j -= m;
            m /= 2;
        }
        j += m;
    }

    // Cooley-Tukey Decimation-in-Time
    for (int k = 1; k < n; k *= 2) {
        float step_re = cos(-PI / k);
        float step_im = sin(-PI / k);
        for (int i = 0; i < n; i += 2 * k) {
            float w_re = 1.0f;
            float w_im = 0.0f;
            for (int current_j = 0; current_j < k; current_j++) {
                float u_re = data_re[i + current_j];
                float u_im = data_im[i + current_j];
                float v_re = data_re[i + current_j + k] * w_re - data_im[i + current_j + k] * w_im;
                float v_im = data_re[i + current_j + k] * w_im + data_im[i + current_j + k] * w_re;
                
                data_re[i + current_j] = u_re + v_re;
                data_im[i + current_j] = u_im + v_im;
                data_re[i + current_j + k] = u_re - v_re;
                data_im[i + current_j + k] = u_im - v_im;

                float next_w_re = w_re * step_re - w_im * step_im;
                float next_w_im = w_re * step_im + w_im * step_re;
                w_re = next_w_re;
                w_im = next_w_im;
            }
        }
    }

    // Calculate magnitude for the first half (Nyquist)
    for (int i = 0; i < n / 2; i++) {
        magnitudes[i] = sqrt(data_re[i] * data_re[i] + data_im[i] * data_im[i]);
    }
}

// --- Menu Functions ---
void stream_raw_audio() {
    printf("\n--- ASCII OSCILLOSCOPE (RAW AUDIO) ---\n");
    printf("Speak into the mic! Press any key to stop.\n\n");
    
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT); 

    const int TERMINAL_WIDTH = 80;
    const int CENTER = TERMINAL_WIDTH / 2;
    
    // TWEAK THIS: Lower = wider wave. 
    // Since we removed DC offset, we can make it much more sensitive!
    const int SCALE_FACTOR = 15; 

    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        if (new_audio_ready) {
            new_audio_ready = false;
            
            // 1. Clean the audio
            remove_dc_offset(sample_buffer, FFT_SIZE);
            
            // 2. Print decimated samples
            for (int i = 0; i < FFT_SIZE; i += 4) {
                int pos = CENTER + (sample_buffer[i] / SCALE_FACTOR);
                
                if (pos < 0) pos = 0;
                if (pos >= TERMINAL_WIDTH) pos = TERMINAL_WIDTH - 1;

                for (int col = 0; col < TERMINAL_WIDTH; col++) {
                    if (col == pos) putchar('*');
                    else if (col == CENTER) putchar('|');
                    else putchar(' ');
                }
                putchar('\n');
            }
            sleep_ms(20); 
        }
    }
}

void stream_fft_spectrum() {
    printf("\n--- LIVE FFT SPECTRUM ANALYZER ---\n");
    printf("Press any key to stop...\n");
    sleep_ms(1000); 

    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT);

    float magnitudes[FFT_SIZE / 2];
    int num_bins = FFT_SIZE / 2; 
    float hz_per_bin = (float)SAMPLE_RATE / FFT_SIZE; 

    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        if (new_audio_ready) {
            new_audio_ready = false;
            
            // 1. Clean the audio so the 0Hz bin isn't swamped!
            remove_dc_offset(sample_buffer, FFT_SIZE);
            
            // 2. Compute FFT
            compute_fft_magnitude(sample_buffer, magnitudes, FFT_SIZE);

            printf("\033[2J\033[H");
            printf("FFT Spectrum (Resolution: %.1f Hz/bin)\n", hz_per_bin);
            printf("--------------------------------------\n");

            int bins_per_row = num_bins / 16; 
            
            for (int row = 0; row < 16; row++) {
                float avg_mag = 0;
                for (int b = 0; b < bins_per_row; b++) {
                    avg_mag += magnitudes[(row * bins_per_row) + b];
                }
                avg_mag /= bins_per_row;

                // TWEAK THIS: Lower divisor = more sensitive bars.
                // 150.0f should pick up a phone tone easily now that DC is gone.
                int bar_len = (int)(avg_mag / 150.0f); 
                if (bar_len > 40) bar_len = 40; 

                int freq_start = (int)(row * bins_per_row * hz_per_bin);
                int freq_end = (int)(((row + 1) * bins_per_row - 1) * hz_per_bin);

                printf("%4d-%4d Hz | ", freq_start, freq_end);
                for (int i = 0; i < bar_len; i++) printf("#");
                printf("\n");
            }
            sleep_ms(40); 
        }
    }
}

int main() {
    stdio_init_all();
    sleep_ms(2000);

    struct pdm_microphone_config config = {
        .gpio_data = MIC_DAT_PIN,
        .gpio_clk = MIC_CLK_PIN,
        .pio = pio1, 
        .pio_sm = 0,
        .sample_rate = SAMPLE_RATE,
        .sample_buffer_size = FFT_SIZE,
    };

    if (pdm_microphone_init(&config) < 0) {
        printf("Failed to initialize PDM microphone library!\n");
        while (1) sleep_ms(1000);
    }

    pdm_microphone_set_samples_ready_handler(on_pdm_samples_ready);
    pdm_microphone_start();

    while (true) {
        printf("\n========================================\n");
        printf("   USB AUDIO & DSP DEBUGGER\n");
        printf("========================================\n");
        printf("1. Stream Raw Audio (ASCII Oscilloscope)\n");
        printf("2. Stream FFT Spectrum Visualizer\n");
        printf("> ");

        int choice = getchar_timeout_us(0xFFFFFFFF);

        switch(choice) {
            case '1': stream_raw_audio(); break;
            case '2': stream_fft_spectrum(); break;
            case '\r':
            case '\n': break;
            default: printf("\nInvalid selection.\n");
        }
    }

    return 0;
} 