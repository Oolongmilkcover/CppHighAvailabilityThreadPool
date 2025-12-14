#ifndef H_THREADPOOL
#define H_THREADPOOL
// 标准库头文件：涵盖线程、同步、容器、类型转换等核心功能
#include<iostream>
#include<vector>
#include<queue>
#include<atomic>
#include<mutex>
#include<thread>
#include<chrono>
#include<condition_variable>
#include<memory>
#include<utility>
#include<functional>
#include<future>
#include<any>
#include<algorithm>
#include<string>
#include<exception>
#include<limits>
#include<semaphore>
#include<optional>
// 线程池工作模式枚举：支持两种核心调度策略
enum class PoolMode {
    FIXED,   // 固定线程数模式：核心线程数=最大线程数，无动态扩缩容
    CACHED   // 缓存线程数模式：支持动态扩缩容，根据任务量调整线程数
};

// 任务封装结构体：统一管理任意类型任务（支持有/无返回值、异常传递）
struct Task {
    std::function<std::any()> func; // 类型擦除后的任务函数：无参数，返回any（兼容任意返回值）
    std::shared_ptr<std::promise<std::any>> promise; // 用于传递任务结果/异常的promise（共享所有权）

    Task() = default; // 默认构造函数

    // 模板构造函数：接收任意函数+任意参数，封装为统一的无参任务
    template<typename Func, typename... Args>
    explicit Task(Func&& func, Args&&... args) {
        // 完美转发参数，绑定为无参lambda（类型擦除核心）
        this->func = [func = std::forward<Func>(func), ... args = std::forward<Args>(args)]() -> std::any {
            try {
                // 编译期判断任务返回值是否为void：避免无返回值任务的类型兼容问题
                if constexpr (std::is_same_v<std::invoke_result_t<Func, Args...>, void>) {
                    std::invoke(func, args...); // 执行无返回值任务
                    return std::any(); // 返回空any，统一接口
                }
                else {
                    return std::invoke(func, args...); // 执行有返回值任务，结果存入any
                }
            }
            catch (...) {
                throw; // 捕获所有异常，向上传递（最终通过promise传递给future）
            }
            };
        // 创建promise，与future绑定（用户通过future获取结果/异常）
        this->promise = std::make_shared<std::promise<std::any>>();
    }
    // 移动构造函数：支持任务对象的移动语义（避免拷贝开销）
    Task(Task&& other) noexcept
        : func(std::move(other.func))
        , promise(std::move(other.promise)) {
    }
    // 移动赋值运算符：支持任务对象的移动赋值
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            func = std::move(other.func);
            promise = std::move(other.promise);
        }
        return *this;
    }

    // 禁止拷贝构造和拷贝赋值：任务对象不可拷贝（避免资源竞争）
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
};

// 线程安全的任务队列：支持多线程池共享、容量控制、自动唤醒
class TaskQueue {
private:
    std::queue<Task> taskQ;                // 存储任务的FIFO队列（核心容器）
    std::atomic_int queueSize = 0;         // 队列中任务数量（原子变量，线程安全读写）
    std::atomic_int maxCapacity = std::numeric_limits<int>::max(); // 队列最大容量（默认无上限）
    std::mutex queueMutex;                 // 保护队列操作的互斥锁（避免数据竞争）
    std::vector<std::condition_variable*> notifyConds; // 多线程池共享核心：存储所有关联线程池的条件变量
    std::atomic_bool queueShutdown = false; // 队列关闭标志（独立于线程池，拒绝新任务）

public:
    TaskQueue() = default; // 默认构造：无上限队列

