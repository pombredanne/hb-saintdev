/* qsv_common.c
 *
 * Copyright (c) 2003-2014 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifdef USE_QSV

#include <stdio.h>

#include "hb.h"
#include "ports.h"
#include "common.h"
#include "hb_dict.h"
#include "qsv_common.h"
#include "h264_common.h"
#include "h265_common.h"

// QSV info for each codec
static hb_qsv_info_t *hb_qsv_info_avc       = NULL;
static hb_qsv_info_t *hb_qsv_info_hevc      = NULL;
// API versions
static mfxVersion qsv_software_version      = { .Version   = 0, };
static mfxVersion qsv_hardware_version      = { .Version   = 0, };
// AVC implementations
static hb_qsv_info_t qsv_software_info_avc  = { .available = 0, .codec_id = MFX_CODEC_AVC,  .implementation = MFX_IMPL_SOFTWARE, };
static hb_qsv_info_t qsv_hardware_info_avc  = { .available = 0, .codec_id = MFX_CODEC_AVC,  .implementation = MFX_IMPL_HARDWARE_ANY|MFX_IMPL_VIA_ANY, };
// HEVC implementations
static mfxPluginUID  qsv_encode_plugin_hevc = { .Data      = { 0x2F, 0xCA, 0x99, 0x74, 0x9F, 0xDB, 0x49, 0xAE, 0xB1, 0x21, 0xA5, 0xB6, 0x3E, 0xF5, 0x68, 0xF7 } };
static hb_qsv_info_t qsv_software_info_hevc = { .available = 0, .codec_id = MFX_CODEC_HEVC, .implementation = MFX_IMPL_SOFTWARE, };
static hb_qsv_info_t qsv_hardware_info_hevc = { .available = 0, .codec_id = MFX_CODEC_HEVC, .implementation = MFX_IMPL_HARDWARE_ANY|MFX_IMPL_VIA_ANY, };

// check available Intel Media SDK version against a minimum
#define HB_CHECK_MFX_VERSION(MFX_VERSION, MAJOR, MINOR) \
    (MFX_VERSION.Major == MAJOR  && MFX_VERSION.Minor >= MINOR)

/*
 * Determine the "generation" of QSV hardware based on the CPU microarchitecture.
 * Anything unknown is assumed to be more recent than the latest known generation.
 * This avoids having to order the hb_cpu_platform enum depending on QSV hardware.
 */
enum
{
    QSV_G0, // third party hardware
    QSV_G1, // Sandy Bridge or equivalent
    QSV_G2, // Ivy Bridge or equivalent
    QSV_G3, // Haswell or equivalent
};
static int qsv_hardware_generation(int cpu_platform)
{
    switch (cpu_platform)
    {
        case HB_CPU_PLATFORM_INTEL_BNL:
            return QSV_G0;
        case HB_CPU_PLATFORM_INTEL_SNB:
            return QSV_G1;
        case HB_CPU_PLATFORM_INTEL_IVB:
        case HB_CPU_PLATFORM_INTEL_SLM:
            return QSV_G2;
        case HB_CPU_PLATFORM_INTEL_HSW:
        default:
            return QSV_G3;
    }
}

/*
 * Determine whether a given mfxIMPL is hardware-accelerated.
 */
static int qsv_implementation_is_hardware(mfxIMPL implementation)
{
    return MFX_IMPL_BASETYPE(implementation) != MFX_IMPL_SOFTWARE;
}

int hb_qsv_available()
{
    return (hb_qsv_video_encoder_is_enabled(HB_VCODEC_QSV_H264) ||
            hb_qsv_video_encoder_is_enabled(HB_VCODEC_QSV_H265));
}

int hb_qsv_video_encoder_is_enabled(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_QSV_H264:
            return hb_qsv_info_avc  != NULL && hb_qsv_info_avc->available;
        case HB_VCODEC_QSV_H265:
            return hb_qsv_info_hevc != NULL && hb_qsv_info_hevc->available;
        default:
            return 0;
    }
}

int hb_qsv_audio_encoder_is_enabled(int encoder)
{
    switch (encoder)
    {
        default:
            return 0;
    }
}

static void init_video_param(mfxVideoParam *videoParam)
{
    if (videoParam == NULL)
    {
        return;
    }

    memset(videoParam, 0, sizeof(mfxVideoParam));
    videoParam->mfx.CodecId                 = MFX_CODEC_AVC;
    videoParam->mfx.CodecLevel              = MFX_LEVEL_UNKNOWN;
    videoParam->mfx.CodecProfile            = MFX_PROFILE_UNKNOWN;
    videoParam->mfx.RateControlMethod       = MFX_RATECONTROL_VBR;
    videoParam->mfx.TargetUsage             = MFX_TARGETUSAGE_BALANCED;
    videoParam->mfx.TargetKbps              = 5000;
    videoParam->mfx.GopOptFlag              = MFX_GOP_CLOSED;
    videoParam->mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
    videoParam->mfx.FrameInfo.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    videoParam->mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    videoParam->mfx.FrameInfo.FrameRateExtN = 25;
    videoParam->mfx.FrameInfo.FrameRateExtD = 1;
    videoParam->mfx.FrameInfo.Width         = 1920;
    videoParam->mfx.FrameInfo.CropW         = 1920;
    videoParam->mfx.FrameInfo.AspectRatioW  = 1;
    videoParam->mfx.FrameInfo.Height        = 1088;
    videoParam->mfx.FrameInfo.CropH         = 1080;
    videoParam->mfx.FrameInfo.AspectRatioH  = 1;
    videoParam->AsyncDepth                  = AV_QSV_ASYNC_DEPTH_DEFAULT;
    videoParam->IOPattern                   = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
}

static void init_ext_coding_option(mfxExtCodingOption *extCodingOption)
{
    if (extCodingOption == NULL)
    {
        return;
    }

    memset(extCodingOption, 0, sizeof(mfxExtCodingOption));
    extCodingOption->Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    extCodingOption->Header.BufferSz = sizeof(mfxExtCodingOption);
    extCodingOption->AUDelimiter     = MFX_CODINGOPTION_OFF;
    extCodingOption->PicTimingSEI    = MFX_CODINGOPTION_OFF;
    extCodingOption->CAVLC           = MFX_CODINGOPTION_OFF;
}

static void init_ext_coding_option2(mfxExtCodingOption2 *extCodingOption2)
{
    if (extCodingOption2 == NULL)
    {
        return;
    }

    memset(extCodingOption2, 0, sizeof(mfxExtCodingOption2));
    extCodingOption2->Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    extCodingOption2->Header.BufferSz = sizeof(mfxExtCodingOption2);
    extCodingOption2->MBBRC           = MFX_CODINGOPTION_ON;
    extCodingOption2->ExtBRC          = MFX_CODINGOPTION_ON;
    extCodingOption2->Trellis         = MFX_TRELLIS_I|MFX_TRELLIS_P|MFX_TRELLIS_B;
    extCodingOption2->RepeatPPS       = MFX_CODINGOPTION_ON;
    extCodingOption2->BRefType        = MFX_B_REF_PYRAMID;
    extCodingOption2->AdaptiveI       = MFX_CODINGOPTION_ON;
    extCodingOption2->AdaptiveB       = MFX_CODINGOPTION_ON;
    extCodingOption2->LookAheadDS     = MFX_LOOKAHEAD_DS_4x;
    extCodingOption2->NumMbPerSlice   = 2040; // 1920x1088/4
}

