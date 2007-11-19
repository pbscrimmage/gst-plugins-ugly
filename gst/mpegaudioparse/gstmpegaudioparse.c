/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) <2006-2007> Jan Schmidt <thaytan@mad.scientist.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstmpegaudioparse.h"

GST_DEBUG_CATEGORY_STATIC (mp3parse_debug);
#define GST_CAT_DEFAULT mp3parse_debug


/* elementfactory information */
static GstElementDetails mp3parse_details = {
  "MPEG1 Audio Parser",
  "Codec/Parser/Audio",
  "Parses and frames mpeg1 audio streams (levels 1-3), provides seek",
  "Jan Schmidt <thaytan@mad.scientist.com>\n"
      "Erik Walthinsen <omega@cse.ogi.edu>"
};

static GstStaticPadTemplate mp3_src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/mpeg, "
        "mpegversion = (int) 1, "
        "layer = (int) [ 1, 3 ], "
        "rate = (int) [ 8000, 48000 ], channels = (int) [ 1, 2 ],"
        "parsed=(boolean) true")
    );

static GstStaticPadTemplate mp3_sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/mpeg, mpegversion = (int) 1, parsed=(boolean)false")
    );

/* GstMPEGAudioParse signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_SKIP,
  ARG_BIT_RATE
      /* FILL ME */
};


static void gst_mp3parse_class_init (GstMPEGAudioParseClass * klass);
static void gst_mp3parse_base_init (gpointer klass);
static void gst_mp3parse_init (GstMPEGAudioParse * mp3parse,
    GstMPEGAudioParseClass * klass);

static gboolean gst_mp3parse_sink_event (GstPad * pad, GstEvent * event);
static GstFlowReturn gst_mp3parse_chain (GstPad * pad, GstBuffer * buffer);
static gboolean mp3parse_src_query (GstPad * pad, GstQuery * query);
static const GstQueryType *mp3parse_get_query_types (GstPad * pad);
static gboolean mp3parse_src_event (GstPad * pad, GstEvent * event);

static int head_check (GstMPEGAudioParse * mp3parse, unsigned long head);

static void gst_mp3parse_dispose (GObject * object);
static void gst_mp3parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_mp3parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstStateChangeReturn gst_mp3parse_change_state (GstElement * element,
    GstStateChange transition);

static gboolean mp3parse_bytepos_to_time (GstMPEGAudioParse * mp3parse,
    gint64 bytepos, GstClockTime * ts);
static gboolean
mp3parse_total_bytes (GstMPEGAudioParse * mp3parse, gint64 * total);

/*static guint gst_mp3parse_signals[LAST_SIGNAL] = { 0 }; */

GST_BOILERPLATE (GstMPEGAudioParse, gst_mp3parse, GstElement, GST_TYPE_ELEMENT);

static const guint mp3types_bitrates[2][3][16] = {
  {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448,},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,}
      },
  {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256,},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,}
      },
};

static const guint mp3types_freqs[3][3] = { {44100, 48000, 32000},
{22050, 24000, 16000},
{11025, 12000, 8000}
};

static inline guint
mp3_type_frame_length_from_header (GstMPEGAudioParse * mp3parse, guint32 header,
    guint * put_version, guint * put_layer, guint * put_channels,
    guint * put_bitrate, guint * put_samplerate)
{
  guint length;
  gulong mode, samplerate, bitrate, layer, channels, padding;
  gint lsf, mpg25;

  if (header & (1 << 20)) {
    lsf = (header & (1 << 19)) ? 0 : 1;
    mpg25 = 0;
  } else {
    lsf = 1;
    mpg25 = 1;
  }

  layer = 4 - ((header >> 17) & 0x3);

  bitrate = (header >> 12) & 0xF;
  bitrate = mp3types_bitrates[lsf][layer - 1][bitrate] * 1000;
  if (bitrate == 0)
    return 0;

  samplerate = (header >> 10) & 0x3;
  samplerate = mp3types_freqs[lsf + mpg25][samplerate];

  padding = (header >> 9) & 0x1;

  mode = (header >> 6) & 0x3;
  channels = (mode == 3) ? 1 : 2;

  switch (layer) {
    case 1:
      length = 4 * ((bitrate * 12) / samplerate + padding);
      break;
    case 2:
      length = (bitrate * 144) / samplerate + padding;
      break;
    default:
    case 3:
      length = (bitrate * 144) / (samplerate << lsf) + padding;
      break;
  }

  GST_DEBUG_OBJECT (mp3parse, "Calculated mp3 frame length of %u bytes",
      length);
  GST_DEBUG_OBJECT (mp3parse, "samplerate = %lu, bitrate = %lu, layer = %lu, "
      "channels = %lu", samplerate, bitrate, layer, channels);

  if (put_version)
    *put_version = lsf ? 2 : 1;
  if (put_layer)
    *put_layer = layer;
  if (put_channels)
    *put_channels = channels;
  if (put_bitrate)
    *put_bitrate = bitrate;
  if (put_samplerate)
    *put_samplerate = samplerate;

  return length;
}

static GstCaps *
mp3_caps_create (guint layer, guint channels, guint bitrate, guint samplerate)
{
  GstCaps *new;

  g_assert (layer);
  g_assert (samplerate);
  g_assert (bitrate);
  g_assert (channels);

  new = gst_caps_new_simple ("audio/mpeg",
      "mpegversion", G_TYPE_INT, 1,
      "layer", G_TYPE_INT, layer,
      "rate", G_TYPE_INT, samplerate,
      "channels", G_TYPE_INT, channels, "parsed", G_TYPE_BOOLEAN, TRUE, NULL);

  return new;
}

static void
gst_mp3parse_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&mp3_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&mp3_src_template));
  gst_element_class_set_details (element_class, &mp3parse_details);
}

static void
gst_mp3parse_class_init (GstMPEGAudioParseClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = gst_mp3parse_set_property;
  gobject_class->get_property = gst_mp3parse_get_property;
  gobject_class->dispose = gst_mp3parse_dispose;

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_SKIP,
      g_param_spec_int ("skip", "skip", "skip",
          G_MININT, G_MAXINT, 0, G_PARAM_READWRITE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_BIT_RATE,
      g_param_spec_int ("bitrate", "Bitrate", "Bit Rate",
          G_MININT, G_MAXINT, 0, G_PARAM_READABLE));

  gstelement_class->change_state = gst_mp3parse_change_state;
}

