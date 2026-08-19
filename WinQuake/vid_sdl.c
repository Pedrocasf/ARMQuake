/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// vid_sdl.c -- SDL2 video, input and keyboard for the software renderer
//
// Unlike vid_x.c, the 8-bit buffer the renderer draws into is kept separate
// from the 32-bit surface handed to the display.  vid_x.c converted its
// framebuffer in place, which destroyed the paletted image every frame and
// broke SCR_ScreenShot_f on any visual deeper than 8bpp.

#include <SDL.h>

#include "quakedef.h"
#include "d_local.h"

viddef_t	vid;				// global video state

static SDL_Window	*sdl_window;
static SDL_Renderer	*sdl_renderer;
static SDL_Texture	*sdl_texture;
static SDL_Surface	*sdl_winsurf;

// Two ways to get the 8-bit frame onto the screen:
//
//   surface path (default) -- convert straight into the window surface and
//     SDL_UpdateWindowSurface.  One pass over the pixels, no texture, no
//     scaling.  This is the right path on a part with no GPU.
//
//   renderer path (-sdlrenderer) -- upload to a streaming texture and let
//     SDL_Renderer scale it to the window.  Wanted only when a GPU can do
//     the scaling for free; with the software renderer it costs a texture
//     upload plus a full rescale of every frame.
static qboolean	use_renderer;

static Uint32	sdl_palette[256];	// palette index -> target pixel format
static byte		*vid_buffer;		// 8-bit, what the renderer draws into
static Uint32	*vid_argb;			// 32-bit conversion target (renderer path)

static int		vid_highhunkmark;
static int		vid_surfcachesize;
static byte		*vid_surfcache;

unsigned short	d_8to16table[256];

static qboolean	vid_initialized = false;

// input
static qboolean	mouse_avail;
static qboolean	mouse_grabbed;
static float	mouse_x, mouse_y;
static float	old_mouse_x, old_mouse_y;

static cvar_t	m_filter = {"m_filter", "0"};
static cvar_t	_windowed_mouse = {"_windowed_mouse", "1", true};

// vid_stats 1 reports, every 100 frames, how long the 8-bit -> native
// conversion takes versus the call that actually hands the frame to the
// display.  r_dspeeds cannot see either of them: it only measures inside
// R_RenderView.  -nopresent skips the handoff entirely, which isolates a
// blocking display path from CPU cost.
static cvar_t	vid_stats = {"vid_stats", "0"};
static qboolean	no_present;

static void IN_GrabMouse (qboolean grab);

/*
================
VID_SetPalette

Called at startup and whenever the palette is shifted (damage flashes,
item pickups, underwater tint).
================
*/
void	VID_SetPalette (unsigned char *palette)
{
	int		i;

	// On the surface path the entries must be in the window surface's own
	// format, which is whatever the panel wants; on the renderer path the
	// texture is ARGB8888.
	if (!use_renderer && sdl_winsurf)
	{
		for (i = 0 ; i < 256 ; i++)
			sdl_palette[i] = SDL_MapRGB (sdl_winsurf->format,
					palette[i*3+0], palette[i*3+1], palette[i*3+2]);
		return;
	}

	for (i = 0 ; i < 256 ; i++)
	{
		sdl_palette[i] = (0xFFu << 24)
					   | ((Uint32)palette[i*3+0] << 16)
					   | ((Uint32)palette[i*3+1] << 8)
					   | ((Uint32)palette[i*3+2]);
	}
}

void	VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}

