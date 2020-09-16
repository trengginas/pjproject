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
#include <pjmedia-codec/and_aud_mediacodec.h>
#include <pjmedia-codec/amr_sdp_match.h>
#include <pjmedia/codec.h>
#include <pjmedia/errno.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/plc.h>
#include <pjmedia/port.h>
#include <pjmedia/silencedet.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/math.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/os.h>
/*
 * Only build this file if PJMEDIA_HAS_ANDROID_MEDIACODEC != 0
 */
#if defined(PJMEDIA_HAS_ANDROID_MEDIACODEC) && \
            PJMEDIA_HAS_ANDROID_MEDIACODEC != 0

/* Android AMediaCodec: */
#include "media/NdkMediaCodec.h"

#define THIS_FILE  "and_aud_mediacodec.cpp"

#define ANMED_KEY_PCM_ENCODING       "pcm-encoding"
#define ANMED_KEY_CHANNEL_COUNT      "channel-count"
#define ANMED_KEY_SAMPLE_RATE        "sample-rate"
#define ANMED_KEY_BITRATE            "bitrate"
#define ANMED_KEY_MIME               "mime"
#define ANMED_KEY_ENCODER            "encoder"

/* Prototypes for Android MediaCodec codecs factory */
static pj_status_t anmed_test_alloc(pjmedia_codec_factory *factory, 
				    const pjmedia_codec_info *id );
static pj_status_t anmed_default_attr(pjmedia_codec_factory *factory, 
				      const pjmedia_codec_info *id, 
				      pjmedia_codec_param *attr );
static pj_status_t anmed_enum_codecs(pjmedia_codec_factory *factory, 
				     unsigned *count, 
				     pjmedia_codec_info codecs[]);
static pj_status_t anmed_alloc_codec(pjmedia_codec_factory *factory, 
				     const pjmedia_codec_info *id, 
				     pjmedia_codec **p_codec);
static pj_status_t anmed_dealloc_codec(pjmedia_codec_factory *factory, 
				       pjmedia_codec *codec );

/* Prototypes for Android MediaCodec codecs implementation. */
static pj_status_t  anmed_codec_init(pjmedia_codec *codec, 
				     pj_pool_t *pool );
static pj_status_t  anmed_codec_open(pjmedia_codec *codec, 
				     pjmedia_codec_param *attr );
static pj_status_t  anmed_codec_close(pjmedia_codec *codec );
static pj_status_t  anmed_codec_modify(pjmedia_codec *codec, 
				       const pjmedia_codec_param *attr );
static pj_status_t  anmed_codec_parse(pjmedia_codec *codec,
				      void *pkt,
				      pj_size_t pkt_size,
				      const pj_timestamp *ts,
				      unsigned *frame_cnt,
				      pjmedia_frame frames[]);
static pj_status_t  anmed_codec_encode(pjmedia_codec *codec, 
				       const struct pjmedia_frame *input,
				       unsigned output_buf_len, 
				       struct pjmedia_frame *output);
static pj_status_t  anmed_codec_decode(pjmedia_codec *codec, 
				       const struct pjmedia_frame *input,
				       unsigned output_buf_len, 
				       struct pjmedia_frame *output);
static pj_status_t  anmed_codec_recover(pjmedia_codec *codec, 
				        unsigned output_buf_len, 
				        struct pjmedia_frame *output);

/* Definition for Android MediaCodec codecs operations. */
static pjmedia_codec_op anmed_op = 
{
    &anmed_codec_init,
    &anmed_codec_open,
    &anmed_codec_close,
    &anmed_codec_modify,
    &anmed_codec_parse,
    &anmed_codec_encode,
    &anmed_codec_decode,
    &anmed_codec_recover
};

/* Definition for Android MediaCodec codecs factory operations. */
static pjmedia_codec_factory_op anmed_factory_op =
{
    &anmed_test_alloc,
    &anmed_default_attr,
    &anmed_enum_codecs,
    &anmed_alloc_codec,
    &anmed_dealloc_codec,
    &pjmedia_codec_anmed_aud_deinit
};

/* Android MediaCodec codecs factory */
static struct anmed_factory {
    pjmedia_codec_factory    base;
    pjmedia_endpt	    *endpt;
    pj_pool_t		    *pool;
    pj_mutex_t        	    *mutex;
} anmed_factory;

/* Android MediaCodec codecs private data. */
typedef struct anmed_private {
    int			 codec_idx;	    /**< Codec index.		    */
    void		*codec_setting;	    /**< Specific codec setting.    */
    pj_pool_t		*pool;		    /**< Pool for each instance.    */
    AMediaCodec         *enc;               /**< Encoder state.		    */
    AMediaCodec         *dec;               /**< Decoder state.		    */

    pj_uint16_t		 frame_size;	    /**< Bitstream frame size.	    */

    pj_bool_t		 plc_enabled;	    /**< PLC enabled flag.	    */
    pjmedia_plc		*plc;		    /**< PJMEDIA PLC engine, NULL if 
						 codec has internal PLC.    */

    pj_bool_t		 vad_enabled;	    /**< VAD enabled flag.	    */
    pjmedia_silence_det	*vad;		    /**< PJMEDIA VAD engine, NULL if 
						 codec has internal VAD.    */
    pj_timestamp	 last_tx;	    /**< Timestamp of last transmit.*/
} anmed_private_t;

/* CUSTOM CALLBACKS */

/* This callback is useful for translating RTP frame into USC frame, e.g:
 * reassigning frame attributes, reorder bitstream. Default behaviour of
 * the translation is just setting the USC frame buffer & its size as 
 * specified in RTP frame, setting USC frame frametype to 0, setting bitrate
 * of USC frame to bitrate info of codec_data. Implement this callback when 
 * the default behaviour is unapplicable.
 */
