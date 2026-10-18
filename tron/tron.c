#include "tron.h"

#include "Buzzer.h"
#include "InputHandler.h"
#include "Joystick.h"
#include "LCD.h"
#include "main.h"

#include <stdint.h>
#include <string.h>

extern ST7789V2_cfg_t cfg0;
extern Joystick_cfg_t joystick_cfg;
extern Joystick_cfg_t joystick2_cfg;
extern Buzzer_cfg_t buzzer_cfg;
extern volatile uint32_t g_tim6_ticks;

#define TRON_GRID_W 80
#define TRON_GRID_H 60
#define TRON_CELL_W 3
#define TRON_CELL_H 4
#define TRON_GRID_CELLS (TRON_GRID_W * TRON_GRID_H)

#define TRON_COLOUR_BG 0
#define TRON_COLOUR_BORDER 1
#define TRON_COLOUR_P1_TRAIL 2
#define TRON_COLOUR_P2_HEAD 3
#define TRON_COLOUR_P2_TRAIL 4
#define TRON_COLOUR_P1_HEAD 6

#define TRON_GRID_EMPTY 0
#define TRON_GRID_P1_TRAIL 1
#define TRON_GRID_P2_TRAIL 2

#define TRON_ROUNDS_TO_WIN 3
#define TRON_STEP_START_TICKS 7
#define TRON_STEP_MIN_TICKS 4
#define TRON_SPEED_RAMP_TICKS 1000u
#define TRON_COUNTDOWN_TICKS 300u
#define TRON_ROUND_END_TICKS 150u
#define TRON_MATCH_END_TICKS 300u

typedef enum {
    TRON_IDLE = 0,
    TRON_COUNTDOWN,
    TRON_PLAYING,
    TRON_ROUND_END,
    TRON_MATCH_END
} tron_state_t;

typedef enum {
    TRON_DIR_NONE = 0,
    TRON_DIR_UP,
    TRON_DIR_RIGHT,
    TRON_DIR_DOWN,
    TRON_DIR_LEFT
} tron_dir_t;

typedef struct {
    int16_t x;
    int16_t y;
    tron_dir_t dir;
    uint8_t score;
    uint8_t grid_value;
    uint8_t trail_colour;
    uint8_t head_colour;
} tron_player_t;

static uint8_t tron_grid[TRON_GRID_CELLS];
static tron_player_t tron_players[2];
static tron_state_t tron_state;
static uint32_t tron_state_enter_tick;
static uint32_t tron_last_move_tick;
static uint32_t tron_buzzer_stop_tick;
static int8_t tron_countdown_phase;
static uint8_t tron_round_winner;
static uint8_t tron_match_winner;
static uint8_t tron_joystick2_ready;

static uint8_t tron_tick_reached(uint32_t now, uint32_t deadline)
{
    return ((int32_t)(now - deadline) >= 0) ? 1u : 0u;
}

static uint16_t tron_abs_i16(int16_t value)
{
    return (value < 0) ? (uint16_t)(-value) : (uint16_t)value;
}

static void tron_beep(uint32_t freq_hz, uint8_t volume, uint32_t duration_ticks)
{
    buzzer_tone(&buzzer_cfg, freq_hz, volume);
    tron_buzzer_stop_tick = g_tim6_ticks + duration_ticks;
}

static void tron_update_buzzer(uint32_t now)
{
    if ((tron_buzzer_stop_tick != 0u) &&
        (tron_tick_reached(now, tron_buzzer_stop_tick) != 0u)) {
        buzzer_off(&buzzer_cfg);
        tron_buzzer_stop_tick = 0u;
    }
}

static uint16_t tron_cell_px(int16_t grid_x)
{
    return (uint16_t)((uint16_t)grid_x * (uint16_t)TRON_CELL_W);
}

static uint16_t tron_cell_py(int16_t grid_y)
{
    return (uint16_t)((uint16_t)grid_y * (uint16_t)TRON_CELL_H);
}

static void tron_draw_cell(int16_t grid_x, int16_t grid_y, uint8_t colour)
{
    uint16_t px = tron_cell_px(grid_x);
    uint16_t py = tron_cell_py(grid_y);
    uint8_t dx;
    uint8_t dy;

    for (dy = 0u; dy < TRON_CELL_H; dy++) {
        for (dx = 0u; dx < TRON_CELL_W; dx++) {
            LCD_Set_Pixel((uint16_t)(px + dx), (uint16_t)(py + dy), colour);
        }
    }
}

static void tron_draw_border(void)
{
    int16_t x;
    int16_t y;

    for (x = 0; x < TRON_GRID_W; x++) {
        tron_draw_cell(x, 0, TRON_COLOUR_BORDER);
        tron_draw_cell(x, TRON_GRID_H - 1, TRON_COLOUR_BORDER);
    }

    for (y = 1; y < TRON_GRID_H - 1; y++) {
        tron_draw_cell(0, y, TRON_COLOUR_BORDER);
        tron_draw_cell(TRON_GRID_W - 1, y, TRON_COLOUR_BORDER);
    }
}

