#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COLS 16
#define ROWS 8
#define RATE 48000
#define PI 3.14159265358979323846
#define DELAY_LENGTH (RATE * 2)
#ifndef OUTPUT_GAIN
#define OUTPUT_GAIN 2.4
#endif
#ifndef LIMIT_RELEASE
#define LIMIT_RELEASE 0.99990
#endif
#ifndef LIMIT_LEVEL
#define LIMIT_LEVEL 1.3
#endif

typedef struct {
    uint8_t cells[ROWS][COLS];
    uint8_t attacking[ROWS];
    float pulse[ROWS][COLS];
    int cursor_x, cursor_y, step, paused, bpm, bank, sound, wonky, delay, reverb, paint_mode, dirty;
    int screen, preview_note;
    double phase[ROWS], envelope[ROWS], note_level[ROWS], note_pitch[ROWS], samples_into_step;
    double lowpass, highpass, previous_mix;
    float delay_wet, reverb_wet;
    float *delay_buffer, *reverb_a, *reverb_b;
    int delay_position, reverb_a_position, reverb_b_position;
    uint32_t save_due;
    uint32_t status_until, preview_due, started_at;
    char status[64];
    char state_path[1024];
    SDL_AudioDeviceID audio;
} App;

typedef struct { uint8_t cells[ROWS][COLS]; int32_t bpm, sound; } SavedStateV1;
typedef struct { uint8_t cells[ROWS][COLS]; int32_t bpm, sound, bank, wonky; } SavedStateV2;
typedef struct { uint8_t cells[ROWS][COLS]; int32_t bpm, sound, bank, wonky, delay, reverb; } SavedState;

static const double frequencies[ROWS] = {
    523.251, 440.000, 391.995, 329.628, 293.665, 261.626, 220.000, 195.998
};

static const char *bank_names[3] = {"RAIN GLASS", "POCKET CIRCUIT", "NIGHT WINDOW"};
static const char *voice_names[3][3] = {
    {"SOFT MALLET", "GLASS BELL", "MUTED PLUCK"},
    {"ROUND LEAD", "TRI BASS", "SPARKLE"},
    {"VELVET PULSE", "HOLLOW KEY", "METAL CHIME"}
};

static double poly_blep(double phase, double increment)
{
    if(phase < increment) {
        phase /= increment;
        return phase + phase - phase * phase - 1.0;
    }
    if(phase > 1.0 - increment) {
        phase = (phase - 1.0) / increment;
        return phase * phase + phase + phase + 1.0;
    }
    return 0.0;
}

static double triangle(double phase)
{
    return 1.0 - 4.0 * fabs(phase - 0.5);
}

static double soft_pulse(double phase, double increment, double width)
{
        double shifted = phase - width;
        double value = phase < width ? 1.0 : -1.0;
        if(shifted < 0.0) shifted += 1.0;
        value += poly_blep(phase, increment) - poly_blep(shifted, increment);
        return value;
}

static double wave(double phase, double increment, int voice)
{
    double fundamental = sin(phase * 2.0 * PI);
    switch(voice) {
    case 0: return fundamental * 0.78 + sin(phase * 4.0 * PI) * 0.15 + triangle(phase) * 0.07;
    case 1: return fundamental * 0.62 + sin(phase * 6.0 * PI) * 0.25 + sin(phase * 10.0 * PI) * 0.13;
    case 2: return fundamental * 0.55 + triangle(phase) * 0.33 + sin(phase * 8.0 * PI) * 0.12;
    case 3: return soft_pulse(phase, increment, 0.5) * 0.52 + fundamental * 0.48;
    case 4: return triangle(phase) * 0.72 + fundamental * 0.28;
    case 5: return fundamental * 0.50 + sin(phase * 4.0 * PI) * 0.19 + sin(phase * 8.0 * PI) * 0.20 + sin(phase * 12.0 * PI) * 0.11;
    case 6: return soft_pulse(phase, increment, 0.38) * 0.38 + fundamental * 0.62;
    case 7: return fundamental * 0.74 - sin(phase * 4.0 * PI) * 0.20 + sin(phase * 6.0 * PI) * 0.06;
    default: return fundamental * 0.48 + sin(phase * 6.0 * PI) * 0.31 + sin(phase * 10.0 * PI) * 0.21;
    }
}

