#include "ThreadPool.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <string>
#include <cassert>
#include <thread>
#include <atomic>

// 测试工具函数：打印测试分隔线
void printTestSeparator(const std::string& testName) {
    std::cout << "\n==================================== " << testName << " ====================================" << std::endl;
}

// 测试1：基础功能测试（FIXED模式+无返回值/有返回值任务）
void testBasicFunction() {
    printTestSeparator("测试1：基础功能测试");
    TaskQueue queue(5);
    ThreadPool pool("Basic-Pool", &queue, PoolMode::FIXED, 2); // 2线程FIXED模式

    // 测试无返回值任务
    std::cout << "\n【无返回值任务】" << std::endl;
    auto f1 = pool.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "无返回值任务执行完成" << std::endl;
        });
    f1.get(); // 等待任务完成

    // 测试有返回值任务（int）
    std::cout << "\n【有返回值任务（int）】" << std::endl;
    auto f2 = pool.submitTask([](int a, int b) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return a + b;
        }, 10, 20);
    int sum = std::any_cast<int>(f2.get());
    assert(sum == 30);
    std::cout << "有返回值任务结果：10+20=" << sum << "（断言通过）" << std::endl;

    // 测试有返回值任务（string）
    std::cout << "\n【有返回值任务（string）】" << std::endl;
    auto f3 = pool.submitTask([](const std::string& s1, const std::string& s2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return s1 + s2;
        }, "Hello ", "ThreadPool!");
    std::string str = std::any_cast<std::string>(f3.get());
    assert(str == "Hello ThreadPool!");
    std::cout << "有返回值任务结果：" << str << "（断言通过）" << std::endl;

    pool.shutdownPool();
    std::cout << "\n测试1通过！" << std::endl;
}

// 测试2：多线程池共享任务队列测试
void testMultiPoolShareQueue() {
    printTestSeparator("测试2：多线程池共享任务队列测试");
    TaskQueue sharedQueue(10); // 共享队列（容量10）

    // 创建3个线程池共享队列
    ThreadPool pool1("Share-Pool1", &sharedQueue, PoolMode::FIXED, 2); // 2线程
    ThreadPool pool2("Share-Pool2", &sharedQueue, PoolMode::FIXED, 3); // 3线程
    ThreadPool pool3("Share-Pool3", &sharedQueue, PoolMode::CACHED, 1, 4); // 1-4线程

    std::vector<std::future<std::any>> futures;
    std::atomic_int finishCount = 0; // 统计任务完成数

    // 提交10个任务到共享队列（通过队列直接提交）
    std::cout << "\n提交10个任务到共享队列" << std::endl;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(sharedQueue.submitTask([i, &finishCount](const std::string& queueName) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::cout << "共享队列任务" << i << "执行完成（队列：" << queueName << "）" << std::endl;
            finishCount++;
            }, "SharedQueue"));
    }

    // 等待所有任务完成
    for (auto& f : futures) {
        f.get();
    }

    // 验证所有任务都完成（10个）
    assert(finishCount == 10);
    std::cout << "\n共享队列任务全部完成（共" << finishCount << "个，断言通过）" << std::endl;

    // 验证队列空
    assert(sharedQueue.empty());
    std::cout << "共享队列任务执行后为空（断言通过）" << std::endl;

    // 关闭所有线程池
    pool1.shutdownPool();
    pool2.shutdownPool();
    pool3.shutdownPool();
    std::cout << "\n测试2通过！" << std::endl;
}

// 测试3：CACHED模式动态扩缩容测试
void testCachedModeScale() {
    printTestSeparator("测试3：CACHED模式动态扩缩容测试");
    TaskQueue queue(20);
    ThreadPool pool("Cached-Pool", &queue, PoolMode::CACHED, 1, 5); // 核心1，最大5线程
    pool.setEverytimeAddCount(1); // 每次扩容1个线程

    std::vector<std::future<std::any>> futures;
    // 提交8个耗时任务（任务积压，触发扩容）
    std::cout << "\n提交8个耗时任务（触发扩容）" << std::endl;
    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submitTask([i]() {
            std::this_thread::sleep_for(std::chrono::seconds(2)); // 每个任务耗时2秒
            std::cout << "CACHED模式任务" << i << "执行完成" << std::endl;
            }));
    }

    // 等待扩容（管理者线程每3秒检查一次，等待4秒确保扩容完成）
    std::this_thread::sleep_for(std::chrono::seconds(4));
    int maxLive = pool.getLiveNum();
    assert(maxLive <= 5 && maxLive > 1); // 扩容后线程数≤5且>1
    std::cout << "CACHED模式扩容后最大存活线程数：" << maxLive << "（断言通过）" << std::endl;

    // 等待所有任务完成（任务完成后线程空闲，触发缩容）
    for (auto& f : futures) {
        f.get();
    }

    // 等待缩容（管理者线程3秒检查一次，等待6秒确保缩容完成）
    std::this_thread::sleep_for(std::chrono::seconds(6));
    int finalLive = pool.getLiveNum();
    assert(finalLive == 1); // 缩容到核心线程数1
    std::cout << "CACHED模式缩容后存活线程数：" << finalLive << "（断言通过）" << std::endl;

    pool.shutdownPool();
    std::cout << "\n测试3通过！" << std::endl;
}