static void
gst_mp3parse_reset (GstMPEGAudioParse * mp3parse)
{
  mp3parse->skip = 0;
  mp3parse->resyncing = TRUE;
  mp3parse->next_ts = GST_CLOCK_TIME_NONE;
  mp3parse->cur_offset = -1;

  mp3parse->tracked_offset = 0;
  mp3parse->pending_ts = GST_CLOCK_TIME_NONE;
  mp3parse->pending_offset = -1;

  gst_adapter_clear (mp3parse->adapter);

  mp3parse->rate = mp3parse->channels = mp3parse->layer = -1;
  mp3parse->version = 1;
  mp3parse->max_bitreservoir = GST_CLOCK_TIME_NONE;

  mp3parse->avg_bitrate = 0;
  mp3parse->bitrate_sum = 0;
  mp3parse->last_posted_bitrate = 0;
  mp3parse->frame_count = 0;
  mp3parse->sent_codec_tag = FALSE;

  mp3parse->xing_flags = 0;
  mp3parse->xing_bitrate = 0;

  if (mp3parse->seek_table) {
    g_list_foreach (mp3parse->seek_table, (GFunc) g_free, NULL);
    g_list_free (mp3parse->seek_table);
    mp3parse->seek_table = NULL;
  }

  g_mutex_lock (mp3parse->pending_accurate_seeks_lock);
  if (mp3parse->pending_accurate_seeks) {
    g_slist_foreach (mp3parse->pending_accurate_seeks, (GFunc) g_free, NULL);
    g_slist_free (mp3parse->pending_accurate_seeks);
    mp3parse->pending_accurate_seeks = NULL;
  }
  g_mutex_unlock (mp3parse->pending_accurate_seeks_lock);

  if (mp3parse->pending_segment) {
    GstEvent **eventp = &mp3parse->pending_segment;

    gst_event_replace (eventp, NULL);
  }

  mp3parse->exact_position = FALSE;
  gst_segment_init (&mp3parse->segment, GST_FORMAT_TIME);
}

static void
gst_mp3parse_init (GstMPEGAudioParse * mp3parse, GstMPEGAudioParseClass * klass)
{
  mp3parse->sinkpad =
      gst_pad_new_from_static_template (&mp3_sink_template, "sink");
  gst_pad_set_event_function (mp3parse->sinkpad, gst_mp3parse_sink_event);
  gst_pad_set_chain_function (mp3parse->sinkpad, gst_mp3parse_chain);
  gst_element_add_pad (GST_ELEMENT (mp3parse), mp3parse->sinkpad);

  mp3parse->srcpad =
      gst_pad_new_from_static_template (&mp3_src_template, "src");
  gst_pad_use_fixed_caps (mp3parse->srcpad);
  gst_pad_set_event_function (mp3parse->srcpad, mp3parse_src_event);
  gst_pad_set_query_function (mp3parse->srcpad, mp3parse_src_query);
  gst_pad_set_query_type_function (mp3parse->srcpad, mp3parse_get_query_types);
  gst_element_add_pad (GST_ELEMENT (mp3parse), mp3parse->srcpad);

  mp3parse->adapter = gst_adapter_new ();
  mp3parse->pending_accurate_seeks_lock = g_mutex_new ();

  gst_mp3parse_reset (mp3parse);
}

