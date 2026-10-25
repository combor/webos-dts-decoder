/* dtsswdec: a GStreamer DTS audio decoder built on FFmpeg's DTS decoder
 *
 * Copyright (C) 2026 webos-dts-decoder contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Written for GStreamer 1.14 on LG webOS TVs, whose media pipeline has no DTS
 * decoder and no DTS parser. The element therefore splits the input into
 * frames itself (it accepts both framed and unframed audio/x-dts) and outputs
 * interleaved integer PCM, which is all LG's audio sink accepts.
 */

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiodecoder.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#define PACKAGE "webos-dts-decoder"
#define VERSION "0.1.0"
#define ORIGIN "https://github.com/combor/webos-dts-decoder"

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
#error "output is written in native byte order and declared as little-endian"
#endif

GST_DEBUG_CATEGORY_STATIC (dtsswdec_debug);
#define GST_CAT_DEFAULT dtsswdec_debug

/* Sync words, as FFmpeg's dca_syncwords.h */
#define DTS_SYNC_CORE_BE      0x7FFE8001U
#define DTS_SYNC_CORE_LE      0xFE7F0180U
#define DTS_SYNC_CORE_14B_BE  0x1FFFE800U
#define DTS_SYNC_CORE_14B_LE  0xFF1F00E8U
#define DTS_SYNC_EXSS         0x64582025U

/* Bytes needed to recognise a sync word */
#define SYNC_BYTES 6
/* Larger than any real frame (DTS-HD MA peaks around 64 KiB); a claimed
 * length above this is treated as a false sync. */
#define MAX_FRAME_SIZE (256 * 1024)
/* Consecutive undecodable frames tolerated before giving up on the stream */
#define MAX_BAD_FRAMES 64
#define MAX_CHANNELS 8

typedef enum
{
  SYNC_NONE,
  SYNC_CORE_BE,
  SYNC_CORE_LE,
  SYNC_CORE_14B_BE,
  SYNC_CORE_14B_LE,
  SYNC_EXSS,
} DtsSync;

typedef struct _GstDtsSwDec
{
  GstAudioDecoder parent;

  /* properties */
  gboolean core_only;
  gint downmix;

  AVCodecContext *ctx;
  AVPacket *pkt;
  AVFrame *frame;

  /* the previous frame ended where this one starts */
  gboolean synced;
  guint bad_frames;
  /* downmix setting in effect since start() */
  gint downmix_mode;

  /* current output format */
  gboolean configured;
  GstAudioFormat out_format;
  gint out_rate;
  AVChannelLayout out_layout;
  gint out_channels;
  /* output channel i comes from decoder channel reorder[i] */
  gint reorder[MAX_CHANNELS];
  /* stereo downmix weights per decoder channel, when downmixing here */
  gboolean downmixing;
  float mix[2][MAX_CHANNELS];
  /* output configured for IEC 61937 at this sample rate, 0 if not */
  gint passthrough_rate;
  /* arc-passthrough: last look at SOUND_OUTPUT_FILE */
  gint64 output_checked;
  gboolean output_is_arc;
  /* arc-passthrough: LG's PCM renderer (see renderer_metadata_type()) */
  gchar *renderer_control;
  gint64 renderer_checked;
  gboolean renderer_failed;
  /* this stream's marker while it relies on the renderer, else NULL */
  gchar *renderer_user;
} GstDtsSwDec;

typedef struct _GstDtsSwDecClass
{
  GstAudioDecoderClass parent_class;
} GstDtsSwDecClass;

#define GST_TYPE_DTSSWDEC (gst_dtsswdec_get_type ())
#define GST_DTSSWDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_DTSSWDEC, GstDtsSwDec))

GType gst_dtsswdec_get_type (void);
G_DEFINE_TYPE (GstDtsSwDec, gst_dtsswdec, GST_TYPE_AUDIO_DECODER);

enum
{
  PROP_0,
  PROP_CORE_ONLY,
  PROP_DOWNMIX,
};

typedef enum
{
  DOWNMIX_NONE,
  DOWNMIX_5_1,
  DOWNMIX_STEREO,
  DOWNMIX_AUTO,
  DOWNMIX_PASSTHROUGH,
  DOWNMIX_ARC_PASSTHROUGH,
} GstDtsSwDecDownmix;

#define DEFAULT_CORE_ONLY FALSE
#define DEFAULT_DOWNMIX DOWNMIX_AUTO

/* The TV's media pipeline cannot set element properties, so "auto" reads the
 * setting from this file (installed by deploy/install.sh) */
#define DOWNMIX_SETTING_FILE "/var/lib/dtsdec/downmix"
/* Runtime directory of deploy/dtsdec, created at boot */
#define RUNTIME_DIR "/tmp/dtsdec"
/* The TV's sound output, kept up to date by deploy/dtsdec's watcher as
 * "<watcher pid> <soundOutput> <soundOutputDigital>" */
#define SOUND_OUTPUT_FILE RUNTIME_DIR "/sound-output"

#define GST_TYPE_DTSSWDEC_DOWNMIX (gst_dtsswdec_downmix_get_type ())
static GType
gst_dtsswdec_downmix_get_type (void)
{
  static gsize type = 0;
  static const GEnumValue values[] = {
    {DOWNMIX_NONE, "Keep every channel", "none"},
    {DOWNMIX_5_1, "At most 5.1 (DTS-HD 7.1 plays its own 5.1 base)", "5.1"},
    {DOWNMIX_STEREO, "Stereo", "stereo"},
    {DOWNMIX_AUTO, "As set in " DOWNMIX_SETTING_FILE ", else stereo", "auto"},
    {DOWNMIX_PASSTHROUGH, "Do not decode: send the DTS core as IEC 61937 "
          "in stereo PCM, for a receiver that decodes DTS (never for "
          "speakers: they would play it as loud noise; leaves LG's PCM "
          "renderer alone, so for testing only)", "passthrough"},
    {DOWNMIX_ARC_PASSTHROUGH, "Pass through while the TV sends sound to an "
          "HDMI ARC/eARC device that takes the bitstream, with LG's PCM "
          "renderer set to leave it unchanged; stereo otherwise",
        "arc-passthrough"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type))
    g_once_init_leave (&type,
        g_enum_register_static ("GstDtsSwDecDownmix", values));
  return type;
}

/* LG's matroskademux does not recognise DTS and exposes it as audio/x-unknown
 * with the Matroska codec ID, so accept that too. */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-dts; "
        "audio/x-unknown, "
        "codec-id = (string) { A_DTS, A_DTS/EXPRESS, A_DTS/LOSSLESS }"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) { S32LE, S16LE }, "
        "layout = (string) interleaved, "
        "rate = (int) [ 8000, 192000 ], "
        "channels = (int) [ 1, 8 ]"));

