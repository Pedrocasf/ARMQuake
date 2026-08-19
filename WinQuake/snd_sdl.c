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
// snd_sdl.c -- SDL2 sound driver
//
// Replaces snd_linux.c, which required OSS /dev/dsp with DSP_CAP_TRIGGER and
// DSP_CAP_MMAP and cannot work on any current system.
//
// Quake's mixer wants a DMA ring buffer it can write into at will, plus a
// read position telling it how far the hardware has consumed.  SDL gives us a
// pull-style callback instead, so the callback copies out of the ring buffer
// and advances shm->samplepos itself.

#include <SDL.h>

#include "quakedef.h"

static SDL_AudioDeviceID	audio_dev;
static int					dmasize;		// ring buffer size in bytes
static qboolean				snd_inited;

/*
================
SDL_PaintAudio

Copy the next chunk out of Quake's ring buffer, wrapping as needed, and
advance the play position the mixer reads back through SNDDMA_GetDMAPos.
================
*/
static void SDL_PaintAudio (void *unused, Uint8 *stream, int len)
{
	int		pos, tobufend;
	int		len1, len2;
	int		bytesper;

	if (!shm || !shm->buffer)
	{
		memset (stream, 0, len);
		return;
	}

	bytesper = shm->samplebits / 8;

	pos = shm->samplepos * bytesper;
	if (pos >= dmasize)
		pos = shm->samplepos = 0;

	tobufend = dmasize - pos;

	len1 = len;
	len2 = 0;
	if (len1 > tobufend)
	{
		len1 = tobufend;
		len2 = len - len1;
	}

	memcpy (stream, (void *)(shm->buffer + pos), len1);

	if (len2 <= 0)
	{
		shm->samplepos += len1 / bytesper;
	}
	else
	{
		memcpy (stream + len1, (void *)shm->buffer, len2);
		shm->samplepos = len2 / bytesper;
	}

	if (shm->samplepos * bytesper >= dmasize)
		shm->samplepos = 0;
}

/*
================
SNDDMA_Init
================
*/
qboolean SNDDMA_Init (void)
{
	SDL_AudioSpec	desired, obtained;
	int				i;

	if (SDL_InitSubSystem (SDL_INIT_AUDIO) < 0)
	{
		Con_Printf ("Could not initialise SDL audio: %s\n", SDL_GetError());
		return false;
	}

	memset (&desired, 0, sizeof(desired));

	desired.freq = 22050;
	if ((i = COM_CheckParm("-sndspeed")) && i < com_argc-1)
		desired.freq = Q_atoi (com_argv[i+1]);

	desired.format   = loadas8bit.value ? AUDIO_U8 : AUDIO_S16SYS;
	desired.channels = COM_CheckParm("-nostereo") ? 1 : 2;
	desired.samples  = 1024;
	desired.callback = SDL_PaintAudio;

	audio_dev = SDL_OpenAudioDevice (NULL, 0, &desired, &obtained,
					SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (audio_dev == 0)
	{
		Con_Printf ("Could not open SDL audio device: %s\n", SDL_GetError());
		SDL_QuitSubSystem (SDL_INIT_AUDIO);
		return false;
	}

	// The format we asked for is the one we can mix; anything else means the
	// mixer would write samples the device cannot play.
	if (obtained.format != desired.format || obtained.channels != desired.channels)
	{
		Con_Printf ("SDL audio returned an unusable format\n");
		SDL_CloseAudioDevice (audio_dev);
		audio_dev = 0;
		SDL_QuitSubSystem (SDL_INIT_AUDIO);
		return false;
	}

	shm = &sn;
	memset ((void *)shm, 0, sizeof(*shm));

	shm->splitbuffer	= 0;
	shm->channels		= obtained.channels;
	shm->samplebits		= (obtained.format & 0xFF);
	shm->speed			= obtained.freq;
	shm->submission_chunk = 1;
	shm->samplepos		= 0;

	// Ring buffer several callbacks deep so the mixer always has room ahead
	// of the play position.  shm->samples counts mono samples.
	shm->samples = obtained.samples * obtained.channels * 8;

	dmasize = shm->samples * (shm->samplebits / 8);
	shm->buffer = (unsigned char *) malloc (dmasize);
	if (!shm->buffer)
	{
		Con_Printf ("Could not allocate the sound buffer\n");
		SDL_CloseAudioDevice (audio_dev);
		audio_dev = 0;
		SDL_QuitSubSystem (SDL_INIT_AUDIO);
		shm = NULL;
		return false;
	}
	memset ((void *)shm->buffer, shm->samplebits == 8 ? 0x80 : 0, dmasize);

	Con_Printf ("SDL audio: %d Hz, %d bit, %d channels\n",
			shm->speed, shm->samplebits, shm->channels);

	SDL_PauseAudioDevice (audio_dev, 0);

	snd_inited = true;
	return true;
}

/*
================
SNDDMA_GetDMAPos
================
*/
int SNDDMA_GetDMAPos (void)
{
	if (!snd_inited)
		return 0;
	return shm->samplepos;
}

/*
================
SNDDMA_Shutdown
================
*/
void SNDDMA_Shutdown (void)
{
	if (!snd_inited)
		return;
	snd_inited = false;

	SDL_PauseAudioDevice (audio_dev, 1);
	SDL_CloseAudioDevice (audio_dev);
	audio_dev = 0;
	SDL_QuitSubSystem (SDL_INIT_AUDIO);

	if (shm && shm->buffer)
	{
		free ((void *)shm->buffer);
		shm->buffer = NULL;
	}
	shm = NULL;
}

/*
================
SNDDMA_Submit

Nothing to do: the callback pulls straight out of the ring buffer.
================
*/
void SNDDMA_Submit (void)
{
}
