#pragma once

#include <future>
#include <functional>
#include <iostream>
#include <queue>
#include <set>
#include <vector>
#include <mutex>
#include <utility>
#include <exception>
#include <memory>

using namespace std;

namespace tars
{
/////////////////////////////////////////////////
/**
 * @file thread_pool.hpp
 * @brief 线程池类,采用c++11来实现了
 * 使用说明:
 * mmThread_poll tpool(5,10);
 * //将任务丢到线程池中
 * tpool.exec(testFunction, 10);    //参数和start相同
 * //等待线程池结束, 有两种方式:
 * //第一种等待线程池中无任务
 * tpool.waitForAllDone(1000);      //参数<0时, 表示无限等待(注意有人调用stop也会推出)
 * //此时: 外部需要结束线程池是调用
 * tpool.stop();
 * 注意:
 * mmThread_poll::exec执行任务返回的是个future, 因此可以通过future异步获取结果, 比如:
 * int testInt(int i)
 * {
 *     return i;
 * }
 * auto f = tpool.exec(testInt, 5);
 * cout << f.get() << endl;   //当testInt在线程池中执行后, f.get()会返回数值5
 *
 * class Test
 * {
 * public:
 *     int test(int i);
 * };
 * Test t;
 * auto f = tpool.exec(std::bind(&Test::test, &t, std::placeholders::_1), 10);
 * //返回的future对象, 可以检查是否执行
 * cout << f.get() << endl;
 */
/////////////////////////////////////////////////
/**
* @brief 线程异常
*/
class mmThread_pollException:public std::exception
{
public:
    mmThread_pollException(const string& detail):_detail("TC_ThreadPoolException:" + detail) { }
    virtual ~mmThread_pollException();

    virtual const char* what() final {return _detail.c_str();} 
private:
    string _detail;

};

/**
* @brief 用通线程池类(采用c++11实现)
*
* 使用方式说明:
* 具体示例代码请参见:examples/util/example_tc_thread_pool.cpp
*/
class mmThread_poll
{
protected:
    struct TaskFunc
    {
        TaskFunc(uint64_t expireTime) : _expireTime(expireTime)
        { }

        std::function<void()>   _func;
        uint64_t                _expireTime = 0;	//超时的绝对时间
    };
    typedef shared_ptr<TaskFunc> TaskFuncPtr;

public:
    /**
    * @brief 构造函数
    *
    */
    mmThread_poll(size_t maxPoolSizeNum, size_t corePoolSize)
    : _corePoolSize(corePoolSize & 0xFFFF)
    , _maxPoolSize(maxPoolSizeNum & 0xFFFF)
    {
        if(_corePoolSize < 1 || _corePoolSize > _maxPoolSize)
            throw mmThread_pollException(string("construct Thread Pool error,params error"));

        SetStat(RUNNING);
    }

    /**
    * @brief 析构, 会停止所有线程
    */
    virtual ~mmThread_poll(){
    }

    /**
    * @brief 获取线程个数.
    *
    * @return size_t 线程个数
    */
    size_t getThreadNum()
    {
        return _workerCnt.load();
    }

    /**
    * @brief 获取当前线程池的任务数
    *
    * @return size_t 线程池的任务数
    */
    size_t getJobNum()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _tasks.size();
    }

    /**
    * @brief 停止所有线程, 会等待所有线程结束
    */
    void stop(bool stopWaitAllDone=true)
    {
        if(!StatIs(RUNNING))
        {
            return;
        }
        
        {
            std::unique_lock<std::mutex> lock(_mutex);
            if(!StatIs(TIDYING))
            {
                if(stopWaitAllDone)
                    SetStat(SHUTDOWN);
                else
                    SetStat(STOP);
                _condition.notify_all();
            }
        }
        {
            std::unique_lock<std::mutex> lock(_mutex);
            while (!StatIs(TIDYING))
                _condition.wait(lock, StatIs(TIDYING));

            for(auto iter = _threads.begin(); iter != _threads.end(); )
            {
                if((*iter)->joinable())
                {
                    (*iter)->join();
                }
                _threads.erase(iter++);
            }
        }

    }


    /**
    * @brief 用线程池启用任务(F是function, Args是参数)
    *
    * @param ParentFunctor
    * @param tf
    * @return 返回任务的future对象, 可以通过这个对象来获取返回值
    */
    template <class F, class... Args>
    auto exec(F&& f, Args&&... args) -> std::pair<bool, std::future<decltype(f(args...))> >
    {
        return exec(0,f,args...);
    }

