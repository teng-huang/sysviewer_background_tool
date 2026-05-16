#include "network_server.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace sysmon {

struct NetworkServer::Impl {
	std::uint16_t port{};
	bool allowRemoteClients{};
	LineProvider provider;
	ListeningCallback onListening;
	ClientCallback onClientDisconnected;
	WSADATA wsa{};
	SOCKET listening{ INVALID_SOCKET };
	SOCKET activeClient{ INVALID_SOCKET };
	std::mutex socketMutex;
	std::atomic<bool> stopping{ false };
};

NetworkServer::NetworkServer(std::uint16_t port, bool allowRemoteClients, LineProvider provider, ListeningCallback onListening, ClientCallback onClientDisconnected) : _impl(new Impl{}) {
	_impl->port = port;
	_impl->allowRemoteClients = allowRemoteClients;
	_impl->provider = std::move(provider);
	_impl->onListening = std::move(onListening);
	_impl->onClientDisconnected = std::move(onClientDisconnected);
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

	std::lock_guard<std::mutex> g(_impl->socketMutex);
	if (_impl->listening != INVALID_SOCKET) {
		shutdown(_impl->listening, SD_BOTH);
		closesocket(_impl->listening);
		_impl->listening = INVALID_SOCKET;
	}
	if (_impl->activeClient != INVALID_SOCKET) {
		shutdown(_impl->activeClient, SD_BOTH);
		closesocket(_impl->activeClient);
		_impl->activeClient = INVALID_SOCKET;
	}
}

static void closeSocketNoThrow(SOCKET& s) noexcept {
	if (s == INVALID_SOCKET) return;
	shutdown(s, SD_BOTH);
	closesocket(s);
	s = INVALID_SOCKET;
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

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET) {
		std::cerr << "Cannot create socket: " << WSAGetLastError() << "\n";
		return 1;
	}

	{
		std::lock_guard<std::mutex> g(_impl->socketMutex);
		if (_impl->stopping.load(std::memory_order_acquire)) {
			closeSocketNoThrow(listenSock);
			return 0;
		}
		_impl->listening = listenSock;
	}

	auto closeListeningSocket = [&]() noexcept {
		std::lock_guard<std::mutex> g(_impl->socketMutex);
		if (_impl->listening == listenSock) {
			closeSocketNoThrow(_impl->listening);
		}
	};

	auto closeActiveClient = [&](SOCKET client) noexcept {
		std::lock_guard<std::mutex> g(_impl->socketMutex);
		if (_impl->activeClient == client) {
			closeSocketNoThrow(_impl->activeClient);
		}
	};

	BOOL reuse = TRUE;
	if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == SOCKET_ERROR) {
		std::cerr << "setsockopt(SO_REUSEADDR) failed: " << WSAGetLastError() << "\n";
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(_impl->port);
	addr.sin_addr.s_addr = htonl(_impl->allowRemoteClients ? INADDR_ANY : INADDR_LOOPBACK);

	if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		if (!_impl->stopping.load(std::memory_order_acquire)) {
			std::cerr << "Cannot bind socket: " << WSAGetLastError() << "\n";
		}
		closeListeningSocket();
		return _impl->stopping.load(std::memory_order_acquire) ? 0 : 1;
	}

	// Restrict backlog to 1 to discourage multiple pending connections, 
	// though the app logic already handles clients sequentially (one at a time).
	if (listen(listenSock, 1) == SOCKET_ERROR) {
		if (!_impl->stopping.load(std::memory_order_acquire)) {
			std::cerr << "Cannot listen on socket: " << WSAGetLastError() << "\n";
		}
		closeListeningSocket();
		return _impl->stopping.load(std::memory_order_acquire) ? 0 : 1;
	}

	std::cout << "Waiting for client on "
		<< (_impl->allowRemoteClients ? "0.0.0.0" : "127.0.0.1")
		<< ":" << _impl->port << "...\n";
	try {
		if (_impl->onListening) _impl->onListening();
	} catch (...) {
	}

	while (!_impl->stopping.load(std::memory_order_acquire)) {
		SOCKET client = accept(listenSock, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			if (_impl->stopping.load(std::memory_order_acquire)) break;
			std::cerr << "Error accepting connection: " << WSAGetLastError() << "\n";
			continue;
		}

		{
			std::lock_guard<std::mutex> g(_impl->socketMutex);
			if (_impl->stopping.load(std::memory_order_acquire)) {
				closeSocketNoThrow(client);
				break;
			}
			_impl->activeClient = client;
		}

		DWORD sendTimeoutMs = 1000;
		if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs)) == SOCKET_ERROR) {
			std::cerr << "setsockopt(SO_SNDTIMEO) failed: " << WSAGetLastError() << "\n";
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

		closeActiveClient(client);
		try {
			if (_impl->onClientDisconnected) _impl->onClientDisconnected();
		} catch (...) {
		}
		std::cout << "Client disconnected.\n";
	}

	closeListeningSocket();
	return 0;
}

} // namespace sysmon
