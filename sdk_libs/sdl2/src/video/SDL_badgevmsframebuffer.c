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

#ifdef SDL_VIDEO_DRIVER_BADGEVMS

#include "../../SDL2/src/video/SDL_sysvideo.h"
#include "SDL_badgevmsframebuffer_c.h"
#include "SDL_badgevmsvideo.h"

#define BADGEVMS_SURFACE "_SDL_BadgeVMSSurface"

int SDL_BADGEVMS_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
	/* Free the old framebuffer surface */
    SDL_BADGEVMS_DestroyWindowFramebuffer(_this, window);

	SDL_WindowData *data = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
	if (!data) {
		return -1;
	}

	SDL_DisplayMode mode;
	SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(window), &mode);

	window_size_t size = { window->w, window->h };
	data->badgevms_window = window_create(window->title, size, WINDOW_FLAG_DOUBLE_BUFFERED);
	if (!data->badgevms_window) {
		SDL_free(data);
		return SDL_SetError("Could not create BadgeVMS window");
	}

	data->window = window;
	window->driverdata = data;

	// BadgeVMS pixel formats are the same as SDL2
	framebuffer_t *newfb = window_framebuffer_create(data->badgevms_window, size, (pixel_format_t)mode.format);
	
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, newfb->w, newfb->h, 16, mode.format);
    if (!surface) {
        return -1;
    }

    /* Save the info and return! */
    SDL_SetWindowData(window, BADGEVMS_SURFACE, surface);
    *format = mode.format;
    *pixels = newfb->pixels; //surface->pixels;
    *pitch = surface->pitch;
    return 0;
}

int SDL_BADGEVMS_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_Surface *surface = (SDL_Surface *)SDL_GetWindowData(window, BADGEVMS_SURFACE);
    if (!surface) {
        return SDL_SetError("Couldn't find BadgeVMS surface for window");
    }

    SDL_WindowData *data = window->driverdata;

    if (!data || !data->badgevms_window) {
        return SDL_SetError("Window not properly initialized");
    }

    window_present(data->badgevms_window, true, NULL, 0);
    
	return 0;
}

void SDL_BADGEVMS_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    SDL_Surface *surface = (SDL_Surface *)SDL_SetWindowData(window, BADGEVMS_SURFACE, NULL);
	SDL_FreeSurface(surface);
}

#endif /* SDL_VIDEO_DRIVER_BADGEVMS */

/* vi: set ts=4 sw=4 expandtab: */
