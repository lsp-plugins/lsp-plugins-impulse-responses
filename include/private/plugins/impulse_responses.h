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

#include <private/meta/impulse_responses.h>

namespace lsp
{
    namespace plugins
    {
        class impulse_responses: public plug::Module
        {
        };
    } // namespace plugins
} // namespace lsp

#endif /* PRIVATE_PLUGINS_IMPULSE_RESPONSES_H_ */
