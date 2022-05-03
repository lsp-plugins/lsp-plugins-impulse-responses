/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins-impulse-responses
 * Created on: 3 авг. 2021 г.
 *
 * lsp-plugins-impulse-responses is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins-impulse-responses is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins-impulse-responses. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/plug-fw/meta/ports.h>
#include <lsp-plug.in/shared/meta/developers.h>
#include <lsp-plug.in/common/status.h>

#include <private/meta/impulse_responses.h>

#define LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MAJOR       1
#define LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MINOR       0
#define LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MICRO       5

#define LSP_PLUGINS_IMPULSE_RESPONSES_VERSION  \
    LSP_MODULE_VERSION( \
        LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MAJOR, \
        LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MINOR, \
        LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MICRO  \
    )

namespace lsp
{
    namespace meta
    {
        //-------------------------------------------------------------------------
        // Impulse responses
        static const port_item_t ir_source_mono[] =
        {
            { "None",           "file.none" },
            { "Left",           "file.left" },
            { "Right",          "file.right" },
            { NULL, NULL }
        };

        static const port_item_t ir_source_stereo[] =
        {
            { "None",           "file.none" },
            { "File 1 Left",    "file.f1l" },
            { "File 1 Right",   "file.f1r" },
            { "File 2 Left",    "file.f2l" },
            { "File 2 Right",   "file.f2r" },
            { NULL, NULL }
        };

        static const port_item_t ir_fft_rank[] =
        {
            { "512",            NULL },
            { "1024",           NULL },
            { "2048",           NULL },
            { "4096",           NULL },
            { "8192",           NULL },
            { "16384",          NULL },
            { "32768",          NULL },
            { "65536",          NULL },
            { NULL, NULL }
        };

        static const port_item_t ir_file_select[] =
        {
            { "File 1",         "file.f1" },
            { "File 2",         "file.f2" },
            { NULL, NULL }
        };

        static const port_item_t filter_slope[] =
        {
            { "off",            "eq.slope.off" },
            { "12 dB/oct",      "eq.slope.12dbo" },
            { "24 dB/oct",      "eq.slope.24dbo" },
            { "36 dB/oct",      "eq.slope.36dbo" },
            { NULL, NULL }
        };

        #define IR_COMMON \
            BYPASS, \
            COMBO("fft", "FFT size", impulse_responses_metadata::FFT_RANK_DEFAULT, ir_fft_rank), \
            DRY_GAIN(1.0f), \
            WET_GAIN(1.0f), \
            OUT_GAIN

        #define IR_SAMPLE_FILE(id, label)   \
            PATH("ifn" id, "Impulse file" label),    \
            CONTROL("ihc" id, "Head cut" label, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            CONTROL("itc" id, "Tail cut" label, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            CONTROL("ifi" id, "Fade in" label, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            CONTROL("ifo" id, "Fade out" label, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            TRIGGER("ils" id, "Impulse listen" label), \
            STATUS("ifs" id, "Load status" label), \
            METER("ifl" id, "Impulse length" label, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            MESH("ifd" id, "Impulse file contents" label, impulse_responses_metadata::TRACKS_MAX, impulse_responses_metadata::MESH_SIZE)

        #define IR_SOURCE(id, label, select) \
            COMBO("cs" id, "Channel source" label, 0, select), \
            AMP_GAIN100("mk" id, "Makeup gain" label, 1.0f), \
            BLINK("ca" id, "Channel activity" label), \
            CONTROL("pd" id, "Pre-delay" label, U_MSEC, impulse_responses_metadata::PREDELAY)

        #define IR_EQ_BAND(id, freq)    \
            CONTROL("eq_" #id, "Band " freq "Hz gain", U_GAIN_AMP, impulse_responses_metadata::BA)

        #define IR_EQUALIZER    \
            SWITCH("wpp", "Wet post-process", 0),    \
            SWITCH("eqv", "Equalizer visibility", 0),    \
            COMBO("lcm", "Low-cut mode", 0, filter_slope),      \
            LOG_CONTROL("lcf", "Low-cut frequency", U_HZ, impulse_responses_metadata::LCF),   \
            IR_EQ_BAND(0, "50"), \
            IR_EQ_BAND(1, "107"), \
            IR_EQ_BAND(2, "227"), \
            IR_EQ_BAND(3, "484"), \
            IR_EQ_BAND(4, "1 k"), \
            IR_EQ_BAND(5, "2.2 k"), \
            IR_EQ_BAND(6, "4.7 k"), \
            IR_EQ_BAND(7, "10 k"), \
            COMBO("hcm", "High-cut mode", 0, filter_slope),      \
            LOG_CONTROL("hcf", "High-cut frequency", U_HZ, impulse_responses_metadata::HCF)

        static const port_t impulse_responses_mono_ports[] =
        {
            // Input audio ports
            PORTS_MONO_PLUGIN,
            IR_COMMON,

            // Input controls
            IR_SAMPLE_FILE("", ""),
            IR_SOURCE("", "", ir_source_mono),
            IR_EQUALIZER,

            PORTS_END
        };

        static const port_t impulse_responses_stereo_ports[] =
        {
            // Input audio ports
            PORTS_STEREO_PLUGIN,
            IR_COMMON,
            COMBO("fsel", "File selector", 0, ir_file_select), \

            // Input controls
            IR_SAMPLE_FILE("0", " 1"),
            IR_SAMPLE_FILE("1", " 2"),
            IR_SOURCE("_l", " Left", ir_source_stereo),
            IR_SOURCE("_r", " Right", ir_source_stereo),
            IR_EQUALIZER,

            PORTS_END
        };

        static const int impulse_responses_classes[] = { C_REVERB, -1 };

        const meta::bundle_t impulse_responses_bundle =
        {
            "impulse_responses",
            "Impulse Responses",
            B_CONVOLUTION,
            "BBcLpAx02UM",
            "This plugin performs highly optimized real time zero-latency convolution\nto the input signal. It can be used as a cabinet emulator, some sort of\nequalizer or as a reverb simulation plugin. All what is needed is audio\nfile with impulse response taken from the linear system (cabinet, equalizer\nor hall/room)."
        };

        const meta::plugin_t  impulse_responses_mono =
        {
            "Impulsantworten Mono",
            "Impulse Responses Mono",
            "IA1M",
            &developers::v_sadovnikov,
            "impulse_responses_mono",
            LSP_LV2_URI("impulse_responses_mono"),
            LSP_LV2UI_URI("impulse_responses_mono"),
            "wvwt",
            0,
            NULL,
            LSP_PLUGINS_IMPULSE_RESPONSES_VERSION,
            impulse_responses_classes,
            E_DUMP_STATE,
            impulse_responses_mono_ports,
            "convolution/impulse_responses/mono.xml",
            NULL,
            mono_plugin_port_groups,
            &impulse_responses_bundle
        };

        const meta::plugin_t  impulse_responses_stereo =
        {
            "Impulsantworten Stereo",
            "Impulse Responses Stereo",
            "IA1S",
            &developers::v_sadovnikov,
            "impulse_responses_stereo",
            LSP_LV2_URI("impulse_responses_stereo"),
            LSP_LV2UI_URI("impulse_responses_stereo"),
            "1khz",
            0,
            NULL,
            LSP_PLUGINS_IMPULSE_RESPONSES_VERSION,
            impulse_responses_classes,
            E_DUMP_STATE,
            impulse_responses_stereo_ports,
            "convolution/impulse_responses/stereo.xml",
            NULL,
            stereo_plugin_port_groups,
            &impulse_responses_bundle
        };
    } // namespace meta
} // namespace lsp
