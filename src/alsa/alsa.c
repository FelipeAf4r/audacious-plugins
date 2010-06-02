/*
 * ALSA Output Plugin for Audacious
 * Copyright 2009-2010 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <stdint.h>

#include "alsa.h"

pthread_mutex_t alsa_mutex = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t * alsa_handle;
pthread_cond_t alsa_cond = PTHREAD_COND_INITIALIZER;
static int initted;

static snd_pcm_format_t alsa_format;
static int alsa_channels, alsa_rate;

static void * alsa_buffer;
static int alsa_buffer_length, alsa_buffer_data_start, alsa_buffer_data_length,
 alsa_buffer_read_length;

static int64_t alsa_time; /* microseconds */
static int alsa_paused, alsa_paused_time;

static int pump_quit;
static pthread_t pump_thread;

static snd_mixer_t * alsa_mixer;
static snd_mixer_elem_t * alsa_mixer_element;

static void * pump (void * unused)
{
    snd_pcm_status_t * status;
    int length;

    pthread_mutex_lock (& alsa_mutex);
    pthread_cond_broadcast (& alsa_cond);

    snd_pcm_status_alloca (& status);

    while (! pump_quit)
    {
        if (alsa_paused)
        {
            pthread_cond_wait (& alsa_cond, & alsa_mutex);
            continue;
        }

        length = snd_pcm_frames_to_bytes (alsa_handle, snd_pcm_bytes_to_frames
         (alsa_handle, alsa_buffer_length / 4));
        length = MIN (length, alsa_buffer_data_length);
        length = MIN (length, alsa_buffer_length - alsa_buffer_data_start);

        /* Currently, snd_pcm_delay doesn't account for data that is being
         * passed by a blocking snd_pcm_writei call.  To minimize the error, we
         * can pass the audio in smaller chunks. */
        if (alsa_config_delay_workaround)
            length = MIN (length, snd_pcm_frames_to_bytes (alsa_handle,
             alsa_rate / 100));

        if (length == 0)
        {
            pthread_cond_wait (& alsa_cond, & alsa_mutex);
            continue;
        }

        alsa_buffer_read_length = length;

#ifdef SND_PCM_HAVE_LOCKED_WRITE
        length = snd_pcm_writei_locked (alsa_handle, (char *) alsa_buffer +
         alsa_buffer_data_start, snd_pcm_bytes_to_frames (alsa_handle,
         length), & alsa_mutex);
#else
        pthread_mutex_unlock (& alsa_mutex);
        length = snd_pcm_writei (alsa_handle, (char *) alsa_buffer +
         alsa_buffer_data_start, snd_pcm_bytes_to_frames (alsa_handle,
         length));
        pthread_mutex_lock (& alsa_mutex);
#endif

        if (length < 0)
        {
            if (! pump_quit && ! alsa_paused) /* ignore errors caused by drop */
                CHECK (snd_pcm_recover, alsa_handle, length, 0);

        FAILED:
            length = 0;
        }

        length = snd_pcm_frames_to_bytes (alsa_handle, length);

        alsa_buffer_data_start = (alsa_buffer_data_start + length) %
         alsa_buffer_length;
        alsa_buffer_data_length -= length;
        alsa_buffer_read_length = 0;

        pthread_cond_broadcast (& alsa_cond);
    }

    pthread_mutex_unlock (& alsa_mutex);
    return NULL;
}

static void start_playback (void)
{
    AUDDBG ("Starting playback.\n");

    if (snd_pcm_state (alsa_handle) == SND_PCM_STATE_PAUSED)
        CHECK (snd_pcm_pause, alsa_handle, 0);
    else
        CHECK (snd_pcm_prepare, alsa_handle);

FAILED:
    alsa_paused = 0;
    pthread_cond_broadcast (& alsa_cond);
}

#define DEBUG_TIMING 0