static uint16_t tron_grid_index(int16_t grid_x, int16_t grid_y)
{
    return (uint16_t)((grid_y * TRON_GRID_W) + grid_x);
}

static void tron_print_score(uint16_t x, uint16_t y)
{
    char score[] = "P1:0 P2:0";

    score[3] = (char)('0' + tron_players[0].score);
    score[8] = (char)('0' + tron_players[1].score);
    LCD_printString(score, x, y, TRON_COLOUR_BORDER, 2);
}

static uint8_t tron_p2_button_pressed(void)
{
    return (HAL_GPIO_ReadPin(BTN4_GPIO_Port, BTN4_Pin) == GPIO_PIN_RESET) ? 1u : 0u;
}

static uint8_t tron_start_pressed(void)
{
    if (current_input.btn3_pressed != 0u) {
        return 1u;
    }

    return tron_p2_button_pressed();
}

static tron_dir_t tron_quantise_joystick(const Joystick_t *joystick)
{
    int16_t x = joystick->x_processed;
    int16_t y = (int16_t)(-joystick->y_processed);
    uint16_t abs_x = tron_abs_i16(x);
    uint16_t abs_y = tron_abs_i16(y);

    if ((abs_x == 0u) && (abs_y == 0u)) {
        return TRON_DIR_NONE;
    }

    if (abs_x > abs_y) {
        return (x > 0) ? TRON_DIR_RIGHT : TRON_DIR_LEFT;
    }

    return (y > 0) ? TRON_DIR_UP : TRON_DIR_DOWN;
}

static uint8_t tron_is_opposite(tron_dir_t current, tron_dir_t candidate)
{
    return (((current == TRON_DIR_UP) && (candidate == TRON_DIR_DOWN)) ||
            ((current == TRON_DIR_DOWN) && (candidate == TRON_DIR_UP)) ||
            ((current == TRON_DIR_LEFT) && (candidate == TRON_DIR_RIGHT)) ||
            ((current == TRON_DIR_RIGHT) && (candidate == TRON_DIR_LEFT))) ? 1u : 0u;
}

static void tron_apply_direction(tron_player_t *player, tron_dir_t candidate)
{
    if ((candidate != TRON_DIR_NONE) &&
        (tron_is_opposite(player->dir, candidate) == 0u)) {
        player->dir = candidate;
    }
}

static void tron_next_position(const tron_player_t *player, int16_t *next_x, int16_t *next_y)
{
    *next_x = player->x;
    *next_y = player->y;

    switch (player->dir) {
        case TRON_DIR_UP:
            (*next_y)--;
            break;
        case TRON_DIR_RIGHT:
            (*next_x)++;
            break;
        case TRON_DIR_DOWN:
            (*next_y)++;
            break;
        case TRON_DIR_LEFT:
            (*next_x)--;
            break;
        case TRON_DIR_NONE:
        default:
            break;
    }
}

static uint8_t tron_hits_wall(int16_t grid_x, int16_t grid_y)
{
    return ((grid_x <= 0) ||
            (grid_x >= (TRON_GRID_W - 1)) ||
            (grid_y <= 0) ||
            (grid_y >= (TRON_GRID_H - 1))) ? 1u : 0u;
}

static uint8_t tron_step_ticks(uint32_t play_elapsed_ticks)
{
    uint32_t reductions = play_elapsed_ticks / TRON_SPEED_RAMP_TICKS;
    uint32_t max_reductions = TRON_STEP_START_TICKS - TRON_STEP_MIN_TICKS;

    if (reductions >= max_reductions) {
        return TRON_STEP_MIN_TICKS;
    }

    return (uint8_t)(TRON_STEP_START_TICKS - reductions);
}

static void tron_setup_round(void)
{
    memset(tron_grid, TRON_GRID_EMPTY, sizeof(tron_grid));

    tron_players[0].x = 15;
    tron_players[0].y = 30;
    tron_players[0].dir = TRON_DIR_RIGHT;
    tron_players[0].grid_value = TRON_GRID_P1_TRAIL;
    tron_players[0].trail_colour = TRON_COLOUR_P1_TRAIL;
    tron_players[0].head_colour = TRON_COLOUR_P1_HEAD;

    tron_players[1].x = 64;
    tron_players[1].y = 30;
    tron_players[1].dir = TRON_DIR_LEFT;
    tron_players[1].grid_value = TRON_GRID_P2_TRAIL;
    tron_players[1].trail_colour = TRON_COLOUR_P2_TRAIL;
    tron_players[1].head_colour = TRON_COLOUR_P2_HEAD;

    tron_countdown_phase = -1;
}

