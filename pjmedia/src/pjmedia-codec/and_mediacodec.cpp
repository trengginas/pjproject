/* $Id$ */
/*
 * Copyright (C)2020 Teluu Inc. (http://www.teluu.com)
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
#include <pjmedia-codec/and_mediacodec.h>
#include <pjmedia-codec/h264_packetizer.h>
#include <pjmedia/vid_codec_util.h>
#include <pjmedia/errno.h>
#include <pj/log.h>

#if defined(PJMEDIA_HAS_ANDROID_MEDIACODEC) && \
            PJMEDIA_HAS_ANDROID_MEDIACODEC != 0 && \
    defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

/* Android AMediaCodec: */
#include "media/NdkMediaCodec.h"

/*
#if __ANDROID_API__ >=19
#endif
*/

/*
 * Constants
 */
#define THIS_FILE		"and_mediacodec.cpp"
#define ANMED_H264_MIME_TYPE    "video/avc"
#define ANMED_OMX_H264_ENCODER  "OMX.google.h264.encoder"
#define ANMED_OMX_H264_DECODER  "OMX.google.h264.decoder"
#define ANMED_KEY_COLOR_FMT     "color-format"
#define ANMED_KEY_WIDTH         "width"
#define ANMED_KEY_HEIGHT        "height"
#define ANMED_KEY_BIT_RATE      "bitrate"
#define ANMED_KEY_PROFILE       "profile"
#define ANMED_KEY_FRAME_RATE    "frame-rate"
#define ANMED_KEY_IFR_INTTERVAL "i-frame-interval"
#define ANMED_KEY_MIME          "mime"
#define ANMED_KEY_REQUEST_SYNCF	"request-sync"
#define ANMED_KEY_CSD0	        "csd-0"
#define ANMED_KEY_CSD1	        "csd-1"
#define ANMED_KEY_MAX_INPUT_SZ  "max-input-size"
#define ANMED_KEY_ENCODER	"encoder"
#define ANMED_KEY_LOW_LATENCY	"low-latency"
#define ANMED_KEY_STRIDE	"stride"
//#define ANMED_COLOR_FMT         0x7f420888 /* YUV420Flexible */
#define ANMED_COLOR_FMT         0x13
#define ANMED_QUEUE_TIMEOUT     2000*100

#define DEFAULT_WIDTH		352
#define DEFAULT_HEIGHT		288

#define DEFAULT_FPS		15
#define DEFAULT_AVG_BITRATE	256000
#define DEFAULT_MAX_BITRATE	256000

#define SPS_PPS_BUF_SIZE	64

#define MAX_RX_WIDTH		1200
#define MAX_RX_HEIGHT		800

/* Maximum duration from one key frame to the next (in seconds). */
#define KEYFRAME_INTERVAL	1

#define CODEC_WAIT_RETRY 	20
#define CODEC_THREAD_WAIT 	10
/* Timeout until the buffer is ready in ms. */
#define CODEC_DEQUEUE_TIMEOUT 	2000

#if 1
#define TRACE_(expr) PJ_LOG(5, expr)
#else
#define TRACE(expr) 0
#endif

/*
 * Factory operations.
 */
static pj_status_t anmed_test_alloc(pjmedia_vid_codec_factory *factory,
                                    const pjmedia_vid_codec_info *info );
static pj_status_t anmed_default_attr(pjmedia_vid_codec_factory *factory,
                                      const pjmedia_vid_codec_info *info,
                                      pjmedia_vid_codec_param *attr );
static pj_status_t anmed_enum_info(pjmedia_vid_codec_factory *factory,
                                   unsigned *count,
                                   pjmedia_vid_codec_info codecs[]);
static pj_status_t anmed_alloc_codec(pjmedia_vid_codec_factory *factory,
                                     const pjmedia_vid_codec_info *info,
                                     pjmedia_vid_codec **p_codec);
static pj_status_t anmed_dealloc_codec(pjmedia_vid_codec_factory *factory,
                                       pjmedia_vid_codec *codec );


/*
 * Codec operations
 */
static pj_status_t anmed_codec_init(pjmedia_vid_codec *codec,
                                    pj_pool_t *pool );
static pj_status_t anmed_codec_open(pjmedia_vid_codec *codec,
                                    pjmedia_vid_codec_param *param );
static pj_status_t anmed_codec_close(pjmedia_vid_codec *codec);
static pj_status_t anmed_codec_modify(pjmedia_vid_codec *codec,
                                      const pjmedia_vid_codec_param *param);
static pj_status_t anmed_codec_get_param(pjmedia_vid_codec *codec,
                                         pjmedia_vid_codec_param *param);
static pj_status_t anmed_codec_encode_begin(pjmedia_vid_codec *codec,
                                            const pjmedia_vid_encode_opt *opt,
                                            const pjmedia_frame *input,
                                            unsigned out_size,
                                            pjmedia_frame *output,
                                            pj_bool_t *has_more);
static pj_status_t anmed_codec_encode_more(pjmedia_vid_codec *codec,
                                           unsigned out_size,
                                           pjmedia_frame *output,
                                           pj_bool_t *has_more);
static pj_status_t anmed_codec_decode(pjmedia_vid_codec *codec,
                                      pj_size_t count,
                                      pjmedia_frame packets[],
                                      unsigned out_size,
                                      pjmedia_frame *output);

/* Definition for Android AMediaCodec operations. */
static pjmedia_vid_codec_op anmed_codec_op =
{
    &anmed_codec_init,
    &anmed_codec_open,
    &anmed_codec_close,
    &anmed_codec_modify,
    &anmed_codec_get_param,
    &anmed_codec_encode_begin,
    &anmed_codec_encode_more,
    &anmed_codec_decode,
    NULL
};

/* Definition for Android AMediaCodec factory operations. */
static pjmedia_vid_codec_factory_op anmed_factory_op =
{
    &anmed_test_alloc,
    &anmed_default_attr,
    &anmed_enum_info,
    &anmed_alloc_codec,
    &anmed_dealloc_codec
};

static struct anmed_factory
{
    pjmedia_vid_codec_factory    base;
    pjmedia_vid_codec_mgr	*mgr;
    pj_pool_factory             *pf;
    pj_pool_t		        *pool;
} anmed_factory;

