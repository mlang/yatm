/*
 * yatm - Yet Another Time Machine
 * Copyright (C) 2004, 2005, 2006, 2013 Mario Lang
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <slang.h>
#include <mad.h>
#include <ogg/ogg.h>
#include <speex/speex.h>
#include <speex/speex_callbacks.h>
#include <speex/speex_header.h>
#include <speex/speex_stereo.h>
#include <soundtouch/SoundTouch.h>
#include <ao/ao.h>

#include <iostream>

#include "config.h"

static unsigned char verbosity = 1;

static int
initTTY ()
{
  SLtt_get_terminfo();
  if (SLkp_init() != -1) {
    if (SLang_init_tty(-1,0,1) != -1) {
      return 1;
    } else {
      SLang_verror(SL_Open_Error, "Slang_init_tty failed.");
    }
  } else {
    SLang_verror(SL_Open_Error, "SLkp_init failed.");
  }
  return 0;
}

static char quit = 0;
static float tempo = 1.0;
static int pitchCentDelta = 0;

using namespace soundtouch;

static class SoundTouch *st;

typedef void (*SeekFunc)(float delta);

static void
pollKeyboard (SeekFunc seekfunc)
{
  if (SLang_input_pending(0) != 0) {
    switch (SLkp_getkey()) {
    case 'l':
    case SL_KEY_RIGHT:
      if (seekfunc)
	seekfunc(5);
      else
	if (verbosity) {
	  printf("Seeking not implemented for this backend\n");
	  fflush(stdout);
	}
      break;
    case 'h':
    case SL_KEY_LEFT:
      if (seekfunc)
	seekfunc(-5);
      else
	if (verbosity) {
	  printf("Seeking not implemented for this backend\n");
	  fflush(stdout);
	}
      break;
    case '+':
      if (tempo < 5.)
	st->setTempo(tempo += .01);
      break;
    case '-':
      if (tempo > 0.02)
	st->setTempo(tempo -= .01);
      break;
    case 'c':
      st->setPitch(powf(2.,(pitchCentDelta -= 1)/1200.));
      break;
    case 'C':
      if (pitchCentDelta < 4800)
	st->setPitch(powf(2.,(pitchCentDelta += 1)/1200.));
      break;
    case 's':
    case SL_KEY_DOWN:
      st->setPitch(powf(2.,(pitchCentDelta -= 100)/1200.));
      break;
    case 'S':
    case SL_KEY_UP:
      if (pitchCentDelta < 4701)
	st->setPitch(powf(2.,(pitchCentDelta += 100)/1200.));
      else
	st->setPitch(powf(2.,(pitchCentDelta = 4800)/1200.));
      break;
    case 'q':
    case SL_KEY_F(10):
      quit = 1;
      break;
    }
    if (!quit && verbosity > 0) {
      printf("%3.0f%% speed %7d cents\r", tempo*100, pitchCentDelta);
      fflush(stdout);
    }
  }
}

static struct sigaction save_sigint, save_sigtstp;
static void signal_handler (int signal);

static int audio_driver;
static ao_device *audio_device;
static ao_sample_format audio_format;

static void
play_ao (int channels, int bufsize)
{
  SAMPLETYPE samples[bufsize * channels];
  SAMPLETYPE *ptr;
  char buffer[bufsize * channels * sizeof(signed int)];
  char *byte;
  int outSamples;
  do {
    ptr = samples;
    byte = buffer;
    outSamples = st->receiveSamples(samples, bufsize);
    for (int i = 0; i < outSamples; i++) {
      signed int sample;
      for (int c = 0; c < channels; c++) {
	sample = (int)*ptr++;
	*byte++ = (sample >> 0) & 0xff;
	*byte++ = (sample >> 8) & 0xff;
      }
    }
    if (byte-buffer > 0)
      ao_play(audio_device, buffer, byte-buffer);
  } while (outSamples != 0);
}

static void print_version();
static int play_speex(int fd, char *begin);
static int play_sndfile (int fd, char const *begin, char const *end);
static int play_mpeg(int fd, char *begin, char *end);

int
main (int argc, char *argv[])
{
  int c;
  char *input_file = NULL;
  char *begin_time = NULL, *end_time = NULL;
  while ((c = getopt(argc, argv, "b:e:c:s:qt:vVh")) != -1) {
    switch (c) {
    case 'b':
      begin_time = strdup(optarg);
      break;
    case 'e':
      end_time = strdup(optarg);
      break;
    case 'c':
      pitchCentDelta = atoi(optarg);
      break;
    case 's':
      pitchCentDelta = atoi(optarg)*100;
      break;
    case 't':
      tempo = atof(optarg);
      break;
    case 'v':
      verbosity++;
      break;
    case 'q':
      verbosity = 0;
      break;
    case 'V':
      print_version();
      return 0;
    case 'h':
      printf("%s [-b TIME] [-e TIME] [-t RATIO] [-s SEMITONES] [-c CENTS] FILENAME\n", argv[0]);
      exit(EXIT_FAILURE);
    }
  }
  if (optind < argc) {
    input_file = strdup(argv[optind++]);
  }
  if (optind < argc) {
    std::cout << "Excessive command line paramters, aborting..." << std::endl;
    exit(EXIT_FAILURE);
  }
  if (!input_file) {
    std::cout << "No input file specified, aborting..." << std::endl;
    exit(EXIT_FAILURE);
  }

  st = new SoundTouch();

  struct sigaction action;

  int fd = open(input_file, O_RDONLY);
  if (fd == -1) {
    fprintf(stderr, "Can not open %s: %s, aborting...\n", input_file, strerror(errno));
    exit(EXIT_FAILURE);
  }

  ao_initialize();
  audio_driver = ao_default_driver_id();

  if (sigaction(SIGTSTP, 0, &save_sigtstp) == -1) {
    fprintf(stderr, "Error saving sigtstp handler.\n");
    exit(EXIT_FAILURE);
  }
  if (sigaction(SIGINT, 0, &save_sigint) == -1) {
    fprintf(stderr, "Error saving sigint handler.\n");
    return 1;
  }
  action = save_sigtstp;
  action.sa_handler = signal_handler;
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGINT);
  action.sa_flags = 0;
  if (sigaction(SIGTSTP, &action, 0) == -1) {
    fprintf(stderr, "Error setting sigtstp handler.\n");
    return 0;
  }
  action = save_sigint;
  action.sa_handler = signal_handler;
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTSTP);
  action.sa_flags = 0;
  if (sigaction(SIGTSTP, &action, 0) == -1) {
    fprintf(stderr, "Error setting sigtstp handler.\n");
    return 0;
  }

  initTTY();
  st->setSetting(SETTING_USE_QUICKSEEK, 0);
  st->setSetting(SETTING_USE_AA_FILTER, 1);
  st->setPitch(powf(2.,pitchCentDelta/1200.));
  st->setTempo(tempo);
  if (!play_sndfile(fd, begin_time, end_time))
    if (!play_speex(fd, begin_time))
      play_mpeg(fd, begin_time, end_time);

  if (begin_time) free(begin_time);
  if (end_time) free(end_time);
  close(fd);
  delete st;
  SLang_reset_tty();
  return EXIT_SUCCESS;
}

/* MPEG */
/*
 * Private message structure for MAD decoder.
 */