static int query_capabilities(mfxSession session, mfxVersion version, hb_qsv_info_t *info)
{
    /*
     * MFXVideoENCODE_Query(mfxSession, mfxVideoParam *in, mfxVideoParam *out);
     *
     * Mode 1:
     * - in is NULL
     * - out has the parameters we want to query set to 1
     * - out->mfx.CodecId field has to be set (mandatory)
     * - MFXVideoENCODE_Query should zero out all unsupported parameters
     *
     * Mode 2:
     * - the paramaters we want to query are set for in
     * - in ->mfx.CodecId field has to be set (mandatory)
     * - out->mfx.CodecId field has to be set (mandatory)
     * - MFXVideoENCODE_Query should sanitize all unsupported parameters
     */
    mfxStatus status;
    mfxExtBuffer *videoExtParam[1];
    mfxVideoParam videoParam, inputParam;
    mfxExtCodingOption extCodingOption;
    mfxExtCodingOption2 extCodingOption2;
    mfxExtVideoSignalInfo extVideoSignalInfo;

    /* Reset capabilities before querying */
    info->capabilities = 0;

    /* Load optional codec plug-ins */
    if (hb_qsv_plugin_load(session, version, info->codec_id) < MFX_ERR_NONE)
    {
        return 0; // couldn't load plugin successfully
    }

    /*
     * First of all, check availability of an encoder for
     * this combination of a codec ID and implementation.
     *
     * Note: can error out rather than sanitizing
     * unsupported codec IDs, so don't log errors.
     */
    if (HB_CHECK_MFX_VERSION(version, HB_QSV_MINVERSION_MAJOR, HB_QSV_MINVERSION_MINOR))
    {
        if (info->implementation & MFX_IMPL_AUDIO)
        {
            /* Not yet supported */
            return 0;
        }
        else
        {
            init_video_param(&inputParam);
            inputParam.mfx.CodecId = info->codec_id;

            memset(&videoParam, 0, sizeof(mfxVideoParam));
            videoParam.mfx.CodecId = inputParam.mfx.CodecId;

            if (MFXVideoENCODE_Query(session, &inputParam, &videoParam) >= MFX_ERR_NONE &&
                videoParam.mfx.CodecId == info->codec_id)
            {
                info->available = 1;
            }
        }
    }
    if (!info->available)
    {
        /* Don't check capabilities for unavailable encoders */
        return 0;
    }

    if (info->implementation & MFX_IMPL_AUDIO)
    {
        /* We don't have any audio capability checks yet */
        return 0;
    }
    else
    {
        /* Implementation-specific features that can't be queried */
        if (qsv_implementation_is_hardware(info->implementation))
        {
            if (qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G3)
            {
                info->capabilities |= HB_QSV_CAP_B_REF_PYRAMID;
            }
        }
        else
        {
            if (HB_CHECK_MFX_VERSION(version, 1, 6))
            {
                info->capabilities |= HB_QSV_CAP_B_REF_PYRAMID;
            }
        }

        /* API-specific features that can't be queried */
        if (HB_CHECK_MFX_VERSION(version, 1, 6))
        {
            // API >= 1.6 (mfxBitstream::DecodeTimeStamp, 4K, H.264 Level 5.2)
            info->capabilities |= HB_QSV_CAP_MSDK_API_1_6;
        }

        /*
         * Check availability of optional rate control methods.
         *
         * Mode 2 tends to error out, but mode 1 gives false negatives, which
         * is worse. So use mode 2 and assume an error means it's unsupported.
         *
         * Also assume that LA and ICQ combined imply LA_ICQ is
         * supported, so we don't need to check the latter too.
         */
        if (HB_CHECK_MFX_VERSION(version, 1, 7))
        {
            init_video_param(&inputParam);
            inputParam.mfx.CodecId           = info->codec_id;
            inputParam.mfx.RateControlMethod = MFX_RATECONTROL_LA;

            memset(&videoParam, 0, sizeof(mfxVideoParam));
            videoParam.mfx.CodecId = inputParam.mfx.CodecId;

            if (MFXVideoENCODE_Query(session, &inputParam, &videoParam) >= MFX_ERR_NONE &&
                videoParam.mfx.RateControlMethod == MFX_RATECONTROL_LA)
            {
                info->capabilities |= HB_QSV_CAP_RATECONTROL_LA;

                // also check for LA + interlaced support
                init_video_param(&inputParam);
                inputParam.mfx.CodecId             = info->codec_id;
                inputParam.mfx.RateControlMethod   = MFX_RATECONTROL_LA;
                inputParam.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_FIELD_TFF;

                memset(&videoParam, 0, sizeof(mfxVideoParam));
                videoParam.mfx.CodecId = inputParam.mfx.CodecId;

                if (MFXVideoENCODE_Query(session, &inputParam, &videoParam) >= MFX_ERR_NONE &&
                    videoParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_FIELD_TFF           &&
                    videoParam.mfx.RateControlMethod   == MFX_RATECONTROL_LA)
                {
                    info->capabilities |= HB_QSV_CAP_RATECONTROL_LAi;
                }
            }
        }
        if (HB_CHECK_MFX_VERSION(version, 1, 8))
        {
            init_video_param(&inputParam);
            inputParam.mfx.CodecId           = info->codec_id;
            inputParam.mfx.RateControlMethod = MFX_RATECONTROL_ICQ;

            memset(&videoParam, 0, sizeof(mfxVideoParam));
            videoParam.mfx.CodecId = inputParam.mfx.CodecId;

            if (MFXVideoENCODE_Query(session, &inputParam, &videoParam) >= MFX_ERR_NONE &&
                videoParam.mfx.RateControlMethod == MFX_RATECONTROL_ICQ)
            {
                info->capabilities |= HB_QSV_CAP_RATECONTROL_ICQ;
            }
        }

        /*
         * Determine whether mfxExtCodingOption is supported.
         */
        if (HB_CHECK_MFX_VERSION(version, 1, 0))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;

            init_ext_coding_option(&extCodingOption);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extCodingOption;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
                /* Encoder can be configured via mfxExtCodingOption */
                info->capabilities |= HB_QSV_CAP_OPTION1;
            }
            else if (info->codec_id == MFX_CODEC_AVC)
            {
                /*
                 * This should not fail for AVC encoders, so we want to know
                 * about it - however, it may fail for other encoders (ignore)
                 */
                fprintf(stderr,
                        "hb_qsv_info_init: mfxExtCodingOption check failed (0x%"PRIX32", 0x%"PRIX32", %d)\n",
                        info->codec_id, info->implementation, status);
            }
        }

        /*
         * Determine whether mfxExtCodingOption2 and its fields are supported.
         *
         * Mode 2 suffers from false negatives with some drivers, whereas mode 1
         * suffers from false positives instead. The latter is probably easier
         * and/or safer to sanitize for us, so use mode 1.
         */
        if (HB_CHECK_MFX_VERSION(version, 1, 6))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;

            init_ext_coding_option2(&extCodingOption2);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extCodingOption2;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
#if 0
                // testing code that could come in handy
                fprintf(stderr, "-------------------\n");
                fprintf(stderr, "MBBRC:         0x%02X\n",     extCodingOption2.MBBRC);
                fprintf(stderr, "ExtBRC:        0x%02X\n",     extCodingOption2.ExtBRC);
                fprintf(stderr, "Trellis:       0x%02X\n",     extCodingOption2.Trellis);
                fprintf(stderr, "RepeatPPS:     0x%02X\n",     extCodingOption2.RepeatPPS);
                fprintf(stderr, "BRefType:      %4"PRIu16"\n", extCodingOption2.BRefType);
                fprintf(stderr, "AdaptiveI:     0x%02X\n",     extCodingOption2.AdaptiveI);
                fprintf(stderr, "AdaptiveB:     0x%02X\n",     extCodingOption2.AdaptiveB);
                fprintf(stderr, "LookAheadDS:   %4"PRIu16"\n", extCodingOption2.LookAheadDS);
                fprintf(stderr, "-------------------\n");
