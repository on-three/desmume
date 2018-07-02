/* main.c - this file is part of DeSmuME
 *
 * Copyright (C) 2006-2017 DeSmuME Team
 * Copyright (C) 2007 Pascal Giard (evilynux)
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
#include <SDL.h>
#include <SDL_thread.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__EMSCRIPTEN__)
#define USE_GLIB
#endif

#if defined(USE_GLIB)
#include <glib.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#ifndef VERSION
#define VERSION "Unknown version"
#endif

/*
 * FIXME: Not sure how to detect OpenGL in a platform portable way.
 */
#if defined(HAVE_GL_GL_H) && !SDL_VERSION_ATLEAST(2,0,0)
#define INCLUDE_OPENGL_2D
#endif

#ifdef INCLUDE_OPENGL_2D
#include <GL/gl.h>
#include <GL/glu.h>
#endif

#ifndef CLI_UI
#define CLI_UI
#endif

#include "../NDSSystem.h"
#include "../driver.h"
#include "../GPU.h"
#include "../SPU.h"
#include "../shared/sndsdl.h"
#include "../shared/ctrlssdl.h"
#include "../render3D.h"
#include "../rasterize.h"
#include "../saves.h"
#include "../frontend/modules/osd/agg/agg_osd.h"
#if defined(USE_GLIB)
#include "../shared/desmume_config.h"
#endif
#include "../commandline.h"
#include "../slot2.h"
#include "../utils/xstring.h"
#include "../path.h"

#ifdef GDB_STUB
#include "../armcpu.h"
#include "../gdbstub.h"
#endif

volatile bool execute = false;

static float nds_screen_size_ratio = 1.0f;

#define DISPLAY_FPS

#ifdef DISPLAY_FPS
#define NUM_FRAMES_TO_TIME 60
#endif

#define FPS_LIMITER_FPS 60

#if !SDL_VERSION_ATLEAST(2,0,0)
static SDL_Surface * surface;
#else
static SDL_Surface * surface = 0;
static SDL_Window* screen = 0;
static SDL_Renderer *renderer = 0;
static SDL_Texture *screenTexture = 0;
#endif
/* Flags to pass to SDL_SetVideoMode */
static int sdl_videoFlags;

SoundInterface_struct *SNDCoreList[] = {
  &SNDDummy,
  &SNDDummy,
  &SNDSDL,
  NULL
};

// expose some controls to javascript to manipulate sound volume
// volume control
#if defined(__EMSCRIPTEN__)

extern "C" {

static int sndvolume = 100;

EMSCRIPTEN_KEEPALIVE
void SetVolume(int volume)
{
  sndvolume = std::max(100, volume);
  sndvolume = std::min(0, sndvolume);
	#if 1
	SPU_SetVolume(sndvolume);
  #else
  SNDSDLSetVolume(sndvolume);
  #endif
}

EMSCRIPTEN_KEEPALIVE
void DecreaseVolume()
{
  #if 1
  printf("%s:%d:%s initial volume %d\n", __FILE__, __LINE__, __func__, sndvolume);
  #endif
	sndvolume = std::max(0, sndvolume - 10);
  #if 1
	SPU_SetVolume(sndvolume);
  #else
  SNDSDLSetVolume(sndvolume);
  #endif
  #if 1
  printf("%s:%d:%s final volume %d\n", __FILE__, __LINE__, __func__, sndvolume);
  #endif
}

EMSCRIPTEN_KEEPALIVE
void IncreaseVolume()
{
  #if 1
  printf("%s:%d:%s initial volume %d\n", __FILE__, __LINE__, __func__, sndvolume);
  #endif
	sndvolume = std::min(100, sndvolume + 10);
	#if 1
	SPU_SetVolume(sndvolume);
  #else
  SNDSDLSetVolume(sndvolume);
  #endif
  #if 1
  printf("%s:%d:%s final volume %d\n", __FILE__, __LINE__, __func__, sndvolume);
  #endif
}

}// extern "C"
#endif

GPU3DInterface *core3DList[] = {
&gpu3DNull,
&gpu3DRasterize,
NULL
};

const char * save_type_names[] = {
    "Autodetect",
    "EEPROM 4kbit",
    "EEPROM 64kbit",
    "EEPROM 512kbit",
    "FRAM 256kbit",
    "FLASH 2mbit",
    "FLASH 4mbit",
    NULL
};


/* Our keyboard config is different because of the directional keys */
/* Please note that m is used for fake microphone */
const u16 cli_kb_cfg[NB_KEYS] =
  { 
    SDLK_x,         // A
    SDLK_z,         // B
    SDLK_RSHIFT,    // select
    SDLK_RETURN,    // start
    SDLK_RIGHT,     // Right
    SDLK_LEFT,      // Left
    SDLK_UP,        // Up
    SDLK_DOWN,      // Down
    SDLK_w,         // R
    SDLK_q,         // L
    SDLK_s,         // X
    SDLK_a,         // Y
    SDLK_p,         // DEBUG
    SDLK_o,         // BOOST
    SDLK_BACKSPACE, // Lid
  };