static void tron_draw_round_start(void)
{
    LCD_Fill_Buffer(TRON_COLOUR_BG);
    tron_draw_border();
    tron_draw_cell(tron_players[0].x, tron_players[0].y, tron_players[0].head_colour);
    tron_draw_cell(tron_players[1].x, tron_players[1].y, tron_players[1].head_colour);
}

static void tron_draw_idle(void)
{
    LCD_Fill_Buffer(TRON_COLOUR_BG);
    LCD_printString("TRON", 70, 45, TRON_COLOUR_P1_HEAD, 4);
    LCD_printString("LIGHT CYCLES", 32, 88, TRON_COLOUR_BORDER, 2);
    LCD_printString("Press P1 SW", 46, 150, TRON_COLOUR_BORDER, 2);
    LCD_printString("or P2 SW", 62, 172, TRON_COLOUR_P2_HEAD, 2);
}

static void tron_draw_round_end(void)
{
    LCD_Fill_Buffer(TRON_COLOUR_BG);

    if (tron_round_winner == 1u) {
        LCD_printString("P1 ROUND", 44, 60, TRON_COLOUR_P1_HEAD, 3);
    } else if (tron_round_winner == 2u) {
        LCD_printString("P2 ROUND", 44, 60, TRON_COLOUR_P2_HEAD, 3);
    } else {
        LCD_printString("DRAW", 82, 60, TRON_COLOUR_BORDER, 3);
    }

    tron_print_score(52, 126);
    LCD_printString("Next round", 52, 178, TRON_COLOUR_BORDER, 2);
}

static void tron_draw_match_end(void)
{
    LCD_Fill_Buffer(TRON_COLOUR_BG);

    if (tron_match_winner == 1u) {
        LCD_printString("P1 WINS", 52, 58, TRON_COLOUR_P1_HEAD, 3);
    } else {
        LCD_printString("P2 WINS", 52, 58, TRON_COLOUR_P2_HEAD, 3);
    }

    LCD_printString("MATCH", 72, 100, TRON_COLOUR_BORDER, 3);
    tron_print_score(52, 145);
    LCD_printString("Returning", 52, 194, TRON_COLOUR_BORDER, 2);
}

static void tron_draw_countdown_phase(int8_t phase)
{
    LCD_Draw_Rect(72, 88, 96, 52, TRON_COLOUR_BG, 1);

    if (phase > 0) {
        char text[] = "0";
        text[0] = (char)('0' + phase);
        LCD_printString(text, 110, 96, TRON_COLOUR_BORDER, 4);
        tron_beep(800u, 35u, 8u);
    } else {
        LCD_printString("GO", 96, 96, TRON_COLOUR_P1_HEAD, 4);
        tron_beep(1200u, 45u, 20u);
    }
}

static void tron_enter_state(tron_state_t next_state, uint32_t now)
{
    tron_state = next_state;
    tron_state_enter_tick = now;

    switch (next_state) {
        case TRON_IDLE:
            tron_round_winner = 0u;
            tron_match_winner = 0u;
            tron_draw_idle();
            break;
        case TRON_COUNTDOWN:
            tron_setup_round();
            tron_draw_round_start();
            break;
        case TRON_PLAYING:
            LCD_Draw_Rect(72, 88, 96, 52, TRON_COLOUR_BG, 1);
            tron_last_move_tick = now;
            break;
        case TRON_ROUND_END:
            tron_draw_round_end();
            tron_beep(260u, 60u, 30u);
            break;
        case TRON_MATCH_END:
            tron_draw_match_end();
            tron_beep(1800u, 55u, 20u);
            break;
        default:
            break;
    }
}

static uint8_t tron_update_idle(uint32_t now)
{
    if (tron_start_pressed() != 0u) {
        tron_enter_state(TRON_COUNTDOWN, now);
        return 1u;
    }

    return 0u;
}

static uint8_t tron_update_countdown(uint32_t now)
{
    uint32_t elapsed = now - tron_state_enter_tick;
    int8_t phase;

    if (elapsed >= TRON_COUNTDOWN_TICKS) {
        tron_enter_state(TRON_PLAYING, now);
        return 1u;
    }

    if (elapsed < 100u) {
        phase = 3;
    } else if (elapsed < 200u) {
        phase = 2;
    } else if (elapsed < 280u) {
        phase = 1;
    } else {
        phase = 0;
    }

    if (phase != tron_countdown_phase) {
        tron_countdown_phase = phase;
        tron_draw_countdown_phase(phase);
        return 1u;
    }

    return 0u;
}

