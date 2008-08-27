/* GStreamer
 * Copyright (C) <2006> Lutz Mueller <lutz at topfrose dot de>
 *               <2006> Wim Taymans <wim@fluendo.com>
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
#  include "config.h"
#endif

#include <string.h>

#include "gstrdtbuffer.h"
#include "rdtdepay.h"

GST_DEBUG_CATEGORY_STATIC (rdtdepay_debug);
#define GST_CAT_DEFAULT rdtdepay_debug

/* elementfactory information */
static const GstElementDetails gst_rdtdepay_details =
GST_ELEMENT_DETAILS ("RDT packet parser",
    "Codec/Depayloader/Network",
    "Extracts RealMedia from RDT packets",
    "Lutz Mueller <lutz at topfrose dot de>, " "Wim Taymans <wim@fluendo.com>");

/* RDTDepay signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
};

static GstStaticPadTemplate gst_rdt_depay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/vnd.rn-realmedia")
    );

static GstStaticPadTemplate gst_rdt_depay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rdt, "
        "media = (string) \"application\", "
        "clock-rate = (int) [1, MAX ], "
        "encoding-name = (string) \"X-REAL-RDT\""
        /* All optional parameters
         *
         * "config=" 
         */
    )
    );

GST_BOILERPLATE (GstRDTDepay, gst_rdt_depay, GstElement, GST_TYPE_ELEMENT);

static void gst_rdt_depay_finalize (GObject * object);

static void gst_rdt_depay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rdt_depay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_rdt_depay_change_state (GstElement *
    element, GstStateChange transition);

static gboolean gst_rdt_depay_setcaps (GstPad * pad, GstCaps * caps);
static GstFlowReturn gst_rdt_depay_chain (GstPad * pad, GstBuffer * buf);

static void
gst_rdt_depay_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_rdt_depay_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_rdt_depay_sink_template));

  gst_element_class_set_details (element_class, &gst_rdtdepay_details);

  GST_DEBUG_CATEGORY_INIT (rdtdepay_debug, "rdtdepay",
      0, "Depayloader for RDT RealMedia packets");
}

static void
gst_rdt_depay_class_init (GstRDTDepayClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = gst_rdt_depay_set_property;
  gobject_class->get_property = gst_rdt_depay_get_property;

  gobject_class->finalize = gst_rdt_depay_finalize;

  gstelement_class->change_state = gst_rdt_depay_change_state;
}

static void
gst_rdt_depay_init (GstRDTDepay * rdtdepay, GstRDTDepayClass * klass)
{
  rdtdepay->sinkpad =
      gst_pad_new_from_static_template (&gst_rdt_depay_sink_template, "sink");
  gst_pad_set_chain_function (rdtdepay->sinkpad, gst_rdt_depay_chain);
  gst_pad_set_setcaps_function (rdtdepay->sinkpad, gst_rdt_depay_setcaps);
  gst_element_add_pad (GST_ELEMENT_CAST (rdtdepay), rdtdepay->sinkpad);

  rdtdepay->srcpad =
      gst_pad_new_from_static_template (&gst_rdt_depay_src_template, "src");
  gst_element_add_pad (GST_ELEMENT_CAST (rdtdepay), rdtdepay->srcpad);
}

static void
gst_rdt_depay_finalize (GObject * object)
{
  GstRDTDepay *rdtdepay;

  rdtdepay = GST_RDT_DEPAY (object);

  if (rdtdepay->header)
    gst_buffer_unref (rdtdepay->header);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_rdt_depay_setcaps (GstPad * pad, GstCaps * caps)
{
  GstStructure *structure;
  GstRDTDepay *rdtdepay;
  GstCaps *srccaps;
  gint clock_rate = 1000;       /* default */
  const GValue *config;
  GstBuffer *header;

  rdtdepay = GST_RDT_DEPAY (GST_PAD_PARENT (pad));

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_field (structure, "clock-rate"))
    gst_structure_get_int (structure, "clock-rate", &clock_rate);

  /* config contains the RealMedia header as a buffer. */
  config = gst_structure_get_value (structure, "config");
  if (!config)
    goto no_header;

  header = gst_value_get_buffer (config);
  if (!header)
    goto no_header;

  /* caps seem good, configure element */
  rdtdepay->clock_rate = clock_rate;

  /* set caps on pad and on header */
  srccaps = gst_caps_new_simple ("application/vnd.rn-realmedia", NULL);
  gst_pad_set_caps (rdtdepay->srcpad, srccaps);
  gst_caps_unref (srccaps);

  if (rdtdepay->header)
    gst_buffer_unref (rdtdepay->header);
  rdtdepay->header = gst_buffer_ref (header);

  return TRUE;

  /* ERRORS */
no_header:
  {
    GST_ERROR_OBJECT (rdtdepay, "no header found in caps, no 'config' field");
    return FALSE;
  }
}

