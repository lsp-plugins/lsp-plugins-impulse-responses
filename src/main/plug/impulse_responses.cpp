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

#include <private/plugins/impulse_responses.h>
#include <lsp-plug.in/plug-fw/meta/func.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/misc/fade.h>
#include <lsp-plug.in/common/alloc.h>

#define TMP_BUF_SIZE            4096
#define CONV_RANK               10
#define TRACE_PORT(p)           lsp_trace("  port id=%s", (p)->metadata()->id);

namespace lsp
{
    namespace plugins
    {
        //---------------------------------------------------------------------
        // Plugin factory
        static const meta::plugin_t *plugins[] =
        {
            &meta::impulse_responses_mono,
            &meta::impulse_responses_stereo
        };

        static plug::Module *plugin_factory(const meta::plugin_t *meta)
        {
            return new impulse_responses(meta);
        }

        static plug::Factory factory(plugin_factory, plugins, 2);

        //-------------------------------------------------------------------------
        static float band_freqs[] =
        {
            73.0f,
            156.0f,
            332.0f,
            707.0f,
            1507.0f,
            3213.0f,
            6849.0f
        };

        //-------------------------------------------------------------------------
        impulse_responses::IRLoader::IRLoader(impulse_responses *base, af_descriptor_t *descr)
        {
            pCore       = base;
            pDescr      = descr;
        }

        impulse_responses::IRLoader::~IRLoader()
        {
            pCore       = NULL;
            pDescr      = NULL;
        }

        status_t impulse_responses::IRLoader::run()
        {
            return pCore->load(pDescr);
        }

        void impulse_responses::IRLoader::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
            v->write("pDescr", pDescr);
        }

        //-------------------------------------------------------------------------
        impulse_responses::IRConfigurator::IRConfigurator(impulse_responses *base)
        {
            pCore       = base;
            for (size_t i=0; i<meta::impulse_responses_metadata::TRACKS_MAX; ++i)
            {
                sReconfig[i].bRender    = false;
                sReconfig[i].nSource    = 0;
                sReconfig[i].nRank      = 0;
            }
        }

        impulse_responses::IRConfigurator::~IRConfigurator()
        {
            pCore       = NULL;
        }

        status_t impulse_responses::IRConfigurator::run()
        {
            return pCore->reconfigure(sReconfig);
        }