static int real_output_time (void)
{
    snd_pcm_status_t * status;
    int time = 0;

#if DEBUG_TIMING
    static int offset = 0;
    struct timeval clock;
    int new_offset;

    gettimeofday (& clock, NULL);
#endif

    snd_pcm_status_alloca (& status);
    CHECK (snd_pcm_status, alsa_handle, status);
    time = (alsa_time - (int64_t) (snd_pcm_bytes_to_frames (alsa_handle,
     alsa_buffer_data_length - alsa_buffer_read_length) +
     snd_pcm_status_get_delay (status)) * 1000000 / alsa_rate) / 1000;

#if DEBUG_TIMING
    new_offset = time - clock.tv_usec / 1000;
    printf ("%d. written %d, buffer %d - %d, delay %d, output %d, drift %d\n",
     (int) clock.tv_usec / 1000, (int) alsa_time / 1000, (int)
     snd_pcm_bytes_to_frames (alsa_handle, alsa_buffer_data_length) * 1000 /
     alsa_rate, (int) snd_pcm_bytes_to_frames (alsa_handle,
     alsa_buffer_read_length) * 1000 / alsa_rate, (int) snd_pcm_status_get_delay
     (status) * 1000 / alsa_rate, time, new_offset - offset);
    offset = new_offset;
#endif

FAILED:
    return time;
}

OutputPluginInitStatus alsa_init (void)
{
    alsa_handle = NULL;
    initted = 0;

    return OUTPUT_PLUGIN_INIT_FOUND_DEVICES;
}

void alsa_soft_init (void)
{
    if (! initted)
    {
        AUDDBG ("Initialize.\n");
        alsa_config_load ();
        alsa_open_mixer ();
        initted = 1;
    }
}

void alsa_cleanup (void)
{
    if (initted)
    {
        AUDDBG ("Cleanup.\n");
        alsa_close_mixer ();
        alsa_config_save ();
    }
}

static snd_pcm_format_t convert_aud_format (AFormat aud_format)
{
    const struct
    {
        AFormat aud_format;
        snd_pcm_format_t format;
    }
    table[] =
    {
        {FMT_FLOAT, SND_PCM_FORMAT_FLOAT},
        {FMT_S8, SND_PCM_FORMAT_S8},
        {FMT_U8, SND_PCM_FORMAT_U8},
        {FMT_S16_LE, SND_PCM_FORMAT_S16_LE},
        {FMT_S16_BE, SND_PCM_FORMAT_S16_BE},
        {FMT_U16_LE, SND_PCM_FORMAT_U16_LE},
        {FMT_U16_BE, SND_PCM_FORMAT_U16_BE},
        {FMT_S24_LE, SND_PCM_FORMAT_S24_LE},
        {FMT_S24_BE, SND_PCM_FORMAT_S24_BE},
        {FMT_U24_LE, SND_PCM_FORMAT_U24_LE},
        {FMT_U24_BE, SND_PCM_FORMAT_U24_BE},
        {FMT_S32_LE, SND_PCM_FORMAT_S32_LE},
        {FMT_S32_BE, SND_PCM_FORMAT_S32_BE},
        {FMT_U32_LE, SND_PCM_FORMAT_U32_LE},
        {FMT_U32_BE, SND_PCM_FORMAT_U32_BE},
    };

    int count;

    for (count = 0; count < G_N_ELEMENTS (table); count ++)
    {
         if (table[count].aud_format == aud_format)
             return table[count].format;
    }

    return SND_PCM_FORMAT_UNKNOWN;
}