/*
================
VID_Init
================
*/
void	VID_Init (unsigned char *palette)
{
	int		pnum;
	int		scale;
	int		pixels;
	Uint32	winflags;

	vid.width = 320;
	vid.height = 200;
	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	if ((pnum = COM_CheckParm("-winsize")))
	{
		if (pnum >= com_argc-2)
			Sys_Error("VID: -winsize <width> <height>");
		vid.width = Q_atoi(com_argv[pnum+1]);
		vid.height = Q_atoi(com_argv[pnum+2]);
		if (!vid.width || !vid.height)
			Sys_Error("VID: Bad window width/height");
	}
	if ((pnum = COM_CheckParm("-width")))
	{
		if (pnum >= com_argc-1)
			Sys_Error("VID: -width <width>");
		vid.width = Q_atoi(com_argv[pnum+1]);
		if (!vid.width)
			Sys_Error("VID: Bad window width");
	}
	if ((pnum = COM_CheckParm("-height")))
	{
		if (pnum >= com_argc-1)
			Sys_Error("VID: -height <height>");
		vid.height = Q_atoi(com_argv[pnum+1]);
		if (!vid.height)
			Sys_Error("VID: Bad window height");
	}

	// integer upscale so 320x200 is not a postage stamp on a modern display
	scale = 1;
	if ((pnum = COM_CheckParm("-scale")) && pnum < com_argc-1)
		scale = Q_atoi(com_argv[pnum+1]);
	if (scale < 1)
		scale = 1;

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		Sys_Error ("VID: SDL_Init failed: %s", SDL_GetError());

	winflags = 0;
	if (COM_CheckParm("-fullscreen"))
		winflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	sdl_window = SDL_CreateWindow ("Quake",
					SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					vid.width * scale, vid.height * scale, winflags);
	if (!sdl_window)
		Sys_Error ("VID: SDL_CreateWindow failed: %s", SDL_GetError());

	use_renderer = (COM_CheckParm("-sdlrenderer") != 0);

	if (!use_renderer)
	{
		sdl_winsurf = SDL_GetWindowSurface (sdl_window);
		if (!sdl_winsurf)
		{
			Con_Printf ("VID: no window surface (%s), using SDL_Renderer\n",
					SDL_GetError());
			use_renderer = true;
		}
		else if (sdl_winsurf->format->BytesPerPixel != 4 &&
				 sdl_winsurf->format->BytesPerPixel != 2)
		{
			Con_Printf ("VID: window surface is %d bytes/pixel, "
					"using SDL_Renderer\n",
					sdl_winsurf->format->BytesPerPixel);
			sdl_winsurf = NULL;
			use_renderer = true;
		}
		else
		{
			const char *drv = SDL_GetCurrentVideoDriver ();

			Con_Printf ("VID: direct surface, %dx%d @ %d bpp, driver %s\n",
					sdl_winsurf->w, sdl_winsurf->h,
					sdl_winsurf->format->BitsPerPixel * 1,
					drv ? drv : "?");
		}
	}

	if (use_renderer)
	{
		SDL_RendererInfo	info;
		int					ow = 0, oh = 0;

		sdl_renderer = SDL_CreateRenderer (sdl_window, -1, 0);
		if (!sdl_renderer)
			Sys_Error ("VID: SDL_CreateRenderer failed: %s", SDL_GetError());

		// letterbox and scale for us; nearest-neighbour keeps the pixels crisp
		SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
		SDL_RenderSetLogicalSize (sdl_renderer, vid.width, vid.height);

		sdl_texture = SDL_CreateTexture (sdl_renderer,
						SDL_PIXELFORMAT_ARGB8888,
						SDL_TEXTUREACCESS_STREAMING, vid.width, vid.height);
		if (!sdl_texture)
			Sys_Error ("VID: SDL_CreateTexture failed: %s", SDL_GetError());

		// On a part with no GPU this path costs a texture upload plus a full
		// software rescale of every frame, and none of it is visible to
		// r_dspeeds, which only measures inside R_RenderView.
		SDL_GetRendererOutputSize (sdl_renderer, &ow, &oh);

		if (SDL_GetRendererInfo (sdl_renderer, &info) == 0)
			Con_Printf ("SDL renderer: %s (%s)\n", info.name,
				(info.flags & SDL_RENDERER_ACCELERATED) ?
					"accelerated" : "SOFTWARE");

		Con_Printf ("VID: rendering %dx%d, output %dx%d%s\n",
				vid.width, vid.height, ow, oh,
				(ow != (int)vid.width || oh != (int)vid.height) ?
					"  <-- RESCALING EVERY FRAME" : "");
	}

	pixels = vid.width * vid.height;

	vid_buffer = (byte *) malloc (pixels);
	// Only the renderer path needs the intermediate 32-bit buffer; the
	// surface path converts straight into the window surface.
	vid_argb = use_renderer ?
			(Uint32 *) malloc (pixels * sizeof(Uint32)) : NULL;
	if (!vid_buffer || (use_renderer && !vid_argb))
		Sys_Error ("VID: not enough memory for the framebuffer");
	memset (vid_buffer, 0, pixels);

	vid.buffer = vid.conbuffer = vid_buffer;
	vid.rowbytes = vid.conrowbytes = vid.width;
	vid.conwidth = vid.width;
	vid.conheight = vid.height;
	vid.direct = 0;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

	// z-buffer and surface cache come out of the high hunk, as in vid_x.c
	vid_highhunkmark = Hunk_HighMark ();
	vid_surfcachesize = D_SurfaceCacheForRes (vid.width, vid.height);

	d_pzbuffer = Hunk_HighAllocName (
			pixels * sizeof(*d_pzbuffer) + vid_surfcachesize, "video");
	if (!d_pzbuffer)
		Sys_Error ("VID: not enough memory for video mode");

	vid_surfcache = (byte *)d_pzbuffer + pixels * sizeof(*d_pzbuffer);
	D_InitCaches (vid_surfcache, vid_surfcachesize);

	VID_SetPalette (palette);

	Cvar_RegisterVariable (&m_filter);
	Cvar_RegisterVariable (&_windowed_mouse);
	Cvar_RegisterVariable (&vid_stats);

	no_present = (COM_CheckParm("-nopresent") != 0);
	if (no_present)
		Con_Printf ("VID: -nopresent, frames will not reach the display\n");

	vid_initialized = true;
}