static void
gst_mp3parse_dispose (GObject * object)
{
  GstMPEGAudioParse *mp3parse = GST_MP3PARSE (object);

  gst_mp3parse_reset (mp3parse);

  if (mp3parse->adapter) {
    g_object_unref (mp3parse->adapter);
    mp3parse->adapter = NULL;
  }
  g_mutex_free (mp3parse->pending_accurate_seeks_lock);
  mp3parse->pending_accurate_seeks_lock = NULL;

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static gboolean
gst_mp3parse_sink_event (GstPad * pad, GstEvent * event)
{
  gboolean res;
  GstMPEGAudioParse *mp3parse;
  GstEvent **eventp;

  mp3parse = GST_MP3PARSE (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NEWSEGMENT:
    {
      gdouble rate, applied_rate;
      GstFormat format;
      gint64 start, stop, pos;
      gboolean update;

      gst_event_parse_new_segment_full (event, &update, &rate, &applied_rate,
          &format, &start, &stop, &pos);

      g_mutex_lock (mp3parse->pending_accurate_seeks_lock);
      if (format == GST_FORMAT_BYTES && mp3parse->pending_accurate_seeks) {
        MPEGAudioPendingAccurateSeek *seek = NULL;
        GSList *node;

        for (node = mp3parse->pending_accurate_seeks; node; node = node->next) {
          MPEGAudioPendingAccurateSeek *tmp = node->data;

          if (tmp->upstream_start == pos) {
            seek = tmp;
            break;
          }
        }
        if (seek) {
          GstSegment *s = &seek->segment;

          event =
              gst_event_new_new_segment_full (FALSE, s->rate, s->applied_rate,
              GST_FORMAT_TIME, s->start, s->stop, s->last_stop);

          mp3parse->segment = seek->segment;

          mp3parse->resyncing = FALSE;
          mp3parse->cur_offset = pos;
          mp3parse->next_ts = seek->timestamp_start;
          mp3parse->pending_ts = GST_CLOCK_TIME_NONE;
          mp3parse->tracked_offset = 0;

          gst_event_parse_new_segment_full (event, &update, &rate,
              &applied_rate, &format, &start, &stop, &pos);

          GST_DEBUG_OBJECT (mp3parse,
              "Pushing accurate newseg rate %g, applied rate %g, "
              "format %d, start %lld, stop %lld, pos %lld\n", rate,
              applied_rate, format, start, stop, pos);

          g_free (seek);
          mp3parse->pending_accurate_seeks =
              g_slist_delete_link (mp3parse->pending_accurate_seeks, node);

          g_mutex_unlock (mp3parse->pending_accurate_seeks_lock);
          if (s->flags & GST_SEEK_FLAG_SEGMENT) {
            gst_element_post_message (GST_ELEMENT_CAST (mp3parse),
                gst_message_new_segment_start (GST_OBJECT_CAST (mp3parse),
                    s->format, s->last_stop));
          }
          res = gst_pad_push_event (mp3parse->srcpad, event);

          return res;
        } else {
          GST_WARNING_OBJECT (mp3parse,
              "Accurate seek not possible, didn't get an appropiate upstream segment");
        }
      }
      g_mutex_unlock (mp3parse->pending_accurate_seeks_lock);

      mp3parse->exact_position = FALSE;

      if (format == GST_FORMAT_BYTES) {
        GstClockTime seg_start, seg_stop, seg_pos;

        /* stop time is allowed to be open-ended, but not start & pos */
        if (!mp3parse_bytepos_to_time (mp3parse, stop, &seg_stop))
          seg_stop = GST_CLOCK_TIME_NONE;
        if (mp3parse_bytepos_to_time (mp3parse, start, &seg_start) &&
            mp3parse_bytepos_to_time (mp3parse, pos, &seg_pos)) {
          gst_event_unref (event);
          event = gst_event_new_new_segment_full (update, rate, applied_rate,
              GST_FORMAT_TIME, seg_start, seg_stop, seg_pos);
          format = GST_FORMAT_TIME;
          GST_DEBUG_OBJECT (mp3parse, "Converted incoming segment to TIME. "
              "start = %" GST_TIME_FORMAT ", stop = %" GST_TIME_FORMAT
              ", pos = %" GST_TIME_FORMAT, GST_TIME_ARGS (seg_start),
              GST_TIME_ARGS (seg_stop), GST_TIME_ARGS (seg_pos));
        }
      } else {
        if (format != GST_FORMAT_TIME) {
          /* Unknown incoming segment format. Output a default open-ended 
           * TIME segment */
          gst_event_unref (event);
          event = gst_event_new_new_segment_full (update, rate, applied_rate,
              GST_FORMAT_TIME, 0, GST_CLOCK_TIME_NONE, 0);
        }
      }

      mp3parse->resyncing = TRUE;
      mp3parse->cur_offset = -1;
      mp3parse->next_ts = GST_CLOCK_TIME_NONE;
      mp3parse->pending_ts = GST_CLOCK_TIME_NONE;
      mp3parse->tracked_offset = 0;

      gst_event_parse_new_segment_full (event, &update, &rate, &applied_rate,
          &format, &start, &stop, &pos);
      GST_DEBUG_OBJECT (mp3parse, "Pushing newseg rate %g, applied rate %g, "
          "format %d, start %lld, stop %lld, pos %lld\n",
          rate, applied_rate, format, start, stop, pos);

      gst_segment_set_newsegment_full (&mp3parse->segment, update, rate,
          applied_rate, format, start, stop, pos);

      /* save the segment for later, right before we push a new buffer so that
       * the caps are fixed and the next linked element can receive the segment. */
      eventp = &mp3parse->pending_segment;
      gst_event_replace (eventp, event);
      gst_event_unref (event);
      res = TRUE;
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      /* Clear our adapter and set up for a new position */
      gst_adapter_clear (mp3parse->adapter);
      eventp = &mp3parse->pending_segment;
      gst_event_replace (eventp, NULL);
      res = gst_pad_push_event (mp3parse->srcpad, event);
      break;
    default:
      res = gst_pad_push_event (mp3parse->srcpad, event);
      break;
  }

  gst_object_unref (mp3parse);

  return res;
}

static MPEGAudioSeekEntry *
mp3parse_seek_table_last_entry (GstMPEGAudioParse * mp3parse)
{
  MPEGAudioSeekEntry *ret = NULL;

  if (mp3parse->seek_table) {
    ret = mp3parse->seek_table->data;
  }

  return ret;
}

/* Prepare a buffer of the indicated size, timestamp it and output */
static GstFlowReturn
gst_mp3parse_emit_frame (GstMPEGAudioParse * mp3parse, guint size)
{
  GstBuffer *outbuf;
  guint bitrate;
  GstFlowReturn ret = GST_FLOW_OK;
  GstClockTime push_start;

  outbuf = gst_adapter_take_buffer (mp3parse->adapter, size);

  GST_BUFFER_DURATION (outbuf) =
      gst_util_uint64_scale (GST_SECOND, mp3parse->spf, mp3parse->rate);

  GST_BUFFER_OFFSET (outbuf) = mp3parse->cur_offset;

  /* Check if we have a pending timestamp from an incoming buffer to apply
   * here */
  if (GST_CLOCK_TIME_IS_VALID (mp3parse->pending_ts)) {
    if (mp3parse->tracked_offset >= mp3parse->pending_offset) {
      /* If the incoming timestamp differs from our expected by more than 2
       * 90khz MPEG ticks, then take it and, if needed, set the discont flag. 
       * This avoids creating imperfect streams just because of 
       * quantization in the MPEG clock sampling */
      GstClockTimeDiff diff = mp3parse->next_ts - mp3parse->pending_ts;

      if (diff < -2 * (GST_SECOND / 90000) || diff > 2 * (GST_SECOND / 90000)) {
        GST_DEBUG_OBJECT (mp3parse, "Updating next_ts from %" GST_TIME_FORMAT
            " to pending ts %" GST_TIME_FORMAT
            " at offset %lld (pending offset was %lld)",
            GST_TIME_ARGS (mp3parse->next_ts),
            GST_TIME_ARGS (mp3parse->pending_ts), mp3parse->tracked_offset,
            mp3parse->pending_offset);

        /* Only set discont if we sent out some timestamps already and we're
         * adjusting */
        if (GST_CLOCK_TIME_IS_VALID (mp3parse->next_ts))
          GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DISCONT);
        mp3parse->next_ts = mp3parse->pending_ts;
      }
      mp3parse->pending_ts = GST_CLOCK_TIME_NONE;
    }
  }

  /* Decide what timestamp we're going to apply */
  if (GST_CLOCK_TIME_IS_VALID (mp3parse->next_ts)) {
    GST_BUFFER_TIMESTAMP (outbuf) = mp3parse->next_ts;
  } else {
    GstClockTime ts;

    /* No timestamp yet, convert our offset to a timestamp if we can, or
     * start at 0 */
    if (mp3parse_bytepos_to_time (mp3parse, mp3parse->cur_offset, &ts))
      GST_BUFFER_TIMESTAMP (outbuf) = ts;
    else {
      GST_BUFFER_TIMESTAMP (outbuf) = 0;
    }
  }

  if (GST_BUFFER_TIMESTAMP (outbuf) == 0)
    mp3parse->exact_position = TRUE;

  if (mp3parse->exact_position && (!mp3parse->seek_table ||
          (mp3parse_seek_table_last_entry (mp3parse))->byte <
          GST_BUFFER_OFFSET (outbuf))) {
    MPEGAudioSeekEntry *entry = g_new0 (MPEGAudioSeekEntry, 1);

    entry->byte = mp3parse->cur_offset;
    entry->timestamp = GST_BUFFER_TIMESTAMP (outbuf);
    mp3parse->seek_table = g_list_prepend (mp3parse->seek_table, entry);
    GST_DEBUG_OBJECT (mp3parse, "Adding index entry %" GST_TIME_FORMAT
        " @ offset 0x%08" G_GINT64_MODIFIER "x",
        GST_TIME_ARGS (entry->timestamp), entry->byte);
  }

  /* Update our byte offset tracking */
  if (mp3parse->cur_offset != -1) {
    mp3parse->cur_offset += size;
  }
  mp3parse->tracked_offset += size;

  mp3parse->next_ts =
      GST_BUFFER_TIMESTAMP (outbuf) + GST_BUFFER_DURATION (outbuf);

  gst_buffer_set_caps (outbuf, GST_PAD_CAPS (mp3parse->srcpad));

  /* Post a bitrate tag if we need to before pushing the buffer */
  if (mp3parse->xing_bitrate != 0)
    bitrate = mp3parse->xing_bitrate;
  else
    bitrate = mp3parse->avg_bitrate;

  if ((mp3parse->last_posted_bitrate / 10000) != (bitrate / 10000)) {
    GstTagList *taglist = gst_tag_list_new ();

    mp3parse->last_posted_bitrate = bitrate;
    gst_tag_list_add (taglist, GST_TAG_MERGE_REPLACE, GST_TAG_BITRATE,
        mp3parse->last_posted_bitrate, NULL);
    gst_element_found_tags_for_pad (GST_ELEMENT (mp3parse),
        mp3parse->srcpad, taglist);
  }

  /* We start pushing 9 frames earlier (29 frames for MPEG2) than
   * segment start to be able to decode the first frame we want.
   * 9 (29) frames are the theoretical maximum of frames that contain
   * data for the current frame (bit reservoir).
   */

  if (mp3parse->segment.start == 0) {
    push_start = 0;
  } else if (GST_CLOCK_TIME_IS_VALID (mp3parse->max_bitreservoir)) {
    if (mp3parse->segment.start > mp3parse->max_bitreservoir)
      push_start = mp3parse->segment.start - mp3parse->max_bitreservoir;
    else
      push_start = 0;
  } else {
    push_start = mp3parse->segment.start;
  }

  if (G_UNLIKELY ((GST_CLOCK_TIME_IS_VALID (push_start) &&
              GST_BUFFER_TIMESTAMP (outbuf) + GST_BUFFER_DURATION (outbuf)
              < push_start)
          || (GST_CLOCK_TIME_IS_VALID (mp3parse->segment.stop)
              && GST_BUFFER_TIMESTAMP (outbuf) >= mp3parse->segment.stop))) {
    GST_DEBUG_OBJECT (mp3parse,
        "Buffer outside of configured segment range %" GST_TIME_FORMAT
        " to %" GST_TIME_FORMAT ", dropping, timestamp %"
        GST_TIME_FORMAT ", offset 0x%08" G_GINT64_MODIFIER "x",
        GST_TIME_ARGS (push_start), GST_TIME_ARGS (mp3parse->segment.stop),
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
        GST_BUFFER_OFFSET (outbuf));
    gst_buffer_unref (outbuf);
    ret = GST_FLOW_OK;
  } else {
    GST_DEBUG_OBJECT (mp3parse,
        "pushing buffer of %d bytes, timestamp %" GST_TIME_FORMAT
        ", offset 0x%08" G_GINT64_MODIFIER "x", size,
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
        GST_BUFFER_OFFSET (outbuf));
    mp3parse->segment.last_stop = GST_BUFFER_TIMESTAMP (outbuf);
    /* push any pending segment now */
    if (mp3parse->pending_segment) {
      gst_pad_push_event (mp3parse->srcpad, mp3parse->pending_segment);
      mp3parse->pending_segment = NULL;
    }
    ret = gst_pad_push (mp3parse->srcpad, outbuf);
  }

  return ret;
}

#define XING_FRAMES_FLAG     0x0001
#define XING_BYTES_FLAG      0x0002
#define XING_TOC_FLAG        0x0004
#define XING_VBR_SCALE_FLAG  0x0008

static void
gst_mp3parse_handle_first_frame (GstMPEGAudioParse * mp3parse)
{
  GstTagList *taglist;
  gchar *codec;
  const guint32 xing_id = 0x58696e67;   /* 'Xing' in hex */
  const guint32 info_id = 0x496e666f;   /* 'Info' in hex - found in LAME CBR files */
  const guint XING_HDR_MIN = 8;
  gint xing_offset;

  guint64 avail;
  guint32 read_id;
  const guint8 *data;

  /* Output codec tag */
  if (!mp3parse->sent_codec_tag) {
    if (mp3parse->layer == 3) {
      codec = g_strdup_printf ("MPEG %d Audio, Layer %d (MP3)",
          mp3parse->version, mp3parse->layer);
    } else {
      codec = g_strdup_printf ("MPEG %d Audio, Layer %d",
          mp3parse->version, mp3parse->layer);
    }

    taglist = gst_tag_list_new ();
    gst_tag_list_add (taglist, GST_TAG_MERGE_REPLACE,
        GST_TAG_AUDIO_CODEC, codec, NULL);
    gst_element_found_tags_for_pad (GST_ELEMENT (mp3parse),
        mp3parse->srcpad, taglist);
    g_free (codec);

    mp3parse->sent_codec_tag = TRUE;
  }
  /* end setting the tag */

  /* Check first frame for Xing info */
  if (mp3parse->version == 1) { /* MPEG-1 file */
    if (mp3parse->channels == 1)
      xing_offset = 0x11;
    else
      xing_offset = 0x20;
  } else {                      /* MPEG-2 header */
    if (mp3parse->channels == 1)
      xing_offset = 0x09;
    else
      xing_offset = 0x11;
  }
  /* Skip the 4 bytes of the MP3 header too */
  xing_offset += 4;

  /* Check if we have enough data to read the Xing header */
  avail = gst_adapter_available (mp3parse->adapter);

  if (avail < xing_offset + XING_HDR_MIN)
    return;

  data = gst_adapter_peek (mp3parse->adapter, xing_offset + XING_HDR_MIN);
  if (data == NULL)
    return;
  /* The header starts at the provided offset */
  data += xing_offset;

  read_id = GST_READ_UINT32_BE (data);
  if (read_id == xing_id || read_id == info_id) {
    guint32 xing_flags;
    guint bytes_needed = xing_offset + XING_HDR_MIN;

    GST_DEBUG_OBJECT (mp3parse, "Found Xing header marker 0x%x", xing_id);

    /* Read 4 base bytes of flags, big-endian */
    xing_flags = GST_READ_UINT32_BE (data + 4);
    if (xing_flags & XING_FRAMES_FLAG)
      bytes_needed += 4;
    if (xing_flags & XING_BYTES_FLAG)
      bytes_needed += 4;
    if (xing_flags & XING_TOC_FLAG)
      bytes_needed += 100;
    if (xing_flags & XING_VBR_SCALE_FLAG)
      bytes_needed += 4;
    if (avail < bytes_needed) {
      GST_DEBUG_OBJECT (mp3parse,
          "Not enough data to read Xing header (need %d)", bytes_needed);
      return;
    }

    GST_DEBUG_OBJECT (mp3parse, "Reading Xing header");
    mp3parse->xing_flags = xing_flags;
    data = gst_adapter_peek (mp3parse->adapter, bytes_needed);
    data += xing_offset + XING_HDR_MIN;

    if (xing_flags & XING_FRAMES_FLAG) {
      gint64 total_bytes;

      mp3parse->xing_frames = GST_READ_UINT32_BE (data);
      mp3parse->xing_total_time = gst_util_uint64_scale (GST_SECOND,
          (guint64) (mp3parse->xing_frames) * (mp3parse->spf), mp3parse->rate);

      /* We know the total time. If we also know the upstream size, compute the 
       * total bitrate, rounded up to the nearest kbit/sec */
      if (mp3parse_total_bytes (mp3parse, &total_bytes)) {
        mp3parse->xing_bitrate = gst_util_uint64_scale (total_bytes,
            8 * GST_SECOND, mp3parse->xing_total_time);
        mp3parse->xing_bitrate += 500;
        mp3parse->xing_bitrate -= mp3parse->xing_bitrate % 1000;
      }

      data += 4;
    } else {
      mp3parse->xing_frames = 0;
      mp3parse->xing_total_time = 0;
    }

    if (xing_flags & XING_BYTES_FLAG) {
      mp3parse->xing_bytes = GST_READ_UINT32_BE (data);
      data += 4;
    } else
      mp3parse->xing_bytes = 0;

    if (xing_flags & XING_TOC_FLAG) {
      int i, percent = 0;
      guchar *table = mp3parse->xing_seek_table;

      /* xing seek table: percent time -> 1/256 bytepos */
      memcpy (mp3parse->xing_seek_table, data, 100);

      /* build inverse table: 1/256 bytepos -> 1/100 percent time */
      for (i = 0; i < 256; i++) {
        while (percent < 99 && table[percent + 1] <= i)
          percent++;

        if (table[percent] == i) {
          mp3parse->xing_seek_table_inverse[i] = percent * 100;
        } else if (table[percent] < i && percent < 99) {
          gdouble fa, fb, fx;
          gint a = percent, b = percent + 1;

          fa = table[a];
          fb = table[b];
          fx = (b - a) / (fb - fa) * (i - fa) + a;
          mp3parse->xing_seek_table_inverse[i] = (guint16) (fx * 100);
        } else if (percent == 98 && table[percent + 1] <= i) {
          gdouble fa, fb, fx;
          gint a = percent + 1, b = 100;

          fa = table[a];
          fb = 256.0;
          fx = (b - a) / (fb - fa) * (i - fa) + a;
          mp3parse->xing_seek_table_inverse[i] = (guint16) (fx * 100);
        }
      }
      data += 100;
    } else {
      memset (mp3parse->xing_seek_table, 0, 100);
      memset (mp3parse->xing_seek_table_inverse, 0, 256);
    }

    if (xing_flags & XING_VBR_SCALE_FLAG) {
      mp3parse->xing_vbr_scale = GST_READ_UINT32_BE (data);
      data += 4;
    } else
      mp3parse->xing_vbr_scale = 0;

    GST_DEBUG_OBJECT (mp3parse, "Xing header reported %u frames, time %"
        G_GUINT64_FORMAT ", vbr scale %u", mp3parse->xing_frames,
        mp3parse->xing_total_time, mp3parse->xing_vbr_scale);
  } else {
    GST_DEBUG_OBJECT (mp3parse, "Xing header not found in first frame");
  }
}

static GstFlowReturn
gst_mp3parse_chain (GstPad * pad, GstBuffer * buf)
{
  GstFlowReturn flow = GST_FLOW_OK;
  GstMPEGAudioParse *mp3parse;
  const guchar *data;
  guint32 header;
  int bpf;
  guint available;
  GstClockTime timestamp;

  mp3parse = GST_MP3PARSE (GST_PAD_PARENT (pad));

  GST_LOG_OBJECT (mp3parse, "buffer of %d bytes", GST_BUFFER_SIZE (buf));

  timestamp = GST_BUFFER_TIMESTAMP (buf);

  /* If we don't yet have a next timestamp, save it and the incoming offset
   * so we can apply it to the right outgoing buffer */
  if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
    gint64 avail = gst_adapter_available (mp3parse->adapter);

    mp3parse->pending_ts = timestamp;
    mp3parse->pending_offset = mp3parse->tracked_offset + avail;

    GST_LOG_OBJECT (mp3parse, "Have pending ts %" GST_TIME_FORMAT
        " to apply in %lld bytes (@ off %lld)\n",
        GST_TIME_ARGS (mp3parse->pending_ts), avail, mp3parse->pending_offset);
  }

  /* Update the cur_offset we'll apply to outgoing buffers */
  if (mp3parse->cur_offset == -1 && GST_BUFFER_OFFSET (buf) != -1)
    mp3parse->cur_offset = GST_BUFFER_OFFSET (buf);

  /* And add the data to the pool */
  gst_adapter_push (mp3parse->adapter, buf);

  /* while we still have at least 4 bytes (for the header) available */
  while (gst_adapter_available (mp3parse->adapter) >= 4) {
    /* search for a possible start byte */
    data = gst_adapter_peek (mp3parse->adapter, 4);
    if (*data != 0xff) {
      /* It'd be nice to make this efficient, but it's ok for now; this is only
       * when resyncing */
      mp3parse->resyncing = TRUE;
      gst_adapter_flush (mp3parse->adapter, 1);
      if (mp3parse->cur_offset != -1)
        mp3parse->cur_offset++;
      mp3parse->tracked_offset++;
      continue;
    }

    available = gst_adapter_available (mp3parse->adapter);

    /* construct the header word */
    header = GST_READ_UINT32_BE (data);
    /* if it's a valid header, go ahead and send off the frame */
    if (head_check (mp3parse, header)) {
      guint bitrate = 0, layer = 0, rate = 0, channels = 0, version = 0;

      if (!(bpf = mp3_type_frame_length_from_header (mp3parse, header,
                  &version, &layer, &channels, &bitrate, &rate)))
        goto header_error;

      /*************************************************************************
      * robust seek support
      * - This performs additional frame validation if the resyncing flag is set
      *   (indicating a discontinuous stream).
      * - The current frame header is not accepted as valid unless the NEXT 
      *   frame header has the same values for most fields.  This significantly
      *   increases the probability that we aren't processing random data.
      * - It is not clear if this is sufficient for robust seeking of Layer III
      *   streams which utilize the concept of a "bit reservoir" by borrowing
      *   bitrate from previous frames.  In this case, seeking may be more 
      *   complicated because the frames are not independently coded.
      *************************************************************************/
      if (mp3parse->resyncing) {
        guint32 header2;
        const guint8 *data2;

        /* wait until we have the the entire current frame as well as the next 
         * frame header */
        if (available < bpf + 4)
          break;

        data2 = gst_adapter_peek (mp3parse->adapter, bpf + 4);
        header2 = GST_READ_UINT32_BE (data2 + bpf);
        GST_DEBUG_OBJECT (mp3parse, "header=%08X, header2=%08X, bpf=%d",
            (unsigned int) header, (unsigned int) header2, bpf);

/* mask the bits which are allowed to differ between frames */
#define HDRMASK ~((0xF << 12)  /* bitrate */ | \
                  (0x1 <<  9)  /* padding */ | \
                  (0x3 <<  4))  /* mode extension */

        /* require 2 matching headers in a row */
        if ((header2 & HDRMASK) != (header & HDRMASK)) {
          GST_DEBUG_OBJECT (mp3parse, "next header doesn't match "
              "(header=%08X, header2=%08X, bpf=%d)",
              (unsigned int) header, (unsigned int) header2, bpf);
          /* This frame is invalid.  Start looking for a valid frame at the 
           * next position in the stream */
          mp3parse->resyncing = TRUE;
          gst_adapter_flush (mp3parse->adapter, 1);
          if (mp3parse->cur_offset != -1)
            mp3parse->cur_offset++;
          mp3parse->tracked_offset++;
          continue;
        }
      }

      /* if we don't have the whole frame... */
      if (available < bpf) {
        GST_DEBUG_OBJECT (mp3parse, "insufficient data available, need "
            "%d bytes, have %d", bpf, available);
        break;
      }
      if (channels != mp3parse->channels ||
          rate != mp3parse->rate ||
          layer != mp3parse->layer || bitrate != mp3parse->bit_rate) {
        GstCaps *caps;

        caps = mp3_caps_create (layer, channels, bitrate, rate);
        gst_pad_set_caps (mp3parse->srcpad, caps);
        gst_caps_unref (caps);

        mp3parse->channels = channels;
        mp3parse->layer = layer;
        mp3parse->rate = rate;
        mp3parse->bit_rate = bitrate;
        mp3parse->version = version;

        /* see http://www.codeproject.com/audio/MPEGAudioInfo.asp */
        if (mp3parse->layer == 1)
          mp3parse->spf = 384;
        else if (mp3parse->layer == 2)
          mp3parse->spf = 1152;
        else if (mp3parse->version == 2) {
          mp3parse->spf = 576;
        } else
          mp3parse->spf = 1152;
      }

      mp3parse->max_bitreservoir = gst_util_uint64_scale (GST_SECOND,
          ((version == 1) ? 10 : 30) * mp3parse->spf, mp3parse->rate);

      /* Check the first frame for a Xing header to get our total length */
      if (mp3parse->frame_count == 0) {
        /* For the first frame in the file, look for a Xing frame after 
         * the header, and output a codec tag */
        gst_mp3parse_handle_first_frame (mp3parse);
      }

      /* Update VBR stats */
      mp3parse->bitrate_sum += mp3parse->bit_rate;
      mp3parse->frame_count++;
      /* Compute the average bitrate, rounded up to the nearest 1000 bits */
      mp3parse->avg_bitrate =
          (mp3parse->bitrate_sum / mp3parse->frame_count + 500);
      mp3parse->avg_bitrate -= mp3parse->avg_bitrate % 1000;

      if (!mp3parse->skip) {
        mp3parse->resyncing = FALSE;
        flow = gst_mp3parse_emit_frame (mp3parse, bpf);
      } else {
        GST_DEBUG_OBJECT (mp3parse, "skipping buffer of %d bytes", bpf);
        gst_adapter_flush (mp3parse->adapter, bpf);
        if (mp3parse->cur_offset != -1)
          mp3parse->cur_offset += bpf;
        mp3parse->tracked_offset += bpf;
        mp3parse->skip--;
      }
    } else {
      mp3parse->resyncing = TRUE;
      gst_adapter_flush (mp3parse->adapter, 1);
      if (mp3parse->cur_offset != -1)
        mp3parse->cur_offset++;
      mp3parse->tracked_offset++;
      GST_DEBUG_OBJECT (mp3parse, "wrong header, skipping byte");
    }

    if (GST_FLOW_IS_FATAL (flow))
      break;
  }

  return flow;