class configured_features : public CommandLine
{
public:
  int auto_pause;
  int frameskip;

  int engine_3d;
  int savetype;
  
#ifdef INCLUDE_OPENGL_2D
  int opengl_2d;
#endif

  int firmware_language;

public:
  void printConfig() const
  {
    printf("Configured features:\n");
    printf("auto_pause: %d\n", auto_pause);
    printf("frameskip: %d\n", frameskip);
    printf("engine_3d %d\n", engine_3d);
    printf("savetype: %d\n", savetype);
    #if defined(INCLUDE_OPENGL_2D)
    printf("opengl_2d: %d\n", opengl_2d);
    #endif
    printf("firmware_language: %d\n", firmware_language);
  }

};

static void
init_config( class configured_features *config) {

  config->auto_pause = 0;
  config->frameskip = 0;

  config->engine_3d = 1;
  config->savetype = 0;

#ifdef INCLUDE_OPENGL_2D
  config->opengl_2d = 0;
#endif

  /* use the default language */
  config->firmware_language = -1;
}

#if !defined(USE_GLIB)
// Simple implementations if GLIB lacking
void g_printerr(const char* str)
{
  fprintf(stderr, "%s\n", str);
}
#endif


static int
fill_config( class configured_features *config,
             int argc, char ** argv) {
  #if defined(USE_GLIB)
  GOptionEntry options[] = {
    { "auto-pause", 0, 0, G_OPTION_ARG_NONE, &config->auto_pause, "Pause emulation if focus is lost", NULL},
    { "frameskip", 0, 0, G_OPTION_ARG_INT, &config->frameskip, "Set frameskip", "FRAMESKIP"},
    { "3d-engine", 0, 0, G_OPTION_ARG_INT, &config->engine_3d, "Select 3d rendering engine. Available engines:\n"
        "\t\t\t\t\t\t  0 = 3d disabled\n"
        "\t\t\t\t\t\t  1 = internal rasterizer (default)\n"
        ,"ENGINE"},
    { "save-type", 0, 0, G_OPTION_ARG_INT, &config->savetype, "Select savetype from the following:\n"
    "\t\t\t\t\t\t  0 = Autodetect (default)\n"
    "\t\t\t\t\t\t  1 = EEPROM 4kbit\n"
    "\t\t\t\t\t\t  2 = EEPROM 64kbit\n"
    "\t\t\t\t\t\t  3 = EEPROM 512kbit\n"
    "\t\t\t\t\t\t  4 = FRAM 256kbit\n"
    "\t\t\t\t\t\t  5 = FLASH 2mbit\n"
    "\t\t\t\t\t\t  6 = FLASH 4mbit\n",
    "SAVETYPE"},
#ifdef INCLUDE_OPENGL_2D
    { "opengl-2d", 0, 0, G_OPTION_ARG_NONE, &config->opengl_2d, "Enables using OpenGL for screen rendering", NULL},
#endif
    { "fwlang", 0, 0, G_OPTION_ARG_INT, &config->firmware_language, "Set the language in the firmware, LANG as follows:\n"
    "\t\t\t\t\t\t  0 = Japanese\n"
    "\t\t\t\t\t\t  1 = English\n"
    "\t\t\t\t\t\t  2 = French\n"
    "\t\t\t\t\t\t  3 = German\n"
    "\t\t\t\t\t\t  4 = Italian\n"
    "\t\t\t\t\t\t  5 = Spanish\n",
    "LANG"},
    { NULL }
  };

  #endif

  //g_option_context_add_main_entries (config->ctx, options, "options");
  config->parse(argc,argv);

  if(!config->validate())
    goto error;

  if (config->savetype < 0 || config->savetype > 6) {
    g_printerr("Accepted savetypes are from 0 to 6.\n");
    return false;
  }

  if (config->firmware_language < -1 || config->firmware_language > 5) {
    g_printerr("Firmware language must be set to a value from 0 to 5.\n");
    goto error;
  }

  if (config->engine_3d != 0 && config->engine_3d != 1) {
    g_printerr("Currently available engines: 0, 1.\n");
    goto error;
  }

  if (config->frameskip < 0) {
    g_printerr("Frameskip must be >= 0.\n");
    goto error;
  }

  if (config->nds_file == "") {
    g_printerr("Need to specify file to load.\n");
    goto error;
  }

#ifdef GDB_STUB
  if (config->arm9_gdb_port != 0 && (config->arm9_gdb_port < 1 || config->arm9_gdb_port > 65535)) {
    g_printerr("ARM9 GDB stub port must be in the range 1 to 65535\n");
    goto error;
  }

  if (config->arm7_gdb_port != 0 && (config->arm7_gdb_port < 1 || config->arm7_gdb_port > 65535)) {
    g_printerr("ARM7 GDB stub port must be in the range 1 to 65535\n");
    goto error;
  }
#endif

  config->printConfig();

  return 1;

error:
  config->errorHelp(argv[0]);

  return 0;
}

/*
 * The thread handling functions needed by the GDB stub code.
 */
