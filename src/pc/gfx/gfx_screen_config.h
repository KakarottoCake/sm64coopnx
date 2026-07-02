#ifndef GFX_SCREEN_CONFIG_H
#define GFX_SCREEN_CONFIG_H

#ifdef __SWITCH__
// matches the Switch's actual display surface; nvnflinger's buffer queue is
// tied to a fixed-size layer, not an arbitrary desktop-style resolution
#define DESIRED_SCREEN_WIDTH 1280
#define DESIRED_SCREEN_HEIGHT 720
#else
#define DESIRED_SCREEN_WIDTH 1024
#define DESIRED_SCREEN_HEIGHT 768
#endif

#endif
