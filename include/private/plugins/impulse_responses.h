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

#ifndef PRIVATE_PLUGINS_IMPULSE_RESPONSES_H_
#define PRIVATE_PLUGINS_IMPULSE_RESPONSES_H_

#include <lsp-plug.in/plug-fw/plug.h>
#include <lsp-plug.in/dsp-units/ctl/Toggle.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/sampling/Sample.h>
#include <lsp-plug.in/dsp-units/sampling/SamplePlayer.h>
#include <lsp-plug.in/dsp-units/util/Convolver.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>

#include <private/meta/impulse_responses.h>

namespace lsp
{
    namespace plugins
    {
        /**
         * Impulse Responses Plugin Series
         */
        class impulse_responses: public plug::Module
        {
            protected:
                class IRLoader;

                typedef struct reconfig_t
                {
                    bool        bRender;
                    size_t      nSource;
                    size_t      nRank;
                } reconfig_t;

                typedef struct af_descriptor_t
                {
                    dspu::Sample       *pCurr;
                    dspu::Sample       *pSwap;

                    dspu::Toggle        sListen;                // Listen toggle
                    dspu::Sample       *pSwapSample;
                    dspu::Sample       *pCurrSample;            // Rendered file sample
                    float              *vThumbs[meta::impulse_responses_metadata::TRACKS_MAX];           // Thumbnails
                    float               fNorm;          // Norming factor
                    bool                bRender;        // Flag that indicates that file needs rendering
                    status_t            nStatus;
                    bool                bSync;          // Synchronize file
                    bool                bSwap;          // Swap samples

                    float               fHeadCut;
                    float               fTailCut;
                    float               fFadeIn;
                    float               fFadeOut;

                    IRLoader           *pLoader;        // Audio file loader task

                    plug::IPort        *pFile;          // Port that contains file name
                    plug::IPort        *pHeadCut;
                    plug::IPort        *pTailCut;
                    plug::IPort        *pFadeIn;
                    plug::IPort        *pFadeOut;
                    plug::IPort        *pListen;
                    plug::IPort        *pStatus;        // Status of file loading
                    plug::IPort        *pLength;        // Length of file
                    plug::IPort        *pThumbs;        // Thumbnails of file
                } af_descriptor_t;

                typedef struct channel_t
                {
                    dspu::Bypass        sBypass;
                    dspu::Delay         sDelay;
                    dspu::SamplePlayer  sPlayer;
                    dspu::Equalizer     sEqualizer;     // Wet signal equalizer

                    dspu::Convolver    *pCurr;
                    dspu::Convolver    *pSwap;

                    float              *vIn;
                    float              *vOut;
                    float              *vBuffer;
                    float               fDryGain;
                    float               fWetGain;
                    size_t              nSource;
                    size_t              nSourceReq;
                    size_t              nRank;
                    size_t              nRankReq;

                    plug::IPort        *pIn;
                    plug::IPort        *pOut;

                    plug::IPort        *pSource;
                    plug::IPort        *pMakeup;
                    plug::IPort        *pActivity;
                    plug::IPort        *pPredelay;

                    plug::IPort        *pWetEq;         // Wet equalization flag
                    plug::IPort        *pLowCut;        // Low-cut flag
                    plug::IPort        *pLowFreq;       // Low-cut frequency
                    plug::IPort        *pHighCut;       // High-cut flag
                    plug::IPort        *pHighFreq;      // Low-cut frequency
                    plug::IPort        *pFreqGain[meta::impulse_responses_metadata::EQ_BANDS];   // Gain for each band of the Equalizer
                } channel_t;

                class IRLoader: public ipc::ITask
                {
                    private:
                        impulse_responses          *pCore;
                        af_descriptor_t            *pDescr;

                    public:
                        explicit IRLoader(impulse_responses *base, af_descriptor_t *descr);
                        virtual ~IRLoader();

                    public:
                        virtual status_t run();

                        void        dump(dspu::IStateDumper *v) const;
                };

                class IRConfigurator: public ipc::ITask
                {
                    private:
                        reconfig_t                  sReconfig[meta::impulse_responses_metadata::TRACKS_MAX];
                        impulse_responses          *pCore;

                    public:
                        explicit IRConfigurator(impulse_responses *base);
                        virtual ~IRConfigurator();

                    public:
                        virtual status_t run();
                        void        dump(dspu::IStateDumper *v) const;

                        inline void set_render(size_t idx, bool render)     { sReconfig[idx].bRender    = render; }
                        inline void set_source(size_t idx, size_t source)   { sReconfig[idx].nSource    = source; }
                        inline void set_rank(size_t idx, size_t rank)       { sReconfig[idx].nRank      = rank; }
                };

            protected:
                status_t                load(af_descriptor_t *descr);
                status_t                reconfigure(const reconfig_t *cfg);
                static void             destroy_file(af_descriptor_t *af);
                static void             destroy_channel(channel_t *c);
                static size_t           get_fft_rank(size_t rank);

            protected:
                IRConfigurator          sConfigurator;

                size_t                  nChannels;
                channel_t              *vChannels;
                af_descriptor_t        *vFiles;
                ipc::IExecutor         *pExecutor;
                size_t                  nReconfigReq;
                size_t                  nReconfigResp;
                float                   fGain;

                plug::IPort            *pBypass;
                plug::IPort            *pRank;
                plug::IPort            *pDry;
                plug::IPort            *pWet;
                plug::IPort            *pOutGain;

                uint8_t                *pData;

            public:
                explicit impulse_responses(const meta::plugin_t *meta);
                virtual ~impulse_responses();

                virtual void        init(plug::IWrapper *wrapper, plug::IPort **ports);
                virtual void        destroy();

            public:
                virtual void        ui_activated();
                virtual void        update_settings();
                virtual void        update_sample_rate(long sr);

                virtual void        process(size_t samples);

                virtual void        dump(dspu::IStateDumper *v) const;
        };
    } // namespace plugins
} // namespace lsp

#endif /* PRIVATE_PLUGINS_IMPULSE_RESPONSES_H_ */