struct player {
#define PLAYER_OPTION_SKIP  0x01
#define PLAYER_OPTION_TIMED 0x02
  int options;
  unsigned char const *start;
  unsigned long length;
  mad_timer_t absolute_time;
  mad_timer_t playback_time;
  mad_timer_t start_time;
  mad_timer_t duration;
};

/*
 * (re)fill the stream buffer which is to be decoded
 */
static enum mad_flow
input (void *data, struct mad_stream *stream)
{
  struct player *player = (struct player *)data;

  if (!player->length)
    return MAD_FLOW_STOP;

  mad_stream_buffer(stream, player->start, player->length);

  player->length = 0;

  return MAD_FLOW_CONTINUE;
}

/*
 * decide whether to continue decoding based on header
 */
static enum mad_flow
decode_header (void *data, struct mad_header const *header)
{
  struct player *player = (struct player *)data;

  if ((player->options & PLAYER_OPTION_TIMED) &&
      mad_timer_compare(player->playback_time, player->duration) > 0)
    return MAD_FLOW_STOP;

  mad_timer_add(&player->absolute_time, header->duration);

  if ((player->options & PLAYER_OPTION_SKIP) &&
      mad_timer_compare(player->absolute_time, player->start_time) < 0)
    return MAD_FLOW_IGNORE;

  mad_timer_add(&player->playback_time, header->duration);
  return MAD_FLOW_CONTINUE;
}