/* FFmpeg channel -> GStreamer position; anything unlisted is unmappable */
static const struct
{
  enum AVChannel av;
  GstAudioChannelPosition gst;
} channel_positions[] = {
  {AV_CHAN_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT},
  {AV_CHAN_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT},
  {AV_CHAN_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER},
  {AV_CHAN_LOW_FREQUENCY, GST_AUDIO_CHANNEL_POSITION_LFE1},
  {AV_CHAN_BACK_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_LEFT},
  {AV_CHAN_BACK_RIGHT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT},
  {AV_CHAN_FRONT_LEFT_OF_CENTER,
      GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER},
  {AV_CHAN_FRONT_RIGHT_OF_CENTER,
      GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER},
  {AV_CHAN_BACK_CENTER, GST_AUDIO_CHANNEL_POSITION_REAR_CENTER},
  {AV_CHAN_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT},
  {AV_CHAN_SIDE_RIGHT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT},
  {AV_CHAN_TOP_CENTER, GST_AUDIO_CHANNEL_POSITION_TOP_CENTER},
  {AV_CHAN_TOP_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT},
  {AV_CHAN_TOP_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER},
  {AV_CHAN_TOP_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT},
  {AV_CHAN_TOP_BACK_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT},
  {AV_CHAN_TOP_BACK_CENTER, GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER},
  {AV_CHAN_TOP_BACK_RIGHT, GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT},
  {AV_CHAN_WIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT},
  {AV_CHAN_WIDE_RIGHT, GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT},
  {AV_CHAN_SURROUND_DIRECT_LEFT, GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT},
  {AV_CHAN_SURROUND_DIRECT_RIGHT, GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT},
  {AV_CHAN_LOW_FREQUENCY_2, GST_AUDIO_CHANNEL_POSITION_LFE2},
  {AV_CHAN_TOP_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_LEFT},
  {AV_CHAN_TOP_SIDE_RIGHT, GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_RIGHT},
  {AV_CHAN_BOTTOM_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_CENTER},
  {AV_CHAN_BOTTOM_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_LEFT},
  {AV_CHAN_BOTTOM_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_RIGHT},
};

static gboolean
av_channel_to_position (enum AVChannel ch, GstAudioChannelPosition * pos)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (channel_positions); i++) {
    if (channel_positions[i].av == ch) {
      *pos = channel_positions[i].gst;
      return TRUE;
    }
  }
  return FALSE;
}

/* Lo/Ro stereo downmix weights for one decoder channel: centre and surrounds
 * at -3 dB (ITU-R BS.775), LFE left out */
static void
downmix_weights (enum AVChannel ch, float *l, float *r)
{
  const float m3db = 0.70710678f;

  *l = *r = 0.0f;
  switch (ch) {
    case AV_CHAN_FRONT_LEFT:
    case AV_CHAN_FRONT_LEFT_OF_CENTER:
    case AV_CHAN_WIDE_LEFT:
    case AV_CHAN_STEREO_LEFT:
      *l = 1.0f;
      break;
    case AV_CHAN_FRONT_RIGHT:
    case AV_CHAN_FRONT_RIGHT_OF_CENTER:
    case AV_CHAN_WIDE_RIGHT:
    case AV_CHAN_STEREO_RIGHT:
      *r = 1.0f;
      break;
    case AV_CHAN_FRONT_CENTER:
    case AV_CHAN_TOP_CENTER:
    case AV_CHAN_TOP_FRONT_CENTER:
    case AV_CHAN_BOTTOM_FRONT_CENTER:
      *l = *r = m3db;
      break;
    case AV_CHAN_BACK_CENTER:
    case AV_CHAN_TOP_BACK_CENTER:
      *l = *r = 0.5f;
      break;
    case AV_CHAN_SIDE_LEFT:
    case AV_CHAN_BACK_LEFT:
    case AV_CHAN_SURROUND_DIRECT_LEFT:
    case AV_CHAN_TOP_FRONT_LEFT:
    case AV_CHAN_TOP_BACK_LEFT:
    case AV_CHAN_TOP_SIDE_LEFT:
    case AV_CHAN_BOTTOM_FRONT_LEFT:
      *l = m3db;
      break;
    case AV_CHAN_SIDE_RIGHT:
    case AV_CHAN_BACK_RIGHT:
    case AV_CHAN_SURROUND_DIRECT_RIGHT:
    case AV_CHAN_TOP_FRONT_RIGHT:
    case AV_CHAN_TOP_BACK_RIGHT:
    case AV_CHAN_TOP_SIDE_RIGHT:
    case AV_CHAN_BOTTOM_FRONT_RIGHT:
      *r = m3db;
      break;
    case AV_CHAN_LOW_FREQUENCY:
    case AV_CHAN_LOW_FREQUENCY_2:
      break;
    default:
      /* position unknown: share it between both sides */
      *l = *r = 0.5f;
      break;
  }
}

/* --- Framing ------------------------------------------------------------ */

/* Identifies a sync word at p (SYNC_BYTES available), with the same extra
 * header bit checks as FFmpeg's DTS parser. */
static DtsSync
dts_sync_type (const guint8 * p)
{
  guint64 s;

  if (GST_READ_UINT32_BE (p) == DTS_SYNC_EXSS)
    return SYNC_EXSS;

  s = ((guint64) GST_READ_UINT32_BE (p) << 16) | GST_READ_UINT16_BE (p + 4);
  if ((s & 0xFFFFFFFFFC00ULL) == (((guint64) DTS_SYNC_CORE_BE << 16) | 0xFC00))
    return SYNC_CORE_BE;
  if ((s & 0xFFFFFFFF00FCULL) == (((guint64) DTS_SYNC_CORE_LE << 16) | 0x00FC))
    return SYNC_CORE_LE;
  if ((s & 0xFFFFFFFFFFF0ULL) ==
      (((guint64) DTS_SYNC_CORE_14B_BE << 16) | 0x07F0))
    return SYNC_CORE_14B_BE;
  if ((s & 0xFFFFFFFFF0FFULL) ==
      (((guint64) DTS_SYNC_CORE_14B_LE << 16) | 0xF007))
    return SYNC_CORE_14B_LE;
  return SYNC_NONE;
}

/* Length of a 16-bit big-endian core frame (8 bytes available), 0 if bad */
static gsize
dts_core_size_be (const guint8 * p)
{
  gsize size = (((p[5] & 0x03) << 12) | (p[6] << 4) | (p[7] >> 4)) + 1;

  return size >= 96 ? size : 0;
}

/* Same for 16-bit little-endian, where each pair of bytes is swapped */
static gsize
dts_core_size_le (const guint8 * p)
{
  gsize size = (((p[4] & 0x03) << 12) | (p[7] << 4) | (p[6] >> 4)) + 1;

  return size >= 96 ? size : 0;
}

/* Lower bound for the length of a 14-bit core frame (10 bytes available).
 * Same arithmetic as FFmpeg's parser; the true length is larger because
 * every 16-bit word carries only 14 bits. */
static gsize
dts_core_min_size_14b (const guint8 * p, DtsSync type)
{
  guint64 s = GST_READ_UINT32_BE (p + 6);
  gsize size;

  if (type == SYNC_CORE_14B_LE)
    s = ((s & 0xFF00FF00) >> 8) | ((s & 0x00FF00FF) << 8);
  s = ((s & 0x3FFF0000) >> 8) | ((s & 0x00003FFF) >> 6);
  size = ((s >> 4) & 0x3FFF) + 1;

  return size >= 96 ? size : 0;
}

/* Length of an extension substream (DTS-HD) header at p (10 bytes
 * available), 0 if bad */
static gsize
dts_exss_size (const guint8 * p)
{
  gsize header, size;

  if (p[5] & 0x20) {
    header = (((p[5] & 0x1F) << 7) | (p[6] >> 1)) + 1;
    size = (((p[6] & 0x01) << 19) | (p[7] << 11) | (p[8] << 3) | (p[9] >> 5))
        + 1;
  } else {
    header = (((p[5] & 0x1F) << 3) | (p[6] >> 5)) + 1;
    size = (((p[6] & 0x1F) << 11) | (p[7] << 3) | (p[8] >> 5)) + 1;
  }

  return size >= header ? size : 0;
}

/* Number of zero bytes at the start of p (some streams pad between frames) */
static gsize
zero_bytes (const guint8 * p, gsize len)
{
  gsize n = 0;

  while (n < len && p[n] == 0)
    n++;
  return n;
}

static gboolean
dts_sync_follows (DtsSync type, DtsSync next)
{
  if (type == SYNC_EXSS)
    return next == SYNC_EXSS;
  return next == type;
}

