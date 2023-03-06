#include "processpool.h"

#include<unistd.h>
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

#include"processpool.h"


class cgi_conn{
public:
    cgi_conn(){}
    ~cgi_conn(){}

    void init(int epollfd, int connfd, sockaddr *addr){
        m_epollfd = epollfd;
        m_connfd = connfd;
        m_addr = addr;

        memset(m_buf, '\0', BUFFER_SIZE);
        m_read_idx = 0;
    }

    void process(){
    //保证process结束之后这个客户端已经服务完成，所以process调用结束后
    //从内核事件表中移除connfd的逻辑放到pool的逻辑中
        int idx = 0; 
        int ret = -1;
        while(true){
            idx = m_read_idx;
            ret = recv(m_connfd, m_buf + idx, BUFFER_SIZE-1-idx, 0);
            //出错  如果是由于非阻塞，暂时没得到数据,则继续读取
            if(ret == -1){
                if(errno != EAGAIN)
                    continue;
                    //removefd(m_epollfd, m_connfd);
                break;
            }
            //连接结束
            else if(ret == 0){
                // removefd(m_epollfd, m_connfd);
                break;
            }
            m_read_idx = ret + m_read_idx;
            //当读到\n时结束
            for(; idx < m_read_idx; ++idx){
                if(m_buf[idx] =='\n'){
                    break;
                }
            }
            //还没有读到\r\n，表示用户数据还没发送完
            if(idx == m_read_idx)
                continue;
            
            m_buf[idx] = '\0';
            printf("user content: %s\n", m_buf);

            //m_buf中为用户请求的CGI程序字符串
            //检查CGI程序是否存在
            ret = access(m_buf, F_OK);
            if(ret == -1){
                printf("[server process %d]: no cgi program \"%s\"\n", getpid(), m_buf);
                removefd(m_epollfd, m_connfd);
                break;
            }
            
            int pid = fork();
            //父进程关闭连接
            if(pid > 0){
                // removefd(m_epollfd, m_connfd);
                break;
            }
            //子进程执行CGI程序
            else{
                //CGI程序输出重定向到socket连接中
                close(STDOUT_FILENO);
                dup(m_connfd);
                execl(m_buf, m_buf, nullptr);
                exit(0);
            }

        }
    }
private:
    static const int BUFFER_SIZE =1024;

    static int m_epollfd;
    int m_connfd;
    sockaddr* m_addr;
    int m_read_idx; 
    char m_buf[BUFFER_SIZE];
};
int cgi_conn::m_epollfd = -1;


int main(int argc, char** argv){
    if(argc <= 2){
        printf("usage: %s ip_address port_number", argv[0]);
        exit(-1);
    }

    char* ip = argv[1];
    short port = atoi(argv[2]);
    int backlog = 5;

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);


    sockaddr_in serveraddr;
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(port);
    inet_pton(AF_INET, ip, (void*)&serveraddr.sin_addr);

    int ret = bind(listenfd, (sockaddr* )&serveraddr, sizeof(serveraddr));
    assert(ret != -1);

    ret = listen(listenfd, backlog);
    assert(ret != -1);


    processpool<cgi_conn>* ppool = processpool<cgi_conn>::create(listenfd, 8);

    ppool->run();
}