#include "stdafx.h"
#include "routemanager.h"
#include "helpers.h"
#include <libcommon/error.h>
#include <libcommon/memory.h>
#include <libcommon/string.h>
#include <libcommon/network/adapters.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include <sstream>

using AutoLockType = std::scoped_lock<std::mutex>;
using AutoRecursiveLockType = std::scoped_lock<std::recursive_mutex>;
using namespace std::placeholders;

namespace winnet::routing
{

namespace
{

using Adapters = common::network::Adapters;

NET_LUID InterfaceLuidFromGateway(const NodeAddress &gateway)
{
	const DWORD adapterFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER
		| GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_INCLUDE_GATEWAYS;

	Adapters adapters(gateway.si_family, adapterFlags);

	//
	// Process adapters to find matching ones.
	//

	std::vector<const IP_ADAPTER_ADDRESSES *> matches;

	for (auto adapter = adapters.next(); nullptr != adapter; adapter = adapters.next())
	{
		if (false == AdapterInterfaceEnabled(adapter, gateway.si_family))
		{
			continue;
		}

		auto gateways = IsolateGatewayAddresses(adapter->FirstGatewayAddress, gateway.si_family);

		if (AddressPresent(gateways, &gateway))
		{
			matches.emplace_back(adapter);
		}
	}

	if (matches.empty())
	{
		THROW_ERROR("Unable to find network adapter with specified gateway");
	}

	//
	// Sort matching interfaces ascending by metric.
	//

	const bool targetV4 = (AF_INET == gateway.si_family);

	std::sort(matches.begin(), matches.end(), [&targetV4](const IP_ADAPTER_ADDRESSES *lhs, const IP_ADAPTER_ADDRESSES *rhs)
	{
		if (targetV4)
		{
			return lhs->Ipv4Metric < rhs->Ipv4Metric;
		}

		return lhs->Ipv6Metric < rhs->Ipv6Metric;
	});

	//
	// Select the interface with the best (lowest) metric.
	//

	return matches[0]->Luid;
}

bool ParseStringEncodedLuid(const std::wstring &encodedLuid, NET_LUID &luid)
{
	//
	// The `#` is a valid character in adapter names so we use `?` instead.
	// The LUID is thus prefixed with `?` and hex encoded and left-padded with zeroes.
	// E.g. `?deadbeefcafebabe` or `?000dbeefcafebabe`.
	//

	static const size_t StringEncodedLuidLength = 17;

	if (encodedLuid.size() != StringEncodedLuidLength
		|| L'?' != encodedLuid[0])
	{
		return false;
	}

	try
	{
		std::wstringstream ss;

		ss << std::hex << &encodedLuid[1];
		ss >> luid.Value;
	}
	catch (...)
	{
		const auto msg = std::string("Failed to parse string encoded LUID: ")
			.append(common::string::ToAnsi(encodedLuid));

		THROW_ERROR(msg.c_str());
	}

	return true;
}

InterfaceAndGateway ResolveNode(ADDRESS_FAMILY family, const std::optional<Node> &optionalNode)
{
	//
	// There are four cases:
	//
	// Unspecified node (use interface and gateway of default route).
	// Node is specified by name.
	// Node is specified by name and gateway.
	// Node is specified by gateway.
	//

	if (false == optionalNode.has_value())
	{
		return GetBestDefaultRoute(family);
	}

	const auto &node = optionalNode.value();

	if (node.deviceName().has_value())
	{
		const auto &deviceName = node.deviceName().value();
		NET_LUID luid;

		if (false == ParseStringEncodedLuid(deviceName, luid)
			&& 0 != ConvertInterfaceAliasToLuid(deviceName.c_str(), &luid))
		{
			const auto msg = std::string("Unable to derive interface LUID from interface alias: ")
				.append(common::string::ToAnsi(deviceName));

			THROW_ERROR(msg.c_str());
		}

		auto onLinkProvider = [&family]()
		{
			NodeAddress onLink = { 0 };
			onLink.si_family = family;

			return onLink;
		};

		return InterfaceAndGateway{ luid, node.gateway().value_or(onLinkProvider()) };
	}

	//
	// The node is specified only by gateway.
	//

	return InterfaceAndGateway{ InterfaceLuidFromGateway(node.gateway().value()), node.gateway().value() };
}

std::wstring FormatNetwork(const Network &network)
{
	using namespace common::string;

	switch (network.Prefix.si_family)
	{
		case AF_INET:
		{
			return FormatIpv4<AddressOrder::NetworkByteOrder>(network.Prefix.Ipv4.sin_addr.s_addr, network.PrefixLength);
		}
		case AF_INET6:
		{
			return FormatIpv6(network.Prefix.Ipv6.sin6_addr.u.Byte, network.PrefixLength);
		}
		default:
		{
			return L"Failed to format network details";
		}
	}
}

} // anonymous namespace

RouteManager::RouteManager(std::shared_ptr<common::logging::ILogSink> logSink)
	: m_logSink(logSink)
	, m_routeMonitorV4(std::make_unique<DefaultRouteMonitor>(
		static_cast<ADDRESS_FAMILY>(AF_INET),
		std::bind(&RouteManager::defaultRouteChanged, this, static_cast<ADDRESS_FAMILY>(AF_INET), _1, _2),
		logSink
	))
	, m_routeMonitorV6(std::make_unique<DefaultRouteMonitor>(
		static_cast<ADDRESS_FAMILY>(AF_INET6),
		std::bind(&RouteManager::defaultRouteChanged, this, static_cast<ADDRESS_FAMILY>(AF_INET6), _1, _2),
		logSink
	))
{
}

RouteManager::~RouteManager()
{
	//
	// Stop callbacks that are triggered by events in Windows from coming in.
	//

	m_routeMonitorV4.reset();
	m_routeMonitorV6.reset();

	//
	// Delete all routes owned by us.
	//

	for (const auto &record : m_routes)
	{
		try
		{
			deleteFromRoutingTable(record.registeredRoute);
		}
		catch (const std::exception &ex)
		{
			std::wstringstream ss;

			ss << L"Failed to delete route as part of cleaning up, Route: "
				<< FormatRegisteredRoute(record.registeredRoute);

			m_logSink->error(common::string::ToAnsi(ss.str()).c_str());
			m_logSink->error(ex.what());
		}
	}
}

void RouteManager::addRoutes(const std::vector<Route> &routes)
{
	AutoLockType lock(m_routesLock);

	std::vector<EventEntry> eventLog;

	for (const auto &route : routes)
	{
		try
		{
			RouteRecord newRecord{ route, addIntoRoutingTable(route) };

			eventLog.emplace_back(EventEntry{ EventType::ADD_ROUTE, newRecord });

			auto existingRecord = findRouteRecord(newRecord.registeredRoute);

			if (m_routes.end() == existingRecord)
			{
				m_routes.emplace_back(std::move(newRecord));
			}
			else
			{
				*existingRecord = std::move(newRecord);
			}
		}
		catch (...)
		{
			undoEvents(eventLog);

			THROW_ERROR("Failed during batch insertion of routes");
		}
	}
}

void RouteManager::deleteRoutes(const std::vector<Route> &routes)
{
	AutoLockType lock(m_routesLock);

	std::vector<EventEntry> eventLog;

	for (const auto &route : routes)
	{
		try
		{
			const auto record = findRouteRecordFromSpec(route);

			if (m_routes.end() == record)
			{
				const auto err = std::wstring(L"Request to delete unknown route: ")
					.append(FormatNetwork(route.network()));

				m_logSink->warning(common::string::ToAnsi(err).c_str());

				continue;
			}

			deleteFromRoutingTable(record->registeredRoute);

			eventLog.emplace_back(EventEntry{ EventType::DELETE_ROUTE, *record });
			m_routes.erase(record);
		}
		catch (...)
		{
			undoEvents(eventLog);

			THROW_ERROR("Failed during batch removal of routes");
		}
	}
}

RouteManager::CallbackHandle RouteManager::registerDefaultRouteChangedCallback(DefaultRouteChangedCallback callback)
{
	AutoRecursiveLockType lock(m_defaultRouteCallbacksLock);

	m_defaultRouteCallbacks.emplace_back(callback);

	// Return raw address of record in list.
	return &m_defaultRouteCallbacks.back();
}

void RouteManager::unregisterDefaultRouteChangedCallback(CallbackHandle handle)
{
	AutoRecursiveLockType lock(m_defaultRouteCallbacksLock);

	for (auto it = m_defaultRouteCallbacks.begin(); it != m_defaultRouteCallbacks.end(); ++it)
	{
		// Match on raw address of record.
		if (&*it == handle)
		{
			m_defaultRouteCallbacks.erase(it);
			return;
		}
	}
}

std::list<RouteManager::RouteRecord>::iterator RouteManager::findRouteRecord(const RegisteredRoute &route)
{
	return std::find_if(m_routes.begin(), m_routes.end(), [&route](const auto &record)
	{
		return route == record.registeredRoute;
	});
}

std::list<RouteManager::RouteRecord>::iterator RouteManager::findRouteRecordFromSpec(const Route &route)
{
	return std::find_if(m_routes.begin(), m_routes.end(), [&route](const auto &record)
	{
		return route == record.route;
	});
}

RouteManager::RegisteredRoute RouteManager::addIntoRoutingTable(const Route &route)
{
	const auto node = ResolveNode(route.network().Prefix.si_family, route.node());

	MIB_IPFORWARD_ROW2 spec;

	InitializeIpForwardEntry(&spec);

	spec.InterfaceLuid = node.iface;
	spec.DestinationPrefix = route.network();
	spec.NextHop = node.gateway;
	spec.Metric = 0;
	spec.Protocol = MIB_IPPROTO_NETMGMT;
	spec.Origin = NlroManual;

	auto status = CreateIpForwardEntry2(&spec);

	//
	// The return code ERROR_OBJECT_ALREADY_EXISTS means there is already an existing route
	// on the same interface, with the same DestinationPrefix and NextHop.
	//
	// However, all the other properties of the route may be different. And the properties may
	// not have the exact same values as when the route was registered, because windows
	// will adjust route properties at time of route insertion as well as later.
	//
	// The simplest thing in this case is to just overwrite the route.
	//

	if (status == ERROR_OBJECT_ALREADY_EXISTS)
	{
		status = SetIpForwardEntry2(&spec);
	}

	if (NO_ERROR != status)
	{
		THROW_WINDOWS_ERROR(status, "Register route in routing table");
	}

	return RegisteredRoute { route.network(), node.iface, node.gateway };
}

void RouteManager::restoreIntoRoutingTable(const RegisteredRoute &route)
{
	MIB_IPFORWARD_ROW2 spec;

	InitializeIpForwardEntry(&spec);

	spec.InterfaceLuid = route.luid;
	spec.DestinationPrefix = route.network;
	spec.NextHop = route.nextHop;
	spec.Metric = 0;
	spec.Protocol = MIB_IPPROTO_NETMGMT;
	spec.Origin = NlroManual;

	const auto status = CreateIpForwardEntry2(&spec);

	if (NO_ERROR != status)
	{
		THROW_WINDOWS_ERROR(status, "Register route in routing table");
	}
}

void RouteManager::deleteFromRoutingTable(const RegisteredRoute &route)
{
	MIB_IPFORWARD_ROW2 r = { 0};

	r.InterfaceLuid = route.luid;
	r.DestinationPrefix = route.network;
	r.NextHop = route.nextHop;

	auto status = DeleteIpForwardEntry2(&r);

	if (ERROR_NOT_FOUND == status)
	{
		status = NO_ERROR;

		const auto err = std::wstring(L"Attempting to delete route which was not present in routing table, " \
			"ignoring and proceeding. Route: ").append(FormatRegisteredRoute(route));

		m_logSink->warning(common::string::ToAnsi(err).c_str());
	}

	if (NO_ERROR != status)
	{
		THROW_WINDOWS_ERROR(status, "Delete route in routing table");
	}
}

void RouteManager::undoEvents(const std::vector<EventEntry> &eventLog)
{
	//
	// Rewind state by processing events in the reverse order.
	//

	for (auto it = eventLog.rbegin(); it != eventLog.rend(); ++it)
	{
		try
		{
			switch (it->type)
			{
				case EventType::ADD_ROUTE:
				{
					const auto record = findRouteRecord(it->record.registeredRoute);

					if (m_routes.end() == record)
					{
						THROW_ERROR("Internal state inconsistency in route manager");
					}

					deleteFromRoutingTable(record->registeredRoute);
					m_routes.erase(record);

					break;
				}
				case EventType::DELETE_ROUTE:
				{
					restoreIntoRoutingTable(it->record.registeredRoute);
					m_routes.emplace_back(it->record);

					break;
				}
				default:
				{
					THROW_ERROR("Missing case handler in switch clause");
				}
			}
		}
		catch (const std::exception &ex)
		{
			const auto err = std::string("Attempting to rollback state: ").append(ex.what());
			m_logSink->error(err.c_str());
		}
	}
}

// static
std::wstring RouteManager::FormatRegisteredRoute(const RegisteredRoute &route)
{
	using namespace common::string;

	std::wstringstream ss;

	if (AF_INET == route.network.Prefix.si_family)
	{
		std::wstring gateway(L"\"On-link\"");

		if (0 != route.nextHop.Ipv4.sin_addr.s_addr)
		{
			gateway = FormatIpv4<AddressOrder::NetworkByteOrder>(route.nextHop.Ipv4.sin_addr.s_addr);
		}

		ss << FormatIpv4<AddressOrder::NetworkByteOrder>(route.network.Prefix.Ipv4.sin_addr.s_addr, route.network.PrefixLength)
			<< L" with gateway " << gateway
			<< L" on interface with LUID 0x" << std::hex << route.luid.Value;
	}
	else if (AF_INET6 == route.network.Prefix.si_family)
	{
		std::wstring gateway(L"\"On-link\"");

		const uint8_t *begin = &route.nextHop.Ipv6.sin6_addr.u.Byte[0];
		const uint8_t *end = begin + 16;

		if (0 != std::accumulate(begin, end, 0))
		{
			gateway = FormatIpv6(route.nextHop.Ipv6.sin6_addr.u.Byte);
		}

		ss << FormatIpv6(route.network.Prefix.Ipv6.sin6_addr.u.Byte, route.network.PrefixLength)
			<< L" with gateway " << gateway
			<< L" on interface with LUID 0x" << std::hex << route.luid.Value;
	}
	else
	{
		ss << L"Failed to format route details";
	}

	return ss.str();
}

void RouteManager::defaultRouteChanged(ADDRESS_FAMILY family, DefaultRouteMonitor::EventType eventType,
	const std::optional<InterfaceAndGateway> &route)
{
	//
	// Forward event to all registered listeners.
	//

	m_defaultRouteCallbacksLock.lock();

	for (const auto &callback : m_defaultRouteCallbacks)
	{
		try
		{
			callback(eventType, family, route);
		}
		catch (const std::exception &ex)
		{
			const auto msg = std::string("Failure in default-route-changed callback: ").append(ex.what());
			m_logSink->error(msg.c_str());
		}
		catch (...)
		{
			m_logSink->error("Unspecified failure in default-route-changed callback");
		}
	}

	m_defaultRouteCallbacksLock.unlock();

	//
	// Examine event to determine if best default route has changed.
	//

	if (DefaultRouteMonitor::EventType::Updated != eventType)
	{
		return;
	}

	//
	// Examine our routes to see if any of them are policy bound to the best default route.
	//

	AutoLockType routesLock(m_routesLock);

	using RecordIterator = std::list<RouteRecord>::iterator;

	std::list<RecordIterator> affectedRoutes;

	for (RecordIterator it = m_routes.begin(); it != m_routes.end(); ++it)
	{
		if (false == it->route.node().has_value()
			&& family == it->route.network().Prefix.si_family)
		{
			affectedRoutes.emplace_back(it);
		}
	}

	if (affectedRoutes.empty())
	{
		return;
	}

	//
	// Update all affected routes.
	//

	m_logSink->info("Best default route has changed. Refreshing dependent routes");

	for (auto &it : affectedRoutes)
	{
		//
		// We can't update the existing route because defining characteristics are being changed.
		// So removing and adding again is the only option.
		//

		try
		{
			deleteFromRoutingTable(it->registeredRoute);
		}
		catch (const std::exception &ex)
		{
			const auto msg = std::string("Failed to delete route when refreshing " \
				"existing routes: ").append(ex.what());

			m_logSink->error(msg.c_str());

			continue;
		}

		it->registeredRoute.luid = route.value().iface;
		it->registeredRoute.nextHop = route.value().gateway;

		try
		{
			restoreIntoRoutingTable(it->registeredRoute);
		}
		catch (const std::exception &ex)
		{
			const auto msg = std::string("Failed to add route when refreshing " \
				"existing routes: ").append(ex.what());

			m_logSink->error(msg.c_str());

			continue;
		}
	}
}

}