enum avc_frm_type {
    AVC_FRM_TYPE_KEYFRAME = 1,
    AVC_FRM_TYPE_SPS_PPS = 2
};

typedef struct anmed_codec_data
{
    pj_pool_t			*pool;
    pjmedia_vid_codec_param	*prm;
    pj_bool_t			 whole;
    pjmedia_h264_packetizer	*pktz;

    /* Encoder state */
    AMediaCodec                 *enc;
    unsigned		 	 enc_input_size;
    pj_uint8_t			*enc_frame_whole;
    unsigned			 enc_frame_size;
    unsigned			 enc_processed;
    AMediaCodecBufferInfo        enc_buf_info;
    int				 enc_output_buf_idx;
    pj_uint8_t			 enc_sps_pps_buf[SPS_PPS_BUF_SIZE];
    unsigned			 enc_sps_pps_len;
    pj_bool_t			 enc_sps_pps_ex;
    pj_bool_t                    enc_started;

    /* Decoder state */
    AMediaCodec                 *dec;
    pj_uint8_t			*dec_buf;
    unsigned			 dec_stride_len;
    unsigned			 dec_buf_size;
    char			 dec_sps_buf[SPS_PPS_BUF_SIZE];
    char			 dec_sps_len;
    char			 dec_pps_buf[SPS_PPS_BUF_SIZE];
    char			 dec_pps_len;
    AMediaCodecBufferInfo        dec_buf_info;
    pj_bool_t                    dec_started;
} anmed_codec_data;

static pj_status_t configure_decoder(anmed_codec_data *anmed_data) {
    media_status_t am_status;
    AMediaFormat *vid_fmt;

    vid_fmt = AMediaFormat_new();
    if (!vid_fmt) {
        return PJ_ENOMEM;
    }

    AMediaFormat_setString(vid_fmt, ANMED_KEY_MIME, ANMED_H264_MIME_TYPE);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_FRAME_RATE,
			  (anmed_data->prm->dec_fmt.det.vid.fps.num /
			   anmed_data->prm->dec_fmt.det.vid.fps.denum));
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_WIDTH,
	                  anmed_data->prm->dec_fmt.det.vid.size.w);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_HEIGHT,
	                  anmed_data->prm->dec_fmt.det.vid.size.h);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_COLOR_FMT, ANMED_COLOR_FMT);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_MAX_INPUT_SZ, 0);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_ENCODER, 0);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_LOW_LATENCY, 1);

    if (anmed_data->dec_sps_len) {
	AMediaFormat_setBuffer(vid_fmt, ANMED_KEY_CSD0,
			       anmed_data->dec_sps_buf,
			       anmed_data->dec_sps_len);
    }

    if (anmed_data->dec_pps_len) {
	AMediaFormat_setBuffer(vid_fmt, ANMED_KEY_CSD1,
			       anmed_data->dec_pps_buf,
			       anmed_data->dec_pps_len);
    }
    am_status = AMediaCodec_configure(anmed_data->dec, vid_fmt, NULL,
				      NULL, 0);

    AMediaFormat_delete(vid_fmt);
    if (am_status != AMEDIA_OK) {
        PJ_LOG(4, (THIS_FILE, "Decoder configure failed, status=%d",
        	   am_status));
        return PJMEDIA_CODEC_EFAILED;
    }

    PJ_LOG(4, (THIS_FILE, "Decoder configure success"));
    if (!anmed_data->dec_started) {
        am_status = AMediaCodec_start(anmed_data->dec);
        if (am_status != AMEDIA_OK) {
            PJ_LOG(4, (THIS_FILE, "Decoder start failed, status=%d",
                       am_status));
            return PJMEDIA_CODEC_EFAILED;
        }
        PJ_LOG(4, (THIS_FILE, "Decoder started"));
        anmed_data->dec_started = PJ_TRUE;
    }
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pjmedia_codec_anmed_vid_init(pjmedia_vid_codec_mgr *mgr,
                                                 pj_pool_factory *pf)
{
    const pj_str_t h264_name = { (char*)"H264", 4};
    pj_status_t status;

    if (anmed_factory.pool != NULL) {
	/* Already initialized. */
	return PJ_SUCCESS;
    }

    if (!mgr) mgr = pjmedia_vid_codec_mgr_instance();
    PJ_ASSERT_RETURN(mgr, PJ_EINVAL);

    /* Create Android AMediaCodec codec factory. */
    anmed_factory.base.op = &anmed_factory_op;
    anmed_factory.base.factory_data = NULL;
    anmed_factory.mgr = mgr;
    anmed_factory.pf = pf;
    anmed_factory.pool = pj_pool_create(pf, "anmedfactory", 256, 256, NULL);
    if (!anmed_factory.pool)
	return PJ_ENOMEM;

    /* Registering format match for SDP negotiation */
    status = pjmedia_sdp_neg_register_fmt_match_cb(
					&h264_name,
					&pjmedia_vid_codec_h264_match_sdp);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Register codec factory to codec manager. */
    status = pjmedia_vid_codec_mgr_register_factory(mgr,
						    &anmed_factory.base);
    if (status != PJ_SUCCESS)
	goto on_error;

    PJ_LOG(4,(THIS_FILE, "Android AMediaCodec initialized"));

    /* Done. */
    return PJ_SUCCESS;

on_error:
    pj_pool_release(anmed_factory.pool);
    anmed_factory.pool = NULL;
    return status;
}

/*
 * Unregister Android AMediaCodec factory from pjmedia endpoint.
 */
PJ_DEF(pj_status_t) pjmedia_codec_anmed_vid_deinit(void)
{
    pj_status_t status = PJ_SUCCESS;

    if (anmed_factory.pool == NULL) {
	/* Already deinitialized */
	return PJ_SUCCESS;
    }

    /* Unregister Android AMediaCodec factory. */
    status = pjmedia_vid_codec_mgr_unregister_factory(anmed_factory.mgr,
						      &anmed_factory.base);

    /* Destroy pool. */
    pj_pool_release(anmed_factory.pool);
    anmed_factory.pool = NULL;

    return status;
}

