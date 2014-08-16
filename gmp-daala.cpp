/*!
 * \copy
 *     Copyright (c)  2009-2014, Cisco Systems
 *     Copyright (c)  2014, Mozilla
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or without
 *     modification, are permitted provided that the following conditions
 *     are met:
 *
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *
 *        * Redistributions in binary form must reproduce the above copyright
 *          notice, this list of conditions and the following disclaimer in
 *          the documentation and/or other materials provided with the
 *          distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *     BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *     CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *     LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *     ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 *
 *************************************************************************************
 */

#include <stdint.h>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <memory>
#include <assert.h>
#include <limits.h>

#include "gmp-platform.h"
#include "gmp-video-host.h"
#include "gmp-video-encode.h"
#include "gmp-video-decode.h"
#include "gmp-video-frame-i420.h"
#include "gmp-video-frame-encoded.h"

#include "daala/daalaenc.h"
#include "daala/daaladec.h"

#include "task_utils.h"

#if defined(_MSC_VER)
#define PUBLIC_FUNC __declspec(dllexport)
#else
#define PUBLIC_FUNC
#endif


// TODO(ekr@rtfm.com): Fix this
#define Error(x)

static int g_log_level = 0;

#define GMPLOG(l, x) do { \
        if (l <= g_log_level) { \
        const char *log_string = "unknown"; \
        if ((l >= 0) && (l <= 3)) {               \
        log_string = kLogStrings[l];            \
        } \
        std::cerr << "GMPDaala: " << log_string << ": " << x << std::endl; \
        } \
    } while(0)

#define GL_CRIT 0
#define GL_ERROR 1
#define GL_INFO  2
#define GL_DEBUG 3

const char* kLogStrings[] = {
  "Critical",
  "Error",
  "Info",
  "Debug"
};


static GMPPlatformAPI* g_platform_api = NULL;

class DaalaVideoEncoder;
class DaalaVideoDecoder;

class EncodedFrame {
 public:
  EncodedFrame() : data(NULL) {}
  ~EncodedFrame() { delete data; }

  uint32_t width_;
  uint32_t height_;
  uint32_t timestamp_;
  uint8_t* data;
  size_t len;
};

static void init_plane(const uint8_t *data,
                       const od_img *img,
                       unsigned char dec,
                       od_img_plane *plane) {
  plane->data = static_cast<unsigned char *>(const_cast<uint8_t *>
                                             (data));
  plane->xdec = plane->ydec = dec;
  plane->xstride = 1;
  plane->ystride = img->width >> dec;
}

class DaalaVideoEncoder : public GMPVideoEncoder {
 public:
  DaalaVideoEncoder (GMPVideoHost* hostAPI) :
    host_ (hostAPI),
    worker_thread_ (NULL),
    callback_ (NULL),
    enc_ctx_(NULL) {
  }

  virtual void InitEncode (const GMPVideoCodec& codecSettings,
                           const uint8_t* aCodecSpecific,
                           uint32_t aCodecSpecificSize,
                           GMPVideoEncoderCallback* callback,
                           int32_t numberOfCores,
                           uint32_t maxPayloadSize) {
    GMPErr err = g_platform_api->createthread (&worker_thread_);
    if (err != GMPNoErr) {
      GMPLOG (GL_ERROR, "Couldn't create new thread");
      Error (GMPGenericErr);
      return;
    }

    daala_info info;
    daala_info_init(&info);

    info.pic_width = codecSettings.mWidth;
    info.pic_height = codecSettings.mHeight;
    info.nplanes = 3;
    info.plane_info[0].xdec = 0;
    info.plane_info[0].ydec = 0;
    info.plane_info[1].xdec = 1;
    info.plane_info[1].ydec = 1;
    info.plane_info[2].xdec = 1;
    info.plane_info[2].ydec = 1;
    info.pixel_aspect_numerator=1;
    info.pixel_aspect_denominator=1;
    info.timebase_numerator=90000;
    info.timebase_denominator=1;
    info.frame_duration = 1;
    info.keyframe_rate = 300;

    enc_ctx_ = daala_encode_create(&info);
    if (!enc_ctx_) {
      GMPLOG (GL_ERROR, "Couldn't create encoder");
      Error (GMPGenericErr);
      return;
    }

    int use_chroma = 0;
    daala_encode_ctl(enc_ctx_, OD_SET_MC_USE_CHROMA,
                     &use_chroma, sizeof(use_chroma));
    int mv_res_min = 2;
    daala_encode_ctl(enc_ctx_, OD_SET_MV_RES_MIN,
                     &mv_res_min, sizeof(mv_res_min));
    int mv_level_min = 4;
    daala_encode_ctl(enc_ctx_, OD_SET_MV_LEVEL_MIN,
                     &mv_level_min, sizeof(mv_level_min));

    daala_comment dc;
    daala_comment_init(&dc);

    // Flush and discard the comments.
    for(;;) {
      ogg_packet op;
      int r = daala_encode_flush_header(enc_ctx_,
                                        &dc,
                                        &op);

      if (!r)
        break;
    }

    callback_ = callback;
    GMPLOG (GL_INFO, "Initialized encoder");
  }