#endif

                /* Encoder can be configured via mfxExtCodingOption2 */
                info->capabilities |= HB_QSV_CAP_OPTION2;

                /*
                 * Sanitize API 1.6 fields:
                 *
                 * - MBBRC  requires G3 hardware (Haswell or equivalent)
                 * - ExtBRC requires G2 hardware (Ivy Bridge or equivalent)
                 */
                if (qsv_implementation_is_hardware(info->implementation) &&
                    qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G3)
                {
                    if (extCodingOption2.MBBRC)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_MBBRC;
                    }
                }
                if (qsv_implementation_is_hardware(info->implementation) &&
                    qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G2)
                {
                    if (extCodingOption2.ExtBRC)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_EXTBRC;
                    }
                }

                /*
                 * Sanitize API 1.7 fields:
                 *
                 * - Trellis requires G3 hardware (Haswell or equivalent)
                 */
                if (HB_CHECK_MFX_VERSION(version, 1, 7))
                {
                    if (qsv_implementation_is_hardware(info->implementation) &&
                        qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G3)
                    {
                        if (extCodingOption2.Trellis)
                        {
                            info->capabilities |= HB_QSV_CAP_OPTION2_TRELLIS;
                        }
                    }
                }

                /*
                 * Sanitize API 1.8 fields:
                 *
                 * - BRefType    requires B-pyramid support
                 * - LookAheadDS requires lookahead support
                 * - AdaptiveI, AdaptiveB, NumMbPerSlice unknown (trust Query)
                 */
                if (HB_CHECK_MFX_VERSION(version, 1, 8))
                {
                    if (info->capabilities & HB_QSV_CAP_B_REF_PYRAMID)
                    {
                        if (extCodingOption2.BRefType)
                        {
                            info->capabilities |= HB_QSV_CAP_OPTION2_BREFTYPE;
                        }
                    }
                    if (info->capabilities & HB_QSV_CAP_RATECONTROL_LA)
                    {
                        if (extCodingOption2.LookAheadDS)
                        {
                            info->capabilities |= HB_QSV_CAP_OPTION2_LA_DOWNS;
                        }
                    }
                    if (extCodingOption2.AdaptiveI && extCodingOption2.AdaptiveB)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_IB_ADAPT;
                    }
                    if (extCodingOption2.NumMbPerSlice)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_NMBSLICE;
                    }
                }
            }
            else if (info->codec_id == MFX_CODEC_AVC)
            {
                /*
                 * This should not fail for AVC encoders, so we want to know
                 * about it - however, it may fail for other encoders (ignore)
                 */
                fprintf(stderr,
                        "hb_qsv_info_init: mfxExtCodingOption2 check failed (0x%"PRIX32", 0x%"PRIX32", %d)\n",
                        info->codec_id, info->implementation, status);
            }
        }
    }

    /* Unload optional codec plug-ins */
    hb_qsv_plugin_unload(session, version, info->codec_id);

    return 0;
}

int hb_qsv_info_init()
{
    static int init_done = 0;
    if (init_done)
        return 0;
    init_done = 1;

    /*
     * First, check for any MSDK version to determine whether one or
     * more implementations are present; then check if we can use them.
     *
     * I've had issues using a NULL version with some combinations of
     * hardware and driver, so use a low version number (1.0) instead.
     */
    mfxSession session;
    mfxVersion version = { .Major = 1, .Minor = 0, };

    // check for software fallback
    if (MFXInit(MFX_IMPL_SOFTWARE, &version, &session) == MFX_ERR_NONE)
    {
        // Media SDK software found, but check that our minimum is supported
        MFXQueryVersion(session, &qsv_software_version);
        if (HB_CHECK_MFX_VERSION(qsv_software_version,
                                 HB_QSV_MINVERSION_MAJOR,
                                 HB_QSV_MINVERSION_MINOR))
        {
            query_capabilities(session, qsv_software_version, &qsv_software_info_avc);
            query_capabilities(session, qsv_software_version, &qsv_software_info_hevc);
            // now that we know which hardware encoders are
            // available, we can set the preferred implementation
            hb_qsv_impl_set_preferred("software");
        }
        MFXClose(session);
    }

    // check for actual hardware support
    if (MFXInit(MFX_IMPL_HARDWARE_ANY, &version, &session) == MFX_ERR_NONE)
    {
        // Media SDK hardware found, but check that our minimum is supported
        //
        // Note: this-party hardware (QSV_G0) is unsupported for the time being
        MFXQueryVersion(session, &qsv_hardware_version);
        if (qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G1 &&
            HB_CHECK_MFX_VERSION(qsv_hardware_version,
                                 HB_QSV_MINVERSION_MAJOR,
                                 HB_QSV_MINVERSION_MINOR))
        {
            query_capabilities(session, qsv_hardware_version, &qsv_hardware_info_avc);
            query_capabilities(session, qsv_hardware_version, &qsv_hardware_info_hevc);
            // now that we know which hardware encoders are
            // available, we can set the preferred implementation
            hb_qsv_impl_set_preferred("hardware");
        }
        MFXClose(session);
    }

    // success
    return 0;
}

static void log_capabilities(int log_level, uint64_t caps, const char *prefix)
{
    if (!caps)
    {
        hb_deep_log(log_level, "%s none (standard feature set)", prefix);
    }
    else
    {
        hb_deep_log(log_level, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", prefix,
                    !(caps & HB_QSV_CAP_MSDK_API_1_6)     ? "" : " api1.6",
                    !(caps & HB_QSV_CAP_B_REF_PYRAMID)    ? "" : " bpyramid",
                    !(caps & HB_QSV_CAP_OPTION2_BREFTYPE) ? "" : " breftype",
                    !(caps & HB_QSV_CAP_RATECONTROL_LA)   ? "" : " lookahead",
                    !(caps & HB_QSV_CAP_RATECONTROL_LAi)  ? "" : " lookaheadi",
                    !(caps & HB_QSV_CAP_OPTION2_LA_DOWNS) ? "" : " lookaheadds",
                    !(caps & HB_QSV_CAP_RATECONTROL_ICQ)  ? "" : " icq",
                    !(caps & HB_QSV_CAP_VSINFO)           ? "" : " videosignalinfo",
                    !(caps & HB_QSV_CAP_OPTION1)          ? "" : " extcodingoption",
                    !(caps & HB_QSV_CAP_OPTION2_MBBRC)    ? "" : " mbbrc",
                    !(caps & HB_QSV_CAP_OPTION2_EXTBRC)   ? "" : " extbrc",
                    !(caps & HB_QSV_CAP_OPTION2_TRELLIS)  ? "" : " trellis",
                    !(caps & HB_QSV_CAP_OPTION2_IB_ADAPT) ? "" : " adaptivei adaptiveb",
                    !(caps & HB_QSV_CAP_OPTION2_NMBSLICE) ? "" : " nummbperslice");
    }
}

