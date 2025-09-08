/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

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
#include "../../SDL2/src/SDL_internal.h"

#include "SDL_timer.h"

#ifdef SDL_VIDEO_DRIVER_BADGEVMS

#include "../../SDL2/src/events/SDL_events_c.h"

#include "SDL_badgevmsvideo.h"
#include "SDL_badgevmsevents_c.h"
#include "SDL_badgevmsframebuffer_c.h"

void BADGEVMS_PumpEvents(_THIS)
{
	if (!_this) {
		return;
	}
	
	SDL_Window *window;

	// Poll events from each window
	for (window = _this->windows; window; window = window->next) {
		SDL_WindowData *window_data = (SDL_WindowData *)window->driverdata;

		if (!window_data || !window_data->badgevms_window) {
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
				sdl_event.type = SDL_QUIT;
				SDL_PushEvent(&sdl_event);
				break;

			case EVENT_KEY_DOWN:
			case EVENT_KEY_UP:
				sdl_event.type = badgevms_event.keyboard.down ? SDL_KEYDOWN : SDL_KEYUP;
				sdl_event.key.timestamp = SDL_GetTicks();
				sdl_event.key.windowID = SDL_GetWindowID(window);
				sdl_event.key.state = badgevms_event.keyboard.down ? SDL_PRESSED : SDL_RELEASED;
				sdl_event.key.repeat = badgevms_event.keyboard.repeat;
				sdl_event.key.keysym.scancode = badgevms_event.keyboard.scancode;
				sdl_event.key.keysym.sym = SDL_GetKeyFromScancode(badgevms_event.keyboard.scancode);
				sdl_event.key.keysym.mod = badgevms_event.keyboard.mod;
				SDL_PushEvent(&sdl_event);
				break;

			case EVENT_WINDOW_RESIZE:
				break;

			default:
				// Unknown event type, ignore
				break;
			}
		}
	}
}

#endif /* SDL_VIDEO_DRIVER_BADGEVMS */

/* vi: set ts=4 sw=4 expandtab: */