static void tron_score_round(uint8_t p1_dead, uint8_t p2_dead)
{
    tron_round_winner = 0u;

    if ((p1_dead != 0u) && (p2_dead == 0u)) {
        tron_round_winner = 2u;
        tron_players[1].score++;
    } else if ((p2_dead != 0u) && (p1_dead == 0u)) {
        tron_round_winner = 1u;
        tron_players[0].score++;
    }

    if (tron_players[0].score >= TRON_ROUNDS_TO_WIN) {
        tron_match_winner = 1u;
    } else if (tron_players[1].score >= TRON_ROUNDS_TO_WIN) {
        tron_match_winner = 2u;
    } else {
        tron_match_winner = 0u;
    }
}

static uint8_t tron_update_playing(uint32_t now)
{
    Joystick_t joystick1;
    Joystick_t joystick2;
    int16_t next_x[2];
    int16_t next_y[2];
    uint8_t alive_next[2] = {1u, 1u};
    uint8_t i;

    if ((now - tron_last_move_tick) < tron_step_ticks(now - tron_state_enter_tick)) {
        return 0u;
    }

    tron_last_move_tick = now;

    Joystick_Read(&joystick_cfg, &joystick1);
    Joystick_Read(&joystick2_cfg, &joystick2);

    tron_apply_direction(&tron_players[0], tron_quantise_joystick(&joystick1));
    tron_apply_direction(&tron_players[1], tron_quantise_joystick(&joystick2));

    for (i = 0u; i < 2u; i++) {
        tron_grid[tron_grid_index(tron_players[i].x, tron_players[i].y)] =
            tron_players[i].grid_value;
        tron_draw_cell(tron_players[i].x, tron_players[i].y, tron_players[i].trail_colour);
        tron_next_position(&tron_players[i], &next_x[i], &next_y[i]);
    }

    for (i = 0u; i < 2u; i++) {
        if ((tron_hits_wall(next_x[i], next_y[i]) != 0u) ||
            (tron_grid[tron_grid_index(next_x[i], next_y[i])] != TRON_GRID_EMPTY)) {
            alive_next[i] = 0u;
        }
    }

    if ((alive_next[0] != 0u) && (alive_next[1] != 0u) &&
        (next_x[0] == next_x[1]) && (next_y[0] == next_y[1])) {
        alive_next[0] = 0u;
        alive_next[1] = 0u;
    }

    if ((alive_next[0] == 0u) || (alive_next[1] == 0u)) {
        tron_score_round((uint8_t)(alive_next[0] == 0u), (uint8_t)(alive_next[1] == 0u));

        if (tron_match_winner != 0u) {
            tron_enter_state(TRON_MATCH_END, now);
        } else {
            tron_enter_state(TRON_ROUND_END, now);
        }

        return 1u;
    }

    for (i = 0u; i < 2u; i++) {
        tron_players[i].x = next_x[i];
        tron_players[i].y = next_y[i];
        tron_draw_cell(tron_players[i].x, tron_players[i].y, tron_players[i].head_colour);
    }

    return 1u;
}

static uint8_t tron_update_round_end(uint32_t now)
{
    if ((now - tron_state_enter_tick) >= TRON_ROUND_END_TICKS) {
        tron_enter_state(TRON_COUNTDOWN, now);
        return 1u;
    }

    return 0u;
}

static uint8_t tron_match_done(uint32_t now)
{
    return ((now - tron_state_enter_tick) >= TRON_MATCH_END_TICKS) ? 1u : 0u;
}

static void tron_init_joystick2(void)
{
    if (tron_joystick2_ready == 0u) {
        Joystick_Init(&joystick2_cfg);
        tron_joystick2_ready = 1u;
    }
}

void tron_run(void)
{
    uint8_t done = 0u;

    tron_init_joystick2();
    tron_players[0].score = 0u;
    tron_players[1].score = 0u;
    tron_buzzer_stop_tick = 0u;
    buzzer_off(&buzzer_cfg);

    tron_enter_state(TRON_IDLE, g_tim6_ticks);
    LCD_Refresh(&cfg0);

    while (done == 0u) {
        uint32_t now = g_tim6_ticks;
        uint8_t refresh = 0u;

        Input_Read();
        tron_update_buzzer(now);

        switch (tron_state) {
            case TRON_IDLE:
                refresh = tron_update_idle(now);
                break;
            case TRON_COUNTDOWN:
                refresh = tron_update_countdown(now);
                break;
            case TRON_PLAYING:
                refresh = tron_update_playing(now);
                break;
            case TRON_ROUND_END:
                refresh = tron_update_round_end(now);
                break;
            case TRON_MATCH_END:
                done = tron_match_done(now);
                break;
            default:
                done = 1u;
                break;
        }

        if (refresh != 0u) {
            LCD_Refresh(&cfg0);
        }
    }

    buzzer_off(&buzzer_cfg);
}
