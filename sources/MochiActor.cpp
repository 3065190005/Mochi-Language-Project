#include "MochiActor.h"
#include "MochiRuntime.h"

namespace Mochi
{
    thread_local std::shared_ptr<AskContext> currentAsk;

    // ActorFuture返回值
        void MochiFuture::setValue(Value v) {
            std::lock_guard<std::mutex> lock(mtx);
            result = std::move(v);
            ready = true;
            cv.notify_all();
        }

        Value MochiFuture::get() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] { return ready; });
            Value ret = std::move(result);   // 移动语义，转移所有权
            result = Value{};                // 清空，释放 Future 持有的引用
            return ret;
        }

    // actor 多线程

        MochiActor::MochiActor(std::function<void(Value)> handler)
            : processMessage(std::move(handler)) {
            worker = std::thread([this]() {
                while (running) {
                    Value msg;
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv.wait(lock, [this] {
                            return !mailbox.empty() || !running || closing;
                            });
                        if (!running) break;
                        if (closing && mailbox.empty()) break;
                        msg = std::move(mailbox.front());
                        mailbox.pop();
                    }

                    try {
                        // 检查是否为 Ask 消息
                        if (msg.type() == typeid(std::shared_ptr<AskContext>)) {
                            auto askCtx = std::any_cast<std::shared_ptr<AskContext>>(msg);
                            // 设置线程局部存储，供 reply() 使用
                            currentAsk = askCtx;
                            processMessage(askCtx->payload);
                            currentAsk = nullptr;
                            // 如果用户没有调用 reply，自动回复 nil
                            if (!askCtx->future->ready) {
                                askCtx->future->setValue(Value{});
                            }
                        }
                        else {
                            processMessage(msg);
                        }
                    }
                    catch (const std::exception& e) {
                        std::cout << "Actor error: " << e.what() << std::endl;
                        // 发生异常时，如果存在 Ask 且未回复，回复 nil
                        if (msg.type() == typeid(std::shared_ptr<AskContext>)) {
                            auto askCtx = std::any_cast<std::shared_ptr<AskContext>>(msg);
                            if (!askCtx->future->ready) {
                                askCtx->future->setValue(Value{});
                            }
                        }
                        currentAsk = nullptr;
                    }
                }
                });
        }

        MochiActor::~MochiActor() {
            stop();   // 析构时强制停止（防止意外长时间阻塞）
            join();
        }

        void MochiActor::send(Value msg) {
            std::lock_guard<std::mutex> lock(mtx);
            mailbox.push(std::move(msg));
            cv.notify_one();
        }

        void MochiActor::stop() {
            running = false;
            cv.notify_all();
        }

        void MochiActor::close() {
            closing = true;      // 标记优雅关闭
            cv.notify_all();     // 唤醒线程检查队列
        }

        void MochiActor::join() {
            if (worker.joinable()) {
                worker.join();
            }
        }

        std::shared_ptr<MochiFuture> MochiActor::ask(Value payload) {
            auto future = std::make_shared<MochiFuture>();
            auto ctx = std::make_shared<AskContext>();
            ctx->future = future;
            ctx->payload = std::move(payload);
            {
                std::lock_guard<std::mutex> lock(mtx);
                mailbox.push(Value(ctx));
            }
            cv.notify_one();
            return future;
        }
}