static pj_status_t anmed_test_alloc(pjmedia_vid_codec_factory *factory,
                                    const pjmedia_vid_codec_info *info )
{
    PJ_ASSERT_RETURN(factory == &anmed_factory.base, PJ_EINVAL);
    

    if (info->fmt_id == PJMEDIA_FORMAT_H264 &&
	info->pt != 0)
    {
	return PJ_SUCCESS;
    }    

    return PJMEDIA_CODEC_EUNSUP;
}

static pj_status_t anmed_default_attr(pjmedia_vid_codec_factory *factory,
                                      const pjmedia_vid_codec_info *info,
                                      pjmedia_vid_codec_param *attr )
{
    PJ_ASSERT_RETURN(factory == &anmed_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info && attr, PJ_EINVAL);

    pj_bzero(attr, sizeof(pjmedia_vid_codec_param));

    attr->dir = PJMEDIA_DIR_ENCODING_DECODING;
    attr->packing = PJMEDIA_VID_PACKING_PACKETS;

    /* Encoded format */
    pjmedia_format_init_video(&attr->enc_fmt, PJMEDIA_FORMAT_H264,
                              DEFAULT_WIDTH, DEFAULT_HEIGHT,
			      DEFAULT_FPS, 1);

    /* Decoded format */
    pjmedia_format_init_video(&attr->dec_fmt, PJMEDIA_FORMAT_I420,
                              DEFAULT_WIDTH, DEFAULT_HEIGHT,
			      DEFAULT_FPS, 1);

    /* Decoding fmtp */
    attr->dec_fmtp.cnt = 2;
    attr->dec_fmtp.param[0].name = pj_str((char*)"profile-level-id");
    attr->dec_fmtp.param[0].val = pj_str((char*)"42e01e");
    attr->dec_fmtp.param[1].name = pj_str((char*)" packetization-mode");
    attr->dec_fmtp.param[1].val = pj_str((char*)"1");

    /* Bitrate */
    attr->enc_fmt.det.vid.avg_bps = DEFAULT_AVG_BITRATE;
    attr->enc_fmt.det.vid.max_bps = DEFAULT_MAX_BITRATE;

    /* Encoding MTU */
    attr->enc_mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

    return PJ_SUCCESS;
}

static pj_status_t anmed_enum_info(pjmedia_vid_codec_factory *factory,
                                   unsigned *count,
                                   pjmedia_vid_codec_info info[])
{
    PJ_ASSERT_RETURN(info && *count > 0, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &anmed_factory.base, PJ_EINVAL);

    *count = 1;
    info->fmt_id = PJMEDIA_FORMAT_H264;
    info->pt = PJMEDIA_RTP_PT_H264;
    info->encoding_name = pj_str((char*)"H264");
    info->encoding_desc = pj_str((char*)"Android AMediaCodec");
    info->clock_rate = 90000;
    info->dir = PJMEDIA_DIR_ENCODING_DECODING;
    info->dec_fmt_id_cnt = 1;
    info->dec_fmt_id[0] = PJMEDIA_FORMAT_I420;
    info->packings = PJMEDIA_VID_PACKING_PACKETS |
		     PJMEDIA_VID_PACKING_WHOLE;
    info->fps_cnt = 3;
    info->fps[0].num = 15;
    info->fps[0].denum = 1;
    info->fps[1].num = 25;
    info->fps[1].denum = 1;
    info->fps[2].num = 30;
    info->fps[2].denum = 1;

    return PJ_SUCCESS;
}

static pj_status_t anmed_alloc_codec(pjmedia_vid_codec_factory *factory,
                                     const pjmedia_vid_codec_info *info,
                                     pjmedia_vid_codec **p_codec)
{
    pj_pool_t *pool;
    pjmedia_vid_codec *codec;
    anmed_codec_data *anmed_data;

    PJ_ASSERT_RETURN(factory == &anmed_factory.base && info && p_codec,
                     PJ_EINVAL);

    *p_codec = NULL;

    pool = pj_pool_create(anmed_factory.pf, "anmed%p", 512, 512, NULL);
    if (!pool)
	return PJ_ENOMEM;

    /* codec instance */
    codec = PJ_POOL_ZALLOC_T(pool, pjmedia_vid_codec);
    codec->factory = factory;
    codec->op = &anmed_codec_op;

    /* codec data */
    anmed_data = PJ_POOL_ZALLOC_T(pool, anmed_codec_data);
    anmed_data->pool = pool;
    codec->codec_data = anmed_data;

    anmed_data->enc = AMediaCodec_createCodecByName(ANMED_OMX_H264_ENCODER);
    if (!anmed_data->enc) {
        PJ_LOG(4,(THIS_FILE, "Failed creating encoder: %s",
                  ANMED_OMX_H264_ENCODER));
        goto on_error;
    }

    PJ_LOG(4, (THIS_FILE, "Success creating encoder: %s",
               ANMED_OMX_H264_ENCODER));

    //anmed_data->dec = AMediaCodec_createCodecByName(ANMED_OMX_H264_DECODER);
    anmed_data->dec = AMediaCodec_createDecoderByType("video/avc");
    if (!anmed_data->dec) {
        PJ_LOG(4,(THIS_FILE, "Failed creating decoder: %s",
                  ANMED_OMX_H264_DECODER));
        goto on_error;
    }
    PJ_LOG(4, (THIS_FILE, "Success creating decoder : %s",
               ANMED_OMX_H264_DECODER));

    *p_codec = codec;
    return PJ_SUCCESS;

on_error:
    anmed_dealloc_codec(factory, codec);
    return PJMEDIA_CODEC_EFAILED;
}

static pj_status_t anmed_dealloc_codec(pjmedia_vid_codec_factory *factory,
                                       pjmedia_vid_codec *codec )
{
    anmed_codec_data *anmed_data;

    PJ_ASSERT_RETURN(codec, PJ_EINVAL);

    PJ_UNUSED_ARG(factory);

    anmed_data = (anmed_codec_data*) codec->codec_data;
    if (anmed_data->enc) {
        AMediaCodec_stop(anmed_data->enc);
        AMediaCodec_delete(anmed_data->enc);
        anmed_data->enc = NULL;
    }

    if (anmed_data->dec) {
        AMediaCodec_stop(anmed_data->dec);
        AMediaCodec_delete(anmed_data->dec);
        anmed_data->dec = NULL;
    }
    anmed_data->enc_started = PJ_FALSE;
    anmed_data->dec_started = PJ_FALSE;
    pj_pool_release(anmed_data->pool);
    return PJ_SUCCESS;
}

