#ifndef PROCESSPOOL_H_
#define PROCESSPOOL_H_

#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<assert.h>
#include<errno.h>

#include<fcntl.h>

#include<sys/types.h>
#include<netdb.h>
#include<arpa/inet.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<signal.h>
#include<sys/wait.h>




static int sig_pipefd[2];

//信号处理函数
void sig_handler(int sig){
    int saved_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], (char* )&msg, 1, 0); //只发送一个字节
    errno = saved_errno;
}

//对信号sig添加handler处理函数，restart表示处理完信号是否重新执行被中断的系统调用
void addsig(int sig, void (*handler)(int sig), bool restart = true){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    sa.sa_handler = handler;
    if(restart){
        sa.sa_flags |= SA_RESTART;
    }

    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//设置对fd的读写位非阻塞
int setnonblocking(int fd){
    int old_fd_flag = fcntl(fd, F_GETFL); //获取fd原来的flag
    int new_fd_flag = old_fd_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_fd_flag);
    return old_fd_flag;
}

//将fd添加到内核创建的内核事件表epollfd中
void addfd(int epollfd, int fd){
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET; //监测 数据可读事件 和 边沿触发(事件发生时只触发一次)
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    //事件就绪 + 非阻塞I/O
    setnonblocking(fd);
}

//将fd从内核创建的内核事件表epollfd中删除
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}


class process{
public:
    process():m_pid(-1){}

    int m_pid;
    int m_pipefd[2]; //用于和父进程消息传递(写端)
};

template<class T>
class processpool{
    processpool(int listenfd, int process_number = 8); //使用单例，不允许创建
public:

    static processpool<T>* create(int listenfd, int process_number = 8){
        if(!m_instance){
            m_instance = new processpool<T> (listenfd, process_number);
        }
        return m_instance;
    }
    ~processpool(){
        delete [] m_sub_process;
    }

    void run();

private:
    //统一事件处理
    void setup_sig_pipe();
    void run_parent();
    void run_child();

    static const int MAX_PROCESS_NUMBER = 16;
    static const int USER_PER_PROCESS = 65535;
    static const int MAX_EVENT_NUMBER = 10000;

    int m_process_number; //进程池中进程数量
    int m_idx;      //通过fork时设置m_idx, 获取子进程在进程池中的索引
    int m_epollfd;   //每个进程都有自己的epoll内核时间表
    int m_listenfd;  //父进程监听listendfd，子进程接受连接
    int m_stop;     //进程是否停止
    process* m_sub_process;     //子进程


    static processpool<T>* m_instance;  //单例

};
template <class T>
processpool<T>* processpool<T>::m_instance = nullptr;

template<class T>
processpool<T>::processpool(int listenfd, int process_number){
    assert(process_number > 0 && process_number <= MAX_PROCESS_NUMBER);

    m_process_number = process_number;
    m_listenfd = listenfd;
    m_idx = -1;
    m_stop = false;

    m_sub_process = new process[process_number];
    assert(m_sub_process != nullptr);

    for(int i = 0; i < process_number; ++i){
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
        assert(ret != -1);

        int pid = fork();
        assert(pid >= -1);

        if(pid > 0){
            m_sub_process[i].m_pid = pid;
            close(m_sub_process[i].m_pipefd[1]);
        }
        else{
            close(m_sub_process[i].m_pipefd[0]);
            m_idx = i;
            break;
        }
    }
}

template<class T>
void processpool<T>::run(){
    if(m_idx == -1){
        run_parent();
    }
    else{
        run_child();
    }
}

template<class T>
void processpool<T>::setup_sig_pipe(){
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert(ret != -1);

    setnonblocking(sig_pipefd[1]);
    
    //添加信号监听
    addfd(m_epollfd, sig_pipefd[0]);

    //设置信号处理函数
    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);

}



