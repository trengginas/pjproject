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

//#if defined(PJMEDIA_HAS_ANDROID_MEDIACODEC) && \
//            PJMEDIA_HAS_ANDROID_MEDIACODEC != 0 && \
//    defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

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
#define ANMED_H264_CODEC_TYPE   "video/avc"
#define ANMED_KEY_COLOR_FMT     "color-format"
#define ANMED_KEY_WIDTH         "width"
#define ANMED_KEY_HEIGHT        "height"
#define ANMED_KEY_BIT_RATE      "bitrate"
#define ANMED_KEY_PROFILE       "profile"
#define ANMED_KEY_FRAME_RATE    "frame-rate"
#define ANMED_KEY_IFR_INTTERVAL "i-frame-interval"
#define ANMED_COLOR_FMT         0x00000013
#define ANMED_QUEUE_TIMEOUT     2000*100

#define DEFAULT_WIDTH		352
#define DEFAULT_HEIGHT	        288

#define DEFAULT_FPS		15
#define DEFAULT_AVG_BITRATE	256000
#define DEFAULT_MAX_BITRATE	256000

#define MAX_RX_WIDTH		1200
#define MAX_RX_HEIGHT		800

/* Maximum duration from one key frame to the next (in seconds). */
#define KEYFRAME_INTERVAL	5


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


typedef struct anmed_codec_data
{
    pj_pool_t			*pool;
    pjmedia_vid_codec_param	*prm;
    pj_bool_t			 whole;
    pjmedia_h264_packetizer	*pktz;
    pj_bool_t                    codec_started;

    /* Encoder state */
    AMediaCodec                 *enc;
    unsigned		 	 enc_input_size;
    pj_uint8_t			*enc_frame_whole;
    unsigned			 enc_frame_size;
    unsigned			 enc_processed;

    /* Decoder state */
    AMediaCodec                 *dec;
    pj_uint8_t			*dec_buf;
    unsigned			 dec_buf_size;
} anmed_codec_data;

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
    int log_level = 5;

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

    PJ_LOG(4,(THIS_FILE, "alloc_codec .. creating encoder"));
    anmed_data->enc = AMediaCodec_createEncoderByType(ANMED_H264_CODEC_TYPE);
    if (!anmed_data->enc) {
        PJ_LOG(4,(THIS_FILE, "alloc_codec .. failed creating encoder"));
        goto on_error;
    }

    PJ_LOG(4,(THIS_FILE, "alloc_codec .. creating decoder"));
    anmed_data->dec = AMediaCodec_createDecoderByType(ANMED_H264_CODEC_TYPE);
    if (!anmed_data->dec) {
        PJ_LOG(4,(THIS_FILE, "alloc_codec .. failed creating decoder"));
        goto on_error;
    }

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
    anmed_data->codec_started = PJ_FALSE;
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

    /* Apply SDP fmtp to format in codec param */
    if (!param->ignore_fmtp) {
	status = pjmedia_vid_codec_h264_apply_fmtp(param);
	if (status != PJ_SUCCESS)
	    return status;
    }
    pj_bzero(&pktz_cfg, sizeof(pktz_cfg));
    pktz_cfg.mtu = param->enc_mtu;
    /* Packetization mode */
    if (h264_fmtp.packetization_mode == 0)
	pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL;
    else if (h264_fmtp.packetization_mode == 1)
	pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED;
    else
	return PJ_ENOTSUP;

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
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_COLOR_FMT, ANMED_COLOR_FMT);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_BIT_RATE,
                          param->enc_fmt.det.vid.avg_bps);
    /* Base profile */
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_PROFILE, 1);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_IFR_INTTERVAL, KEYFRAME_INTERVAL);
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_FRAME_RATE,
                          (param->enc_fmt.det.vid.fps.num /
    			   param->enc_fmt.det.vid.fps.denum));

    /* Configure as encoder. */
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

    /*
     * Configure decoder.
     */
    vid_fmt = AMediaFormat_new();
    if (!vid_fmt) {
        return PJ_ENOMEM;
    }
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_FRAME_RATE,
                          (param->enc_fmt.det.vid.fps.num /
                           param->enc_fmt.det.vid.fps.denum));
    AMediaFormat_setInt32(vid_fmt, ANMED_KEY_COLOR_FMT, ANMED_COLOR_FMT);

    am_status = AMediaCodec_configure(anmed_data->dec, vid_fmt, NULL, NULL, 0);
    AMediaFormat_delete(vid_fmt);
    if (am_status != AMEDIA_OK) {
        PJ_LOG(4,(THIS_FILE, "Decoder configure failed, status=%d", am_status));
        return PJMEDIA_CODEC_EFAILED;
    }
    am_status = AMediaCodec_start(anmed_data->dec);
    if (am_status != AMEDIA_OK) {
        PJ_LOG(4,(THIS_FILE, "Decoder start failed, status=%d", am_status));
        return PJMEDIA_CODEC_EFAILED;
    }

    anmed_data->dec_buf_size = (MAX_RX_WIDTH * MAX_RX_HEIGHT * 3 >> 1) +
			       (MAX_RX_WIDTH);
    anmed_data->dec_buf = (pj_uint8_t*)pj_pool_alloc(anmed_data->pool,
                                                     anmed_data->dec_buf_size);

    PJ_LOG(4,(THIS_FILE, "AMediaCodec configure done"));
    anmed_data->codec_started = PJ_TRUE;
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

    PJ_ASSERT_RETURN(codec && input && out_size && output && has_more,
                     PJ_EINVAL);

    return PJ_SUCCESS;
}