//typedef void (*predecode_cb)(anmed_private_t *codec_data,
//			     const pjmedia_frame *rtp_frame,
//			     USC_Bitstream *usc_frame);

/* Parse frames from a packet. Default behaviour of frame parsing is 
 * just separating frames based on calculating frame length derived 
 * from bitrate. Implement this callback when the default behaviour is 
 * unapplicable.
 */
//typedef pj_status_t (*parse_cb)(anmed_private_t *codec_data, void *pkt,
//				pj_size_t pkt_size, const pj_timestamp *ts,
//				unsigned *frame_cnt, pjmedia_frame frames[]);

/* Pack frames into a packet. Default behaviour of packing frames is 
 * just stacking the frames with octet aligned without adding any 
 * payload header. Implement this callback when the default behaviour is
 * unapplicable.
 */
//typedef pj_status_t (*pack_cb)(anmed_private_t *codec_data, void *pkt,
//			       pj_size_t *pkt_size, pj_size_t max_pkt_size);



/* Custom callback implementations. */
//static void predecode_amr(anmed_private_t *codec_data,
//			  const pjmedia_frame *rtp_frame,
//			  USC_Bitstream *usc_frame);
static pj_status_t parse_amr(anmed_private_t *codec_data, void *pkt, 
			     pj_size_t pkt_size, const pj_timestamp *ts,
			     unsigned *frame_cnt, pjmedia_frame frames[]);
static  pj_status_t pack_amr(anmed_private_t *codec_data, void *pkt, 
			     pj_size_t *pkt_size, pj_size_t max_pkt_size);

/* Android MediaCodec codec implementation descriptions. */
static struct anmed_codec {
    int		     enabled;		/* Is this codec enabled?	    */
    const char	    *name;		/* Codec name.			    */
    const char      *mime_type;         /* Mime type.                       */
    const char      *encoder_name;      /* Encoder name.                    */
    const char      *decoder_name;      /* Decoder name.                    */

    pj_uint8_t	     pt;		/* Payload type.		    */
    unsigned	     clock_rate;	/* Codec's clock rate.		    */
    unsigned	     channel_count;	/* Codec's channel count.	    */
    unsigned	     samples_per_frame;	/* Codec's samples count.	    */
    unsigned	     def_bitrate;	/* Default bitrate of this codec.   */
    unsigned	     max_bitrate;	/* Maximum bitrate of this codec.   */
    pj_uint8_t	     frm_per_pkt;	/* Default num of frames per packet.*/
    int		     has_native_vad;	/* Codec has internal VAD?	    */
    int		     has_native_plc;	/* Codec has internal PLC?	    */

//    predecode_cb     predecode;		/* Callback to translate RTP frame
//					   into USC frame.		    */
    parse_cb	     parse;		/* Callback to parse bitstream.	    */
    pack_cb	     pack;		/* Callback to pack bitstream.	    */

    pjmedia_codec_fmtp dec_fmtp;	/* Decoder's fmtp params.	    */
}

anmed_codec[] =
{
#   if PJMEDIA_HAS_ANMED_AMRNB
    {1, "AMR", "audio/3gpp", "OMX.google.amrnb.encoder",
        "OMX.google.amrnb.decoder",
        PJMEDIA_RTP_PT_AMR, 8000, 1, 160, 7400, 12200, 2, 1, 1,
	//&predecode_amr,
	&parse_amr, &pack_amr,
        {1, {{{(char *)"octet-align", 11}, {(char *)"1", 1}}}}
    },
#   endif

#   if PJMEDIA_HAS_ANMED_AMRWB
    {1, "AMR-WB", "audio/amr-wb", "OMX.google.amrwb.encoder",
        "OMX.google.amrwb.decoder",
        PJMEDIA_RTP_PT_AMRWB, 16000, 1, 320, 15850, 23850, 2, 1, 1,
        //&predecode_amr,
        &parse_amr, &pack_amr,
	{1, {{{(char *)"octet-align", 11}, {(char *)"1", 1}}}}
    },
#   endif
};

#if PJMEDIA_HAS_ANMED_AMRNB || PJMEDIA_HAS_ANMED_AMRWB

#include <pjmedia-codec/amr_helper.h>

typedef struct amr_settings_t {
    pjmedia_codec_amr_pack_setting enc_setting;
    pjmedia_codec_amr_pack_setting dec_setting;
    pj_int8_t enc_mode;
} amr_settings_t;

/* Rearrange AMR bitstream and convert RTP frame into USC frame:
 * - make the start_bit to be 0
 * - if it is speech frame, reorder bitstream from sensitivity bits order
 *   to encoder bits order.
 * - set the appropriate value of usc_frame.
 */
//static void predecode_amr( anmed_private_t *codec_data,
//			   const pjmedia_frame *rtp_frame,
//			   USC_Bitstream *usc_frame)
//{
//    pjmedia_frame frame;
//    pjmedia_codec_amr_bit_info *info;
//    pjmedia_codec_amr_pack_setting *setting;
//
//    setting = &((amr_settings_t*)codec_data->codec_setting)->dec_setting;
//
//    frame = *rtp_frame;
//    pjmedia_codec_amr_predecode(rtp_frame, setting, &frame);
//    info = (pjmedia_codec_amr_bit_info*) &frame.bit_info;
//
//    usc_frame->pBuffer = frame.buf;
//    usc_frame->nbytes = frame.size;
//    if (info->mode != -1) {
//	usc_frame->bitrate = setting->amr_nb?
//			     pjmedia_codec_amrnb_bitrates[info->mode]:
//			     pjmedia_codec_amrwb_bitrates[info->mode];
//    } else {
//	usc_frame->bitrate = 0;
//    }
//
//    if (frame.size > 5) {
//	/* Speech */
//	if (info->good_quality)
//	    usc_frame->frametype = 0;
//	else
//	    usc_frame->frametype = setting->amr_nb ? 5 : 6;
//    } else if (frame.size == 5) {
//	/* SID */
//	if (info->good_quality) {
//	    usc_frame->frametype = info->STI? 2 : 1;
//	} else {
//	    usc_frame->frametype = setting->amr_nb ? 6 : 7;
//	}
//    } else {
//	/* no data */
//	usc_frame->frametype = 3;
//    }
//}