/* Works out the length of the frame whose sync word is at data[pos].
 * Returns the length, 0 if more data is needed, or -1 if data[pos] does not
 * start a frame. */
static gssize
dts_frame_length (const guint8 * data, gsize avail, gsize pos, DtsSync type,
    gboolean eos)
{
  const guint8 *p = data + pos;
  gsize left = avail - pos;
  gsize size, min, next, ext;

  switch (type) {
    case SYNC_EXSS:
      if (left < 10)
        return eos ? -1 : 0;
      size = dts_exss_size (p);
      break;

    case SYNC_CORE_BE:
      if (left < 8)
        return eos ? -1 : 0;
      size = dts_core_size_be (p);
      if (!size)
        return -1;
      /* A DTS-HD extension substream may follow the core in the same frame,
       * starting at the next 4-byte boundary (where FFmpeg looks for it) */
      ext = (size + 3) & ~(gsize) 3;
      if (left < ext + 4) {
        if (!eos)
          return 0;
      } else if (GST_READ_UINT32_BE (p + ext) == DTS_SYNC_EXSS) {
        if (left < ext + 10) {
          if (!eos)
            return 0;
        } else {
          gsize exss = dts_exss_size (p + ext);

          /* one cut off at the end of the stream still leaves the core */
          if (exss && (!eos || ext + exss <= left))
            size = ext + exss;
        }
        /* an invalid one is skipped as junk before the next frame */
      }
      break;

    case SYNC_CORE_LE:
      if (left < 8)
        return eos ? -1 : 0;
      size = dts_core_size_le (p);
      break;

    case SYNC_CORE_14B_BE:
    case SYNC_CORE_14B_LE:
      /* No exact length in the header: the frame runs to the next sync */
      if (left < 10)
        return eos ? -1 : 0;
      min = dts_core_min_size_14b (p, type);
      if (!min)
        return -1;
      for (next = min; next + SYNC_BYTES <= left; next++) {
        if (dts_sync_type (p + next) == type)
          return next;
      }
      if (eos)
        return left;
      return left > MAX_FRAME_SIZE ? -1 : 0;

    default:
      return -1;
  }

  if (size == 0 || size > MAX_FRAME_SIZE)
    return -1;
  if (size > left)
    return eos ? -1 : 0;
  return size;
}

static GstFlowReturn
gst_dtsswdec_parse (GstAudioDecoder * dec, GstAdapter * adapter,
    gint * offset, gint * length)
{
  GstDtsSwDec *self = GST_DTSSWDEC (dec);
  gsize avail = gst_adapter_available (adapter);
  const guint8 *data;
  gboolean eos;
  gsize pos;
  GstFlowReturn ret = GST_FLOW_EOS;     /* i.e. need more data */

  gst_audio_decoder_get_parse_state (dec, NULL, &eos);
  *offset = 0;

  if (avail < SYNC_BYTES)
    return GST_FLOW_EOS;

  data = gst_adapter_map (adapter, avail);

  for (pos = 0; pos + SYNC_BYTES <= avail; pos++) {
    DtsSync type = dts_sync_type (data + pos);
    gssize size;

    if (type == SYNC_NONE)
      continue;

    size = dts_frame_length (data, avail, pos, type, eos);
    if (size < 0)
      continue;
    if (size == 0)
      break;

    /* Not locked on (zero padding after the last frame does not count as
     * losing sync): only trust the sync word if another frame follows,
     * possibly after zero padding */
    if ((!self->synced || zero_bytes (data, pos) < pos) && !eos) {
      gsize next = pos + size;
      gsize padding = zero_bytes (data + next, avail - next);

      if (padding > MAX_FRAME_SIZE) {
        GST_LOG_OBJECT (self, "false sync at %" G_GSIZE_FORMAT, pos);
        continue;
      }
      next += padding;
      if (next + SYNC_BYTES > avail)
        break;
      if (!dts_sync_follows (type, dts_sync_type (data + next))) {
        GST_LOG_OBJECT (self, "false sync at %" G_GSIZE_FORMAT, pos);
        continue;
      }
    }

    if (pos > 0)
      GST_DEBUG_OBJECT (self, "skipped %" G_GSIZE_FORMAT " bytes", pos);
    self->synced = TRUE;
    *length = size;
    ret = GST_FLOW_OK;
    break;
  }

  /* Drop what cannot start a frame. If nothing was found, pos stops short of
   * the last SYNC_BYTES - 1 bytes, keeping a possibly incomplete sync word. */
  *offset = pos;
  if (ret != GST_FLOW_OK && zero_bytes (data, pos) < pos)
    self->synced = FALSE;

  gst_adapter_unmap (adapter);
  return ret;
}

/* --- Output ------------------------------------------------------------- */

static GstAudioFormat
gst_dtsswdec_pick_format (GstDtsSwDec * self)
{
  GstAudioFormat format = GST_AUDIO_FORMAT_S32LE;
  GstCaps *allowed, *s32;

  allowed = gst_pad_get_allowed_caps (GST_AUDIO_DECODER_SRC_PAD (self));
  if (allowed) {
    s32 = gst_caps_new_simple ("audio/x-raw",
        "format", G_TYPE_STRING, "S32LE", NULL);
    if (!gst_caps_can_intersect (allowed, s32))
      format = GST_AUDIO_FORMAT_S16LE;
    gst_caps_unref (s32);
    gst_caps_unref (allowed);
  }
  return format;
}

