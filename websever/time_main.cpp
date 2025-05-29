#include<string.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include<cstdio>
#include<signal.h>
#include<assert.h>
#include"http_conn.h"
#include"locker.h"
#include"threadpool.h"
#include"noactive/lst_timer.h"

#define MAX_FD 65535 //文件描述符最大数
#define MAX_EVENT_NUMBER 10000 //监听最大

#define TIMESLOT 5

static int pipefd[2];
static sort_timer_lst timer_lst;

extern void setnonblocking(int fd);
//添加文件描述符到epoll中
extern int addfd(int epollfd,int fd,bool one_host);

//从epoll中删除文件描述符
extern int removefd(int epollfd, int fd);

//添加信号捕捉
void addsig(int sig,void(handler)(int)){
    struct sigaction sa;
    memset(&sa,'\0', sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);//处理该信号时，屏蔽所有其他信号，防止处理函数被打断。
    assert( sigaction( sig, &sa, NULL ) != -1 );//类似QT的connect

}

void sig_handler( int sig )//回调函数 用于响应定时信号
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( HttpConn* user_data )
{
    printf( "close fd %d\n", user_data->m_sockfd );
    user_data->close_conn();
    
}

void addfd_pipe( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );

    //设置文件描述符为非阻塞
    setnonblocking(fd);
}


int main(int argc,char* argv[]){
    if(argc <= 1){
        printf("按照如下格式运行:%s port_number\n",argv[0]);
        exit(-1);
    }

    //获取端口号
    int port= atoi(argv[1]);

    //sigpie信号处理
    addsig(SIGPIPE,SIG_IGN);//忽略断开连接信号

    //创建监听的套接字
    int listenfd = socket(PF_INET,SOCK_STREAM,0);//PF_INET:ipv4,SOCK_STREAM :TCP

    //设置端口复用 可以再Timewait 立即重复绑定
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family =AF_INET;
    address.sin_addr.s_addr=INADDR_ANY; 
    address.sin_port = htons(port);//转换大端小端 并指定端口号
    if(bind(listenfd,(struct sockaddr*)&address,sizeof(address))==-1){
        printf("bind failure\n");
        exit(EXIT_FAILURE);      // 使用标准退出码
    }

    //监听
    if (listen(listenfd, 5) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
//---------------------------------------------------------创建socket ->bind ->listen--------------------------
    ThreadPool<HttpConn> *pool=NULL;

    try{
        pool=new ThreadPool<HttpConn> ();
    }catch(...){
        exit(-1);
    }

    //创建一个数组用于保存所有的客户端信息
    HttpConn * users = new HttpConn[MAX_FD]; 

    //创建epoll对象,事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    //套接字listenfd在epoll上注册读就绪事件
    addfd(epollfd,listenfd,false);

    //连接也记录一下epoll
    HttpConn::m_epollfd=epollfd;

    // 创建信号管道
    if(socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd)==-1){
        printf("create pipefd failure\n");
        exit(EXIT_FAILURE);      // 使用标准退出码
    }
    setnonblocking( pipefd[1] );
    addfd_pipe( epollfd, pipefd[0] );

    // 设置信号处理函数
    addsig( SIGALRM ,sig_handler);
    addsig( SIGTERM ,sig_handler);
    bool stop_server = false;
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while(true){
        
        int num =epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);//循环监听事件并返回个数
        if((num<0)&&(errno != EINTR)){
            printf("epoll failure\n");
            break;
        }
        //循环遍历事件数组
        for(int i=0;i<num;i++){
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd){//是否是监听套接字（listenfd）上的连接请求事件   listenfd监听连接请求
                //有客户端连接进来
                sockaddr_in client_address;
                socklen_t client_addrlen =sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlen);//建立连接，生成双方通讯的新的套接字connfd

                if(HttpConn::m_user_count >=MAX_FD){
                    //目前连接数满了
                    //给客户端写一个信息：服务器内部正忙
                    close(connfd);
                    continue;
                }
                //将新的客户的数据与套接字初始化放到数组里
                users[connfd].init(connfd,client_address);

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer();
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );
                // printf("==========%d\n",connfd);
                // printf("=====00=====%d\n",users[connfd].m_sockfd);

            }else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 处理信号
                int sig;
                char signals[1024];
                int ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }else if(events[i].events &(EPOLLRDHUP|EPOLLHUP|EPOLLERR)){
                //对方异常断开或者错误等事件
                printf("对方异常断开或者错误等事件\n");
                // 关闭连接，并移除对应的定时器。
                cb_func( &(users[sockfd]));
                util_timer* timer = users[sockfd].timer;
                if( timer ){
                    timer_lst.del_timer( timer );
                }
            }else if(events[i].events &EPOLLIN){//客户端写入数据
                // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                util_timer* timer = users[sockfd].timer;
                if( timer ) {
                    time_t cur = time( NULL );
                    timer->expire = cur + 3 * TIMESLOT;
                    printf( "adjust timer once\n" );
                    timer_lst.adjust_timer( timer );
                }
                if(users[sockfd].read()){
                    //一次性把所有数据读完
                    pool->append(&users[sockfd]);
                }else{
                    cb_func( &(users[sockfd]) );
                }
            }else if(events[i].events &EPOLLOUT){//客户端请求数据
                // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                util_timer* timer = users[sockfd].timer;
                if( timer ) {
                    time_t cur = time( NULL );
                    timer->expire = cur + 3 * TIMESLOT;
                    printf( "adjust timer once\n" );
                    timer_lst.adjust_timer( timer );
                }
                // printf("服务器返回数据\n");
                if(!users[sockfd].write()){
                    cb_func( &(users[sockfd]) );
                }
            }
        }

        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;
    return 0;

}