#ifndef SKY_HOP_GAME_H
#define SKY_HOP_GAME_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Original compact arcade art: cream/sky, ink, teal and gold. A 4-unit spacing grid,
 * bespoke 5x7 bitmap labels, no decorative cards. READY -> PLAYING -> DEAD -> restart;
 * PAUSED retains every simulation value. One action: flap. Fixed 120 Hz, no heap per tick. */
#define PIPE_COUNT 4
#define PIPE_W     42.f
#define GAP        100.f
#define BIRD_X     90.f
#define RADIUS     8.f
#define FLOOR      350.f
#define STEP       (1.0 / 120.0)
typedef enum mode { READY, PLAYING, DEAD, PAUSED } mode;
typedef struct pipe_pair {
    float x, gap;
    bool scored;
} pipe_pair;
typedef struct game {
    mode mode;
    float y, velocity, time;
    unsigned score, best;
    uint32_t random;
    pipe_pair pipes[PIPE_COUNT];
} game;
static float next_gap(game *g) {
    g->random = g->random * 1664525u + 1013904223u;
    return 132.f + (float)((g->random >> 8) % 120);
}
static void game_reset(game *g) {
    unsigned best = g->best;
    memset(g, 0, sizeof *g);
    g->best = best;
    g->y = 190;
    g->random = 0x534b5948;
    for (unsigned i = 0; i < PIPE_COUNT; ++i)
        g->pipes[i] = (pipe_pair){360 + i * 174.f, next_gap(g), false};
}
static void game_flap(game *g) {
    if (g->mode == PAUSED)
        return;
    if (g->mode == DEAD)
        game_reset(g);
    g->mode = PLAYING;
    g->velocity = -220;
}
static void game_pause(game *g) {
    if (g->mode == PLAYING)
        g->mode = PAUSED;
    else if (g->mode == PAUSED)
        g->mode = PLAYING;
}
static void game_tick(game *g, double dt) {
    if (g->mode == PAUSED || g->mode == DEAD)
        return;
    g->time += (float)dt;
    if (g->mode != PLAYING)
        return;
    g->velocity += 650.f * (float)dt;
    g->y += g->velocity * (float)dt;
    for (unsigned i = 0; i < PIPE_COUNT; ++i) {
        pipe_pair *p = &g->pipes[i];
        p->x -= 65.f * (float)dt;
        if (!p->scored && p->x + PIPE_W < BIRD_X - RADIUS) {
            p->scored = true;
            ++g->score;
            if (g->score > g->best)
                g->best = g->score;
        }
        if (p->x + PIPE_W < 0) {
            float right = 0;
            for (unsigned j = 0; j < PIPE_COUNT; ++j)
                if (g->pipes[j].x > right)
                    right = g->pipes[j].x;
            p->x = right + 174;
            p->gap = next_gap(g);
            p->scored = false;
        }
        float near_x = fmaxf(p->x, fminf(BIRD_X, p->x + PIPE_W)), dx = BIRD_X - near_x;
        float top = p->gap - GAP * .5f, bottom = p->gap + GAP * .5f;
        float dy = g->y < top ? 0 : g->y - top;
        if (g->y >= bottom)
            dy = 0;
        else if (g->y > p->gap)
            dy = bottom - g->y;
        if (dx * dx + dy * dy < RADIUS * RADIUS)
            g->mode = DEAD;
    }
    if (g->y - RADIUS <= 48 || g->y + RADIUS >= FLOOR)
        g->mode = DEAD;
}
static int game_self_check(void) {
    game g = {0};
    game_reset(&g);
    if (g.mode != READY)
        return 1;
    game_flap(&g);
    game_tick(&g, STEP);
    if (g.mode != PLAYING || g.y >= 190)
        return 1;
    game_pause(&g);
    game frozen = g;
    for (int i = 0; i < 240; ++i)
        game_tick(&g, STEP);
    if (memcmp(&g, &frozen, sizeof g))
        return 1;
    game_pause(&g);
    g.y = 200;
    g.velocity = 0;
    g.pipes[0] = (pipe_pair){BIRD_X - RADIUS - PIPE_W - .1f, 200, false};
    game_tick(&g, STEP);
    if (g.score != 1)
        return 1;
    game_tick(&g, STEP);
    if (g.score != 1)
        return 1;
    g.pipes[0] = (pipe_pair){BIRD_X - 10, 120, false};
    g.y = 260;
    game_tick(&g, STEP);
    if (g.mode != DEAD)
        return 1;
    game_flap(&g);
    if (g.mode != PLAYING || g.score || g.best != 1)
        return 1;
    g.y = FLOOR;
    game_tick(&g, STEP);
    if (g.mode != DEAD)
        return 1;
    puts(
        "Sky Hop self-check: flap, gravity, pause, score once, pipe/floor collision, restart pass");
    return 0;
}
#endif