static GstFlowReturn
gst_rdt_depay_push (GstRDTDepay * rdtdepay, GstBuffer * buffer)
{
  GstFlowReturn ret;

  gst_buffer_set_caps (buffer, GST_PAD_CAPS (rdtdepay->srcpad));

  if (rdtdepay->discont) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
    rdtdepay->discont = FALSE;
  }
  ret = gst_pad_push (rdtdepay->srcpad, buffer);

  return ret;
}

static GstFlowReturn
gst_rdt_depay_handle_data (GstRDTDepay * rdtdepay, GstClockTime outtime,
    GstRDTPacket * packet)
{
  GstFlowReturn ret;
  GstBuffer *outbuf;
  guint8 *data, *outdata;
  guint size;
  guint16 stream_id;
  guint32 timestamp;

  /* get pointers to the packet data */
  gst_rdt_packet_data_peek_data (packet, &data, &size);

  outbuf = gst_buffer_new_and_alloc (12 + size);
  outdata = GST_BUFFER_DATA (outbuf);
  GST_BUFFER_TIMESTAMP (outbuf) = outtime;

  GST_DEBUG_OBJECT (rdtdepay, "have size %u", size);

  /* copy over some things */
  stream_id = gst_rdt_packet_data_get_stream_id (packet);
  timestamp = gst_rdt_packet_data_get_timestamp (packet);

  GST_WRITE_UINT16_BE (outdata + 0, 0); /* version   */
  GST_WRITE_UINT16_BE (outdata + 2, size + 12); /* length    */
  GST_WRITE_UINT16_BE (outdata + 4, stream_id); /* stream    */
  GST_WRITE_UINT32_BE (outdata + 6, timestamp); /* timestamp */
  GST_WRITE_UINT16_BE (outdata + 10, 0);        /* flags     */
  memcpy (outdata + 12, data, size);

  GST_DEBUG_OBJECT (rdtdepay, "Passing on packet "
      "stream_id=%u timestamp=%u, outtime %" GST_TIME_FORMAT, stream_id,
      timestamp, GST_TIME_ARGS (outtime));

  ret = gst_rdt_depay_push (rdtdepay, outbuf);

  return ret;
}

static GstFlowReturn
gst_rdt_depay_chain (GstPad * pad, GstBuffer * buf)
{
  GstRDTDepay *rdtdepay;
  GstFlowReturn ret;
  GstClockTime timestamp;
  gboolean more;
  GstRDTPacket packet;

  rdtdepay = GST_RDT_DEPAY (GST_PAD_PARENT (pad));

  if (GST_BUFFER_IS_DISCONT (buf)) {
    GST_LOG_OBJECT (rdtdepay, "received discont");
    rdtdepay->discont = TRUE;
  }

  if (rdtdepay->header) {
    GstBuffer *out;

    out = rdtdepay->header;
    rdtdepay->header = NULL;

    /* push header data first */
    ret = gst_rdt_depay_push (rdtdepay, out);
  }

  /* save timestamp */
  timestamp = GST_BUFFER_TIMESTAMP (buf);

  ret = GST_FLOW_OK;

  GST_LOG_OBJECT (rdtdepay, "received buffer timestamp %" GST_TIME_FORMAT,
      GST_TIME_ARGS (timestamp));

  /* data is in RDT format. */
  more = gst_rdt_buffer_get_first_packet (buf, &packet);
  while (more) {
    GstRDTType type;

    type = gst_rdt_packet_get_type (&packet);
    GST_DEBUG_OBJECT (rdtdepay, "Have packet of type %04x", type);

    if (GST_RDT_IS_DATA_TYPE (type)) {
      GST_DEBUG_OBJECT (rdtdepay, "We have a data packet");
      ret = gst_rdt_depay_handle_data (rdtdepay, timestamp, &packet);
    } else {
      switch (type) {
        default:
          GST_DEBUG_OBJECT (rdtdepay, "Ignoring packet");
          break;
      }
    }
    if (ret != GST_FLOW_OK)
      break;

    more = gst_rdt_packet_move_to_next (&packet);
  }

  return ret;
}

static void
gst_rdt_depay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRDTDepay *rdtdepay;

  rdtdepay = GST_RDT_DEPAY (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rdt_depay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRDTDepay *rdtdepay;

  rdtdepay = GST_RDT_DEPAY (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_rdt_depay_change_state (GstElement * element, GstStateChange transition)
{
  GstRDTDepay *rdtdepay;
  GstStateChangeReturn ret;

  rdtdepay = GST_RDT_DEPAY (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (rdtdepay->header)
        gst_buffer_unref (rdtdepay->header);
      rdtdepay->header = NULL;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }
  return ret;
}

gboolean
gst_rdt_depay_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "rdtdepay",
      GST_RANK_MARGINAL, GST_TYPE_RDT_DEPAY);
}