  virtual void Encode (GMPVideoi420Frame* inputImage,
                         const uint8_t* aCodecSpecificInfo,
                         uint32_t aCodecSpecificInfoLength,
                         const GMPVideoFrameType* aFrameTypes,
                         uint32_t aFrameTypesLength) {
    GMPLOG (GL_DEBUG,
            __FUNCTION__
            << " size="
            << inputImage->Width() << "x" << inputImage->Height());

    assert (aFrameTypesLength != 0);

    worker_thread_->Post (WrapTask (
        this, &DaalaVideoEncoder::Encode_w,
        inputImage,
        (aFrameTypes)[0]));
  }

  void Encode_w (GMPVideoi420Frame* inputImage,
                 GMPVideoFrameType frame_type) {
    GMPLOG (GL_DEBUG,  __FUNCTION__);
    int encoded_ct = 0;

    const uint8_t* y = inputImage->Buffer(kGMPYPlane);
    const uint8_t* u = inputImage->Buffer(kGMPUPlane);
    const uint8_t* v = inputImage->Buffer(kGMPVPlane);

    od_img daala_img;
    daala_img.nplanes = 3;
    daala_img.width = inputImage->Width();
    daala_img.height = inputImage->Height();
    init_plane(y, &daala_img, 0, &daala_img.planes[0]);
    init_plane(u, &daala_img, 1, &daala_img.planes[1]);
    init_plane(v, &daala_img, 1, &daala_img.planes[2]);

    int rv = daala_encode_img_in(enc_ctx_,
                                 &daala_img,
                                 1); // Dummy duration
    if (rv) {
      GMPLOG(GL_ERROR, "Failure encoding image");
      return;
    }

    for(;;) {
     ogg_packet op;

      rv = daala_encode_packet_out(enc_ctx_, 0, &op);
      if (rv == 0)
        break;

      if (rv < 0) {
        GMPLOG(GL_ERROR, "Failure encoding output packet "
               << op.bytes);
        return;
      }

      /* We have a packet, let's output it */
      GMPLOG(GL_DEBUG, "Packet size " << op.bytes);

      // TODO(ekr@rtfm.com): Check bytes in-range
      size_t len = op.bytes + 5;
      uint8_t* data = new uint8_t[len];
      uint32_t* lp = reinterpret_cast<uint32_t*>(data);
      *lp = op.bytes + 1;  // include NAL type
      // daala_packet_iskeyframe(op.packet, op.bytes);
      data[4] = 5;
      memcpy(data + 5, op.packet, op.bytes);

      // Synchronously send this back to the main thread for delivery.
      g_platform_api->syncrunonmainthread (WrapTask (
          this,
          &DaalaVideoEncoder::Encode_m,
          inputImage,
          frame_type,
          data, len));

      delete[] data;
    }

    DestroyFrame(inputImage);
    GMPLOG (GL_DEBUG,  __FUNCTION__ << " done");
  }