void hb_qsv_info_print()
{
    // is QSV available and usable?
    hb_log("Intel Quick Sync Video support: %s",
           hb_qsv_available() ? "yes": "no");

    // also print the details
    if (qsv_hardware_version.Version)
    {
        hb_log(" - Intel Media SDK hardware: API %"PRIu16".%"PRIu16" (minimum: %"PRIu16".%"PRIu16")",
               qsv_hardware_version.Major, qsv_hardware_version.Minor,
               HB_QSV_MINVERSION_MAJOR,    HB_QSV_MINVERSION_MINOR);
    }
    if (qsv_software_version.Version)
    {
        hb_log(" - Intel Media SDK software: API %"PRIu16".%"PRIu16" (minimum: %"PRIu16".%"PRIu16")",
               qsv_software_version.Major, qsv_software_version.Minor,
               HB_QSV_MINVERSION_MAJOR,    HB_QSV_MINVERSION_MINOR);
    }
    if (hb_qsv_available())
    {
        if (hb_qsv_info_avc != NULL && hb_qsv_info_avc->available)
        {
            hb_log(" - H.264 encoder: yes");
            hb_log("    - preferred implementation: %s",
                   hb_qsv_impl_get_name(hb_qsv_info_avc->implementation));
            if (qsv_hardware_info_avc.available)
            {
                log_capabilities(2, qsv_hardware_info_avc.capabilities,
                                 "    - capabilities (hardware): ");
            }
            if (qsv_software_info_avc.available)
            {
                log_capabilities(2, qsv_software_info_avc.capabilities,
                                 "    - capabilities (software): ");
            }
        }
        else
        {
            hb_log(" - H.264 encoder: no");
        }
        if (hb_qsv_info_hevc != NULL && hb_qsv_info_hevc->available)
        {
            hb_log(" - H.265 encoder: yes");
            hb_log("    - preferred implementation: %s",
                   hb_qsv_impl_get_name(hb_qsv_info_hevc->implementation));
            if (qsv_hardware_info_hevc.available)
            {
                log_capabilities(2, qsv_hardware_info_hevc.capabilities,
                                 "    - capabilities (hardware): ");
            }
            if (qsv_software_info_hevc.available)
            {
                log_capabilities(2, qsv_software_info_hevc.capabilities,
                                 "    - capabilities (software): ");
            }
        }
        else
        {
            hb_log(" - H.265 encoder: no");
        }
    }
}

hb_qsv_info_t* hb_qsv_info_get(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_QSV_H264:
            return hb_qsv_info_avc;
        case HB_VCODEC_QSV_H265:
            return hb_qsv_info_hevc;
        default:
            return NULL;
    }
}

mfxStatus hb_qsv_plugin_load(mfxSession session, mfxVersion version, mfxU32 CodecId)
{
    mfxPluginUID *pluginUID = NULL;

    switch (CodecId)
    {
        case MFX_CODEC_HEVC:
            pluginUID = &qsv_encode_plugin_hevc;
            break;
        default:
            break;
    }

    if (pluginUID != NULL && HB_CHECK_MFX_VERSION(version, 1, 8))
    {
        return MFXVideoUSER_Load(session, pluginUID, 0);
    }

    return MFX_ERR_NONE;
}

mfxStatus hb_qsv_plugin_unload(mfxSession session, mfxVersion version, mfxU32 CodecId)
{
    mfxPluginUID *pluginUID = NULL;

    switch (CodecId)
    {
        case MFX_CODEC_HEVC:
            pluginUID = &qsv_encode_plugin_hevc;
            break;
        default:
            break;
    }

    if (pluginUID != NULL && HB_CHECK_MFX_VERSION(version, 1, 8))
    {
        MFXVideoUSER_UnLoad(session, pluginUID);
    }

    return MFX_ERR_NONE;
}

const char* hb_qsv_decode_get_codec_name(enum AVCodecID codec_id)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return "h264_qsv";

        default:
            return NULL;
    }
}

int hb_qsv_decode_is_enabled(hb_job_t *job)
{
    return ((job != NULL && job->qsv.decode)                        &&
            (job->vcodec                      & HB_VCODEC_QSV_MASK) &&
            (job->title->video_decode_support & HB_DECODE_SUPPORT_QSV));
}

int hb_qsv_copyframe_is_slow(int encoder)
{
    hb_qsv_info_t *info = hb_qsv_info_get(encoder);
    if (info != NULL && qsv_implementation_is_hardware(info->implementation))
    {
        // we should really check the driver version, but since it's not
        // available, checking the API version is the best we can do :-(
        return !HB_CHECK_MFX_VERSION(qsv_hardware_version, 1, 7);
    }
    return 0;
}

int hb_qsv_codingoption_xlat(int val)
{
    switch (HB_QSV_CLIP3(-1, 2, val))
    {
        case 0:
            return MFX_CODINGOPTION_OFF;
        case 1:
        case 2: // MFX_CODINGOPTION_ADAPTIVE, reserved
            return MFX_CODINGOPTION_ON;
        case -1:
        default:
            return MFX_CODINGOPTION_UNKNOWN;
    }
}

int hb_qsv_trellisvalue_xlat(int val)
{
    switch (HB_QSV_CLIP3(0, 3, val))
    {
        case 0:
            return MFX_TRELLIS_OFF;
        case 1: // I-frames only
            return MFX_TRELLIS_I;
        case 2: // I- and P-frames
            return MFX_TRELLIS_I|MFX_TRELLIS_P;
        case 3: // all frames
            return MFX_TRELLIS_I|MFX_TRELLIS_P|MFX_TRELLIS_B;
        default:
            return MFX_TRELLIS_UNKNOWN;
    }
}

const char* hb_qsv_codingoption_get_name(int val)
{
    switch (val)
    {
        case MFX_CODINGOPTION_ON:
            return "on";
        case MFX_CODINGOPTION_OFF:
            return "off";
        case MFX_CODINGOPTION_ADAPTIVE:
            return "adaptive";
        case MFX_CODINGOPTION_UNKNOWN:
            return "unknown (auto)";
        default:
            return NULL;
    }
}

int hb_qsv_atoindex(const char* const *arr, const char *str, int *err)
{
    int i;
    for (i = 0; arr[i] != NULL; i++)
    {
        if (!strcasecmp(arr[i], str))
        {
            break;
        }
    }
    *err = (arr[i] == NULL);
    return i;
}

// adapted from libx264
int hb_qsv_atobool(const char *str, int *err)
{
    if (!strcasecmp(str,    "1") ||
        !strcasecmp(str,  "yes") ||
        !strcasecmp(str, "true"))
    {
        return 1;
    }
    if (!strcasecmp(str,     "0") ||
        !strcasecmp(str,    "no") ||
        !strcasecmp(str, "false"))
    {
        return 0;
    }
    *err = 1;
    return 0;
}

// adapted from libx264
int hb_qsv_atoi(const char *str, int *err)
{
    char *end;
    int v = strtol(str, &end, 0);
    if (end == str || end[0] != '\0')
    {
        *err = 1;
    }
    return v;
}

// adapted from libx264
float hb_qsv_atof(const char *str, int *err)
{
    char *end;
    float v = strtod(str, &end);
    if (end == str || end[0] != '\0')
    {
        *err = 1;
    }
    return v;
}

