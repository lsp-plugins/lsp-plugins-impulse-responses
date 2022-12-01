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

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/atomic.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/misc/fade.h>
#include <lsp-plug.in/plug-fw/meta/func.h>

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
        }

        impulse_responses::IRConfigurator::~IRConfigurator()
        {
            pCore       = NULL;
        }

        status_t impulse_responses::IRConfigurator::run()
        {
            return pCore->reconfigure();
        }

        void impulse_responses::IRConfigurator::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
            v->end_array();
        }

        //-------------------------------------------------------------------------
        impulse_responses::GCTask::GCTask(impulse_responses *base)
        {
            pCore       = base;
        }

        impulse_responses::GCTask::~GCTask()
        {
            pCore       = NULL;
        }

        status_t impulse_responses::GCTask::run()
        {
            pCore->perform_gc();
            return STATUS_OK;
        }

        void impulse_responses::GCTask::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
        }

        //-------------------------------------------------------------------------
        impulse_responses::impulse_responses(const meta::plugin_t *metadata):
            plug::Module(metadata),
            sConfigurator(this),
            sGCTask(this)
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
            nRank           = 0;
            pGCList         = NULL;

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

        void impulse_responses::destroy_samples(dspu::Sample *gc_list)
        {
            // Iterate over the list and destroy each sample in the list
            while (gc_list != NULL)
            {
                dspu::Sample *next = gc_list->gc_next();
                gc_list->destroy();
                delete gc_list;
                lsp_trace("Destroyed sample %p", gc_list);
                gc_list = next;
            }
        }

        void impulse_responses::destroy_file(af_descriptor_t *af)
        {
            // Destroy samples
            if (af->pOriginal != NULL)
            {
                af->pOriginal->destroy();
                delete af->pOriginal;
                lsp_trace("Destroyed sample %p", af->pOriginal);
                af->pOriginal = NULL;
            }
            if (af->pProcessed != NULL)
            {
                af->pProcessed->destroy();
                delete af->pProcessed;
                lsp_trace("Destroyed sample %p", af->pOriginal);
                af->pProcessed = NULL;
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

        void impulse_responses::perform_gc()
        {
            dspu::Sample *gc_list = lsp::atomic_swap(&pGCList, NULL);
            destroy_samples(gc_list);
        }

        void impulse_responses::init(plug::IWrapper *wrapper, plug::IPort **ports)
        {
            // Pass wrapper
            plug::Module::init(wrapper, ports);

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

                f->pOriginal    = NULL;
                f->pProcessed   = NULL;

                for (size_t j=0; j<meta::impulse_responses_metadata::TRACKS_MAX; ++j, ptr += thumbs_size)
                    f->vThumbs[j]   = reinterpret_cast<float *>(ptr);

                f->fNorm        = 1.0f;
                f->nStatus      = STATUS_UNSPECIFIED;
                f->bSync        = true;
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
                TRACE_PORT(ports[port_id]);
                vChannels[i].pIn    = ports[port_id++];
            }
            for (size_t i=0; i<nChannels; ++i)
            {
                TRACE_PORT(ports[port_id]);
                vChannels[i].pOut   = ports[port_id++];
            }

            // Bind common ports
            lsp_trace("Binding common ports");
            TRACE_PORT(ports[port_id]);
            pBypass     = ports[port_id++];
            TRACE_PORT(ports[port_id]);
            pRank       = ports[port_id++];
            TRACE_PORT(ports[port_id]);
            pDry        = ports[port_id++];
            TRACE_PORT(ports[port_id]);
            pWet        = ports[port_id++];
            TRACE_PORT(ports[port_id]);
            pOutGain    = ports[port_id++];

            // Skip file selector
            if (nChannels > 1)
            {
                TRACE_PORT(ports[port_id]);
                port_id++;
            }

            // Bind impulse file ports
            for (size_t i=0; i<nChannels; ++i)
            {
                lsp_trace("Binding impulse file #%d ports", int(i));
                af_descriptor_t *f  = &vFiles[i];

                f->sListen.init();

                TRACE_PORT(ports[port_id]);
                f->pFile        = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pHeadCut     = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pTailCut     = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pFadeIn      = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pFadeOut     = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pListen      = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pStatus      = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pLength      = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                f->pThumbs      = ports[port_id++];
            }

            // Bind convolution ports
            for (size_t i=0; i<nChannels; ++i)
            {
                lsp_trace("Binding convolution #%d ports", int(i));
                channel_t *c    = &vChannels[i];

                TRACE_PORT(ports[port_id]);
                c->pSource      = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                c->pMakeup      = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                c->pActivity    = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                c->pPredelay    = ports[port_id++];
            }

            // Bind wet processing ports
            lsp_trace("Binding wet processing ports");
            size_t port         = port_id;
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];

                TRACE_PORT(ports[port_id]);
                c->pWetEq           = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                port_id++;          // Skip equalizer visibility port
                TRACE_PORT(ports[port_id]);
                c->pLowCut          = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                c->pLowFreq         = ports[port_id++];

                for (size_t j=0; j<meta::impulse_responses_metadata::EQ_BANDS; ++j)
                {
                    TRACE_PORT(ports[port_id]);
                    c->pFreqGain[j]     = ports[port_id++];
                }

                TRACE_PORT(ports[port_id]);
                c->pHighCut         = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                c->pHighFreq        = ports[port_id++];

                port_id         = port;
            }

        }

        void impulse_responses::destroy()
        {
            // Perform garbage collection
            perform_gc();

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
            size_t rank         = get_fft_rank(pRank->value());
            fGain               = pOutGain->value();
            if (rank != nRank)
            {
                ++nReconfigReq;
                nRank               = rank;
            }

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
                    nReconfigReq        ++;
                }

                // Listen button pressed?
                if (f->pListen != NULL)
                    f->sListen.submit(f->pListen->value());

                size_t source       = c->pSource->value();
                if (source != c->nSource)
                {
                    ++nReconfigReq;
                    c->nSource          = source;
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
                ++nReconfigReq;

                c->sBypass.init(sr);
                c->sDelay.init(dspu::millis_to_samples(sr, meta::impulse_responses_metadata::PREDELAY_MAX));
                c->sEqualizer.set_sample_rate(sr);
            }
        }

        bool impulse_responses::has_active_loading_tasks()
        {
            for (size_t i=0; i<nChannels; ++i)
                if (!vFiles[i].pLoader->idle())
                    return true;
            return false;
        }

        void impulse_responses::process_loading_tasks()
        {
            // Do nothing with loading while configurator is active
            if (!sConfigurator.idle())
                return;

            // Process each audio file
            for (size_t i=0; i<nChannels; ++i)
            {
                af_descriptor_t *af     = &vFiles[i];
                if (af->pFile == NULL)
                    continue;

                // Get path and check task state
                if (af->pLoader->idle())
                {
                    // Get path
                    plug::path_t *path      = af->pFile->buffer<plug::path_t>();
                    if ((path != NULL) && (path->pending()))
                    {
                        // Try to submit task
                        if (pExecutor->submit(af->pLoader))
                        {
                            lsp_trace("Successfully submitted load task for file %d", int(i));
                            af->nStatus         = STATUS_LOADING;
                            path->accept();
                        }
                    }
                }
                else if (af->pLoader->completed())
                {
                    plug::path_t *path = af->pFile->buffer<plug::path_t>();
                    if ((path != NULL) && (path->accepted()))
                    {
                        // Update file status and set re-rendering flag
                        af->nStatus         = af->pLoader->code();
                        ++nReconfigReq;

                        // Now we surely can commit changes and reset task state
                        path->commit();
                        af->pLoader->reset();
                    }
                }
            } // for
        }

        void impulse_responses::process_configuration_tasks()
        {
            // Do nothing if at least one loader is active
            if (has_active_loading_tasks())
                return;

            // Check the status and look for a job
            if ((nReconfigReq != nReconfigResp) && (sConfigurator.idle()))
            {
                // Try to submit task
                if (pExecutor->submit(&sConfigurator))
                {
                    // Clear render state and reconfiguration request
                    nReconfigResp   = nReconfigReq;
                    lsp_trace("Successfully submitted reconfiguration task");
                }
            }
            else if (sConfigurator.completed())
            {
                // Update state for channels
                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c        = &vChannels[i];

                    // Commit new convolver
                    lsp::swap(c->pCurr, c->pSwap);
                }

                // Bind processed samples to the sampler
                for (size_t i=0; i<nChannels; ++i)
                {
                    af_descriptor_t *f  = &vFiles[i];
                    for (size_t j=0; j<nChannels; ++j)
                        vChannels[j].sPlayer.bind(i, f->pProcessed);
                    f->pProcessed   = NULL;
                    f->bSync        = true;
                }

                // Reset configurator task
                sConfigurator.reset();
            }
        }

        void impulse_responses::process_gc_events()
        {
            if (sGCTask.completed())
                sGCTask.reset();

            if (sGCTask.idle())
            {
                // Obtain the list of samples for destroy
                if (pGCList == NULL)
                {
                    for (size_t i=0; i<nChannels; ++i)
                        if ((pGCList = vChannels[i].sPlayer.gc()) != NULL)
                            break;
                }
                if (pGCList != NULL)
                    pExecutor->submit(&sGCTask);
            }
        }

        void impulse_responses::process_listen_events()
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                af_descriptor_t *f  = &vFiles[i];

                if (!f->sListen.pending())
                    continue;

                lsp_trace("Submitted listen toggle");
                dspu::Sample *s = vChannels[0].sPlayer.get(i);
                size_t n_c = (s != NULL) ? s->channels() : 0;
                if (n_c > 0)
                {
                    for (size_t j=0; j<nChannels; ++j)
                        vChannels[j].sPlayer.play(i, j%n_c, 1.0f, 0);
                }
                f->sListen.commit();
            }
        }

        void impulse_responses::perform_convolution(size_t samples)
        {
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
        }

        void impulse_responses::output_parameters()
        {
            // Do not sync state of mesh if there are active tasks
            bool tasks_active  = (!sConfigurator.idle()) || (has_active_loading_tasks());

            // Update channel activity
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];
                c->pActivity->set_value(c->pCurr != NULL);
            }

            // Update indicators and meshes (if possible)
            for (size_t i=0; i<nChannels; ++i)
            {
                af_descriptor_t *af     = &vFiles[i];

                // Output information about the file
                dspu::Sample *active    = vChannels[0].sPlayer.get(i);
                size_t channels         = (active != NULL) ? active->channels() : 0;
                channels                = lsp_min(channels, nChannels);

                // Output activity indicator
                size_t length           = (af->pOriginal != NULL) ? af->pOriginal->samples() : 0;
                af->pLength->set_value(dspu::samples_to_millis(fSampleRate, length));
                af->pStatus->set_value(af->nStatus);

                // Store file dump to mesh
                plug::mesh_t *mesh      = af->pThumbs->buffer<plug::mesh_t>();
                if ((mesh == NULL) || (!mesh->isEmpty()) || (!af->bSync) || (tasks_active))
                    continue;

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

        void impulse_responses::process(size_t samples)
        {
            process_loading_tasks();
            process_configuration_tasks();
            process_gc_events();
            process_listen_events();
            perform_convolution(samples);
            output_parameters();
        }

        status_t impulse_responses::load(af_descriptor_t *descr)
        {
            lsp_trace("descr = %p", descr);

            // Remove swap data
            if (descr->pOriginal != NULL)
            {
                descr->pOriginal->destroy();
                delete descr->pOriginal;
                lsp_trace("Destroyed sample %p", descr->pOriginal);
                descr->pOriginal    = NULL;
            }

            // Check state
            if ((descr == NULL) || (descr->pFile == NULL))
                return STATUS_UNKNOWN_ERR;

            // Get path
            plug::path_t *path = descr->pFile->buffer<plug::path_t>();
            if (path == NULL)
                return STATUS_UNKNOWN_ERR;

            // Get file name
            const char *fname = path->path();
            if (strlen(fname) <= 0)
                return STATUS_UNSPECIFIED;

            // Load audio file
            dspu::Sample *af    = new dspu::Sample();
            if (af == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (af != NULL)
                {
                    af->destroy();
                    delete af;
                }
            };

            // Try to load file
            float convLengthMaxSeconds = meta::impulse_responses_metadata::CONV_LENGTH_MAX * 0.001f;
            status_t status = af->load(fname,  convLengthMaxSeconds);
            if (status != STATUS_OK)
            {
                lsp_trace("load failed: status=%d (%s)", status, get_status(status));
                return status;
            }

            // Try to resample
            status  = af->resample(fSampleRate);
            if (status != STATUS_OK)
            {
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

            // File was successfully loaded, pass result to the caller
            lsp::swap(descr->pOriginal, af);

            return STATUS_OK;
        }

        status_t impulse_responses::reconfigure()
        {
            // Re-render all files
            for (size_t i=0; i<nChannels; ++i)
            {
                // Get audio file
                af_descriptor_t *f      = &vFiles[i];

                // Destroy swap sample
                if (f->pProcessed != NULL)
                {
                    f->pProcessed->destroy();
                    delete f->pProcessed;
                    lsp_trace("Destroyed sample %p", f->pProcessed);
                    f->pProcessed  = NULL;
                }

                // Get sample to process
                const dspu::Sample *af  = f->pOriginal;
                if (af == NULL)
                    continue;

                // Allocate new sample
                dspu::Sample *s     = new dspu::Sample();
                if (s == NULL)
                    return STATUS_NO_MEM;
                lsp_finally {
                    if (s != NULL)
                    {
                        s->destroy();
                        delete s;
                    }
                };

                // Obtain new sample parameters
                ssize_t flen        = af->samples();
                size_t channels     = lsp_min(af->channels(), meta::impulse_responses_metadata::TRACKS_MAX);
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
                    dspu::fade_in(dst, &src[head_cut], dspu::millis_to_samples(fSampleRate, f->fFadeIn), fsamples);
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

                // Commit sample to the processed list
                lsp::swap(f->pProcessed, s);
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
                size_t ch   = c->nSource;
                if (ch == 0)
                    continue;
                --ch;

                // Apply new routing
                size_t track    = ch % meta::impulse_responses_metadata::TRACKS_MAX;
                size_t file     = ch / meta::impulse_responses_metadata::TRACKS_MAX;
                if (file >= nChannels)
                    continue;

                // Analyze sample
                dspu::Sample *s = vFiles[file].pProcessed;
                if ((s == NULL) || (!s->valid()) || (s->channels() <= track))
                    continue;

                // Now we can create convolver
                dspu::Convolver *cv   = new dspu::Convolver();
                if (cv == NULL)
                    continue;
                lsp_finally {
                    if (cv != NULL)
                    {
                        cv->destroy();
                        delete cv;
                    }
                };

                // Initialize convolver
                if (!cv->init(s->channel(track), s->length(), nRank, float((phase + i*step) & 0x7fffffff)/float(0x80000000)))
                    return STATUS_NO_MEM;

                // Commit convolver
                lsp::swap(c->pSwap, cv);
            }

            return STATUS_OK;
        }

        void impulse_responses::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            v->write_object("sConfigurator", &sConfigurator);
            v->write_object("sGCTask", &sGCTask);
            v->write("nChannels", nChannels);
            v->begin_array("vChannels", vChannels, nChannels);
            {
                for (size_t i=0; i<nChannels; ++i)
                {
                    const channel_t *c = &vChannels[i];
                    v->begin_object(c, sizeof(channel_t));
                    {
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
                    v->end_object();
                }
            }
            v->end_array();
            v->begin_array("vFiles", vFiles, nChannels);
            {
                for (size_t i=0; i<nChannels; ++i)
                {
                    const af_descriptor_t *af = &vFiles[i];
                    v->begin_object(af, sizeof(af_descriptor_t));
                    {
                        v->write_object("sListen", &af->sListen);
                        v->write_object("pOriginal", af->pOriginal);
                        v->write_object("pProcessed", af->pProcessed);

                        v->writev("vThumbs", af->vThumbs, meta::impulse_responses_metadata::TRACKS_MAX);

                        v->write("fNorm", af->fNorm);
                        v->write("nStatus", af->nStatus);
                        v->write("bSync", af->bSync);

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
                    v->end_object();
                }
            }
            v->end_array();

            v->write("pExecutor", pExecutor);
            v->write("nReconfigReq", nReconfigReq);
            v->write("nReconfigResp", nReconfigResp);
            v->write("fGain", fGain);
            v->write("nRank", nRank);
            v->write("pGCList", pGCList);

            v->write("pBypass", pBypass);
            v->write("pRank", pRank);
            v->write("pDry", pDry);
            v->write("pWet", pWet);
            v->write("pOutGain", pOutGain);

            v->write("pData", pData);
        }
    } // namespace plugins
} // namespace lsp


