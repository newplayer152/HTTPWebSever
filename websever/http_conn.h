#ifndef HTTP_CONNT_H
#define HTTP_CONNT_H

#include<sys/epoll.h>
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/stat.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <stdarg.h>
#include<sys/uio.h>
#include"noactive/lst_timer.h"

class HttpConn{
    
public:
     static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static int m_epollfd; //所有socket上的事件都被注册到同一个epoll上
    static int m_user_count;//统计用户数量
    static const int READ_BUFFER_SIZE=2048; //缓冲区大小
    static const int WRITE_BUFFER_SIZE=2048;//缓冲区大小

    HttpConn(){};
    ~HttpConn(){};


    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };


    void run();
    void init(int sockfd,const sockaddr_in & addr);//初始化新接受的
    void init();//初始化连接
    void close_conn();//关闭连接

    bool read();//非阻塞的
    bool write();//非阻塞的


    int m_sockfd;//该http连接的socket


private:
    
    sockaddr_in m_address;//通信的socket的地址

    char m_read_buf[READ_BUFFER_SIZE];//读缓冲区
    int m_read_indx;//读偏移量 ：读到数据的结尾
    int m_checked_index;//当前分析中的偏移量
    int m_start_line;//当前分析行
    char m_real_file[ FILENAME_LEN ];       // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录

    //相关字段
    char *m_url;//请求文件名
    char *m_version;//协议版本
    METHOD m_method;//请求方法
    char * m_host;//主机名
    bool m_linger;//http是否保持连接
    int m_content_length;//是否有请求体

    CHECK_STATE m_check_state;//主状态机当前所处的状态



    
    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

    int bytes_to_send;//将要发送字节数
    int bytes_have_send;//已经发送字节数

    
    HTTP_CODE process_read();//解析HTTP请求
    HTTP_CODE parse_request_line(char *text);//解析HTTP请求 行
    HTTP_CODE parse_headers(char *text);//解析HTTP请求 头
    HTTP_CODE parse_content(char *text);//解析HTTP请求 体


    LINE_STATUS parse_line();//解析行
    char *get_line(){return m_read_buf+m_start_line;}
    HTTP_CODE do_request();


    bool process_write( HTTP_CODE ret );    // 填充HTTP应答
    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();


public://定时器相关成员变量
    util_timer* timer;          // 定时器

};


#endif
