/*
 * Copyright (C) 2008 Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>
 * Copyright (C) 2018 Centricular Ltd.
 *   Author: Nirbheek Chauhan <nirbheek@centricular.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-wasapisrc
 * @title: wasapisrc
 *
 * Provides audio capture from the Windows Audio Session API available with
 * Vista and newer.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 -v wasapisrc ! fakesink
 * ]| Capture from the default audio device and render to fakesink.
 *
 */
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstwasapisrc.h"

#include <mmdeviceapi.h>

GST_DEBUG_CATEGORY_STATIC (gst_wasapi_src_debug);
#define GST_CAT_DEFAULT gst_wasapi_src_debug

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_FORMATS_ALL ", "
        "layout = (string) interleaved, "
        "rate = " GST_AUDIO_RATE_RANGE ", channels = (int) [1, 2]"));

#define DEFAULT_ROLE    GST_WASAPI_DEVICE_ROLE_CONSOLE

enum
{
  PROP_0,
  PROP_ROLE,
  PROP_DEVICE
};

static void gst_wasapi_src_dispose (GObject * object);
static void gst_wasapi_src_finalize (GObject * object);
static void gst_wasapi_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_wasapi_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstCaps *gst_wasapi_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter);

static gboolean gst_wasapi_src_open (GstAudioSrc * asrc);
static gboolean gst_wasapi_src_close (GstAudioSrc * asrc);
static gboolean gst_wasapi_src_prepare (GstAudioSrc * asrc,
    GstAudioRingBufferSpec * spec);
static gboolean gst_wasapi_src_unprepare (GstAudioSrc * asrc);
static guint gst_wasapi_src_read (GstAudioSrc * asrc, gpointer data,
    guint length, GstClockTime * timestamp);
static guint gst_wasapi_src_delay (GstAudioSrc * asrc);
static void gst_wasapi_src_reset (GstAudioSrc * asrc);

static GstClockTime gst_wasapi_src_get_time (GstClock * clock,
    gpointer user_data);

#define gst_wasapi_src_parent_class parent_class
G_DEFINE_TYPE (GstWasapiSrc, gst_wasapi_src, GST_TYPE_AUDIO_SRC);