/*
================
VID_Shutdown

Idempotent: Sys_Error can reach this from a signal handler and then again
from the normal shutdown path.
================
*/
void	VID_Shutdown (void)
{
	if (!vid_initialized)
		return;
	vid_initialized = false;

	IN_GrabMouse (false);

	sdl_winsurf = NULL;		// owned by the window, not freed here

	if (sdl_texture)  { SDL_DestroyTexture (sdl_texture);   sdl_texture = NULL; }
	if (sdl_renderer) { SDL_DestroyRenderer (sdl_renderer); sdl_renderer = NULL; }
	if (sdl_window)   { SDL_DestroyWindow (sdl_window);     sdl_window = NULL; }

	SDL_QuitSubSystem (SDL_INIT_VIDEO);

	free (vid_buffer); vid_buffer = NULL;
	free (vid_argb);   vid_argb = NULL;
	vid.buffer = vid.conbuffer = NULL;
}

/*
================
VID_Update

rects is ignored: we always convert and present the whole frame, which is
what SDL's texture streaming wants anyway.
================
*/
void	VID_Update (vrect_t *rects)
{
	int		i, pixels;
	byte	*src;
	Uint32	*dst;

	if (!vid_initialized)
		return;

	if (!use_renderer)
	{
		int		w, h, x, y, ox, oy, zoom;
		byte	*srow;
		double	tstart = vid_stats.value ? Sys_FloatTime () : 0;

		// The surface is invalidated by a resize, so re-fetch each frame;
		// this is a cheap accessor, not an allocation.
		sdl_winsurf = SDL_GetWindowSurface (sdl_window);
		if (!sdl_winsurf)
			return;

		// Integer nearest-neighbour scaling only, centred, with letterboxing
		// for whatever is left over.  Pixel replication is cheap; SDL's
		// general scaler is not, and on a GPU-less part it can cost more than
		// the rasteriser itself.  A window that cannot fit one whole copy
		// falls back to scale 1 and gets cropped.
		zoom = sdl_winsurf->w / (int)vid.width;
		if (sdl_winsurf->h / (int)vid.height < zoom)
			zoom = sdl_winsurf->h / (int)vid.height;
		if (zoom < 1)
			zoom = 1;

		w = (int)vid.width;
		h = (int)vid.height;
		if (w * zoom > sdl_winsurf->w) w = sdl_winsurf->w / zoom;
		if (h * zoom > sdl_winsurf->h) h = sdl_winsurf->h / zoom;

		ox = (sdl_winsurf->w - w * zoom) / 2;
		oy = (sdl_winsurf->h - h * zoom) / 2;

		if (SDL_MUSTLOCK (sdl_winsurf) && SDL_LockSurface (sdl_winsurf) < 0)
			return;

		if (sdl_winsurf->format->BytesPerPixel == 4)
		{
			for (y = 0 ; y < h ; y++)
			{
				srow = vid_buffer + y * vid.width;
				dst = (Uint32 *)((byte *)sdl_winsurf->pixels
						+ (y * zoom + oy) * sdl_winsurf->pitch) + ox;

				if (zoom == 1)
				{
					for (x = 0 ; x < w ; x++)
						dst[x] = sdl_palette[srow[x]];
				}
				else
				{
					int		k;
					Uint32	*d = dst;

					for (x = 0 ; x < w ; x++)
					{
						Uint32 c = sdl_palette[srow[x]];
						for (k = 0 ; k < zoom ; k++)
							*d++ = c;
					}
					// replicate the row
					for (k = 1 ; k < zoom ; k++)
						memcpy ((byte *)dst + k * sdl_winsurf->pitch, dst,
								w * zoom * sizeof(Uint32));
				}
			}
		}
		else	// 2 bytes per pixel
		{
			Uint16	*d16;

			for (y = 0 ; y < h ; y++)
			{
				srow = vid_buffer + y * vid.width;
				d16 = (Uint16 *)((byte *)sdl_winsurf->pixels
						+ (y * zoom + oy) * sdl_winsurf->pitch) + ox;

				if (zoom == 1)
				{
					for (x = 0 ; x < w ; x++)
						d16[x] = (Uint16)sdl_palette[srow[x]];
				}
				else
				{
					int		k;
					Uint16	*d = d16;

					for (x = 0 ; x < w ; x++)
					{
						Uint16 c = (Uint16)sdl_palette[srow[x]];
						for (k = 0 ; k < zoom ; k++)
							*d++ = c;
					}
					for (k = 1 ; k < zoom ; k++)
						memcpy ((byte *)d16 + k * sdl_winsurf->pitch, d16,
								w * zoom * sizeof(Uint16));
				}
			}
		}

		if (SDL_MUSTLOCK (sdl_winsurf))
			SDL_UnlockSurface (sdl_winsurf);

		if (vid_stats.value)
		{
			static double	acc_convert, acc_present;
			static int		nframes;
			double			tmid, tend;

			tmid = Sys_FloatTime ();
			if (!no_present)
				SDL_UpdateWindowSurface (sdl_window);
			tend = Sys_FloatTime ();

			acc_convert += tmid - tstart;
			acc_present += tend - tmid;

			if (++nframes >= 100)
			{
				Con_Printf ("VID: convert %.2fms  present %.2fms\n",
						acc_convert * 1000 / nframes,
						acc_present * 1000 / nframes);
				acc_convert = acc_present = 0;
				nframes = 0;
			}
			return;
		}

		if (!no_present)
			SDL_UpdateWindowSurface (sdl_window);
		return;
	}

	pixels = vid.width * vid.height;
	src = vid_buffer;
	dst = vid_argb;

	for (i = 0 ; i < pixels ; i++)
		dst[i] = sdl_palette[src[i]];

	SDL_UpdateTexture (sdl_texture, NULL, vid_argb, vid.width * sizeof(Uint32));
	SDL_RenderClear (sdl_renderer);
	SDL_RenderCopy (sdl_renderer, sdl_texture, NULL, NULL);
	SDL_RenderPresent (sdl_renderer);
}