static double samples_per_step(const App *app)
{
    static const double jitter[8] = {0.0, 0.018, -0.012, 0.008, -0.020, 0.014, -0.006, 0.010};
    double factor = 1.0;
    int position = (app->step + 1) & 7;
    if(app->wonky) {
        double amount = app->wonky == 1 ? 2.80 : 4.20;
        if(app->bank == 0) factor += jitter[position] * amount;
        else if(app->bank == 1) factor += ((position & 1) ? 0.035 : -0.035) * amount;
        else factor += jitter[(position * 3) & 7] * amount * 0.72;
    }
    return RATE * 60.0 / (app->bpm * 4.0) * factor;
}

static void trigger_step(App *app)
{
    int row;
    app->step = (app->step + 1) % COLS;
    for(row = 0; row < ROWS; row++) {
        if(app->cells[row][app->step]) {
            int colour = (app->step * 5 + row * 3 + app->bank * 7) & 15;
            app->attacking[row] = 1;
            if(app->envelope[row] < 0.05) app->phase[row] = 0.0;
            app->note_level[row] = 1.0;
            app->note_pitch[row] = 1.0;
            if(app->wonky) {
                double depth = app->wonky == 1 ? 0.23 : 0.34;
                app->note_level[row] = 0.93 + (colour % 3) * depth * 0.5;
                if(app->bank == 1 && app->wonky == 2 && colour == 0) app->note_pitch[row] = 2.0;
                if(app->bank == 0 && colour == 5) app->note_level[row] *= 0.86;
                if(app->bank == 2) app->note_level[row] *= 0.92 + (app->step & 3) * 0.025;
            }
            app->pulse[row][app->step] = 1.0f;
        }
    }
}

static void audio_callback(void *userdata, Uint8 *stream, int length)
{
    static const double attacks[9] = {0.006, 0.003, 0.004, 0.005, 0.004, 0.0025, 0.008, 0.006, 0.003};
    static const double releases[9] = {0.99935, 0.99962, 0.99918, 0.99912, 0.99892, 0.99928, 0.99952, 0.99942, 0.99968};
    static const double filters[9] = {0.42, 0.58, 0.36, 0.34, 0.40, 0.56, 0.30, 0.44, 0.54};
    static const double gains[9] = {0.84, 0.96, 1.02, 0.58, 0.98, 1.00, 0.62, 0.86, 0.98};
    App *app = userdata;
    int16_t *out = (int16_t *)stream;
    int frames = length / (int)(sizeof(int16_t) * 2);
    int i, row;
    int voice = app->bank * 3 + app->sound;
    memset(stream, 0, length);
    for(i = 0; i < frames; i++) {
        double mixed = 0.0, left, right;
        if(app->screen == 2 && !app->paused) {
            double step_samples = samples_per_step(app);
            app->samples_into_step += 1.0;
            if(app->samples_into_step >= step_samples) {
                app->samples_into_step -= step_samples;
                trigger_step(app);
            }
        }
        for(row = 0; row < ROWS; row++) {
            if(app->attacking[row] || app->envelope[row] > 0.001) {
                if(app->attacking[row]) {
                    app->envelope[row] += 1.0 / (RATE * attacks[voice]);
                    if(app->envelope[row] >= app->note_level[row]) {
                        app->envelope[row] = app->note_level[row];
                        app->attacking[row] = 0;
                    }
                } else {
                    app->envelope[row] *= releases[voice];
                }
                double increment = frequencies[row] * app->note_pitch[row] / RATE;
                mixed += wave(app->phase[row], increment, voice) * app->envelope[row];
                app->phase[row] += increment;
                if(app->phase[row] >= 1.0) app->phase[row] -= 1.0;
            }
        }
        {
            app->lowpass += filters[voice] * (mixed - app->lowpass);
            mixed = app->lowpass * gains[voice];
        }
        app->highpass = mixed - app->previous_mix + 0.9844 * app->highpass;
        app->previous_mix = mixed;
        {
            static const float delay_mix[4] = {0.0f, 0.12f, 0.20f, 0.28f};
            static const float delay_feedback[4] = {0.0f, 0.17f, 0.27f, 0.37f};
            static const float room_mix[4] = {0.0f, 0.08f, 0.14f, 0.21f};
            static const float room_feedback[4] = {0.0f, 0.52f, 0.62f, 0.70f};
            double quarter = RATE * 60.0 / app->bpm;
            double delay_factor = app->delay == 1 ? 0.5 : app->delay == 2 ? 0.75 : 1.5;
            int delay_samples = (int)(quarter * delay_factor);
            int read_position;
            float delayed, room_a, room_b, room_left, room_right;
            if(delay_samples > DELAY_LENGTH - 1) delay_samples = DELAY_LENGTH - 1;
            app->delay_wet += (delay_mix[app->delay] - app->delay_wet) * 0.0009f;
            app->reverb_wet += (room_mix[app->reverb] - app->reverb_wet) * 0.0009f;
            read_position = (app->delay_position + DELAY_LENGTH - delay_samples) % DELAY_LENGTH;
            delayed = app->delay_buffer[read_position];
            app->delay_buffer[app->delay_position] = (float)app->highpass + delayed * delay_feedback[app->delay];
            if(++app->delay_position >= DELAY_LENGTH) app->delay_position = 0;
            room_a = app->reverb_a[app->reverb_a_position];
            room_b = app->reverb_b[app->reverb_b_position];
            app->reverb_a[app->reverb_a_position] = (float)(app->highpass + delayed * app->delay_wet) + room_a * room_feedback[app->reverb];
            app->reverb_b[app->reverb_b_position] = (float)(app->highpass + delayed * app->delay_wet) + room_b * (room_feedback[app->reverb] - 0.035f);
            if(++app->reverb_a_position >= 3557) app->reverb_a_position = 0;
            if(++app->reverb_b_position >= 4217) app->reverb_b_position = 0;
            room_left = room_a * 0.70f + room_b * 0.30f;
            room_right = room_a * 0.30f + room_b * 0.70f;
            mixed = app->highpass + delayed * app->delay_wet;
            left = (mixed + room_left * app->reverb_wet) * 0.92 * OUTPUT_GAIN;
            right = (mixed + room_right * app->reverb_wet) * 0.92 * OUTPUT_GAIN;
            {
                static double limit_env = 0.0;
                double peak = fabs(left) > fabs(right) ? fabs(left) : fabs(right);
                limit_env = peak > limit_env ? peak : limit_env * LIMIT_RELEASE;
                if(limit_env > LIMIT_LEVEL) {
                    left *= LIMIT_LEVEL / limit_env;
                    right *= LIMIT_LEVEL / limit_env;
                }
            }
            left = tanh(left) * 0.94;
            right = tanh(right) * 0.94;
        }
        out[i * 2] = (int16_t)(left * 32767.0);
        out[i * 2 + 1] = (int16_t)(right * 32767.0);
    }
}