header_error:
  GST_ELEMENT_ERROR (mp3parse, STREAM, DECODE,
      ("Invalid MP3 header found"), (NULL));
  return GST_FLOW_ERROR;
}

static gboolean
head_check (GstMPEGAudioParse * mp3parse, unsigned long head)
{
  GST_DEBUG_OBJECT (mp3parse, "checking mp3 header 0x%08lx", head);
  /* if it's not a valid sync */
  if ((head & 0xffe00000) != 0xffe00000) {
    GST_DEBUG_OBJECT (mp3parse, "invalid sync");
    return FALSE;
  }
  /* if it's an invalid MPEG version */
  if (((head >> 19) & 3) == 0x1) {
    GST_DEBUG_OBJECT (mp3parse, "invalid MPEG version");
    return FALSE;
  }
  /* if it's an invalid layer */
  if (!((head >> 17) & 3)) {
    GST_DEBUG_OBJECT (mp3parse, "invalid layer");
    return FALSE;
  }
  /* if it's an invalid bitrate */
  if (((head >> 12) & 0xf) == 0x0) {
    GST_DEBUG_OBJECT (mp3parse, "invalid bitrate");
    return FALSE;
  }
  if (((head >> 12) & 0xf) == 0xf) {
    GST_DEBUG_OBJECT (mp3parse, "invalid bitrate");
    return FALSE;
  }
  /* if it's an invalid samplerate */
  if (((head >> 10) & 0x3) == 0x3) {
    GST_DEBUG_OBJECT (mp3parse, "invalid samplerate");
    return FALSE;
  }
  if ((head & 0xffff0000) == 0xfffe0000) {
    GST_DEBUG_OBJECT (mp3parse, "invalid sync");
    return FALSE;
  }
  if (head & 0x00000002) {
    GST_DEBUG_OBJECT (mp3parse, "invalid emphasis");
    return FALSE;
  }

  return TRUE;
}