  void Encode_m (GMPVideoi420Frame* inputImage,
                 GMPVideoFrameType frame_type,
                 const uint8_t* data, size_t len) {
    if (frame_type  == kGMPKeyFrame) {
      if (!inputImage)
        return;
    }
    if (!inputImage) {
      GMPLOG (GL_ERROR, "no input image");
      return;
    }

    // Now return the encoded data back to the parent.
    GMPVideoFrame* ftmp;
    GMPErr err = host_->CreateFrame (kGMPEncodedVideoFrame, &ftmp);
    if (err != GMPNoErr) {
      GMPLOG (GL_ERROR, "Error creating encoded frame");
      return;
    }

    GMPVideoEncodedFrame* f = static_cast<GMPVideoEncodedFrame*> (ftmp);
    err = f->CreateEmptyFrame (len);
    if (err != GMPNoErr) {
      GMPLOG (GL_ERROR, "Error allocating frame data");
      f->Destroy();
      return;
    }
    memcpy(f->Buffer(), data, len);

    f->SetEncodedWidth (inputImage->Width());
    f->SetEncodedHeight (inputImage->Height());
    f->SetTimeStamp (inputImage->Timestamp());
    f->SetFrameType (frame_type);
    f->SetCompleteFrame (true);
    f->SetBufferType (GMP_BufferLength32);

    GMPLOG (GL_DEBUG, "Encoding complete. type= "
            << f->FrameType()
            << " length="
            << f->Size()
            << " timestamp="
            << f->TimeStamp());

    // Return the encoded frame.
    GMPCodecSpecificInfo info;
    memset (&info, 0, sizeof (info));
    info.mCodecType = kGMPVideoCodecH264;
    info.mBufferType = GMP_BufferLength32;
    info.mCodecSpecific.mH264.mSimulcastIdx = 0;
    GMPLOG (GL_DEBUG, "Calling callback");
    callback_->Encoded (f, reinterpret_cast<uint8_t*> (&info), sizeof(info));
    GMPLOG (GL_DEBUG, "Callback called");
  }

  virtual void SetChannelParameters (uint32_t aPacketLoss, uint32_t aRTT) {
  }

  virtual void SetRates (uint32_t aNewBitRate, uint32_t aFrameRate) {
  }

  virtual void SetPeriodicKeyFrames (bool aEnable) {
  }

  virtual void EncodingComplete() {
    delete this;
  }

 private:
  void DestroyFrame(GMPVideoi420Frame* frame) {
    g_platform_api->syncrunonmainthread (WrapTask (
        this,
        &DaalaVideoEncoder::DestroyFrame_m,
        frame));
  }

  void DestroyFrame_m(GMPVideoi420Frame* frame) {
    frame->Destroy();
  }

  uint8_t AveragePlane(uint8_t* ptr, size_t len) {
    uint64_t val = 0;

    for (size_t i=0; i<len; ++i) {
      val += ptr[i];
    }

    return (val / len) % 0xff;
  }

  GMPVideoHost* host_;
  GMPThread* worker_thread_;
  GMPVideoEncoderCallback* callback_;
  ::daala_enc_ctx *enc_ctx_;
};


static unsigned char kDummyPacket1[] = {
0x80, 0x64, 0x61, 0x61, 0x6c, 0x61, 0x00, 0x00,
0x00, 0xb0, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x03, 0x00,
0x00, 0x01, 0x01, 0x01, 0x01
};

static unsigned char kDummyPacket2[] = {
0x81, 0x64, 0x61, 0x61, 0x6c, 0x61, 0x2f, 0x00,
0x00, 0x00, 0x58, 0x69, 0x70, 0x68, 0x27, 0x73,
0x20, 0x65, 0x78, 0x70, 0x65, 0x72, 0x69, 0x6d,
0x65, 0x6e, 0x74, 0x61, 0x6c, 0x20, 0x65, 0x6e,
0x63, 0x6f, 0x64, 0x65, 0x72, 0x20, 0x6c, 0x69,
0x62, 0x72, 0x61, 0x72, 0x79, 0x20, 0x53, 0x65,
0x70, 0x20, 0x33, 0x30, 0x20, 0x32, 0x30, 0x31,
0x33, 0x00, 0x00, 0x00, 0x00
};