static pj_status_t anmed_codec_init(pjmedia_vid_codec *codec,
                                    pj_pool_t *pool )
{
    PJ_ASSERT_RETURN(codec && pool, PJ_EINVAL);
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    return PJ_SUCCESS;
}

static pj_status_t anmed_codec_open(pjmedia_vid_codec *codec,
                                    pjmedia_vid_codec_param *codec_param )
{
    anmed_codec_data *anmed_data;
    pjmedia_vid_codec_param	*param;
    pjmedia_h264_packetizer_cfg  pktz_cfg;
    pjmedia_vid_codec_h264_fmtp  h264_fmtp;
    pj_status_t status;
    media_status_t am_status;
    AMediaFormat *vid_fmt = NULL;

    PJ_LOG(5,(THIS_FILE, "Opening codec.."));

    anmed_data = (anmed_codec_data*) codec->codec_data;
    anmed_data->prm = pjmedia_vid_codec_param_clone( anmed_data->pool,
                                                     codec_param);
    param = anmed_data->prm;

    /* Parse remote fmtp */
    pj_bzero(&h264_fmtp, sizeof(h264_fmtp));
    status = pjmedia_vid_codec_h264_parse_fmtp(&param->enc_fmtp, &h264_fmtp);
    if (status != PJ_SUCCESS)
	return status;

    TRACE_((THIS_FILE, "OPENING CODEC, ignore fmtp %d",
	    param->ignore_fmtp));
    /* Apply SDP fmtp to format in codec param */
    if (!param->ignore_fmtp) {
	status = pjmedia_vid_codec_h264_apply_fmtp(param);
	if (status != PJ_SUCCESS)
	    return status;
    }
    pj_bzero(&pktz_cfg, sizeof(pktz_cfg));
    pktz_cfg.mtu = param->enc_mtu;
    pktz_cfg.unpack_nal_start = 4;
    /* Packetization mode */
    if (h264_fmtp.packetization_mode == 0)
	pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL;
    else if (h264_fmtp.packetization_mode == 1)
	pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED;
    else
	return PJ_ENOTSUP;

    pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED;
    status = pjmedia_h264_packetizer_create(anmed_data->pool, &pktz_cfg,
                                            &anmed_data->pktz);
    if (status != PJ_SUCCESS)
        return status;

    anmed_data->whole = (param->packing == PJMEDIA_VID_PACKING_WHOLE);

    /*
     * Configure encoder.
     */
    vid_fmt = AMediaFormat_new();
    if (!vid_fmt) {
        return PJ_ENOMEM;
    }
    AMediaFormat_setString(vid_fmt, ANMED_KEY_MIME, ANMED_H264_MIME_TYPE);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_COLOR_FMT, ANMED_COLOR_FMT);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_HEIGHT,
                          param->enc_fmt.det.vid.size.h);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_WIDTH,
                          param->enc_fmt.det.vid.size.w);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_BIT_RATE,
                          param->enc_fmt.det.vid.avg_bps);
    /* Base profile */
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_PROFILE, 1);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_IFR_INTTERVAL, KEYFRAME_INTERVAL);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_FRAME_RATE,
                          (param->enc_fmt.det.vid.fps.num /
    			   param->enc_fmt.det.vid.fps.denum));
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_LOW_LATENCY, 1);

    /* Configure and start encoder. */
    am_status = AMediaCodec_configure(anmed_data->enc, vid_fmt, NULL, NULL,
                                      AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(vid_fmt);
    if (am_status != AMEDIA_OK) {
        PJ_LOG(4,(THIS_FILE, "Encoder configure failed, status=%d", am_status));
        return PJMEDIA_CODEC_EFAILED;
    }
    am_status = AMediaCodec_start(anmed_data->enc);
    if (am_status != AMEDIA_OK) {
        PJ_LOG(4,(THIS_FILE, "Encoder start failed, status=%d", am_status));
        return PJMEDIA_CODEC_EFAILED;
    }
    anmed_data->enc_started = PJ_TRUE;
    PJ_LOG(4,(THIS_FILE, "Encoder started"));

    /* If available, use the "sprop-parameter-sets" fmtp from remote SDP
     * to create the decoder.
     */
    if (h264_fmtp.sprop_param_sets_len) {
    	const pj_uint8_t start_code[3] = {0, 0, 1};
    	const int code_size = PJ_ARRAY_SIZE(start_code);
    	const pj_uint8_t med_start_code[4] = {0, 0, 0, 1};
    	const int med_code_size = PJ_ARRAY_SIZE(med_start_code);
    	unsigned i, j;

    	for (i = h264_fmtp.sprop_param_sets_len-code_size; i >= code_size;
    	     i--)
    	{
    	    pj_bool_t found = PJ_TRUE;
    	    for (j = 0; j < code_size; j++) {
    	        if (h264_fmtp.sprop_param_sets[i+j] != start_code[j]) {
    	            found = PJ_FALSE;
    	            break;
    	        }
    	    }
    	}

    	if (i >= code_size) {
    	    unsigned buf_size;

 	    pj_memcpy(anmed_data->dec_sps_buf, med_start_code,
 		      med_code_size);
 	    anmed_data->dec_sps_len = med_code_size;
 	    buf_size = i - code_size;
 	    pj_memcpy(anmed_data->dec_sps_buf+anmed_data->dec_sps_len,
 	    	      &h264_fmtp.sprop_param_sets[code_size],
 	    	      buf_size);
 	    anmed_data->dec_sps_len += buf_size;
 	    pj_memcpy(anmed_data->dec_pps_buf, med_start_code, med_code_size);
 	    anmed_data->dec_pps_len = med_code_size;
 	    buf_size = h264_fmtp.sprop_param_sets_len - code_size - i;
 	    pj_memcpy(anmed_data->dec_pps_buf+anmed_data->dec_pps_len,
 	              &h264_fmtp.sprop_param_sets[i + code_size],
 	    	      buf_size);
 	    anmed_data->dec_pps_len += buf_size;
    	}
    }

    status = configure_decoder(anmed_data);

    anmed_data->dec_buf_size = (MAX_RX_WIDTH * MAX_RX_HEIGHT * 3 >> 1) +
			       (MAX_RX_WIDTH);
    anmed_data->dec_buf = (pj_uint8_t*)pj_pool_alloc(anmed_data->pool,
                                                     anmed_data->dec_buf_size);

    /* Need to update param back after values are negotiated */
    pj_memcpy(codec_param, param, sizeof(*codec_param));

    return PJ_SUCCESS;
}