static void save_state(const App *app)
{
    SavedState saved;
    char temporary[1060];
    FILE *file;
    memcpy(saved.cells, app->cells, sizeof(saved.cells));
    saved.bpm = app->bpm;
    saved.sound = app->sound;
    saved.bank = app->bank;
    saved.wonky = app->wonky;
    saved.delay = app->delay;
    saved.reverb = app->reverb;
    snprintf(temporary, sizeof(temporary), "%s.tmp", app->state_path);
    file = fopen(temporary, "wb");
    if(!file) return;
    if(fwrite(&saved, sizeof(saved), 1, file) == 1) {
        fflush(file);
        fsync(fileno(file));
        fclose(file);
        rename(temporary, app->state_path);
    } else fclose(file);
}

static void load_state(App *app)
{
    SavedState saved;
    SavedStateV2 previous;
    SavedStateV1 legacy;
    FILE *file = fopen(app->state_path, "rb");
    long size;
    if(!file) return;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);
    if(size == (long)sizeof(saved) && fread(&saved, sizeof(saved), 1, file) == 1) {
        memcpy(app->cells, saved.cells, sizeof(app->cells));
        if(saved.bpm >= 60 && saved.bpm <= 180) app->bpm = saved.bpm;
        if(saved.sound >= 0 && saved.sound <= 2) app->sound = saved.sound;
        if(saved.bank >= 0 && saved.bank <= 2) app->bank = saved.bank;
        if(saved.wonky >= 0 && saved.wonky <= 2) app->wonky = saved.wonky;
        if(saved.delay >= 0 && saved.delay <= 3) app->delay = saved.delay;
        if(saved.reverb >= 0 && saved.reverb <= 3) app->reverb = saved.reverb;
    } else if(size == (long)sizeof(previous) && fread(&previous, sizeof(previous), 1, file) == 1) {
        memcpy(app->cells, previous.cells, sizeof(app->cells));
        if(previous.bpm >= 60 && previous.bpm <= 180) app->bpm = previous.bpm;
        if(previous.sound >= 0 && previous.sound <= 2) app->sound = previous.sound;
        if(previous.bank >= 0 && previous.bank <= 2) app->bank = previous.bank;
        if(previous.wonky >= 0 && previous.wonky <= 2) app->wonky = previous.wonky;
    } else if(size == (long)sizeof(legacy) && fread(&legacy, sizeof(legacy), 1, file) == 1) {
        memcpy(app->cells, legacy.cells, sizeof(app->cells));
        if(legacy.bpm >= 60 && legacy.bpm <= 180) app->bpm = legacy.bpm;
        if(legacy.sound >= 0 && legacy.sound <= 2) app->sound = legacy.sound;
    }
    fclose(file);
}