// 测试4：队列容量限制与任务拒绝测试
void testQueueCapacityLimit() {
    printTestSeparator("测试4：队列容量限制与任务拒绝测试");
    int maxCapacity  = 5 ;
    TaskQueue queue(maxCapacity); 
    ThreadPool pool("Limit-Pool", &queue, PoolMode::FIXED, 1); // 1线程（执行慢，确保任务积压）

    std::vector<std::future<std::any>> futures;
    int totalSubmit = 10; 
    int rejectCount = 0;

    // 第一步：批量提交所有5个任务（不等待，让队列积压）
    std::cout << "\n批量提交" << totalSubmit << "个任务（队列容量3）" << std::endl;
    for (int i = 0; i < totalSubmit; ++i) {
        auto f = pool.submitTask([i]() {
            std::this_thread::sleep_for(std::chrono::seconds(2)); // 任务执行慢（2秒），确保队列积压
            std::cout << "限制队列任务" << i+1 << "执行完成" << std::endl;
            });
        futures.push_back(std::move(f)); // 保存future，后续统一等待
    }

    // 第二步：统一等待所有任务结果，统计被拒绝的任务数
    std::cout << "\n开始等待所有任务结果，统计拒绝数..." << std::endl;
    for (int i = 0; i < totalSubmit; ++i) {
        try {
            futures[i].get(); // 尝试获取结果，被拒绝则抛出异常
            std::cout << "任务" << i+1 << "执行成功" << std::endl;
        }
        catch (...) {
            rejectCount++;
            std::cout << "任务" << i+1 << "被拒绝（队列满）" << std::endl;
        }
    }
    //实际上会执行maxCapacity个任务 因为在第一个任务入队的时候就立刻被拿走，剩下maxCapacity被挤压！！！
    // 验证被拒绝的任务数为1
    assert(rejectCount == (totalSubmit-maxCapacity-1));
    std::cout << "\n被拒绝的任务数：" << rejectCount << "（断言通过）" << std::endl;
    std::cout << "队列最大容量：" << queue.getMaxCapacity() << "，实际积压最大任务数："<< (totalSubmit - maxCapacity - 1) << std::endl;
    std::cout << "在此任务中我设置了队列上限为maxCapacity，但是我瞬间添加了totalSubmit个任务。\n"<<
        "第一个任务会被添加的一瞬间被拿走，剩下的才会积压maxCapacity个，\n所以在这里被拒绝的任务数应该为(totalSubmit - maxCapacity - 1)" 
        << std::endl;

    std::cout << "\n测试4通过！" << std::endl;
}

// 测试5：异常传递测试（任务执行抛出异常）
void testExceptionTransfer() {
    printTestSeparator("测试5：异常传递测试");
    TaskQueue queue(5);
    ThreadPool pool("Exception-Pool", &queue, PoolMode::FIXED, 2);

    // 测试任务抛出std::runtime_error
    std::cout << "\n【测试std::runtime_error传递】" << std::endl;
    auto f1 = pool.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        throw std::runtime_error("任务执行失败：参数非法");
        });
    try {
        f1.get();
    }
    catch (const std::runtime_error& e) {
        std::cout << "捕获到任务异常：" << e.what() << "（符合预期）" << std::endl;
    }

    // 测试任务抛出自定义异常
    std::cout << "\n【测试自定义异常传递】" << std::endl;
    struct CustomException : public std::exception {
        const char* what() const noexcept override {
            return "自定义异常：任务超时";
        }
    };
    auto f2 = pool.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        throw CustomException();
        });
    try {
        f2.get();
    }
    catch (const CustomException& e) {
        std::cout << "捕获到自定义异常：" << e.what() << "（符合预期）" << std::endl;
    }

    pool.shutdownPool();
    std::cout << "\n测试5通过！" << std::endl;
}

// 测试6：线程池销毁/关机自动解绑测试
void testPoolDestructUnbind() {
    printTestSeparator("测试6：线程池销毁/关机自动解绑测试");
    TaskQueue sharedQueue(5);

    // 测试1：线程池超出作用域销毁（自动解绑）
    std::cout << "\n【测试线程池超出作用域销毁】" << std::endl;
    {
        ThreadPool pool1("Destruct-Pool1", &sharedQueue, PoolMode::FIXED, 2);
        auto f1 = pool1.submitTask([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::cout << "Destruct-Pool1 任务执行完成" << std::endl;
            });
        f1.get();
    } // pool1超出作用域，析构自动解绑

    // 测试2：线程池手动shutdown（自动解绑）
    std::cout << "\n【测试线程池手动shutdown】" << std::endl;
    ThreadPool pool2("Destruct-Pool2", &sharedQueue, PoolMode::FIXED, 2);
    auto f2 = pool2.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "Destruct-Pool2 任务执行完成" << std::endl;
        });
    f2.get();
    pool2.shutdownPool(); // 手动shutdown

    // 验证：新线程池绑定队列后正常工作
    std::cout << "\n【验证新线程池正常工作】" << std::endl;
    ThreadPool pool3("Destruct-Pool3", &sharedQueue, PoolMode::FIXED, 1);
    auto f3 = pool3.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "新线程池任务执行完成" << std::endl;
        });
    f3.get();
    pool3.shutdownPool();


    //测试3：pool2重启后能否接着完成任务
    std::cout << "\n【验证pool2重启后线程池正常工作】" << std::endl;
    pool2.resumePool();
    f2 = pool2.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "Destruct-Pool2 任务执行完成" << std::endl;
        });
    f2.get();

    std::cout << "\n测试6通过！" << std::endl;
}

