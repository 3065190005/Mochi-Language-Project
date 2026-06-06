// socket_wrapper.cpp
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#define closesocket close
#endif

#include "socket_wrapper.h"
#include <stdexcept>
#include <cstring>

namespace socket_wrap {

    static bool initialized = false;

    void init() {
        if (initialized) return;
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
        initialized = true;
    }

    SocketHandle create(int type) {
        init();
        int sockType = (type == 1) ? SOCK_STREAM : SOCK_DGRAM;
        SocketHandle s = socket(AF_INET, sockType, 0);
        if (s == INVALID_SOCKET) throw std::runtime_error("socket() failed");
        return s;
    }

    void bind(SocketHandle sock, const std::string& addr, int port) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        if (addr.empty() || addr == "0.0.0.0") {
            sa.sin_addr.s_addr = INADDR_ANY;
        }
        else {
            inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);
        }
        if (::bind((SOCKET)sock, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
            throw std::runtime_error("bind() failed");
    }

    void listen(SocketHandle sock, int backlog) {
        if (::listen((SOCKET)sock, backlog) == SOCKET_ERROR)
            throw std::runtime_error("listen() failed");
    }

    SocketHandle accept(SocketHandle sock) {
        struct sockaddr_in sa;
        socklen_t len = sizeof(sa);
        SocketHandle client = ::accept((SOCKET)sock, (struct sockaddr*)&sa, &len);
        if (client == INVALID_SOCKET) throw std::runtime_error("accept() failed");
        return client;
    }

    void connect(SocketHandle sock, const std::string& addr, int port) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);
        if (::connect((SOCKET)sock, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
            throw std::runtime_error("connect() failed");
    }

    int send(SocketHandle sock, const std::string& data) {
        int ret = ::send((SOCKET)sock, data.data(), (int)data.size(), 0);
        if (ret == SOCKET_ERROR) throw std::runtime_error("send() failed");
        return ret;
    }

    std::string recv(SocketHandle sock, int maxSize) {
        std::string buffer(maxSize, '\0');
        int ret = ::recv((SOCKET)sock, &buffer[0], maxSize, 0);
        if (ret == SOCKET_ERROR) throw std::runtime_error("recv() failed");
        if (ret == 0) return ""; // 连接关闭
        buffer.resize(ret);
        return buffer;
    }

    void close(SocketHandle sock) {
        ::closesocket((SOCKET)sock);
    }

    std::vector<SocketHandle> selectRead(const std::vector<SocketHandle>& sockets, double timeoutSec) {
        fd_set readfds;
        FD_ZERO(&readfds);
        SocketHandle maxfd = 0;
        for (auto s : sockets) {
            FD_SET((SOCKET)s, &readfds);
            if (s > maxfd) maxfd = s;
        }
        struct timeval tv;
        struct timeval* ptv = nullptr;
        if (timeoutSec >= 0) {
            tv.tv_sec = (long)timeoutSec;
            tv.tv_usec = (long)((timeoutSec - tv.tv_sec) * 1000000);
            ptv = &tv;
        }
        int ret = select((int)maxfd + 1, &readfds, nullptr, nullptr, ptv);
        if (ret == SOCKET_ERROR) throw std::runtime_error("select() failed");
        std::vector<SocketHandle> result;
        if (ret > 0) {
            for (auto s : sockets) {
                if (FD_ISSET((SOCKET)s, &readfds))
                    result.push_back(s);
            }
        }
        return result;
    }

} // namespace socket_wrap