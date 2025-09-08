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

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../../SDL2/src/video/SDL_sysvideo.h"
#include "../../SDL2/src/video/SDL_pixels_c.h"
#include "../../SDL2/src/events/SDL_events_c.h"

#include "SDL_badgevmsvideo.h"
#include "SDL_badgevmsevents_c.h"
#include "SDL_badgevmsframebuffer_c.h"
#include "SDL_hints.h"

#define BADGEVMSVID_DRIVER_NAME       "badgevms"

/* Initialization/Query functions */
static int BADGEVMS_VideoInit(_THIS);
static void BADGEVMS_VideoQuit(_THIS);

/* BADGEVMS driver bootstrap functions */

static int BADGEVMS_Available(void)
{
    return 1;
}

static void BADGEVMS_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *BADGEVMS_CreateDevice(void)
{
    SDL_VideoDevice *device;

    if (!BADGEVMS_Available()) {
        return 0;
    }

    /* Initialize all variables that we clean on shutdown */
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return 0;
    }
    device->is_dummy = SDL_FALSE;

    /* Set the function pointers */
    device->VideoInit = BADGEVMS_VideoInit;
    device->VideoQuit = BADGEVMS_VideoQuit;
    device->PumpEvents = BADGEVMS_PumpEvents;
    device->CreateWindowFramebuffer = SDL_BADGEVMS_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = SDL_BADGEVMS_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SDL_BADGEVMS_DestroyWindowFramebuffer;

    device->free = BADGEVMS_DeleteDevice;

    return device;
}

VideoBootStrap BADGEVMS_bootstrap = {
    BADGEVMSVID_DRIVER_NAME, "SDL badgevms video driver",
    BADGEVMS_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};

int BADGEVMS_VideoInit(_THIS)
{
    SDL_DisplayMode mode;

    SDL_zero(mode);
    
	float refresh_rate;
	// BadgeVMS pixel formats are the same as SDL2
	get_screen_info(&mode.w, &mode.h, (pixel_format_t *)&mode.format, &refresh_rate);
	mode.refresh_rate = (int)refresh_rate;

    mode.driverdata = NULL;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }

    SDL_AddDisplayMode(&_this->displays[0], &mode);

    return 0;
}

void BADGEVMS_VideoQuit(_THIS)
{
}

#endif /* SDL_VIDEO_DRIVER_BADGEVMS */

/* vi: set ts=4 sw=4 expandtab: */
