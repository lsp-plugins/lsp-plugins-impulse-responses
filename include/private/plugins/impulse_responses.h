/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
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

                typedef struct af_descriptor_t
                {
                    dspu::Toggle        sListen;        // Listen toggle
                    dspu::Sample       *pOriginal;      // Original file sample
                    dspu::Sample       *pProcessed;     // Processed file sample by the reconfigure() call
                    float              *vThumbs[meta::impulse_responses_metadata::TRACKS_MAX];           // Thumbnails
                    float               fNorm;          // Norming factor
                    status_t            nStatus;
                    bool                bSync;          // Synchronize file

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
                        virtual ~IRLoader() override;

                    public:
                        virtual status_t run() override;

                        void        dump(dspu::IStateDumper *v) const;
                };

                class IRConfigurator: public ipc::ITask
                {
                    private:
                        impulse_responses          *pCore;

                    public:
                        explicit IRConfigurator(impulse_responses *base);
                        virtual ~IRConfigurator() override;

                    public:
                        virtual status_t run() override;
                        void        dump(dspu::IStateDumper *v) const;
                };

                class GCTask: public ipc::ITask
                {
                    private:
                        impulse_responses          *pCore;

                    public:
                        explicit GCTask(impulse_responses *base);
                        virtual ~GCTask() override;

                    public:
                        virtual status_t run() override;

                        void        dump(dspu::IStateDumper *v) const;
                };

            protected:
                bool                    has_active_loading_tasks();
                status_t                load(af_descriptor_t *descr);
                status_t                reconfigure();
                void                    process_configuration_tasks();
                void                    process_loading_tasks();
                void                    process_gc_events();
                void                    process_listen_events();
                void                    perform_convolution(size_t samples);
                void                    output_parameters();
                void                    perform_gc();

            protected:
                static void             destroy_samples(dspu::Sample *gc_list);
                static void             destroy_sample(dspu::Sample * &s);
                static void             destroy_convolver(dspu::Convolver * &c);
                static void             destroy_file(af_descriptor_t *af);
                static void             destroy_channel(channel_t *c);
                static size_t           get_fft_rank(size_t rank);

            protected:
                IRConfigurator          sConfigurator;
                GCTask                  sGCTask;

                size_t                  nChannels;
                channel_t              *vChannels;
                af_descriptor_t        *vFiles;
                ipc::IExecutor         *pExecutor;
                size_t                  nReconfigReq;
                size_t                  nReconfigResp;
                float                   fGain;
                size_t                  nRank;
                dspu::Sample           *pGCList;        // Garbage collection list

                plug::IPort            *pBypass;
                plug::IPort            *pRank;
                plug::IPort            *pDry;
                plug::IPort            *pWet;
                plug::IPort            *pOutGain;

                uint8_t                *pData;

            protected:
                void                do_destroy();

            public:
                explicit impulse_responses(const meta::plugin_t *meta);
                impulse_responses(const impulse_responses &) = delete;
                impulse_responses(impulse_responses &&) = delete;
                virtual ~impulse_responses() override;

                impulse_responses & operator = (const impulse_responses &) = delete;
                impulse_responses & operator = (impulse_responses &&) = delete;

                virtual void        init(plug::IWrapper *wrapper, plug::IPort **ports) override;
                virtual void        destroy() override;

            public:
                virtual void        ui_activated() override;
                virtual void        update_settings() override;
                virtual void        update_sample_rate(long sr) override;
                virtual void        process(size_t samples) override;
                virtual void        dump(dspu::IStateDumper *v) const override;
        };
    } /* namespace plugins */
} /* namespace lsp */

#endif /* PRIVATE_PLUGINS_IMPULSE_RESPONSES_H_ */
