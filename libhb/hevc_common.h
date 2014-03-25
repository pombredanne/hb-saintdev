/*
 * hevc_common.h
 *
 * Copyright (c) 2003-2014 HandBrake Team
 * This file is part of the HandBrake source code
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HB_HEVC_COMMON_H
#define HB_HEVC_COMMON_H

static const char* const   hb_hevc_profile_names[] = { "auto", "main", NULL, };
static const char* const     hb_hevc_level_names[] = { "auto", "1.0", "2.0", "2.1", "3.0", "3.1", "4.0", "4.1", "5.0", "5.1", "5.2", "6.0", "6.1", "6.2", NULL, };
static const int   const    hb_hevc_level_values[] = {     -1,    10,   20,    21,    30,    31,    40,    41,    50,    51,    52,    60,    61,    62,     0, };

// stolen from libx265's x265.h
static const char* const hb_hevc_fullrange_names[] = { "off", "on", NULL, };
static const char* const hb_hevc_vidformat_names[] = { "component", "pal", "ntsc", "secam", "mac", "undef", NULL, };
static const char* const hb_hevc_colorprim_names[] = { "", "bt709", "undef", "", "bt470m", "bt470bg", "smpte170m", "smpte240m", "film", "bt2020", NULL, };
static const char* const  hb_hevc_transfer_names[] = { "", "bt709", "undef", "", "bt470m", "bt470bg", "smpte170m", "smpte240m", "linear", "log100", "log316", "iec61966-2-4", "bt1361e", "iec61966-2-1", "bt2020-10", "bt2020-12", NULL, };
static const char* const hb_hevc_colmatrix_names[] = { "GBR", "bt709", "undef", "", "fcc", "bt470bg", "smpte170m", "smpte240m", "YCgCo", "bt2020nc", "bt2020c", NULL, };

#endif // HB_HEVC_COMMON_H