static void mark_dirty(App *app)
{
    app->dirty = 1;
    app->save_due = SDL_GetTicks() + 500;
}

static const uint8_t glyphs[36][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}
};

static void glyph_for(char character, uint8_t rows[7])
{
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const char *found;
    int y;
    memset(rows, 0, 7);
    if(character >= 'a' && character <= 'z') character -= 32;
    found = strchr(alphabet, character);
    if(found) { memcpy(rows, glyphs[found - alphabet], 7); return; }
    if(character == ':') rows[2] = rows[5] = 4;
    else if(character == '+') { rows[2] = rows[4] = 4; rows[3] = 14; }
    else if(character == '/') { rows[0]=1; rows[1]=2; rows[2]=2; rows[3]=4; rows[4]=8; rows[5]=8; rows[6]=16; }
    else if(character == '[') for(y=0;y<7;y++) rows[y] = y==0 || y==6 ? 14 : 8;
    else if(character == ']') for(y=0;y<7;y++) rows[y] = y==0 || y==6 ? 14 : 2;
    else if(character == '|') for(y=0;y<7;y++) rows[y] = 4;
    else if(character == '.') rows[6] = 4;
}

static int text_width(const char *text, int scale)
{
    int count = (int)strlen(text);
    return count ? count * 6 * scale - scale : 0;
}

static void draw_text(SDL_Renderer *renderer, int x, int y, const char *text, int scale, Uint8 shade)
{
    uint8_t rows[7];
    SDL_Rect pixel = {0, 0, scale, scale};
    int row, column;
    SDL_SetRenderDrawColor(renderer, shade, shade, shade, 255);
    while(*text) {
        glyph_for(*text++, rows);
        for(row = 0; row < 7; row++) for(column = 0; column < 5; column++) if(rows[row] & (16 >> column)) {
            pixel.x = x + column * scale;
            pixel.y = y + row * scale;
            SDL_RenderFillRect(renderer, &pixel);
        }
        x += 6 * scale;
    }
}

static void draw_centered(SDL_Renderer *renderer, int y, const char *text, int scale, Uint8 shade)
{
    draw_text(renderer, (640 - text_width(text, scale)) / 2, y, text, scale, shade);
}

static void draw_lilt_wordmark(SDL_Renderer *renderer, int y, int scale, int visible)
{
    static const uint8_t letters[4][7] = {
        {16,16,16,16,16,16,31}, {14,4,4,4,4,4,14},
        {16,16,16,16,16,16,31}, {31,4,4,4,4,4,4}
    };
    SDL_Rect pixel = {0, 0, scale, scale};
    int x = (640 - text_width("LILT", scale)) / 2;
    int letter, row, column;
    if(visible > 4) visible = 4;
    for(letter = 0; letter < visible; letter++) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        for(row = 0; row < 7; row++) for(column = 0; column < 5; column++) if(letters[letter][row] & (16 >> column)) {
            pixel.x = x + letter * 6 * scale + column * scale;
            pixel.y = y + row * scale;
            SDL_RenderFillRect(renderer, &pixel);
        }
    }
}

static void set_status(App *app, const char *text)
{
    snprintf(app->status, sizeof(app->status), "%s", text);
    app->status_until = SDL_GetTicks() + 1100;
}

static void set_effect_status(App *app)
{
    char message[48];
    snprintf(message, sizeof(message), "ECHO : %d | SPACE : %d", app->delay, app->reverb);
    set_status(app, message);
}

static void begin_preview(App *app)
{
    app->sound = 0;
    app->preview_note = 0;
    app->preview_due = SDL_GetTicks() + 80;
}

static void service_preview(App *app)
{
    static const int notes[3][3] = {{6, 4, 2}, {7, 4, 1}, {7, 5, 2}};
    int row;
    if(app->screen != 0 || app->preview_note >= 3 || !SDL_TICKS_PASSED(SDL_GetTicks(), app->preview_due)) return;
    row = notes[app->bank][app->preview_note++];
    SDL_LockAudioDevice(app->audio);
    app->attacking[row] = 1;
    if(app->envelope[row] < 0.05) app->phase[row] = 0.0;
    app->note_level[row] = 0.72;
    app->note_pitch[row] = 1.0;
    SDL_UnlockAudioDevice(app->audio);
    app->preview_due = SDL_GetTicks() + 155;
}

