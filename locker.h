#ifndef LOCKER_H_
#define LOCKER_H_
#include<pthread.h>
#include<semaphore.h>
#include<iostream>

class Semphore{
public:
    Semphore(int val){
        int ret = sem_init(&m_sem, 0, val);
        if(ret != 0){
            throw std::exception();
        }
    }

    bool wait(){
        return  sem_wait(&m_sem) == 0;
    }
    bool post(){
        return sem_post(&m_sem) == 0;
    }
    ~Semphore(){
        sem_destroy(&m_sem);
    }
private:
    sem_t m_sem;
};

class Mutex{
public:
    Mutex(){
        int ret = pthread_mutex_init(&m_mtx, NULL);
        if(ret != 0){
            throw std::exception();
        }
    }

    bool lock(){
        return pthread_mutex_lock(&m_mtx) == 0;
    }

    bool unlock(){
        return pthread_mutex_unlock(&m_mtx) == 0;
    }

    ~Mutex(){
        pthread_mutex_destroy(&m_mtx);
    }

private:
    pthread_mutex_t m_mtx;
};

class Condition{
public:
    //条件变量wait前要上锁，wait时会释放锁，wait返回时会继续获取锁
    //wait的返回条件是signal/broadcast说明条件**可能满足**，wait返回之后继续检查条件，若不满足继续wait
    Condition(){
        int ret = pthread_mutex_init(&m_mtx, NULL);
        if(ret != 0){
            throw std::exception();
        }
        ret = pthread_cond_init(&m_cond, NULL);
        if(ret != 0){
            pthread_mutex_destroy(&m_mtx);
            throw std::exception();
        }
    }

    bool wait(){
        pthread_mutex_lock(&m_mtx);
        int ret = pthread_cond_wait(&m_cond, &m_mtx);
        pthread_mutex_unlock(&m_mtx);
        return ret == 0;
    }

    bool signal(){
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast(){
        return pthread_cond_broadcast(&m_cond) == 0;
    }

    ~Condition(){
        pthread_mutex_destroy(&m_mtx);
        pthread_cond_destroy(&m_cond);
    }
private:
    pthread_mutex_t m_mtx;
    pthread_cond_t m_cond;
};

#endif // LOCKER_H_