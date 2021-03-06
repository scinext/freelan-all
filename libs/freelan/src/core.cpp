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
 * \file core.cpp
 * \author Julien KAUFFMANN <julien.kauffmann@freelan.org>
 * \brief The freelan core class.
 */

#include "core.hpp"

#include "client.hpp"
#include "routes_request_message.hpp"
#include "routes_message.hpp"

#include <fscp/server_error.hpp>

#include <asiotap/types/ip_network_address.hpp>

#ifdef WINDOWS
#include <executeplus/windows_system.hpp>
#else
#include <executeplus/posix_system.hpp>
#endif

#include <boost/make_shared.hpp>
#include <boost/foreach.hpp>
#include <boost/thread/future.hpp>
#include <boost/iterator/transform_iterator.hpp>

#include <cassert>

namespace freelan
{
	using boost::asio::buffer;
	using boost::asio::buffer_cast;
	using boost::asio::buffer_size;

	namespace
	{
		void null_simple_write_handler(const boost::system::error_code&)
		{
		}

		void null_switch_write_handler(const switch_::multi_write_result_type&)
		{
		}

		void null_router_write_handler(const boost::system::error_code&)
		{
		}

		asiotap::endpoint to_endpoint(const core::ep_type& host)
		{
			if (host.address().is_v4())
			{
				return asiotap::ipv4_endpoint(host.address().to_v4(), host.port());
			}
			else
			{
				return asiotap::ipv6_endpoint(host.address().to_v6(), host.port());
			}
		}

		template <typename SharedBufferType, typename Handler>
		class shared_buffer_handler
		{
			public:

				typedef void result_type;

				shared_buffer_handler(SharedBufferType _buffer, Handler _handler) :
					m_buffer(_buffer),
					m_handler(_handler)
				{}

				result_type operator()()
				{
					m_handler();
				}

				template <typename Arg1>
				result_type operator()(Arg1 arg1)
				{
					m_handler(arg1);
				}

				template <typename Arg1, typename Arg2>
				result_type operator()(Arg1 arg1, Arg2 arg2)
				{
					m_handler(arg1, arg2);
				}

			private:

				SharedBufferType m_buffer;
				Handler m_handler;
		};

		template <typename SharedBufferType, typename Handler>
		inline shared_buffer_handler<SharedBufferType, Handler> make_shared_buffer_handler(SharedBufferType _buffer, Handler _handler)
		{
			return shared_buffer_handler<SharedBufferType, Handler>(_buffer, _handler);
		}

		template <typename Handler, typename CausalHandler>
		class causal_handler
		{
			private:

				class automatic_caller : public boost::noncopyable
				{
					public:

						automatic_caller(CausalHandler& _handler) :
							m_auto_handler(_handler)
						{
						}

						~automatic_caller()
						{
							m_auto_handler();
						}

					private:

						CausalHandler& m_auto_handler;
				};

			public:

				typedef void result_type;

				causal_handler(Handler _handler, CausalHandler _causal_handler) :
					m_handler(_handler),
					m_causal_handler(_causal_handler)
				{}

				result_type operator()()
				{
					automatic_caller ac(m_causal_handler);

					m_handler();
				}

				template <typename Arg1>
				result_type operator()(Arg1 arg1)
				{
					automatic_caller ac(m_causal_handler);

					m_handler(arg1);
				}

				template <typename Arg1, typename Arg2>
				result_type operator()(Arg1 arg1, Arg2 arg2)
				{
					automatic_caller ac(m_causal_handler);

					m_handler(arg1, arg2);
				}

			private:

				Handler m_handler;
				CausalHandler m_causal_handler;
		};

		template <typename Handler, typename CausalHandler>
		inline causal_handler<Handler, CausalHandler> make_causal_handler(Handler _handler, CausalHandler _causal_handler)
		{
			return causal_handler<Handler, CausalHandler>(_handler, _causal_handler);
		}

		unsigned int get_auto_mtu_value()
		{
			const unsigned int default_mtu_value = 1500;
			const size_t static_payload_size = 20 + 8 + 4 + 22; // IP + UDP + FSCP HEADER + FSCP DATA HEADER

			return default_mtu_value - static_payload_size;
		}

		static const unsigned int TAP_ADAPTERS_GROUP = 0;
		static const unsigned int ENDPOINTS_GROUP = 1;

		asiotap::ip_route_set filter_routes(const asiotap::ip_route_set& routes, router_configuration::internal_route_scope_type scope, unsigned int limit, const asiotap::ip_network_address_list& network_addresses)
		{
			asiotap::ip_route_set result;
			auto ipv4_limit = limit;
			auto ipv6_limit = limit;

			auto check_limit = [limit, &ipv4_limit, &ipv6_limit](const asiotap::ip_route& route) {

				if (limit == 0)
				{
					return true;
				}

				const bool is_ipv4 = get_network_address(network_address(route)).is_v4();

				if (is_ipv4 ? ipv4_limit : ipv6_limit > 0)
				{
					(is_ipv4 ? ipv4_limit : ipv6_limit)--;

					return true;
				}

				return false;
			};

			switch (scope)
			{
				case router_configuration::internal_route_scope_type::none:
					break;
				case router_configuration::internal_route_scope_type::unicast_in_network:
				{
					for (auto&& ina: network_addresses)
					{
						for (auto&& route : routes)
						{
							if (is_unicast(route) && has_network(ina, network_address(route)))
							{
								if (check_limit(route))
								{
									result.insert(route);
								}
							}
						}
					}

					break;
				}
				case router_configuration::internal_route_scope_type::unicast:
				{
					for (auto&& route : routes)
					{
						if (is_unicast(route))
						{
							if (check_limit(route))
							{
								result.insert(route);
							}
						}
					}

					break;
				}
				case router_configuration::internal_route_scope_type::subnet:
				{
					for (auto&& ina : network_addresses)
					{
						for (auto&& route : routes)
						{
							if (has_network(ina, network_address(route)))
							{
								if (check_limit(route))
								{
									result.insert(route);
								}
							}
						}
					}

					break;
				}
				case router_configuration::internal_route_scope_type::any:
				{
					for (auto&& route : routes)
					{
						if (check_limit(route))
						{
							result.insert(route);
						}
					}

					break;
				}
			}

			return result;
		}

		asiotap::ip_route_set filter_routes(const asiotap::ip_route_set& routes, router_configuration::system_route_scope_type scope, unsigned int limit)
		{
			asiotap::ip_route_set result;
			auto ipv4_limit = limit;
			auto ipv6_limit = limit;

			auto check_limit = [limit, &ipv4_limit, &ipv6_limit](const asiotap::ip_route& route) {

				if (limit == 0)
				{
					return true;
				}

				const bool is_ipv4 = get_network_address(network_address(route)).is_v4();

				if (is_ipv4 ? ipv4_limit : ipv6_limit > 0)
				{
					(is_ipv4 ? ipv4_limit : ipv6_limit)--;

					return true;
				}

				return false;
			};

			switch (scope)
			{
				case router_configuration::system_route_scope_type::none:
					break;
				case router_configuration::system_route_scope_type::unicast:
				case router_configuration::system_route_scope_type::unicast_with_gateway:
				{
					for (auto&& route : routes)
					{
						if (is_unicast(route))
						{
							if ((scope == router_configuration::system_route_scope_type::unicast_with_gateway) || !has_gateway(route))
							{
								if (check_limit(route))
								{
									result.insert(route);
								}
							}
						}
					}

					break;
				}
				case router_configuration::system_route_scope_type::any:
				case router_configuration::system_route_scope_type::any_with_gateway:
				{
					for (auto&& route : routes)
					{
						if ((scope == router_configuration::system_route_scope_type::any_with_gateway) || !has_gateway(route))
						{
							if (check_limit(route))
							{
								result.insert(route);
							}
						}
					}

					break;
				}
			}

			return result;
		}
	}