/* Pack AMR payload */
static pj_status_t pack_amr(anmed_private_t *codec_data, void *pkt, 
			    pj_size_t *pkt_size, pj_size_t max_pkt_size)
{
    enum {MAX_FRAMES_PER_PACKET = PJMEDIA_MAX_FRAME_DURATION_MS / 20};

    pjmedia_frame frames[MAX_FRAMES_PER_PACKET];
    unsigned nframes = 0;
    pjmedia_codec_amr_bit_info *info;
    pj_uint8_t *r; /* Read cursor */
    pj_uint8_t SID_FT;
    pjmedia_codec_amr_pack_setting *setting;
    const pj_uint8_t *framelen_tbl;

    setting = &((amr_settings_t*)codec_data->codec_setting)->enc_setting;
    framelen_tbl = setting->amr_nb? pjmedia_codec_amrnb_framelen:
				    pjmedia_codec_amrwb_framelen;

    SID_FT = (pj_uint8_t)(setting->amr_nb? 8 : 9);

    /* Align pkt buf right */
    r = (pj_uint8_t*)pkt + max_pkt_size - *pkt_size;
    pj_memmove(r, pkt, *pkt_size);

    /* Get frames */
    for (;;) {
	pj_bool_t eof;
	pj_uint16_t info_;

	info_ = *((pj_uint16_t*)r);
	eof = ((info_ & 0x40) != 0);

	info = (pjmedia_codec_amr_bit_info*) &frames[nframes].bit_info;
	pj_bzero(info, sizeof(*info));
	info->frame_type = (pj_uint8_t)(info_ & 0x0F);
	info->good_quality = (pj_uint8_t)((info_ & 0x80) == 0);
	info->mode = (pj_int8_t) ((info_ >> 8) & 0x0F);
	info->STI = (pj_uint8_t)((info_ >> 5) & 1);

	frames[nframes].buf = r + 2;
	frames[nframes].size = info->frame_type <= SID_FT ?
			       framelen_tbl[info->frame_type] : 0;

	r += frames[nframes].size + 2;

	/* Last frame */
	if (++nframes >= MAX_FRAMES_PER_PACKET || eof)
	    break;
    }

    /* Pack */
    *pkt_size = max_pkt_size;
    return pjmedia_codec_amr_pack(frames, nframes, setting, pkt, pkt_size);
}

/* Parse AMR payload into frames. */
static pj_status_t parse_amr(anmed_private_t *codec_data, void *pkt, 
			     pj_size_t pkt_size, const pj_timestamp *ts,
			     unsigned *frame_cnt, pjmedia_frame frames[])
{
    amr_settings_t* s = (amr_settings_t*)codec_data->codec_setting;
    pjmedia_codec_amr_pack_setting *setting;
    pj_status_t status;
    pj_uint8_t cmr;

    setting = &s->dec_setting;

    status = pjmedia_codec_amr_parse(pkt, pkt_size, ts, setting, frames, 
				     frame_cnt, &cmr);
    if (status != PJ_SUCCESS)
	return status;

    /* Check Change Mode Request. */
    if (((setting->amr_nb && cmr <= 7) || (!setting->amr_nb && cmr <= 8)) &&
	s->enc_mode != cmr)
    {
	struct anmed_codec *anmed_data = &anmed_codec[codec_data->codec_idx];

	s->enc_mode = cmr;
    }

    return PJ_SUCCESS;
}

#endif /* PJMEDIA_HAS_ANMED_AMRNB || PJMEDIA_HAS_ANMED_AMRWB */

static pj_status_t configure_codec(anmed_private_t *anmed_data,
				   pj_bool_t is_encoder)
{
    media_status_t am_status;
    AMediaFormat *aud_fmt;
    int idx = anmed_data->codec_idx;
    AMediaCodec *codec = (is_encoder?anmed_data->enc:anmed_data->dec);

    aud_fmt = AMediaFormat_new();
    if (!aud_fmt) {
        return PJ_ENOMEM;
    }
    AMediaFormat_setString(aud_fmt, ANMED_KEY_MIME,
                           anmed_codec[idx].mime_type);
    AMediaFormat_setInt32(aud_fmt, ANMED_KEY_BITRATE,
			  anmed_codec[idx].def_bitrate);
    AMediaFormat_setInt32(aud_fmt, ANMED_KEY_PCM_ENCODING, 2);
    AMediaFormat_setInt32(aud_fmt, ANMED_KEY_SAMPLE_RATE,
                          anmed_codec[idx].clock_rate);
    AMediaFormat_setInt32(aud_fmt, ANMED_KEY_CHANNEL_COUNT,
                          anmed_codec[idx].channel_count);

    /* Configure and start encoder. */
    am_status = AMediaCodec_configure(codec, aud_fmt, NULL, NULL, is_encoder);
    AMediaFormat_delete(aud_fmt);
    if (am_status != AMEDIA_OK) {
        PJ_LOG(4, (THIS_FILE, "%s configure failed, status=%d",
               is_encoder?"Encoder":"Decoder", am_status));
        return PJMEDIA_CODEC_EFAILED;
    }
    am_status = AMediaCodec_start(codec);
    if (am_status != AMEDIA_OK) {
	PJ_LOG(4, (THIS_FILE, "%s start failed, status=%d",
	       is_encoder?"Encoder":"Decoder", am_status));
	return PJMEDIA_CODEC_EFAILED;
    }
    PJ_LOG(4, (THIS_FILE, "%s started", is_encoder?"Encoder":"Decoder"));
    return PJ_SUCCESS;
}