    /**
    * @brief 用线程池启用任务(F是function, Args是参数)
    *
    * @param 超时时间 ，单位ms (为0时不做超时控制) ；若任务超时，此任务将被丢弃
    * @param bind function
    * @return 返回 任务添加是否成功 + 任务的future对象, 可以通过这个对象来获取返回值
    */
    template <class F, class... Args>
    auto exec(int64_t timeoutMs, F&& f, Args&&... args) -> std::pair<bool, std::future<decltype(f(args...))> >
    {
        int64_t expireTime =  (timeoutMs == 0 ? 0 : std::time(nullptr)*1000 + timeoutMs);
        //定义返回值类型
        using RetType = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<RetType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        TaskFuncPtr fPtr = TaskFuncPtr(new TaskFunc(expireTime));
        fPtr->_func = [task]() {
            (*task)();
        };

        if(! StatIs(RUNNING))
            return std::make_pair(false,std::future<RetType>());

        //else

        auto currentThreadC = getThreadNum();
        if(currentThreadC < _corePoolSize)
        {
            //addWorker
            std::unique_lock<std::mutex> lock(_mutex);
            _threads.push_back(unique_ptr<thread>( new thread(&mmThread_poll::run, this) ));
            _workerCnt.fetch_add (1);

        }
        //hook:fix me task list is full
        if(getJobNum() >=_corePoolSize  )
        {
            if( currentThreadC <= _maxPoolSize)
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _threads.push_back(unique_ptr<thread>( new thread(&mmThread_poll::run, this)));
                _workerCnt.fetch_add (1);
            }
            else 
            {
                //hook:fix me
                //RejectedHander(task)
            }
        }
        
        {
        //add to worker queue
        std::unique_lock<std::mutex> lock(_mutex);
        _tasks.push(fPtr);
        _condition.notify_one();
        }

        return std::make_pair(true,task->get_future());
    }

    /**
     * @brief 等待当前任务队列中, 所有task全部结束(队列无任务).
     *
     * @param millsecond 等待的时间(ms), -1:永远等待
     * @return           true, 所有工作都处理完毕 
     *                   false,超时退出
     */
    bool waitForAllDone(int millsecond = -1)
    {
    std::unique_lock<std::mutex> lock(_mutex);

    if (_tasks.empty())
        return true;

    if (millsecond < 0)
    {
        _condition.wait(lock, [this] { return _tasks.empty(); });
        return true;
    }
    else
    {
        return _condition.wait_for(lock, std::chrono::milliseconds(millsecond), [this] { return _tasks.empty(); });
    }
    }

    /**
    * @brief 线程池是否退出
    */
    typedef enum _Stat 
    {
        RUNNING =0,
        SHUTDOWN=1,
        STOP=2,
        TIDYING=3,
        TERMNATED=4
    }Stat;

    bool isTerminate() { return _runState.load() == TERMNATED; }
    bool StatIs(mmThread_poll::Stat ckStat) { return _runState.load()== ckStat; }
    void SetStat(mmThread_poll::Stat stat) {  _runState.store(stat);}


protected:
    /**
    * @brief 获取任务
    *
    * @return TaskFuncPtr
    */
    bool get(TaskFuncPtr&task)
    {
     std::unique_lock<std::mutex> lock(_mutex);
    while (_tasks.empty() && StatIs(RUNNING))
    {
        _condition.wait(lock, [this] { return !(_tasks.empty() && StatIs(RUNNING)); });
    }

    if (!(StatIs(RUNNING) || StatIs(SHUTDOWN)))
        return false;

    if (!_tasks.empty())
    {
        task = std::move(_tasks.front());

        _tasks.pop();

        return true;
    }

    return false;

    }

    /**
    * @brief 线程运行态
    */
    void run()
    {
        static atomic<size_t> _taskCurrentExecCnt {0};
    //调用处理部分
        while (StatIs(RUNNING) || StatIs(SHUTDOWN))
        {
            TaskFuncPtr task;
            bool ok = get(task);
            if (ok)
            {
                _taskCurrentExecCnt.fetch_add(1);
                try
                {
                    if (task->_expireTime != 0 && task->_expireTime  < std::time(nullptr))
                    {
                        //超时任务，是否需要处理?
                    }
                    else
                    {
                        task->_func();
                    }
                }
                catch (...)
                {
                }

                _taskCurrentExecCnt.fetch_sub(1);

                //任务都执行完毕了
                std::unique_lock<std::mutex> lock(_mutex);
                if(_tasks.empty())
                {
                    if(_workerCnt.load () > _corePoolSize)
                    {
                        _workerCnt.fetch_sub(1);
                        return;
                    }
                
                    if (_taskCurrentExecCnt.load()== 0)
                    {
                        for(auto iter = _threads.begin(); iter != _threads.end(); )
                        {
                            if((*iter)->joinable())
                            {
                                (*iter)->join();
                                _threads.erase(iter++);
                                continue;
                            }
                            iter++;
                        }
                        _condition.notify_all();
                    }
                }
            }
        }//while
        _workerCnt.fetch_sub(1);
        if(_workerCnt.load() == 0)
            SetStat(TIDYING);
    }

protected:

    /**
    * 任务队列
    */
    queue<TaskFuncPtr> _tasks;

    /**
    * 工作线程
    */
    std::vector<std::unique_ptr<std::thread> > _threads;

    std::mutex                _mutex;

    std::condition_variable   _condition;

    //size_t                    _threadNum;

    //bool                      _bTerminate;

    
    struct {
    /// @brief core thread counter
        size_t _corePoolSize:16;
        /// @brief max thread counter
        size_t _maxPoolSize:16;
    } ;

    //_poolSize;

    std::atomic<size_t>          _workerCnt{ 0 }; //current woker counter
    std::atomic<u_char>          _runState{ 0 }; //current state


};



};