static void
gst_wasapi_src_class_init (GstWasapiSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstAudioSrcClass *gstaudiosrc_class = GST_AUDIO_SRC_CLASS (klass);

  gobject_class->dispose = gst_wasapi_src_dispose;
  gobject_class->finalize = gst_wasapi_src_finalize;
  gobject_class->set_property = gst_wasapi_src_set_property;
  gobject_class->get_property = gst_wasapi_src_get_property;

  g_object_class_install_property (gobject_class,
      PROP_ROLE,
      g_param_spec_enum ("role", "Role",
          "Role of the device: communications, multimedia, etc",
          GST_WASAPI_DEVICE_TYPE_ROLE, DEFAULT_ROLE, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_DEVICE,
      g_param_spec_string ("device", "Device",
          "WASAPI playback device as a GUID string",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (gstelement_class, &src_template);
  gst_element_class_set_static_metadata (gstelement_class, "WasapiSrc",
      "Source/Audio",
      "Stream audio from an audio capture device through WASAPI",
      "Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>");

  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_wasapi_src_get_caps);

  gstaudiosrc_class->open = GST_DEBUG_FUNCPTR (gst_wasapi_src_open);
  gstaudiosrc_class->close = GST_DEBUG_FUNCPTR (gst_wasapi_src_close);
  gstaudiosrc_class->read = GST_DEBUG_FUNCPTR (gst_wasapi_src_read);
  gstaudiosrc_class->prepare = GST_DEBUG_FUNCPTR (gst_wasapi_src_prepare);
  gstaudiosrc_class->unprepare = GST_DEBUG_FUNCPTR (gst_wasapi_src_unprepare);
  gstaudiosrc_class->delay = GST_DEBUG_FUNCPTR (gst_wasapi_src_delay);
  gstaudiosrc_class->reset = GST_DEBUG_FUNCPTR (gst_wasapi_src_reset);

  GST_DEBUG_CATEGORY_INIT (gst_wasapi_src_debug, "wasapisrc",
      0, "Windows audio session API source");
}

static void
gst_wasapi_src_init (GstWasapiSrc * self)
{
  /* override with a custom clock */
  if (GST_AUDIO_BASE_SRC (self)->clock)
    gst_object_unref (GST_AUDIO_BASE_SRC (self)->clock);

  GST_AUDIO_BASE_SRC (self)->clock = gst_audio_clock_new ("GstWasapiSrcClock",
      gst_wasapi_src_get_time, gst_object_ref (self),
      (GDestroyNotify) gst_object_unref);

  self->event_handle = CreateEvent (NULL, FALSE, FALSE, NULL);

  CoInitialize (NULL);
}

static void
gst_wasapi_src_dispose (GObject * object)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  if (self->event_handle != NULL) {
    CloseHandle (self->event_handle);
    self->event_handle = NULL;
  }

  if (self->client_clock != NULL) {
    IUnknown_Release (self->client_clock);
    self->client_clock = NULL;
  }

  if (self->client != NULL) {
    IUnknown_Release (self->client);
    self->client = NULL;
  }

  if (self->capture_client != NULL) {
    IUnknown_Release (self->capture_client);
    self->capture_client = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_wasapi_src_finalize (GObject * object)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  g_clear_pointer (&self->mix_format, CoTaskMemFree);

  CoUninitialize ();

  g_clear_pointer (&self->cached_caps, gst_caps_unref);
  g_clear_pointer (&self->device, g_free);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_wasapi_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  switch (prop_id) {
    case PROP_ROLE:
      self->role = gst_wasapi_device_role_to_erole (g_value_get_enum (value));
      break;
    case PROP_DEVICE:
    {
      const gchar *device = g_value_get_string (value);
      g_free (self->device);
      self->device =
          device ? g_utf8_to_utf16 (device, -1, NULL, NULL, NULL) : NULL;
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wasapi_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  switch (prop_id) {
    case PROP_ROLE:
      g_value_set_enum (value, gst_wasapi_erole_to_device_role (self->role));
      break;
    case PROP_DEVICE:
      g_value_take_string (value, self->device ?
          g_utf16_to_utf8 (self->device, -1, NULL, NULL, NULL) : NULL);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_wasapi_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (bsrc);
  WAVEFORMATEX *format = NULL;
  GstCaps *caps = NULL;
  HRESULT hr;

  GST_DEBUG_OBJECT (self, "entering get caps");

  if (self->cached_caps) {
    caps = gst_caps_ref (self->cached_caps);
  } else {
    GstCaps *template_caps;

    template_caps = gst_pad_get_pad_template_caps (bsrc->srcpad);

    if (!self->client)
      gst_wasapi_src_open (GST_AUDIO_SRC (bsrc));

    hr = IAudioClient_GetMixFormat (self->client, &format);
    if (hr != S_OK || format == NULL) {
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
          ("GetMixFormat failed: %s", gst_wasapi_util_hresult_to_string (hr)));
      goto out;
    }

    caps =
        gst_wasapi_util_waveformatex_to_caps ((WAVEFORMATEXTENSIBLE *) format,
        template_caps);
    if (caps == NULL) {
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unknown format"));
      goto out;
    }

    self->mix_format = format;
    gst_caps_replace (&self->cached_caps, caps);
    gst_caps_unref (template_caps);
  }

  if (filter) {
    GstCaps *filtered =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = filtered;
  }

  GST_DEBUG_OBJECT (self, "returning caps %" GST_PTR_FORMAT, caps);

out:
  return caps;
}

static gboolean
gst_wasapi_src_open (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  gboolean res = FALSE;
  IAudioClient *client = NULL;

  if (self->client)
    return TRUE;

  /* FIXME: Switching the default device does not switch the stream to it,
   * even if the old device was unplugged. We need to handle this somehow.
   * For example, perhaps we should automatically switch to the new device if
   * the default device is changed and a device isn't explicitly selected. */
  if (!gst_wasapi_util_get_device_client (GST_ELEMENT (self), TRUE,
          self->role, self->device, &client)) {
    if (!self->device)
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ, (NULL),
          ("Failed to get default device"));
    else
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ, (NULL),
          ("Failed to open device %S", self->device));
    goto beach;
  }

  self->client = client;
  res = TRUE;

beach:

  return res;
}

static gboolean
gst_wasapi_src_close (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);

  if (self->client != NULL) {
    IUnknown_Release (self->client);
    self->client = NULL;
  }

  return TRUE;
}

static gboolean
gst_wasapi_src_prepare (GstAudioSrc * asrc, GstAudioRingBufferSpec * spec)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  gboolean res = FALSE;
  IAudioClock *client_clock = NULL;
  guint64 client_clock_freq = 0;
  IAudioCaptureClient *capture_client = NULL;
  REFERENCE_TIME latency_rt;
  HRESULT hr;

  hr = IAudioClient_Initialize (self->client, AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK, spec->buffer_time * 10, 0,
      self->mix_format, NULL);
  if (hr != S_OK) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ, (NULL),
        ("IAudioClient::Initialize failed: %s",
            gst_wasapi_util_hresult_to_string (hr)));
    goto beach;
  }

  /* Get latency for logging */
  hr = IAudioClient_GetStreamLatency (self->client, &latency_rt);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::GetStreamLatency failed");
    goto beach;
  }
  GST_INFO_OBJECT (self, "wasapi stream latency: %" G_GINT64_FORMAT " (%"
      G_GINT64_FORMAT " ms)", latency_rt, latency_rt / 10000);

  /* Set the event handler which will trigger reads */
  hr = IAudioClient_SetEventHandle (self->client, self->event_handle);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::SetEventHandle failed");
    goto beach;
  }

  /* Get the clock and the clock freq */
  if (!gst_wasapi_util_get_clock (GST_ELEMENT (self), self->client,
          &client_clock)) {
    goto beach;
  }

  hr = IAudioClock_GetFrequency (client_clock, &client_clock_freq);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClock::GetFrequency failed");
    goto beach;
  }

  /* Total size of the allocated buffer that we will read from
   * XXX: Will this ever change while playing? */
  hr = IAudioClient_GetBufferSize (self->client, &self->buffer_frame_count);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::GetBufferSize failed");
    goto beach;
  }
  GST_INFO_OBJECT (self, "frame count is %i, blockAlign is %i, "
      "buffer_time is %" G_GINT64_FORMAT, self->buffer_frame_count,
      self->mix_format->nBlockAlign, spec->buffer_time);

  /* Get capture source client and start it up */
  if (!gst_wasapi_util_get_capture_client (GST_ELEMENT (self), self->client,
          &capture_client)) {
    goto beach;
  }

  hr = IAudioClient_Start (self->client);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::Start failed");
    goto beach;
  }

  self->client_clock = client_clock;
  self->client_clock_freq = client_clock_freq;
  self->capture_client = capture_client;

  res = TRUE;