/*
 * perform filtering on decoded frame
 */
static enum mad_flow
decode_filter (
  void *data,
  struct mad_stream const *stream,
  struct mad_frame *frame
) {
  pollKeyboard(NULL);
  if (quit)
    return MAD_FLOW_STOP;
  else
    return MAD_FLOW_CONTINUE;
}

/*
 * The following utility routine performs simple rounding, clipping, and
 * scaling of MAD's high-resolution samples down to 16 bits. It does not
 * perform any dithering or noise shaping, which would be recommended to
 * obtain any exceptional audio quality. It is therefore not recommended to
 * use this routine if high-quality output is desired.
 */

static inline signed int
scale (mad_fixed_t sample)
{
  /* round */
  sample += (1L << (MAD_F_FRACBITS - 16));

  /* clip */
  if (sample >= MAD_F_ONE)
    sample = MAD_F_ONE - 1;
  else if (sample < -MAD_F_ONE)
    sample = -MAD_F_ONE;

  /* quantize */
  return sample >> (MAD_F_FRACBITS + 1 - 16);
}

/*
 * This is the output callback function. It is called after each frame of
 * MPEG audio data has been completely decoded. The purpose of this callback
 * is to output (or play) the decoded PCM audio.
 */

static enum mad_flow
process_output (
  void *data,
  struct mad_header const *header,
  struct mad_pcm *pcm
) {
  unsigned int nchannels = pcm->channels,
               inSamples = pcm->length,
               rate = pcm->samplerate;
  mad_fixed_t const *left_ch = pcm->samples[0], *right_ch = pcm->samples[1];
  SAMPLETYPE samples[nchannels * inSamples];
  SAMPLETYPE *ptr = samples;

  if (!audio_device) {
    audio_format.bits = 16;
    audio_format.channels = nchannels;
    audio_format.rate = rate;
    audio_format.byte_format = AO_FMT_LITTLE;
    audio_device = ao_open_live(audio_driver, &audio_format, NULL);
    if (!audio_device) {
      fprintf(stderr, "Error opening audio device.\n");
      return MAD_FLOW_BREAK;
    }
  }

  for (int i = 0; i < inSamples; i++) {
    SAMPLETYPE sample;
    sample = scale(*left_ch++);
    *ptr++ = sample;
    if (nchannels == 2) {
      sample = scale(*right_ch++);
      *ptr++ = sample;
    }
  }
  st->setSampleRate(rate);
  st->setChannels(nchannels);
  st->putSamples(samples, inSamples);
  play_ao(nchannels, inSamples);
  return MAD_FLOW_CONTINUE;
}

/*
 * This is the error callback function. It is called whenever a decoding
 * error occurs. The error is indicated by stream->error; the list of
 * possible MAD_ERROR_* errors can be found in the mad.h (or stream.h)
 * header file.
 */