static void choose_bank(App *app, int direction)
{
    app->bank = (app->bank + direction + 3) % 3;
    begin_preview(app);
    mark_dirty(app);
}


static void line(SDL_Renderer *renderer, int x1, int y1, int x2, int y2, Uint8 shade)
{
    SDL_SetRenderDrawColor(renderer, shade, shade, shade, 255);
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
}

static void render(App *app, SDL_Renderer *renderer)
{
    const int gx = 32, gy = 56, cw = 36, ch = 42;
    int x, y;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    for(x = 0; x <= COLS; x++) line(renderer, gx + x * cw, gy, gx + x * cw, gy + ROWS * ch, 58);
    for(y = 0; y <= ROWS; y++) line(renderer, gx, gy + y * ch, gx + COLS * cw, gy + y * ch, 58);
    if(!app->paused) {
        double progress = app->samples_into_step / samples_per_step(app);
        int px = gx + (int)((app->step + progress) * cw);
        line(renderer, px, gy - 12, px, gy + ROWS * ch + 12, 150);
    }
    for(y = 0; y < ROWS; y++) for(x = 0; x < COLS; x++) {
        int wobble_amount = app->wonky == 1 ? 4 : app->wonky == 2 ? 7 : 0;
        int wobble = app->cells[y][x] ? (int)(sin((SDL_GetTicks() * 0.004) + x * 0.8 + y) * wobble_amount) : 0;
        SDL_Rect cell = {gx + x * cw + 11 + wobble, gy + y * ch + 13, 14, 16};
        if(app->pulse[y][x] > 0.03f) {
            Uint8 glow = (Uint8)(90 + app->pulse[y][x] * 145);
            SDL_Rect pulse = {gx + x * cw + 7, gy + y * ch + 8, 22, 26};
            SDL_SetRenderDrawColor(renderer, glow, glow, glow, 255);
            SDL_RenderDrawRect(renderer, &pulse);
            app->pulse[y][x] *= 0.86f;
        }
        if(app->cells[y][x]) {
            SDL_SetRenderDrawColor(renderer, 242, 242, 242, 255);
            SDL_RenderFillRect(renderer, &cell);
        }
        if(x == app->cursor_x && y == app->cursor_y) {
            SDL_Rect cursor = {gx + x * cw + 4, gy + y * ch + 5, cw - 8, ch - 10};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawRect(renderer, &cursor);
        }
    }
    if(app->status[0] && !SDL_TICKS_PASSED(SDL_GetTicks(), app->status_until))
        draw_centered(renderer, 428, app->status, 2, 235);
    else {
        char identity[64];
        snprintf(identity, sizeof(identity), "%s : %s", bank_names[app->bank], voice_names[app->bank][app->sound]);
        draw_centered(renderer, 428, identity, 2, 150);
    }
    for(x = 0; x < 7; x++) {
        SDL_Rect mark = {32 + x * 12, 470, 7, 2};
        SDL_SetRenderDrawColor(renderer, x < (app->bpm - 60) / 18 + 1 ? 220 : 58, x < (app->bpm - 60) / 18 + 1 ? 220 : 58, x < (app->bpm - 60) / 18 + 1 ? 220 : 58, 255);
        SDL_RenderFillRect(renderer, &mark);
    }
    for(x = 0; x < 3; x++) {
        SDL_Rect mark = {584 + x * 10, 467, 5, 5};
        SDL_SetRenderDrawColor(renderer, x == app->sound ? 235 : 58, x == app->sound ? 235 : 58, x == app->sound ? 235 : 58, 255);
        SDL_RenderFillRect(renderer, &mark);
    }
    SDL_RenderPresent(renderer);
}

static void render_bank_screen(App *app, SDL_Renderer *renderer)
{
    int visible = (int)((SDL_GetTicks() - app->started_at) / 95) + 1;
    int index;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    draw_lilt_wordmark(renderer, 62, 5, visible);
    draw_centered(renderer, 138, "PICK A SOUND BANK", 2, 150);
    for(index = 0; index < 3; index++) {
        char option[40];
        snprintf(option, sizeof(option), index == app->bank ? "[ %s ]" : "  %s  ", bank_names[index]);
        draw_centered(renderer, 210 + index * 68, option, 3, index == app->bank ? 255 : 125);
    }
    SDL_RenderPresent(renderer);
}