	typedef boost::asio::ip::udp::resolver::query resolver_query;

	// Has to be put first, as static variables definition order matters.
	const int core::ex_data_index = cryptoplus::x509::store_context::register_index();

	const boost::posix_time::time_duration core::CONTACT_PERIOD = boost::posix_time::seconds(30);
	const boost::posix_time::time_duration core::DYNAMIC_CONTACT_PERIOD = boost::posix_time::seconds(45);
	const boost::posix_time::time_duration core::ROUTES_REQUEST_PERIOD = boost::posix_time::seconds(180);

	const std::string core::DEFAULT_SERVICE = "12000";

	core::core(boost::asio::io_service& io_service, const freelan::configuration& _configuration) :
		m_io_service(io_service),
		m_configuration(_configuration),
		m_logger_strand(m_io_service),
		m_logger(m_logger_strand.wrap(boost::bind(&core::do_handle_log, this, _1, _2, _3))),
		m_log_callback(),
		m_core_opened_callback(),
		m_core_closed_callback(),
		m_session_failed_callback(),
		m_session_error_callback(),
		m_session_established_callback(),
		m_session_lost_callback(),
		m_certificate_validation_callback(),
		m_tap_adapter_up_callback(),
		m_tap_adapter_down_callback(),
		m_server(),
		m_contact_timer(m_io_service, CONTACT_PERIOD),
		m_dynamic_contact_timer(m_io_service, DYNAMIC_CONTACT_PERIOD),
		m_routes_request_timer(m_io_service, ROUTES_REQUEST_PERIOD),
		m_tap_adapter_strand(m_io_service),
		m_proxies_strand(m_io_service),
		m_tap_write_queue_strand(m_io_service),
		m_arp_filter(m_ethernet_filter),
		m_ipv4_filter(m_ethernet_filter),
		m_udp_filter(m_ipv4_filter),
		m_bootp_filter(m_udp_filter),
		m_dhcp_filter(m_bootp_filter),
		m_router_strand(m_io_service),
		m_switch(m_configuration.switch_),
		m_router(m_configuration.router),
		m_route_manager(io_service)
	{
		if (!m_configuration.security.identity)
		{
			throw std::runtime_error("No user certificate or private key set. Unable to continue.");
		}

		m_arp_filter.add_handler(boost::bind(&core::do_handle_arp_frame, this, _1));
		m_dhcp_filter.add_handler(boost::bind(&core::do_handle_dhcp_frame, this, _1));

		// Setup the route manager.
		auto route_registration_success_handler = [this](const asiotap::route_manager::route_type& route){
			m_logger(LL_INFORMATION) << "Added system route: " << route;
		};

		m_route_manager.set_route_registration_success_handler(route_registration_success_handler);
		m_route_manager.set_route_registration_failure_handler([this](const asiotap::route_manager::route_type& route, const boost::system::system_error& ex){
			m_logger(LL_WARNING) << "Unable to add system route (" << route << "): " << ex.what();
		});
		m_route_manager.set_route_unregistration_success_handler([this](const asiotap::route_manager::route_type& route){
			m_logger(LL_INFORMATION) << "Removed system route: " << route;
		});
		m_route_manager.set_route_unregistration_failure_handler([this](const asiotap::route_manager::route_type& route, const boost::system::system_error& ex){
			m_logger(LL_WARNING) << "Unable to remove system route (" << route << "): " << ex.what();
		});
	}

	void core::open()
	{
		m_logger(LL_DEBUG) << "Opening core...";

		open_server();
		open_tap_adapter();

		m_logger(LL_DEBUG) << "Core opened.";
	}

	void core::close()
	{
		m_logger(LL_DEBUG) << "Closing core...";

		close_tap_adapter();
		close_server();

		m_logger(LL_DEBUG) << "Core closed.";
	}

	// Private methods

	void core::do_handle_log(log_level level, const std::string& msg, const boost::posix_time::ptime& timestamp)
	{
		// All do_handle_log() calls are done within the same strand, so the user does not need to protect his callback with a mutex that might slow things down.
		if (m_log_callback)
		{
			m_log_callback(level, msg, timestamp);
		}
	}

	bool core::is_banned(const boost::asio::ip::address& address) const
	{
		return has_address(m_configuration.fscp.never_contact_list.begin(), m_configuration.fscp.never_contact_list.end(), address);
	}

	void core::open_server()
	{
		m_server = boost::make_shared<fscp::server>(boost::ref(m_io_service), boost::cref(*m_configuration.security.identity));

		m_server->set_debug_callback([this] (fscp::server::debug_event event, const std::string& context, const boost::optional<ep_type>& ep) {

			if (ep)
			{
				m_logger(LL_TRACE) << context << ": " << event << " (" << *ep << ")";
			}
			else
			{
				m_logger(LL_TRACE) << context << ": " << event;
			}
		});

		m_server->set_cipher_suites(m_configuration.fscp.cipher_suite_capabilities);
		m_server->set_elliptic_curves(m_configuration.fscp.elliptic_curve_capabilities);

		m_server->set_hello_message_received_callback(boost::bind(&core::do_handle_hello_received, this, _1, _2));
		m_server->set_contact_request_received_callback(boost::bind(&core::do_handle_contact_request_received, this, _1, _2, _3, _4));
		m_server->set_contact_received_callback(boost::bind(&core::do_handle_contact_received, this, _1, _2, _3));
		m_server->set_presentation_message_received_callback(boost::bind(&core::do_handle_presentation_received, this, _1, _2, _3, _4));
		m_server->set_session_request_message_received_callback(boost::bind(&core::do_handle_session_request_received, this, _1, _2, _3, _4));
		m_server->set_session_message_received_callback(boost::bind(&core::do_handle_session_received, this, _1, _2, _3, _4));
		m_server->set_session_failed_callback(boost::bind(&core::do_handle_session_failed, this, _1, _2));
		m_server->set_session_error_callback(boost::bind(&core::do_handle_session_error, this, _1, _2, _3));
		m_server->set_session_established_callback(boost::bind(&core::do_handle_session_established, this, _1, _2, _3, _4));
		m_server->set_session_lost_callback(boost::bind(&core::do_handle_session_lost, this, _1, _2));
		m_server->set_data_received_callback(boost::bind(&core::do_handle_data_received, this, _1, _2, _3, _4));

		resolver_type resolver(m_io_service);

		const ep_type listen_endpoint = boost::apply_visitor(
			asiotap::endpoint_resolve_visitor(
				resolver,
				to_protocol(m_configuration.fscp.hostname_resolution_protocol),
				resolver_query::address_configured | resolver_query::passive, DEFAULT_SERVICE
			),
			m_configuration.fscp.listen_on
		);

		m_logger(LL_IMPORTANT) << "Core set to listen on: " << listen_endpoint;

		if (m_configuration.security.certificate_validation_method == security_configuration::CVM_DEFAULT)
		{
			m_ca_store = cryptoplus::x509::store::create();

			BOOST_FOREACH(const cert_type& cert, m_configuration.security.certificate_authority_list)
			{
				m_ca_store.add_certificate(cert);
			}

			BOOST_FOREACH(const crl_type& crl, m_configuration.security.certificate_revocation_list_list)
			{
				m_ca_store.add_certificate_revocation_list(crl);
			}

			switch (m_configuration.security.certificate_revocation_validation_method)
			{
				case security_configuration::CRVM_LAST:
					{
						m_ca_store.set_verification_flags(X509_V_FLAG_CRL_CHECK);
						break;
					}
				case security_configuration::CRVM_ALL:
					{
						m_ca_store.set_verification_flags(X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
						break;
					}
				case security_configuration::CRVM_NONE:
					{
						break;
					}
			}
		}

		for(auto&& network_address : m_configuration.fscp.never_contact_list)
		{
			m_logger(LL_INFORMATION) << "Configured not to accept requests from: " << network_address;
		}

		// Let's open the server.
		m_server->open(listen_endpoint);

#ifdef LINUX
		if (!m_configuration.fscp.listen_on_device.empty())
		{
			const auto socket_fd = m_server->get_socket().native();
			const std::string device_name = m_configuration.fscp.listen_on_device;

			if (::setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, device_name.c_str(), device_name.size()) == 0)
			{
				m_logger(LL_IMPORTANT) << "Restricting VPN traffic on: " << device_name;
			}
			else
			{
				m_logger(LL_WARNING) << "Unable to restrict traffic on: " << device_name << ". Error was: " << boost::system::error_code(errno, boost::system::system_category()).message();
			}
		}
#endif

		// We start the contact loop.
		async_contact_all();

		m_contact_timer.async_wait(boost::bind(&core::do_handle_periodic_contact, this, boost::asio::placeholders::error));
		m_dynamic_contact_timer.async_wait(boost::bind(&core::do_handle_periodic_dynamic_contact, this, boost::asio::placeholders::error));
		m_routes_request_timer.async_wait(boost::bind(&core::do_handle_periodic_routes_request, this, boost::asio::placeholders::error));
	}