/*
 * Initialize and register Android MediaCodec codec factory to pjmedia endpoint.
 */
PJ_DEF(pj_status_t) pjmedia_codec_anmed_aud_init( pjmedia_endpt *endpt )
{
    pjmedia_codec_mgr *codec_mgr;
    pj_str_t codec_name;
    pj_status_t status;

    if (anmed_factory.pool != NULL) {
	/* Already initialized. */
	return PJ_SUCCESS;
    }

    PJ_LOG(4, (THIS_FILE, "Initing codec"));

    /* Create Android MediaCodec codec factory. */
    anmed_factory.base.op = &anmed_factory_op;
    anmed_factory.base.factory_data = NULL;
    anmed_factory.endpt = endpt;

    anmed_factory.pool = pjmedia_endpt_create_pool(endpt,
                                                   "Android MediaCodec codecs",
                                                   4000, 4000);
    if (!anmed_factory.pool)
	return PJ_ENOMEM;

    /* Create mutex. */
    status = pj_mutex_create_simple(anmed_factory.pool, 
                                    "Android MediaCodec codecs",
				    &anmed_factory.mutex);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Get the codec manager. */
    codec_mgr = pjmedia_endpt_get_codec_mgr(endpt);
    if (!codec_mgr) {
	status = PJ_EINVALIDOP;
	goto on_error;
    }

#if PJMEDIA_HAS_ANMED_AMRNB
    PJ_LOG(4, (THIS_FILE, "Registering AMRNB codec"));

    pj_cstr(&codec_name, "AMR");
    status = pjmedia_sdp_neg_register_fmt_match_cb(
					&codec_name,
					&pjmedia_codec_amr_match_sdp);
    if (status != PJ_SUCCESS)
	goto on_error;
#endif

#if PJMEDIA_HAS_ANMED_AMRWB
    PJ_LOG(4, (THIS_FILE, "Registering AMRWB codec"));

    pj_cstr(&codec_name, "AMR-WB");
    status = pjmedia_sdp_neg_register_fmt_match_cb(
					&codec_name,
					&pjmedia_codec_amr_match_sdp);
    if (status != PJ_SUCCESS)
	goto on_error;
#endif

    /* Suppress compile warning */
    PJ_UNUSED_ARG(codec_name);

    /* Register codec factory to endpoint. */
    status = pjmedia_codec_mgr_register_factory(codec_mgr, 
						&anmed_factory.base);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Done. */
    return PJ_SUCCESS;

on_error:
    pj_pool_release(anmed_factory.pool);
    anmed_factory.pool = NULL;
    return status;
}

/*
 * Unregister Android MediaCodec codecs factory from pjmedia endpoint.
 */
PJ_DEF(pj_status_t) pjmedia_codec_anmed_aud_deinit(void)
{
    pjmedia_codec_mgr *codec_mgr;
    pj_status_t status;

    if (anmed_factory.pool == NULL) {
	/* Already deinitialized */
	return PJ_SUCCESS;
    }

    pj_mutex_lock(anmed_factory.mutex);

    /* Get the codec manager. */
    codec_mgr = pjmedia_endpt_get_codec_mgr(anmed_factory.endpt);
    if (!codec_mgr) {
	pj_pool_release(anmed_factory.pool);
	anmed_factory.pool = NULL;
	pj_mutex_unlock(anmed_factory.mutex);
	return PJ_EINVALIDOP;
    }

    /* Unregister Android MediaCodec codecs factory. */
    status = pjmedia_codec_mgr_unregister_factory(codec_mgr,
						  &anmed_factory.base);

    /* Destroy mutex. */
    pj_mutex_unlock(anmed_factory.mutex);
    pj_mutex_destroy(anmed_factory.mutex);
    anmed_factory.mutex = NULL;

    /* Destroy pool. */
    pj_pool_release(anmed_factory.pool);
    anmed_factory.pool = NULL;

    return status;
}

/*
 * Check if factory can allocate the specified codec. 
 */
static pj_status_t anmed_test_alloc(pjmedia_codec_factory *factory,
				    const pjmedia_codec_info *info )
{
    unsigned i;

    PJ_UNUSED_ARG(factory);

    /* Type MUST be audio. */
    if (info->type != PJMEDIA_TYPE_AUDIO)
	return PJMEDIA_CODEC_EUNSUP;

    for (i = 0; i < PJ_ARRAY_SIZE(anmed_codec); ++i) {
	pj_str_t name = pj_str((char*)anmed_codec[i].name);
	if ((pj_stricmp(&info->encoding_name, &name) == 0) &&
	    (info->clock_rate == (unsigned)anmed_codec[i].clock_rate) &&
	    (info->channel_cnt == (unsigned)anmed_codec[i].channel_count) &&
	    (anmed_codec[i].enabled))
	{
	    return PJ_SUCCESS;
	}
    }

    /* Unsupported, or mode is disabled. */
    return PJMEDIA_CODEC_EUNSUP;
}

/*
 * Generate default attribute.
 */