static void
gst_mp3parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMPEGAudioParse *src;

  g_return_if_fail (GST_IS_MP3PARSE (object));
  src = GST_MP3PARSE (object);

  switch (prop_id) {
    case ARG_SKIP:
      src->skip = g_value_get_int (value);
      break;
    default:
      break;
  }
}

static void
gst_mp3parse_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMPEGAudioParse *src;

  g_return_if_fail (GST_IS_MP3PARSE (object));
  src = GST_MP3PARSE (object);

  switch (prop_id) {
    case ARG_SKIP:
      g_value_set_int (value, src->skip);
      break;
    case ARG_BIT_RATE:
      g_value_set_int (value, src->bit_rate * 1000);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_mp3parse_change_state (GstElement * element, GstStateChange transition)
{
  GstMPEGAudioParse *mp3parse;
  GstStateChangeReturn result;

  mp3parse = GST_MP3PARSE (element);

  result = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_mp3parse_reset (mp3parse);
      break;
    default:
      break;
  }

  return result;
}

static gboolean
mp3parse_total_bytes (GstMPEGAudioParse * mp3parse, gint64 * total)
{
  GstFormat fmt = GST_FORMAT_BYTES;

  if (gst_pad_query_peer_duration (mp3parse->sinkpad, &fmt, total))
    return TRUE;

  if (mp3parse->xing_flags & XING_BYTES_FLAG) {
    *total = mp3parse->xing_bytes;
    return TRUE;
  }

  return FALSE;
}