	void core::close_server()
	{
		// Stop the contact loop timers.
		m_routes_request_timer.cancel();
		m_dynamic_contact_timer.cancel();
		m_contact_timer.cancel();

		m_server->close();
	}

	void core::async_contact(const endpoint& target, duration_handler_type handler)
	{
		m_logger(LL_DEBUG) << "Resolving " << target << " for potential contact...";

		// This is a ugly workaround for a bug in Boost::Variant (<1.55)
		endpoint target1 = target;

		const auto resolve_handler = [this, handler, target1] (const boost::system::error_code& ec, boost::asio::ip::udp::resolver::iterator it)
		{
			if (!ec)
			{
				const ep_type host = *it;

				// This is a ugly workaround for a bug in Boost::Variant (<1.55)
				endpoint target2 = target1;

				// The host was resolved: we first make sure no session exist with that host before doing anything else.
				m_server->async_has_session_with_endpoint(
					host,
					[this, handler, host, target2] (bool has_session)
					{
						if (!has_session)
						{
							m_logger(LL_DEBUG) << "No session exists with " << target2 << " (at " << host << "). Contacting...";

							do_contact(host, handler);
						}
						else
						{
							m_logger(LL_DEBUG) << "A session already exists with " << target2 << " (at " << host << "). Not contacting again.";
						}
					}
				);
			}
			else
			{
				handler(ep_type(), ec, boost::posix_time::time_duration());
			}
		};

		boost::apply_visitor(
			asiotap::endpoint_async_resolve_visitor(
				boost::make_shared<resolver_type>(boost::ref(m_io_service)),
				to_protocol(m_configuration.fscp.hostname_resolution_protocol),
				resolver_query::address_configured,
				DEFAULT_SERVICE,
				resolve_handler
			),
			target
		);
	}

	void core::async_contact(const endpoint& target)
	{
		async_contact(target, boost::bind(&core::do_handle_contact, this, target, _1, _2, _3));
	}

	void core::async_contact_all()
	{
		BOOST_FOREACH(const endpoint& contact, m_configuration.fscp.contact_list)
		{
			async_contact(contact);
		}
	}

	void core::async_dynamic_contact_all()
	{
		using boost::make_transform_iterator;

		hash_type (*func)(cert_type) = fscp::get_certificate_hash;

		const hash_list_type hash_list(make_transform_iterator(m_configuration.fscp.dynamic_contact_list.begin(), func), make_transform_iterator(m_configuration.fscp.dynamic_contact_list.end(), func));

		async_send_contact_request_to_all(hash_list);
	}

	void core::async_send_contact_request_to_all(const hash_list_type& hash_list, multiple_endpoints_handler_type handler)
	{
		m_server->async_send_contact_request_to_all(hash_list, handler);
	}

	void core::async_send_contact_request_to_all(const hash_list_type& hash_list)
	{
		async_send_contact_request_to_all(hash_list, boost::bind(&core::do_handle_send_contact_request_to_all, this, _1));
	}

	void core::async_introduce_to(const ep_type& target, simple_handler_type handler)
	{
		assert(m_server);

		m_server->async_introduce_to(target, handler);
	}

	void core::async_introduce_to(const ep_type& target)
	{
		async_introduce_to(target, boost::bind(&core::do_handle_introduce_to, this, target, _1));
	}

	void core::async_request_session(const ep_type& target, simple_handler_type handler)
	{
		assert(m_server);

		m_logger(LL_DEBUG) << "Sending SESSION_REQUEST to " << target << ".";

		m_server->async_request_session(target, handler);
	}

	void core::async_request_session(const ep_type& target)
	{
		async_request_session(target, boost::bind(&core::do_handle_request_session, this, target, _1));
	}

	void core::async_handle_routes_request(const ep_type& sender, const routes_request_message& msg)
	{
		// The routes request message does not contain any meaningful information.
		static_cast<void>(msg);

		m_router_strand.post(
			boost::bind(
				&core::do_handle_routes_request,
				this,
				sender
			)
		);
	}

	void core::async_handle_routes(const ep_type& sender, const routes_message& msg)
	{
		const auto version = msg.version();
		const auto routes = msg.routes();

		async_get_tap_addresses([this, sender, version, routes](const asiotap::ip_network_address_list& ip_addresses){
			m_router_strand.post(
				boost::bind(
					&core::do_handle_routes,
					this,
					ip_addresses,
					sender,
					version,
					routes
				)
			);
		});
	}

	void core::async_send_routes_request(const ep_type& target, simple_handler_type handler)
	{
		assert(m_server);

		m_logger(LL_DEBUG) << "Sending routes request to " << target << ".";

		// We take the proxy memory because we don't need much place and the tap_adapter_memory_pool is way more critical.
		const proxy_memory_pool::shared_buffer_type data_buffer = m_proxy_memory_pool.allocate_shared_buffer();

		const size_t size = routes_request_message::write(
			buffer_cast<uint8_t*>(data_buffer),
			buffer_size(data_buffer)
		);

		m_server->async_send_data(
			target,
			fscp::CHANNEL_NUMBER_1,
			buffer(data_buffer, size),
			make_shared_buffer_handler(
				data_buffer,
				handler
			)
		);
	}

	void core::async_send_routes_request(const ep_type& target)
	{
		async_send_routes_request(target, boost::bind(&core::do_handle_send_routes_request, this, target, _1));
	}

	void core::async_send_routes_request_to_all(multiple_endpoints_handler_type handler)
	{
		assert(m_server);

		m_logger(LL_DEBUG) << "Sending routes request to all hosts.";

		// We take the proxy memory because we don't need much place and the tap_adapter_memory_pool is way more critical.
		const proxy_memory_pool::shared_buffer_type data_buffer = m_proxy_memory_pool.allocate_shared_buffer();

		const size_t size = routes_request_message::write(
			buffer_cast<uint8_t*>(data_buffer),
			buffer_size(data_buffer)
		);

		m_server->async_send_data_to_all(
			fscp::CHANNEL_NUMBER_1,
			buffer(data_buffer, size),
			make_shared_buffer_handler(
				data_buffer,
				handler
			)
		);
	}

