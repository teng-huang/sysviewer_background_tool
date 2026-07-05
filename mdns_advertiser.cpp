#include "mdns_advertiser.h"

#include <windows.h>
#include <windns.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace sysmon {

namespace {

static constexpr wchar_t kPreferredAdvertisedIp[] = L"192.168.0.146";
static constexpr wchar_t kServiceType[] = L"_sysviewer._tcp.local";
static constexpr wchar_t kInstanceLabel[] = L"SysViewer-192\\.168\\.0\\.146";
static constexpr wchar_t kHostName[] = L"SysViewer-192-168-0-146.local";

struct Endpoint {
	IP4_ADDRESS ip4{};
	ULONG interfaceIndex{};
	bool hasIp4{};
	int score{};
};

struct Registration {
	PDNS_SERVICE_INSTANCE instance{};
	DNS_SERVICE_REGISTER_REQUEST request{};
	bool active{};
};

static void WINAPI dnsServiceCallback(DWORD, PVOID, PDNS_SERVICE_INSTANCE instance) {
	if (instance) DnsServiceFreeInstance(instance);
}

static bool isUsableIPv4(ULONG networkOrderAddress) {
	const ULONG hostOrderAddress = ntohl(networkOrderAddress);
	const ULONG firstOctet = (hostOrderAddress >> 24) & 0xff;
	if (hostOrderAddress == 0 || firstOctet == 127) return false;
	return true;
}

static int adapterScore(const IP_ADAPTER_ADDRESSES* adapter, ULONG networkOrderAddress) {
	int score = 0;
	if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211) score += 100;
	if (adapter->FirstGatewayAddress) score += 50;

	const ULONG hostOrderAddress = ntohl(networkOrderAddress);
	if ((hostOrderAddress & 0xffff0000UL) != 0xa9fe0000UL) score += 20;
	return score;
}

static bool preferredIPv4(ULONG& outNetworkOrderAddress) {
	IN_ADDR addr{};
	if (InetPtonW(AF_INET, kPreferredAdvertisedIp, &addr) != 1) return false;
	outNetworkOrderAddress = addr.S_un.S_addr;
	return true;
}

static bool chooseEndpoint(Endpoint& out) {
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG size = 15 * 1024;
	std::vector<unsigned char> buffer(size);

	ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr,
		reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.assign(size, 0);
		result = GetAdaptersAddresses(AF_INET, flags, nullptr,
			reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
	}
	if (result != NO_ERROR) return false;

	ULONG preferredAddress{};
	const bool hasPreferredAddress = preferredIPv4(preferredAddress);
	bool found = false;
	for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
		adapter;
		adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp) continue;
		if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) continue;

		for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
			if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
			auto* sin = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
			const ULONG address = sin->sin_addr.S_un.S_addr;
			if (!isUsableIPv4(address)) continue;

			if (hasPreferredAddress && address == preferredAddress) {
				out.ip4 = address;
				out.interfaceIndex = adapter->IfIndex;
				out.hasIp4 = true;
				out.score = adapterScore(adapter, address);
				return true;
			}

			const int score = adapterScore(adapter, address);
			if (!found || score > out.score) {
				out.ip4 = address;
				out.interfaceIndex = adapter->IfIndex;
				out.hasIp4 = true;
				out.score = score;
				found = true;
			}
		}
	}

	return found;
}

static std::wstring makeInstanceName() {
	return std::wstring(kInstanceLabel) + L"." + kServiceType;
}

} // namespace

struct MdnsAdvertiser::Impl {
	std::uint16_t port{};
	std::wstring hostName;
	std::wstring instanceName;
	std::unique_ptr<Registration> registration;
};

MdnsAdvertiser::MdnsAdvertiser(std::uint16_t port) : _impl(new Impl{}) {
	_impl->port = port;
	_impl->hostName = kHostName;
	_impl->instanceName = makeInstanceName();
}

MdnsAdvertiser::~MdnsAdvertiser() {
	stop();
	delete _impl;
	_impl = nullptr;
}

bool MdnsAdvertiser::start() {
	if (!_impl) return false;
	if (_impl->registration && _impl->registration->active) return true;

	Endpoint endpoint{};
	chooseEndpoint(endpoint);

	PCWSTR keys[] = { L"app", L"version" };
	PCWSTR values[] = { L"SysViewer", L"1" };

	std::unique_ptr<Registration> reg(new Registration{});
	reg->instance = DnsServiceConstructInstance(
		_impl->instanceName.c_str(),
		_impl->hostName.c_str(),
		endpoint.hasIp4 ? &endpoint.ip4 : nullptr,
		nullptr,
		_impl->port,
		0,
		0,
		static_cast<DWORD>(_countof(keys)),
		keys,
		values);
	if (!reg->instance) return false;

	reg->instance->dwInterfaceIndex = endpoint.interfaceIndex;
	reg->request.Version = DNS_QUERY_REQUEST_VERSION1;
	reg->request.InterfaceIndex = endpoint.interfaceIndex;
	reg->request.pServiceInstance = reg->instance;
	reg->request.pRegisterCompletionCallback = dnsServiceCallback;
	reg->request.pQueryContext = nullptr;
	reg->request.unicastEnabled = FALSE;

	const DWORD status = DnsServiceRegister(&reg->request, nullptr);
	if (status != DNS_REQUEST_PENDING) {
		DnsServiceFreeInstance(reg->instance);
		reg->instance = nullptr;
		return false;
	}

	reg->active = true;
	_impl->registration = std::move(reg);
	return true;
}

void MdnsAdvertiser::stop() noexcept {
	if (!_impl || !_impl->registration) return;

	Registration* reg = _impl->registration.get();
	if (reg->active && reg->instance) {
		DNS_SERVICE_REGISTER_REQUEST request = reg->request;
		request.pRegisterCompletionCallback = dnsServiceCallback;
		request.pQueryContext = nullptr;
		DnsServiceDeRegister(&request, nullptr);
		reg->active = false;
	}

	if (reg->instance) {
		DnsServiceFreeInstance(reg->instance);
		reg->instance = nullptr;
	}
	_impl->registration.reset();
}

} // namespace sysmon