template<class T>
void processpool<T>::run_child(){

    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //设置子进程的信号事件统一处理
    setup_sig_pipe();

    //监听与父进程通信I/O
    int pipefd = m_sub_process[m_idx].m_pipefd[1];
    addfd(m_epollfd, pipefd);

    T *users = new T[USER_PER_PROCESS];
    epoll_event events[MAX_EVENT_NUMBER];

    while(!m_stop){
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        
        if(number < 0 && errno != EINTR){ //epoll_wait失败并且不是被中断
            printf("pid %d: epoll wait failure\n", getpid());
            break;
        }

        for(int i = 0; i < number; ++i){
            int sockfd = events[i].data.fd;

            //父进程监测到有客户端请求连接，子进程接受连接
            if((sockfd == pipefd) && (events[i].events&EPOLLIN)){
                printf("pid %d:准备接受客户端连接\n", getpid());
                //只有读到父进程管道中发来的1时才确保有新客户到来
                int client = 0;
                int ret = recv(pipefd, &client, sizeof(client), 0);
                if((ret < 0 && errno != EINTR) || ret == 0){
                    continue;
                }

                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                int connfd = accept(m_listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
                if(connfd < 0){
                    printf("pid %d: accept failure. errno: %d\n", getpid(), errno);
                    continue;
                }
                addfd(m_epollfd, connfd);

                //池使用的用户处理模块需要提供的接口
                users[connfd].init(m_epollfd, connfd, (sockaddr* )&client_addr);
            }
            //统一信号处理
            else if(sockfd == sig_pipefd[0]){
                char signals[1024];
                int ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if(ret <=0){
                    continue;
                }
                else {
                    for(int j = 0; j <ret; ++j){
                        switch (signals[j])
                        {
                        case SIGCHLD:
                            int stat;
                            pid_t pid;
                            //因为已经是收到子进程结束信号，所以不需要阻塞，直接回收即可
                            while((pid = waitpid(-1, &stat, WNOHANG)) > 0){
                                printf("join cgi process %d (grandson)\n", pid);
                                continue;
                            }
                            break;
                        case SIGINT:
                        case SIGTERM:
                            printf("[server process %d]: terminate\n", getpid());
                            m_stop = true;
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
            //处理发送的用户数据
            else if(events[i].events & EPOLLIN){
                printf("user request is going to process: \n");
                //调用‘池’的用户定义需要满足的接口
                users[sockfd].process();
                removefd(m_epollfd, sockfd);
            }
            else{
            }
        }
   }

    delete [] users;
    users = nullptr;
    close(pipefd);
    close(m_epollfd);

    close(sig_pipefd[0]);
    close(sig_pipefd[1]);
}

template<class T>
void processpool<T>::run_parent(){
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //父进程监听listenfd
    addfd(m_epollfd, m_listenfd);

    //设置子进程的信号事件统一处理
    setup_sig_pipe();

    epoll_event events[MAX_EVENT_NUMBER];
    int new_conn = 1;
    int rr_counter = 0; //Round Robin 计数器

    printf("[server]: start pid: %d\n", getpid());

    while(!m_stop){
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        
        if(number < 0 && errno != EINTR){
            printf("pool %d: epoll_wait failure\n", getpid());
            break;
        }

        for(int i = 0; i < number; ++i){
            int sockfd = events[i].data.fd;

            //监测到客户连接请求，通知子进程进行接受并处理
            if(sockfd == m_listenfd && events[i].events & EPOLLIN){
                //选择子进程，使用轮询的方式
                int j = rr_counter;
                do{
                    if(m_sub_process[j].m_pid != -1){
                        break;
                    }
                    j = (j+1) % m_process_number;
                }while(j != rr_counter);

                if(m_sub_process[j].m_pid == -1){
                    printf("no subprocess in serve, stop!!!\n");
                    m_stop = true;
                    break;
                }

                rr_counter = (j+1) % m_process_number;
                
                int pipefd = m_sub_process[j].m_pipefd[0];
                //发送消息
                send(pipefd, (char*)&new_conn, sizeof(new_conn), 0);

                printf("send connection to child[%d] to serve\n", j);
            }
            //parent统一信号处理
            else if(sockfd == sig_pipefd[0] && events[i].events & EPOLLIN){
                char signals[1024];
                int ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if(ret <=0){
                    continue;
                }
                else {
                    for(int j = 0; j <ret; ++j){
                        switch (signals[j])
                        {
                        case SIGCHLD:
                            int stat;
                            pid_t pid;
                            //因为已经是收到子进程结束信号，所以不需要阻塞，直接回收即可
                            while((pid = waitpid(-1, &stat, WNOHANG)) > 0){
                                //在池中标记子进程已经推迟这是pid为-1，使得轮询时跳过该子进程
                                for(int k = 0; k < m_process_number; ++k){
                                    if(pid == m_sub_process[k].m_pid){
                                        printf("child[%d] join\n", k);
                                        close(m_sub_process[k].m_pipefd[0]);
                                        m_sub_process[k].m_pid = -1;
                                        break;
                                    }
                                }
                            }
                            //若回收完了所有子进程，则父进程也退出
                            m_stop = true;
                            for(int k = 0; k < m_process_number; ++k){
                                if(m_sub_process[k].m_pid != -1){
                                    m_stop = false;
                                    break;
                                }
                            }
                            break;
                        case SIGINT:
                        case SIGTERM:
                            printf("kill all childern now\n");
                            for(int k = 0; k < m_process_number; ++k){
                                int pid = m_sub_process[k].m_pid; 
                                if(pid != -1){
                                    kill(pid, SIGTERM);
                                }
                            }
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
            else{//其他事件

            }
        }
    }

    close(m_epollfd);
    
    close(sig_pipefd[0]);
    close(sig_pipefd[1]);
}



#endif// PROCESSPOOL_H_  