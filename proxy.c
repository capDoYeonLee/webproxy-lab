#include <stdio.h>
#include "csapp.h"
#include "cache.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

//typedef struct cache_entry{
//    char uri[MAXLINE];
//    char *object;
//    int size;
//    struct cache_entry *next;
//    struct cache_entry *prev;
//}cache_entry;
//
//typedef struct {
//    cache_entry *head;
//    cache_entry *tail;
//    int total_size;
//    pthread_rwlock_t lock;
//}cache_list;
//
//cache_list cache;

void doit(int clientfd);
void *thread(void *vargp);
void parse_uri(char *uri, char *hostname, char *port, char *path);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void read_requesthdrs(rio_t *rp, char *host_header, char *other_header);
void reassemble(char *req, char *path, char *hostname, char *other_header);

//void cache_init();
//void cache_move_to_end(cache_list *cache, cache_entry *node);
//void cache_evict(cache_list *cache, int size_needed);
//void cache_insert(cache_list *cache, const char *uri, const char *object, int size);
//int cache_find(cache_list *cache, const char *uri, char *object_buf, int *size_buf);

/* You won't lose style points for including this long line in your code */
static const int is_local_test = 1; // 테스트 환경에 따른 도메인 및 포트 지정을 위한 상수
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv)
{
    int listenfd, *clientfd;
    char client_hostname[MAXLINE], client_port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    signal(SIGPIPE, SIG_IGN); // SIGPIPE 예외처리

    //rootp = (web_object_t *)calloc(1, sizeof(web_object_t));
    //lastp = (web_object_t *)calloc(1, sizeof(web_object_t));

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]); // 전달받은 포트 번호를 사용해 수신 소켓 생성
    init_cache();

    while (1)
    {
        clientlen = sizeof(clientaddr);
        clientfd = Malloc(sizeof(int));
        *clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); // 클라이언트 연결 요청 수신
        Getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);
        Pthread_create(&tid, NULL, thread, clientfd); // Concurrent 프록시
    }
}


void *thread(void *vargp) {
    int clientfd = *((int*)vargp);
    Pthread_detach(pthread_self()); // 새로운 thread 생성 시 언제 반환해 줄지 고민해 볼 것.
    Free(vargp);

    printf("enter doit function\n");
    doit(clientfd);
    Close(clientfd);
    return NULL;
}