static enum mad_flow
decode_error (void *data, struct mad_stream *stream, struct mad_frame *frame)
{
  struct player *player = (struct player *)data;

  fprintf(stderr, "decoding error 0x%04x (%s) at byte offset %ld\n",
	  stream->error, mad_stream_errorstr(stream),
	  stream->this_frame - player->start);

  /* return MAD_FLOW_BREAK here to stop decoding (and propagate an error) */

  return MAD_FLOW_CONTINUE;
}

/* Parse time specification string to mad_timer_t
 */
static int
parse_mad_time (mad_timer_t *timer, char const *str)
{
  mad_timer_t time, accum = mad_timer_zero;
  signed long decimal;
  unsigned long seconds, fraction, fracpart;
  int minus;

  while (isspace((unsigned char) *str)) ++str;

  do {
    seconds = fraction = fracpart = 0;

    switch (*str) {
    case '-':
      ++str;
      minus = 1;
      break;
    case '+':
      ++str;
    default:
      minus = 0;
    }

    do {
      decimal = strtol(str, (char **) &str, 10);
      if (decimal < 0)
        return -1;

      seconds += decimal;

      if (*str == ':') {
        seconds *= 60;
        ++str;
      }
    }
    while (*str >= '0' && *str <= '9');

    if (*str == '.') {
      char const *ptr;

      decimal = strtol(++str, (char **) &ptr, 10);
      if (decimal < 0)
        return -1;

      fraction = decimal;

      for (fracpart = 1; str != ptr; ++str)
        fracpart *= 10;
    } else if (*str == '/') {
      ++str;

      decimal = strtol(str, (char **) &str, 10);
      if (decimal < 0)
        return -1;

      fraction = seconds;
      fracpart = decimal;
      seconds  = 0;
    }

    mad_timer_set(&time, seconds, fraction, fracpart);
    if (minus)
      mad_timer_negate(&time);

    mad_timer_add(&accum, time);
  }
  while (*str == '-' || *str == '+');

  while (isspace((unsigned char) *str)) ++str;

  if (*str != 0)
    return -1;

  *timer = accum;

  return 0;
}

/*
 * This is the function called by main() above to perform all the decoding.
 * It instantiates a decoder object and configures it with the input,
 * output, and error callback functions above. A single call to
 * mad_decoder_run() continues until a callback function returns
 * MAD_FLOW_STOP (to stop decoding) or MAD_FLOW_BREAK (to stop decoding and
 * signal an error).
 */