static gboolean
gst_dtsswdec_configure (GstDtsSwDec * self, const AVFrame * frame)
{
  GstAudioChannelPosition from[MAX_CHANNELS], to[MAX_CHANNELS];
  gint channels = frame->ch_layout.nb_channels;
  gint map[MAX_CHANNELS];
  gboolean positioned = TRUE;
  GstAudioInfo info;
  gint i;

  if (channels < 1 || channels > MAX_CHANNELS) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
        ("unsupported channel count %d", channels));
    return FALSE;
  }

  /* FFmpeg downmixes to stereo itself only when the stream carries downmix
   * coefficients; otherwise mix the channels down here */
  self->downmixing = self->downmix_mode != DOWNMIX_NONE &&
      self->downmix_mode != DOWNMIX_5_1 && channels > 2;
  if (self->downmixing) {
    float l, r, sum_l = 0.0f, sum_r = 0.0f, scale;

    for (i = 0; i < channels; i++) {
      downmix_weights (av_channel_layout_channel_from_index
          (&frame->ch_layout, i), &l, &r);
      self->mix[0][i] = l;
      self->mix[1][i] = r;
      sum_l += l;
      sum_r += r;
    }
    /* never clip, whatever the channels carry */
    scale = 1.0f / MAX (1.0f, MAX (sum_l, sum_r));
    for (i = 0; i < channels; i++) {
      self->mix[0][i] *= scale;
      self->mix[1][i] *= scale;
    }
    GST_INFO_OBJECT (self, "downmixing %d channels to stereo", channels);
    channels = 2;
    to[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
    to[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
    goto set_format;
  }

  if (channels == 1) {
    from[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
  } else {
    for (i = 0; i < channels && positioned; i++)
      positioned = av_channel_to_position
          (av_channel_layout_channel_from_index (&frame->ch_layout, i),
          &from[i]);
  }

  if (positioned) {
    memcpy (to, from, sizeof (from[0]) * channels);
    positioned = gst_audio_channel_positions_to_valid_order (to, channels)
        && gst_audio_get_channel_reorder_map (channels, from, to, map);
  }
  if (!positioned) {
    GST_WARNING_OBJECT (self, "cannot map channel layout, output unpositioned");
    for (i = 0; i < channels; i++) {
      to[i] = GST_AUDIO_CHANNEL_POSITION_NONE;
      map[i] = i;
    }
  }
  for (i = 0; i < channels; i++)
    self->reorder[map[i]] = i;

set_format:
  self->passthrough_rate = 0;
  self->out_channels = channels;
  self->out_format = gst_dtsswdec_pick_format (self);
  self->out_rate = frame->sample_rate;
  av_channel_layout_uninit (&self->out_layout);
  if (av_channel_layout_copy (&self->out_layout, &frame->ch_layout) < 0)
    return FALSE;

  gst_audio_info_init (&info);
  gst_audio_info_set_format (&info, self->out_format, frame->sample_rate,
      channels, to);
  GST_INFO_OBJECT (self, "output %s, %d Hz, %d channels",
      gst_audio_format_to_string (self->out_format), frame->sample_rate,
      channels);
  self->configured = gst_audio_decoder_set_output_format (GST_AUDIO_DECODER
      (self), &info);
  return self->configured;
}

static inline gint32
flt_to_s32 (float v)
{
  v *= 2147483648.0f;
  if (v >= 2147483647.0f)
    return G_MAXINT32;
  if (v <= -2147483648.0f)
    return G_MININT32;
  return lrintf (v);
}

static inline gint16
flt_to_s16 (float v)
{
  v *= 32768.0f;
  if (v >= 32767.0f)
    return G_MAXINT16;
  if (v <= -32768.0f)
    return G_MININT16;
  return lrintf (v);
}

/* Planar decoder output -> interleaved, reordered, little-endian integers.
 * The conversions match libswresample's, so the result is identical to
 * `ffmpeg -c:a dca ... -f s32le` / `-f s16le`. */
/* Sample i of decoder channel c as a float in [-1, 1) */
static inline float
sample_as_float (const AVFrame * frame, gint c, gint i)
{
  switch (frame->format) {
    case AV_SAMPLE_FMT_FLTP:
      return ((const float *) frame->extended_data[c])[i];
    case AV_SAMPLE_FMT_S32P:
      return ((const gint32 *) frame->extended_data[c])[i] / 2147483648.0f;
    default:
      return ((const gint16 *) frame->extended_data[c])[i] / 32768.0f;
  }
}

static void
gst_dtsswdec_downmix (GstDtsSwDec * self, const AVFrame * frame,
    gpointer dest)
{
  gint channels = frame->ch_layout.nb_channels;
  gboolean s32 = self->out_format == GST_AUDIO_FORMAT_S32LE;
  gint32 *d32 = dest;
  gint16 *d16 = dest;
  gint c, i;

  for (i = 0; i < frame->nb_samples; i++) {
    float l = 0.0f, r = 0.0f;

    for (c = 0; c < channels; c++) {
      float v = sample_as_float (frame, c, i);
      l += v * self->mix[0][c];
      r += v * self->mix[1][c];
    }
    if (s32) {
      d32[2 * i] = flt_to_s32 (l);
      d32[2 * i + 1] = flt_to_s32 (r);
    } else {
      d16[2 * i] = flt_to_s16 (l);
      d16[2 * i + 1] = flt_to_s16 (r);
    }
  }
}

static gboolean
gst_dtsswdec_interleave (GstDtsSwDec * self, const AVFrame * frame,
    gpointer dest)
{
  gint channels = frame->ch_layout.nb_channels;
  gint n = frame->nb_samples;
  gboolean s32 = self->out_format == GST_AUDIO_FORMAT_S32LE;
  gint32 *d32 = dest;
  gint16 *d16 = dest;
  gint c, i;

  switch (frame->format) {
    case AV_SAMPLE_FMT_FLTP:
    case AV_SAMPLE_FMT_S32P:
    case AV_SAMPLE_FMT_S16P:
      break;
    default:
      GST_ELEMENT_ERROR (self, STREAM, DECODE, (NULL),
          ("unexpected sample format %s",
              av_get_sample_fmt_name (frame->format)));
      return FALSE;
  }
  if (self->downmixing) {
    gst_dtsswdec_downmix (self, frame, dest);
    return TRUE;
  }

  for (c = 0; c < channels; c++) {
    const uint8_t *plane = frame->extended_data[self->reorder[c]];

    switch (frame->format) {
      case AV_SAMPLE_FMT_FLTP:{
        const float *in = (const float *) plane;
        if (s32)
          for (i = 0; i < n; i++)
            d32[i * channels + c] = flt_to_s32 (in[i]);
        else
          for (i = 0; i < n; i++)
            d16[i * channels + c] = flt_to_s16 (in[i]);
        break;
      }
      case AV_SAMPLE_FMT_S32P:{
        const gint32 *in = (const gint32 *) plane;
        if (s32)
          for (i = 0; i < n; i++)
            d32[i * channels + c] = in[i];
        else
          for (i = 0; i < n; i++)
            d16[i * channels + c] = in[i] >> 16;
        break;
      }
      case AV_SAMPLE_FMT_S16P:{
        const gint16 *in = (const gint16 *) plane;
        if (s32)
          for (i = 0; i < n; i++)
            d32[i * channels + c] = (gint32) ((guint32) in[i] << 16);
        else
          for (i = 0; i < n; i++)
            d16[i * channels + c] = in[i];
        break;
      }
      default:
        g_assert_not_reached ();
    }
  }
  return TRUE;
}

/* Converts one decoded frame and appends it to *outbuf */
static GstFlowReturn
gst_dtsswdec_output (GstDtsSwDec * self, const AVFrame * frame,
    GstBuffer ** outbuf)
{
  GstAudioDecoder *dec = GST_AUDIO_DECODER (self);
  GstBuffer *buf;
  GstMapInfo map;
  gsize bps;
  gboolean ok;

  if (frame->nb_samples <= 0)
    return GST_FLOW_OK;

  if (!self->configured || frame->sample_rate != self->out_rate ||
      av_channel_layout_compare (&frame->ch_layout, &self->out_layout) != 0) {
    if (*outbuf) {
      /* one output buffer has one format; FFmpeg's DTS decoder never
       * returns more than one frame per packet anyway */
      GST_WARNING_OBJECT (self, "format changed within a frame, dropping");
      return GST_FLOW_OK;
    }
    if (!gst_dtsswdec_configure (self, frame))
      return GST_FLOW_NOT_NEGOTIATED;
  }

  bps = self->out_format == GST_AUDIO_FORMAT_S32LE ? 4 : 2;
  buf = gst_audio_decoder_allocate_output_buffer (dec,
      (gsize) frame->nb_samples * self->out_channels * bps);
  if (!buf || !gst_buffer_map (buf, &map, GST_MAP_WRITE)) {
    if (buf)
      gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
  ok = gst_dtsswdec_interleave (self, frame, map.data);
  gst_buffer_unmap (buf, &map);
  if (!ok) {
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  *outbuf = *outbuf ? gst_buffer_append (*outbuf, buf) : buf;
  return GST_FLOW_OK;
}

/* LG's audio sink ignores channel-mask. It sets up its mixer from two fields
 * that LG's own decoders add to their caps: the Dolby-style audio coding mode
 * "acmod" and "lfe-enable". Without them it assumes acmod 0 (two independent
 * mono channels) and plays only the first two channels. So add them to the
 * caps we send downstream; other sinks ignore unknown fields. */
static GstPadProbeReturn
gst_dtsswdec_add_acmod (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  const guint64 front = (1 << GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT) |
      (1 << GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT);
  const guint64 center = 1 << GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
  const guint64 lfe = 1 << GST_AUDIO_CHANNEL_POSITION_LFE1;
  const guint64 rear = 1 << GST_AUDIO_CHANNEL_POSITION_REAR_CENTER;
  const guint64 sides = (1 << GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT) |
      (1 << GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT);
  const guint64 rears = (1 << GST_AUDIO_CHANNEL_POSITION_REAR_LEFT) |
      (1 << GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT);
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GstStructure *s;
  GstCaps *caps;
  guint64 mask = 0, speakers;
  gint channels, acmod;

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  gst_event_parse_caps (event, &caps);
  s = gst_caps_get_structure (caps, 0);
  if (gst_structure_has_field (s, "acmod") ||
      !gst_structure_get_int (s, "channels", &channels))
    return GST_PAD_PROBE_OK;
  if (!gst_structure_get (s, "channel-mask", GST_TYPE_BITMASK, &mask, NULL)
      && channels == 2)
    mask = front;

  speakers = mask & ~lfe;
  if (channels == 1)
    acmod = 1;                  /* C */
  else if (speakers == front)
    acmod = 2;                  /* L R */
  else if (speakers == (front | center))
    acmod = 3;                  /* L C R */
  else if (speakers == (front | rear))
    acmod = 4;                  /* L R S */
  else if (speakers == (front | center | rear))
    acmod = 5;                  /* L C R S */
  else if (speakers == (front | sides) || speakers == (front | rears))
    acmod = 6;                  /* L R Ls Rs */
  else if (speakers == (front | center | sides) ||
      speakers == (front | center | rears))
    acmod = 7;                  /* L C R Ls Rs */
  else
    return GST_PAD_PROBE_OK;

  caps = gst_caps_copy (caps);
  gst_caps_set_simple (caps, "acmod", G_TYPE_UINT, acmod,
      "lfe-enable", G_TYPE_BOOLEAN, (mask & lfe) != 0, NULL);
  GST_DEBUG_OBJECT (pad, "output caps for LG's sink: %" GST_PTR_FORMAT, caps);
  GST_PAD_PROBE_INFO_DATA (info) = gst_event_new_caps (caps);
  gst_caps_unref (caps);
  gst_event_unref (event);
  return GST_PAD_PROBE_OK;
}

/* --- Decoding ----------------------------------------------------------- */

static GstFlowReturn
gst_dtsswdec_drop_frame (GstDtsSwDec * self, int err)
{
  char msg[AV_ERROR_MAX_STRING_SIZE];

  av_strerror (err, msg, sizeof (msg));
  self->bad_frames++;
  self->synced = FALSE;
  GST_WARNING_OBJECT (self, "dropping undecodable frame: %s", msg);

  if (self->bad_frames > MAX_BAD_FRAMES) {
    GST_ELEMENT_ERROR (self, STREAM, DECODE, (NULL),
        ("%u frames in a row failed to decode, last error: %s",
            self->bad_frames, msg));
    return GST_FLOW_ERROR;
  }
  return gst_audio_decoder_finish_frame (GST_AUDIO_DECODER (self), NULL, 1);
}

/* IEC 61937-5 DTS burst: Pa, Pb, Pc (data type), Pd (payload bits) */
#define IEC61937_PA 0xF872
#define IEC61937_PB 0x4E1F

static const gint dts_sample_rates[16] = {
  0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000,
  48000, 96000, 192000
};

/* Wraps the DTS core of one frame in an IEC 61937 burst, as S16LE stereo PCM
 * lasting exactly as long as the frame (like FFmpeg's spdif muxer). Returns
 * NULL if the frame has no 16-bit big-endian core or cannot be wrapped. */
static GstBuffer *
gst_dtsswdec_iec61937 (GstDtsSwDec * self, const guint8 * data, gsize size,
    gint * rate)
{
  GstBuffer *buf;
  GstMapInfo map;
  gsize core, period, i;
  gint blocks, type;
  guint16 *out;

  if (size < 9 || GST_READ_UINT32_BE (data) != DTS_SYNC_CORE_BE)
    return NULL;
  blocks = ((GST_READ_UINT16_BE (data + 4) >> 2) & 0x7f) + 1;
  core = dts_core_size_be (data);
  *rate = dts_sample_rates[(data[8] >> 2) & 0x0f];
  switch (blocks) {
    case 512 >> 5:
      type = 0x0B;
      break;
    case 1024 >> 5:
      type = 0x0C;
      break;
    case 2048 >> 5:
      type = 0x0D;
      break;
    default:
      return NULL;
  }
  period = (gsize) blocks << 7;           /* samples x 2 channels x 2 bytes */
  if (!core || core > size || !*rate || core + 8 > period)
    return NULL;

  buf = gst_audio_decoder_allocate_output_buffer (GST_AUDIO_DECODER (self),
      period);
  if (!buf || !gst_buffer_map (buf, &map, GST_MAP_WRITE)) {
    if (buf)
      gst_buffer_unref (buf);
    return NULL;
  }
  memset (map.data, 0, period);
  out = (guint16 *) map.data;
  out[0] = GUINT16_TO_LE (IEC61937_PA);
  out[1] = GUINT16_TO_LE (IEC61937_PB);
  out[2] = GUINT16_TO_LE (type);
  /* payload bits, as FFmpeg's spdif muxer: a frame that is all core counts
   * its odd last byte as a whole 16-bit word */
  out[3] = GUINT16_TO_LE ((core < size ? core : (core + 1) & ~(gsize) 1) << 3);
  /* the big-endian DTS words become little-endian samples */
  for (i = 0; i + 1 < core; i += 2)
    out[4 + i / 2] = GUINT16_TO_LE ((data[i] << 8) | data[i + 1]);
  if (core & 1)
    out[4 + core / 2] = GUINT16_TO_LE (data[core - 1] << 8);
  gst_buffer_unmap (buf, &map);
  return buf;
}

/* True if process pid is the sound output watcher (its PID may have been
 * reused if it died without cleaning up) */
static gboolean
is_watcher (const gchar * pid)
{
  gchar *path, *cmdline = NULL;
  gsize len = 0, i;
  gboolean found = FALSE;

  for (i = 0; pid[i]; i++)
    if (!g_ascii_isdigit (pid[i]))
      return FALSE;
  if (i == 0)
    return FALSE;

  path = g_strdup_printf ("/proc/%s/cmdline", pid);
  if (g_file_get_contents (path, &cmdline, &len, NULL)) {
    for (i = 0; i < len; i++)
      if (cmdline[i] == '\0')
        cmdline[i] = ' ';
    found = strstr (cmdline, "watch-output") != NULL;
    g_free (cmdline);
  }
  g_free (path);
  return found;
}

/* For arc-passthrough: is the TV sending sound to an HDMI ARC/eARC device
 * that takes the bitstream (Digital Sound Out set to Pass Through; Auto may
 * let the TV re-encode)? Anything unexpected, a missing file or a dead
 * watcher counts as no, because the TV speakers would play the bitstream as
 * loud noise. Looked up at most every 100 ms. */
static gboolean
gst_dtsswdec_output_is_arc (GstDtsSwDec * self)
{
  gint64 now = g_get_monotonic_time ();
  gboolean arc = FALSE;
  gchar *text = NULL;

  if (self->output_checked && now - self->output_checked < 100 * 1000)
    return self->output_is_arc;
  self->output_checked = now;

  if (g_file_get_contents (SOUND_OUTPUT_FILE, &text, NULL, NULL)) {
    gchar **field = g_strsplit (g_strstrip (text), " ", 3);

    if (g_strv_length (field) == 3)
      arc = strcmp (field[1], "external_arc") == 0 &&
          g_ascii_strcasecmp (field[2], "passThrough") == 0 &&
          is_watcher (field[0]);
    g_strfreev (field);
    g_free (text);
  }
  if (arc != self->output_is_arc)
    GST_INFO_OBJECT (self, "%s", arc ? "sound goes to HDMI ARC: passing DTS "
        "through" : "sound does not go to HDMI ARC: decoding to stereo");
  self->output_is_arc = arc;
  return arc;
}

/* LG's MS12 PCM renderer, which every app's PCM passes through on its way to
 * HDMI ARC/eARC, scales the samples by 63/64 while its "metadata type" is
 * at the default, 4. That turns an IEC 61937 bitstream into noise; with
 * metadata type 0 or 1 the samples come out unchanged. The setting is shared
 * by everything that plays, so arc-passthrough changes it only while it
 * passes DTS through and puts the old value back afterwards. */
#define RENDERER_STATUS "/proc/aaudio/module/PCMREN_P0/status"
#define RENDERER_INSTANCES "/proc/aaudio/instances"
#define RENDERER_SET_METADATA_TYPE 0x64040004

/* The renderer's metadata type ("= Param   : metadata type    : 4"), or -1
 * if it cannot be read */
static gint
renderer_metadata_type (void)
{
  gchar *text = NULL, **lines, **l;
  gint type = -1;

  if (!g_file_get_contents (RENDERER_STATUS, &text, NULL, NULL))
    return -1;
  lines = g_strsplit (text, "\n", -1);
  for (l = lines; *l; l++) {
    gchar *colon, *end;
    gint64 v;

    if (!strstr (*l, "Param") || !strstr (*l, "metadata type") ||
        !(colon = strrchr (*l, ':')))
      continue;
    v = g_ascii_strtoll (colon + 1, &end, 10);
    if (end != colon + 1 && v >= 0 && v < 16)
      type = v;
    break;
  }
  g_strfreev (lines);
  g_free (text);
  return type;
}

static gboolean
has_line (const gchar * text, const gchar * line)
{
  gsize n = strlen (line);
  const gchar *p;

  for (p = text; (p = strstr (p, line)); p += n)
    if ((p == text || p[-1] == '\n') && (p[n] == '\n' || p[n] == '\0'))
      return TRUE;
  return FALSE;
}

/* The command file of the renderer's DSP instance: the one whose properties
 * carry the module number its status reports ("=== PCMREN_P0[PCMREN_P0](39)
 * Info"). NULL if not found. */
static gchar *
renderer_control (void)
{
  gchar *text = NULL, *p, *want, *path = NULL;
  const gchar *name;
  gint64 id;
  GDir *dir;

  if (!g_file_get_contents (RENDERER_STATUS, &text, NULL, NULL))
    return NULL;
  p = strstr (text, "](");
  id = p ? g_ascii_strtoll (p + 2, NULL, 10) : 0;
  g_free (text);
  if (id <= 0 || !(dir = g_dir_open (RENDERER_INSTANCES, 0, NULL)))
    return NULL;

  want = g_strdup_printf ("id=(int)%" G_GINT64_FORMAT, id);
  while (!path && (name = g_dir_read_name (dir))) {
    gchar *prop = g_build_filename (RENDERER_INSTANCES, name, "prop", NULL);

    if (g_file_get_contents (prop, &text, NULL, NULL)) {
      if (has_line (text, want))
        path = g_build_filename (RENDERER_INSTANCES, name, "cmd", NULL);
      g_free (text);
    }
    g_free (prop);
  }
  g_free (want);
  g_dir_close (dir);
  return path;
}

/* Sets the metadata type and checks that the renderer took it */
static gboolean
renderer_set_metadata_type (const gchar * control, gint type)
{
  gchar *cmd = g_strdup_printf ("0x%08x %d\n", RENDERER_SET_METADATA_TYPE,
      type);
  gssize len = strlen (cmd);
  gboolean written = FALSE;
  gint i, fd;

  fd = open (control, O_WRONLY);
  if (fd >= 0) {
    written = write (fd, cmd, len) == len;
    close (fd);
  }
  g_free (cmd);
  for (i = 0; written && i < 10; i++) {
    if (renderer_metadata_type () == type)
      return TRUE;
    g_usleep (20 * 1000);
  }
  return FALSE;
}

/* Streams in other processes may pass through at the same time (one starting
 * while another stops), so the value to put back is kept where all of them
 * see it, and each stream relying on the renderer leaves a marker named after
 * its process. The last one to stop restores the value; markers of processes
 * that died do not count. */
#define RENDERER_SAVED_FILE RUNTIME_DIR "/renderer-metadata-type"
#define RENDERER_USER_PREFIX "renderer-user."

/* Does a live stream other than the one with marker mine rely on it? */
static gboolean
renderer_other_users (const gchar * mine)
{
  const gchar *name;
  gboolean found = FALSE;
  GDir *dir = g_dir_open (RUNTIME_DIR, 0, NULL);

  if (!dir)
    return FALSE;
  while (!found && (name = g_dir_read_name (dir))) {
    gchar *path, *proc;
    gint64 pid;

    if (!g_str_has_prefix (name, RENDERER_USER_PREFIX))
      continue;
    path = g_build_filename (RUNTIME_DIR, name, NULL);
    pid = g_ascii_strtoll (name + strlen (RENDERER_USER_PREFIX), NULL, 10);
    proc = g_strdup_printf ("/proc/%" G_GINT64_FORMAT, pid);
    found = strcmp (path, mine) != 0 && pid > 0 &&
        g_file_test (proc, G_FILE_TEST_IS_DIR);
    g_free (proc);
    g_free (path);
  }
  g_dir_close (dir);
  return found;
}

/* For arc-passthrough: makes sure the renderer leaves the samples unchanged,
 * looked at once a second, since another stream may change it. FALSE if that
 * cannot be done: the bitstream would be noise, so decode instead, and try
 * again a few seconds later. */
static gboolean
gst_dtsswdec_renderer_ready (GstDtsSwDec * self)
{
  gint64 now = g_get_monotonic_time ();
  gint type;

  if (self->renderer_checked && now - self->renderer_checked <
      (self->renderer_failed ? 5 : 1) * G_USEC_PER_SEC)
    return !self->renderer_failed;
  self->renderer_checked = now;

  if (!self->renderer_user) {
    gchar *path = g_strdup_printf (RUNTIME_DIR "/" RENDERER_USER_PREFIX "%d.%p",
        (int) getpid (), (void *) self);

    if (g_file_set_contents (path, "", 0, NULL))
      self->renderer_user = path;
    else
      g_free (path);
  }

  type = renderer_metadata_type ();
  if (self->renderer_user && (type == 0 || type == 1))
    goto ready;
  if (self->renderer_user && type >= 0) {
    gchar *saved = g_strdup_printf ("%d\n", type);
    gboolean ok;

    /* save the value first, in case the change lands after we give up */
    ok = renderer_other_users (self->renderer_user) ||
        g_file_set_contents (RENDERER_SAVED_FILE, saved, -1, NULL);
    g_free (saved);
    if (ok && !self->renderer_control)
      self->renderer_control = renderer_control ();
    if (ok && self->renderer_control &&
        renderer_set_metadata_type (self->renderer_control, 0)) {
      GST_INFO_OBJECT (self, "LG's PCM renderer: metadata type %d -> 0", type);
      goto ready;
    }
    /* the instance may have been replaced: look for it again next time */
    g_clear_pointer (&self->renderer_control, g_free);
  }
  if (!self->renderer_failed)
    GST_WARNING_OBJECT (self, "cannot make LG's PCM renderer leave samples "
        "unchanged: decoding to stereo for now");
  self->renderer_failed = TRUE;
  return FALSE;

ready:
  self->renderer_failed = FALSE;
  return TRUE;
}

/* Done relying on the renderer. The last stream to stop puts back the saved
 * metadata type, unless something else changed it since. */
static void
gst_dtsswdec_renderer_restore (GstDtsSwDec * self)
{
  gchar *text = NULL;

  self->renderer_checked = 0;
  self->renderer_failed = FALSE;
  if (!self->renderer_user)
    return;
  unlink (self->renderer_user);
  if (!renderer_other_users (self->renderer_user) &&
      g_file_get_contents (RENDERER_SAVED_FILE, &text, NULL, NULL)) {
    gint64 type = g_ascii_strtoll (text, NULL, 10);

    if (type > 1 && type < 16 && renderer_metadata_type () == 0) {
      if (!self->renderer_control)
        self->renderer_control = renderer_control ();
      if (self->renderer_control &&
          renderer_set_metadata_type (self->renderer_control, type))
        GST_INFO_OBJECT (self, "LG's PCM renderer: metadata type 0 -> %d",
            (gint) type);
      else
        GST_WARNING_OBJECT (self, "could not restore LG's PCM renderer");
    }
    unlink (RENDERER_SAVED_FILE);
    g_free (text);
  }
  g_clear_pointer (&self->renderer_user, g_free);
}

static GstFlowReturn
gst_dtsswdec_passthrough (GstDtsSwDec * self, GstBuffer * inbuf,
    gboolean * done)
{
  GstBuffer *outbuf;
  GstMapInfo map;
  gint rate = 0;

  *done = FALSE;
  if (!gst_buffer_map (inbuf, &map, GST_MAP_READ))
    return GST_FLOW_ERROR;
  outbuf = gst_dtsswdec_iec61937 (self, map.data, map.size, &rate);
  gst_buffer_unmap (inbuf, &map);
  if (!outbuf)
    return GST_FLOW_OK;         /* decode this frame instead */

  if (rate != self->passthrough_rate) {
    GstAudioChannelPosition pos[2] = { GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
      GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT
    };
    GstAudioInfo info;

    gst_audio_info_init (&info);
    gst_audio_info_set_format (&info, GST_AUDIO_FORMAT_S16LE, rate, 2, pos);
    GST_INFO_OBJECT (self, "passing the DTS core through as IEC 61937, %d Hz",
        rate);
    if (!gst_audio_decoder_set_output_format (GST_AUDIO_DECODER (self), &info)) {
      gst_buffer_unref (outbuf);
      return GST_FLOW_NOT_NEGOTIATED;
    }
    self->passthrough_rate = rate;
    /* a decoded frame after this must set its own format again */
    self->configured = FALSE;
  }
  *done = TRUE;
  self->bad_frames = 0;
  return gst_audio_decoder_finish_frame (GST_AUDIO_DECODER (self), outbuf, 1);
}

static GstFlowReturn
gst_dtsswdec_handle_frame (GstAudioDecoder * dec, GstBuffer * inbuf)
{
  GstDtsSwDec *self = GST_DTSSWDEC (dec);
  GstBuffer *outbuf = NULL;
  GstFlowReturn flow = GST_FLOW_OK;
  GstMapInfo map;
  int ret;

  /* Every DTS frame decodes on its own, so there is nothing to drain */
  if (inbuf == NULL)
    return GST_FLOW_OK;

  if (self->downmix_mode == DOWNMIX_PASSTHROUGH ||
      (self->downmix_mode == DOWNMIX_ARC_PASSTHROUGH &&
          gst_dtsswdec_output_is_arc (self) &&
          gst_dtsswdec_renderer_ready (self))) {
    gboolean done;

    flow = gst_dtsswdec_passthrough (self, inbuf, &done);
    if (done || flow != GST_FLOW_OK)
      return flow;
  } else if (self->renderer_user) {
    gst_dtsswdec_renderer_restore (self);
  }

  if (!gst_buffer_map (inbuf, &map, GST_MAP_READ))
    return GST_FLOW_ERROR;
  /* FFmpeg needs zeroed padding after the data, so copy into a packet */
  ret = av_new_packet (self->pkt, map.size);
  if (ret == 0)
    memcpy (self->pkt->data, map.data, map.size);
  gst_buffer_unmap (inbuf, &map);
  if (ret < 0)
    return GST_FLOW_ERROR;

  ret = avcodec_send_packet (self->ctx, self->pkt);
  av_packet_unref (self->pkt);

  while (ret >= 0) {
    ret = avcodec_receive_frame (self->ctx, self->frame);
    if (ret < 0)
      break;
    flow = gst_dtsswdec_output (self, self->frame, &outbuf);
    av_frame_unref (self->frame);
    if (flow != GST_FLOW_OK) {
      if (outbuf)
        gst_buffer_unref (outbuf);
      return flow;
    }
  }

  if (ret != AVERROR (EAGAIN) && ret != AVERROR_EOF) {
    if (outbuf)
      gst_buffer_unref (outbuf);
    return gst_dtsswdec_drop_frame (self, ret);
  }

  self->bad_frames = 0;
  return gst_audio_decoder_finish_frame (dec, outbuf, 1);
}

static gboolean gst_dtsswdec_stop (GstAudioDecoder * dec);

/* LG's audio output plays only the first two channels of multichannel PCM on
 * the TV speakers (front left and right, dropping the rest), so stereo is the
 * fallback */
static gint
gst_dtsswdec_auto_downmix (GstDtsSwDec * self)
{
  GEnumClass *klass = g_type_class_ref (GST_TYPE_DTSSWDEC_DOWNMIX);
  gint mode = DOWNMIX_STEREO;
  gchar *text = NULL;

  if (g_file_get_contents (DOWNMIX_SETTING_FILE, &text, NULL, NULL)) {
    GEnumValue *value = g_enum_get_value_by_nick (klass, g_strstrip (text));

    /* not "passthrough": that would reach the TV speakers as loud noise */
    if (value && (value->value == DOWNMIX_STEREO ||
            value->value == DOWNMIX_5_1 || value->value == DOWNMIX_NONE ||
            value->value == DOWNMIX_ARC_PASSTHROUGH))
      mode = value->value;
    else
      GST_WARNING_OBJECT (self, "ignoring unknown setting '%s' in %s", text,
          DOWNMIX_SETTING_FILE);
    g_free (text);
  }
  GST_INFO_OBJECT (self, "downmix: %s", g_enum_get_value (klass, mode)->value_nick);
  g_type_class_unref (klass);
  return mode;
}

static gboolean
gst_dtsswdec_start (GstAudioDecoder * dec)
{
  GstDtsSwDec *self = GST_DTSSWDEC (dec);
  const AVCodec *codec = avcodec_find_decoder (AV_CODEC_ID_DTS);
  AVDictionary *opts = NULL;
  gboolean core_only;
  int ret;

  GST_OBJECT_LOCK (self);
  core_only = self->core_only;
  self->downmix_mode = self->downmix;
  GST_OBJECT_UNLOCK (self);
  if (self->downmix_mode == DOWNMIX_AUTO)
    self->downmix_mode = gst_dtsswdec_auto_downmix (self);

  self->ctx = avcodec_alloc_context3 (codec);
  self->pkt = av_packet_alloc ();
  self->frame = av_frame_alloc ();
  if (!codec || !self->ctx || !self->pkt || !self->frame) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL),
        ("could not set up the DTS decoder"));
    gst_dtsswdec_stop (dec);
    return FALSE;
  }

  av_dict_set (&opts, "core_only", core_only ? "1" : "0", 0);
  /* Both only affect streams with more channels. Stereo uses the downmix
   * coefficients in the stream if there are any; 5.1 makes DTS-HD MA 7.1
   * decode its own lossless 5.1 base. */
  /* in passthrough mode, frames without a DTS core are decoded to stereo */
  if (self->downmix_mode == DOWNMIX_STEREO ||
      self->downmix_mode == DOWNMIX_PASSTHROUGH ||
      self->downmix_mode == DOWNMIX_ARC_PASSTHROUGH)
    av_dict_set (&opts, "downmix", "stereo", 0);
  else if (self->downmix_mode == DOWNMIX_5_1)
    /* FFmpeg's "5.1" is the back-surround layout, which the DTS decoder
     * rejects */
    av_dict_set (&opts, "downmix", "5.1(side)", 0);
  ret = avcodec_open2 (self->ctx, codec, &opts);
  av_dict_free (&opts);
  if (ret < 0) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL),
        ("could not open the DTS decoder: %s", av_err2str (ret)));
    gst_dtsswdec_stop (dec);
    return FALSE;
  }

  self->synced = FALSE;
  self->bad_frames = 0;
  self->configured = FALSE;
  self->passthrough_rate = 0;
  self->output_checked = 0;
  self->output_is_arc = FALSE;
  self->renderer_checked = 0;
  self->renderer_failed = FALSE;

  /* With no dcaparse on the TV, raw DTS arrives in a BYTES segment; this
   * lets the base class convert it to TIME. Set here because the base class
   * resets it just before calling start(). */
  gst_audio_decoder_set_estimate_rate (dec, TRUE);
  return TRUE;
}