static unsigned char kDummyPacket3[] = {
  0x82, 0x64, 0x61, 0x61, 0x6c, 0x61
};

class DaalaVideoDecoder : public GMPVideoDecoder {
 public:
  DaalaVideoDecoder (GMPVideoHost* hostAPI) :
    host_ (hostAPI),
    worker_thread_ (NULL),
    callback_ (NULL),
    dec_ctx_(NULL) {}

  virtual ~DaalaVideoDecoder() {
  }

  virtual void InitDecode (const GMPVideoCodec& codecSettings,
                             const uint8_t* aCodecSpecific,
                             uint32_t aCodecSpecificSize,
                             GMPVideoDecoderCallback* callback,
                             int32_t coreCount) {
    GMPLOG (GL_INFO, "InitDecode");

    GMPErr err = g_platform_api->createthread (&worker_thread_);
    if (err != GMPNoErr) {
      GMPLOG (GL_ERROR, "Couldn't create new thread");
      Error (GMPGenericErr);
      return;
    }

    daala_info info;
    daala_info_init(&info);
    daala_comment dc;
    daala_comment_init(&dc);
    daala_setup_info* ds = NULL;
    ogg_packet op;
    int rv;

    memset(&op, 0, sizeof(op));
    op.packet = kDummyPacket1;
    op.bytes = sizeof(kDummyPacket1);
    op.b_o_s = 1;
    rv = daala_decode_header_in(&info, &dc, &ds, &op);
    if (rv <= 0) {
      GMPLOG(GL_ERROR, "Failure reading header packet 1");
    }

    memset(&op, 0, sizeof(op));
    op.packet = kDummyPacket2;
    op.bytes = sizeof(kDummyPacket2);
    rv = daala_decode_header_in(&info, &dc, &ds, &op);
    if (rv <= 0) {
      GMPLOG(GL_ERROR, "Failure reading header packet 2");
    }

    memset(&op, 0, sizeof(op));
    op.packet = kDummyPacket3;
    op.bytes = sizeof(kDummyPacket3);
    rv = daala_decode_header_in(&info, &dc, &ds, &op);
    if (rv <= 0) {
      GMPLOG(GL_ERROR, "Failure reading header packet 3");
    }

    dec_ctx_ = daala_decode_alloc(&info, ds);
    if (!dec_ctx_) {
      GMPLOG(GL_ERROR, "Failure creating Daala ctx");
    }

    callback_ = callback;
  }

  virtual void Decode (GMPVideoEncodedFrame* inputFrame,
                         bool missingFrames,
                         const uint8_t* aCodecSpecificInfo,
                         uint32_t aCodecSpecificInfoLength,
                         int64_t renderTimeMs = -1) {
    GMPLOG (GL_DEBUG, __FUNCTION__
            << "Decoding frame size=" << inputFrame->Size()
            << " timestamp=" << inputFrame->TimeStamp());

    worker_thread_->Post (WrapTask (
        this, &DaalaVideoDecoder::Decode_w,
        inputFrame,
        missingFrames,
        renderTimeMs));

  }

  virtual void Reset() {
  }

  virtual void Drain() {
  }

  virtual void DecodingComplete() {
    delete this;
  }