static gboolean
mp3parse_total_time (GstMPEGAudioParse * mp3parse, GstClockTime * total)
{
  gint64 total_bytes;

  *total = GST_CLOCK_TIME_NONE;

  if (mp3parse->xing_flags & XING_FRAMES_FLAG) {
    *total = mp3parse->xing_total_time;
    return TRUE;
  }

  /* Calculate time from the measured bitrate */
  if (!mp3parse_total_bytes (mp3parse, &total_bytes))
    return FALSE;

  if (total_bytes != -1
      && !mp3parse_bytepos_to_time (mp3parse, total_bytes, total))
    return FALSE;

  return TRUE;
}

/* Convert a timestamp to the file position required to start decoding that
 * timestamp. For now, this just uses the avg bitrate. Later, use an 
 * incrementally accumulated seek table */
static gboolean
mp3parse_time_to_bytepos (GstMPEGAudioParse * mp3parse, GstClockTime ts,
    gint64 * bytepos)
{
  gint64 total_bytes;
  GstClockTime total_time;

  /* -1 always maps to -1 */
  if (ts == -1) {
    *bytepos = -1;
    return TRUE;
  }

  /* If XING seek table exists use this for time->byte conversion */
  if ((mp3parse->xing_flags & XING_TOC_FLAG) &&
      mp3parse_total_bytes (mp3parse, &total_bytes) &&
      mp3parse_total_time (mp3parse, &total_time)) {
    gdouble fa, fb, fx;
    gdouble percent =
        CLAMP ((100.0 * gst_util_guint64_to_gdouble (ts)) /
        gst_util_guint64_to_gdouble (total_time), 0.0, 100.0);
    gint index = CLAMP (percent, 0, 99);

    fa = mp3parse->xing_seek_table[index];
    if (index < 99)
      fb = mp3parse->xing_seek_table[index + 1];
    else
      fb = 256.0;

    fx = fa + (fb - fa) * (percent - index);

    *bytepos = (1.0 / 256.0) * fx * total_bytes;

    return TRUE;
  }

  if (mp3parse->avg_bitrate == 0)
    goto no_bitrate;

  *bytepos =
      gst_util_uint64_scale (ts, mp3parse->avg_bitrate, (8 * GST_SECOND));
  return TRUE;
no_bitrate:
  GST_DEBUG_OBJECT (mp3parse, "Cannot seek yet - no average bitrate");
  return FALSE;
}