	void core::async_send_routes_request_to_all()
	{
		async_send_routes_request_to_all(boost::bind(&core::do_handle_send_routes_request_to_all, this, _1));
	}

	void core::async_send_routes(const ep_type& target, routes_message::version_type version, const asiotap::ip_route_set& routes, simple_handler_type handler)
	{
		assert(m_server);

		m_logger(LL_DEBUG) << "Sending routes to " << target << ": version " << version << " (" << routes << ").";

		const tap_adapter_memory_pool::shared_buffer_type data_buffer = m_tap_adapter_memory_pool.allocate_shared_buffer();

		const size_t size = routes_message::write(
			buffer_cast<uint8_t*>(data_buffer),
			buffer_size(data_buffer),
			version,
			routes
		);

		m_server->async_send_data(
			target,
			fscp::CHANNEL_NUMBER_1,
			buffer(data_buffer, size),
			make_shared_buffer_handler(
				data_buffer,
				handler
			)
		);
	}

	void core::do_contact(const ep_type& address, duration_handler_type handler)
	{
		assert(m_server);

		m_logger(LL_DEBUG) << "Sending HELLO to " << address;

		m_server->async_greet(address, boost::bind(handler, address, _1, _2));
	}

	void core::do_handle_contact(const endpoint& host, const ep_type& address, const boost::system::error_code& ec, const boost::posix_time::time_duration& duration)
	{
		if (!ec)
		{
			m_logger(LL_DEBUG) << "Received HELLO_RESPONSE from " << host << " at " << address << ". Latency: " << duration << "";

			async_introduce_to(address);
		}
		else
		{
			if (ec == fscp::server_error::hello_request_timed_out)
			{
				m_logger(LL_DEBUG) << "Received no HELLO_RESPONSE from " << host << " at " << address << ": " << ec.message() << " (timeout: " << duration << ")";
			}
			else
			{
				m_logger(LL_DEBUG) << "Unable to send HELLO to " << host << ": " << ec.message();
			}
		}
	}

	void core::do_handle_periodic_contact(const boost::system::error_code& ec)
	{
		if (ec != boost::asio::error::operation_aborted)
		{
			async_contact_all();

			m_contact_timer.expires_from_now(CONTACT_PERIOD);
			m_contact_timer.async_wait(boost::bind(&core::do_handle_periodic_contact, this, boost::asio::placeholders::error));
		}
	}

	void core::do_handle_periodic_dynamic_contact(const boost::system::error_code& ec)
	{
		if (ec != boost::asio::error::operation_aborted)
		{
			async_dynamic_contact_all();

			m_dynamic_contact_timer.expires_from_now(DYNAMIC_CONTACT_PERIOD);
			m_dynamic_contact_timer.async_wait(boost::bind(&core::do_handle_periodic_dynamic_contact, this, boost::asio::placeholders::error));
		}
	}

	void core::do_handle_periodic_routes_request(const boost::system::error_code& ec)
	{
		if (ec != boost::asio::error::operation_aborted)
		{
			async_send_routes_request_to_all();

			m_routes_request_timer.expires_from_now(ROUTES_REQUEST_PERIOD);
			m_routes_request_timer.async_wait(boost::bind(&core::do_handle_periodic_routes_request, this, boost::asio::placeholders::error));
		}
	}

	void core::do_handle_send_contact_request(const ep_type& target, const boost::system::error_code& ec)
	{
		if (ec)
		{
			m_logger(LL_WARNING) << "Error sending contact request to " << target << ": " << ec.message();
		}
	}

	void core::do_handle_send_contact_request_to_all(const std::map<ep_type, boost::system::error_code>& results)
	{
		for (std::map<ep_type, boost::system::error_code>::const_iterator result = results.begin(); result != results.end(); ++result)
		{
			do_handle_send_contact_request(result->first, result->second);
		}
	}

	void core::do_handle_introduce_to(const ep_type& target, const boost::system::error_code& ec)
	{
		if (ec)
		{
			m_logger(LL_WARNING) << "Error sending introduction message to " << target << ": " << ec.message();
		}
	}

	void core::do_handle_request_session(const ep_type& target, const boost::system::error_code& ec)
	{
		if (ec)
		{
			m_logger(LL_WARNING) << "Error requesting session to " << target << ": " << ec.message();
		}
	}

	void core::do_handle_send_routes_request(const ep_type& target, const boost::system::error_code& ec)
	{
		if (ec)
		{
			m_logger(LL_WARNING) << "Error sending routes request to " << target << ": " << ec.message();
		}
	}

	void core::do_handle_send_routes_request_to_all(const std::map<ep_type, boost::system::error_code>& results)
	{
		for (std::map<ep_type, boost::system::error_code>::const_iterator result = results.begin(); result != results.end(); ++result)
		{
			do_handle_send_routes_request(result->first, result->second);
		}
	}

	bool core::do_handle_hello_received(const ep_type& sender, bool default_accept)
	{
		m_logger(LL_DEBUG) << "Received HELLO_REQUEST from " << sender << ".";

		if (is_banned(sender.address()))
		{
			m_logger(LL_WARNING) << "Ignoring HELLO_REQUEST from " << sender << " as it is a banned host.";

			default_accept = false;
		}

		if (default_accept)
		{
			async_introduce_to(sender);
		}

		return default_accept;
	}

	bool core::do_handle_contact_request_received(const ep_type& sender, cert_type cert, hash_type hash, const ep_type& answer)
	{
		if (m_configuration.fscp.accept_contact_requests)
		{
			m_logger(LL_INFORMATION) << "Received contact request from " << sender << " for " << cert.subject().oneline() << " (" << hash << "). Host is at: " << answer;

			return true;
		}
		else
		{
			return false;
		}
	}

	void core::do_handle_contact_received(const ep_type& sender, hash_type hash, const ep_type& answer)
	{
		if (m_configuration.fscp.accept_contacts)
		{
			// We check if the contact belongs to the forbidden network list.
			if (is_banned(answer.address()))
			{
				m_logger(LL_WARNING) << "Received forbidden contact from " << sender << ": " << hash << " is at " << answer << " but won't be contacted.";
			}
			else
			{
				m_logger(LL_INFORMATION) << "Received contact from " << sender << ": " << hash << " is at: " << answer;

				async_contact(to_endpoint(answer));
			}
		}
	}

	bool core::do_handle_presentation_received(const ep_type& sender, cert_type sig_cert, fscp::server::presentation_status_type status, bool has_session)
	{
		if (m_logger.level() <= LL_DEBUG)
		{
			m_logger(LL_DEBUG) << "Received PRESENTATION from " << sender << ": " << sig_cert.subject().oneline() << ".";
		}

		if (is_banned(sender.address()))
		{
			m_logger(LL_WARNING) << "Ignoring PRESENTATION from " << sender << " as it is a banned host.";

			return false;
		}

		if (has_session)
		{
			m_logger(LL_WARNING) << "Ignoring PRESENTATION from " << sender << " as an active session currently exists with this host.";

			return false;
		}

		if (!certificate_is_valid(sig_cert))
		{
			m_logger(LL_WARNING) << "Ignoring PRESENTATION from " << sender << " as the signature certificate is invalid.";

			return false;
		}

		m_logger(LL_INFORMATION) << "Accepting PRESENTATION from " << sender << " (" << sig_cert.subject().oneline() << "): " << status << ".";

		async_request_session(sender);

		return true;
	}