static pj_status_t anmed_default_attr (pjmedia_codec_factory *factory, 
				       const pjmedia_codec_info *id, 
				       pjmedia_codec_param *attr )
{
    unsigned i;

    PJ_ASSERT_RETURN(factory==&anmed_factory.base, PJ_EINVAL);

    pj_bzero(attr, sizeof(pjmedia_codec_param));

    for (i = 0; i < PJ_ARRAY_SIZE(anmed_codec); ++i) {
	pj_str_t name = pj_str((char*)anmed_codec[i].name);
	if ((pj_stricmp(&id->encoding_name, &name) == 0) &&
	    (id->clock_rate == (unsigned)anmed_codec[i].clock_rate) &&
	    (id->channel_cnt == (unsigned)anmed_codec[i].channel_count) &&
	    (id->pt == (unsigned)anmed_codec[i].pt))
	{
	    attr->info.pt = (pj_uint8_t)id->pt;
	    attr->info.channel_cnt = anmed_codec[i].channel_count;
	    attr->info.clock_rate = anmed_codec[i].clock_rate;
	    attr->info.avg_bps = anmed_codec[i].def_bitrate;
	    attr->info.max_bps = anmed_codec[i].max_bitrate;
	    attr->info.pcm_bits_per_sample = 16;
	    attr->info.frm_ptime =  (pj_uint16_t)
				    (anmed_codec[i].samples_per_frame * 1000 /
				    anmed_codec[i].channel_count /
				    anmed_codec[i].clock_rate);
	    attr->setting.frm_per_pkt = anmed_codec[i].frm_per_pkt;

	    /* Default flags. */
	    attr->setting.plc = 1;
	    attr->setting.penh= 0;
	    attr->setting.vad = 1;
	    attr->setting.cng = attr->setting.vad;
	    attr->setting.dec_fmtp = anmed_codec[i].dec_fmtp;

	    return PJ_SUCCESS;
	}
    }

    return PJMEDIA_CODEC_EUNSUP;
}

/*
 * Enum codecs supported by this factory.
 */
static pj_status_t anmed_enum_codecs(pjmedia_codec_factory *factory, 
				     unsigned *count, 
				     pjmedia_codec_info codecs[])
{
    unsigned max;
    unsigned i;

    PJ_UNUSED_ARG(factory);
    PJ_ASSERT_RETURN(codecs && *count > 0, PJ_EINVAL);

    max = *count;

    for (i = 0, *count = 0; i < PJ_ARRAY_SIZE(anmed_codec) && *count < max; ++i) 
    {
	if (!anmed_codec[i].enabled)
	    continue;

	pj_bzero(&codecs[*count], sizeof(pjmedia_codec_info));
	codecs[*count].encoding_name = pj_str((char*)anmed_codec[i].name);
	codecs[*count].pt = anmed_codec[i].pt;
	codecs[*count].type = PJMEDIA_TYPE_AUDIO;
	codecs[*count].clock_rate = anmed_codec[i].clock_rate;
	codecs[*count].channel_cnt = anmed_codec[i].channel_count;

	++*count;
    }

    return PJ_SUCCESS;
}

static void create_codec(anmed_private_t *anmed_data)
{
    char const *enc_name = anmed_codec[anmed_data->codec_idx].encoder_name;
    char const *dec_name = anmed_codec[anmed_data->codec_idx].decoder_name;

    if (!anmed_data->enc) {
	anmed_data->enc = AMediaCodec_createCodecByName(enc_name);
	if (!anmed_data->enc) {
	    PJ_LOG(4, (THIS_FILE, "Failed creating encoder: %s", enc_name));
	}
	PJ_LOG(4, (THIS_FILE, "Done creating encoder: %s", enc_name));
    }

    if (!anmed_data->dec) {
	anmed_data->dec = AMediaCodec_createCodecByName(dec_name);
	if (!anmed_data->dec) {
	    PJ_LOG(4, (THIS_FILE, "Failed creating decoder: %s", dec_name));
	}
	PJ_LOG(4, (THIS_FILE, "Done creating decoder: %s", dec_name));
    }
}

/*
 * Allocate a new codec instance.
 */
static pj_status_t anmed_alloc_codec(pjmedia_codec_factory *factory,
				     const pjmedia_codec_info *id,
				     pjmedia_codec **p_codec)
{
    anmed_private_t *codec_data;
    pjmedia_codec *codec;
    int idx;
    pj_pool_t *pool;
    unsigned i;

    PJ_ASSERT_RETURN(factory && id && p_codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &anmed_factory.base, PJ_EINVAL);

    pj_mutex_lock(anmed_factory.mutex);

    /* Find codec's index */
    idx = -1;
    for (i = 0; i < PJ_ARRAY_SIZE(anmed_codec); ++i) {
	pj_str_t name = pj_str((char*)anmed_codec[i].name);
	if ((pj_stricmp(&id->encoding_name, &name) == 0) &&
	    (id->clock_rate == (unsigned)anmed_codec[i].clock_rate) &&
	    (id->channel_cnt == (unsigned)anmed_codec[i].channel_count) &&
	    (anmed_codec[i].enabled))
	{
	    idx = i;
	    break;
	}
    }
    if (idx == -1) {
	*p_codec = NULL;
	return PJMEDIA_CODEC_EFAILED;
    }

    /* Create pool for codec instance */
    pool = pjmedia_endpt_create_pool(anmed_factory.endpt, "anmedaud%p",
                                     512, 512);
    codec = PJ_POOL_ZALLOC_T(pool, pjmedia_codec);
    PJ_ASSERT_RETURN(codec != NULL, PJ_ENOMEM);
    codec->op = &anmed_op;
    codec->factory = factory;
    codec->codec_data = PJ_POOL_ZALLOC_T(pool, anmed_private_t);
    codec_data = (anmed_private_t*) codec->codec_data;

    /* Create PLC if codec has no internal PLC */
    if (!anmed_codec[idx].has_native_plc) {
	pj_status_t status;
	status = pjmedia_plc_create(pool, anmed_codec[idx].clock_rate, 
				    anmed_codec[idx].samples_per_frame, 0,
				    &codec_data->plc);
	if (status != PJ_SUCCESS) {
	    goto on_error;
	}
    }

    /* Create silence detector if codec has no internal VAD */
    if (!anmed_codec[idx].has_native_vad) {
	pj_status_t status;
	status = pjmedia_silence_det_create(pool,
					    anmed_codec[idx].clock_rate,
					    anmed_codec[idx].samples_per_frame,
					    &codec_data->vad);
	if (status != PJ_SUCCESS) {
	    goto on_error;
	}
    }

    codec_data->pool = pool;
    codec_data->codec_idx = idx;

    create_codec(codec_data);
    if (!codec_data->enc || !codec_data->dec) {
	goto on_error;
    }
    pj_mutex_unlock(anmed_factory.mutex);

    *p_codec = codec;
    return PJ_SUCCESS;

on_error:
    pj_mutex_unlock(anmed_factory.mutex);
    anmed_dealloc_codec(factory, codec);
    return PJMEDIA_CODEC_EFAILED;
}