static gboolean
mp3parse_bytepos_to_time (GstMPEGAudioParse * mp3parse,
    gint64 bytepos, GstClockTime * ts)
{
  gint64 total_bytes;
  GstClockTime total_time;

  if (bytepos == -1) {
    *ts = GST_CLOCK_TIME_NONE;
    return TRUE;
  }

  if (bytepos == 0) {
    *ts = 0;
    return TRUE;
  }

  /* If XING seek table exists use this for byte->time conversion */
  if ((mp3parse->xing_flags & XING_TOC_FLAG) &&
      mp3parse_total_bytes (mp3parse, &total_bytes) &&
      mp3parse_total_time (mp3parse, &total_time)) {
    gdouble fa, fb, fx;
    gdouble pos = CLAMP ((bytepos * 256.0) / total_bytes, 0.0, 256.0);
    gint index = CLAMP (pos, 0, 255);

    fa = mp3parse->xing_seek_table_inverse[index];
    if (index < 255)
      fb = mp3parse->xing_seek_table_inverse[index + 1];
    else
      fb = 10000.0;

    fx = fa + (fb - fa) * (pos - index);

    *ts = (1.0 / 10000.0) * fx * gst_util_guint64_to_gdouble (total_time);

    return TRUE;
  }

  /* Cannot convert anything except 0 if we don't have a bitrate yet */
  if (mp3parse->avg_bitrate == 0)
    return FALSE;

  *ts = (GstClockTime) gst_util_uint64_scale (GST_SECOND, bytepos * 8,
      mp3parse->avg_bitrate);
  return TRUE;
}

