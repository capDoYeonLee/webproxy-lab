#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

void doit(int clientfd);
void parse_uri(char *uri, char *hostname, char *port, char *path);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void read_requesthdrs(rio_t *request_rio, void *request_buf, int serverfd, char *hostname, char *port);

/* You won't lose style points for including this long line in your code */
static const int is_local_test = 1; // 테스트 환경에 따른 도메인 및 포트 지정을 위한 상수
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv) {
  //printf("%s", user_agent_hdr);
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE]; // MAXLINE = 8192
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

//    listenfd = Open_listenfd(argv[1]); // 전달받은 포트 번호를 사용해 수신 소켓 생성
//    while (1)
//    {
//        clientlen = sizeof(clientaddr);
//        clientfd = Malloc(sizeof(int));
//        *clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); // 클라이언트 연결 요청 수신
//        Getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
//        printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);
//        Pthread_create(&tid, NULL, thread, clientfd); // Concurrent 프록시
//    }

    listenfd = open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        printf("enter doit\n");
        doit(connfd);
        Close(connfd);
    }

    return 0;
}


void doit(int clientfd) {
    signal(SIGPIPE, SIG_IGN);

    int serverfd, content_length;
    char request_buf[MAXLINE]; // 클라의 요청 라인 또는 헤더 읽을 때 사용하는 버퍼
    char response_buf[MAXLINE]; // 서버한테 받은 응답 헤더를 읽고, 클라에게 전송할 때 사용하는 버퍼
    // MAXLINE은 왜 8KB(8192)로 초기화할까? -> Apache 기본 8kb(권장).
    char method[MAXLINE], uri[MAXLINE], path[MAXLINE], hostname[MAXLINE], port[MAXLINE];
    char *response_ptr, filename[MAXLINE], cgiargs[MAXLINE];
    rio_t request_rio; // 클라 소켓에서 요청을 읽을 때 사용
    rio_t response_rio; // 서버 소켓에서 응답을 읽을 때 사용


    /* 1️⃣ -1) Request Line 읽기 [ Client ->  Proxy] */
    Rio_readinitb(&request_rio, clientfd); //fd기반으로 rio_t 구조체가 어떤 파일 디스크립터에서 데이터를 읽을지 초기화
    Rio_readlineb(&request_rio, request_buf, MAXLINE);  // 클라의 요청을 읽고 request_buf에 저장한다.
    printf("Request headers:\n %s\n", request_buf);

    sscanf(request_buf, "%s %s", method, uri);
    parse_uri(uri, hostname, port, path);

    printf("change request buf setting\n");
    sprintf(request_buf, "%s %s %s\r\n", method, path, "HTTP/1.0");

    /* 1️⃣ -2) Request Line 전송 [ Proxy ->  Server] */
    printf("=====test bebug logging=====\n");
    printf("hostname: %s\n", hostname);
    printf("port: %s\n", port);
    serverfd = Open_clientfd(hostname, port); // proxy -> server connect 시도
    printf("serverfd is %d\n", serverfd);
    if (serverfd < 0) {
        clienterror(serverfd, method, "502", "Bad Gateway", "Failed to establish connection with the end server");
        return;
    }
    Rio_writen(serverfd, request_buf, strlen(request_buf));

    /* 2️⃣ Request Header 읽기 & 전송 [️ Client ->  Proxy ->  Server] */
    read_requesthdrs(&request_rio, request_buf, serverfd, hostname, port);
    printf("c -> p -> s %s %s %s\n", hostname, port, path);


    /* 3️⃣ Response Header 읽기 & 전송  Server ->  Proxy -> ️ Client] */
    Rio_readinitb(&response_rio, serverfd);
    while (strcmp(response_buf, "\r\n"))
    {
        Rio_readlineb(&response_rio, response_buf, MAXLINE);
        if (strstr(response_buf, "Content-length")) // Response Body 수신에 사용하기 위해 Content-length 저장
            content_length = atoi(strchr(response_buf, ':') + 1);
        Rio_writen(clientfd, response_buf, strlen(response_buf));
    }

    /* 4️⃣ Response Body 읽기 & 전송 [ Server ->  Proxy -> Client] */
    response_ptr = malloc(content_length);
    Rio_readnb(&response_rio, response_ptr, content_length);
    Rio_writen(clientfd, response_ptr, content_length); // Client에 Response Body 전송
    free(response_ptr); // 캐싱하지 않은 경우만 메모리 반환

    Close(serverfd);
}