	bool core::do_handle_session_request_received(const ep_type& sender, const fscp::cipher_suite_list_type& cscap, const fscp::elliptic_curve_list_type& eccap, bool default_accept)
	{
		m_logger(LL_DEBUG) << "Received SESSION_REQUEST from " << sender << " (default: " << (default_accept ? std::string("accept") : std::string("deny")) << ").";

		if (m_logger.level() <= LL_DEBUG)
		{
			std::ostringstream oss;

			for (auto&& cs : cscap)
			{
				oss << " " << cs;
			}

			m_logger(LL_DEBUG) << "Cipher suites capabilities:" << oss.str();

			oss.str("");

			for (auto&& ec : eccap)
			{
				oss << " " << ec;
			}

			m_logger(LL_DEBUG) << "Elliptic curve capabilities:" << oss.str();
		}

		return default_accept;
	}

	bool core::do_handle_session_received(const ep_type& sender, fscp::cipher_suite_type cs, fscp::elliptic_curve_type ec, bool default_accept)
	{
		m_logger(LL_DEBUG) << "Received SESSION from " << sender << " (default: " << (default_accept ? std::string("accept") : std::string("deny")) << ").";
		m_logger(LL_DEBUG) << "Cipher suite: " << cs;
		m_logger(LL_DEBUG) << "Elliptic curve: " << ec;

		return default_accept;
	}

	void core::do_handle_session_failed(const ep_type& host, bool is_new)
	{
		if (is_new)
		{
			m_logger(LL_WARNING) << "Session establishment with " << host << " failed.";
		}
		else
		{
			m_logger(LL_WARNING) << "Session renewal with " << host << " failed.";
		}

		if (m_session_failed_callback)
		{
			m_session_failed_callback(host, is_new);
		}
	}

	void core::do_handle_session_error(const ep_type& host, bool is_new, const std::exception& error)
	{
		if (is_new)
		{
			m_logger(LL_WARNING) << "Session establishment with " << host << " encountered an error: " << error.what();
		}
		else
		{
			m_logger(LL_WARNING) << "Session renewal with " << host << " encountered an error: " << error.what();
		}

		if (m_session_error_callback)
		{
			m_session_error_callback(host, is_new, error);
		}
	}

	void core::do_handle_session_established(const ep_type& host, bool is_new, const fscp::cipher_suite_type& cs, const fscp::elliptic_curve_type& ec)
	{
		if (is_new)
		{
			m_logger(LL_IMPORTANT) << "Session established with " << host << ".";
		}
		else
		{
			m_logger(LL_INFORMATION) << "Session renewed with " << host << ".";
		}

		m_logger(LL_INFORMATION) << "Cipher suite: " << cs;
		m_logger(LL_INFORMATION) << "Elliptic curve: " << ec;

		if (is_new)
		{
			if (m_configuration.tap_adapter.type == tap_adapter_configuration::tap_adapter_type::tap)
			{
				async_register_switch_port(host, void_handler_type());
			}
			else
			{
				// We register the router port without any routes, at first.
				async_register_router_port(host, boost::bind(&core::async_send_routes_request, this, host));
			}

			const auto route = m_route_manager.get_route_for(host.address());
			async_save_system_route(host, route, void_handler_type());
		}

		if (m_session_established_callback)
		{
			m_session_established_callback(host, is_new, cs, ec);
		}
	}

	void core::do_handle_session_lost(const ep_type& host, fscp::server::session_loss_reason reason)
	{
		m_logger(LL_IMPORTANT) << "Session with " << host << " lost (" << reason << ").";

		if (m_session_lost_callback)
		{
			m_session_lost_callback(host, reason);
		}

		if (m_configuration.tap_adapter.type == tap_adapter_configuration::tap_adapter_type::tap)
		{
			async_unregister_switch_port(host, void_handler_type());
		}
		else
		{
			async_unregister_router_port(host, void_handler_type());
		}

		async_clear_client_router_info(host, void_handler_type());
	}

	void core::do_handle_data_received(const ep_type& sender, fscp::channel_number_type channel_number, fscp::server::shared_buffer_type buffer, boost::asio::const_buffer data)
	{
		switch (channel_number)
		{
			// Channel 0 contains ethernet/ip frames
			case fscp::CHANNEL_NUMBER_0:
				if (m_configuration.tap_adapter.type == tap_adapter_configuration::tap_adapter_type::tap)
				{
					async_write_switch(
						make_port_index(sender),
						data,
						make_shared_buffer_handler(
							buffer,
							&null_switch_write_handler
						)
					);
				}
				else
				{
					async_write_router(
						make_port_index(sender),
						data,
						make_shared_buffer_handler(
							buffer,
							&null_router_write_handler
						)
					);
				}

				break;
			// Channel 1 contains messages
			case fscp::CHANNEL_NUMBER_1:
				try
				{
					const message msg(buffer_cast<const uint8_t*>(data), buffer_size(data));

					do_handle_message(sender, buffer, msg);
				}
				catch (std::runtime_error& ex)
				{
					m_logger(LL_WARNING) << "Received incorrectly formatted message from " << sender << ". Error was: " << ex.what();
				}

				break;
			default:
				m_logger(LL_WARNING) << "Received unhandled " << buffer_size(data) << " byte(s) of data on FSCP channel #" << static_cast<int>(channel_number);
				break;
		}
	}

	void core::do_handle_message(const ep_type& sender, fscp::server::shared_buffer_type, const message& msg)
	{
		switch (msg.type())
		{
			case message::MT_ROUTES_REQUEST:
				{
					routes_request_message rr_msg(msg);

					async_handle_routes_request(sender, rr_msg);

					break;
				}

			case message::MT_ROUTES:
				{
					routes_message r_msg(msg);

					async_handle_routes(sender, r_msg);

					break;
				}

			default:
				m_logger(LL_WARNING) << "Received unhandled message of type " << static_cast<int>(msg.type()) << " on the message channel";
				break;
		}
	}

	void core::do_handle_routes_request(const ep_type& sender)
	{
		// All calls to do_handle_routes_request() are done within the m_router_strand, so the following is safe.
		if (!m_configuration.router.accept_routes_requests)
		{
			m_logger(LL_DEBUG) << "Received routes request from " << sender << " but ignoring as specified in the configuration";
		}
		else
		{
			if (m_tap_adapter && (m_tap_adapter->layer() == asiotap::tap_adapter_layer::ip))
			{
				const auto local_port = m_router.get_port(make_port_index(m_tap_adapter));

				if (m_local_routes_version.is_initialized())
				{
					const auto& routes = local_port->local_routes();

					m_logger(LL_DEBUG) << "Received routes request from " << sender << ". Replying with version " << *m_local_routes_version << ": " << routes;

					async_send_routes(sender, *m_local_routes_version, routes, &null_simple_write_handler);
				}
				else
				{
					m_logger(LL_DEBUG) << "Received routes request from " << sender << " but no local routes are set. Not sending anything.";
				}
			}
			else
			{
				const auto routes = m_configuration.router.local_ip_routes;
				const auto version = 0;

				m_logger(LL_DEBUG) << "Received routes request from " << sender << ". Replying with version " << version << ": " << routes;

				async_send_routes(sender, version, routes, &null_simple_write_handler);
			}
		}
	}

