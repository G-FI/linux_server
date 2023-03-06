进程池使用单例模式，`processpool`使用静态成员函数`creat()`函数返回单一实例，pool有两个接口供用户使用`run()`和`create()`
1. 主要思路：
   1. 创建：池创建时，`fork()`出输入数量的子进程，并用`process类数组`来表示子进程的信息(包括pid让父进程知道，pipefd[2]用socketpair创建，用于父子进程间通信父子进程分别关闭一端)
   2. 运行：`run()`根据m_idx判断，分别运行父子进程的处理逻辑:**半同步/半异步**并发模式，父进程用于监听客户端是否有连接到来，如果有：两步：1.选择一个子进程(使用round Robin) 2.通过pipefd来向子进程通知有客户端连接到来(传输的消息只是标记而已，无实意)，子进程监听到父进程的消息时，说明有连接到来，子进程就`accept()`接受连接
   3. 父子进程都注册内核事件表进行I/O复用，统一事件源，**文件描述符都设置为非阻塞**
      1. 父进程事件：
         1. listenfd客户端连接请求-> 选择+通知子进程
         2. 信号处理-> SIGCHILD回收子进程 + SIGTERM/SIGINT时先终止池中的子进程kill发送信号
      2. 子进程事件：
         1. 父进程pipefd-> accept客户端请求+将connfd添加到内核监听时间表中，初始化对应的**处理类接口T/UserHandler**
         2. 信号处理->SIGCHILD回收子进程(对于CGI程序，子进程会fork出子进程并调用用户指定的CGI程序，当程序结束后子进程应该回收它) + SIGTERM/SIGINT时终止事件处理循环
         3. connfd客户端发来数据-> 交给T类型(或UserHandler)处理，即调用它的`process()`进行处理

2. "框架的用户"即T类型，当处理完用户数据之后，`recv()`出错/返回0时由**用户T将connfd移除内核注册表**(因为不再需要监听它了)