#ifdef GDB_STUB
void *
createThread_gdb( void (*thread_function)( void *data),
                  void *thread_data) {
  SDL_Thread *new_thread = SDL_CreateThread( (int (*)(void *data))thread_function,
                                             thread_data);

  return new_thread;
}

void
joinThread_gdb( void *thread_handle) {
  int ignore;
  SDL_WaitThread( (SDL_Thread*)thread_handle, &ignore);
}
#endif

#ifdef INCLUDE_OPENGL_2D
/* initialization openGL function */
static int
initGL( GLuint *screen_texture) {
  GLenum errCode;
  u16 blank_texture[256 * 256];

  memset(blank_texture, 0, sizeof(blank_texture));

  /* Enable Texture Mapping */
  glEnable( GL_TEXTURE_2D );

  /* Set the background black */
  glClearColor( 0.0f, 0.0f, 0.0f, 0.5f );

  /* Depth buffer setup */
  glClearDepth( 1.0f );

  /* Enables Depth Testing */
  glEnable( GL_DEPTH_TEST );

  /* The Type Of Depth Test To Do */
  glDepthFunc( GL_LEQUAL );

  /* Create The Texture */
  glGenTextures(2, screen_texture);

  for (int i = 0; i < 2; i++)
  {
    glBindTexture(GL_TEXTURE_2D, screen_texture[i]);

    /* Generate The Texture */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256,
      0, GL_RGBA,
      GL_UNSIGNED_SHORT_1_5_5_5_REV,
      blank_texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Linear Filtering */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }

  if ((errCode = glGetError()) != GL_NO_ERROR) {
    const GLubyte *errString;

    // currently issues linking with gluErrorString on emscripten
    #if !defined(__EMSCRIPTEN__)
    errString = gluErrorString(errCode);
    fprintf( stderr, "Failed to init GL: %s\n", errString);
    #else
    fprintf(stderr, "Failed to init GL via errCode: %d\n", errCode);
    #endif
    return 0;
  }

  return 1;
}

static void
resizeWindow( u16 width, u16 height, GLuint *screen_texture) {

  #if !SDL_VERSION_ATLEAST(2,0,0)

  int comp_width = 3 * width;
  int comp_height = 2 * height;
  GLenum errCode;

  #if 1
  surface = SDL_SetVideoMode(width, height, 32, sdl_videoFlags);
  #else
  surface = SDL_CreateWindow("My Game Window",
                          SDL_WINDOWPOS_UNDEFINED,
                          SDL_WINDOWPOS_UNDEFINED,
                          width, height,
                          sdl_videoFlags);
  #endif
  initGL(screen_texture);

#ifdef HAVE_LIBAGG
  Hud.reset();
#endif

  if ( comp_width > comp_height) {
    width = 2*height/3;
  }
  height = 3*width/2;
  nds_screen_size_ratio = 256.0 / (double)width;

  /* Setup our viewport. */
  glViewport( 0, 0, ( GLint )width, ( GLint )height );

  /*
   * change to the projection matrix and set
   * our viewing volume.
   */
  glMatrixMode( GL_PROJECTION );
  glLoadIdentity( );

  gluOrtho2D( 0.0, 256.0, 384.0, 0.0);

  /* Make sure we're chaning the model view and not the projection */
  glMatrixMode( GL_MODELVIEW );

  /* Reset The View */
  glLoadIdentity( );

  if ((errCode = glGetError()) != GL_NO_ERROR) {
    const GLubyte *errString;

    #if !defined(__EMSCRIPTEN__)
    errString = gluErrorString(errCode);
    fprintf( stderr, "GL resize failed: %s\n", errString);
    #else
    fprintf( stderr, "GL resize failed: %d\n", errCode);
    #endif
  }
  #endif // !SDL_VERSION_ATLEAST(2,0,0)
}