static pj_status_t anmed_codec_close(pjmedia_vid_codec *codec)
{
    PJ_ASSERT_RETURN(codec, PJ_EINVAL);
    PJ_UNUSED_ARG(codec);
    return PJ_SUCCESS;
}

static pj_status_t anmed_codec_modify(pjmedia_vid_codec *codec,
                                      const pjmedia_vid_codec_param *param)
{
    PJ_ASSERT_RETURN(codec && param, PJ_EINVAL);
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(param);
    return PJ_EINVALIDOP;
}

static pj_status_t anmed_codec_get_param(pjmedia_vid_codec *codec,
                                         pjmedia_vid_codec_param *param)
{
    struct anmed_codec_data *anmed_data;

    PJ_ASSERT_RETURN(codec && param, PJ_EINVAL);

    anmed_data = (anmed_codec_data*) codec->codec_data;
    pj_memcpy(param, anmed_data->prm, sizeof(*param));

    return PJ_SUCCESS;
}

static pj_status_t anmed_codec_encode_begin(pjmedia_vid_codec *codec,
                                            const pjmedia_vid_encode_opt *opt,
                                            const pjmedia_frame *input,
                                            unsigned out_size,
                                            pjmedia_frame *output,
                                            pj_bool_t *has_more)
{
    struct anmed_codec_data *anmed_data;
    unsigned i;
    pj_ssize_t buf_idx;

    PJ_ASSERT_RETURN(codec && input && out_size && output && has_more,
                     PJ_EINVAL);

    anmed_data = (anmed_codec_data*) codec->codec_data;

    if (!anmed_data->enc_started) {
        PJ_LOG(4,(THIS_FILE, "Codec not started"));
        return PJMEDIA_CODEC_EFAILED;
    }

    if (opt && opt->force_keyframe) {
#if __ANDROID_API__ >=26
	AMediaFormat *vid_fmt = NULL;
	media_status_t am_status;

	vid_fmt = AMediaFormat_new();
	if (!vid_fmt) {
	    return PJMEDIA_CODEC_EFAILED;
	}
	AMediaFormat_setInt32(vid_fmt, ANMED_KEY_REQUEST_SYNCF, 0);
	am_status = AMediaCodec_setParameters(anmed_data->enc, vid_fmt);
	AMediaFormat_delete(vid_fmt);
#else
	PJ_LOG(4, (THIS_FILE, "Encoder cannot be forced to send keyframe"));
#endif
    }

    for (i = 0; i < CODEC_WAIT_RETRY; ++i) {
        buf_idx = AMediaCodec_dequeueInputBuffer(anmed_data->enc,
                                                 CODEC_DEQUEUE_TIMEOUT);
        if (buf_idx >= 0) {
            media_status_t am_status;
            pj_size_t output_size;
            pj_uint8_t *input_buf = AMediaCodec_getInputBuffer(anmed_data->enc,
                                                        buf_idx, &output_size);
            if (input_buf && output_size >= input->size) {
                pj_memcpy(input_buf, input->buf, input->size);
                am_status = AMediaCodec_queueInputBuffer(anmed_data->enc,
                                        buf_idx,
                                        0,
                                        input->size,
                                        0,
                                        0);
                if (am_status != AMEDIA_OK) {
                    TRACE_((THIS_FILE, "Encoder queueInputBuffer return %d",
                            am_status));
                    goto on_return;
                }
                break;
            } else {
        	if (!input_buf) {
		    PJ_LOG(4,(THIS_FILE, "Encoder getInputBuffer "
			                 "returns no input buff"));
        	} else {
		    PJ_LOG(4,(THIS_FILE, "Encoder getInputBuffer "
			                 "size: %d, expecting %d.",
			      input_buf, output_size, input->size));
        	}
                goto on_return;
            }
        } else {
            TRACE_((THIS_FILE, "Encoder dequeueInputBuffer timeout[%d]",
        	    i+1));
            pj_thread_sleep(CODEC_THREAD_WAIT);
        }
    }

    if (i == CODEC_WAIT_RETRY) {
        PJ_LOG(4, (THIS_FILE, "Encoder dequeueInputBuffer failed"));
        goto on_return;
    }

    for (i = 0; i < CODEC_WAIT_RETRY; ++i) {
	buf_idx = AMediaCodec_dequeueOutputBuffer(anmed_data->enc,
						  &anmed_data->enc_buf_info,
						  CODEC_DEQUEUE_TIMEOUT);
	if (buf_idx == -1) {
            TRACE_((THIS_FILE, "Encoder dequeueOutputBuffer timeout[%d]",
        	    i+1));
            pj_thread_sleep(CODEC_THREAD_WAIT);
	} else {
	    break;
	}
    }

    if (i == CODEC_WAIT_RETRY) {
        PJ_LOG(4, (THIS_FILE, "Encoder dequeueOutputBuffer failed"));
        goto on_return;
    }

    if (buf_idx >= 0) {
        pj_size_t output_size;
        pj_uint8_t *output_buf = AMediaCodec_getOutputBuffer(anmed_data->enc,
                                                             buf_idx,
                                                             &output_size);
        if (!output_buf) {
            PJ_LOG(4, (THIS_FILE, "Encoder output_buf:0x%x "
		       "get output buffer size: %d, offset %d, flags %d",
		       output_buf, anmed_data->enc_buf_info.size,
		       anmed_data->enc_buf_info.offset));
            goto on_return;
        }
        anmed_data->enc_frame_size = anmed_data->enc_processed = 0;
        anmed_data->enc_frame_whole = output_buf;
        anmed_data->enc_output_buf_idx = buf_idx;

        if (anmed_data->enc_buf_info.flags & AVC_FRM_TYPE_SPS_PPS)
        {
            /*
             * Config data or SPS+PPS. Update the SPS and PPS buffer,
             * this will be sent later when sending Keyframe.
             */
            anmed_data->enc_sps_pps_len = PJ_MIN(anmed_data->enc_buf_info.size,
        				   sizeof(anmed_data->enc_sps_pps_buf));
            pj_memcpy(anmed_data->enc_sps_pps_buf, output_buf,
        	      anmed_data->enc_sps_pps_len);

            goto on_return;
        }
        if(anmed_data->enc_buf_info.flags & AVC_FRM_TYPE_KEYFRAME)
        {
            //TRACE_((THIS_FILE, "KEYFRAME DETECTED"));
            output->bit_info |= PJMEDIA_VID_FRM_KEYFRAME;
            anmed_data->enc_sps_pps_ex = PJ_TRUE;
        } else {
            anmed_data->enc_sps_pps_ex = PJ_FALSE;
        }

        if (anmed_data->whole) {
            unsigned payload_size = 0;
            unsigned start_data = 0;

            *has_more = PJ_FALSE;

            if(anmed_data->enc_buf_info.flags & AVC_FRM_TYPE_KEYFRAME)
            {
        	start_data = anmed_data->enc_sps_pps_len;
                pj_memcpy(output->buf, anmed_data->enc_sps_pps_buf,
                	anmed_data->enc_sps_pps_len);
            }
            payload_size = anmed_data->enc_buf_info.size + start_data;

            if (payload_size > out_size)
                return PJMEDIA_CODEC_EFRMTOOSHORT;

            output->type = PJMEDIA_FRAME_TYPE_VIDEO;
            output->size = payload_size;
            output->timestamp = input->timestamp;
            pj_memcpy((pj_uint8_t*)output->buf+start_data,
        	      anmed_data->enc_frame_whole,
        	      anmed_data->enc_buf_info.size);

            return PJ_SUCCESS;
        }
    } else {
        TRACE_((THIS_FILE, "Encoder dequeueOutputBuffer return %d index",
                buf_idx));
        goto on_return;
    }

    return anmed_codec_encode_more(codec, out_size, output, has_more);

on_return:
    output->size = 0;
    output->type = PJMEDIA_FRAME_TYPE_NONE;
    *has_more = PJ_FALSE;
    return PJ_SUCCESS;
}

