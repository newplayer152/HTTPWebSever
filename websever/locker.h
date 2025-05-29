#ifndef LOCKER_H
#define LOCKER_H

#include <mutex>
#include <condition_variable>

class Semaphore {//实现信号量
public:
    Semaphore():m_count(0){
    }
    Semaphore(int count):m_count(count){
    }

    void wait(){
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond.wait(lock,[this](){return m_count>0;});
        --m_count;
    }

    void post(){
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_count;
        m_cond.notify_one();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_cond;
    int m_count;


};






#endif