    // 带初始容量的构造函数：指定队列最大容量
    explicit TaskQueue(int initCapacity) {
        if (initCapacity > 0) {
            maxCapacity .store(initCapacity);
            std::cout << "队列最大容量设置为:" << maxCapacity.load() << std::endl;
        }
        else {
            std::cerr << "[任务队列] 初始容量非法（必须>0），设置为无上限" << std::endl;
            maxCapacity = std::numeric_limits<int>::max();
        }
    }
    ~TaskQueue() = default; // 析构函数：无需额外操作（容器自动释放）
    // 绑定线程池的条件变量：多线程池共享时，每个线程池必须调用此接口
    void bindConditionVariable(std::condition_variable* cond) {
        if (cond == nullptr || queueShutdown.load()) {
            std::cerr << "[任务队列] 条件变量为空或队列已关闭，绑定失败" << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
        // 避免重复绑定（防止同一线程池多次绑定导致重复唤醒）
        for (auto existingCond : notifyConds) {
            if (existingCond == cond) {
                std::cerr << "[任务队列] 条件变量已绑定，忽略" << std::endl;
                return;
            }
        }
        notifyConds.push_back(cond);
        std::cout << "[任务队列] 成功绑定条件变量，当前绑定数：" << notifyConds.size() << std::endl;
    }
    // 解绑线程池的条件变量：线程池销毁/关机时调用，避免野指针
    void unbindConditionVariable(std::condition_variable* cond) {
        if (cond == nullptr) return;
        std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
        auto it = std::find(notifyConds.begin(), notifyConds.end(), cond);
        if (it != notifyConds.end()) {
            notifyConds.erase(it);
            std::cout << "[任务队列] 成功解绑条件变量，当前绑定数：" << notifyConds.size() << std::endl;
        }
    }
    // 核心接口：提交任意任务（兼容有/无返回值），返回future供用户获取结果
    template<typename Func, typename... Args>
    std::future<std::any> submitTask(Func&& func, Args&&... args) {
        // 队列已关闭：拒绝提交新任务
        if (queueShutdown.load()) {
            std::cerr << "[任务队列] 队列已关闭，拒绝提交任务" << std::endl;
            std::promise<std::any> emptyPromise;
            emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("TaskQueue is shutdown")));
            return emptyPromise.get_future();
        }

        // 队列已满：拒绝提交新任务（避免内存溢出）
        if (isQueueFull()) {
            std::cerr << "[任务队列] 队列已满（当前：" << queueSize.load() << "，最大：" << maxCapacity << "），拒绝提交" << std::endl;
            std::promise<std::any> emptyPromise;
            emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("TaskQueue is full")));
            return emptyPromise.get_future();
        }

        // 封装任务：创建Task对象，绑定函数和参数
        Task task(std::forward<Func>(func), std::forward<Args>(args)...);
        std::future<std::any> future = task.promise->get_future(); // 获取与promise绑定的future

        // 任务入队：加锁保证线程安全
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            taskQ.emplace(std::move(task)); // 移动语义，避免拷贝
            queueSize++; // 任务数原子递增
        }

        wakeupAllBoundPools(); // 唤醒所有关联线程池的空闲线程（多池共享核心）
        return future;
    }
    // 工作线程调用：从队列取出任务（非阻塞，无任务返回nullopt）
    std::optional<Task> taskTake() {
        std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护队列操作
        // 队列空或已关闭：返回空
        if (taskQ.empty() || queueShutdown.load()) {
            return std::nullopt;
        }

        // 取出队首任务（移动语义，避免拷贝）
        Task task = std::move(taskQ.front());
        taskQ.pop();
        queueSize--; // 任务数原子递减
        return task;
    }
    // 唤醒所有关联线程池的一个空闲线程：任务入队时调用，确保所有线程池感知任务
    void wakeupAllBoundPools() {
        std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
        for (auto cond : notifyConds) {
            if (cond != nullptr) {
                cond->notify_one(); // 每个线程池唤醒一个线程（减少上下文切换）
            }
        }
        if (!notifyConds.empty()) {
            std::cout << "[任务队列] 唤醒所有关联线程池（" << notifyConds.size() << "个），各唤醒一个线程" << std::endl;
        }
    }
    // 强制唤醒所有关联线程池的所有线程：线程池/队列关闭时调用，确保线程正常退出
    void wakeupAllThreadsInBoundPools() {
        std::lock_guard<std::mutex> lock(queueMutex); // 加锁保护条件变量列表
        for (auto cond : notifyConds) {
            if (cond != nullptr) {
                cond->notify_all(); // 唤醒所有线程（避免线程阻塞在wait）
            }
        }
        std::cout << "[任务队列] 强制唤醒所有关联线程池的所有线程" << std::endl;
    }
    // 关闭队列：拒绝新任务，唤醒所有线程处理剩余任务
    void shutdownQueue() {
        if (queueShutdown.load()) return;
        queueShutdown.store(true); // 设置关闭标志（原子操作）
        wakeupAllThreadsInBoundPools(); // 唤醒所有线程处理剩余任务
        std::cout << "[任务队列] 队列已关闭，拒绝新任务，唤醒所有线程处理剩余任务" << std::endl;
    }
    // 判断队列是否已关闭：供外部线程池检查状态
    bool isQueueShutdown() const {
        return queueShutdown.load();
    }

    // 设置队列最大容量：支持动态调整（运行时可修改）
    void setMaxCapacity(int capacity) {
        if (capacity <= 0 || queueShutdown.load()) {
            std::cerr << "[任务队列] 容量非法或队列已关闭，设置失败" << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(queueMutex);
        maxCapacity = capacity;
        std::cout << "[任务队列] 最大容量设置为：" << capacity << "（当前任务数：" << queueSize.load() << "）" << std::endl;
    }
    // 获取当前队列任务数（线程安全）
    int getCurrentSize() const { return queueSize.load(); }
    // 获取队列最大容量（线程安全）
    int getMaxCapacity() const { 
        return maxCapacity.load(); }
    //判断队列是否满
    bool isQueueFull() {
        if (maxCapacity.load() == std::numeric_limits<int>::max()) {
            return false;
        }
        return queueSize.load() >= maxCapacity.load();
    }
    // 判断队列是否为空（线程安全）
    bool empty() { return queueSize.load() == 0; }

    // 禁止拷贝和移动：队列是核心资源，不可拷贝
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    TaskQueue(TaskQueue&&) = delete;
    TaskQueue& operator=(TaskQueue&&) = delete;
};