static pj_status_t anmed_codec_encode_more(pjmedia_vid_codec *codec,
                                           unsigned out_size,
                                           pjmedia_frame *output,
                                           pj_bool_t *has_more)
{
    return PJ_SUCCESS;
}

static int write_yuv(pj_uint8_t *buf,
                     unsigned dst_len,
                     unsigned char* pData[3],
                     int iStride[2],
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

    pPtr = pData[0];
    for (i = 0; i < iHeight && (dst + iWidth < max); i++) {
	pj_memcpy(dst, pPtr, iWidth);
	pPtr += iStride[0];
	dst += iWidth;
    }

    if (i < iHeight)
	return -1;

    iHeight = iHeight / 2;
    iWidth = iWidth / 2;
    pPtr = pData[1];
    for (i = 0; i < iHeight && (dst + iWidth <= max); i++) {
	pj_memcpy(dst, pPtr, iWidth);
	pPtr += iStride[1];
	dst += iWidth;
    }

    if (i < iHeight)
	return -1;

    pPtr = pData[2];
    for (i = 0; i < iHeight && (dst + iWidth <= max); i++) {
	pj_memcpy(dst, pPtr, iWidth);
	pPtr += iStride[1];
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
    unsigned buf_pos, whole_len = 0;
    unsigned i, frm_cnt;
    pj_status_t status;
    int wait_times = 5;
    AMediaCodec *dec;

    PJ_ASSERT_RETURN(codec && count && packets && out_size && output,
                     PJ_EINVAL);
    PJ_ASSERT_RETURN(output->buf, PJ_EINVAL);

    anmed_data = (anmed_codec_data*) codec->codec_data;
    dec = anmed_data->dec;
 //   /*
 //    * Step 1: unpacketize the packets/frames
 //    */
 //   whole_len = 0;
 //   if (anmed_data->whole) {
	//for (i=0; i<count; ++i) {
	//    if (whole_len + packets[i].size > anmed_data->dec_buf_size) {
	//	PJ_LOG(4,(THIS_FILE, "Decoding buffer overflow"));
	//	status = PJMEDIA_CODEC_EFRMTOOSHORT;
	//	break;
	//    }

	//    pj_memcpy( anmed_data->dec_buf + whole_len,
	//               (pj_uint8_t*)packets[i].buf,
	//               packets[i].size);
	//    whole_len += packets[i].size;
	//}

 //   } else {
	//for (i=0; i<count; ++i) {
	//    if (whole_len + packets[i].size + code_size >
	//	anmed_data->dec_buf_size)
	//    {
	//	PJ_LOG(4,(THIS_FILE, "Decoding buffer overflow [1]"));
	//	status = PJMEDIA_CODEC_EFRMTOOSHORT;
	//	break;
	//    }

	//    status = pjmedia_h264_unpacketize( anmed_data->pktz,
	//				       (pj_uint8_t*)packets[i].buf,
	//				       packets[i].size,
	//				       anmed_data->dec_buf,
	//				       anmed_data->dec_buf_size,
	//				       &whole_len);
	//    if (status != PJ_SUCCESS) {
	//	PJ_PERROR(4,(THIS_FILE, status, "Unpacketize error"));
	//	continue;
	//    }
	//}
 //   }

 //   /* Dummy NAL sentinel */
 //   pj_memcpy( anmed_data->dec_buf + whole_len, nal_start, sizeof(nal_start));

 //   /*
 //    * Step 2: parse the individual NAL and give to decoder
 //    */
 //   buf_pos = 0;
 //   for (frm_cnt = 0; ; ++frm_cnt) {

 //   }

 //   do {
 //       pj_size_t idx = AMediaCodec_dequeueInputBuffer(dec, 
 //                                                      ANMED_QUEUE_TIMEOUT);
 //       if (idx >= 0) {
 //           pj_size_t out_size;
 //           uint8_t *inputBuf = AMediaCodec_getInputBuffer(dec, idx,
 //                                                          &out_size);
 //           if (inputBuf != NULL && anmed_data->dec_buf_size <= outSize) {
 //               memcpy(inputBuf, frame, frameLen);
 //               media_status_t status = AMediaCodec_queueInputBuffer(dec,
 //                                                 bufIdx, 0, frameLen, 2000, 0);
 //           } else {

 //           }
 //           break;
 //       } else {
 //           pj_thread_sleep(10);
 //       }
 //   } while (--times > 0)


    return PJ_SUCCESS;
}

//#endif	/* PJMEDIA_HAS_ANDROID_MEDIACODEC */