/*
 * Free codec.
 */
static pj_status_t anmed_dealloc_codec(pjmedia_codec_factory *factory,
				       pjmedia_codec *codec )
{
    anmed_private_t *codec_data;

    PJ_ASSERT_RETURN(factory && codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &anmed_factory.base, PJ_EINVAL);

    /* Close codec, if it's not closed. */
    codec_data = (anmed_private_t*) codec->codec_data;
    if (codec_data->enc) {
        AMediaCodec_stop(codec_data->enc);
        AMediaCodec_delete(codec_data->enc);
        codec_data->enc = NULL;
    }

    if (codec_data->dec) {
        AMediaCodec_stop(codec_data->dec);
        AMediaCodec_delete(codec_data->dec);
        codec_data->dec = NULL;
    }
    pj_pool_release(codec_data->pool);

    return PJ_SUCCESS;
}

/*
 * Init codec.
 */
static pj_status_t anmed_codec_init( pjmedia_codec *codec, 
				   pj_pool_t *pool )
{
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    return PJ_SUCCESS;
}

/*
 * Open codec.
 */
static pj_status_t anmed_codec_open(pjmedia_codec *codec,
				    pjmedia_codec_param *attr)
{
    anmed_private_t *codec_data = (anmed_private_t*) codec->codec_data;
    struct anmed_codec *anmed_data = &anmed_codec[codec_data->codec_idx];
    pjmedia_codec_amr_pack_setting *setting;
    unsigned i;
    pj_status_t status;

    PJ_ASSERT_RETURN(codec && attr, PJ_EINVAL);
    PJ_ASSERT_RETURN(codec_data != NULL, PJ_EINVALIDOP);

    PJ_LOG(5,(THIS_FILE, "Opening codec.."));

    codec_data->vad_enabled = (attr->setting.vad != 0);
    codec_data->plc_enabled = (attr->setting.plc != 0);
    anmed_data->clock_rate = attr->info.clock_rate;

#if PJMEDIA_HAS_ANMED_AMRNB
    if (anmed_data->pt == PJMEDIA_RTP_PT_AMR ||
	anmed_data->pt == PJMEDIA_RTP_PT_AMRWB)
    {
	amr_settings_t *s;
	pj_uint8_t octet_align = 0;
	pj_int8_t enc_mode;

	enc_mode = pjmedia_codec_amr_get_mode(attr->info.avg_bps);

	pj_assert(enc_mode >= 0 && enc_mode <= 8);

	/* Check AMR specific attributes */
	for (i = 0; i < attr->setting.dec_fmtp.cnt; ++i) {
	    /* octet-align, one of the parameters that must have same value
	     * in offer & answer (RFC 4867 Section 8.3.1). Just check fmtp
	     * in the decoder side, since it's value is guaranteed to fulfil
	     * above requirement (by SDP negotiator).
	     */
	    const pj_str_t STR_FMTP_OCTET_ALIGN = {(char *)"octet-align", 11};

	    if (pj_stricmp(&attr->setting.dec_fmtp.param[i].name,
			   &STR_FMTP_OCTET_ALIGN) == 0)
	    {
		octet_align=(pj_uint8_t)
			    pj_strtoul(&attr->setting.dec_fmtp.param[i].val);
		break;
	    }
	}
	for (i = 0; i < attr->setting.enc_fmtp.cnt; ++i) {
	    /* mode-set, encoding mode is chosen based on local default mode
	     * setting:
	     * - if local default mode is included in the mode-set, use it
	     * - otherwise, find the closest mode to local default mode;
	     *   if there are two closest modes, prefer to use the higher
	     *   one, e.g: local default mode is 4, the mode-set param
	     *   contains '2,3,5,6', then 5 will be chosen.
	     */
	    const pj_str_t STR_FMTP_MODE_SET = {(char *)"mode-set", 8};

	    if (pj_stricmp(&attr->setting.enc_fmtp.param[i].name,
			   &STR_FMTP_MODE_SET) == 0)
	    {
		const char *p;
		pj_size_t l;
		pj_int8_t diff = 99;

		p = pj_strbuf(&attr->setting.enc_fmtp.param[i].val);
		l = pj_strlen(&attr->setting.enc_fmtp.param[i].val);

		while (l--) {
		    if ((anmed_data->pt==PJMEDIA_RTP_PT_AMR &&
			 *p>='0' && *p<='7') ||
		        (anmed_data->pt==PJMEDIA_RTP_PT_AMRWB &&
		         *p>='0' && *p<='8'))
		    {
			pj_int8_t tmp = (pj_int8_t)(*p - '0' - enc_mode);

			if (PJ_ABS(diff) > PJ_ABS(tmp) ||
			    (PJ_ABS(diff) == PJ_ABS(tmp) && tmp > diff))
			{
			    diff = tmp;
			    if (diff == 0) break;
			}
		    }
		    ++p;
		}
		if (diff == 99)
		    goto on_error;

		enc_mode = (pj_int8_t)(enc_mode + diff);

		break;
	    }
	}
	/* Initialize AMR specific settings */
	s = PJ_POOL_ZALLOC_T(codec_data->pool, amr_settings_t);
	codec_data->codec_setting = s;

	s->enc_setting.amr_nb = (pj_uint8_t)
					(anmed_data->pt == PJMEDIA_RTP_PT_AMR);
	s->enc_setting.octet_aligned = octet_align;
	s->enc_setting.reorder = PJ_TRUE;
	s->enc_setting.cmr = 15;

	s->dec_setting.amr_nb = (pj_uint8_t)
					(anmed_data->pt == PJMEDIA_RTP_PT_AMR);
	s->dec_setting.octet_aligned = octet_align;
	s->dec_setting.reorder = PJ_TRUE;
	/* Apply encoder mode/bitrate */
	s->enc_mode = enc_mode;
    }
#endif
    status = configure_codec(codec_data, PJ_TRUE);
    if (status != PJ_SUCCESS) {
        goto on_error;
    }
    status = configure_codec(codec_data, PJ_FALSE);
    if (status != PJ_SUCCESS) {
	goto on_error;
    }

    return PJ_SUCCESS;

on_error:
    return PJMEDIA_CODEC_EFAILED;
}

