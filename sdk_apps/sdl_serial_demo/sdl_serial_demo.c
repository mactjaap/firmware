#include <SDL3/SDL.h>
#include <stdio.h>

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // conservative size similar to Snake (576x432) so the renderer can match
    int w = 576, h = 432;

    SDL_Window *win = SDL_CreateWindow("SDL Serial Demo", w, h, 0);
    if (!win) { printf("SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {
        printf("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        return 1;
    }

    printf("SDL demo ready. Arrow keys move; ESC exits.\n");

    int x = w/2 - 40, y = h/2 - 40, s = 80, vx = 2, vy = 2;
    int run = 1;

    while (run) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) run = 0;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                    case SDLK_UP:    y -= 20; printf("Key UP\n"); break;
                    case SDLK_DOWN:  y += 20; printf("Key DOWN\n"); break;
                    case SDLK_LEFT:  x -= 20; printf("Key LEFT\n"); break;
                    case SDLK_RIGHT: x += 20; printf("Key RIGHT\n"); break;
                    case SDLK_ESCAPE: run = 0; break;
                }
            }
        }

        x += vx; y += vy;
        if (x < 0 || x + s > w) { vx = -vx; x += vx; }
        if (y < 0 || y + s > h) { vy = -vy; y += vy; }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        SDL_SetRenderDrawColor(ren, 255, 120, 0, 255);
        SDL_FRect r = { (float)x, (float)y, (float)s, (float)s };
        SDL_RenderFillRect(ren, &r);

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
