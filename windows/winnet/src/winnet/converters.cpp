#include <stdafx.h>
#include "converters.h"
#include <libcommon/error.h>
#include <cstdint>

using namespace winnet::routing;

namespace
{

void IpToNative(const WINNET_IP &from, SOCKADDR_INET &to)
{
	memset(&to, 0, sizeof(to));

	switch (from.family)
	{
		case WINNET_ADDR_FAMILY_IPV4:
		{
			to.Ipv4.sin_family = AF_INET;
			to.Ipv4.sin_addr.s_addr = *reinterpret_cast<const uint32_t*>(from.bytes);

			break;
		}
		case WINNET_ADDR_FAMILY_IPV6:
		{
			to.Ipv6.sin6_family = AF_INET6;
			memcpy(to.Ipv6.sin6_addr.u.Byte, from.bytes, 16);

			break;
		}
		default:
		{
			THROW_ERROR("Invalid network address family");
		}
	}
}

} // anonymous namespace

namespace winnet
{

Network ConvertNetwork(const WINNET_IP_NETWORK &in)
{
	//
	// Convert WINNET_IPNETWORK into Network aka IP_ADDRESS_PREFIX
	//

	Network out = { 0 };

	out.PrefixLength = in.prefix;

	IpToNative(in.addr, out.Prefix);

	return out;
}

std::optional<Node> ConvertNode(const WINNET_NODE *in)
{
	if (nullptr == in)
	{
		return std::nullopt;
	}

	if (nullptr == in->deviceName && nullptr == in->gateway)
	{
		THROW_ERROR("Invalid 'WINNET_NODE' definition");
	}

	std::optional<std::wstring> deviceName;
	std::optional<NodeAddress> gateway;

	if (nullptr != in->deviceName)
	{
		deviceName = in->deviceName;
	}

	if (nullptr != in->gateway)
	{
		NodeAddress gw = { 0 };

		IpToNative(*in->gateway, gw);

		gateway = std::move(gw);
	}

	return Node(deviceName, gateway);
}

std::vector<Route> ConvertRoutes(const WINNET_ROUTE *routes, uint32_t numRoutes)
{
	std::vector<Route> out;

	out.reserve(numRoutes);

	for (size_t i = 0; i < numRoutes; ++i)
	{
		out.emplace_back(Route
		{
			ConvertNetwork(routes[i].network),
			ConvertNode(routes[i].node)
		});
	}

	return out;
}

std::vector<SOCKADDR_INET> ConvertAddresses(const WINNET_IP *addresses, uint32_t numAddresses)
{
	std::vector<SOCKADDR_INET> out;
	out.reserve(numAddresses);

	for (uint32_t i = 0; i < numAddresses; ++i)
	{
		SOCKADDR_INET to = { 0 };

		IpToNative(addresses[i], to);

		out.emplace_back(std::move(to));
	}

	return out;
}

}