static void render_help_screen(SDL_Renderer *renderer)
{
    static const char *controls[][2] = {
        {"D PAD", "MOVE"}, {"SELECT + L R", "ECHO"}, {"SELECT + U D", "SPACE"},
        {"A", "PLACE"}, {"B", "REMOVE"}, {"X", "EMPTY"}, {"Y", "WOBBLE"}, {"L1 R1", "PACE"}, {"L2 R2", "VOICE"},
        {"START", "REST"}, {"SELECT + X", "FRESH START"}, {"SELECT + START", "LEAVE"}
    };
    int index;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    draw_centered(renderer, 28, "HOW TO WAKE THE GRID", 3, 255);
    line(renderer, 80, 68, 560, 68, 90);
    for(index = 0; index < 12; index++) {
        draw_text(renderer, 76, 78 + index * 25, controls[index][0], 2, 235);
        draw_text(renderer, 390, 78 + index * 25, controls[index][1], 2, 160);
    }
    draw_centered(renderer, 414, "PRESS A BUTTON", 3, 255);
    SDL_RenderPresent(renderer);
}

static void edit(App *app, int action)
{
    SDL_LockAudioDevice(app->audio);
    if(action == 1) app->cells[app->cursor_y][app->cursor_x] ^= 1;
    else if(action == 2) app->cells[app->cursor_y][app->cursor_x] = 0;
    else if(action == 3) memset(app->cells, 0, sizeof(app->cells));
    SDL_UnlockAudioDevice(app->audio);
    mark_dirty(app);
}

static void move_cursor(App *app, int dx, int dy)
{
    app->cursor_x = (app->cursor_x + dx + COLS) % COLS;
    app->cursor_y = (app->cursor_y + dy + ROWS) % ROWS;
    if(app->paint_mode) {
        SDL_LockAudioDevice(app->audio);
        app->cells[app->cursor_y][app->cursor_x] = app->paint_mode > 0;
        SDL_UnlockAudioDevice(app->audio);
        mark_dirty(app);
    }
}

static void silence_notes(App *app)
{
    SDL_LockAudioDevice(app->audio);
    memset(app->attacking, 0, sizeof(app->attacking));
    memset(app->envelope, 0, sizeof(app->envelope));
    if(app->delay_buffer) memset(app->delay_buffer, 0, DELAY_LENGTH * sizeof(float));
    if(app->reverb_a) memset(app->reverb_a, 0, 3557 * sizeof(float));
    if(app->reverb_b) memset(app->reverb_b, 0, 4217 * sizeof(float));
    app->lowpass = app->highpass = app->previous_mix = 0.0;
    SDL_UnlockAudioDevice(app->audio);
}

static void reset_tool(App *app)
{
    SDL_LockAudioDevice(app->audio);
    memset(app->cells, 0, sizeof(app->cells));
    memset(app->attacking, 0, sizeof(app->attacking));
    memset(app->envelope, 0, sizeof(app->envelope));
    memset(app->pulse, 0, sizeof(app->pulse));
    if(app->delay_buffer) memset(app->delay_buffer, 0, DELAY_LENGTH * sizeof(float));
    if(app->reverb_a) memset(app->reverb_a, 0, 3557 * sizeof(float));
    if(app->reverb_b) memset(app->reverb_b, 0, 4217 * sizeof(float));
    app->bpm = 108;
    app->bank = 0;
    app->sound = 0;
    app->wonky = 0;
    app->delay = 0;
    app->reverb = 0;
    app->paused = 0;
    app->step = COLS - 1;
    app->samples_into_step = 0.0;
    app->cursor_x = app->cursor_y = 0;
    app->paint_mode = 0;
    app->lowpass = app->highpass = app->previous_mix = 0.0;
    SDL_UnlockAudioDevice(app->audio);
    if(app->screen == 0) begin_preview(app);
    set_status(app, "FRESH START");
    mark_dirty(app);
}

static int controller_to_button(int button)
{
    switch(button) {
    case SDL_CONTROLLER_BUTTON_A: return 1;
    case SDL_CONTROLLER_BUTTON_B: return 0;
    case SDL_CONTROLLER_BUTTON_X: return 2;
    case SDL_CONTROLLER_BUTTON_Y: return 3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 4;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 5;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 8;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 9;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 10;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 11;
    case SDL_CONTROLLER_BUTTON_BACK: return 12;
    case SDL_CONTROLLER_BUTTON_START: return 13;
    default: return -1;
    }
}