	void core::do_handle_routes(const asiotap::ip_network_address_list& tap_addresses, const ep_type& sender, routes_message::version_type version, const asiotap::ip_route_set& routes)
	{
		// All calls to do_handle_routes() are done within the m_router_strand, so the following is safe.

		client_router_info_type& client_router_info = m_client_router_info_map[sender];

		if (!client_router_info.is_older_than(version))
		{
			m_logger(LL_DEBUG) << "Ignoring old routes message with version " << version << " as current version is " << *client_router_info.version;

			return;
		}

		if (!m_tap_adapter)
		{
			m_logger(LL_INFORMATION) << "Ignoring routes message as no tap adapter is currently associated.";

			return;
		}

		asiotap::ip_route_set filtered_routes;

		if (m_tap_adapter->layer() == asiotap::tap_adapter_layer::ip)
		{
			if (m_configuration.router.internal_route_acceptance_policy == router_configuration::internal_route_scope_type::none)
			{
				m_logger(LL_WARNING) << "Received routes from " << sender << " (version " << version << ") will be ignored, as the configuration requires: " << routes;

				return;
			}

			const auto port = m_router.get_port(make_port_index(sender));

			filtered_routes = filter_routes(routes, m_configuration.router.internal_route_acceptance_policy, m_configuration.router.maximum_routes_limit, tap_addresses);

			if (filtered_routes != routes)
			{
				if (filtered_routes.empty() && !routes.empty())
				{
					m_logger(LL_WARNING) << "Received routes from " << sender << " (version " << version << ") but none matched the internal route acceptance policy (" << m_configuration.router.internal_route_acceptance_policy << ", limit " << m_configuration.router.maximum_routes_limit << "): " << routes;

					return;
				}
				else
				{
					asiotap::ip_route_set excluded_routes;
					std::set_difference(routes.begin(), routes.end(), filtered_routes.begin(), filtered_routes.end(), std::inserter(excluded_routes, excluded_routes.end()));

					m_logger(LL_WARNING) << "Received routes from " << sender << " (version " << version << ") but some did not match the internal route acceptance policy (" << m_configuration.router.internal_route_acceptance_policy << ", limit " << m_configuration.router.maximum_routes_limit << "): " << excluded_routes;
				}
			}

			if (port)
			{
				port->set_local_routes(filtered_routes);

				m_logger(LL_INFORMATION) << "Received routes from " << sender << " (version " << version << ") were applied: " << filtered_routes;
			}
			else
			{
				m_logger(LL_DEBUG) << "Received routes from " << sender << " but unable to get the associated router port. Doing nothing";
			}
		}
		else
		{
			if (m_configuration.router.system_route_acceptance_policy == router_configuration::system_route_scope_type::none)
			{
				m_logger(LL_WARNING) << "Received routes from " << sender << " (version " << version << ") will be ignored, as the configuration requires: " << routes;

				return;
			}

			filtered_routes = routes;
		}

		// Silently filter out routes that are already covered by the default interface routing table entries (aka. routes that belong to the interface's network).
		asiotap::ip_route_set filtered_system_routes;

		for (auto&& ina: tap_addresses)
		{
			for (auto&& route : filtered_routes)
			{
				if (!has_network(ina, network_address(route)))
				{
					filtered_system_routes.insert(route);
				}
			}
		}

		const auto system_routes = filter_routes(filtered_system_routes, m_configuration.router.system_route_acceptance_policy, m_configuration.router.maximum_routes_limit);

		if (system_routes != filtered_system_routes)
		{
			if (system_routes.empty() && !filtered_system_routes.empty())
			{
				m_logger(LL_WARNING) << "Received system routes from " << sender << " (version " << version << ") but none matched the system route acceptance policy (" << m_configuration.router.system_route_acceptance_policy << ", limit " << m_configuration.router.maximum_routes_limit << "): " << filtered_system_routes;

				return;
			}
			else
			{
				asiotap::ip_route_set excluded_routes;
				std::set_difference(filtered_system_routes.begin(), filtered_system_routes.end(), system_routes.begin(), system_routes.end(), std::inserter(excluded_routes, excluded_routes.end()));

				m_logger(LL_WARNING) << "Received system routes from " << sender << " (version " << version << ") but some did not match the system route acceptance policy (" << m_configuration.router.system_route_acceptance_policy << ", limit " << m_configuration.router.maximum_routes_limit << "): " << excluded_routes;
			}
		}

		client_router_info_type new_client_router_info;
		new_client_router_info.saved_system_route = client_router_info.saved_system_route;
		new_client_router_info.version = client_router_info.version;

		for (auto&& route : filtered_system_routes)
		{
			new_client_router_info.system_route_entries.push_back(m_route_manager.get_route_entry(m_tap_adapter->get_route(route)));
		}

		client_router_info = new_client_router_info;
	}

	int core::certificate_validation_callback(int ok, X509_STORE_CTX* ctx)
	{
		cryptoplus::x509::store_context store_context(ctx);

		core* _this = static_cast<core*>(store_context.get_external_data(core::ex_data_index));

		return (_this->certificate_validation_method(ok != 0, store_context)) ? 1 : 0;
	}

	bool core::certificate_validation_method(bool ok, cryptoplus::x509::store_context store_context)
	{
		cert_type cert = store_context.get_current_certificate();

		if (!ok)
		{
			m_logger(LL_WARNING) << "Error when validating " << cert.subject().oneline() << ": " << store_context.get_error_string() << " (depth: " << store_context.get_error_depth() << ")";
		}
		else
		{
			m_logger(LL_INFORMATION) << cert.subject().oneline() << " is valid.";
		}

		return ok;
	}

	bool core::certificate_is_valid(cert_type cert)
	{
		switch (m_configuration.security.certificate_validation_method)
		{
			case security_configuration::CVM_DEFAULT:
				{
					using namespace cryptoplus;

					// We can't easily ensure m_ca_store is used only in one strand, so we protect it with a mutex instead.
					boost::mutex::scoped_lock lock(m_ca_store_mutex);

					// Create a store context to proceed to verification
					x509::store_context store_context = x509::store_context::create();

					store_context.initialize(m_ca_store, cert, NULL);

					// Ensure to set the verification callback *AFTER* you called initialize or it will be ignored.
					store_context.set_verification_callback(&core::certificate_validation_callback);

					// Add a reference to the current instance into the store context.
					store_context.set_external_data(core::ex_data_index, this);

					if (!store_context.verify())
					{
						return false;
					}

					break;
				}
			case security_configuration::CVM_NONE:
				{
					break;
				}
		}

		if (m_certificate_validation_callback)
		{
			return m_certificate_validation_callback(cert);
		}

		return true;
	}

