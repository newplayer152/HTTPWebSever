#include"http_conn.h"
// char httpVarsion[9]="HTTP/1.0";//压力测试改这里 改成 “HHTTP/1.0”

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
const char * doc_root="/home/user/Linux/websever/resource";

int HttpConn::m_epollfd=-1; //所有socket上的事件都被注册到同一个epoll上
int HttpConn::m_user_count=0;//统计用户数量

//设置文件描述符为非阻塞
void setnonblocking(int fd){
    int old_flag = fcntl(fd,F_GETFL);
    int new_lag=old_flag|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_lag);
}

//向epoll中添加需要监听的文件描述符 客户端的连接
void addfd(int epollfd,int fd,bool one_host){

    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN | EPOLLRDHUP;
    //event.events=EPOLLIN |EPOLLET| EPOLLRDHUP;//改成ET模式边缘触发

    if(one_host){
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);  

}

//向epoll中删除文件描述符
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//重置epolloneshot事件确保下一次可读事件
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
    
}

//初始化新的套接字
void HttpConn::init(int sockfd,const sockaddr_in & addr){
    m_sockfd =sockfd;
    m_address=addr;

    int reuse =1;
    setsockopt (m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}

//初始化连接
void HttpConn::init(){
    m_read_indx=0;
    m_checked_index=0;//当前分析中的偏移量
    m_start_line=0;//当前分析行
    m_check_state=CHECK_STATE_REQUESTLINE;//主状态机当前所处的状态

    m_url=0;
    m_version=0;
    m_method=GET;
    m_content_length=0;
    m_host = 0;
    m_write_idx = 0;

    bytes_have_send=0;
    bytes_to_send=0;
    m_linger=false;

    
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

}

/// 关闭连接
void HttpConn::close_conn(){
    //printf( "close fd %d\n", m_sockfd );
    if(m_sockfd != -1){
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;
    }
}

//循环读数据直到没数据可读或关闭连接
bool HttpConn::read(){

    if(m_read_indx>=READ_BUFFER_SIZE){
        printf("m_read_indx>=READ_BUFFER_SIZE\n");
        return false;
    }

    //读到的字节
    int bytes_read=0;
    while(true){
        bytes_read =recv(m_sockfd,m_read_buf+m_read_indx,READ_BUFFER_SIZE-m_read_indx,0);
        
        //printf("%d\n",bytes_read);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;//没有数据了
            }
            return false;
        }else if(bytes_read == 0){
            printf("对端关闭连接\n");
            //对端关闭连接
            return false;
        }
        m_read_indx += bytes_read;
        
    }
    //printf("读到的数据:\n%s ",m_read_buf);

    return true;


};

bool HttpConn::write(){

    int temp = 0;

    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        //printf("将要发送的字节为0，这一次响应结束\n");
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

 
    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);//temp表示已经发送的字节数
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;

        if(bytes_have_send>=m_iv[0].iov_len){
            //大头部已经发送完毕
            m_iv[0].iov_len=0;
            m_iv[1].iov_base=m_file_address+(bytes_have_send-m_write_idx);
            m_iv[1].iov_len=bytes_to_send;
        }else{
            m_iv[0].iov_base=m_write_buf + bytes_have_send;
            m_iv[0].iov_len=m_iv[0].iov_len-temp;
        }

        if ( bytes_to_send <= 0 ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
    
};


HttpConn::HTTP_CODE HttpConn::process_read(){//解析HTTP请求

    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret =NO_REQUEST;

    char *text =0;

    while(((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))
        ||((line_status=parse_line())==LINE_OK)){
            
            text =get_line();
            m_start_line = m_checked_index;
            //printf("got 1 http line : %s\n",text);

            switch(m_check_state){
                case CHECK_STATE_REQUESTLINE:
                {
                    ret = parse_request_line(text);
                    if(ret==BAD_REQUEST){
                        return BAD_REQUEST;
                    }
                    break;
                }
                case CHECK_STATE_HEADER:
                {
                    ret = parse_headers(text);
                    if(ret==BAD_REQUEST){
                        return BAD_REQUEST;
                    }else if(ret == GET_REQUEST){
                        return do_request();
                    }
                    break;
                }
                case CHECK_STATE_CONTENT:
                {
                    ret = parse_content(text);
                    if(ret==GET_REQUEST){
                        return do_request();
                    }
                    line_status=LINE_OPEN;
                    break;
                }
                default:
                {
                    return INTERNAL_ERROR;
                }
            }



    }


    return NO_REQUEST;


};

//解析HTTP请求 行
HttpConn::HTTP_CODE HttpConn::parse_request_line(char *text){
     // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); //判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp( m_version, m_version) != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;

};

//解析HTTP请求 头
HttpConn::HTTP_CODE HttpConn::parse_headers(char *text){

    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;

};

//解析HTTP请求 体
HttpConn::HTTP_CODE HttpConn::parse_content(char *text){

     if ( m_read_indx >= ( m_content_length + m_checked_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;


};

//解析行
HttpConn::LINE_STATUS HttpConn::parse_line(){
    char temp;
    for ( ; m_checked_index < m_read_indx; ++m_checked_index ) {
        temp = m_read_buf[ m_checked_index ];
        if ( temp == '\r' ) {
            if ( ( m_checked_index + 1 ) == m_read_indx ) {//没有\r结尾的
                return LINE_OPEN;
            } else if ( m_read_buf[ m_checked_index + 1 ] == '\n' ) {//正常结尾\r\n
                m_read_buf[ m_checked_index++ ] = '\0';
                m_read_buf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            if( ( m_checked_index > 1) && ( m_read_buf[ m_checked_index - 1 ] == '\r' ) ) {//考虑数据被截断的情况
                m_read_buf[ m_checked_index-1 ] = '\0';
                m_read_buf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;

};

//做响应函数
HttpConn::HTTP_CODE HttpConn::do_request(){

    // "/home/nowcoder/webserver/resources"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    //printf("\n%ld\n",m_file_stat.st_size);
    close( fd );
    return FILE_REQUEST;


};

// 对内存映射区执行munmap操作
void HttpConn::unmap() {
    if( m_file_address ){
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 往写缓冲中写入待发送的数据
bool HttpConn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );

    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;//只记录状态行、响应头、响应空行(不包含响应体)
    va_end( arg_list );
    return true;
}

bool HttpConn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", m_version, status, title );
}

bool HttpConn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool HttpConn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool HttpConn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool HttpConn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool HttpConn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool HttpConn::add_content_type() {
    return add_response("Content-Type:%s\r\n", " text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool HttpConn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            bytes_to_send=m_write_idx+m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

/// 关闭连接
void HttpConn::run(){
    //解析http请求
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST){
        modfd(m_epollfd , m_sockfd , EPOLLIN);//重新重置epolloneshot事件确保下一次可读事件
        return;
    }
    printf("parse request,create response\n");

     // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);

}