static void dpad_move(App *app, int b)
{
    if(b == 8) move_cursor(app, 0, -1);
    else if(b == 9) move_cursor(app, 0, 1);
    else if(b == 10) move_cursor(app, -1, 0);
    else if(b == 11) move_cursor(app, 1, 0);
}

int main(int argc, char **argv)
{
    App app;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_AudioSpec wanted;
    SDL_Joystick *joystick = NULL;
    SDL_GameController *controller = NULL;
    int held_dpad = -1, trigger_held[2] = {0, 0}, i;
    uint32_t next_dpad_move = 0;
    uint32_t buttons = 0;
    uint32_t next_axis_move = 0;
    int axis_x = 0, axis_y = 0, running = 1;
    memset(&app, 0, sizeof(app));
    app.bpm = 108; app.step = COLS - 1;
    snprintf(app.state_path, sizeof(app.state_path), "%s/lilt.state", argc > 1 ? argv[1] : ".");
    load_state(&app);
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) return 1;
    window = SDL_CreateWindow("LILT", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if(!window) return 2;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if(!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if(!renderer) return 3;
    SDL_RenderSetLogicalSize(renderer, 640, 480);
    for(i = 0; i < SDL_NumJoysticks() && !controller; i++)
        if(SDL_IsGameController(i)) controller = SDL_GameControllerOpen(i);
    if(!controller && SDL_NumJoysticks() > 0) joystick = SDL_JoystickOpen(0);
    app.delay_buffer = calloc(DELAY_LENGTH, sizeof(float));
    app.reverb_a = calloc(3557, sizeof(float));
    app.reverb_b = calloc(4217, sizeof(float));
    if(!app.delay_buffer || !app.reverb_a || !app.reverb_b) return 4;
    SDL_zero(wanted); wanted.freq = RATE; wanted.format = AUDIO_S16SYS; wanted.channels = 2; wanted.samples = 512; wanted.callback = audio_callback; wanted.userdata = &app;
    app.audio = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
    if(!app.audio) return 5;
    SDL_PauseAudioDevice(app.audio, 0);
    begin_preview(&app);
    app.started_at = SDL_GetTicks();
    while(running) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_QUIT) running = 0;
            else if(event.type == SDL_KEYDOWN) {
                if(event.key.keysym.sym == SDLK_ESCAPE) running = 0;
                else if(app.screen == 0) {
                    if(event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_UP) choose_bank(&app, -1);
                    else if(event.key.keysym.sym == SDLK_RIGHT || event.key.keysym.sym == SDLK_DOWN) choose_bank(&app, 1);
                    else if(event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) app.screen = 1;
                } else if(app.screen == 1) {
                    silence_notes(&app); app.screen = 2; set_status(&app, voice_names[app.bank][app.sound]);
                }
            }
            else if(controller && (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP)) {
                SDL_Event translated;
                int b = controller_to_button(event.cbutton.button);
                if(b < 0) continue;
                SDL_zero(translated);
                translated.type = event.type == SDL_CONTROLLERBUTTONDOWN ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                translated.jbutton.button = (Uint8)b;
                event = translated;
            } else if(controller && event.type == SDL_CONTROLLERAXISMOTION) {
                int axis = event.caxis.axis;
                if(axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                    int t = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : 1;
                    int pressed = trigger_held[t] ? event.caxis.value > 8000 : event.caxis.value > 16000;
                    if(pressed == trigger_held[t]) continue;
                    trigger_held[t] = pressed;
                    SDL_zero(event);
                    event.type = pressed ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                    event.jbutton.button = (Uint8)(6 + t);
                } else if(axis < 2) {
                    int value = abs(event.caxis.value) > 10000 ? event.caxis.value : 0;
                    if(axis == 0) axis_x = value;
                    else axis_y = value;
                    continue;
                } else continue;
            } else if(controller && (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP || event.type == SDL_JOYAXISMOTION))
                continue;
            if(event.type == SDL_JOYAXISMOTION && event.jaxis.axis < 2) {
                int value = abs(event.jaxis.value) > 10000 ? event.jaxis.value : 0;
                if(event.jaxis.axis == 0) axis_x = value;
                else axis_y = value;
            } else if(event.type == SDL_JOYBUTTONDOWN) {
                int b = event.jbutton.button;
                if(b < 32) buttons |= 1u << b;
                if((buttons & (1u << 12)) && (buttons & (1u << 13))) running = 0;
                else if(b == 16) reset_tool(&app);
                else if(app.screen == 0) {
                    if(b == 8 || b == 10) choose_bank(&app, -1);
                    else if(b == 9 || b == 11) choose_bank(&app, 1);
                    else if(b == 1) app.screen = 1;
                } else if(app.screen == 1) {
                    if(b != 12 && b != 13) { silence_notes(&app); app.screen = 2; set_status(&app, voice_names[app.bank][app.sound]); }
                }
                else if((buttons & (1u << 12)) && b == 2) reset_tool(&app);
                else if(b >= 8 && b <= 11 && !(buttons & (1u << 12))) {
                    dpad_move(&app, b);
                    held_dpad = b;
                    next_dpad_move = SDL_GetTicks() + 220;
                }
                else if(b == 1) { edit(&app, 1); app.paint_mode = app.cells[app.cursor_y][app.cursor_x] ? 1 : -1; }
                else if(b == 0) { edit(&app, 2); app.paint_mode = -1; }
                else if(b == 2) { edit(&app, 3); set_status(&app, "ALL QUIET"); }
                else if(b == 3) {
                    app.wonky = (app.wonky + 1) % 3;
                    set_status(&app, app.wonky == 0 ? "STEADY AGAIN" : app.wonky == 1 ? "A LITTLE WONKY" : "VERY WONKY");
                    mark_dirty(&app);
                }
                else if(b == 4 || b == 5) {
                    char pace[32];
                    app.bpm += b == 5 ? 4 : -4; if(app.bpm < 60) app.bpm = 60; if(app.bpm > 180) app.bpm = 180;
                    snprintf(pace, sizeof(pace), "PACE : %d", app.bpm); set_status(&app, pace); mark_dirty(&app);
                }
                else if(b == 6 || b == 7) {
                    app.sound = (app.sound + (b == 7 ? 1 : 2)) % 3;
                    set_status(&app, voice_names[app.bank][app.sound]); mark_dirty(&app);
                }
                else if(b == 8 || b == 9) {
                    app.reverb += b == 8 ? 1 : -1; if(app.reverb < 0) app.reverb = 0; if(app.reverb > 3) app.reverb = 3;
                    set_effect_status(&app); mark_dirty(&app);
                }
                else if(b == 10 || b == 11) {
                    app.delay += b == 11 ? 1 : -1; if(app.delay < 0) app.delay = 0; if(app.delay > 3) app.delay = 3;
                    set_effect_status(&app); mark_dirty(&app);
                }
                else if(b == 13) { app.paused ^= 1; set_status(&app, app.paused ? "LILT IS LISTENING" : "AWAKE"); }
            } else if(event.type == SDL_JOYBUTTONUP && event.jbutton.button < 32) {
                buttons &= ~(1u << event.jbutton.button);
                if(event.jbutton.button == 0 || event.jbutton.button == 1) app.paint_mode = 0;
                if(event.jbutton.button == held_dpad) held_dpad = -1;
            }
        }
        if((axis_x || axis_y) && SDL_TICKS_PASSED(SDL_GetTicks(), next_axis_move)) {
            if(app.screen == 0) {
                choose_bank(&app, abs(axis_x) > abs(axis_y) ? (axis_x > 0 ? 1 : -1) : (axis_y > 0 ? 1 : -1));
                next_axis_move = SDL_GetTicks() + 220;
            } else if(app.screen == 2) {
                if(abs(axis_x) > abs(axis_y)) move_cursor(&app, axis_x > 0 ? 1 : -1, 0);
                else move_cursor(&app, 0, axis_y > 0 ? 1 : -1);
                next_axis_move = SDL_GetTicks() + 72;
            }
        }
        if(held_dpad >= 0 && app.screen == 2 && !(buttons & (1u << 12)) && SDL_TICKS_PASSED(SDL_GetTicks(), next_dpad_move)) {
            dpad_move(&app, held_dpad);
            next_dpad_move = SDL_GetTicks() + 72;
        }
        service_preview(&app);
        if(app.dirty && SDL_TICKS_PASSED(SDL_GetTicks(), app.save_due)) {
            save_state(&app);
            app.dirty = 0;
        }
        if(app.screen == 0) render_bank_screen(&app, renderer);
        else if(app.screen == 1) render_help_screen(renderer);
        else render(&app, renderer);
        SDL_Delay(4);
    }
    save_state(&app);
    SDL_CloseAudioDevice(app.audio);
    free(app.delay_buffer); free(app.reverb_a); free(app.reverb_b);
    if(controller) SDL_GameControllerClose(controller);
    if(joystick) SDL_JoystickClose(joystick);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