	void core::open_tap_adapter()
	{
		if (m_configuration.tap_adapter.enabled)
		{
			const asiotap::tap_adapter_layer tap_adapter_type = (m_configuration.tap_adapter.type == tap_adapter_configuration::tap_adapter_type::tap) ? asiotap::tap_adapter_layer::ethernet : asiotap::tap_adapter_layer::ip;

			m_tap_adapter = boost::make_shared<asiotap::tap_adapter>(boost::ref(m_io_service), tap_adapter_type);

			const auto write_func = [this] (boost::asio::const_buffer data, simple_handler_type handler) {
				async_write_tap(buffer(data), [handler](const boost::system::error_code& ec, size_t) {
					handler(ec);
				});
			};

			m_tap_adapter->open(m_configuration.tap_adapter.name);

			asiotap::tap_adapter_configuration tap_config;

			// The device MTU.
			tap_config.mtu = compute_mtu(m_configuration.tap_adapter.mtu, get_auto_mtu_value());

			m_logger(LL_IMPORTANT) << "Tap adapter \"" << *m_tap_adapter << "\" opened in mode " << m_configuration.tap_adapter.type << " with a MTU set to: " << tap_config.mtu;

			// IPv4 address
			if (!m_configuration.tap_adapter.ipv4_address_prefix_length.is_null())
			{
				m_logger(LL_INFORMATION) << "IPv4 address: " << m_configuration.tap_adapter.ipv4_address_prefix_length;

				tap_config.ipv4.network_address = { m_configuration.tap_adapter.ipv4_address_prefix_length.address(), m_configuration.tap_adapter.ipv4_address_prefix_length.prefix_length() };
			}
			else
			{
				if (m_configuration.tap_adapter.type == tap_adapter_configuration::tap_adapter_type::tun)
				{
					throw std::runtime_error("No IPv4 address configured but we are in tun mode: unable to continue");
				}
				else
				{
					m_logger(LL_INFORMATION) << "No IPv4 address configured.";
				}
			}

			// IPv6 address
			if (!m_configuration.tap_adapter.ipv6_address_prefix_length.is_null())
			{
				m_logger(LL_INFORMATION) << "IPv6 address: " << m_configuration.tap_adapter.ipv6_address_prefix_length;

				tap_config.ipv6.network_address = { m_configuration.tap_adapter.ipv6_address_prefix_length.address(), m_configuration.tap_adapter.ipv6_address_prefix_length.prefix_length() };
			}
			else
			{
				m_logger(LL_INFORMATION) << "No IPv6 address configured.";
			}

			if (m_configuration.tap_adapter.type == tap_adapter_configuration::tap_adapter_type::tun)
			{
				if (m_configuration.tap_adapter.remote_ipv4_address)
				{
					m_logger(LL_INFORMATION) << "IPv4 remote address: " << m_configuration.tap_adapter.remote_ipv4_address->to_string();

					tap_config.ipv4.remote_address = *m_configuration.tap_adapter.remote_ipv4_address;
				}
				else
				{
					const boost::asio::ip::address_v4 remote_ipv4_address = m_configuration.tap_adapter.ipv4_address_prefix_length.get_network_address();

					m_logger(LL_INFORMATION) << "No IPv4 remote address configured. Using a default of: " << remote_ipv4_address.to_string();

					tap_config.ipv4.remote_address = remote_ipv4_address;
				}
			}

			m_tap_adapter->configure(tap_config);

#ifdef WINDOWS
			const auto metric_value = get_metric_value(m_configuration.tap_adapter.metric);

			if (metric_value)
			{
				m_logger(LL_INFORMATION) << "Setting interface metric to: " << *metric_value;

				m_tap_adapter->set_metric(*metric_value);
			}
#endif

			m_tap_adapter->set_connected_state(true);

			if (tap_adapter_type == asiotap::tap_adapter_layer::ethernet)
			{
				// Registers the switch port.
				m_switch.register_port(make_port_index(m_tap_adapter), switch_::port_type(write_func, TAP_ADAPTERS_GROUP));

				// The ARP proxy
				if (m_configuration.tap_adapter.arp_proxy_enabled)
				{
					m_arp_proxy.reset(new arp_proxy_type());
					m_arp_proxy->set_arp_request_callback(boost::bind(&core::do_handle_arp_request, this, _1, _2));
				}
				else
				{
					m_arp_proxy.reset();
				}

				// The DHCP proxy
				if (m_configuration.tap_adapter.dhcp_proxy_enabled)
				{
					m_dhcp_proxy.reset(new dhcp_proxy_type());
					m_dhcp_proxy->set_hardware_address(m_tap_adapter->ethernet_address().data());

					if (!m_configuration.tap_adapter.dhcp_server_ipv4_address_prefix_length.is_null())
					{
						m_dhcp_proxy->set_software_address(m_configuration.tap_adapter.dhcp_server_ipv4_address_prefix_length.address());
					}

					if (!m_configuration.tap_adapter.ipv4_address_prefix_length.is_null())
					{
						m_dhcp_proxy->add_entry(
								m_tap_adapter->ethernet_address().data(),
								m_configuration.tap_adapter.ipv4_address_prefix_length.address(),
								m_configuration.tap_adapter.ipv4_address_prefix_length.prefix_length()
						);
					}
				}
				else
				{
					m_dhcp_proxy.reset();
				}
			}
			else
			{
				// Registers the router port.
				m_router.register_port(make_port_index(m_tap_adapter), router::port_type(write_func, TAP_ADAPTERS_GROUP));

				// Add the routes.
				auto local_routes = m_configuration.router.local_ip_routes;

				const auto tap_ip_addresses = m_tap_adapter->get_ip_addresses();

				for (auto&& ip_address : tap_ip_addresses)
				{
					local_routes.insert(asiotap::to_network_address(asiotap::ip_address(ip_address)));
				}

				m_local_routes_version = routes_message::version_type();
				m_router.get_port(make_port_index(m_tap_adapter))->set_local_routes(local_routes);

				if (local_routes.empty())
				{
					m_logger(LL_INFORMATION) << "Not advertising any route";
				}
				else
				{
					m_logger(LL_INFORMATION) << "Advertising the following routes: " << local_routes;
				}

				// We don't need any proxies in TUN mode.
				m_arp_proxy.reset();
				m_dhcp_proxy.reset();
			}

			if (m_tap_adapter_up_callback)
			{
				m_tap_adapter_up_callback(*m_tap_adapter);
			}

			async_read_tap();
		}
		else
		{
			m_tap_adapter.reset();
		}
	}

	void core::close_tap_adapter()
	{
		// Clear the endpoint routes, if any.
		m_router_strand.post([this](){
			m_client_router_info_map.clear();
		});

		m_dhcp_proxy.reset();
		m_arp_proxy.reset();

		if (m_tap_adapter)
		{
			if (m_tap_adapter_down_callback)
			{
				m_tap_adapter_down_callback(*m_tap_adapter);
			}

			m_router_strand.post([this](){
				m_switch.unregister_port(make_port_index(m_tap_adapter));
				m_router.unregister_port(make_port_index(m_tap_adapter));
			});

			m_tap_adapter->cancel();
			m_tap_adapter->set_connected_state(false);

			m_tap_adapter->close();
		}
	}

	void core::async_get_tap_addresses(ip_network_address_list_handler_type handler)
	{
		if (m_tap_adapter)
		{
			m_tap_adapter_strand.post([this, handler](){
			handler(m_tap_adapter->get_ip_addresses());
			});
		}
		else
		{
			handler(asiotap::ip_network_address_list {});
		}
	}

	void core::async_read_tap()
	{
		m_tap_adapter_strand.post(boost::bind(&core::do_read_tap, this));
	}

	void core::push_tap_write(void_handler_type handler)
	{
		// All push_write() calls are done in the same strand so the following is thread-safe.
		if (m_tap_write_queue.empty())
		{
			// Nothing is being written, lets start the write immediately.
			m_tap_adapter_strand.post(make_causal_handler(handler, m_tap_write_queue_strand.wrap(boost::bind(&core::pop_tap_write, this))));
		}

		m_tap_write_queue.push(handler);
	}

	void core::pop_tap_write()
	{
		// All pop_write() calls are done in the same strand so the following is thread-safe.
		m_tap_write_queue.pop();

		if (!m_tap_write_queue.empty())
		{
			m_tap_adapter_strand.post(make_causal_handler(m_tap_write_queue.front(), m_tap_write_queue_strand.wrap(boost::bind(&core::pop_tap_write, this))));
		}
	}