/*
 * Close codec.
 */
static pj_status_t anmed_codec_close( pjmedia_codec *codec )
{
    PJ_UNUSED_ARG(codec);

    return PJ_SUCCESS;
}

/*
 * Modify codec settings.
 */
static pj_status_t  anmed_codec_modify(pjmedia_codec *codec, 
				     const pjmedia_codec_param *attr )
{
    anmed_private_t *codec_data = (anmed_private_t*) codec->codec_data;
    struct anmed_codec *anmed_data = &anmed_codec[codec_data->codec_idx];

    codec_data->vad_enabled = (attr->setting.vad != 0);
    codec_data->plc_enabled = (attr->setting.plc != 0);

    return PJ_SUCCESS;
}

/*
 * Get frames in the packet.
 */
static pj_status_t anmed_codec_parse( pjmedia_codec *codec,
				     void *pkt,
				     pj_size_t pkt_size,
				     const pj_timestamp *ts,
				     unsigned *frame_cnt,
				     pjmedia_frame frames[])
{
    anmed_private_t *codec_data = (anmed_private_t*) codec->codec_data;
    struct anmed_codec *anmed_data = &anmed_codec[codec_data->codec_idx];
    unsigned count = 0;

    PJ_ASSERT_RETURN(frame_cnt, PJ_EINVAL);

    if (anmed_data->parse != NULL) {
	return anmed_data->parse(codec_data, pkt,  pkt_size, ts, frame_cnt,
				 frames);
    }

    while (pkt_size >= codec_data->frame_size && count < *frame_cnt) {
	frames[count].type = PJMEDIA_FRAME_TYPE_AUDIO;
	frames[count].buf = pkt;
	frames[count].size = codec_data->frame_size;
	frames[count].timestamp.u64 = ts->u64 +
				      count*anmed_data->samples_per_frame;

	pkt = ((char*)pkt) + codec_data->frame_size;
	pkt_size -= codec_data->frame_size;

	++count;
    }

    if (pkt_size && count < *frame_cnt) {
	frames[count].type = PJMEDIA_FRAME_TYPE_AUDIO;
	frames[count].buf = pkt;
	frames[count].size = pkt_size;
	frames[count].timestamp.u64 = ts->u64 +
				      count*anmed_data->samples_per_frame;
	++count;
    }

    *frame_cnt = count;
    return PJ_SUCCESS;
}

/*
 * Encode frames.
 */
static pj_status_t anmed_codec_encode( pjmedia_codec *codec, 
				     const struct pjmedia_frame *input,
				     unsigned output_buf_len, 
				     struct pjmedia_frame *output)
{
    anmed_private_t *codec_data = (anmed_private_t*) codec->codec_data;
    struct anmed_codec *anmed_data = &anmed_codec[codec_data->codec_idx];
    unsigned samples_per_frame;
    unsigned nsamples;
    pj_size_t tx = 0;
    pj_int16_t *pcm_in   = (pj_int16_t*)input->buf;
    pj_uint8_t  *bits_out = (pj_uint8_t*) output->buf;
    pj_uint8_t pt;

    /* Invoke external VAD if codec has no internal VAD */
    if (codec_data->vad && codec_data->vad_enabled) {
	pj_bool_t is_silence;
	pj_int32_t silence_duration;

	silence_duration = pj_timestamp_diff32(&codec_data->last_tx, 
					       &input->timestamp);

	is_silence = pjmedia_silence_det_detect(codec_data->vad, 
					        (const pj_int16_t*) input->buf,
						(input->size >> 1),
						NULL);
	if (is_silence &&
	    (PJMEDIA_CODEC_MAX_SILENCE_PERIOD == -1 ||
	     silence_duration < (PJMEDIA_CODEC_MAX_SILENCE_PERIOD *
	 			 (int)anmed_data->clock_rate / 1000)))
	{
	    output->type = PJMEDIA_FRAME_TYPE_NONE;
	    output->buf = NULL;
	    output->size = 0;
	    output->timestamp = input->timestamp;
	    return PJ_SUCCESS;
	} else {
	    codec_data->last_tx = input->timestamp;
	}
    }

    nsamples = input->size >> 1;
    samples_per_frame = anmed_data->samples_per_frame;
    pt = anmed_data->pt;

    PJ_ASSERT_RETURN(nsamples % samples_per_frame == 0, 
		     PJMEDIA_CODEC_EPCMFRMINLEN);

