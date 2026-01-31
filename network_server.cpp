#include "network_server.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <iostream>
#include <string>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace sysmon {

struct NetworkServer::Impl {
	std::uint16_t port{};
	LineProvider provider;
	WSADATA wsa{};
	SOCKET listening{ INVALID_SOCKET };
	std::atomic<bool> stopping{ false };
};

NetworkServer::NetworkServer(std::uint16_t port, LineProvider provider) : _impl(new Impl{}) {
	_impl->port = port;
	_impl->provider = std::move(provider);
}

NetworkServer::~NetworkServer() {
	stop();

	if (!_impl) return;
	WSACleanup();
	delete _impl;
	_impl = nullptr;
}

void NetworkServer::stop() noexcept {
	if (!_impl) return;

	_impl->stopping.store(true, std::memory_order_release);

	if (_impl->listening != INVALID_SOCKET) {
		shutdown(_impl->listening, SD_BOTH);
		closesocket(_impl->listening);
		_impl->listening = INVALID_SOCKET;
	}
}

static bool sendAll(SOCKET s, const char* data, int len) {
	int total = 0;
	while (total < len) {
		int sent = send(s, data + total, len - total, 0);
		if (sent == SOCKET_ERROR || sent == 0) return false;
		total += sent;
	}
	return true;
}

int NetworkServer::run() {
	if (!_impl) return 1;

	int wsaInit = WSAStartup(MAKEWORD(2, 2), &_impl->wsa);
	if (wsaInit != 0) {
		std::cerr << "WSAStartup failed: " << wsaInit << "\n";
		return 1;
	}

	_impl->listening = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (_impl->listening == INVALID_SOCKET) {
		std::cerr << "Cannot create socket: " << WSAGetLastError() << "\n";
		return 1;
	}

	BOOL reuse = TRUE;
	if (setsockopt(_impl->listening, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == SOCKET_ERROR) {
		std::cerr << "setsockopt(SO_REUSEADDR) failed: " << WSAGetLastError() << "\n";
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(_impl->port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(_impl->listening, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		std::cerr << "Cannot bind socket: " << WSAGetLastError() << "\n";
		return 1;
	}

	// Restrict backlog to 1 to discourage multiple pending connections, 
	// though the app logic already handles clients sequentially (one at a time).
	if (listen(_impl->listening, 1) == SOCKET_ERROR) {
		std::cerr << "Cannot listen on socket: " << WSAGetLastError() << "\n";
		return 1;
	}

	std::cout << "Waiting for client on 0.0.0.0:" << _impl->port << "...\n";

	while (!_impl->stopping.load(std::memory_order_acquire)) {
		SOCKET client = accept(_impl->listening, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			if (_impl->stopping.load(std::memory_order_acquire)) break;
			std::cerr << "Error accepting connection: " << WSAGetLastError() << "\n";
			continue;
		}

		std::cout << "Client connected.\n";

		while (!_impl->stopping.load(std::memory_order_acquire)) {
			try {
				std::string line = _impl->provider ? _impl->provider() : std::string{};
				if (line.empty()) {
					line = "\r\n";
				} else {
					if (line.size() < 2 || line.compare(line.size() - 2, 2, "\r\n") != 0) {
						if (!line.empty() && line.back() == '\n') {
							// normalize \n -> \r\n
							if (line.size() < 2 || line[line.size() - 2] != '\r') {
								line.insert(line.end() - 1, '\r');
							}
						} else {
							line += "\r\n";
						}
					}
				}

				if (!sendAll(client, line.c_str(), static_cast<int>(line.size()))) {
					int err = WSAGetLastError();
					// Normal behavior when client disconnects
					if (err != 0) std::cerr << "Client send failed: " << err << "\n";
					break;
				}
			} catch (const std::exception& e) {
				std::cerr << "Exception in provider: " << e.what() << "\n";
				break;
			} catch (...) {
				std::cerr << "Unknown exception in provider.\n";
				break;
			}
			Sleep(1000);
		}

		shutdown(client, SD_BOTH);
		closesocket(client);
		std::cout << "Client disconnected.\n";
	}

	return 0;
}

} // namespace sysmon