int alsa_open_audio (AFormat aud_format, int rate, int channels)
{
    snd_pcm_format_t format = convert_aud_format (aud_format);
    snd_pcm_hw_params_t * params;
    guint useconds;
    snd_pcm_uframes_t frames, period;
    int hard_buffer, soft_buffer;

    pthread_mutex_lock (& alsa_mutex);
    alsa_soft_init ();

    AUDDBG ("Opening PCM device %s for %s, %d channels, %d Hz.\n",
     alsa_config_pcm, snd_pcm_format_name (format), channels, rate);
    CHECK_NOISY (snd_pcm_open, & alsa_handle, alsa_config_pcm,
     SND_PCM_STREAM_PLAYBACK, 0);

    snd_pcm_hw_params_alloca (& params);
    CHECK_NOISY (snd_pcm_hw_params_any, alsa_handle, params);
    CHECK_NOISY (snd_pcm_hw_params_set_access, alsa_handle, params,
     SND_PCM_ACCESS_RW_INTERLEAVED);
    CHECK_NOISY (snd_pcm_hw_params_set_format, alsa_handle, params, format);
    CHECK_NOISY (snd_pcm_hw_params_set_channels, alsa_handle, params, channels);
    CHECK_NOISY (snd_pcm_hw_params_set_rate, alsa_handle, params, rate, 0);
    useconds = 1000 * aud_cfg->output_buffer_size / 2;

    /* If we cannot use snd_pcm_drain, we lose any audio that is buffered at the
     * end of each song.  We can minimize the damage by using a smaller buffer. */
    if (alsa_config_drain_workaround)
        useconds = MIN (useconds, 100000);

    CHECK_NOISY (snd_pcm_hw_params_set_buffer_time_max, alsa_handle, params,
     & useconds, 0);
    CHECK_NOISY (snd_pcm_hw_params, alsa_handle, params);

    alsa_format = format;
    alsa_channels = channels;
    alsa_rate = rate;

    CHECK_NOISY (snd_pcm_get_params, alsa_handle, & frames, & period);
    hard_buffer = (int64_t) frames * 1000 / rate;
    soft_buffer = MAX (aud_cfg->output_buffer_size / 2,
     aud_cfg->output_buffer_size - hard_buffer);
    AUDDBG ("Hardware buffer %d ms, software buffer %d ms.\n", hard_buffer,
     soft_buffer);

    alsa_buffer_length = snd_pcm_frames_to_bytes (alsa_handle, (int64_t)
     soft_buffer * rate / 1000);
    alsa_buffer = malloc (alsa_buffer_length);
    alsa_buffer_data_start = 0;
    alsa_buffer_data_length = 0;
    alsa_buffer_read_length = 0;

    alsa_time = 0;
    alsa_paused = 1; /* for buffering */
    alsa_paused_time = 0;

    pump_quit = 0;
    pthread_create (& pump_thread, NULL, pump, NULL);
    pthread_cond_wait (& alsa_cond, & alsa_mutex);

    pthread_mutex_unlock (& alsa_mutex);
    return 1;

FAILED:
    if (alsa_handle != NULL)
    {
        snd_pcm_close (alsa_handle);
        alsa_handle = NULL;
    }

    pthread_mutex_unlock (& alsa_mutex);
    return 0;
}

void alsa_close_audio (void)
{
    AUDDBG ("Closing audio.\n");
    pthread_mutex_lock (& alsa_mutex);
    pump_quit = 1;

    if (! alsa_config_drop_workaround)
        CHECK (snd_pcm_drop, alsa_handle);

FAILED:
    pthread_cond_broadcast (& alsa_cond);
    pthread_mutex_unlock (& alsa_mutex);
    pthread_join (pump_thread, NULL);
    pthread_mutex_lock (& alsa_mutex);

    free (alsa_buffer);
    snd_pcm_close (alsa_handle);
    alsa_handle = NULL;
    pthread_mutex_unlock (& alsa_mutex);
}

