/*
 * libfreelan - A C++ library to establish peer-to-peer virtual private
 * networks.
 * Copyright (C) 2010-2011 Julien KAUFFMANN <julien.kauffmann@freelan.org>
 *
 * This file is part of libfreelan.
 *
 * libfreelan is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * libfreelan is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 *
 * If you intend to use libfreelan in a commercial software, please
 * contact me : we may arrange this for a small fee or no fee at all,
 * depending on the nature of your project.
 */

/**
 * \file client.hpp
 * \author Julien KAUFFMANN <julien.kauffmann@freelan.org>
 * \brief A client implementation.
 */

#pragma once

#include <boost/asio.hpp>
#include <boost/function.hpp>
#include <boost/enable_shared_from_this.hpp>

#include <cryptoplus/x509/certificate.hpp>
#include <cryptoplus/x509/certificate_request.hpp>

#include <fscp/memory_pool.hpp>
#include <fscp/logger.hpp>

#include "os.hpp"
#include "configuration.hpp"
#include "curl.hpp"

namespace freelan
{
	class web_client : public boost::enable_shared_from_this<web_client>
	{
		public:
			typedef boost::function<void (const boost::system::error_code&, cryptoplus::x509::certificate)> request_certificate_callback;

			/**
			 * \brief Create a new web client.
			 * \param io_service The io_service to bind to.
			 * \param _logger The logger to use.
			 * \param configuration The configuration to use.
			 */
			static boost::shared_ptr<web_client> create(boost::asio::io_service& io_service, fscp::logger& _logger, const freelan::client_configuration& configuration)
			{
				return boost::shared_ptr<web_client>(new web_client(io_service, _logger, configuration));
			}

			/**
			 * \brief Request the server for a certificate.
			 * \param certifcate_request The certificate request to send.
			 * \param handler The handler that will get called when the response is received.
			 */
			void request_certificate(cryptoplus::x509::certificate_request certificate_request, request_certificate_callback handler);

		private:
			typedef fscp::memory_pool<8192, 2> memory_pool;

			web_client(boost::asio::io_service& io_service, fscp::logger& _logger, const freelan::client_configuration& configuration);
			boost::shared_ptr<curl> make_request(const std::string& path) const;

			memory_pool m_memory_pool;
			boost::shared_ptr<curl_multi_asio> m_curl_multi_asio;
			fscp::logger& m_logger;
			freelan::client_configuration m_configuration;
			std::string m_url_prefix;
	};
}
