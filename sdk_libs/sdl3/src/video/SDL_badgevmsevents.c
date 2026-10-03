/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2025 HP van Braam <hp@tmm.cx>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_BADGEVMS

#include "../../SDL3/src/events/SDL_events_c.h"
#include "../../SDL3/src/events/SDL_keyboard_c.h"

#include "SDL_badgevmsevents_c.h"
#include "SDL_badgevmsvideo.h"

void BADGEVMS_PumpEvents(SDL_VideoDevice *_this)
{
    // Get all windows on this display
    SDL_Window **windows = SDL_GetWindows(NULL);
    if (!windows) {
        return;
    }

    // Poll events from each window
    for (int i = 0; windows[i] != 0; i++) {
        SDL_Window *sdl_window = windows[i];
        if (!sdl_window || !sdl_window->internal) {
            continue;
        }

        SDL_WindowData *window_data = (SDL_WindowData *)sdl_window->internal;
        if (!window_data->badgevms_window) {
            continue;
        }

        event_t badgevms_event;
        SDL_Event sdl_event;

        while (true) {
            badgevms_event = window_event_poll(window_data->badgevms_window, false, 0);

            if (badgevms_event.type == EVENT_NONE) {
                break; // No more events from this window
            }

            SDL_zero(sdl_event);

            switch (badgevms_event.type) {
            case EVENT_QUIT:
                sdl_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&sdl_event);
                break;

            case EVENT_KEY_DOWN:
            case EVENT_KEY_UP:
            {
                // BadgeVMS only delivers key events to the foreground window
                if (SDL_GetKeyboardFocus() != sdl_window) {
                    SDL_SetKeyboardFocus(sdl_window);
                }

                // Go through SDL's keyboard code so SDL_GetKeyboardState()/SDL_GetModState() are updated
                SDL_Scancode scancode = (SDL_Scancode)badgevms_event.keyboard.scancode;
                SDL_SendKeyboardKey(0, SDL_DEFAULT_KEYBOARD_ID, (int)scancode, scancode, badgevms_event.keyboard.down);

                unsigned char ch = (unsigned char)badgevms_event.keyboard.text;
                if (badgevms_event.keyboard.down && ch >= 0x20 && ch <= 0x7e) {
                    char text[2] = { (char)ch, 0 };
                    SDL_SendKeyboardText(text);
                }
                break;
            }

            case EVENT_WINDOW_RESIZE:
                break;

            default:
                // Unknown event type, ignore
                break;
            }
        }
    }

    SDL_free(windows);
}

#endif // SDL_VIDEO_DRIVER_BADGEVMS
