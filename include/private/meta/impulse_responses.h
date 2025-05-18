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

#ifndef PRIVATE_META_IMPULSE_RESPONSES_H_
#define PRIVATE_META_IMPULSE_RESPONSES_H_

#include <lsp-plug.in/plug-fw/meta/types.h>
#include <lsp-plug.in/plug-fw/const.h>


namespace lsp
{
    namespace meta
    {
        struct impulse_responses_metadata
        {
            static constexpr float FILE_PITCH_MIN           = -24.0f;   // Minimum pitch (st)
            static constexpr float FILE_PITCH_MAX           = 24.0f;    // Maximum pitch (st)
            static constexpr float FILE_PITCH_DFL           = 0.0f;     // Pitch (st)
            static constexpr float FILE_PITCH_STEP          = 0.01f;    // Pitch step (st)

            static constexpr float CONV_LENGTH_MIN          = 0.0f;     // Minimum convolution length (ms)
            static constexpr float CONV_LENGTH_MAX          = 10000.0f; // Maximum convoluition length (ms)
            static constexpr float CONV_LENGTH_DFL          = 0.0f;     // Convolution length (ms)
            static constexpr float CONV_LENGTH_STEP         = 0.1f;     // Convolution step (ms)

            static constexpr float PREDELAY_MIN             = 0.0f;     // Minimum pre-delay length (ms)
            static constexpr float PREDELAY_MAX             = 100.0f;   // Maximum pre-delay length (ms)
            static constexpr float PREDELAY_DFL             = 0.0f;     // Pre-delay length (ms)
            static constexpr float PREDELAY_STEP            = 0.01f;    // Pre-delay step (ms)

            static constexpr float LCF_MIN                  = 10.0f;
            static constexpr float LCF_MAX                  = 1000.0f;
            static constexpr float LCF_DFL                  = 50.0f;
            static constexpr float LCF_STEP                 = 0.001f;

            static constexpr float HCF_MIN                  = 2000.0f;
            static constexpr float HCF_MAX                  = 22000.0f;
            static constexpr float HCF_DFL                  = 10000.0f;
            static constexpr float HCF_STEP                 = 0.001f;

            static constexpr float BA_MIN                   = GAIN_AMP_M_12_DB;
            static constexpr float BA_MAX                   = GAIN_AMP_P_12_DB;
            static constexpr float BA_DFL                   = GAIN_AMP_0_DB;
            static constexpr float BA_STEP                  = 0.0025f;

            static constexpr size_t EQ_BANDS                = 8;        // 8 bands for equalization

            static constexpr size_t MESH_SIZE               = 600;      // Maximum mesh size
            static constexpr size_t TRACKS_MAX              = 2;        // Maximum tracks per mesh/sample
            static constexpr size_t FILES_MAX               = 2;        // Maximum number of files

            static constexpr size_t FFT_RANK_MIN            = 9;        // Minimum FFT rank

            enum fft_rank_t
            {
                FFT_RANK_512,
                FFT_RANK_1024,
                FFT_RANK_2048,
                FFT_RANK_4096,
                FFT_RANK_8192,
                FFT_RANK_16384,
                FFT_RANK_32767,
                FFT_RANK_65536,

                FFT_RANK_DEFAULT = FFT_RANK_32767
            };
        };

        extern const meta::plugin_t impulse_responses_mono;
        extern const meta::plugin_t impulse_responses_stereo;
    } // namespace meta
} // namespace lsp


#endif /* PRIVATE_META_IMPULSE_RESPONSES_H_ */