beach:
  if (!res) {
    if (capture_client != NULL)
      IUnknown_Release (capture_client);

    if (client_clock != NULL)
      IUnknown_Release (client_clock);
  }

  return res;
}

static gboolean
gst_wasapi_src_unprepare (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);

  if (self->client != NULL) {
    IAudioClient_Stop (self->client);
  }

  if (self->capture_client != NULL) {
    IUnknown_Release (self->capture_client);
    self->capture_client = NULL;
  }

  if (self->client_clock != NULL) {
    IUnknown_Release (self->client_clock);
    self->client_clock = NULL;
  }

  return TRUE;
}

static guint
gst_wasapi_src_read (GstAudioSrc * asrc, gpointer data, guint length,
    GstClockTime * timestamp)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  HRESULT hr;
  gint16 *from = NULL;
  guint wanted = length;
  DWORD flags;

  while (wanted > 0) {
    guint have_frames, n_frames, want_frames, read_len;

    /* Wait for data to become available */
    WaitForSingleObject (self->event_handle, INFINITE);

    hr = IAudioCaptureClient_GetBuffer (self->capture_client,
        (BYTE **) & from, &have_frames, &flags, NULL, NULL);
    if (hr != S_OK) {
      if (hr == AUDCLNT_S_BUFFER_EMPTY)
        GST_WARNING_OBJECT (self, "IAudioCaptureClient::GetBuffer failed: %s"
            ", retrying", gst_wasapi_util_hresult_to_string (hr));
      else
        GST_ERROR_OBJECT (self, "IAudioCaptureClient::GetBuffer failed: %s",
            gst_wasapi_util_hresult_to_string (hr));
      length = 0;
      goto beach;
    }

    if (flags != 0)
      GST_INFO_OBJECT (self, "buffer flags=%#08x", (guint) flags);

    /* XXX: How do we handle AUDCLNT_BUFFERFLAGS_SILENT? We're supposed to write
     * out silence when that flag is set? See:
     * https://msdn.microsoft.com/en-us/library/windows/desktop/dd370800(v=vs.85).aspx */

    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
      GST_WARNING_OBJECT (self, "WASAPI reported glitch in buffer");

    want_frames = wanted / self->mix_format->nBlockAlign;

    /* If GetBuffer is returning more frames than we can handle, all we can do is
     * hope that this is temporary and that things will settle down later. */
    if (G_UNLIKELY (have_frames > want_frames))
      GST_WARNING_OBJECT (self, "captured too many frames: have %i, want %i",
          have_frames, want_frames);

    /* Only copy data that will fit into the allocated buffer of size @length */
    n_frames = MIN (have_frames, want_frames);
    read_len = n_frames * self->mix_format->nBlockAlign;

    {
      guint bpf = self->mix_format->nBlockAlign;
      GST_TRACE_OBJECT (self, "have: %i (%i bytes), can read: %i (%i bytes), "
          "will read: %i (%i bytes)", have_frames, have_frames * bpf,
          want_frames, wanted, n_frames, read_len);
    }

    memcpy (data, from, read_len);
    wanted -= read_len;

    /* Always release all captured buffers if we've captured any at all */
    hr = IAudioCaptureClient_ReleaseBuffer (self->capture_client, have_frames);
    if (hr != S_OK) {
      GST_ERROR_OBJECT (self,
          "IAudioCaptureClient::ReleaseBuffer () failed: %s",
          gst_wasapi_util_hresult_to_string (hr));
      goto beach;
    }
  }