static pj_status_t anmed_codec_encode_more(pjmedia_vid_codec *codec,
                                           unsigned out_size,
                                           pjmedia_frame *output,
                                           pj_bool_t *has_more)
{
    struct anmed_codec_data *anmed_data;
    const pj_uint8_t *payload;
    pj_size_t payload_len;
    pj_status_t status;
    pj_uint8_t *data_buf;

    PJ_ASSERT_RETURN(codec && out_size && output && has_more, PJ_EINVAL);

    anmed_data = (anmed_codec_data*) codec->codec_data;

    if (anmed_data->enc_processed < anmed_data->enc_frame_size) {
	if (anmed_data->enc_sps_pps_ex) {
	    data_buf = anmed_data->enc_sps_pps_buf;
	} else {
	    data_buf = anmed_data->enc_frame_whole;
	}
	/* We have outstanding frame in packetizer */
	status = pjmedia_h264_packetize(anmed_data->pktz,
	                                data_buf,
	                                anmed_data->enc_frame_size,
	                                &anmed_data->enc_processed,
	                                &payload, &payload_len);
	if (status != PJ_SUCCESS) {
	    /* Reset */
	    anmed_data->enc_frame_size = anmed_data->enc_processed = 0;
	    *has_more = (anmed_data->enc_processed <
                         anmed_data->enc_frame_size);

	    if (!(*has_more)) {
		AMediaCodec_releaseOutputBuffer(anmed_data->enc,
						anmed_data->enc_output_buf_idx,
						0);
	    }
	    PJ_PERROR(3,(THIS_FILE, status, "pjmedia_h264_packetize() error"));
	    return status;
	}

	PJ_ASSERT_RETURN(payload_len <= out_size, PJMEDIA_CODEC_EFRMTOOSHORT);

        output->type = PJMEDIA_FRAME_TYPE_VIDEO;
        pj_memcpy(output->buf, payload, payload_len);
        output->size = payload_len;

        if (anmed_data->enc_processed >= anmed_data->enc_frame_size) {
            if (anmed_data->enc_sps_pps_ex) {
        	*has_more = PJ_TRUE;
        	anmed_data->enc_sps_pps_ex = PJ_FALSE;
        	anmed_data->enc_processed = 0;
        	anmed_data->enc_frame_size = 0;
            } else {
        	*has_more = PJ_FALSE;
            }
        } else {
            *has_more = PJ_TRUE;
        }
        if (!(*has_more)) {
            AMediaCodec_releaseOutputBuffer(anmed_data->enc,
    				            anmed_data->enc_output_buf_idx,
    				            0);
        }
        return PJ_SUCCESS;
    }

    anmed_data->enc_processed = 0;
    if (anmed_data->enc_sps_pps_ex) {
	anmed_data->enc_frame_size = anmed_data->enc_sps_pps_len;
	data_buf = anmed_data->enc_sps_pps_buf;
    } else {
	anmed_data->enc_frame_size = anmed_data->enc_buf_info.size;
	data_buf = anmed_data->enc_frame_whole;
    }
    status = pjmedia_h264_packetize(anmed_data->pktz,
				    data_buf,
				    anmed_data->enc_frame_size,
				    &anmed_data->enc_processed,
				    &payload, &payload_len);

    if (status != PJ_SUCCESS) {
	/* Reset */
	anmed_data->enc_frame_size = anmed_data->enc_processed = 0;
	*has_more = PJ_FALSE;
        AMediaCodec_releaseOutputBuffer(anmed_data->enc,
				            anmed_data->enc_output_buf_idx,
				            0);

	PJ_PERROR(3,(THIS_FILE, status, "pjmedia_h264_packetize() error [2]"));
	return status;
    }

    PJ_ASSERT_RETURN(payload_len <= out_size, PJMEDIA_CODEC_EFRMTOOSHORT);

    output->type = PJMEDIA_FRAME_TYPE_VIDEO;
    pj_memcpy(output->buf, payload, payload_len);
    output->size = payload_len;

    *has_more = (anmed_data->enc_processed < anmed_data->enc_frame_size);
    if (!(*has_more)) {
	AMediaCodec_releaseOutputBuffer(anmed_data->enc,
				        anmed_data->enc_output_buf_idx,
				        0);
    }

    return PJ_SUCCESS;
}