void doit(int clientfd) {

    char host_header[MAXLINE], other_header[MAXLINE];
    int serverfd, content_length;
    char request_buf[MAXLINE]; // 클라의 요청 라인 또는 헤더 읽을 때 사용하는 버퍼
    char response_buf[MAXLINE]; // 서버한테 받은 응답 헤더를 읽고, 클라에게 전송할 때 사용하는 버퍼
    // MAXLINE은 왜 8KB(8192)로 초기화할까? -> Apache 기본 8kb(권장).
    char method[MAXLINE], uri[MAXLINE], path[MAXLINE], hostname[MAXLINE], port[MAXLINE], version[MAXLINE];
    char *response_ptr, filename[MAXLINE], cgiargs[MAXLINE];
    rio_t request_rio; // 클라 소켓에서 요청을 읽을 때 사용
    rio_t response_rio; // 서버 소켓에서 응답을 읽을 때 사용



    /* 1️⃣ -1) Request Line 읽기 [ Client ->  Proxy] */
    printf("1️⃣ -1) Request Line 읽기 [ Client ->  Proxy]");
    Rio_readinitb(&request_rio, clientfd); //fd기반으로 rio_t 구조체가 어떤 파일 디스크립터에서 데이터를 읽을지 초기화
    Rio_readlineb(&request_rio, request_buf, MAXLINE);  // 클라의 요청을 읽고 request_buf에 저장한다.
    printf("Request headers:\n %s\n", request_buf);
    sscanf(request_buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET") != 0){
        clienterror(clientfd, method, "501", "Not implemented", "This Server does not implement this method");
        return;
    }

    read_requesthdrs(&request_rio, host_header, other_header);
    parse_uri(uri, hostname, port, path);

    // proxy cache 우선 탐색
    char cache_buf[MAX_OBJECT_SIZE];
    int cache_size;

    // proxy cache hit
    if (find_cache(&cache, uri, cache_buf, &cache_size)) {
        printf("proxy cache hit!!\n");
        Rio_writen(clientfd, cache_buf, cache_size);
        return;
    }

    printf("proxy cache miss\n!!");
    /* 1️⃣ -2) Request Line 전송 [ Proxy ->  Server] */
    serverfd = Open_clientfd(hostname, port);
    if (serverfd < 0) {
        printf("enter client error\n");
        clienterror(serverfd, method, "502", "Bad Gateway", "Failed to establish connection with the end server");
        return;
    }

    // proxy -> server
    reassemble(request_buf, path, hostname, other_header);
    Rio_writen(serverfd, request_buf, strlen(request_buf));

    int total_size = 0;
    char temp_cache[MAX_OBJECT_SIZE];
    rio_t server_rio;
    Rio_readinitb(&server_rio, serverfd);
    ssize_t n;

    // server -> proxy
    while ((n = Rio_readnb(&server_rio, response_buf, MAXLINE)) > 0) {
      Rio_writen(clientfd, response_buf, n);
      if (total_size + n <= MAX_OBJECT_SIZE) {
          memcpy(temp_cache + total_size, response_buf, n);
      }
      total_size += n;
    }
    Close(serverfd);

    if (total_size <= MAX_OBJECT_SIZE) {
        printf("insert proxy cache\n");
        insert_cache(&cache, uri, temp_cache, total_size);
    }
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


void read_requesthdrs(rio_t *rp, char *host_header, char *other_header){
    char buf[MAXLINE];
    host_header[0] = '\0';
    other_header[0] = '\0';

    while(Rio_readlineb(rp, buf, MAXLINE) > 0 && strcmp(buf, "\r\n")){
        if (!strncasecmp(buf, "Host:", 5)){
            strcpy(host_header, buf);
        }
        else if (!strncasecmp(buf, "User-Agent:", 11) || !strncasecmp(buf, "Connection:", 11) || !strncasecmp(buf, "Proxy-Connection:", 17)) {
            continue;  // 무시
        }
        else{
            strcat(other_header, buf);
        }
    }
}

void reassemble(char *req, char *path, char *hostname, char *other_header){
    sprintf(req,
      "GET %s HTTP/1.0\r\n"
      "Host: %s\r\n"
      "%s"
      "Connection: close\r\n"
      "Proxy-Connection: close\r\n"
      "%s"
      "\r\n",
      path,
      hostname,
      user_agent_hdr,
      other_header
    );
}

void init_cache() {
    cache.head = NULL;
    cache.tail = NULL;
    cache.total_size = 0;
    pthread_rwlock_init(&cache.lock, NULL);
}

/* 최근 사용된 entry를 cache list 내부 제일 끝(tail)으로 이동시키는 함수*/
void move_cache_to_end(cache_list *cache, cache_entry *entry) {
    if (cache->tail == entry) return; // entry가 이미 가장 최근에 사용된 엔트리라면 이동할 필요 없으니 바로 return

    /* 기존 위치에서 최근 사용된 entry를 옮기기 위해서 cache list에서 떼어내는 과정*/
    if (entry -> prev) {
        entry->prev->next = entry->next;
    } else {
        cache->head = entry->next; // entry가 head였다면 head 갱신
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache->tail = entry->prev;
    }

    /* 최근 사용된 entry를 tail로 연결*/
    entry->prev = cache->tail;
    entry->next = NULL;

    if (cache->tail) {
        cache->tail->next = entry;
    } else {
        cache->head = entry;
    }
    cache->tail = entry;
}

/* 캐시에서 오래된 데이터를 쫓아내는(evict)함수*/
void remove_cache(cache_list *cache, int size_needed) {

    while (cache->total_size + size_needed > MAX_CACHE_SIZE) {
        //캐시가 비어있으면 즉시 종료
        if (cache->head == NULL) return;

        // 가장 오래된 캐시 entry 제거
        cache_entry *old_entry = cache->head;
        cache->head = cache->head->next;
        if (cache->head) {
            cache->head->prev = NULL;
        } else {
            cache->tail = NULL;
        }

        cache->total_size -= old_entry->size;
        free(old_entry->object);
        free(old_entry);
    }
}

/* proxy cache miss 상황에서 서버에서 생성한 응답을 proxy cache에 저장하는 함수 */
void insert_cache(cache_list *cache, const char *uri, const char *object, int size) {
    if (size > MAX_CACHE_SIZE) return;

    pthread_rwlock_wrlock(&cache->lock); // write lock 획득

    remove_cache(cache, size); // 캐시 공간 확보

    cache_entry *new_entry = malloc(sizeof(cache_entry));  // 새로운 캐시 엔트리 메모리 할당
    if (!new_entry){
        pthread_rwlock_unlock(&cache->lock);
        return;
    }

    //URI 복사
    strncpy(new_entry->uri, uri, MAXLINE - 1);
    new_entry->uri[MAXLINE - 1] = '\0';

    // object 복사
    new_entry->object = malloc(size);
    if (!new_entry->object){
        free(new_entry);
        pthread_rwlock_unlock(&cache->lock);
        return;
    }
    memcpy(new_entry->object, object, size);

    // entry 정보 설정(크기, 포인터)
    new_entry->size = size;
    new_entry->prev = cache->tail;
    new_entry->next = NULL;

    // cache list에 cache entry 삽입
    if (cache->tail) {
        cache->tail->next = new_entry;
    } else {
        cache->head = new_entry;
    }
    cache->tail = new_entry;
    cache->total_size += size;

    pthread_rwlock_unlock(&cache->lock);
}

/* 주어진 URI를 통해 해당 데이터를 proxy 캐시에서 검색하고 반환하는 함수 */
int find_cache(cache_list *cache, const char *uri, char *object_buf, int *size_buf) {

    pthread_rwlock_rdlock(&cache->lock); // read lock 획득

    cache_entry *entry = cache->head;
    while (entry) {
        // URI 검색
        if (strcmp(entry->uri, uri) == 0) {
            // cache hit
            // 처음에는 읽기 락이었지만, cache hit 경우 -> move_cache_to_end write 작업이 발생함에 따라 write lock 획득
            pthread_rwlock_unlock(&cache->lock);
            pthread_rwlock_wrlock(&cache->lock);

            // LRU algoritms
            move_cache_to_end(cache, entry);

            // 결과 복사 및 크기 설정
            memcpy(object_buf, entry->object, entry->size);
            *size_buf = entry->size;

            // write lock 반환
            pthread_rwlock_unlock(&cache->lock);
            return 1;
        }
        entry = entry->next;
    }
    pthread_rwlock_unlock(&cache->lock);
    return 0;
}


























