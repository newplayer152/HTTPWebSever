#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<queue>
#include<vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include "locker.h"


template<typename T>
class ThreadPool{
public:
    ThreadPool(int thread_number=8,int max_requests=10000)
        :m_stop(false),m_thread_number(thread_number),m_max_requests(max_requests){
        m_workers.emplace_back([this]{
            for(;;){
                T *task;
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);//获取锁
                    m_condition.wait(lock,[this]{return m_stop||!m_tasks.empty();});//条件变量等待有锁、停止或者任务队列不为空才运行
                    if(m_stop && m_tasks.empty())return;
                    task=std::move(m_tasks.front());
                    m_tasks.pop();
                }
                task->run();//执行任务
            }

        });

    }
    ~ThreadPool(){//设置标志位唤醒线程等待结束
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_stop=true;
        }
        m_condition.notify_all();
        for(auto & worker: m_workers)worker.join();

    }

    bool append(T* task){
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if ( m_tasks.size() >= m_max_requests ) {
                return false;
            }
            m_tasks.push(std::move(task));
        }
        m_condition.notify_one();
    }

private:
    //线程数量
    int m_thread_number;

    //存储线程
    std::vector<std::thread> m_workers;

    //请求任务队列
    std::queue<T*> m_tasks;

    //加锁
    std::mutex m_queueMutex;//用于加锁保护对线程池的异步操作

    //条件变量
    std::condition_variable m_condition;

    // 请求队列中最多允许的、等待处理的请求的数量  
    int m_max_requests; 

    //是否有任务需要处理
    Semaphore m_semaphore;

    // 是否结束线程          
    bool m_stop;  


};










#endif