    /* Encode the frames */
    while (nsamples >= samples_per_frame) {

#if PJMEDIA_HAS_ANMED_AMRNB
	/* For AMR: reserve two octets for AMR frame info */
#endif

#if PJMEDIA_HAS_ANMED_AMRNB
	/* For AMR: put info (frametype, degraded, last frame, mode) in the 
	 * first two octets for payload packing.
	 */
//	if (pt == PJMEDIA_RTP_PT_AMR || pt == PJMEDIA_RTP_PT_AMRWB) {
//	    pj_uint16_t *info = (pj_uint16_t*)bits_out;
//
//	    /* Two octets for AMR frame info, 0=LSB:
//	     * bit 0-3	: frame type
//	     * bit 5	: STI flag
//	     * bit 6	: last frame flag
//	     * bit 7	: quality flag
//	     * bit 8-11	: mode
//	     */
//	    out.nbytes += 2;
//	    if (out.frametype == 0 || out.frametype == 4 ||
//		(pt == PJMEDIA_RTP_PT_AMR && out.frametype == 5) ||
//		(pt == PJMEDIA_RTP_PT_AMRWB && out.frametype == 6))
//	    {
//		/* Speech frame type */
//		*info = (char)pjmedia_codec_amr_get_mode(out.bitrate);
//		/* Quality */
//		if (out.frametype == 5 || out.frametype == 6)
//		    *info |= 0x80;
//	    } else if (out.frametype == 1 || out.frametype == 2 ||
//		       (pt == PJMEDIA_RTP_PT_AMR && out.frametype == 6) ||
//		       (pt == PJMEDIA_RTP_PT_AMRWB && out.frametype == 7))
//	    {
//		/* SID frame type */
//		*info = (pj_uint8_t)(pt == PJMEDIA_RTP_PT_AMRWB? 9 : 8);
//		/* Quality */
//		if (out.frametype == 6 || out.frametype == 7)
//		    *info |= 0x80;
//		/* STI */
//		if (out.frametype != 1)
//		    *info |= 0x20;
//	    } else {
//		/* Untransmited */
//		*info = 15;
//		out.nbytes = 2;
//	    }
//
//	    /* Mode */
//	    *info |= (char)pjmedia_codec_amr_get_mode(out.bitrate) << 8;
//
//	    /* Last frame flag */
//	    if (nsamples == samples_per_frame)
//		*info |= 0x40;
//	}
#endif

	pcm_in += samples_per_frame;
	nsamples -= samples_per_frame;
    }

//    if (anmed_data->pack != NULL) {
//	anmed_data->pack(codec_data, output->buf, &tx, output_buf_len);
//    }

    /* Check if we don't need to transmit the frame (DTX) */
    if (tx == 0) {
	output->buf = NULL;
	output->size = 0;
	output->timestamp.u64 = input->timestamp.u64;
	output->type = PJMEDIA_FRAME_TYPE_NONE;
	return PJ_SUCCESS;
    }

    output->size = tx;
    output->type = PJMEDIA_FRAME_TYPE_AUDIO;
    output->timestamp = input->timestamp;

    return PJ_SUCCESS;
}

/*
 * Decode frame.
 */
static pj_status_t anmed_codec_decode( pjmedia_codec *codec, 
				     const struct pjmedia_frame *input,
				     unsigned output_buf_len, 
				     struct pjmedia_frame *output)
{
    anmed_private_t *codec_data = (anmed_private_t*) codec->codec_data;
    struct anmed_codec *anmed_data = &anmed_codec[codec_data->codec_idx];
    unsigned samples_per_frame;
    pj_uint8_t pt;

    if (1)
	return PJ_EINVAL;

    pt = anmed_data->pt; 
    samples_per_frame = anmed_data->samples_per_frame;

    PJ_ASSERT_RETURN(output_buf_len >= samples_per_frame << 1,
		     PJMEDIA_CODEC_EPCMTOOSHORT);

    if (input->type != PJMEDIA_FRAME_TYPE_AUDIO)
    {
	pjmedia_zero_samples((pj_int16_t*)output->buf, samples_per_frame);
	output->size = samples_per_frame << 1;
	output->timestamp.u64 = input->timestamp.u64;
	output->type = PJMEDIA_FRAME_TYPE_AUDIO;
	return PJ_SUCCESS;
    }
    output->type = PJMEDIA_FRAME_TYPE_AUDIO;
    output->size = samples_per_frame << 1;
    output->timestamp.u64 = input->timestamp.u64;

    /* Invoke external PLC if codec has no internal PLC */
    if (codec_data->plc && codec_data->plc_enabled)
	pjmedia_plc_save(codec_data->plc, (pj_int16_t*)output->buf);

    return PJ_SUCCESS;
}

/* 
 * Recover lost frame.
 */
static pj_status_t  anmed_codec_recover(pjmedia_codec *codec, 
				      unsigned output_buf_len, 
				      struct pjmedia_frame *output)
{
    anmed_private_t *codec_data = (anmed_private_t*) codec->codec_data;
    struct anmed_codec *anmed_data = &anmed_codec[codec_data->codec_idx];
    unsigned samples_per_frame;

    PJ_UNUSED_ARG(output_buf_len);

    samples_per_frame = anmed_data->samples_per_frame;

    output->type = PJMEDIA_FRAME_TYPE_AUDIO;
    output->size = samples_per_frame << 1;

    if (codec_data->plc_enabled) {
	if (codec_data->plc) {
	    pjmedia_plc_generate(codec_data->plc, (pj_int16_t*)output->buf);
	} else {
	}
    } else {
	//pjmedia_zero_samples((pj_int16_t*)output->buf, samples_per_frame);
    }

    return PJ_SUCCESS;
}


#endif	/* PJMEDIA_HAS_ANDROID_MEDIACODEC */

