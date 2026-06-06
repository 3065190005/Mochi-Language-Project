#include "MochiInclude.h"
#include "MochiRuntime.h"

#ifndef MOCHIACTOR
#define MOCHIACTOR

// ============================================================
// Actor多线程
// ============================================================

namespace Mochi
{
// ActorFuture返回值
    struct MochiFuture {
        std::mutex mtx;
        std::condition_variable cv;
        Value result;
        bool ready = false;

        void setValue(Value v);

        Value get();
    };

    struct AskContext {
        std::shared_ptr<MochiFuture> future;
        Value payload;  // 原始消息内容
    };

    // 单个线程独一份
    extern thread_local std::shared_ptr<AskContext> currentAsk;

    // actor 多线程
    struct MochiActor {
        std::function<void(Value)> processMessage;
        std::queue<Value> mailbox;
        std::mutex mtx;
        std::condition_variable cv;
        std::thread worker;
        bool running = true;
        bool closing = false;   // 优雅关闭标志

        explicit MochiActor(std::function<void(Value)> handler);

        ~MochiActor();

        void send(Value msg);

        void stop();

        void close();

        void join();

        std::shared_ptr<MochiFuture> ask(Value payload);
    };
}

#endif