static int write_yuv(pj_uint8_t *buf,
                     unsigned dst_len,
                     unsigned char* input,
                     int stride_len,
                     int iWidth,
                     int iHeight)
{
    unsigned req_size;
    pj_uint8_t *dst = buf;
    pj_uint8_t *max = dst + dst_len;
    int   i;
    unsigned char*  pPtr = NULL;

    req_size = (iWidth * iHeight) + (iWidth / 2 * iHeight / 2) +
	       (iWidth / 2 * iHeight / 2);
    if (dst_len < req_size)
	return -1;

    pPtr = input;
    for (i = 0; i < iHeight && (dst + iWidth < max); i++) {
	pj_memcpy(dst, pPtr, iWidth);
	pPtr += stride_len;
	dst += iWidth;
    }

    if (i < iHeight)
	return -1;

    iHeight = iHeight / 2;
    iWidth = iWidth / 2;
    for (i = 0; i < iHeight && (dst + iWidth <= max); i++) {
	pj_memcpy(dst, pPtr, iWidth);
	pPtr += stride_len/2;
	dst += iWidth;
    }

    if (i < iHeight)
	return -1;

    for (i = 0; i < iHeight && (dst + iWidth <= max); i++) {
	pj_memcpy(dst, pPtr, iWidth);
	pPtr += stride_len/2;
	dst += iWidth;
    }

    if (i < iHeight)
	return -1;

    return dst - buf;
}