void alsa_write_audio (void * data, int length)
{
    pthread_mutex_lock (& alsa_mutex);

    while (1)
    {
        int writable = MIN (alsa_buffer_length - alsa_buffer_data_length,
         length);
        int start = (alsa_buffer_data_start + alsa_buffer_data_length) %
         alsa_buffer_length;

        if (writable > alsa_buffer_length - start)
        {
            int part = alsa_buffer_length - start;

            memcpy ((gint8 *) alsa_buffer + start, data, part);
            memcpy (alsa_buffer, (gint8 *) data + part, writable - part);
        }
        else
            memcpy ((gint8 *) alsa_buffer + start, data, writable);

        data = (gint8 *) data + writable;
        length -= writable;

        alsa_buffer_data_length += writable;
        alsa_time += (int64_t) snd_pcm_bytes_to_frames (alsa_handle, writable) *
         1000000 / alsa_rate;

        if (! length)
            break;

        if (alsa_paused) /* buffering completed */
            start_playback ();

        pthread_cond_broadcast (& alsa_cond);
        pthread_cond_wait (& alsa_cond, & alsa_mutex);
    }

    pthread_mutex_unlock (& alsa_mutex);
}

void alsa_drain (void)
{
    AUDDBG ("Drain.\n");
    pthread_mutex_lock (& alsa_mutex);

    while (alsa_buffer_data_length > 0)
    {
        /* start / wake up pump thread */
        if (alsa_paused)
            start_playback ();
        else
            pthread_cond_broadcast (& alsa_cond);

        pthread_cond_wait (& alsa_cond, & alsa_mutex);
    }

    pthread_mutex_unlock (& alsa_mutex);

    if (! alsa_config_drain_workaround)
        CHECK (snd_pcm_drain, alsa_handle);

FAILED:
    return;
}

void alsa_set_written_time (int time)
{
    AUDDBG ("Setting time counter to %d.\n", time);
    pthread_mutex_lock (& alsa_mutex);
    alsa_time = 1000 * (int64_t) time;
    pthread_mutex_unlock (& alsa_mutex);
}

int alsa_written_time (void)
{
    int time;

    pthread_mutex_lock (& alsa_mutex);
    time = alsa_time / 1000;
    pthread_mutex_unlock (& alsa_mutex);
    return time;
}

int alsa_output_time (void)
{
    int time = 0;

    pthread_mutex_lock (& alsa_mutex);

    if (alsa_paused)
        time = alsa_paused_time;
    else
        time = real_output_time ();

    pthread_mutex_unlock (& alsa_mutex);
    return time;
}

void alsa_flush (int time)
{
    AUDDBG ("Seek requested; discarding buffer.\n");
    pthread_mutex_lock (& alsa_mutex);

    alsa_time = (int64_t) time * 1000;
    alsa_paused = 1; /* for buffering */
    alsa_paused_time = time;

    if (! alsa_config_drop_workaround)
        CHECK (snd_pcm_drop, alsa_handle);

FAILED:
    while (alsa_buffer_read_length)
        pthread_cond_wait (& alsa_cond, & alsa_mutex);

    alsa_buffer_data_start = 0;
    alsa_buffer_data_length = 0;

    pthread_cond_broadcast (& alsa_cond);
    pthread_mutex_unlock (& alsa_mutex);
}

void alsa_pause (gshort pause)
{
    AUDDBG ("%sause.\n", pause ? "P" : "Unp");
    pthread_mutex_lock (& alsa_mutex);

    if (pause)
    {
        alsa_paused = 1;
        alsa_paused_time = real_output_time ();

        CHECK (snd_pcm_pause, alsa_handle, pause);
    }

FAILED:
    pthread_cond_broadcast (& alsa_cond);
    pthread_mutex_unlock (& alsa_mutex);
}