/*
================
D_BeginDirectRect / D_EndDirectRect

The "accessing disk" icon drawn straight to the front buffer.  Not supported
here, as in every other unix back-end.
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}

void D_EndDirectRect (int x, int y, int width, int height)
{
}

/*
=================================================================
INPUT
=================================================================
*/

static void IN_GrabMouse (qboolean grab)
{
	if (grab == mouse_grabbed)
		return;
	mouse_grabbed = grab;
	SDL_SetRelativeMouseMode (grab ? SDL_TRUE : SDL_FALSE);
}

/*
================
IN_MapKey

SDL keysym -> Quake key.  Printable ASCII passes through unchanged, which is
what keys.c expects for bind names and console typing.
================
*/
static int IN_MapKey (SDL_Keysym *ks)
{
	switch (ks->sym)
	{
	case SDLK_TAB:			return K_TAB;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:		return K_ENTER;
	case SDLK_ESCAPE:		return K_ESCAPE;
	case SDLK_SPACE:		return K_SPACE;
	case SDLK_BACKSPACE:	return K_BACKSPACE;

	case SDLK_UP:			return K_UPARROW;
	case SDLK_DOWN:			return K_DOWNARROW;
	case SDLK_LEFT:			return K_LEFTARROW;
	case SDLK_RIGHT:		return K_RIGHTARROW;

	case SDLK_LALT:
	case SDLK_RALT:			return K_ALT;
	case SDLK_LCTRL:
	case SDLK_RCTRL:		return K_CTRL;
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:		return K_SHIFT;

	case SDLK_F1:			return K_F1;
	case SDLK_F2:			return K_F2;
	case SDLK_F3:			return K_F3;
	case SDLK_F4:			return K_F4;
	case SDLK_F5:			return K_F5;
	case SDLK_F6:			return K_F6;
	case SDLK_F7:			return K_F7;
	case SDLK_F8:			return K_F8;
	case SDLK_F9:			return K_F9;
	case SDLK_F10:			return K_F10;
	case SDLK_F11:			return K_F11;
	case SDLK_F12:			return K_F12;

	case SDLK_INSERT:
	case SDLK_KP_0:			return K_INS;
	case SDLK_DELETE:
	case SDLK_KP_PERIOD:	return K_DEL;
	case SDLK_PAGEDOWN:
	case SDLK_KP_3:			return K_PGDN;
	case SDLK_PAGEUP:
	case SDLK_KP_9:			return K_PGUP;
	case SDLK_HOME:
	case SDLK_KP_7:			return K_HOME;
	case SDLK_END:
	case SDLK_KP_1:			return K_END;

	case SDLK_PAUSE:		return K_PAUSE;

	default:
		if (ks->sym >= SDLK_a && ks->sym <= SDLK_z)
			return (int)ks->sym;			// already lowercase ascii
		if (ks->sym > 0 && ks->sym < 128)
			return (int)ks->sym;
		return 0;
	}
}