// uri를 `hostname`, `port`, `path`로 파싱하는 함수
// uri 형태: `http://hostname:port/path` 혹은 `http://hostname/path` (port는 optional)
void parse_uri(char *uri, char *hostname, char *port, char *path)
{
    // host_name의 시작 위치 포인터: '//'가 있으면 //뒤(ptr+2)부터, 없으면 uri 처음부터
    char *hostname_ptr = strstr(uri, "//") ? strstr(uri, "//") + 2 : uri;
    char *port_ptr = strchr(hostname_ptr, ':'); // port 시작 위치 (없으면 NULL)
    char *path_ptr = strchr(hostname_ptr, '/'); // path 시작 위치 (없으면 NULL)
    strcpy(path, path_ptr);

    if (port_ptr) // port 있는 경우
    {
        strncpy(port, port_ptr + 1, path_ptr - port_ptr - 1);
        strncpy(hostname, hostname_ptr, port_ptr - hostname_ptr);
    }
    else // port 없는 경우
    {
        if (is_local_test)
            strcpy(port, "80"); // port의 기본 값인 80으로 설정
        else
            strcpy(port, "8000");
        strncpy(hostname, hostname_ptr, path_ptr - hostname_ptr);
    }
}



// 클라이언트에 에러를 전송하는 함수(cause: 오류 원인, errnum: 오류 번호, shortmsg: 짧은 오류 메시지, longmsg: 긴 오류 메시지)
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];

    // 에러 Bdoy 생성
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor="
                  "ffffff"
                  ">\r\n",
            body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    // 에러 Header 생성 & 전송
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));

    // 에러 Body 전송
    Rio_writen(fd, body, strlen(body));
}


void read_requesthdrs(rio_t *request_rio, void *request_buf, int serverfd, char *hostname, char *port) {
    int is_host_exist;
    int is_connection_exist;
    int is_proxy_connection_exist;
    int is_user_agent_exist;

    Rio_readlineb(request_rio, request_buf, MAXLINE); // 첫번째 줄 읽기
    while (strcmp(request_buf, "\r\n"))
    {
        if (strstr(request_buf, "Proxy-Connection") != NULL)
        {
            sprintf(request_buf, "Proxy-Connection: close\r\n");
            is_proxy_connection_exist = 1;
        }
        else if (strstr(request_buf, "Connection") != NULL)
        {
            sprintf(request_buf, "Connection: close\r\n");
            is_connection_exist = 1;
        }
        else if (strstr(request_buf, "User-Agent") != NULL)
        {
            sprintf(request_buf, user_agent_hdr);
            is_user_agent_exist = 1;
        }
        else if (strstr(request_buf, "Host") != NULL)
        {
            is_host_exist = 1;
        }

        Rio_writen(serverfd, request_buf, strlen(request_buf)); // Server에 전송
        Rio_readlineb(request_rio, request_buf, MAXLINE);       // 다음 줄 읽기
    }

    // 필수 헤더 미포함 시 추가로 전송
    if (!is_proxy_connection_exist)
    {
        sprintf(request_buf, "Proxy-Connection: close\r\n");
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }
    if (!is_connection_exist)
    {
        sprintf(request_buf, "Connection: close\r\n");
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }
    if (!is_host_exist)
    {
        sprintf(request_buf, "Host: %s:%s\r\n", hostname, port);
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }
    if (!is_user_agent_exist)
    {
        sprintf(request_buf, user_agent_hdr);
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }

    sprintf(request_buf, "\r\n"); // 종료문
    Rio_writen(serverfd, request_buf, strlen(request_buf));
    return;
}