void alsa_open_mixer (void)
{
    snd_mixer_selem_id_t * selem_id;

    alsa_mixer = NULL;

    if (alsa_config_mixer_element == NULL)
        goto FAILED;

    AUDDBG ("Opening mixer card %s.\n", alsa_config_mixer);
    CHECK_NOISY (snd_mixer_open, & alsa_mixer, 0);
    CHECK_NOISY (snd_mixer_attach, alsa_mixer, alsa_config_mixer);
    CHECK_NOISY (snd_mixer_selem_register, alsa_mixer, NULL, NULL);
    CHECK_NOISY (snd_mixer_load, alsa_mixer);

    snd_mixer_selem_id_alloca (& selem_id);
    snd_mixer_selem_id_set_name (selem_id, alsa_config_mixer_element);
    alsa_mixer_element = snd_mixer_find_selem (alsa_mixer, selem_id);

    if (alsa_mixer_element == NULL)
    {
        ERROR_NOISY ("snd_mixer_find_selem failed.\n");
        goto FAILED;
    }

    CHECK (snd_mixer_selem_set_playback_volume_range, alsa_mixer_element, 0, 100);
    return;

FAILED:
    if (alsa_mixer != NULL)
    {
        snd_mixer_close (alsa_mixer);
        alsa_mixer = NULL;
    }
}

void alsa_close_mixer (void)
{
    if (alsa_mixer != NULL)
        snd_mixer_close (alsa_mixer);
}

void alsa_get_volume (int * left, int * right)
{
    glong left_l = 0, right_l = 0;

    pthread_mutex_lock (& alsa_mutex);
    alsa_soft_init ();

    if (alsa_mixer == NULL)
        goto FAILED;

    CHECK (snd_mixer_handle_events, alsa_mixer);

    if (snd_mixer_selem_is_playback_mono (alsa_mixer_element))
    {
        CHECK (snd_mixer_selem_get_playback_volume, alsa_mixer_element,
         SND_MIXER_SCHN_MONO, & left_l);
        right_l = left_l;
    }
    else
    {
        CHECK (snd_mixer_selem_get_playback_volume, alsa_mixer_element,
         SND_MIXER_SCHN_FRONT_LEFT, & left_l);
        CHECK (snd_mixer_selem_get_playback_volume, alsa_mixer_element,
         SND_MIXER_SCHN_FRONT_RIGHT, & right_l);
    }

FAILED:
    pthread_mutex_unlock (& alsa_mutex);

    * left = left_l;
    * right = right_l;
}

void alsa_set_volume (int left, int right)
{
    pthread_mutex_lock (& alsa_mutex);
    alsa_soft_init ();

    if (alsa_mixer == NULL)
        goto FAILED;

    if (snd_mixer_selem_is_playback_mono (alsa_mixer_element))
    {
        CHECK (snd_mixer_selem_set_playback_volume, alsa_mixer_element,
         SND_MIXER_SCHN_MONO, MAX (left, right));

        if (snd_mixer_selem_has_playback_switch (alsa_mixer_element))
            CHECK (snd_mixer_selem_set_playback_switch, alsa_mixer_element,
             SND_MIXER_SCHN_MONO, MAX (left, right) != 0);
    }
    else
    {
        CHECK (snd_mixer_selem_set_playback_volume, alsa_mixer_element,
         SND_MIXER_SCHN_FRONT_LEFT, left);
        CHECK (snd_mixer_selem_set_playback_volume, alsa_mixer_element,
         SND_MIXER_SCHN_FRONT_RIGHT, right);

        if (snd_mixer_selem_has_playback_switch (alsa_mixer_element))
        {
            if (snd_mixer_selem_has_playback_switch_joined (alsa_mixer_element))
                CHECK (snd_mixer_selem_set_playback_switch, alsa_mixer_element,
                 SND_MIXER_SCHN_FRONT_LEFT, MAX (left, right) != 0);
            else
            {
                CHECK (snd_mixer_selem_set_playback_switch, alsa_mixer_element,
                 SND_MIXER_SCHN_FRONT_LEFT, left != 0);
                CHECK (snd_mixer_selem_set_playback_switch, alsa_mixer_element,
                 SND_MIXER_SCHN_FRONT_RIGHT, right != 0);
            }
        }
    }

    CHECK (snd_mixer_handle_events, alsa_mixer);

FAILED:
    pthread_mutex_unlock (& alsa_mutex);
}