	void core::do_read_tap()
	{
		// All calls to do_read_tap() are done within the m_tap_adapter_strand, so the following is safe.
		assert(m_tap_adapter);

		const tap_adapter_memory_pool::shared_buffer_type receive_buffer = m_tap_adapter_memory_pool.allocate_shared_buffer();

		m_tap_adapter->async_read(
			buffer(receive_buffer),
			m_proxies_strand.wrap(
				boost::bind(
					&core::do_handle_tap_adapter_read,
					this,
					receive_buffer,
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred
				)
			)
		);
	}

	void core::do_handle_tap_adapter_read(tap_adapter_memory_pool::shared_buffer_type receive_buffer, const boost::system::error_code& ec, size_t count)
	{
		// All calls to do_read_tap() are done within the m_proxies_strand, so the following is safe.
		if (ec != boost::asio::error::operation_aborted)
		{
			// We try to read again, as soon as possible.
			async_read_tap();
		}

		if (!ec)
		{
			const boost::asio::const_buffer data = buffer(receive_buffer, count);

#ifdef FREELAN_DEBUG
			std::cerr << "Read " << buffer_size(data) << " byte(s) on " << *m_tap_adapter << std::endl;
#endif

			if (m_tap_adapter->layer() == asiotap::tap_adapter_layer::ethernet)
			{
				bool handled = false;

				if (m_arp_proxy || m_dhcp_proxy)
				{
					// This line will eventually call the filters callbacks.
					m_ethernet_filter.parse(data);

					if (m_arp_proxy && m_arp_filter.get_last_helper())
					{
						handled = true;
						m_arp_filter.clear_last_helper();
					}

					if (m_dhcp_proxy && m_dhcp_filter.get_last_helper())
					{
						handled = true;
						m_dhcp_filter.clear_last_helper();
					}
				}

				if (!handled)
				{
					async_write_switch(
						make_port_index(m_tap_adapter),
						data,
						make_shared_buffer_handler(
							receive_buffer,
							&null_switch_write_handler
						)
					);
				}
			}
			else
			{
				// This is a TUN interface. We receive either IPv4 or IPv6 frames.
				async_write_router(
					make_port_index(m_tap_adapter),
					data,
					make_shared_buffer_handler(
						receive_buffer,
						&null_router_write_handler
					)
				);
			}
		}
		else if (ec != boost::asio::error::operation_aborted)
		{
			m_logger(LL_ERROR) << "Read failed on " << m_tap_adapter->name() << ". Error: " << ec.message();
		}
	}

	void core::do_handle_tap_adapter_write(const boost::system::error_code& ec)
	{
		if (ec)
		{
			if (ec != boost::asio::error::operation_aborted)
			{
				m_logger(LL_WARNING) << "Write failed on " << m_tap_adapter->name() << ". Error: " << ec.message();
			}
		}
	}

	void core::do_handle_arp_frame(const arp_helper_type& helper)
	{
		if (m_arp_proxy)
		{
			const proxy_memory_pool::shared_buffer_type response_buffer = m_proxy_memory_pool.allocate_shared_buffer();

			const boost::optional<boost::asio::const_buffer> data = m_arp_proxy->process_frame(
				*m_arp_filter.parent().get_last_helper(),
				helper,
				buffer(response_buffer)
			);

			if (data)
			{
				async_write_tap(
					buffer(*data),
					make_shared_buffer_handler(
						response_buffer,
						boost::bind(
							&core::do_handle_tap_adapter_write,
							this,
							boost::asio::placeholders::error
						)
					)
				);
			}
		}
	}

	void core::do_handle_dhcp_frame(const dhcp_helper_type& helper)
	{
		if (m_dhcp_proxy)
		{
			const proxy_memory_pool::shared_buffer_type response_buffer = m_proxy_memory_pool.allocate_shared_buffer();

			const boost::optional<boost::asio::const_buffer> data = m_dhcp_proxy->process_frame(
				*m_dhcp_filter.parent().parent().parent().parent().get_last_helper(),
				*m_dhcp_filter.parent().parent().parent().get_last_helper(),
				*m_dhcp_filter.parent().parent().get_last_helper(),
				*m_dhcp_filter.parent().get_last_helper(),
				helper,
				buffer(response_buffer)
			);

			if (data)
			{
				async_write_tap(
					buffer(*data),
					make_shared_buffer_handler(
						response_buffer,
						boost::bind(
							&core::do_handle_tap_adapter_write,
							this,
							boost::asio::placeholders::error
						)
					)
				);
			}
		}
	}

	bool core::do_handle_arp_request(const boost::asio::ip::address_v4& logical_address, ethernet_address_type& ethernet_address)
	{
		if (!m_configuration.tap_adapter.ipv4_address_prefix_length.is_null())
		{
			if (logical_address != m_configuration.tap_adapter.ipv4_address_prefix_length.address())
			{
				ethernet_address = m_configuration.tap_adapter.arp_proxy_fake_ethernet_address;

				return true;
			}
		}

		return false;
	}

	void core::do_register_switch_port(const ep_type& host, void_handler_type handler)
	{
		// All calls to do_register_switch_port() are done within the m_router_strand, so the following is safe.
		m_switch.register_port(make_port_index(host), switch_::port_type(boost::bind(&fscp::server::async_send_data, m_server, host, fscp::CHANNEL_NUMBER_0, _1, _2), ENDPOINTS_GROUP));

		if (handler)
		{
			handler();
		}
	}

	void core::do_unregister_switch_port(const ep_type& host, void_handler_type handler)
	{
		// All calls to do_unregister_switch_port() are done within the m_router_strand, so the following is safe.
		m_switch.unregister_port(make_port_index(host));

		if (handler)
		{
			handler();
		}
	}

	void core::do_register_router_port(const ep_type& host, void_handler_type handler)
	{
		// All calls to do_register_router_port() are done within the m_router_strand, so the following is safe.
		m_router.register_port(make_port_index(host), router::port_type(boost::bind(&fscp::server::async_send_data, m_server, host, fscp::CHANNEL_NUMBER_0, _1, _2), ENDPOINTS_GROUP));

		if (handler)
		{
			handler();
		}
	}

	void core::do_unregister_router_port(const ep_type& host, void_handler_type handler)
	{
		// All calls to do_unregister_router_port() are done within the m_router_strand, so the following is safe.
		m_router.unregister_port(make_port_index(host));

		if (handler)
		{
			handler();
		}
	}

	void core::do_save_system_route(const ep_type& host, const route_type& route, void_handler_type handler)
	{
		// All calls to do_save_system_route() are done within the m_router_strand, so the following is safe.
		client_router_info_type& client_router_info = m_client_router_info_map[host];
		client_router_info.saved_system_route = m_route_manager.get_route_entry(route);

		if (handler)
		{
			handler();
		}
	}

	void core::do_clear_client_router_info(const ep_type& host, void_handler_type handler)
	{
		// All calls to do_clear_client_router_info() are done within the m_router_strand, so the following is safe.

		// This clears the routes, if any.
		m_client_router_info_map.erase(host);

		if (handler)
		{
			handler();
		}
	}

	void core::do_write_switch(const port_index_type& index, boost::asio::const_buffer data, switch_::multi_write_handler_type handler)
	{
		// All calls to do_write_switch() are done within the m_router_strand, so the following is safe.
		m_switch.async_write(index, data, handler);
	}

	void core::do_write_router(const port_index_type& index, boost::asio::const_buffer data, router::port_type::write_handler_type handler)
	{
		// All calls to do_write_router() are done within the m_router_strand, so the following is safe.
		m_router.async_write(index, data, handler);
	}
}