static void
opengl_Draw(GLuint *texture) {
  const NDSDisplayInfo &displayInfo = GPU->GetDisplayInfo();

  /* Clear The Screen And The Depth Buffer */
  glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

  /* Move Into The Screen 5 Units */
  glLoadIdentity( );

  /* Draw the main screen as a textured quad */
  glBindTexture(GL_TEXTURE_2D, texture[NDSDisplayID_Main]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT,
                  GL_RGBA,
                  GL_UNSIGNED_SHORT_1_5_5_5_REV,
                  displayInfo.renderedBuffer[NDSDisplayID_Main]);

  GLfloat backlightIntensity = displayInfo.backlightIntensity[NDSDisplayID_Main];

  glBegin(GL_QUADS);
    glTexCoord2f(0.00f, 0.00f); glVertex2f(  0.0f,   0.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
    glTexCoord2f(1.00f, 0.00f); glVertex2f(256.0f,   0.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
    glTexCoord2f(1.00f, 0.75f); glVertex2f(256.0f, 192.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
    glTexCoord2f(0.00f, 0.75f); glVertex2f(  0.0f, 192.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
  glEnd();

  /* Draw the touch screen as a textured quad */
  glBindTexture(GL_TEXTURE_2D, texture[NDSDisplayID_Touch]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT,
                  GL_RGBA,
                  GL_UNSIGNED_SHORT_1_5_5_5_REV,
                  displayInfo.renderedBuffer[NDSDisplayID_Touch]);

  backlightIntensity = displayInfo.backlightIntensity[NDSDisplayID_Touch];

  glBegin(GL_QUADS);
    glTexCoord2f(0.00f, 0.00f); glVertex2f(  0.0f, 192.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
    glTexCoord2f(1.00f, 0.00f); glVertex2f(256.0f, 192.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
    glTexCoord2f(1.00f, 0.75f); glVertex2f(256.0f, 384.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
    glTexCoord2f(0.00f, 0.75f); glVertex2f(  0.0f, 384.0f); glColor4f(backlightIntensity, backlightIntensity, backlightIntensity, 1.0f);
  glEnd();

  /* Flush the drawing to the screen */
  #if !SDL_VERSION_ATLEAST(2,0,0)
  SDL_GL_SwapBuffers( );
  #endif
}
#endif

/* this is a stub for resizeWindow_stub in the case of no gl headers or no opengl 2d */
#ifdef INCLUDE_OPENGL_2D
static void
resizeWindow_stub (u16 width, u16 height, GLuint *screen_texture) {
}
#else
static void
resizeWindow_stub (u16 width, u16 height, void *screen_texture) {
}
#endif

#if defined(__EMSCRIPTEN__)

void PutPixel16_nolock(SDL_Surface * surface, int x, int y, Uint32 color)
{
    Uint8 * pixel = (Uint8*)surface->pixels;
    pixel += (y * surface->pitch) + (x * sizeof(Uint16));
    *((Uint16*)pixel) = color & 0xFFFF;
}


/*
 as of emscripten 1.38.6 emscripten only has a partial impl of SDL_CreateRGBSurfaceFrom
 so this naieve implementation is needed.
 TODO: move to GL rendering as this is processing intensive.
 */
SDL_Surface * _SDL_CreateRGBSurfaceFrom(void *pixels,
                          int width, int height, int depth, int pitch,
                          Uint32 Rmask, Uint32 Gmask, Uint32 Bmask,
                         Uint32 Amask)
{
  SDL_Surface *surface;
  surface = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, depth,
                                   Rmask, Gmask, Bmask, Amask);
  if (surface != NULL) {

    #if 0 // DEBUG
    printf("%s:%d:%s width: %d height: %d depth: %d pitch %d rmask: %x gmask %x bmask %x amask: %x\n", __FILE__, __LINE__, __func__, width, height, depth, pitch, Rmask, Gmask, Bmask, Amask);
    //main.cpp:457:_SDL_CreateRGBSurfaceFrom width: 256 height: 384 depth: 16 pitch 512 rmask: 1f gmask 3e0 bmask 7c00 amask: 0
    //r: 0001 1111 --> 5 bits
    //g: 0011 1110 0000 --> 5 bits
    //b: 0111 1100 0000 0000 --> 5 bits
    #endif

    #if 0 // DEBUG
    printf("Created surface: width: %d height: %d pitch %d bits per pixel %u bytes per pixel %u\n", surface->w, surface->h, surface->pitch, surface->format->BitsPerPixel, surface->format->BytesPerPixel);
    //Created surface: width: 256 height: 384 pitch 1024 bits per pixel 32 bytes per pixel 4
    printf("Created surface aMask: %x gMask: %x bMask: %x aMask %x\n", surface->format->Rmask, surface->format->Gmask, surface->format->Bmask, surface->format->Amask);
    // Created surface aMask: 1f gMask: 3e0 bMask: 7c00 aMask ff000000
    #endif

    SDL_LockSurface( surface );

    const unsigned int srcPixelPitch = pitch/(depth/8);
    const unsigned int destBytesPerPixel = surface->format->BytesPerPixel;
    for(int y = 0; y < height; ++y)
    {
      for(int x = 0; x < width; ++x)
      {
        Uint16* pSrc = (Uint16*)pixels + y * srcPixelPitch + x;
        Uint8* pDest = ((Uint8*)surface->pixels) + y * surface->pitch + x * destBytesPerPixel;

        Uint8 r = (pSrc[0] & Rmask);
        double _r = r * 255.0/32.0;
        Uint8 g = ((pSrc[0] & Gmask)>> 5);
        double _g = g * 255.0/32.0;
        Uint8 b = ((pSrc[0] & Bmask)>> 10);
        double _b = b * 255.0/32.0;

        pDest[0] = (Uint8)_r;
        pDest[1] = (Uint8)_g;
        pDest[2] = (Uint8)_b;
        pDest[3] = 0xff; // alpha
      }
    }
    //SDL_SetClipRect(surface, NULL);
    SDL_UnlockSurface(surface);
  }
  return surface;
}
#endif // __EMSCRIPTEN__


static void
Draw( void) {
  #if !SDL_VERSION_ATLEAST(2,0,0)
  const NDSDisplayInfo &displayInfo = GPU->GetDisplayInfo();
  const size_t pixCount = GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT;
  ColorspaceApplyIntensityToBuffer16<false, false>((u16 *)displayInfo.masterNativeBuffer, pixCount, displayInfo.backlightIntensity[NDSDisplayID_Main]);
  ColorspaceApplyIntensityToBuffer16<false, false>((u16 *)displayInfo.masterNativeBuffer + pixCount, pixCount, displayInfo.backlightIntensity[NDSDisplayID_Touch]);

  #if !defined(__EMSCRIPTEN__)
  SDL_Surface *rawImage = SDL_CreateRGBSurfaceFrom(displayInfo.masterNativeBuffer, GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2, 16, GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16), 0x001F, 0x03E0, 0x7C00, 0);
  #else
  SDL_Surface *rawImage = _SDL_CreateRGBSurfaceFrom(displayInfo.masterNativeBuffer, GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2, 16, GPU_FRAMEBUFFER_NATIVE_WIDTH * sizeof(u16), 0x001F, 0x03E0, 0x7C00, 0);
  #endif
  if(rawImage == NULL) return;

  SDL_BlitSurface(rawImage, 0, surface, 0);
  SDL_UpdateRect(surface, 0, 0, 0, 0);
  SDL_FreeSurface(rawImage);
#else // SDL 2+

  if(!surface && !screenTexture)
  {
    surface = SDL_CreateRGBSurface(0, GPU_FRAMEBUFFER_NATIVE_WIDTH,
                                  GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2,
                                  16, // DEPTH
                                  0x001F, 0x03E0, 0x7C00, 0);
    if(!surface)
    {
      ::fprintf(stderr, "%s%d%s failed to init rgb surface for screen texture.\n", __FILE__, __LINE__, __func__);
    }
  
    screenTexture = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_ABGR1555,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            GPU_FRAMEBUFFER_NATIVE_WIDTH,
                                            GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2);
    if(!screenTexture) {
      ::fprintf(stderr, "%s%d%s failed to init texture screen texture.\n", __FILE__, __LINE__, __func__);
    }
  }

  const NDSDisplayInfo &displayInfo = GPU->GetDisplayInfo();
  SDL_UpdateTexture(screenTexture,
                    NULL,
                    displayInfo.masterNativeBuffer,
                    surface->pitch);
  SDL_RenderCopy(renderer, screenTexture, NULL, NULL);
  SDL_RenderPresent(renderer);

  #endif // !SDL_VERSION_ATLEAST(2,0,0)
  
  return;
}

static void desmume_cycle(struct ctrls_event_config * cfg)
{
    SDL_Event event;

    cfg->nds_screen_size_ratio = nds_screen_size_ratio;

    /* Look for queued events and update keypad status */
    /* IMPORTANT: Reenable joystick events iif needed. */
    if(SDL_JoystickEventState(SDL_QUERY) == SDL_IGNORE)
      SDL_JoystickEventState(SDL_ENABLE);

    /* There's an event waiting to be processed? */
    #if !defined(__EMSCRIPTEN__)
    while ( !cfg->sdl_quit &&
        (SDL_PollEvent(&event) || (!cfg->focused && SDL_WaitEvent(&event))))
    #else
    if( !cfg->sdl_quit && SDL_PollEvent(&event))
    #endif
    {
        process_ctrls_event( event, cfg);
    }

    /* Update mouse position and click */
    if(mouse.down) NDS_setTouchPos(mouse.x, mouse.y);
    if(mouse.click)
      { 
        NDS_releaseTouch();
        mouse.click = FALSE;
      }

    update_keypad(cfg->keypad);     /* Update keypad */
    NDS_exec<false>();
    SPU_Emulate_user();
}

/*
  Emscritpen requires a non-blocking main loop, so it's traditional
  to break out the work we do each frame, as well as any cleanup into separate methods.
  emscripten_set_main_loop(update_loop, 60, 1);

*/

int limiter_frame_counter = 0;
int limiter_tick0 = 0;
int error;

#if defined(USE_GLIB)
GKeyFile *keyfile;
#endif

int now;

#ifdef DISPLAY_FPS
u32 fps_timing = 0;
u32 fps_frame_counter = 0;
u32 fps_previous_time = 0;
#endif

#ifdef INCLUDE_OPENGL_2D
GLuint screen_texture[2];
#endif
class configured_features my_config;
struct ctrls_event_config ctrls_cfg;

#if defined(__EMSCRIPTEN__)
void update_loop()
{
  #if 0
  static unsigned int _frameNum = 0;
  _frameNum++;
  printf("%s:%d:%s frame: %u\n", __FILE__, __LINE__, __func__, _frameNum);
  #endif

  desmume_cycle(&ctrls_cfg);

#ifdef HAVE_LIBAGG
  osd->update();
  DrawHUD();
#endif

#ifdef INCLUDE_OPENGL_2D
  if ( my_config.opengl_2d) {
    opengl_Draw(screen_texture);
    ctrls_cfg.resize_cb = &resizeWindow;
  }
  else
#endif
    Draw();

#ifdef HAVE_LIBAGG
  osd->clear();
#endif

  #if 0
  for ( int i = 0; i < my_config.frameskip; i++ )
  {
      NDS_SkipNextFrame();
      desmume_cycle(&ctrls_cfg);
  }
  #endif

#ifdef DISPLAY_FPS
  now = SDL_GetTicks();
#endif


  #if 0
  if ( !my_config.disable_limiter && !ctrls_cfg.boost) {
#ifndef DISPLAY_FPS
    now = SDL_GetTicks();
#endif
    int delay =  (limiter_tick0 + limiter_frame_counter*1000/FPS_LIMITER_FPS) - now;
    if (delay < -500 || delay > 100) { // reset if we fall too far behind don't want to run super fast until we catch up
      limiter_tick0 = now;
      limiter_frame_counter = 0;
    } else if (delay > 0) {
      SDL_Delay(delay);
    }
  }
  #endif

  // always count frames, we'll mess up if the limiter gets turned on later otherwise
  limiter_frame_counter += 1 + my_config.frameskip;

#ifdef DISPLAY_FPS
  fps_frame_counter += 1;
  fps_timing += now - fps_previous_time;
  fps_previous_time = now;

  if ( fps_frame_counter == NUM_FRAMES_TO_TIME) {
    char win_title[20];
    float fps = NUM_FRAMES_TO_TIME * 1000.f / fps_timing;

    fps_frame_counter = 0;
    fps_timing = 0;

    snprintf( win_title, sizeof(win_title), "Desmume %f", fps);

    #if !SDL_VERSION_ATLEAST(2,0,0)
    SDL_WM_SetCaption( win_title, NULL);
    #endif
  }
#endif
}
#endif

int main(int argc, char ** argv) {
  
  #if defined(__EMSCRIPTEN__)
  printf("%s:%d:%s\n", __FILE__, __LINE__, __func__);
  for(int i = 0; i < argc; ++i)
  {
    printf("%s:%d:%s argv[%d] = %s\n", __FILE__, __LINE__, __func__, i, argv[i]);
  }
  #endif

  /* this holds some info about our display */
  #if !SDL_VERSION_ATLEAST(2,0,0)
  const SDL_VideoInfo *videoInfo;
  #endif
  
  /* the firmware settings */
  struct NDS_fw_config_data fw_config;

  NDS_Init();

  /* default the firmware settings, they may get changed later */
  NDS_FillDefaultFirmwareConfigData( &fw_config);

  init_config( &my_config);

  if ( !fill_config( &my_config, argc, argv)) {
    exit(1);
  }

  #if defined(__EMSCRIPTEN__)

  // Set some paths standard paths for web version
  strncpy(path.pathToModule, "/", MAX_PATH);
  strncpy(path.pathToSlot1D, "/Slot1D", MAX_PATH);
  strncpy(path.pathToBattery, "/Battery", MAX_PATH);

  #endif

  /* use any language set on the command line */
  if ( my_config.firmware_language != -1) {
    fw_config.language = my_config.firmware_language;
  }

    my_config.process_addonCommands();

    int slot2_device_type = NDS_SLOT2_AUTO;

    if (my_config.is_cflash_configured)
        slot2_device_type = NDS_SLOT2_CFLASH;

    if(my_config.gbaslot_rom != "") {

        //set the GBA rom and sav paths
        GBACartridge_RomPath = my_config.gbaslot_rom.c_str();
        if(toupper(strright(GBACartridge_RomPath,4)) == ".GBA")
          GBACartridge_SRAMPath = strright(GBACartridge_RomPath,4) + ".sav";
        else
          //what to do? lets just do the same thing for now
          GBACartridge_SRAMPath = strright(GBACartridge_RomPath,4) + ".sav";

        // Check if the file exists and can be opened
        FILE * test = fopen(GBACartridge_RomPath.c_str(), "rb");
        if (test) {
            slot2_device_type = NDS_SLOT2_GBACART;
            fclose(test);
        }
    }

	switch (slot2_device_type)
	{
		case NDS_SLOT2_NONE:
			break;
		case NDS_SLOT2_AUTO:
			break;
		case NDS_SLOT2_CFLASH:
			break;
		case NDS_SLOT2_RUMBLEPAK:
			break;
		case NDS_SLOT2_GBACART:
			break;
		case NDS_SLOT2_GUITARGRIP:
			break;
		case NDS_SLOT2_EXPMEMORY:
			break;
		case NDS_SLOT2_EASYPIANO:
			break;
		case NDS_SLOT2_PADDLE:
			break;
		case NDS_SLOT2_PASSME:
			break;
		default:
			slot2_device_type = NDS_SLOT2_NONE;
			break;
	}
    
    slot2_Init();
    slot2_Change((NDS_SLOT2_TYPE)slot2_device_type);

  #if defined(USE_GLIB)
  if ( !g_thread_supported()) {
    g_thread_init( NULL);
  }
  #endif

  driver = new BaseDriver();
  
#ifdef GDB_STUB
  gdbstub_mutex_init();

  /*
   * Activate the GDB stubs
   * This has to come after NDS_Init() where the CPUs are set up.
   */
  gdbstub_handle_t arm9_gdb_stub = NULL;
  gdbstub_handle_t arm7_gdb_stub = NULL;
  
  if ( my_config.arm9_gdb_port > 0) {
    arm9_gdb_stub = createStub_gdb( my_config.arm9_gdb_port,
                                   &NDS_ARM9,
                                   &arm9_direct_memory_iface);
    
    if ( arm9_gdb_stub == NULL) {
      fprintf( stderr, "Failed to create ARM9 gdbstub on port %d\n",
              my_config.arm9_gdb_port);
      exit( 1);
    }
    else {
      activateStub_gdb( arm9_gdb_stub);
    }
  }
  if ( my_config.arm7_gdb_port > 0) {
    arm7_gdb_stub = createStub_gdb( my_config.arm7_gdb_port,
                                   &NDS_ARM7,
                                   &arm7_base_memory_iface);
    
    if ( arm7_gdb_stub == NULL) {
      fprintf( stderr, "Failed to create ARM7 gdbstub on port %d\n",
              my_config.arm7_gdb_port);
      exit( 1);
    }
    else {
      activateStub_gdb( arm7_gdb_stub);
    }
  }
#endif

  /* Create the dummy firmware */
  NDS_CreateDummyFirmware( &fw_config);

  if ( !my_config.disable_sound) {
    SPU_ChangeSoundCore(SNDCORE_SDL, 735 * 4);
  }

  if (!GPU->Change3DRendererByID(my_config.engine_3d)) {
    GPU->Change3DRendererByID(RENDERID_SOFTRASTERIZER);
    fprintf(stderr, "3D renderer initialization failed!\nFalling back to 3D core: %s\n", core3DList[RENDERID_SOFTRASTERIZER]->name);
  }

  backup_setManualBackupType(my_config.savetype);

  #if 1
  printf("%s:%d:%s my_config.nds_file: %s\n", __FILE__, __LINE__, __func__, my_config.nds_file.c_str());
  #endif

  error = NDS_LoadROM( my_config.nds_file.c_str() );
  if (error < 0) {
    fprintf(stderr, "error while loading %s\n", my_config.nds_file.c_str());
    exit(-1);
  }

  execute = true;

  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == -1)
    {
      fprintf(stderr, "Error trying to initialize SDL: %s\n",
              SDL_GetError());
      return 1;
    }

  #if !SDL_VERSION_ATLEAST(2,0,0)
  SDL_WM_SetCaption("Desmume SDL", NULL);
  #endif
  
  #if !SDL_VERSION_ATLEAST(2,0,0)
  /* Fetch the video info */
  videoInfo = SDL_GetVideoInfo( );
  if ( !videoInfo ) {
    fprintf( stderr, "Video query failed: %s\n", SDL_GetError( ) );
    exit( -1);
  }
  #endif

  /* This checks if hardware blits can be done */
  #if !SDL_VERSION_ATLEAST(2,0,0)
  if ( videoInfo->blit_hw )
    sdl_videoFlags |= SDL_HWACCEL;
  #endif

#if defined(INCLUDE_OPENGL_2D) && !SDL_VERSION_ATLEAST(2,0,0)
  if ( my_config.opengl_2d) {
    /* the flags to pass to SDL_SetVideoMode */
    sdl_videoFlags  = SDL_OPENGL;          /* Enable OpenGL in SDL */
    sdl_videoFlags |= SDL_HWPALETTE;       /* Store the palette in hardware */
    sdl_videoFlags |= SDL_RESIZABLE;       /* Enable window resizing */


    /* This checks to see if surfaces can be stored in memory */
    if ( videoInfo->hw_available )
      sdl_videoFlags |= SDL_HWSURFACE;
    else
      sdl_videoFlags |= SDL_SWSURFACE;


    /* Sets up OpenGL double buffering */
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

    surface = SDL_SetVideoMode( 256, 192 * 2, 32,
                                sdl_videoFlags );

    /* Verify there is a surface */
    if ( !surface ) {
      fprintf( stderr, "Video mode set failed: %s\n", SDL_GetError( ) );
      exit( -1);
    }


    /* initialize OpenGL */
    if ( !initGL( screen_texture)) {
      fprintf( stderr, "Failed to init GL, fall back to software render\n");

      my_config.opengl_2d = 0;
    }
  }

  if ( !my_config.opengl_2d) {
#endif

    #if !SDL_VERSION_ATLEAST(2,0,0)
    sdl_videoFlags |= SDL_SWSURFACE;
    surface = SDL_SetVideoMode(256, 384, 32, sdl_videoFlags);

    if ( !surface ) {
      fprintf( stderr, "Video mode set failed: %s\n", SDL_GetError( ) );
      exit( -1);
    }
    #else // !SDL_VERSION_ATLEAST(2,0,0)
    sdl_videoFlags = SDL_WINDOW_OPENGL;
    screen = SDL_CreateWindow("My Game Window",
                          SDL_WINDOWPOS_UNDEFINED,
                          SDL_WINDOWPOS_UNDEFINED,
                          GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT*2,
                          sdl_videoFlags);
    if(!screen)
    {
      ::fprintf(stderr,"Failure to initialize sdl screen.\n");
      return -1;
    }
    renderer = SDL_CreateRenderer(screen, -1, 0);
    if(!renderer)
    {
      ::fprintf(stderr, "Failure to initialize sdl renderer.\n");
      return -1;
    }
    #endif // !SDL_VERSION_ATLEAST(2,0,0)

#if defined(INCLUDE_OPENGL_2D) && !SDL_VERSION_ATLEAST(2,0,0)
  }

  /* set the initial window size */
  if ( my_config.opengl_2d) {
    resizeWindow( 256, 192*2, screen_texture);
  }
#endif

  /* Initialize joysticks */
  if(!init_joy()) return 1;

  #if defined(USE_GLIB)
  /* Load keyboard and joystick configuration */
  keyfile = desmume_config_read_file(cli_kb_cfg);
  desmume_config_dispose(keyfile);
  #endif

  /* Since gtk has a different mapping the keys stop to work with the saved configuration :| */
  load_default_config(cli_kb_cfg);

  if(my_config.load_slot != -1){
    loadstate_slot(my_config.load_slot);
  }

#ifdef HAVE_LIBAGG
  Desmume_InitOnce();
  Hud.reset();
  // Now that gtk port draws to RGBA buffer directly, the other one
  // has to use ugly ways to make HUD rendering work again.
  // desmume gtk: Sorry desmume-cli :(
  T_AGG_RGB555 agg_targetScreen_cli((u8 *)GPU->GetDisplayInfo().masterNativeBuffer, 256, 384, 512);
  aggDraw.hud = &agg_targetScreen_cli;
  aggDraw.hud->setFont("verdana18_bold");
  
  osd = new OSDCLASS(-1);
#endif

  ctrls_cfg.boost = 0;
  ctrls_cfg.sdl_quit = 0;
  ctrls_cfg.auto_pause = my_config.auto_pause;
  ctrls_cfg.focused = 1;
  ctrls_cfg.fake_mic = 0;
  ctrls_cfg.keypad = 0;
#ifdef INCLUDE_OPENGL_2D
  ctrls_cfg.screen_texture = screen_texture;
#else
  ctrls_cfg.screen_texture = NULL;
#endif
  ctrls_cfg.resize_cb = &resizeWindow_stub;

  #if !defined(__EMSCRIPTEN__)
  while(!ctrls_cfg.sdl_quit)
  {
    desmume_cycle(&ctrls_cfg);

#ifdef HAVE_LIBAGG
    osd->update();
    DrawHUD();
#endif

#ifdef INCLUDE_OPENGL_2D
    if ( my_config.opengl_2d) {
      opengl_Draw(screen_texture);
      ctrls_cfg.resize_cb = &resizeWindow;
    }
    else
#endif
      Draw();

#ifdef HAVE_LIBAGG
    osd->clear();
#endif

    for ( int i = 0; i < my_config.frameskip; i++ ) {
        NDS_SkipNextFrame();
        desmume_cycle(&ctrls_cfg);
    }

#ifdef DISPLAY_FPS
    now = SDL_GetTicks();
#endif
    if ( !my_config.disable_limiter && !ctrls_cfg.boost) {
#ifndef DISPLAY_FPS
      now = SDL_GetTicks();
#endif
      int delay =  (limiter_tick0 + limiter_frame_counter*1000/FPS_LIMITER_FPS) - now;
      if (delay < -500 || delay > 100) { // reset if we fall too far behind don't want to run super fast until we catch up
        limiter_tick0 = now;
        limiter_frame_counter = 0;
      } else if (delay > 0) {
        SDL_Delay(delay);
      }
    }
    // always count frames, we'll mess up if the limiter gets turned on later otherwise
    limiter_frame_counter += 1 + my_config.frameskip;

#ifdef DISPLAY_FPS
    fps_frame_counter += 1;
    fps_timing += now - fps_previous_time;
    fps_previous_time = now;

    if ( fps_frame_counter == NUM_FRAMES_TO_TIME) {
      char win_title[20];
      float fps = NUM_FRAMES_TO_TIME * 1000.f / fps_timing;

      fps_frame_counter = 0;
      fps_timing = 0;

      snprintf( win_title, sizeof(win_title), "Desmume %f", fps);

      SDL_WM_SetCaption( win_title, NULL);
    }
#endif
  }
  #else // __EMSCRIPTEN__

  emscripten_set_main_loop(update_loop, 0, 1);
  
  #endif

  // TODO: move cleanup to a simple method
  #if !defined(__EMSCRIPTEN__)
  /* Unload joystick */
  uninit_joy();

#ifdef GDB_STUB
  destroyStub_gdb( arm9_gdb_stub);
  arm9_gdb_stub = NULL;
  
  destroyStub_gdb( arm7_gdb_stub);
  arm7_gdb_stub = NULL;

  gdbstub_mutex_destroy();
#endif
  
  SDL_Quit();
  NDS_DeInit();
  #endif // __EMSCRIPTEN__

  return 0;
}