static pj_status_t anmed_codec_decode(pjmedia_vid_codec *codec,
                                      pj_size_t count,
                                      pjmedia_frame packets[],
                                      unsigned out_size,
                                      pjmedia_frame *output)
{
    struct anmed_codec_data *anmed_data;
    const pj_uint8_t start_code[] = { 0, 0, 0, 1 };
    const int code_size = PJ_ARRAY_SIZE(start_code);
    unsigned buf_pos, whole_len = 0;
    unsigned i, frm_cnt;
    pj_status_t status;
    pj_ssize_t buf_idx;

    PJ_ASSERT_RETURN(codec && count && packets && out_size && output,
                     PJ_EINVAL);
    PJ_ASSERT_RETURN(output->buf, PJ_EINVAL);

    anmed_data = (anmed_codec_data*) codec->codec_data;

    if (!anmed_data->dec_started) {
        PJ_LOG(4, (THIS_FILE, "Decoder not started!!"));
        goto on_return;
    }
    /*
     * Step 1: unpacketize the packets/frames
     */
    whole_len = 0;
    if (anmed_data->whole) {
	for (i=0; i<count; ++i) {
	    if (whole_len + packets[i].size > anmed_data->dec_buf_size) {
		PJ_LOG(4,(THIS_FILE, "Decoding buffer overflow [1]"));
		return PJMEDIA_CODEC_EFRMTOOSHORT;
	    }

	    pj_memcpy( anmed_data->dec_buf + whole_len,
	               (pj_uint8_t*)packets[i].buf,
	               packets[i].size);
	    whole_len += packets[i].size;
	}

    } else {
	for (i=0; i<count; ++i) {

	    if (whole_len + packets[i].size + code_size >
                anmed_data->dec_buf_size)
	    {
		PJ_LOG(4,(THIS_FILE, "Decoding buffer overflow [1]"));
		return PJMEDIA_CODEC_EFRMTOOSHORT;
	    }

	    status = pjmedia_h264_unpacketize( anmed_data->pktz,
					       (pj_uint8_t*)packets[i].buf,
					       packets[i].size,
					       anmed_data->dec_buf,
					       anmed_data->dec_buf_size,
					       &whole_len);
	    if (status != PJ_SUCCESS) {
		PJ_PERROR(4,(THIS_FILE, status, "Unpacketize error"));
		continue;
	    }
	}
    }

    if (whole_len + code_size > anmed_data->dec_buf_size ||
    	whole_len <= code_size + 1)
    {
	PJ_LOG(4,(THIS_FILE, "Decoding buffer overflow or unpacketize error "
			     "size: %d, buffer: %d", whole_len,
			     anmed_data->dec_buf_size));
	return PJMEDIA_CODEC_EFRMTOOSHORT;
    }

    /* Dummy NAL sentinel */
    pj_memcpy(anmed_data->dec_buf + whole_len, start_code, code_size);

    /*
     * Step 2: parse the individual NAL and give to decoder
     */
    buf_pos = 0;
    for ( frm_cnt=0; ; ++frm_cnt) {
	uint32_t frm_size, nalu_type;
	unsigned char *start;

	for (i = code_size - 1; buf_pos + i < whole_len; i++) {
	    if (anmed_data->dec_buf[buf_pos + i] == 0 &&
		anmed_data->dec_buf[buf_pos + i + 1] == 0 &&
		anmed_data->dec_buf[buf_pos + i + 2] == 0 &&
		anmed_data->dec_buf[buf_pos + i + 3] == 1)
	    {
		break;
	    }
	}

	frm_size = i;
	start = anmed_data->dec_buf + buf_pos;
	nalu_type = (start[code_size] & 0x1F);

	for (i = 0; i < CODEC_WAIT_RETRY; ++i) {
	    buf_idx = AMediaCodec_dequeueInputBuffer(anmed_data->dec,
						     CODEC_DEQUEUE_TIMEOUT);
	    if (buf_idx >= 0) {
		pj_size_t output_size;
		pj_uint8_t *input_buf =
			    AMediaCodec_getInputBuffer(anmed_data->dec,
						       buf_idx,
						       &output_size);

		if (input_buf && output_size >= frm_size) {
		    int buf_flag;
		    media_status_t am_status;

		    pj_memcpy(input_buf, start, frm_size);
		    switch (nalu_type) {
		    case 7:
		    case 8:
			buf_flag = 2;
			break;
		    case 5:
			buf_flag = 1;
			break;
		    default:
			buf_flag = 0;
		    }
		    am_status =
			      AMediaCodec_queueInputBuffer(anmed_data->dec,
							   buf_idx,
							   0,
							   frm_size,
							   0,
							   buf_flag);
		    if (am_status != AMEDIA_OK) {
			PJ_LOG(4,(THIS_FILE, "Decoder queueInputBuffer "
					   "err return %d", am_status));
			goto on_return;
		    }
		    break;
		} else {
		    if (!input_buf) {
			PJ_LOG(4,(THIS_FILE, "Decoder getInputBuffer "
				  "returns no input buffer"));
		    } else {
			PJ_LOG(4,(THIS_FILE, "Decoder getInputBuffer "
				  "size: %d, expecting %d.",
				  input_buf, output_size, frm_size));
		    }
		    goto on_return;
		}
	    } else {
		TRACE_((THIS_FILE, "Decoder dequeueInputBuffer timeout[%d]",
			i+1));
		pj_thread_sleep(CODEC_THREAD_WAIT);
	    }
	}

	if (i == CODEC_WAIT_RETRY) {
	    PJ_LOG(4,(THIS_FILE, "Decoder dequeueInputBuffer failed"));
	    goto on_return;
	}

	if (buf_pos + frm_size >= whole_len)
	    break;

	buf_pos += frm_size;
    }

    for (i = 0; i < CODEC_WAIT_RETRY; ++i) {
	buf_idx = AMediaCodec_dequeueOutputBuffer(anmed_data->dec,
						  &anmed_data->dec_buf_info,
						  CODEC_DEQUEUE_TIMEOUT);
	if (buf_idx == -1) {
	    TRACE_((THIS_FILE, "Decoder dequeueOutputBuffer timeout[%d]", i+1));
	    pj_thread_sleep(CODEC_THREAD_WAIT);
	} else {
	    break;
	}
    }

    if (i == CODEC_WAIT_RETRY) {
	PJ_LOG(4,(THIS_FILE, "Decoder dequeueOutputBuffer failed"));
	goto on_return;
    }

    if (buf_idx >= 0) {
	int len;
	pj_size_t output_size;
	pj_uint8_t *output_buf = AMediaCodec_getOutputBuffer(anmed_data->dec,
							     buf_idx,
							     &output_size);
	if (!output_buf) {
	    PJ_LOG(4, (THIS_FILE, "Decoder cannot get output buffer."));
	    goto on_return;
	}

	len = write_yuv((pj_uint8_t *)output->buf,
			anmed_data->dec_buf_info.size,
			output_buf,
			anmed_data->dec_stride_len,
			anmed_data->prm->dec_fmt.det.vid.size.w,
			anmed_data->prm->dec_fmt.det.vid.size.h);

	AMediaCodec_releaseOutputBuffer(anmed_data->dec,
					buf_idx,
					0);
	if (len > 0) {
	    output->type = PJMEDIA_FRAME_TYPE_VIDEO;
	    output->size = len;
	    output->timestamp = packets[0].timestamp;
	} else {
	    output->size = 0;
	    return PJMEDIA_CODEC_EFRMTOOSHORT;
	}
    } else if (buf_idx == -2) {
	int width, height, color_fmt, stride;

	/* Format change. */
	AMediaFormat *vid_fmt = AMediaCodec_getOutputFormat(anmed_data->dec);

	AMediaFormat_getInt32(vid_fmt, ANMED_KEY_WIDTH, &width);
	AMediaFormat_getInt32(vid_fmt, ANMED_KEY_HEIGHT, &height);
	AMediaFormat_getInt32(vid_fmt, ANMED_KEY_COLOR_FMT, &color_fmt);
	AMediaFormat_getInt32(vid_fmt, ANMED_KEY_STRIDE, &stride);
	TRACE_((THIS_FILE, "Decoder detect new width %d, height %d, "
		"color_fmt 0x%X, stride %d buf_size %d",
		width, height, color_fmt, stride,
		anmed_data->dec_buf_info.size));

	AMediaFormat_delete(vid_fmt);
	anmed_data->dec_stride_len = stride;
	if (width != anmed_data->prm->dec_fmt.det.vid.size.w ||
	    height != anmed_data->prm->dec_fmt.det.vid.size.h)
	{
	    pjmedia_event event;

	    anmed_data->prm->dec_fmt.det.vid.size.w = width;
	    anmed_data->prm->dec_fmt.det.vid.size.h = height;

	    PJ_LOG(4,(THIS_FILE, "Frame size changed to %dx%d",
		      anmed_data->prm->dec_fmt.det.vid.size.w,
		      anmed_data->prm->dec_fmt.det.vid.size.h));

	    /* Broadcast format changed event */
	    pjmedia_event_init(&event, PJMEDIA_EVENT_FMT_CHANGED,
			       &output->timestamp, codec);
	    event.data.fmt_changed.dir = PJMEDIA_DIR_DECODING;
	    pjmedia_format_copy(&event.data.fmt_changed.new_fmt,
				&anmed_data->prm->dec_fmt);
	    pjmedia_event_publish(NULL, codec, &event,
				  PJMEDIA_EVENT_PUBLISH_DEFAULT);
	}
	goto on_return;
    } else {
	PJ_LOG(4, (THIS_FILE, "Decoder dequeueOutputBuffer return %d index",
	       buf_idx));
	goto on_return;
    }

    return PJ_SUCCESS;

on_return:
    output->type = PJMEDIA_FRAME_TYPE_NONE;
    output->size = 0;
    output->timestamp = packets[0].timestamp;
    return PJ_SUCCESS;
}

#endif	/* PJMEDIA_HAS_ANDROID_MEDIACODEC */