static int
play_mpeg(int fd, char *begin, char *end)
{
  struct player player;
  struct mad_decoder decoder;
  struct stat stat;
  void *fdm;
  int result;
  if (fstat(fd, &stat) == -1 ||
      stat.st_size == 0)
    return 0;

  fdm = mmap(0, stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (fdm == MAP_FAILED) {
    fprintf(stderr, "mmap failed, aborting...\n");
    return 0;
  }

  player.options = 0;
  player.start = (unsigned char *)fdm;
  player.length = stat.st_size;
  player.absolute_time = player.playback_time = player.start_time =
    player.duration = mad_timer_zero;
  if (begin) {
    if (parse_mad_time(&player.start_time, begin) == -1) {
      fprintf(stderr, "Failed to parse time spec %s\n", begin);
      return 1;
    }
    fprintf(stderr, "Setting skip time\n");
    player.options |= PLAYER_OPTION_SKIP;
  }
  if (end) {
    if (parse_mad_time(&player.duration, end) == -1) {
      fprintf(stderr, "Failed to parse time spec %s\n", begin);
      return 1;
    }
    fprintf(stderr, "Setting end to %s\n", end);
    player.options |= PLAYER_OPTION_TIMED;
  }
  mad_decoder_init(&decoder, &player,
		   input, decode_header, decode_filter, process_output,
		   decode_error, 0 /* message */);

  result = mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
  mad_decoder_finish(&decoder);
  ao_close(audio_device);
  if (munmap((void *)player.start, stat.st_size) == -1)
    return 4;

  return result;
}

static int
parse_double_time (double *timer, char const *str)
{
  double time, accum = 0;
  signed long decimal;
  unsigned long seconds, fraction, fracpart;
  int minus;

  while (isspace(*str)) ++str;
  do {
    seconds = fraction = fracpart = 0;

    switch (*str) {
    case '-':
      ++str;
      minus = 1;
      break;
    case '+':
      ++str;
    default:
      minus = 0;
    }

    do {
      decimal = strtol(str, (char **) &str, 10);
      if (decimal < 0) return -1;
      seconds += decimal;
      if (*str == ':') {
        seconds *= 60;
        ++str;
      }
    }
    while (*str >= '0' && *str <= '9');

    if (*str == '.') {
      char const *ptr;

      decimal = strtol(++str, (char **) &ptr, 10);
      if (decimal < 0) return -1;
      fraction = decimal;

      for (fracpart = 1; str != ptr; ++str)
        fracpart *= 10;
    }

    time = seconds;
    if (fraction) time += fraction / (double)fracpart;
    if (minus) time *= -1;

    accum += time;
  } while (*str == '-' || *str == '+');

  while (isspace(*str)) ++str;

  if (*str != 0)
    return -1;

  *timer = accum;

  return 0;
}

static void
print_version()
{
  fprintf(stderr, "YATM " VERSION "\n");
}

static int
play_speex (int fd, char *begin)
{
  FILE *fin;
  int frame_size = 0, packet_count = 0, stream_init = 0;
  void *stc = NULL;
  SpeexBits bits;
  ogg_sync_state   oy;
  ogg_page         og;
  ogg_packet       op;
  ogg_stream_state os;
  int nframes=2;
  int eos=0;
  int total_samples=0, decoded_samples=0, played_samples=0,
    skip_samples=0;
  float loss_percent=-1;
  SpeexStereoState stereo = SPEEX_STEREO_STATE_INIT;
  int enhance_mode = 1;
  int channels=-1;
  int rate=0;
  int extra_headers;
  float output[2000];
  
  fin = fdopen(dup(fd), "rb");
  if (!fin) {
    perror("fdopen");
    return 0;
  }
  /* Init Ogg data structure */
  ogg_sync_init(&oy);

  speex_bits_init(&bits);

  while (!feof(fin)) { /* Main decoding loop */
    char *data = ogg_sync_buffer(&oy, 200);
    int i, j, nb_read;

    /* Read bitstream from input file */
    nb_read = fread(data, sizeof(char), 200, fin);
    ogg_sync_wrote(&oy, nb_read);

    /* Loop for all complete pages we got (most likely only one) */
    while (ogg_sync_pageout(&oy, &og) == 1) {
      if (!stream_init) {
        ogg_stream_init(&os, ogg_page_serialno(&og));
        stream_init = 1;
      }
      /* Add page to the bitstream */
      ogg_stream_pagein(&os, &og);
      /* Extract all available packets */
      while (!eos && ogg_stream_packetout(&os, &op) == 1) {
        if (packet_count == 0) { /* Speex header */
	  const SpeexMode *mode;
	  SpeexHeader *header;
	  int modeID;
	  SpeexCallback callback;

	  if (!(header = speex_packet_to_header((char*)op.packet, op.bytes))) {
	    fprintf (stderr, "Cannot read Speex header.\n");
	    rewind(fin);
	    fclose(fin);
	    return 0;
	  }
	  if (header->mode >= SPEEX_NB_MODES) {
	    fprintf (stderr, "Speex mode %d does not (yet/any longer) exist in this version\n",
		     header->mode);
	    return 1;
	  }
	  mode = speex_mode_list[header->mode];
	  if (header->speex_version_id > 1) {
	    fprintf (stderr, "This file was encoded with Speex bit-stream version %d, which I don't know how to decode\n",
		     header->speex_version_id);
	    return 1;
	  }
	  if (mode->bitstream_version < header->mode_bitstream_version) {
	    fprintf (stderr, "The file was encoded with a newer version of Speex. You need to upgrade in order to play it.\n");
	    return 1;
	  } else if (mode->bitstream_version > header->mode_bitstream_version) {
	    fprintf (stderr, "The file was encoded with an older version of Speex. You would need to downgrade the version in order to play it.\n");
	    return 1;
	  }
	  if (!(stc = speex_decoder_init(mode))) {
	    fprintf (stderr, "Decoder initialization failed.\n");
	    rewind(fin);
	    fclose(fin);
	    return 0;
	  }
	  speex_decoder_ctl(stc, SPEEX_SET_ENH, &enhance_mode);
	  speex_decoder_ctl(stc, SPEEX_GET_FRAME_SIZE, &frame_size);
	  if (channels != 1) {
	    callback.callback_id = SPEEX_INBAND_STEREO;
	    callback.func = speex_std_stereo_request_handler;
	    callback.data = &stereo;
	    speex_decoder_ctl(stc, SPEEX_SET_HANDLER, &callback);
	  }
	  rate = header->rate;
	  speex_decoder_ctl(stc, SPEEX_SET_SAMPLING_RATE, &rate);
	  nframes = header->frames_per_packet;
	  channels = header->nb_channels;
	  fprintf(stderr, "Decoding %d Hz audio using %s mode",
		  rate, mode->modeName);
	  if (channels == 1) fprintf(stderr, " (mono");
	  else fprintf (stderr, " (stereo");
	  fprintf(stderr, header->vbr ? ", VBR)\n" : ")\n");
	  extra_headers = header->extra_headers;
	  free(header);
	  if (begin) {
	    double time;
	    if (parse_double_time(&time, begin) == -1) {
	      fprintf(stderr, "Unable to parse time spec: %s\n", begin);
	      goto close;
	    }
	    skip_samples = (int)(time * rate);
	  }
	  if (!nframes) nframes = 1;
	  audio_format.bits = 16;
	  audio_format.channels = channels;
	  audio_format.rate = rate;
	  audio_format.byte_format = AO_FMT_LITTLE;
	  if (audio_device) {
	    fprintf(stderr, "Audio device already open.\n");
	    fclose(fin);
	    return 1;
	  }
	  audio_device = ao_open_live(audio_driver, &audio_format, NULL);
	  if (!audio_device) {
	    fprintf(stderr, "Error opening audio device: %d.\n", errno);
	    fclose(fin);
	    return 1;
	  }
	  st->setSampleRate(rate);
	  st->setChannels(channels);
	} else if (packet_count == 1) {
	  fprintf(stderr, "Ignoring comment packet.\n");
	} else if (packet_count <= 1+extra_headers) {
	  fprintf(stderr, "Ignoring extra headers.\n");
	} else {
	  int lost = 0;
	  pollKeyboard(NULL);
	  if (quit) goto close;

	  if (loss_percent > 0 &&
	      100 * ((float)rand())/RAND_MAX < loss_percent) lost = 1;
	  if (op.e_o_s) eos = 1;
	  /* Copy Ogg packet to Speex bitstream */
	  speex_bits_read_from(&bits, (char*)op.packet, op.bytes);
	  for (j=0; j!=nframes; j++) {
	    int ret;
	    /* Decode frame */
	    if (!lost) ret = speex_decode(stc, &bits, output);
	    else ret = speex_decode(stc, NULL, output);
	    if (ret == -1) break;
	    if (ret == -2) {
	      fprintf(stderr, "Decoding error: corrupted stream?\n");
	      break;
	    }
	    if (speex_bits_remaining(&bits) < 0) {
	      fprintf (stderr, "Decoding overflow: corrupted stream?\n");
	      break;
	    }
	    if (channels==2) {
	      /* Due to some yet undetermined reason, the stereo decoding
	       * symbols can not be found by the linker.
	       * speex_decode_stereo(output, frame_size, &stereo); */
	    }
	    if (total_samples >= skip_samples) {
	      SAMPLETYPE samples[frame_size * channels];
	      for (i=0; i<frame_size*channels; i++) {
		SAMPLETYPE sample = output[i];
		if (sample > 32000) sample = 32000;
		else if (sample < -32000) sample = -32000;
		samples[i] = sample;
	      }
	      st->putSamples(samples, frame_size);
	      play_ao(channels, frame_size);
	    }
	    total_samples+=frame_size;
	  }
	}
        packet_count++;
      }
    }
  }
 close:
  if (stc) speex_decoder_destroy(stc);
  else {
    fprintf(stderr, "This doesn't look like a Speex file\n");
    return 0;
  }
  speex_bits_destroy(&bits);
  if (stream_init) ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  fclose(fin);
  return 1;
}

#include <sndfile.h>

SNDFILE *sndfile;
SF_INFO sfinfo;

static void
seek_sndfile (float delta)
{
  sf_seek(sndfile, (sf_count_t)(sfinfo.samplerate*delta), SEEK_CUR);
}

static int
play_sndfile (int fd, char const *begin, char const *end)
{
  sf_count_t maxFrames = 0;
  memset (&sfinfo, 0, sizeof (sfinfo));
  if ((sndfile = sf_open_fd (dup(fd), SFM_READ, &sfinfo, TRUE))) {
    if (begin) {
      double time;
      if (parse_double_time(&time, begin) == -1) {
	fprintf(stderr, "Unable to parse time spec: %s\n", begin);
	goto close;
      }
      sf_seek(sndfile, (sf_count_t)(time * sfinfo.samplerate), SEEK_SET);
    }
    if (end) {
      double time;
      if (parse_double_time(&time, end) == -1) {
        fprintf(stderr, "Unable to parse end time spec: %s\n", end);
        goto close;
      }
      maxFrames = (sf_count_t)(time * sfinfo.samplerate);
    }
    audio_format.bits = 16;
    audio_format.channels = sfinfo.channels;
    audio_format.rate = sfinfo.samplerate;
    audio_format.byte_format = AO_FMT_LITTLE;
    if (audio_device) {
      fprintf(stderr, "Audio device already open.\n");
      goto close;
      return 1;
    }
    audio_device = ao_open_live(audio_driver, &audio_format, NULL);
    if (!audio_device) {
      fprintf(stderr, "Error opening audio device: %d.\n", errno);
      goto close;
      return 1;
    }
    st->setSampleRate(sfinfo.samplerate);
    st->setChannels(sfinfo.channels);
    {
      float buf[512 * sfinfo.channels];
      sf_count_t nFrames;
      sf_count_t readFrames = 0;
      while ((nFrames = sf_readf_float(sndfile, buf, 512)) > 0 && readFrames < maxFrames) {
	if (readFrames + nFrames > maxFrames) nFrames = maxFrames - readFrames;
	SAMPLETYPE samples[nFrames * sfinfo.channels];
	int i;
	for (i=0; i<nFrames*sfinfo.channels; i++) {
	  samples[i] = buf[i]*32700.0;
	}
	st->putSamples(samples, nFrames);
        readFrames += nFrames;
	play_ao(sfinfo.channels, nFrames);
	pollKeyboard(seek_sndfile);
	if (quit) goto close;
      }
    }
  close:
    sf_close(sndfile);
    return 1;
  } else {
    fprintf(stderr, "libsndfile: %s\n", sf_strerror(NULL));
  }
  lseek(fd,0,SEEK_SET);
  return 0;
}

static void
signal_handler (int signal)
{
  SLang_reset_tty();
  kill(getpid(), signal);
  return;
}