        void impulse_responses::IRConfigurator::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
            v->begin_array("sReconfig", &sReconfig, meta::impulse_responses_metadata::TRACKS_MAX);
            {
                for (size_t i=0; i<meta::impulse_responses_metadata::TRACKS_MAX; ++i)
                {
                    const reconfig_t *r = &sReconfig[i];
                    v->begin_object(r, sizeof(reconfig_t));
                    {
                        v->write("bRender", r->bRender);
                        v->write("nSource", r->nSource);
                        v->write("nRank", r->nRank);
                    }
                    v->end_object();
                }
            }
            v->end_array();
        }

        //-------------------------------------------------------------------------
        impulse_responses::impulse_responses(const meta::plugin_t *metadata):
            plug::Module(metadata),
            sConfigurator(this)
        {
            nChannels       = 0;
            for (const meta::port_t *p = metadata->ports; p->id != NULL; ++p)
                if ((meta::is_out_port(p)) && (meta::is_audio_port(p)))
                    ++nChannels;

            vChannels       = NULL;
            vFiles          = NULL;
            pExecutor       = NULL;
            nReconfigReq    = 0;
            nReconfigResp   = -1;
            fGain           = 1.0f;

            pBypass         = NULL;
            pRank           = NULL;
            pDry            = NULL;
            pWet            = NULL;
            pOutGain        = NULL;

            pData           = NULL;
        }

        impulse_responses::~impulse_responses()
        {
        }

        void impulse_responses::destroy_file(af_descriptor_t *af)
        {
            // Destroy sample
            if (af->pSwapSample != NULL)
            {
                af->pSwapSample->destroy();
                delete af->pSwapSample;
                af->pSwapSample = NULL;
            }
            if (af->pCurrSample != NULL)
            {
                af->pCurrSample->destroy();
                delete af->pCurrSample;
                af->pCurrSample = NULL;
            }

            // Destroy current file
            if (af->pCurr != NULL)
            {
                af->pCurr->destroy();
                delete af->pCurr;
                af->pCurr    = NULL;
            }

            if (af->pSwap != NULL)
            {
                af->pSwap->destroy();
                delete af->pSwap;
                af->pSwap    = NULL;
            }

            // Destroy loader
            if (af->pLoader != NULL)
            {
                delete af->pLoader;
                af->pLoader     = NULL;
            }

            // Forget port
            af->pFile       = NULL;
        }

        void impulse_responses::destroy_channel(channel_t *c)
        {
            if (c->pCurr != NULL)
            {
                c->pCurr->destroy();
                delete c->pCurr;
                c->pCurr    = NULL;
            }

            if (c->pSwap != NULL)
            {
                c->pSwap->destroy();
                delete c->pSwap;
                c->pSwap    = NULL;
            }

            c->sDelay.destroy();
            c->sPlayer.destroy(false);
            c->sEqualizer.destroy();
        }

        size_t impulse_responses::get_fft_rank(size_t rank)
        {
            return meta::impulse_responses_metadata::FFT_RANK_MIN + rank;
        }

        void impulse_responses::init(plug::IWrapper *wrapper)
        {
            // Pass wrapper
            plug::Module::init(wrapper);

            // Remember executor service
            pExecutor       = wrapper->executor();
            lsp_trace("Executor = %p", pExecutor);

            // Allocate buffer data
            size_t tmp_buf_size = TMP_BUF_SIZE * sizeof(float);
            size_t thumbs_size  = meta::impulse_responses_metadata::MESH_SIZE * sizeof(float);
            size_t thumbs_perc  = thumbs_size * meta::impulse_responses_metadata::TRACKS_MAX;
            size_t alloc        = (tmp_buf_size + thumbs_perc) * nChannels;
            pData               = new uint8_t[alloc + DEFAULT_ALIGN];
            if (pData == NULL)
                return;
            uint8_t *ptr    = align_ptr(pData, DEFAULT_ALIGN);

            // Allocate channels
            vChannels       = new channel_t[nChannels];
            if (vChannels == NULL)
                return;

            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c    = &vChannels[i];

                if (!c->sPlayer.init(nChannels, 32))
                    return;
                if (!c->sEqualizer.init(meta::impulse_responses_metadata::EQ_BANDS + 2, CONV_RANK))
                    return;
                c->sEqualizer.set_mode(dspu::EQM_BYPASS);

                c->pCurr        = NULL;
                c->pSwap        = NULL;

                c->vIn          = NULL;
                c->vOut         = NULL;
                c->vBuffer      = reinterpret_cast<float *>(ptr);
                ptr            += tmp_buf_size;

                c->fDryGain     = 0.0f;
                c->fWetGain     = 1.0f;
                c->nSource      = 0;
                c->nSourceReq   = 0;
                c->nRank        = 0;
                c->nRankReq     = 0;

                c->pIn          = NULL;
                c->pOut         = NULL;

                c->pSource      = NULL;
                c->pMakeup      = NULL;
                c->pActivity    = NULL;
                c->pPredelay    = NULL;

                c->pWetEq       = NULL;
                c->pLowCut      = NULL;
                c->pLowFreq     = NULL;
                c->pHighCut     = NULL;
                c->pHighFreq    = NULL;

                for (size_t j=0; j<meta::impulse_responses_metadata::EQ_BANDS; ++j)
                    c->pFreqGain[j]     = NULL;
            }

            // Allocate files
            vFiles          = new af_descriptor_t[nChannels];
            if (vFiles == NULL)
                return;

            for (size_t i=0; i<nChannels; ++i)
            {
                af_descriptor_t *f    = &vFiles[i];

                f->pCurr        = NULL;
                f->pSwap        = NULL;
                f->pSwapSample  = NULL;
                f->pCurrSample  = NULL;

                for (size_t j=0; j<meta::impulse_responses_metadata::TRACKS_MAX; ++j, ptr += thumbs_size)
                    f->vThumbs[j]   = reinterpret_cast<float *>(ptr);

                f->fNorm        = 1.0f;
                f->bRender      = false;
                f->nStatus      = STATUS_UNSPECIFIED;
                f->bSync        = true;
                f->bSwap        = false;
                f->fHeadCut     = 0.0f;
                f->fTailCut     = 0.0f;
                f->fFadeIn      = 0.0f;
                f->fFadeOut     = 0.0f;

                f->pLoader      = new IRLoader(this, f);
                if (f->pLoader == NULL)
                    return;
                f->pFile        = NULL;
                f->pHeadCut     = NULL;
                f->pTailCut     = NULL;
                f->pFadeIn      = NULL;
                f->pFadeOut     = NULL;
                f->pListen      = NULL;
                f->pStatus      = NULL;
                f->pLength      = NULL;
                f->pThumbs      = NULL;
            }

            // Bind ports
            size_t port_id = 0;

            lsp_trace("Binding audio ports");
            for (size_t i=0; i<nChannels; ++i)
            {
                TRACE_PORT(vPorts[port_id]);
                vChannels[i].pIn    = vPorts[port_id++];
            }
            for (size_t i=0; i<nChannels; ++i)
            {
                TRACE_PORT(vPorts[port_id]);
                vChannels[i].pOut   = vPorts[port_id++];
            }

            // Bind common ports
            lsp_trace("Binding common ports");
            TRACE_PORT(vPorts[port_id]);
            pBypass     = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            pRank       = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            pDry        = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            pWet        = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            pOutGain    = vPorts[port_id++];

            // Skip file selector
            if (nChannels > 1)
            {
                TRACE_PORT(vPorts[port_id]);
                port_id++;
            }

            // Bind impulse file ports
            for (size_t i=0; i<nChannels; ++i)
            {
                lsp_trace("Binding impulse file #%d ports", int(i));
                af_descriptor_t *f  = &vFiles[i];

                f->sListen.init();

                TRACE_PORT(vPorts[port_id]);
                f->pFile        = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pHeadCut     = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pTailCut     = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pFadeIn      = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pFadeOut     = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pListen      = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pStatus      = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pLength      = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                f->pThumbs      = vPorts[port_id++];
            }

            // Bind convolution ports
            for (size_t i=0; i<nChannels; ++i)
            {
                lsp_trace("Binding convolution #%d ports", int(i));
                channel_t *c    = &vChannels[i];

                TRACE_PORT(vPorts[port_id]);
                c->pSource      = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                c->pMakeup      = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                c->pActivity    = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                c->pPredelay    = vPorts[port_id++];
            }

            // Bind wet processing ports
            lsp_trace("Binding wet processing ports");
            size_t port         = port_id;
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];

                TRACE_PORT(vPorts[port_id]);
                c->pWetEq           = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                port_id++;          // Skip equalizer visibility port
                TRACE_PORT(vPorts[port_id]);
                c->pLowCut          = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                c->pLowFreq         = vPorts[port_id++];

                for (size_t j=0; j<meta::impulse_responses_metadata::EQ_BANDS; ++j)
                {
                    TRACE_PORT(vPorts[port_id]);
                    c->pFreqGain[j]     = vPorts[port_id++];
                }

                TRACE_PORT(vPorts[port_id]);
                c->pHighCut         = vPorts[port_id++];
                TRACE_PORT(vPorts[port_id]);
                c->pHighFreq        = vPorts[port_id++];

                port_id         = port;
            }

        }

        void impulse_responses::destroy()
        {
            // Drop buffers
            if (vChannels != NULL)
            {
                for (size_t i=0; i<nChannels; ++i)
                    destroy_channel(&vChannels[i]);
                delete [] vChannels;
                vChannels       = NULL;
            }

            if (vFiles != NULL)
            {
                for (size_t i=0; i<nChannels; ++i)
                    destroy_file(&vFiles[i]);

                delete [] vFiles;
                vFiles          = NULL;
            }

            if (pData != NULL)
            {
                delete [] pData;
                pData           = NULL;
            }
        }

        void impulse_responses::ui_activated()
        {
            // Force file contents to be synchronized with UI
            for (size_t i=0; i<nChannels; ++i)
                vFiles[i].bSync     = true;
        }

        void impulse_responses::update_settings()
        {
            fGain               = pOutGain->value();

            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];
                af_descriptor_t *f  = &vFiles[i];

                c->fDryGain         = pDry->value() * fGain;
                c->fWetGain         = pWet->value() * c->pMakeup->value() * fGain;

                // Update delay and bypass configuration
                c->sPlayer.set_gain(fGain);
                c->sDelay.set_delay(dspu::millis_to_samples(fSampleRate, c->pPredelay->value()));
                c->sBypass.set_bypass(pBypass->value() >= 0.5f);

                // Check that file parameters have changed
                float head_cut      = f->pHeadCut->value();
                float tail_cut      = f->pTailCut->value();
                float fade_in       = f->pFadeIn->value();
                float fade_out      = f->pFadeOut->value();
                if ((f->fHeadCut != head_cut) ||
                    (f->fTailCut != tail_cut) ||
                    (f->fFadeIn  != fade_in ) ||
                    (f->fFadeOut != fade_out))
                {
                    f->fHeadCut         = head_cut;
                    f->fTailCut         = tail_cut;
                    f->fFadeIn          = fade_in;
                    f->fFadeOut         = fade_out;
                    f->bRender          = true;
                    nReconfigReq        ++;
                }

                // Listen button pressed?
                if (f->pListen != NULL)
                    f->sListen.submit(f->pListen->value());

                if (f->sListen.pending())
                {
                    lsp_trace("Submitted listen toggle");
                    size_t n_c = (f->pCurrSample != NULL) ? f->pCurrSample->channels() : 0;
                    if (n_c > 0)
                    {
                        for (size_t j=0; j<nChannels; ++j)
                            vChannels[j].sPlayer.play(i, j%n_c, 1.0f, 0);
                    }
                    f->sListen.commit();
                }

                size_t source       = c->pSource->value();
                size_t rank         = get_fft_rank(pRank->value());
                if ((source != c->nSourceReq) || (rank != c->nRankReq))
                {
                    nReconfigReq        ++;
                    c->nSourceReq       = source;
                    c->nRankReq         = rank;
                }

                // Get path
                plug::path_t *path      = f->pFile->buffer<plug::path_t>();
                if ((path != NULL) && (path->pending()) && (f->pLoader->idle()))
                {
                    // Try to submit task
                    if (pExecutor->submit(f->pLoader))
                    {
                        lsp_trace("successfully submitted load task");
                        f->nStatus      = STATUS_LOADING;
                        path->accept();
                    }
                }

                // Update equalization parameters
                dspu::Equalizer *eq             = &c->sEqualizer;
                dspu::equalizer_mode_t eq_mode  = (c->pWetEq->value() >= 0.5f) ? dspu::EQM_IIR : dspu::EQM_BYPASS;
                eq->set_mode(eq_mode);

                if (eq_mode != dspu::EQM_BYPASS)
                {
                    dspu::filter_params_t fp;
                    size_t band     = 0;

                    // Set-up parametric equalizer
                    while (band < meta::impulse_responses_metadata::EQ_BANDS)
                    {
                        if (band == 0)
                        {
                            fp.fFreq        = band_freqs[band];
                            fp.fFreq2       = fp.fFreq;
                            fp.nType        = dspu::FLT_MT_LRX_LOSHELF;
                        }
                        else if (band == (meta::impulse_responses_metadata::EQ_BANDS - 1))
                        {
                            fp.fFreq        = band_freqs[band-1];
                            fp.fFreq2       = fp.fFreq;
                            fp.nType        = dspu::FLT_MT_LRX_HISHELF;
                        }
                        else
                        {
                            fp.fFreq        = band_freqs[band-1];
                            fp.fFreq2       = band_freqs[band];
                            fp.nType        = dspu::FLT_MT_LRX_LADDERPASS;
                        }

                        fp.fGain        = c->pFreqGain[band]->value();
                        fp.nSlope       = 2;
                        fp.fQuality     = 0.0f;

                        // Update filter parameters
                        eq->set_params(band++, &fp);
                    }

                    // Setup hi-pass filter
                    size_t hp_slope = c->pLowCut->value() * 2;
                    fp.nType        = (hp_slope > 0) ? dspu::FLT_BT_BWC_HIPASS : dspu::FLT_NONE;
                    fp.fFreq        = c->pLowFreq->value();
                    fp.fFreq2       = fp.fFreq;
                    fp.fGain        = 1.0f;
                    fp.nSlope       = hp_slope;
                    fp.fQuality     = 0.0f;
                    eq->set_params(band++, &fp);

                    // Setup low-pass filter
                    size_t lp_slope = c->pHighCut->value() * 2;
                    fp.nType        = (lp_slope > 0) ? dspu::FLT_BT_BWC_LOPASS : dspu::FLT_NONE;
                    fp.fFreq        = c->pHighFreq->value();
                    fp.fFreq2       = fp.fFreq;
                    fp.fGain        = 1.0f;
                    fp.nSlope       = lp_slope;
                    fp.fQuality     = 0.0f;
                    eq->set_params(band++, &fp);
                }
            }
        }

        void impulse_responses::update_sample_rate(long sr)
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c = &vChannels[i];

                c->sBypass.init(sr);
                c->sDelay.init(dspu::millis_to_samples(sr, meta::impulse_responses_metadata::PREDELAY_MAX));
                c->sEqualizer.set_sample_rate(sr);
            }
        }

        void impulse_responses::process(size_t samples)
        {
            //---------------------------------------------------------------------
            // Stage 1: process reconfiguration requests and file events
            if (sConfigurator.idle())
            {
                // Check that reconfigure is pending
                if (nReconfigReq != nReconfigResp)
                {
                    // Remember render state
                    for (size_t i=0; i<nChannels; ++i)
                        sConfigurator.set_render(i, vFiles[i].bRender);
                    for (size_t i=0; i<nChannels; ++i)
                    {
                        sConfigurator.set_source(i, vChannels[i].nSourceReq);
                        sConfigurator.set_rank(i, vChannels[i].nRankReq);
                    }

                    // Try to submit task
                    if (pExecutor->submit(&sConfigurator))
                    {
                        lsp_trace("successfully submitted configuration task");

                        // Clear render state and reconfiguration request
                        nReconfigResp   = nReconfigReq;
                        for (size_t i=0; i<nChannels; ++i)
                            vFiles[i].bRender   = false;
                    }
                }
                else
                {
                    // Process file requests
                    for (size_t i=0; i<nChannels; ++i)
                    {
                        // Get descriptor
                        af_descriptor_t *af     = &vFiles[i];
                        if (af->pFile == NULL)
                            continue;

                        // Get path and check task state
                        plug::path_t *path = af->pFile->buffer<plug::path_t>();
                        if ((path != NULL) && (path->accepted()) && (af->pLoader->completed()))
                        {
                            // Swap file data
                            lsp::swap(af->pCurr, af->pSwap);

                            // Update file status and set re-rendering flag
                            af->nStatus         = af->pLoader->code();
                            af->bRender         = true;
                            nReconfigReq        ++;

                            // Now we surely can commit changes and reset task state
                            path->commit();
                            af->pLoader->reset();
                        }
                    }
                }
            }
            else if (sConfigurator.completed())
            {
                // Update samples
                for (size_t i=0; i<nChannels; ++i)
                {
                    af_descriptor_t *f = &vFiles[i];
                    if (f->bSwap)
                    {
                        lsp::swap(f->pCurrSample, f->pSwapSample);
                        f->bSwap            = false;
                    }

                    f->bSync = true;
                }

                // Update convolvers
                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c        = &vChannels[i];
                    dspu::Convolver *cv = c->pCurr;
                    c->pCurr            = c->pSwap;
                    c->pSwap            = cv;

                    // Bind sample player
                    for (size_t j=0; j<nChannels; ++j)
                    {
                        af_descriptor_t *f = &vFiles[j];
                        c->sPlayer.bind(j, f->pCurrSample, false);
                    }
                }

                // Reset configurator
                sConfigurator.reset();
            }

            //---------------------------------------------------------------------
            // Stage 2: perform convolution
            // Get pointers to data channels
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c    = &vChannels[i];

                c->vIn          = c->pIn->buffer<float>();
                c->vOut         = c->pOut->buffer<float>();
            }

            // Process samples
            while (samples > 0)
            {
                size_t to_do        = TMP_BUF_SIZE;
                if (to_do > samples)
                    to_do               = samples;

                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c    = &vChannels[i];

                    // Do processing
                    if (c->pCurr != NULL)
                        c->pCurr->process(c->vBuffer, c->vIn, to_do);
                    else
                        dsp::fill_zero(c->vBuffer, to_do);
                    c->sEqualizer.process(c->vBuffer, c->vBuffer, to_do); // Process wet signal with equalizer
                    c->sDelay.process(c->vBuffer, c->vBuffer, to_do);
                    dsp::mix2(c->vBuffer, c->vIn, c->fWetGain, c->fDryGain, to_do);
                    c->sPlayer.process(c->vBuffer, c->vBuffer, to_do);
                    c->sBypass.process(c->vOut, c->vIn, c->vBuffer, to_do);

                    // Update pointers
                    c->vIn             += to_do;
                    c->vOut            += to_do;
                }

                samples            -= to_do;
            }

            //---------------------------------------------------------------------
            // Stage 3: output parameters

            for (size_t i=0; i<nChannels; ++i)
            {
                af_descriptor_t *af     = &vFiles[i];
                channel_t *c            = &vChannels[i];

                // Output information about the file
                c->pActivity->set_value(c->pCurr != NULL);
                size_t length           = (af->pCurr != NULL) ? af->pCurr->samples() : 0;
    //            lsp_trace("channels=%d, samples=%d, timing=%f",
    //                    int((af->pCurr != NULL) ? af->pCurr->channels() : -1),
    //                    int(length),
    //                    samples_to_millis(fSampleRate, length)
    //                );
                af->pLength->set_value(dspu::samples_to_millis(fSampleRate, length));
                af->pStatus->set_value(af->nStatus);

                // Store file dump to mesh
                plug::mesh_t *mesh  = af->pThumbs->buffer<plug::mesh_t>();
    //            lsp_trace("i=%d, mesh=%p, is_empty=%d, bSync=%d", int(i), mesh, int(mesh->isEmpty()), int(af->bSync));
                if ((mesh == NULL) || (!mesh->isEmpty()) || (!af->bSync))
                    continue;

                size_t channels     = (af->pCurrSample != NULL) ? af->pCurrSample->channels() : 0;
    //            lsp_trace("curr_sample=%p, channels=%d", af->pCurrSample, int(channels));
                if (channels > 0)
                {
                    // Copy thumbnails
                    for (size_t j=0; j<channels; ++j)
                        dsp::copy(mesh->pvData[j], af->vThumbs[j], meta::impulse_responses_metadata::MESH_SIZE);
                    mesh->data(channels, meta::impulse_responses_metadata::MESH_SIZE);
                }
                else
                    mesh->data(0, 0);
                af->bSync           = false;
            }
        }

        status_t impulse_responses::load(af_descriptor_t *descr)
        {
            lsp_trace("descr = %p", descr);

            // Remove swap data
            if (descr->pSwap != NULL)
            {
                descr->pSwap->destroy();
                delete descr->pSwap;
                descr->pSwap    = NULL;
            }

            // Check state
            if ((descr == NULL) || (descr->pFile == NULL))
                return STATUS_UNKNOWN_ERR;

            // Get path
            plug::path_t *path = descr->pFile->buffer<plug::path_t>();
            if (path == NULL)
                return STATUS_UNKNOWN_ERR;

            // Get file name
            const char *fname = path->get_path();
            if (strlen(fname) <= 0)
                return STATUS_UNSPECIFIED;

            // Load audio file
            dspu::Sample *af    = new dspu::Sample();
            if (af == NULL)
                return STATUS_NO_MEM;

            // Try to load file
            float convLengthMaxSeconds = meta::impulse_responses_metadata::CONV_LENGTH_MAX * 0.001f;
            status_t status = af->load(fname,  convLengthMaxSeconds);
            if (status != STATUS_OK)
            {
                af->destroy();
                delete af;
                lsp_trace("load failed: status=%d (%s)", status, get_status(status));
                return status;
            }

            // Try to resample
            status  = af->resample(fSampleRate);
            if (status != STATUS_OK)
            {
                af->destroy();
                delete af;
                lsp_trace("resample failed: status=%d (%s)", status, get_status(status));
                return status;
            }

            // Determine the normalizing factor
            size_t channels         = af->channels();
            float max = 0.0f;

            for (size_t i=0; i<channels; ++i)
            {
                // Determine the maximum amplitude
                float a_max = dsp::abs_max(af->channel(i), af->samples());
                if (max < a_max)
                    max     = a_max;
            }
            descr->fNorm    = (max != 0.0f) ? 1.0f / max : 1.0f;

            // File was successfully loaded, report to caller
            descr->pSwap    = af;

            return STATUS_OK;
        }

        status_t impulse_responses::reconfigure(const reconfig_t *cfg)
        {
            // Re-render files (if needed)
            for (size_t i=0; i<nChannels; ++i)
            {
                // Do we need to re-render file?
                if (!cfg[i].bRender)
                    continue;

                // Get audio file
                af_descriptor_t *f  = &vFiles[i];
                dspu::Sample *af    = f->pCurr;

                // Destroy swap sample
                if (f->pSwapSample != NULL)
                {
                    f->pSwapSample->destroy();
                    delete f->pSwapSample;
                    f->pSwapSample  = NULL;
                }

                dspu::Sample *s     = new dspu::Sample();
                if (s == NULL)
                    return STATUS_NO_MEM;
                f->pSwapSample      = s;
                f->bSwap            = true;

                if (af == NULL)
                    continue;

                ssize_t flen        = af->samples();
                size_t channels     = lsp_min(af->channels(), meta::impulse_responses_metadata::TRACKS_MAX);

                // Buffer is present, file is present, check boundaries
                size_t head_cut     = dspu::millis_to_samples(fSampleRate, f->fHeadCut);
                size_t tail_cut     = dspu::millis_to_samples(fSampleRate, f->fTailCut);
                ssize_t fsamples    = flen - head_cut - tail_cut;
                if (fsamples <= 0)
                {
                    for (size_t j=0; j<channels; ++j)
                        dsp::fill_zero(f->vThumbs[j], meta::impulse_responses_metadata::MESH_SIZE);
                    s->set_length(0);
                    continue;
                }

                // Now ensure that we have enough space for sample
                if (!s->init(channels, flen, fsamples))
                    return STATUS_NO_MEM;

                // Copy data to temporary buffer and apply fading
                for (size_t i=0; i<channels; ++i)
                {
                    float *dst = s->channel(i);
                    const float *src = af->channel(i);

                    // Copy sample data and apply fading
                    dsp::copy(dst, &src[head_cut], fsamples);
                    dspu::fade_in(dst, dst, dspu::millis_to_samples(fSampleRate, f->fFadeIn), fsamples);
                    dspu::fade_out(dst, dst, dspu::millis_to_samples(fSampleRate, f->fFadeOut), fsamples);

                    // Now render thumbnail
                    src                 = dst;
                    dst                 = f->vThumbs[i];
                    for (size_t k=0; k<meta::impulse_responses_metadata::MESH_SIZE; ++k)
                    {
                        size_t first    = (k * fsamples) / meta::impulse_responses_metadata::MESH_SIZE;
                        size_t last     = ((k + 1) * fsamples) / meta::impulse_responses_metadata::MESH_SIZE;
                        if (first < last)
                            dst[k]          = dsp::abs_max(&src[first], last - first);
                        else
                            dst[k]          = fabs(src[first]);
                    }

                    // Normalize graph if possible
                    if (f->fNorm != 1.0f)
                        dsp::mul_k2(dst, f->fNorm, meta::impulse_responses_metadata::MESH_SIZE);
                }
            }

            // Randomize phase of the convolver
            uint32_t phase  = seed_addr(this);
            phase           = ((phase << 16) | (phase >> 16)) & 0x7fffffff;
            uint32_t step   = 0x80000000 / (nChannels + 1);

            // OK, files have been rendered, now need to commutate
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c    = &vChannels[i];

                // Check that we need to free previous convolver
                if (c->pSwap != NULL)
                {
                    c->pSwap->destroy();
                    delete c->pSwap;
                    c->pSwap = NULL;
                }

                // Check that routing has changed
                size_t ch   = cfg[i].nSource;
                if ((ch--) == 0)
                {
                    c->nSource  = 0;
                    c->nRank    = cfg[i].nRank;
                    continue;
                }

                // Apply new routing
                size_t track    = ch % meta::impulse_responses_metadata::TRACKS_MAX;
                size_t file     = ch / meta::impulse_responses_metadata::TRACKS_MAX;
                if (file >= nChannels)
                    continue;

                // Analyze sample
                dspu::Sample *s = (vFiles[file].bSwap) ? vFiles[file].pSwapSample : vFiles[file].pCurrSample;
                if ((s == NULL) || (!s->valid()) || (s->channels() <= track))
                    continue;

                // Now we can create convolver
                dspu::Convolver *cv   = new dspu::Convolver();
                if (cv == NULL)
                    continue;
                if (!cv->init(s->channel(track), s->length(), cfg[i].nRank, float((phase + i*step) & 0x7fffffff)/float(0x80000000)))
                {
                    cv->destroy();
                    delete cv;
                    return STATUS_NO_MEM;
                }
                c->pSwap        = cv;
            }

            return STATUS_OK;
        }

        void impulse_responses::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            v->write_object("sConfigurator", &sConfigurator);
            v->write("nChannels", nChannels);
            v->begin_array("vChannels", vChannels, nChannels);
            {
                for (size_t i=0; i<nChannels; ++i)
                {
                    const channel_t *c = &vChannels[i];

                    v->write_object("sBypass", &c->sBypass);
                    v->write_object("sDelay", &c->sDelay);
                    v->write_object("sPlayer", &c->sPlayer);
                    v->write_object("sEqualizer", &c->sEqualizer);
                    v->write_object("pCurr", c->pCurr);
                    v->write_object("pSwap", c->pSwap);

                    v->write("vIn", c->vIn);
                    v->write("vOut", c->vOut);
                    v->write("vBuffer", c->vBuffer);
                    v->write("fDryGain", c->fDryGain);
                    v->write("fWetGain", c->fWetGain);
                    v->write("nSource", c->nSource);
                    v->write("nSourceReq", c->nSourceReq);
                    v->write("nRank", c->nRank);
                    v->write("nRankReq", c->nRankReq);

                    v->write("pIn", c->pIn);
                    v->write("pOut", c->pOut);

                    v->write("pSource", c->pSource);
                    v->write("pMakeup", c->pMakeup);
                    v->write("pActivity", c->pActivity);
                    v->write("pPredelay", c->pPredelay);

                    v->write("pWetEq", c->pWetEq);
                    v->write("pLowCut", c->pLowCut);
                    v->write("pLowFreq", c->pLowFreq);
                    v->write("pHighCut", c->pHighCut);
                    v->write("pHighFreq", c->pHighFreq);

                    v->writev("pFreqGain", c->pFreqGain, meta::impulse_responses_metadata::EQ_BANDS);
                }
            }
            v->end_array();
            v->begin_array("vFiles", vFiles, nChannels);
            {
                for (size_t i=0; i<nChannels; ++i)
                {
                    const af_descriptor_t *af = &vFiles[i];

                    v->write_object("pCurr", af->pCurr);
                    v->write_object("pSwap", af->pSwap);

                    v->write_object("sListen", &af->sListen);
                    v->write_object("pSwapSample", af->pSwapSample);
                    v->write_object("pCurrSample", af->pCurrSample);

                    v->writev("vThumbs", af->vThumbs, meta::impulse_responses_metadata::TRACKS_MAX);

                    v->write("fNorm", af->fNorm);
                    v->write("bRender", af->bRender);
                    v->write("nStatus", af->nStatus);
                    v->write("bSync", af->bSync);
                    v->write("bSwap", af->bSwap);

                    v->write("fHeadCut", af->fHeadCut);
                    v->write("fTailCut", af->fTailCut);
                    v->write("fFadeIn", af->fFadeIn);
                    v->write("fFadeOut", af->fFadeOut);

                    v->write_object("pLoader", af->pLoader);

                    v->write("pFile", af->pFile);
                    v->write("pHeadCut", af->pHeadCut);
                    v->write("pTailCut", af->pTailCut);
                    v->write("pFadeIn", af->pFadeIn);
                    v->write("pFadeOut", af->pFadeOut);
                    v->write("pListen", af->pListen);
                    v->write("pStatus", af->pStatus);
                    v->write("pLength", af->pLength);
                    v->write("pThumbs", af->pThumbs);
                }
            }
            v->end_array();

            v->write("pExecutor", pExecutor);
            v->write("nReconfigReq", nReconfigReq);
            v->write("nReconfigResp", nReconfigResp);
            v->write("fGain", fGain);

            v->write("pBypass", pBypass);
            v->write("pRank", pRank);
            v->write("pDry", pDry);
            v->write("pWet", pWet);
            v->write("pOutGain", pOutGain);

            v->write("pData", pData);
        }
    } // namespace plugins
} // namespace lsp