// 测试7：队列关闭测试（拒绝新任务+处理剩余任务）
void testQueueShutdown() {
    printTestSeparator("测试7：队列关闭测试");
    TaskQueue queue(5);
    ThreadPool pool("Queue-Shutdown-Pool", &queue, PoolMode::FIXED, 2);

    // 提交2个任务（未关闭队列，正常执行）
    std::cout << "\n提交2个任务（队列未关闭）" << std::endl;
    auto f1 = pool.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "队列关闭前任务1执行完成" << std::endl;
        });
    auto f2 = pool.submitTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "队列关闭前任务2执行完成" << std::endl;
        });

    // 关闭队列（拒绝新任务，处理剩余任务）
    std::cout << "\n关闭队列" << std::endl;
    queue.shutdownQueue();

    // 尝试提交新任务（被拒绝）
    std::cout << "\n尝试提交队列关闭后的任务（预期被拒绝）" << std::endl;
    auto f3 = pool.submitTask([]() {
        std::cout << "队列关闭后任务（不应执行）" << std::endl;
        });
    try {
        f3.get();
    }
    catch (...) {
        
        std::cout << "捕获到队列关闭异常：" << "（断言通过）" << std::endl;
    }

    // 等待剩余任务完成
    f1.get();
    f2.get();

    // 验证队列空
    assert(queue.empty());
    std::cout << "队列关闭后剩余任务全部执行完成（断言通过）" << std::endl;

    std::cout << "\n测试7通过！" << std::endl;
}

// 测试8：高并发压力测试（100个任务+多线程池）
void testHighConcurrency() {
    printTestSeparator("测试8：高并发压力测试");
    TaskQueue queue(100); // 队列容量50

    // 创建2个线程池（共7个线程）
    ThreadPool pool1("Concurrency-Pool1", &queue, PoolMode::FIXED, 3);
    ThreadPool pool2("Concurrency-Pool2", &queue, PoolMode::CACHED, 4, 4);

    std::vector<std::future<std::any>> futures;
    std::atomic_int successCount = 0;
    const int TASK_COUNT = 100; // 100个并发任务

    // 提交100个任务（每个任务耗时100ms）
    std::cout << "\n提交" << TASK_COUNT << "个高并发任务" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < TASK_COUNT; ++i) {
        futures.push_back(pool1.submitTask([i, &successCount]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            successCount++;
            if (i % 10 == 0) {
                std::cout << "高并发任务" << i << "执行完成" << std::endl;
            }
            }));
    }

    // 等待所有任务完成
    for (auto& f : futures) {
        f.get();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    //验证所有任务都成功执行
    assert(successCount == TASK_COUNT);
    std::cout << "\n高并发任务全部完成（共" << successCount << "个，断言通过）" << std::endl;
    std::cout << "总耗时：" << duration << "ms（预期<2000ms，体现并发优势）" << std::endl;
    std::cout << "\n测试8通过！" << std::endl;
    ////// 关闭线程池
    //pool1.shutdownPool();
    //pool2.shutdownPool();
}
#if 1
int main() {
    // 执行所有测试
    std::cout << "每一项测试都需要手动按下回车键后继续" << std::endl;
    std::getchar();
    testBasicFunction();
    std::cout << "按下任意键后继续下一项测试" << std::endl;
    std::getchar();
    testMultiPoolShareQueue();
    std::cout << "按下任意键后继续下一项测试" << std::endl;
    std::getchar();
    testCachedModeScale();
    std::cout << "按下任意键后继续下一项测试" << std::endl;
    std::getchar();
    testQueueCapacityLimit();
    std::cout << "按下任意键后继续下一项测试" << std::endl;
    std::getchar();
    testExceptionTransfer();
    std::cout << "按下任意键后继续下一项测试" << std::endl;
    std::getchar();
    testPoolDestructUnbind();
    std::cout << "按下任意键后继续下一项测试" << std::endl;
    std::getchar();
    testQueueShutdown();
    std::cout << "按下任意键后继续下一项测试" << std::endl;
    std::getchar();
    testHighConcurrency();
    std::cout << "\n==================================== 所有测试全部通过！====================================" << std::endl;
    return 0;
}
#endif