beach:

  return length;
}

static guint
gst_wasapi_src_delay (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  guint delay = 0;
  HRESULT hr;

  hr = IAudioClient_GetCurrentPadding (self->client, &delay);
  if (hr != S_OK) {
    GST_ELEMENT_ERROR (self, RESOURCE, READ, (NULL),
        ("IAudioClient::GetCurrentPadding failed %s",
            gst_wasapi_util_hresult_to_string (hr)));
  }

  return delay;
}

static void
gst_wasapi_src_reset (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  HRESULT hr;

  if (self->client) {
    hr = IAudioClient_Stop (self->client);
    if (hr != S_OK) {
      GST_ERROR_OBJECT (self, "IAudioClient::Stop () failed: %s",
          gst_wasapi_util_hresult_to_string (hr));
      return;
    }

    hr = IAudioClient_Reset (self->client);
    if (hr != S_OK) {
      GST_ERROR_OBJECT (self, "IAudioClient::Reset () failed: %s",
          gst_wasapi_util_hresult_to_string (hr));
      return;
    }
  }
}

static GstClockTime
gst_wasapi_src_get_time (GstClock * clock, gpointer user_data)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (user_data);
  HRESULT hr;
  guint64 devpos;
  GstClockTime result;

  if (G_UNLIKELY (self->client_clock == NULL))
    return GST_CLOCK_TIME_NONE;

  hr = IAudioClock_GetPosition (self->client_clock, &devpos, NULL);
  if (G_UNLIKELY (hr != S_OK))
    return GST_CLOCK_TIME_NONE;

  result = gst_util_uint64_scale_int (devpos, GST_SECOND,
      self->client_clock_freq);

  /*
     GST_DEBUG_OBJECT (self, "devpos = %" G_GUINT64_FORMAT
     " frequency = %" G_GUINT64_FORMAT
     " result = %" G_GUINT64_FORMAT " ms",
     devpos, self->client_clock_freq, GST_TIME_AS_MSECONDS (result));
   */

  return result;
}