/*
================
Sys_SendKeyEvents

Pumps the SDL event queue.  Called once per frame from the main loop.
================
*/
void Sys_SendKeyEvents (void)
{
	SDL_Event	ev;
	int			key;

	if (!vid_initialized)
		return;

	while (SDL_PollEvent (&ev))
	{
		switch (ev.type)
		{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			key = IN_MapKey (&ev.key.keysym);
			if (key)
				Key_Event (key, ev.type == SDL_KEYDOWN);
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			switch (ev.button.button)
			{
			case SDL_BUTTON_LEFT:	key = K_MOUSE1; break;
			case SDL_BUTTON_RIGHT:	key = K_MOUSE2; break;
			case SDL_BUTTON_MIDDLE:	key = K_MOUSE3; break;
			default:				key = 0; break;
			}
			if (key)
				Key_Event (key, ev.type == SDL_MOUSEBUTTONDOWN);
			break;

		case SDL_MOUSEWHEEL:
			// no dedicated wheel keys in WinQuake; map to the aux range so
			// they can at least be bound
			if (ev.wheel.y > 0)
			{
				Key_Event (K_AUX1, true);
				Key_Event (K_AUX1, false);
			}
			else if (ev.wheel.y < 0)
			{
				Key_Event (K_AUX2, true);
				Key_Event (K_AUX2, false);
			}
			break;

		case SDL_MOUSEMOTION:
			if (mouse_grabbed)
			{
				mouse_x += ev.motion.xrel;
				mouse_y += ev.motion.yrel;
			}
			break;

		case SDL_QUIT:
			Sys_Quit ();
			break;

		default:
			break;
		}
	}
}

void IN_Init (void)
{
	mouse_avail = !COM_CheckParm ("-nomouse");
	mouse_x = mouse_y = 0.0;
	old_mouse_x = old_mouse_y = 0.0;
}

void IN_Shutdown (void)
{
	IN_GrabMouse (false);
	mouse_avail = false;
}

/*
================
IN_Commands

Grab the pointer while the game has focus and the console/menu is not up,
so relative motion keeps working and the cursor stays put.
================
*/
void IN_Commands (void)
{
	qboolean want;

	if (!mouse_avail)
		return;

	want = _windowed_mouse.value
		&& (key_dest == key_game)
		&& (SDL_GetWindowFlags (sdl_window) & SDL_WINDOW_INPUT_FOCUS);

	IN_GrabMouse (want);
}

/*
================
IN_Move

Same filtering and angle handling as vid_x.c's version.
================
*/
void IN_Move (usercmd_t *cmd)
{
	if (!mouse_avail)
		return;

	if (m_filter.value)
	{
		mouse_x = (mouse_x + old_mouse_x) * 0.5;
		mouse_y = (mouse_y + old_mouse_y) * 0.5;
	}

	old_mouse_x = mouse_x;
	old_mouse_y = mouse_y;

	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value;

	if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
		cmd->sidemove += m_side.value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;

	if (in_mlook.state & 1)
		V_StopPitchDrift ();

	if ( (in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}

	mouse_x = mouse_y = 0.0;
}
