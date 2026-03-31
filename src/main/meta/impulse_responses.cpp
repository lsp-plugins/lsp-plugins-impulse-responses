/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
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
#include <lsp-plug.in/plug-fw/meta/registry.h>
#include <lsp-plug.in/shared/meta/developers.h>
#include <lsp-plug.in/common/status.h>

#include <private/meta/impulse_responses.h>

#define LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MAJOR       1
#define LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MINOR       0
#define LSP_PLUGINS_IMPULSE_RESPONSES_VERSION_MICRO       36

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
        // Lisf of different revisions for adding controls
        #define REV_0       0
        #define REV_1       1

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
            COMBO("fft", "FFT size", "FFT size", impulse_responses_metadata::FFT_RANK_DEFAULT, ir_fft_rank), \
            DRY_GAIN(1.0f), \
            WET_GAIN(1.0f), \
            DRYWET(100.0f), \
            OUT_GAIN

        #define IR_SAMPLE_FILE(id, label)   \
            PATH("ifn" id, "Impulse file" label),    \
            CONTROL("psh" id, "File pitch" label, NULL, U_SEMITONES, impulse_responses_metadata::FILE_PITCH), \
            CONTROL("ihc" id, "Head cut" label, NULL, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            CONTROL("itc" id, "Tail cut" label, NULL, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            CONTROL("ifi" id, "Fade in" label, NULL, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            CONTROL("ifo" id, "Fade out" label, NULL, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            TRIGGER("ils" id, "Impulse preview listen" label, "Play" label), \
            TRIGGER("ilc" id, "Impulse preview stop" label, "Stop" label), \
            SWITCH("irv" id, "Impulse reverse" label, "Reverse" label, 0.0f), \
            STATUS("ifs" id, "Load status" label), \
            METER("ifl" id, "Impulse length" label, U_MSEC, impulse_responses_metadata::CONV_LENGTH), \
            MESH("ifd" id, "Impulse file contents" label, impulse_responses_metadata::TRACKS_MAX, impulse_responses_metadata::MESH_SIZE)

        #define IR_SOURCE(id, label, alias, select, dfl) \
            COMBO("cs" id, "Channel source" label, "Source" alias, dfl, select), \
            AMP_GAIN100("mk" id, "Makeup gain" label, "Makeup" alias, 1.0f), \
            BLINK("ca" id, "Channel activity" label), \
            CONTROL("pd" id, "Pre-delay" label, "Pre-delay" alias, U_MSEC, impulse_responses_metadata::PREDELAY)

        #define IR_EQ_BAND(rev, id, name, alias, freq)    \
            ADDON_CONTROL(rev, "eq_" id, "Band " name freq "Hz gain", "Eq " alias freq, U_GAIN_AMP, impulse_responses_metadata::BA)

        #define IR_EQ_BANDS(rev, id, name, alias) \
            ADDON_COMBO(rev, "lcm" id, "Low-cut mode" name, "LC mode" alias, 0, filter_slope),      \
            ADDON_LOG_CONTROL(rev, "lcf" id, "Low-cut frequency" name, "LC freq" alias, U_HZ, impulse_responses_metadata::LCF),   \
            IR_EQ_BAND(rev, "0" id, name, alias, "50"), \
            IR_EQ_BAND(rev, "1" id, name, alias, "107"), \
            IR_EQ_BAND(rev, "2" id, name, alias, "227"), \
            IR_EQ_BAND(rev, "3" id, name, alias, "484"), \
            IR_EQ_BAND(rev, "4" id, name, alias, "1 k"), \
            IR_EQ_BAND(rev, "5" id, name, alias, "2.2 k"), \
            IR_EQ_BAND(rev, "6" id, name, alias, "4.7 k"), \
            IR_EQ_BAND(rev, "7" id, name, alias, "10 k"), \
            ADDON_COMBO(rev, "hcm" id, "High-cut mode" name, "HC mode" alias, 0, filter_slope),      \
            ADDON_LOG_CONTROL(rev, "hcf" id, "High-cut frequency" name, "HC freq" alias, U_HZ, impulse_responses_metadata::HCF)

        #define IR_EQ_BANDS_MONO \
            IR_EQ_BANDS(REV_0, "", "", "")

        #define IR_EQ_BANDS_STEREO \
            ADDON_SWITCH(REV_1, "ssplit", "Stereo equalizer split", "Eq split", 0.0f), \
            IR_EQ_BANDS(REV_0, "", "", ""), \
            IR_EQ_BANDS(REV_1, "r", "Right ", "R ")

        #define IR_EQUALIZER(bands)    \
            SWITCH("wpp", "Wet post-process", "Wet postproc", 0),    \
            SWITCH("eqv", "Equalizer visibility", "Show Eq", 0),    \
            bands

        static const port_t impulse_responses_mono_ports[] =
        {
            // Input audio ports
            PORTS_MONO_PLUGIN,
            IR_COMMON,

            // Input controls
            IR_SAMPLE_FILE("", ""),
            IR_SOURCE("", "", "", ir_source_mono, 1),
            IR_EQUALIZER(IR_EQ_BANDS_MONO),

            PORTS_END
        };

        static const port_t impulse_responses_stereo_ports[] =
        {
            // Input audio ports
            PORTS_STEREO_PLUGIN,
            IR_COMMON,
            COMBO("fsel", "File selector", "File selector", 0, ir_file_select), \

            // Input controls
            IR_SAMPLE_FILE("0", " 1"),
            IR_SAMPLE_FILE("1", " 2"),
            IR_SOURCE("_l", " Left", " L", ir_source_stereo, 1),
            IR_SOURCE("_r", " Right", " R", ir_source_stereo, 2),
            IR_EQUALIZER(IR_EQ_BANDS_STEREO),

            PORTS_END
        };

        static const int plugin_classes[]           = { C_REVERB, -1 };
        static const int clap_features_mono[]       = { CF_AUDIO_EFFECT, CF_REVERB, CF_MONO, -1 };
        static const int clap_features_stereo[]     = { CF_AUDIO_EFFECT, CF_REVERB, CF_STEREO, -1 };

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
            "Impulse Responses Mono",
            "IA1M",
            &developers::v_sadovnikov,
            "impulse_responses_mono",
            {
                LSP_LV2_URI("impulse_responses_mono"),
                LSP_LV2UI_URI("impulse_responses_mono"),
                "wvwt",
                LSP_VST3_UID("ia1m    wvwt"),
                LSP_VST3UI_UID("ia1m    wvwt"),
                0,
                NULL,
                LSP_CLAP_URI("impulse_responses_mono"),
                LSP_GST_UID("impulse_responses_mono"),
            },
            LSP_PLUGINS_IMPULSE_RESPONSES_VERSION,
            plugin_classes,
            clap_features_mono,
            E_DUMP_STATE | E_FILE_PREVIEW,
            impulse_responses_mono_ports,
            "plugins/convolution/impulse_responses/mono.xml",
            NULL,
            mono_plugin_port_groups,
            &impulse_responses_bundle,
            2
        };
        LSP_REGISTER_METADATA(impulse_responses_mono);

        const meta::plugin_t  impulse_responses_stereo =
        {
            "Impulsantworten Stereo",
            "Impulse Responses Stereo",
            "Impulse Responses Stereo",
            "IA1S",
            &developers::v_sadovnikov,
            "impulse_responses_stereo",
            {
                LSP_LV2_URI("impulse_responses_stereo"),
                LSP_LV2UI_URI("impulse_responses_stereo"),
                "1khz",
                LSP_VST3_UID("ia1s    1khz"),
                LSP_VST3UI_UID("ia1s    1khz"),
                0,
                NULL,
                LSP_CLAP_URI("impulse_responses_stereo"),
                LSP_GST_UID("impulse_responses_stereo"),
            },
            LSP_PLUGINS_IMPULSE_RESPONSES_VERSION,
            plugin_classes,
            clap_features_stereo,
            E_DUMP_STATE | E_FILE_PREVIEW,
            impulse_responses_stereo_ports,
            "plugins/convolution/impulse_responses/stereo.xml",
            NULL,
            stereo_plugin_port_groups,
            &impulse_responses_bundle,
            1
        };
        LSP_REGISTER_METADATA(impulse_responses_stereo);

    } // namespace meta
} // namespace lsp