int hb_qsv_param_parse(hb_qsv_param_t *param, hb_qsv_info_t *info,
                       const char *key, const char *value)
{
    float fvalue;
    int ivalue, error = 0;
    if (param == NULL || info == NULL)
    {
        return HB_QSV_PARAM_ERROR;
    }
    if (value == NULL || value[0] == '\0')
    {
        value = "true";
    }
    else if (value[0] == '=')
    {
        value++;
    }
    if (key == NULL || key[0] == '\0')
    {
        return HB_QSV_PARAM_BAD_NAME;
    }
    else if (!strncasecmp(key, "no-", 3))
    {
        key  += 3;
        value = hb_qsv_atobool(value, &error) ? "false" : "true";
        if (error)
        {
            return HB_QSV_PARAM_BAD_VALUE;
        }
    }
    if (!strcasecmp(key, "target-usage") ||
        !strcasecmp(key, "tu"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->videoParam->mfx.TargetUsage = HB_QSV_CLIP3(MFX_TARGETUSAGE_1,
                                                              MFX_TARGETUSAGE_7,
                                                              ivalue);
        }
    }
    else if (!strcasecmp(key, "num-ref-frame") ||
             !strcasecmp(key, "ref"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->videoParam->mfx.NumRefFrame = HB_QSV_CLIP3(0, 16, ivalue);
        }
    }
    else if (!strcasecmp(key, "gop-ref-dist"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->gop.gop_ref_dist = HB_QSV_CLIP3(-1, 32, ivalue);
        }
    }
    else if (!strcasecmp(key, "gop-pic-size") ||
             !strcasecmp(key, "keyint"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->gop.gop_pic_size = HB_QSV_CLIP3(-1, UINT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "b-pyramid"))
    {
        if (info->capabilities & HB_QSV_CAP_B_REF_PYRAMID)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                param->gop.b_pyramid = HB_QSV_CLIP3(-1, 1, ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "scenecut"))
    {
        ivalue = hb_qsv_atobool(value, &error);
        if (!error)
        {
            if (!ivalue)
            {
                param->videoParam->mfx.GopOptFlag |= MFX_GOP_STRICT;
            }
            else
            {
                param->videoParam->mfx.GopOptFlag &= ~MFX_GOP_STRICT;
            }
        }
    }
    else if (!strcasecmp(key, "adaptive-i") ||
             !strcasecmp(key, "i-adapt"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_IB_ADAPT)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->codingOption2.AdaptiveI = hb_qsv_codingoption_xlat(ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "adaptive-b") ||
             !strcasecmp(key, "b-adapt"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_IB_ADAPT)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->codingOption2.AdaptiveB = hb_qsv_codingoption_xlat(ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "force-cqp"))
    {
        ivalue = hb_qsv_atobool(value, &error);
        if (!error)
        {
            param->rc.icq = !ivalue;
        }
    }
    else if (!strcasecmp(key, "cqp-offset-i"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[0] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cqp-offset-p"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[1] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cqp-offset-b"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[2] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "vbv-init"))
    {
        fvalue = hb_qsv_atof(value, &error);
        if (!error)
        {
            param->rc.vbv_buffer_init = HB_QSV_CLIP3(0, UINT16_MAX, fvalue);
        }
    }
    else if (!strcasecmp(key, "vbv-bufsize"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.vbv_buffer_size = HB_QSV_CLIP3(0, UINT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "vbv-maxrate"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.vbv_max_bitrate = HB_QSV_CLIP3(0, UINT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cavlc") || !strcasecmp(key, "cabac"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION1)
        {
            switch (info->codec_id)
            {
                case MFX_CODEC_AVC:
//              case MFX_CODEC_HEVC:
                    ivalue = hb_qsv_atobool(value, &error);
                    break;
                default:
                    return HB_QSV_PARAM_UNSUPPORTED;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            if (!strcasecmp(key, "cabac"))
            {
                ivalue = !ivalue;
            }
            param->codingOption.CAVLC = hb_qsv_codingoption_xlat(ivalue);
        }
    }
    else if (!strcasecmp(key, "videoformat"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ivalue = hb_qsv_atoindex(hb_h264_vidformat_names, value, &error);
                break;
            case MFX_CODEC_HEVC:
                ivalue = hb_qsv_atoindex(hb_h265_vidformat_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.VideoFormat = ivalue;
        }
    }
    else if (!strcasecmp(key, "fullrange"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ivalue = hb_qsv_atoindex(hb_h264_fullrange_names, value, &error);
                break;
            case MFX_CODEC_HEVC:
                ivalue = hb_qsv_atoindex(hb_h265_fullrange_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.VideoFullRange = ivalue;
        }
    }
    else if (!strcasecmp(key, "colorprim"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ivalue = hb_qsv_atoindex(hb_h264_colorprim_names, value, &error);
                break;
            case MFX_CODEC_HEVC:
                ivalue = hb_qsv_atoindex(hb_h265_colorprim_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.ColourDescriptionPresent = 1;
            param->videoSignalInfo.ColourPrimaries = ivalue;
        }
    }
    else if (!strcasecmp(key, "transfer"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ivalue = hb_qsv_atoindex(hb_h264_transfer_names, value, &error);
                break;
            case MFX_CODEC_HEVC:
                ivalue = hb_qsv_atoindex(hb_h265_transfer_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.ColourDescriptionPresent = 1;
            param->videoSignalInfo.TransferCharacteristics = ivalue;
        }
    }
    else if (!strcasecmp(key, "colormatrix"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ivalue = hb_qsv_atoindex(hb_h264_colmatrix_names, value, &error);
                break;
            case MFX_CODEC_HEVC:
                ivalue = hb_qsv_atoindex(hb_h265_colmatrix_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.ColourDescriptionPresent = 1;
            param->videoSignalInfo.MatrixCoefficients = ivalue;
        }
    }
    else if (!strcasecmp(key, "tff") ||
             !strcasecmp(key, "interlaced"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
//          case MFX_CODEC_HEVC:
                ivalue = hb_qsv_atobool(value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoParam->mfx.FrameInfo.PicStruct = (ivalue                  ?
                                                          MFX_PICSTRUCT_FIELD_TFF :
                                                          MFX_PICSTRUCT_PROGRESSIVE);
        }
    }
    else if (!strcasecmp(key, "bff"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
//          case MFX_CODEC_HEVC:
                ivalue = hb_qsv_atobool(value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoParam->mfx.FrameInfo.PicStruct = (ivalue                  ?
                                                          MFX_PICSTRUCT_FIELD_BFF :
                                                          MFX_PICSTRUCT_PROGRESSIVE);
        }
    }
    else if (!strcasecmp(key, "mbbrc"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_MBBRC)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->codingOption2.MBBRC = hb_qsv_codingoption_xlat(ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "extbrc"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_EXTBRC)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->codingOption2.ExtBRC = hb_qsv_codingoption_xlat(ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead") ||
             !strcasecmp(key, "la"))
    {
        if (info->capabilities & HB_QSV_CAP_RATECONTROL_LA)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->rc.lookahead = ivalue;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead-depth") ||
             !strcasecmp(key, "la-depth"))
    {
        if (info->capabilities & HB_QSV_CAP_RATECONTROL_LA)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                // LookAheadDepth 10 will cause a hang with some driver versions
                param->codingOption2.LookAheadDepth = HB_QSV_CLIP3(11, 100,
                                                                   ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead-ds") ||
             !strcasecmp(key, "la-ds"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_LA_DOWNS)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                param->codingOption2.LookAheadDS = HB_QSV_CLIP3(MFX_LOOKAHEAD_DS_UNKNOWN,
                                                                MFX_LOOKAHEAD_DS_4x,
                                                                ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "trellis"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_TRELLIS)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                param->codingOption2.Trellis = hb_qsv_trellisvalue_xlat(ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else
    {
        /*
         * TODO:
         * - slice count (num-slice/slices, num-mb-per-slice/slice-max-mbs)
         * - open-gop
         * - fake-interlaced (mfxExtCodingOption.FramePicture???)
         * - intra-refresh
         */
        return HB_QSV_PARAM_BAD_NAME;
    }
    return error ? HB_QSV_PARAM_BAD_VALUE : HB_QSV_PARAM_OK;
}

static const char* const h264_profile_names [] = { "baseline",              "main",                "high",            NULL, };
static const int   const h264_profile_values[] = { MFX_PROFILE_AVC_BASELINE, MFX_PROFILE_AVC_MAIN, MFX_PROFILE_AVC_HIGH, 0, };

static const char* const h265_profile_names [] = { "main",                "mainstillpicture",   NULL, };
static const int   const h265_profile_values[] = { MFX_PROFILE_HEVC_MAIN, MFX_PROFILE_HEVC_MAINSP, 0, };

static int name2val(const char* const *names, const int const *values,
                    const char        *name,  int             *value)
{
    int err;
    int idx = hb_qsv_atoindex(names, name, &err);
    if (err || idx >= (sizeof(values) / sizeof(values[0])) || value == NULL)
    {
        return -1;
    }
    *value = values[idx];
    return 0;
}

int hb_qsv_profile_parse(hb_qsv_param_t *param, hb_qsv_info_t *info, const char *profile)
{
    if (profile != NULL && *profile && strcasecmp(profile, "auto"))
    {
        int ret, val = MFX_PROFILE_UNKNOWN;

        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ret = name2val(h264_profile_names, h264_profile_values, profile, &val);
                break;
            case MFX_CODEC_HEVC:
                ret = name2val(h265_profile_names, h265_profile_values, profile, &val);
                break;
            default:
                return -1;
        }

        param->videoParam->mfx.CodecProfile = val;
        return ret;
    }
    return 0;
}

int hb_qsv_level_parse(hb_qsv_param_t *param, hb_qsv_info_t *info, const char *level)
{
    if (level != NULL && *level && strcasecmp(level, "auto"))
    {
        int ret, val = MFX_LEVEL_UNKNOWN;

        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ret = name2val(hb_h264_level_names, hb_h264_level_values, level, &val);
                break;
            case MFX_CODEC_HEVC:
                ret = name2val(hb_h265_level_names, hb_h265_level_values, level, &val);
                break;
            default:
                return -1;
        }

        /* 4K encoding and H.264 level 5.2 require Media SDK with API >= 1.6 */
        if (info->codec_id == MFX_CODEC_AVC && !(info->capabilities &
                                                 HB_QSV_CAP_MSDK_API_1_6))
        {
            val = HB_QSV_CLIP3(MFX_LEVEL_UNKNOWN, MFX_LEVEL_AVC_51, val);
        }

        param->videoParam->mfx.CodecLevel = val;
        return ret;
    }
    return 0;
}

#ifdef HB_API_OLD_PRESET_GETTERS
const char* const* hb_qsv_presets()
{
    return hb_qsv_preset_get_names();
}
#endif

const char* const* hb_qsv_preset_get_names()
{
    if (qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G3)
    {
        return hb_qsv_preset_names2;
    }
    else
    {
        return hb_qsv_preset_names1;
    }
}

const char* const* hb_qsv_profile_get_names(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_QSV_H264:
            return hb_h264_profile_names;
        case HB_VCODEC_QSV_H265:
            return hb_h265_profile_names;
        default:
            return NULL;
    }
}

const char* const* hb_qsv_level_get_names(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_QSV_H264:
            return hb_h264_level_names;
        case HB_VCODEC_QSV_H265:
            return hb_h265_level_names;
        default:
            return NULL;
    }
}

const char* hb_qsv_video_quality_get_name(uint32_t codec)
{
    uint64_t codec_caps = 0;
    hb_qsv_info_t *info = hb_qsv_info_get(codec);

    if (info != NULL)
    {
        codec_caps = info->capabilities;
    }

    return (codec_caps & HB_QSV_CAP_RATECONTROL_ICQ) ? "ICQ" : "QP";
}

void hb_qsv_video_quality_get_limits(uint32_t codec, float *low, float *high,
                                     float *granularity, int *direction)
{
    uint64_t codec_caps = 0;
    hb_qsv_info_t *info = hb_qsv_info_get(codec);

    if (info != NULL)
    {
        codec_caps = info->capabilities;
    }

    switch (codec)
    {
        case HB_VCODEC_QSV_H264:
        case HB_VCODEC_QSV_H265://fixme
            *direction   = 1;
            *granularity = 1.;
            *low         = (codec_caps & HB_QSV_CAP_RATECONTROL_ICQ) ? 1. : 0.;
            *high        = 51.;
            break;

        default:
            *direction   = 1;
            *granularity = 1.;
            *low         = 0.;
            *high        = 51.;
            break;
    }
}

int hb_qsv_param_default_preset(hb_qsv_param_t *param,
                                mfxVideoParam *videoParam,
                                hb_qsv_info_t *info, const char *preset)
{
    if (param != NULL && videoParam != NULL && info != NULL)
    {
        int ret = hb_qsv_param_default(param, videoParam, info);
        if (ret)
        {
            return ret;
        }
    }
    else
    {
        hb_error("hb_qsv_param_default_preset: invalid pointer(s)");
        return -1;
    }
    if (preset != NULL && preset[0] != '\0')
    {
        if (!strcasecmp(preset, "quality"))
        {
            /*
             * HSW TargetUsage:     2
             *     NumRefFrame:     0
             *     GopRefDist:      4 (CQP), 3 (VBR)        -> -1 (set by encoder)
             *     GopPicSize:     32 (CQP), 1 second (VBR) -> -1 (set by encoder)
             *     BPyramid:        1 (CQP), 0 (VBR)        -> -1 (set by encoder)
             *     LookAhead:       1 (on)
             *     LookAheadDepth: 40
             *
             *
             * SNB
             * IVB Preset Not Available
             *
             * Note: this preset is the libhb default (like x264's "medium").
             */
        }
        else if (!strcasecmp(preset, "balanced"))
        {
            /*
             * HSW TargetUsage:     4
             *     NumRefFrame:     1
             *     GopRefDist:      4 (CQP), 3 (VBR)        -> -1 (set by encoder)
             *     GopPicSize:     32 (CQP), 1 second (VBR) -> -1 (set by encoder)
             *     BPyramid:        1 (CQP), 0 (VBR)        -> -1 (set by encoder)
             *     LookAhead:       0 (off)
             *     LookAheadDepth: Not Applicable
             */
            if (qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G3)
            {
                param->rc.lookahead                = 0;
                param->videoParam->mfx.NumRefFrame = 1;
                param->videoParam->mfx.TargetUsage = MFX_TARGETUSAGE_4;
            }
            else
            {
                /*
                 * SNB
                 * IVB TargetUsage:     2
                 *     NumRefFrame:     0
                 *     GopRefDist:      4 (CQP), 3 (VBR)        -> -1 (set by encoder)
                 *     GopPicSize:     32 (CQP), 1 second (VBR) -> -1 (set by encoder)
                 *     BPyramid:       Not Applicable
                 *     LookAhead:      Not Applicable
                 *     LookAheadDepth: Not Applicable
                 *
                 * Note: this preset is not the libhb default,
                 * but the settings are the same so do nothing.
                 */
            }
        }
        else if (!strcasecmp(preset, "speed"))
        {
            if (qsv_hardware_generation(hb_get_cpu_platform()) >= QSV_G3)
            {
                /*
                 * HSW TargetUsage:     6
                 *     NumRefFrame:     0 (CQP), 1 (VBR)        -> see note
                 *     GopRefDist:      4 (CQP), 3 (VBR)        -> -1 (set by encoder)
                 *     GopPicSize:     32 (CQP), 1 second (VBR) -> -1 (set by encoder)
                 *     BPyramid:        1 (CQP), 0 (VBR)        -> -1 (set by encoder)
                 *     LookAhead:       0 (off)
                 *     LookAheadDepth: Not Applicable
                 *
                 * Note: NumRefFrame depends on the RC method, which we don't
                 *       know here. Rather than have an additional variable and
                 *       having the encoder set it, we set it to 1 and let the
                 *       B-pyramid code sanitize it. Since BPyramid is 1 w/CQP,
                 *       the result (3) is the same as what MSDK would pick for
                 *       NumRefFrame 0 GopRefDist 4 GopPicSize 32.
                 */
                param->rc.lookahead                = 0;
                param->videoParam->mfx.NumRefFrame = 1;
                param->videoParam->mfx.TargetUsage = MFX_TARGETUSAGE_6;
            }
            else
            {
                /*
                 * SNB
                 * IVB TargetUsage:     4
                 *     NumRefFrame:     0
                 *     GopRefDist:      4 (CQP), 3 (VBR)        -> -1 (set by encoder)
                 *     GopPicSize:     32 (CQP), 1 second (VBR) -> -1 (set by encoder)
                 *     BPyramid:       Not Applicable
                 *     LookAhead:      Not Applicable
                 *     LookAheadDepth: Not Applicable
                 */
                param->videoParam->mfx.TargetUsage = MFX_TARGETUSAGE_4;
            }
        }
        else
        {
            hb_error("hb_qsv_param_default_preset: invalid preset '%s'", preset);
            return -1;
        }
    }
    return 0;
}

int hb_qsv_param_default(hb_qsv_param_t *param, mfxVideoParam *videoParam,
                         hb_qsv_info_t  *info)
{
    if (param != NULL && videoParam != NULL && info != NULL)
    {
        // introduced in API 1.0
        memset(&param->codingOption, 0, sizeof(mfxExtCodingOption));
        param->codingOption.Header.BufferId      = MFX_EXTBUFF_CODING_OPTION;
        param->codingOption.Header.BufferSz      = sizeof(mfxExtCodingOption);
        param->codingOption.MECostType           = 0; // reserved, must be 0
        param->codingOption.MESearchType         = 0; // reserved, must be 0
        param->codingOption.MVSearchWindow.x     = 0; // reserved, must be 0
        param->codingOption.MVSearchWindow.y     = 0; // reserved, must be 0
        param->codingOption.RefPicListReordering = 0; // reserved, must be 0
        param->codingOption.IntraPredBlockSize   = 0; // reserved, must be 0
        param->codingOption.InterPredBlockSize   = 0; // reserved, must be 0
        param->codingOption.MVPrecision          = 0; // reserved, must be 0
        param->codingOption.EndOfSequence        = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.RateDistortionOpt    = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.ResetRefList         = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.MaxDecFrameBuffering = 0; // unspecified
        param->codingOption.AUDelimiter          = MFX_CODINGOPTION_OFF;
        param->codingOption.SingleSeiNalUnit     = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.PicTimingSEI         = MFX_CODINGOPTION_OFF;
        param->codingOption.VuiNalHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.FramePicture         = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.CAVLC                = MFX_CODINGOPTION_OFF;
        // introduced in API 1.3
        param->codingOption.RefPicMarkRep        = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.FieldOutput          = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.NalHrdConformance    = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.SingleSeiNalUnit     = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.VuiVclHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
        // introduced in API 1.4
        param->codingOption.ViewOutput           = MFX_CODINGOPTION_UNKNOWN;
        // introduced in API 1.6
        param->codingOption.RecoveryPointSEI     = MFX_CODINGOPTION_UNKNOWN;

        // introduced in API 1.3
        memset(&param->videoSignalInfo, 0, sizeof(mfxExtVideoSignalInfo));
        param->videoSignalInfo.Header.BufferId          = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
        param->videoSignalInfo.Header.BufferSz          = sizeof(mfxExtVideoSignalInfo);
        param->videoSignalInfo.VideoFormat              = 5; // undefined
        param->videoSignalInfo.VideoFullRange           = 0; // TV range
        param->videoSignalInfo.ColourDescriptionPresent = 0; // don't write to bitstream
        param->videoSignalInfo.ColourPrimaries          = 2; // undefined
        param->videoSignalInfo.TransferCharacteristics  = 2; // undefined
        param->videoSignalInfo.MatrixCoefficients       = 2; // undefined

        // introduced in API 1.6
        memset(&param->codingOption2, 0, sizeof(mfxExtCodingOption2));
        param->codingOption2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
        param->codingOption2.Header.BufferSz = sizeof(mfxExtCodingOption2);
        param->codingOption2.IntRefType      = 0;
        param->codingOption2.IntRefCycleSize = 2;
        param->codingOption2.IntRefQPDelta   = 0;
        param->codingOption2.MaxFrameSize    = 0;
        param->codingOption2.BitrateLimit    = MFX_CODINGOPTION_ON;
        param->codingOption2.MBBRC           = MFX_CODINGOPTION_ON;
        param->codingOption2.ExtBRC          = MFX_CODINGOPTION_OFF;
        // introduced in API 1.7
        param->codingOption2.LookAheadDepth  = 40;
        param->codingOption2.Trellis         = MFX_TRELLIS_OFF;
        // introduced in API 1.8
        param->codingOption2.RepeatPPS       = MFX_CODINGOPTION_ON;
        param->codingOption2.BRefType        = MFX_B_REF_UNKNOWN; // controlled via gop.b_pyramid
        param->codingOption2.AdaptiveI       = MFX_CODINGOPTION_ON;
        param->codingOption2.AdaptiveB       = MFX_CODINGOPTION_ON;
        param->codingOption2.LookAheadDS     = MFX_LOOKAHEAD_DS_OFF;
        param->codingOption2.NumMbPerSlice   = 0;

        // GOP & rate control
        param->gop.b_pyramid          = -1; // set automatically
        param->gop.gop_pic_size       = -1; // set automatically
        param->gop.gop_ref_dist       = -1; // set automatically
        param->gop.int_ref_cycle_size = -1; // set automatically
        param->rc.icq                 =  1; // enabled by default (if supported)
        param->rc.lookahead           =  1; // enabled by default (if supported)
        param->rc.cqp_offsets[0]      =  0;
        param->rc.cqp_offsets[1]      =  2;
        param->rc.cqp_offsets[2]      =  4;
        param->rc.vbv_max_bitrate     =  0; // set automatically
        param->rc.vbv_buffer_size     =  0; // set automatically
        param->rc.vbv_buffer_init     = .0; // set automatically

        // introduced in API 1.0
        memset(videoParam, 0, sizeof(mfxVideoParam));
        param->videoParam                   = videoParam;
        param->videoParam->Protected        = 0; // reserved, must be 0
        param->videoParam->NumExtParam      = 0;
        param->videoParam->IOPattern        = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        param->videoParam->mfx.TargetUsage  = MFX_TARGETUSAGE_2;
        param->videoParam->mfx.GopOptFlag   = MFX_GOP_CLOSED;
        param->videoParam->mfx.NumThread    = 0; // deprecated, must be 0
        param->videoParam->mfx.EncodedOrder = 0; // input is in display order
        param->videoParam->mfx.IdrInterval  = 0; // all I-frames are IDR
        param->videoParam->mfx.NumSlice     = 0; // use Media SDK default
        param->videoParam->mfx.NumRefFrame  = 0; // use Media SDK default
        param->videoParam->mfx.GopPicSize   = 0; // use Media SDK default
        param->videoParam->mfx.GopRefDist   = 0; // use Media SDK default
        // introduced in API 1.1
        param->videoParam->AsyncDepth = AV_QSV_ASYNC_DEPTH_DEFAULT;
        // introduced in API 1.3
        param->videoParam->mfx.BRCParamMultiplier = 0; // no multiplier

        // FrameInfo: set by video encoder, except PicStruct
        param->videoParam->mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

        // attach supported mfxExtBuffer structures to the mfxVideoParam
        param->videoParam->ExtParam    = param->ExtParamArray;
        param->videoParam->NumExtParam = 0;
        if (info->capabilities & HB_QSV_CAP_VSINFO)
        {
            param->videoParam->ExtParam[param->videoParam->NumExtParam++] = (mfxExtBuffer*)&param->videoSignalInfo;
        }
        if (info->capabilities & HB_QSV_CAP_OPTION1)
        {
            param->videoParam->ExtParam[param->videoParam->NumExtParam++] = (mfxExtBuffer*)&param->codingOption;
        }
        if (info->capabilities & HB_QSV_CAP_OPTION2)
        {
            param->videoParam->ExtParam[param->videoParam->NumExtParam++] = (mfxExtBuffer*)&param->codingOption2;
        }
    }
    else
    {
        hb_error("hb_qsv_param_default: invalid pointer(s)");
        return -1;
    }
    return 0;
}

const char* hb_qsv_codec_name(uint32_t qsv_codec)
{
    switch (qsv_codec)
    {
        case MFX_CODEC_AVC:
            return "H.264";
        case MFX_CODEC_HEVC:
            return "H.265";
        default:
            return NULL;
    }
}

const char* hb_qsv_profile_name(uint32_t qsv_codec, uint16_t qsv_profile)
{
    if (qsv_codec == MFX_CODEC_AVC)
    {
        switch (qsv_profile)
        {
            case MFX_PROFILE_AVC_CONSTRAINED_BASELINE:
                return "Constrained Baseline";
            case MFX_PROFILE_AVC_BASELINE:
                return "Baseline";
            case MFX_PROFILE_AVC_EXTENDED:
                return "Extended";
            case MFX_PROFILE_AVC_MAIN:
                return "Main";
            case MFX_PROFILE_AVC_CONSTRAINED_HIGH:
                return "Constrained High";
            case MFX_PROFILE_AVC_PROGRESSIVE_HIGH:
                return "Progressive High";
            case MFX_PROFILE_AVC_HIGH:
                return "High";
            case MFX_PROFILE_UNKNOWN:
            default:
                return NULL;
        }
    }
    if (qsv_codec == MFX_CODEC_HEVC)
    {
        switch (qsv_profile)
        {
            case MFX_PROFILE_HEVC_MAIN:
                return "Main";
            case MFX_PROFILE_HEVC_MAIN10:
                return "Main 10";
            case MFX_PROFILE_HEVC_MAINSP:
                return "Main Still Picture";
            case MFX_PROFILE_UNKNOWN:
            default:
                return NULL;
        }
    }
    return NULL;
}

static const char* level2name(const char* const *names, const int const *values,
                              int level)
{
    int i;
    for (i = 0; names[i] != NULL; i++)
    {
        if (values[i] == level)
        {
            return names[i];
        }
    }
    return NULL;
}

const char* hb_qsv_level_name(uint32_t qsv_codec, uint16_t qsv_level)
{
    switch (qsv_codec)
    {
        case MFX_CODEC_AVC:
            return level2name(hb_h264_level_names, hb_h264_level_values, qsv_level);
        case MFX_CODEC_HEVC:
            return level2name(hb_h265_level_names, hb_h265_level_values, qsv_level);
        default:
            return NULL;
    }
}

const char* hb_qsv_frametype_name(uint16_t qsv_frametype)
{
    if      (qsv_frametype & MFX_FRAMETYPE_IDR)
    {
        return qsv_frametype & MFX_FRAMETYPE_REF ? "IDR (ref)" : "IDR";
    }
    else if (qsv_frametype & MFX_FRAMETYPE_I)
    {
        return qsv_frametype & MFX_FRAMETYPE_REF ? "I (ref)"   : "I";
    }
    else if (qsv_frametype & MFX_FRAMETYPE_P)
    {
        return qsv_frametype & MFX_FRAMETYPE_REF ? "P (ref)"   : "P";
    }
    else if (qsv_frametype & MFX_FRAMETYPE_B)
    {
        return qsv_frametype & MFX_FRAMETYPE_REF ? "B (ref)"   : "B";
    }
    else
    {
        return "unknown";
    }
}

uint8_t hb_qsv_frametype_xlat(uint16_t qsv_frametype, uint16_t *out_flags)
{
    uint16_t flags     = 0;
    uint8_t  frametype = 0;

    if      (qsv_frametype & MFX_FRAMETYPE_IDR)
    {
        frametype = HB_FRAME_IDR;
    }
    else if (qsv_frametype & MFX_FRAMETYPE_I)
    {
        frametype = HB_FRAME_I;
    }
    else if (qsv_frametype & MFX_FRAMETYPE_P)
    {
        frametype = HB_FRAME_P;
    }
    else if (qsv_frametype & MFX_FRAMETYPE_B)
    {
        frametype = HB_FRAME_B;
    }

    if (qsv_frametype & MFX_FRAMETYPE_REF)
    {
        flags |= HB_FRAME_REF;
    }

    if (out_flags != NULL)
    {
       *out_flags = flags;
    }
    return frametype;
}

int hb_qsv_impl_set_preferred(const char *name)
{
    if (name == NULL)
    {
        return -1;
    }
    if (!strcasecmp(name, "software"))
    {
        if (qsv_software_info_avc.available)
        {
            hb_qsv_info_avc = &qsv_software_info_avc;
        }
        if (qsv_software_info_hevc.available)
        {
            hb_qsv_info_hevc = &qsv_software_info_hevc;
        }
        return 0;
    }
    if (!strcasecmp(name, "hardware"))
    {
        if (qsv_hardware_info_avc.available)
        {
            hb_qsv_info_avc = &qsv_hardware_info_avc;
        }
        if (qsv_hardware_info_hevc.available)
        {
            hb_qsv_info_hevc = &qsv_hardware_info_hevc;
        }
        return 0;
    }
    return -1;
}

const char* hb_qsv_impl_get_name(int impl)
{
    switch (MFX_IMPL_BASETYPE(impl))
    {
        case MFX_IMPL_SOFTWARE:
            return "software";

        case MFX_IMPL_HARDWARE:
            return "hardware (1)";
        case MFX_IMPL_HARDWARE2:
            return "hardware (2)";
        case MFX_IMPL_HARDWARE3:
            return "hardware (3)";
        case MFX_IMPL_HARDWARE4:
            return "hardware (4)";
        case MFX_IMPL_HARDWARE_ANY:
            return "hardware (any)";

        case MFX_IMPL_AUTO:
            return "automatic";
        case MFX_IMPL_AUTO_ANY:
            return "automatic (any)";

        default:
            return NULL;
    }
}

void hb_qsv_force_workarounds()
{
    qsv_software_info_avc.capabilities  &= ~HB_QSV_CAP_MSDK_API_1_6;
    qsv_hardware_info_avc.capabilities  &= ~HB_QSV_CAP_MSDK_API_1_6;
    qsv_software_info_hevc.capabilities &= ~HB_QSV_CAP_MSDK_API_1_6;
    qsv_hardware_info_hevc.capabilities &= ~HB_QSV_CAP_MSDK_API_1_6;
}

#endif // USE_QSV