// 线程池核心类：管理工作线程、调度任务、动态扩缩容（支持多池共享任务队列）
class ThreadPool {
private:
    // 工作线程状态结构体：存储线程对象、是否完成、线程ID
    struct WorkerStatus {
        std::thread thread;       // 工作线程对象
        std::atomic_bool isFinish; // 线程是否已完成（用于清理）
        std::thread::id tid;      // 线程ID（用于标识和调试）

        WorkerStatus() : isFinish(false), tid(std::thread::id()) {} // 默认构造

        // 带线程对象的构造函数：移动语义接收线程
        WorkerStatus(std::thread&& t)
            : thread(std::move(t))
            , isFinish(false)
            , tid(this->thread.get_id()) { // 获取线程ID
        }

        // 移动构造函数：支持WorkerStatus对象的移动
        WorkerStatus(WorkerStatus&& other) noexcept
            : thread(std::move(other.thread))
            , isFinish(other.isFinish.load()) // 原子变量加载
            , tid(other.tid) {
        }

        // 移动赋值运算符：支持WorkerStatus对象的移动赋值
        WorkerStatus& operator=(WorkerStatus&& other) noexcept {
            if (this != &other) {
                thread = std::move(other.thread);
                isFinish = other.isFinish.load();
                tid = other.tid;
            }
            return *this;
        }

        // 禁止拷贝：线程对象不可拷贝
        WorkerStatus(const WorkerStatus&) = delete;
        WorkerStatus& operator=(const WorkerStatus&) = delete;
    };

    // RAII嵌套类：自动管理忙线程数（构造+1，析构-1，异常安全）
    class BusyNumGuard {
    public:
        // 构造：忙线程数+1（加锁保证线程安全）
        BusyNumGuard(std::atomic_int& busyNum, std::mutex& lock)
            : m_busyNum(busyNum)
            , m_lock(lock) {
            std::lock_guard<std::mutex> guard(m_lock);
            m_busyNum++;
        }
        // 析构：忙线程数-1（无论正常返回还是异常，都会执行）
        ~BusyNumGuard() {
            std::lock_guard<std::mutex> guard(m_lock);
            m_busyNum--;
        }

        // 禁止拷贝：RAII对象不可拷贝
        BusyNumGuard(const BusyNumGuard&) = delete;
        BusyNumGuard& operator=(const BusyNumGuard&) = delete;
    private:
        std::atomic_int& m_busyNum; // 引用线程池的忙线程数
        std::mutex& m_lock;         // 保护忙线程数的互斥锁
    };

private:
    std::atomic_int liveNum = 0;     // 存活的工作线程数（原子变量，线程安全）
    std::atomic_int busyNum = 0;     // 正在执行任务的忙线程数（原子变量）
    std::atomic_int exitNum = 0;     // 待退出的线程数（CACHED模式缩容用）
    std::atomic_int minNum = 0;      // 核心线程数（最小存活线程数）
    std::atomic_int maxNum = 0;      // 最大线程数（CACHED模式上限）
    std::atomic_bool shutdown = false; // 线程池关闭标志（原子变量）