static gboolean
gst_dtsswdec_stop (GstAudioDecoder * dec)
{
  GstDtsSwDec *self = GST_DTSSWDEC (dec);

  gst_dtsswdec_renderer_restore (self);
  g_clear_pointer (&self->renderer_control, g_free);
  avcodec_free_context (&self->ctx);
  av_packet_free (&self->pkt);
  av_frame_free (&self->frame);
  av_channel_layout_uninit (&self->out_layout);
  self->configured = FALSE;
  return TRUE;
}

static gboolean
gst_dtsswdec_set_format (GstAudioDecoder * dec, GstCaps * caps)
{
  GST_DEBUG_OBJECT (dec, "input caps %" GST_PTR_FORMAT, caps);
  return TRUE;
}

static void
gst_dtsswdec_flush (GstAudioDecoder * dec, gboolean hard)
{
  GstDtsSwDec *self = GST_DTSSWDEC (dec);

  if (self->ctx)
    avcodec_flush_buffers (self->ctx);
  self->synced = FALSE;
}

/* --- GObject boilerplate ------------------------------------------------ */

static void
gst_dtsswdec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDtsSwDec *self = GST_DTSSWDEC (object);

  switch (prop_id) {
    case PROP_CORE_ONLY:
      GST_OBJECT_LOCK (self);
      self->core_only = g_value_get_boolean (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_DOWNMIX:
      GST_OBJECT_LOCK (self);
      self->downmix = g_value_get_enum (value);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_dtsswdec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDtsSwDec *self = GST_DTSSWDEC (object);

  switch (prop_id) {
    case PROP_CORE_ONLY:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->core_only);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_DOWNMIX:
      GST_OBJECT_LOCK (self);
      g_value_set_enum (value, self->downmix);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_dtsswdec_class_init (GstDtsSwDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAudioDecoderClass *dec_class = GST_AUDIO_DECODER_CLASS (klass);

  gobject_class->set_property = gst_dtsswdec_set_property;
  gobject_class->get_property = gst_dtsswdec_get_property;

  g_object_class_install_property (gobject_class, PROP_CORE_ONLY,
      g_param_spec_boolean ("core-only", "Core only",
          "Decode only the DTS core and skip DTS-HD extensions (less CPU, "
          "lossy 5.1 at most). Takes effect when the element starts.",
          DEFAULT_CORE_ONLY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_DOWNMIX,
      g_param_spec_enum ("downmix", "Downmix",
          "Channels to output. LG's audio output plays only the first two "
          "channels of multichannel PCM on the TV speakers, hence stereo "
          "unless " DOWNMIX_SETTING_FILE " says otherwise. Takes effect when "
          "the element starts.", GST_TYPE_DTSSWDEC_DOWNMIX,
          DEFAULT_DOWNMIX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class,
      "DTS audio decoder (FFmpeg)", "Codec/Decoder/Audio",
      "Decodes DTS and DTS-HD audio with FFmpeg's DTS decoder",
      "webos-dts-decoder contributors");

  dec_class->start = GST_DEBUG_FUNCPTR (gst_dtsswdec_start);
  dec_class->stop = GST_DEBUG_FUNCPTR (gst_dtsswdec_stop);
  dec_class->set_format = GST_DEBUG_FUNCPTR (gst_dtsswdec_set_format);
  dec_class->parse = GST_DEBUG_FUNCPTR (gst_dtsswdec_parse);
  dec_class->handle_frame = GST_DEBUG_FUNCPTR (gst_dtsswdec_handle_frame);
  dec_class->flush = GST_DEBUG_FUNCPTR (gst_dtsswdec_flush);
}

static void
gst_dtsswdec_init (GstDtsSwDec * self)
{
  GstAudioDecoder *dec = GST_AUDIO_DECODER (self);

  self->core_only = DEFAULT_CORE_ONLY;
  self->downmix = DEFAULT_DOWNMIX;
  /* Matroska rounds timestamps to the millisecond; count samples instead and
   * only follow upstream timestamps when they differ by more than that */
  gst_audio_decoder_set_tolerance (dec, 2 * GST_MSECOND);
  gst_audio_decoder_set_use_default_pad_acceptcaps (dec, TRUE);
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_AUDIO_DECODER_SINK_PAD (dec));
  gst_pad_add_probe (GST_AUDIO_DECODER_SRC_PAD (dec),
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, gst_dtsswdec_add_acmod, NULL, NULL);
}

/* --- Plugin ------------------------------------------------------------- */

static void
gst_dtsswdec_av_log (void *avcl, int level, const char *fmt, va_list vl)
{
  GstDebugLevel gst_level;
  gchar *msg;

  if (level <= AV_LOG_ERROR)
    gst_level = GST_LEVEL_WARNING;
  else if (level <= AV_LOG_WARNING)
    gst_level = GST_LEVEL_INFO;
  else if (level <= AV_LOG_VERBOSE)
    gst_level = GST_LEVEL_DEBUG;
  else
    gst_level = GST_LEVEL_LOG;

  if (gst_level > gst_debug_category_get_threshold (dtsswdec_debug))
    return;

  msg = g_strdup_vprintf (fmt, vl);
  g_strchomp (msg);
  GST_CAT_LEVEL_LOG (dtsswdec_debug, gst_level, NULL, "ffmpeg: %s", msg);
  g_free (msg);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GTypeQuery query;

  GST_DEBUG_CATEGORY_INIT (dtsswdec_debug, "dtsswdec", 0,
      "DTS decoder (FFmpeg)");

  /* Refuse to load if the GstAudioDecoder we run against has a different
   * layout from the 1.14 headers we were built with (e.g. a vendor patch). */
  g_type_query (GST_TYPE_AUDIO_DECODER, &query);
  if (query.class_size != sizeof (GstAudioDecoderClass) ||
      query.instance_size != sizeof (GstAudioDecoder)) {
    GST_ERROR ("GstAudioDecoder ABI mismatch (class %u/%u, instance %u/%u)",
        query.class_size, (guint) sizeof (GstAudioDecoderClass),
        query.instance_size, (guint) sizeof (GstAudioDecoder));
    return FALSE;
  }

  av_log_set_callback (gst_dtsswdec_av_log);

  return gst_element_register (plugin, "dtsswdec", GST_RANK_PRIMARY,
      GST_TYPE_DTSSWDEC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, dtsswdec,
    "DTS audio decoder using FFmpeg", plugin_init, VERSION, "LGPL", PACKAGE,
    ORIGIN)