static gboolean
mp3parse_handle_seek (GstMPEGAudioParse * mp3parse, GstEvent * event)
{
  GstFormat format;
  gdouble rate;
  GstSeekFlags flags;
  GstSeekType cur_type, stop_type;
  gint64 cur, stop;
  gint64 byte_cur, byte_stop;

  gst_event_parse_seek (event, &rate, &format, &flags, &cur_type, &cur,
      &stop_type, &stop);

  GST_DEBUG_OBJECT (mp3parse, "Performing seek to %" GST_TIME_FORMAT,
      GST_TIME_ARGS (cur));

  /* For any format other than TIME, see if upstream handles
   * it directly or fail. For TIME, try upstream, but do it ourselves if
   * it fails upstream */
  if (format != GST_FORMAT_TIME) {
    gst_event_ref (event);
    return gst_pad_push_event (mp3parse->sinkpad, event);
  } else {
    gst_event_ref (event);
    if (gst_pad_push_event (mp3parse->sinkpad, event))
      return TRUE;
  }

  /* Handle TIME based seeks by converting to a BYTE position */

  /* For accurate seeking get the frame 9 (MPEG1) or 29 (MPEG2) frames
   * before the one we want to seek to and push them all to the decoder.
   *
   * This is necessary because of the bit reservoir. See
   * http://www.mars.org/mailman/public/mad-dev/2002-May/000634.html
   *
   */

  if (flags & GST_SEEK_FLAG_ACCURATE) {
    MPEGAudioPendingAccurateSeek *seek =
        g_new0 (MPEGAudioPendingAccurateSeek, 1);
    GstClockTime start;

    seek->segment = mp3parse->segment;

    gst_segment_set_seek (&seek->segment, rate, GST_FORMAT_TIME,
        flags, cur_type, cur, stop_type, stop, NULL);

    if (!mp3parse->seek_table) {
      byte_cur = 0;
      byte_stop = -1;
      start = 0;
    } else {
      MPEGAudioSeekEntry *entry = NULL, *start_entry = NULL, *stop_entry = NULL;
      GList *start_node, *stop_node;

      for (start_node = mp3parse->seek_table; start_node;
          start_node = start_node->next) {
        entry = start_node->data;

        if (cur - mp3parse->max_bitreservoir >= entry->timestamp) {
          start_entry = entry;
          break;
        }
      }

      if (!start_entry) {
        start_entry = mp3parse->seek_table->data;
        start = start_entry->timestamp;
        byte_cur = start_entry->byte;
      } else {
        start = start_entry->timestamp;
        byte_cur = start_entry->byte;
      }

      for (stop_node = mp3parse->seek_table; stop_node;
          stop_node = stop_node->next) {
        entry = stop_node->data;

        if (stop >= entry->timestamp) {
          stop_node = stop_node->prev;
          stop_entry = (stop_node) ? stop_node->data : NULL;
          break;
        }
      }

      if (!stop_entry) {
        byte_stop = -1;
      } else {
        byte_stop = stop_entry->byte;
      }

    }
    g_mutex_lock (mp3parse->pending_accurate_seeks_lock);
    event = gst_event_new_seek (rate, GST_FORMAT_BYTES, flags, cur_type,
        byte_cur, stop_type, byte_stop);
    if (gst_pad_push_event (mp3parse->sinkpad, event)) {
      mp3parse->exact_position = TRUE;
      seek->upstream_start = byte_cur;
      seek->timestamp_start = start;
      mp3parse->pending_accurate_seeks =
          g_slist_prepend (mp3parse->pending_accurate_seeks, seek);
      g_mutex_unlock (mp3parse->pending_accurate_seeks_lock);
      return TRUE;
    } else {
      g_mutex_unlock (mp3parse->pending_accurate_seeks_lock);
      mp3parse->exact_position = TRUE;
      g_free (seek);
      return TRUE;
    }
  }

  mp3parse->exact_position = FALSE;

  /* Convert the TIME to the appropriate BYTE position at which to resume
   * decoding. */
  if (!mp3parse_time_to_bytepos (mp3parse, (GstClockTime) cur, &byte_cur))
    goto no_pos;
  if (!mp3parse_time_to_bytepos (mp3parse, (GstClockTime) stop, &byte_stop))
    goto no_pos;

  GST_DEBUG_OBJECT (mp3parse, "Seeking to byte range %" G_GINT64_FORMAT
      " to %" G_GINT64_FORMAT, byte_cur, byte_stop);

  /* Send BYTE based seek upstream */
  event = gst_event_new_seek (rate, GST_FORMAT_BYTES, flags, cur_type,
      byte_cur, stop_type, byte_stop);

  if (flags & GST_SEEK_FLAG_SEGMENT) {
    gst_element_post_message (GST_ELEMENT_CAST (mp3parse),
        gst_message_new_segment_start (GST_OBJECT_CAST (mp3parse),
            GST_FORMAT_TIME, cur));
  }
  return gst_pad_push_event (mp3parse->sinkpad, event);
no_pos:
  GST_DEBUG_OBJECT (mp3parse,
      "Could not determine byte position for desired time");
  return FALSE;
}

static gboolean
mp3parse_src_event (GstPad * pad, GstEvent * event)
{
  GstMPEGAudioParse *mp3parse = GST_MP3PARSE (gst_pad_get_parent (pad));
  gboolean res = FALSE;

  g_return_val_if_fail (mp3parse != NULL, FALSE);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      res = mp3parse_handle_seek (mp3parse, event);
      gst_event_unref (event);
      break;
    default:
      res = gst_pad_event_default (pad, event);
      break;
  }

  gst_object_unref (mp3parse);
  return res;
}

static gboolean
mp3parse_src_query (GstPad * pad, GstQuery * query)
{
  GstFormat format;
  GstClockTime total;
  GstMPEGAudioParse *mp3parse = GST_MP3PARSE (gst_pad_get_parent (pad));
  gboolean res = FALSE;
  GstPad *peer;

  g_return_val_if_fail (mp3parse != NULL, FALSE);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
      gst_query_parse_position (query, &format, NULL);

      if (format == GST_FORMAT_BYTES || format == GST_FORMAT_DEFAULT) {
        if (mp3parse->cur_offset != -1) {
          gst_query_set_position (query, GST_FORMAT_BYTES,
              mp3parse->cur_offset);
          res = TRUE;
        }
      } else if (format == GST_FORMAT_TIME) {
        if (mp3parse->next_ts == GST_CLOCK_TIME_NONE)
          goto out;
        gst_query_set_position (query, GST_FORMAT_TIME, mp3parse->next_ts);
        res = TRUE;
      }

      /* If no answer above, see if upstream knows */
      if (!res) {
        if ((peer = gst_pad_get_peer (mp3parse->sinkpad)) != NULL) {
          res = gst_pad_query (peer, query);
          gst_object_unref (peer);
          if (res)
            goto out;
        }
      }
      break;
    case GST_QUERY_DURATION:
      gst_query_parse_duration (query, &format, NULL);

      /* First, see if upstream knows */
      if ((peer = gst_pad_get_peer (mp3parse->sinkpad)) != NULL) {
        res = gst_pad_query (peer, query);
        gst_object_unref (peer);
        if (res)
          goto out;
      }

      if (format == GST_FORMAT_TIME) {
        if (!mp3parse_total_time (mp3parse, &total) || total == -1)
          goto out;
        gst_query_set_duration (query, format, total);
        res = TRUE;
      }
      break;
    default:
      res = gst_pad_query_default (pad, query);
      break;
  }

out:
  gst_object_unref (mp3parse);
  return res;
}

static const GstQueryType *
mp3parse_get_query_types (GstPad * pad ATTR_UNUSED)
{
  static const GstQueryType query_types[] = {
    GST_QUERY_POSITION,
    GST_QUERY_DURATION,
    0
  };

  return query_types;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (mp3parse_debug, "mp3parse", 0, "MP3 Parser");

  return gst_element_register (plugin, "mp3parse",
      GST_RANK_PRIMARY + 1, GST_TYPE_MP3PARSE);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "mpegaudioparse",
    "MPEG-1 layer 1/2/3 audio parser",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