    std::string PoolName;            // 线程池名称（用于日志和调试）
    std::mutex poolMutex;            // 保护线程池状态的互斥锁（如工作线程列表）
    std::mutex busyMutex;            // 保护忙线程数的互斥锁（配合BusyNumGuard）
    std::condition_variable notEmpty; // 线程池的条件变量（任务队列非空时唤醒）

    std::thread managerThread;       // 管理者线程（CACHED模式：动态扩缩容）
    std::vector<WorkerStatus> workersThread; // 工作线程列表（存储所有工作线程）
    std::thread fixedGuardThread;   //fixed管理者

    TaskQueue* TaskQ = nullptr;      // 关联的任务队列（支持多池共享）
    PoolMode poolmode;               // 线程池工作模式（FIXED/CACHED）
    int ADDCOUNT = 2;                // CACHED模式每次扩容的线程数
    bool isInitTrue = false;         // 线程池是否初始化成功

    std::counting_semaphore<10> fixedRebuildSem{ 0 };

public:
    // FIXED模式构造函数：固定线程数（核心线程数=最大线程数）
    ThreadPool(std::string name, TaskQueue* taskq, PoolMode mode, int livenum = std::thread::hardware_concurrency())
        : PoolName(name)
        , poolmode(mode) {
        // 参数合法性检查：线程数>0、任务队列非空、模式为FIXED、队列未关闭
        if (livenum <= 0 || taskq == nullptr || mode != PoolMode::FIXED || taskq->isQueueShutdown()) {
            std::cerr << PoolName << "[ThreadPool] 初始化失败：参数非法或队列已关闭" << std::endl;
            exit(EXIT_FAILURE); // 初始化失败直接退出（避免后续错误）
        }

        minNum = livenum;  // FIXED模式：核心线程数=传入的线程数
        maxNum = livenum;  // FIXED模式：最大线程数=核心线程数
        liveNum = livenum; // 初始存活线程数=核心线程数
        TaskQ = taskq;     // 绑定任务队列
        isInitTrue = true; // 标记初始化成功

        TaskQ->bindConditionVariable(&notEmpty); // 将线程池的条件变量绑定到队列

        workersThread.reserve(livenum); // 预分配线程列表容量（避免频繁扩容）
        // 创建核心工作线程
        for (int i = 0; i < livenum; ++i) {
            workersThread.emplace_back(
                std::thread([this]() { this->workerFunc(); }) // 线程执行workerFunc
            );
            std::cout << PoolName << "[初始化] 创建核心线程，tid=" << workersThread.back().tid << std::endl;
        }
        //创建线程保护者
        fixedGuardThread = std::thread([this] {this->fixedGuardFunc(); });
        std::cout << PoolName << "[初始化] 创建守护者线程，tid=" << fixedGuardThread.get_id() << std::endl;
    }
    // CACHED模式构造函数：动态扩缩容（核心线程数+最大线程数）
    ThreadPool(std::string name, TaskQueue* taskq, PoolMode mode, int min, int max)
        : PoolName(name)
        , poolmode(mode) {
        // 参数合法性检查：核心线程数>0、最大>=核心、任务队列非空、模式为CACHED、队列未关闭
        if (min <= 0 || max < min || taskq == nullptr || mode != PoolMode::CACHED || taskq->isQueueShutdown()) {
            std::cerr << PoolName << "[ThreadPool] 初始化失败：参数非法或队列已关闭" << std::endl;
            exit(EXIT_FAILURE);
        }

        minNum = min;      // 核心线程数（最小存活数）
        maxNum = max;      // 最大线程数（扩容上限）
        liveNum = min;     // 初始存活线程数=核心线程数
        TaskQ = taskq;     // 绑定任务队列
        isInitTrue = true; // 标记初始化成功

        TaskQ->bindConditionVariable(&notEmpty); // 绑定条件变量到队列

        workersThread.reserve(max); // 预分配最大容量（避免扩容开销）
        // 创建核心工作线程
        for (int i = 0; i < min; ++i) {
            workersThread.emplace_back(
                std::thread([this]() { this->workerFunc(); })
            );
            std::cout << PoolName << "[初始化] 创建核心线程，tid=" << workersThread.back().tid << std::endl;
        }

        // 创建管理者线程（负责动态扩缩容）
        managerThread = std::thread([this] { this->managerFunc(); });
        std::cout << PoolName << "[初始化] 创建管理者线程，tid=" << managerThread.get_id() << std::endl;
    }
    // 析构函数：核心！自动解绑队列关联+关闭线程池（确保资源回收）
    ~ThreadPool() {
        std::cout << PoolName << "[析构] 线程池开始销毁，自动解绑任务队列关联" << std::endl;
        
        // 关闭线程池：回收所有工作线程和管理者线程
        shutdownPool();
        // 解绑条件变量：避免队列持有已销毁线程池的条件变量（野指针）
        if (TaskQ != nullptr) {
            TaskQ->unbindConditionVariable(&notEmpty);
        }
        std::cout << PoolName << "[析构] 线程池销毁完成" << std::endl;
    }
    // 核心接口：提交任务（对外暴露，兼容任意函数+参数）
    template<typename Func, typename... Args>
    std::future<std::any> submitTask(Func&& func, Args&&... args) {
        // 合法性检查：未初始化、已关闭、队列空、队列已关闭 → 拒绝提交
        if (!isInitTrue || shutdown.load() || TaskQ == nullptr || TaskQ->isQueueShutdown()
            || TaskQ->isQueueFull()) {
            std::cerr << PoolName << "[提交任务] 线程池无效或队列已关闭，拒绝提交" << std::endl;
            std::promise<std::any> emptyPromise;
            emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("Submit failed")));
            return emptyPromise.get_future();
        }
        // 委托给任务队列的submitTask（统一逻辑）
        return TaskQ->submitTask(std::forward<Func>(func), std::forward<Args>(args)...);
    }
    // 关闭线程池：优雅退出，回收所有线程（可手动调用，析构时自动调用）
    void shutdownPool() {
        if (shutdown.load() || !isInitTrue) {
            return; // 已关闭或未初始化，直接返回
        }

        shutdown.store(true); // 设置关闭标志（原子操作）
        std::cout << PoolName << "[关机] 开始关闭线程池" << std::endl;

        // 唤醒所有线程：通过队列唤醒所有关联线程（避免遗漏）
        if (TaskQ != nullptr) {
            TaskQ->wakeupAllThreadsInBoundPools();
        }
        else {
            notEmpty.notify_all(); // 队列空时直接唤醒自身线程
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 避免虚假唤醒
        // 二次唤醒：确保所有线程都能收到关闭信号（防止第一次唤醒失败）
        if (TaskQ != nullptr) {
            TaskQ->wakeupAllThreadsInBoundPools();
        }
        else {
            notEmpty.notify_all();
        }

        // 回收工作线程：加锁保护线程列表
        {
            std::lock_guard<std::mutex> lock(poolMutex);
            for (auto& worker : workersThread) {
                if (worker.thread.joinable()) { // 线程可join（未被回收）
                    worker.thread.join(); // 阻塞等待线程退出
                    std::cout << PoolName << "[关机] 回收线程，tid=" << worker.tid << std::endl;
                }
            }
            workersThread.clear(); // 清空线程列表

        }
        // 回收管理者线程（仅CACHED模式有）
        if (poolmode == PoolMode::CACHED && managerThread.joinable()) {
            std::thread::id tid = managerThread.get_id();
            managerThread.join();
            std::cout << PoolName << "[关机] 回收管理者线程，tid=" << tid << std::endl;
        }
        // 回收守护者线程（仅FIXED模式有）
        if (poolmode == PoolMode::FIXED && fixedGuardThread.joinable()) {
            std::thread::id tid = fixedGuardThread.get_id();
            fixedRebuildSem.release();
            fixedGuardThread.join();
            std::cout << PoolName << "[关机] 回收守护者线程，tid=" <<tid << std::endl;
        }

        std::cout << PoolName << "[关机] 线程池关闭完成，回收" << liveNum.load() << "个核心线程" << std::endl;
        isInitTrue = false; // 标记未初始化
    }
    // 辅助接口：获取线程池名称（用于测试和日志）
    std::string getPoolName() const {
        return PoolName;
    }
    // 重启线程池：仅支持已关闭且队列未关闭的线程池
    void resumePool() {
        if (!shutdown.load() || TaskQ->isQueueShutdown()) {
            std::cerr << PoolName << "[重启] 无法重启：未关闭或队列已关闭" << std::endl;
            return;
        }

        shutdown.store(false); // 重置关闭标志
        exitNum.store(0);      // 重置待退出数
        liveNum.store(minNum); // 重置存活数为核心数
        isInitTrue = true;     // 标记初始化成功

        // 重新绑定条件变量到队列
        if (TaskQ != nullptr) {
            TaskQ->bindConditionVariable(&notEmpty);
        }

        // 重建核心工作线程
        {
            std::lock_guard<std::mutex> lock(poolMutex);
            workersThread.reserve(maxNum);
            for (int i = 0; i < minNum; ++i) {
                workersThread.emplace_back(
                    std::thread([this]() { this->workerFunc(); })
                );
                std::cout << PoolName << "[重启] 创建核心线程，tid=" << workersThread.back().tid << std::endl;
            }
        }

        // 重建管理者线程（仅CACHED模式）
        if (poolmode == PoolMode::CACHED) {
            if (managerThread.joinable()) {
                managerThread.join();
            }
            managerThread = std::thread([this]() { this->managerFunc(); });
            std::cout << PoolName << "[重启] 创建管理者线程，tid=" << managerThread.get_id() << std::endl;
        }

        // 重建守护者线程（仅FIXED模式）
        if (poolmode == PoolMode::FIXED) {
            if (fixedGuardThread.joinable()) {
                fixedGuardThread.join();
            }
            fixedGuardThread = std::thread([this]() { this->fixedGuardFunc(); });
            std::cout << PoolName << "[重启] 创建守护者线程，tid=" << fixedGuardThread.get_id() << std::endl;
        }

    }
    // 设置CACHED模式每次扩容的线程数
    void setEverytimeAddCount(int num) {
        if (poolmode != PoolMode::CACHED) {
            std::cerr << PoolName << "[灵活调节] 仅CACHED模式支持" << std::endl;
            return;
        }
        if (num <= 0) {
            std::cerr << PoolName << "[灵活调节] 扩容数必须>0" << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(poolMutex);
        ADDCOUNT = num;
        std::cout << PoolName << "[灵活调节] 扩容数设置为：" << num << std::endl;
    }
    // 线程池重命名：仅支持已关闭的线程池
    void Rename(std::string name) {
        if (!shutdown.load()) {
            std::cerr << PoolName << "[重命名] 仅关闭后支持重命名" << std::endl;
            return;
        }
        std::cout << PoolName << "[重命名] 重命名为：" << name << std::endl;
        PoolName = name;
    }
    // 获取存活线程数（对外接口）
    int getLiveNum() const { return liveNum.load(); }
    // 获取忙线程数（对外接口）
    int getBusyNum() const { return busyNum.load(); }
    // 获取队列当前任务数（对外接口）
    int getQueueCurrentSize() const { return TaskQ->getCurrentSize(); }
    // 获取队列最大容量（对外接口）
    int getQueueMaxCapacity() const { return TaskQ->getMaxCapacity(); }
    // 设置队列最大容量（对外接口）
    void setQueueMaxCapacity(int capacity) {
        if (TaskQ == nullptr || shutdown.load() || TaskQ->isQueueShutdown()) {
            std::cerr << PoolName << "[任务队列改变容量] 设置失败：无效状态" << std::endl;
            return;
        }
        TaskQ->setMaxCapacity(capacity);
    }
private:
    // 工作线程核心逻辑：循环取任务→执行任务→处理结果/异常
    void workerFunc() {
        std::thread::id curTid = std::this_thread::get_id(); // 获取当前线程ID
        std::cout << PoolName << "[核心线程] 线程启动，tid=" << curTid << std::endl;

        try {
            // 循环：未关闭则持续取任务（关闭后退出）
            while (!shutdown.load()) {
                // 超时等待：1秒超时，避免永久阻塞（多池竞争时公平性）
                std::unique_lock<std::mutex> lock(poolMutex);
                // 等待条件：队列非空 / 线程池关闭 / 缩容（CACHED模式）
                bool waitResult = notEmpty.wait_for(lock, std::chrono::seconds(1), [this]() {
                    return !TaskQ->empty() || shutdown.load() || (exitNum.load() > 0 && liveNum.load() > minNum.load());
                    });

                // 优先检测关闭信号：线程池关闭或队列关闭→退出
                if (shutdown.load() || TaskQ->isQueueShutdown()) {
                    lock.unlock();
                    break;
                }

                // CACHED模式缩容：当前线程是待退出线程（exitNum>0且存活数>核心数）
                if (exitNum.load() > 0 && liveNum.load() > minNum.load()) {
                    exitNum--; // 待退出数减1
                    liveNum--; // 存活数减1
                    lock.unlock();
                    std::cout << PoolName << "[核心线程] 缩容退出，tid=" << curTid << "，剩余线程数=" << liveNum.load() << std::endl;
                    break;
                }

                // 超时且队列空：重试（避免永久阻塞）
                if (!waitResult && TaskQ->empty()) {
                    lock.unlock();
                    continue;
                }

                // 从队列取任务（非阻塞，已加锁）
                auto taskOpt = TaskQ->taskTake();
                lock.unlock(); // 解锁：任务执行不需要持有poolMutex

                // 无任务：继续循环
                if (!taskOpt.has_value()) {
                    continue;
                }

                // 执行任务：用BusyNumGuard自动管理忙线程数
                Task task = std::move(taskOpt.value());
                {
                    BusyNumGuard guard(busyNum, busyMutex); // 忙线程数+1
                    try {
                        std::any result = task.func(); // 执行任务，获取结果
                        if (task.promise != nullptr) {
                            task.promise->set_value(std::move(result)); // 设置结果到promise
                        }
                    }
                    catch (...) {
                        // 捕获任务执行异常，传递给future
                        if (task.promise != nullptr) {
                            task.promise->set_exception(std::current_exception());
                        }
                        std::cerr << PoolName << "[核心线程] 任务异常，tid=" << curTid << std::endl;
                    }
                } // BusyNumGuard析构：忙线程数-1

                std::cout << PoolName << "[核心线程] 任务完成，tid=" << curTid << "，剩余任务数=" << TaskQ->getCurrentSize() << std::endl;
            }
        }
        catch (const std::exception& e) {
            // 捕获线程执行异常（如锁异常）
            std::cerr << PoolName << "[核心线程] 线程异常退出，tid=" << curTid << "，原因：" << e.what() << std::endl;
            liveNum--; // 存活数减1（异常退出）
        }
        catch (...) {
            // 捕获未知异常
            std::cerr << PoolName << "[核心线程] 线程未知异常退出，tid=" << curTid << std::endl;
            liveNum--;
        }

        // 标记线程为已完成（供管理者线程清理）
        std::lock_guard<std::mutex> lock(poolMutex);
        for (auto& worker : workersThread) {
            if (worker.tid == curTid) {
                worker.isFinish = true;
                break;
            }
        }

        std::cout << PoolName << "[核心线程] 线程退出，tid=" << curTid << std::endl;
        if (poolmode == PoolMode::FIXED && !shutdown.load()) {
            std::cout << PoolName << "发送线程退出信号，信号量计数+1" << std::endl;
            fixedRebuildSem.release(); //也就是post
        }

    }
    //FIEXD模式守护者(对标managerFunc以解决因异常而退出线程没有及时删除和补充的)
    void fixedGuardFunc() {
        std::thread::id curTid = std::this_thread::get_id();
        std::cout << PoolName << "[守护者] [启动]，tid=" << curTid << std::endl;
        while (!shutdown.load() && !TaskQ->isQueueShutdown()) {
            fixedRebuildSem.acquire();  //也就是wait
            if (shutdown.load()) {
                break;
            }
            std::cout << PoolName << "[守护者] 收到线程退出信号，开始清理+补充" << std::endl;
            workersThread.emplace_back(
                std::thread([this]() { this->workerFunc(); })
            );
            liveNum++;
            std::cout << PoolName << "[守护者] [rebulid]，tid=" << workersThread.back().tid << std::endl;
            {
                std::lock_guard<std::mutex> lock(poolMutex);  // 加锁保护线程列表
                auto it = workersThread.begin();
                while (it != workersThread.end()) {
                    if (it->isFinish) {
                        std::thread::id tid = it->tid;
                        // 回收线程系统资源：必须join，避免僵尸线程
                        if (it->thread.joinable()) {
                            it->thread.join();
                            std::cout << PoolName << "[守护者][清理] 回收已完成线程，tid=" << tid << std::endl;
                        }
                        // 从列表中删除线程状态对象：释放容器内存
                        it = workersThread.erase(it);
                        std::cout << PoolName << "[守护者][清理] 删除线程状态对象，tid=" << tid << std::endl;
                    }
                    else {
                        ++it;
                    }
                }
            }
        }
        std::cout << PoolName << "[守护者线程] [退出]，tid=" << curTid << std::endl;
    }
    // 管理者线程逻辑（仅CACHED模式）：动态扩缩容、清理已完成线程
    void managerFunc() {
        std::thread::id curTid = std::this_thread::get_id();
        std::cout << PoolName << "[管理者] [启动]，tid=" << curTid << std::endl;

        // 循环：未关闭且队列未关闭→持续监控
        while (!shutdown.load() && !TaskQ->isQueueShutdown()) {
            std::this_thread::sleep_for(std::chrono::seconds(3)); // 每3秒监控一次（避免频繁检查）

            // 获取当前线程池状态（原子变量加载，线程安全）
            int curLive = liveNum.load();
            int curBusy = busyNum.load();
            int curQueue = TaskQ->getCurrentSize();

            // 打印监控日志
            std::cout << PoolName << "\n[管理者] [监控]：存活=" << curLive << "，忙=" << curBusy << "，队列任务数=" << curQueue << std::endl;

            // 扩容逻辑：
            // 1. 存活数 < 核心数（异常情况，补充核心线程）
            // 2. 队列任务数 > 存活数（任务积压）且 存活数 < 最大数（未达扩容上限）
            if (curLive < minNum.load() || (curQueue > curLive && curLive < maxNum.load())) {
                std::lock_guard<std::mutex> lock(poolMutex); // 加锁保护线程列表
                int addCount = 0;
                int targetLive = std::min(curLive + ADDCOUNT, maxNum.load()); // 扩容目标（不超过最大数）
                // 循环创建线程，直到达到目标或任务积压缓解
                while (curLive < targetLive && (TaskQ->getCurrentSize() > curLive || curLive < minNum.load())) {
                    workersThread.emplace_back(
                        std::thread([this]() { this->workerFunc(); })
                    );
                    curLive++;
                    addCount++;
                    liveNum++; // 存活数原子递增
                    std::cout << PoolName << "[管理者] [扩容]，tid=" << workersThread.back().tid << "，当前存活=" << curLive << std::endl;
                }
            }

            // 缩容逻辑：忙线程数 * 2 < 存活数（线程空闲过多）且 存活数 > 核心数（未低于核心数）
            if (curBusy * 2 < curLive && curLive > minNum.load()) {
                std::lock_guard<std::mutex> lock(poolMutex); // 加锁保护exitNum
                int needExit = std::min(ADDCOUNT, curLive - minNum.load()); // 待退出数（不超过空闲数）
                if (needExit > 0) {
                    exitNum += needExit; // 标记待退出线程数
                    TaskQ->wakeupAllBoundPools(); // 唤醒线程，触发缩容判断
                    std::cout << PoolName << "[管理者] [缩容]，标记" << needExit << "个线程退出" << std::endl;
                }
            }

            // 清理已完成线程：移除isFinish=true且不可join的线程（避免列表冗余）
            {
                std::lock_guard<std::mutex> lock(poolMutex);  // 加锁保护线程列表
                auto it = workersThread.begin();
                int cleanCount = 0;
                while (it != workersThread.end()) {
                    // 仅清理已标记为完成的线程（isFinish=true）
                    if (it->isFinish) {
                        std::thread::id tid = it->tid;
                        // 回收线程系统资源：必须join，避免僵尸线程
                        if (it->thread.joinable()) {
                            it->thread.join();
                            std::cout << PoolName << "[管理者][清理] 回收已完成线程，tid=" << tid << std::endl;
                        }
                        // 从列表中删除线程状态对象：释放容器内存
                        it = workersThread.erase(it);
                        cleanCount++;
                        std::cout << PoolName << "[管理者][清理] 删除线程状态对象，tid=" << tid << std::endl;
                    }
                    else {
                        ++it;
                    }
                }

                if (cleanCount > 0) {
                    std::cout << PoolName << "[管理者][清理] 本次清理" << cleanCount << "个已完成线程，剩余列表大小=" << workersThread.size() << std::endl;
                }
            }
        }
        std::cout << PoolName << "[管理者线程] [退出]，tid=" << curTid << std::endl;
    }
};

#endif // !H_THREADPOOL