  void Decode_w (GMPVideoEncodedFrame* inputFrame,
                 bool missingFrames,
                 int64_t renderTimeMs = -1) {
    GMPLOG (GL_DEBUG, __FUNCTION__ <<" on worker thread length = "
            << inputFrame->Size());

    if (inputFrame->Size() < 5) {
      GMPLOG(GL_ERROR, "Bogus length");
      return;
    }

    ogg_packet op;
    memset(&op, 0, sizeof(op));
    op.packet = inputFrame->Buffer() + 5;
    op.bytes = inputFrame->Size() - 5;

    od_img img;
    memset(&img, 0, sizeof(img));
    int rv = daala_decode_packet_in(dec_ctx_, &img, &op);
    if (rv) {
      GMPLOG(GL_ERROR, "Failure reading data");
    }

    assert(!(img.width & 1));
    assert(!(img.height & 1));

    // TODO(ekr@rtfm.com): Assert that xstride == 1
    size_t y_len = img.height * img.width;
    uint8_t* y = new uint8_t[y_len];
    size_t u_len = (img.height * img.width) / 4;
    uint8_t* u = new uint8_t[u_len];
    size_t v_len = (img.height * img.width) / 4;
    uint8_t* v = new uint8_t[v_len];

    // Now copy the Daala packet into len.
    // First Y
    size_t to_offset = 0;
    size_t from_offset = 0;
    for (int32_t row = 0; row < img.height; ++row) {

      memcpy(y + to_offset, img.planes[0].data + from_offset,
             img.width);

      to_offset += img.width;
      from_offset += img.planes[0].ystride;
    }

    // Now U and V
    // TODO(ekr@rtfm.com): assert that the strides are equal.
    to_offset = from_offset = 0;
    for (int32_t row = 0; row < img.height/2; ++row) {
      memcpy(u + to_offset, img.planes[1].data + from_offset,
             img.width/2);
      memcpy(v + to_offset, img.planes[2].data + from_offset,
             img.width/2);

      to_offset += img.width/2;
      from_offset += img.planes[2].ystride;
    }

    GMPLOG (GL_DEBUG, __FUNCTION__ <<" decoded");

    g_platform_api->syncrunonmainthread(WrapTask (
        this,
        &DaalaVideoDecoder::Decode_m,
        inputFrame,
        img,
        y_len, y,
        u_len, u,
        v_len, v,
        renderTimeMs));

    delete[] y;
    delete[] u;
    delete[] v;
  }

  // Return the decoded data back to the parent.
  void Decode_m (GMPVideoEncodedFrame* inputFrame,
                 const od_img& img,
                 size_t y_len, const uint8_t* y,
                 size_t u_len, const uint8_t* u,
                 size_t v_len, const uint8_t* v,
                 int64_t renderTimeMs) {
    GMPLOG (GL_DEBUG, __FUNCTION__ << " Video frame ready for display "
            << img.width
            << "x"
            << img.height
            << " timestamp="
            << inputFrame->TimeStamp());

    GMPVideoFrame* ftmp = NULL;

    // Translate the image.
    GMPErr err = host_->CreateFrame (kGMPI420VideoFrame, &ftmp);
    if (err != GMPNoErr) {
      GMPLOG (GL_ERROR, "Couldn't allocate empty I420 frame");
      return;
    }

    GMPVideoi420Frame* frame = static_cast<GMPVideoi420Frame*> (ftmp);
    err = frame->CreateFrame (
        y_len, y,
        u_len, u,
        v_len, v,
        img.width, img.height,
        img.width,
        img.width/2,
        img.width/2);
    if (err != GMPNoErr) {
      GMPLOG (GL_ERROR, "Couldn't make decoded frame");
      return;
    }
    frame->SetTimestamp (inputFrame->TimeStamp());
    frame->SetDuration (inputFrame->Duration());
    callback_->Decoded (frame);
    inputFrame->Destroy();
  }

  GMPVideoHost* host_;
  GMPThread* worker_thread_;
  GMPVideoDecoderCallback* callback_;
  ::daala_dec_ctx *dec_ctx_;
};

extern "C" {

  PUBLIC_FUNC GMPErr
  GMPInit (GMPPlatformAPI* aPlatformAPI) {
    g_platform_api = aPlatformAPI;
    return GMPNoErr;
  }

  PUBLIC_FUNC GMPErr
  GMPGetAPI (const char* aApiName, void* aHostAPI, void** aPluginApi) {
    if (!strcmp (aApiName, "decode-video")) {
      *aPluginApi = new DaalaVideoDecoder (static_cast<GMPVideoHost*> (aHostAPI));
      return GMPNoErr;
    } else if (!strcmp (aApiName, "encode-video")) {
      *aPluginApi = new DaalaVideoEncoder (static_cast<GMPVideoHost*> (aHostAPI));
      return GMPNoErr;
    }
    return GMPGenericErr;
  }

  PUBLIC_FUNC void
  GMPShutdown (void) {
    g_platform_api = NULL;
  }

} // extern "C"
