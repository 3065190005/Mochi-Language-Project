// socket_wrapper.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

using SocketHandle = intptr_t;

namespace socket_wrap {
    // 初始化套接字库（Windows 需要，Linux 可省略）
    void init();

    // 创建套接字，type: 1=SOCK_STREAM (TCP), 2=SOCK_DGRAM (UDP)
    SocketHandle create(int type);

    // 绑定地址和端口
    void bind(SocketHandle sock, const std::string& addr, int port);

    // 监听（仅 TCP）
    void listen(SocketHandle sock, int backlog);

    // 接受连接，返回新的 SocketHandle
    SocketHandle accept(SocketHandle sock);

    // 连接到远端
    void connect(SocketHandle sock, const std::string& addr, int port);

    // 发送数据，返回发送字节数，失败返回 -1
    int send(SocketHandle sock, const std::string& data);

    // 接收数据，返回读取的字符串，连接关闭返回空字符串，错误抛出异常
    std::string recv(SocketHandle sock, int maxSize);

    // 关闭套接字
    void close(SocketHandle sock);

    // select 轮询，返回可读的 SocketHandle 列表
    // timeoutSec: 超时秒数，-1 表示永久等待
    std::vector<SocketHandle> selectRead(const std::vector<SocketHandle>& sockets, double timeoutSec);
}