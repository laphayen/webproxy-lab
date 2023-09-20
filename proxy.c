#include <stdio.h>

#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

// LRU(Least Recently Used) 알고리즘 - 가장 오랫동안 참조되지 않은 페이지를 교체하는 기법
#define LRU_MAGIC_NUMBER 9999

#define CACHE_OBJS_COUNT 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

static const char *requestline_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_key = "Host";
static const char *connection_key = "Connection";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *user_agent_key = "User-Agent";

void *thread(void *vargsp);
void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_endServer(char *hostname, int port, char *http_header);

// cache function
void cache_init();
int cache_find(char *url);
void cache_uri(char *uri, char *buf);

void readerPre(int i);
void readerAfter(int i);

typedef struct
{
  char cache_obj[MAX_OBJECT_SIZE];
  char cache_url[MAXLINE];
  int LRU;
  int isEmpty;

  int readCnt;
  sem_t wmutex;
  sem_t rdcntmutex;
}cache_block;


typedef struct
{
  cache_block cacheobjs[CACHE_OBJS_COUNT];
  int cache_num;
}Cache;

Cache cache;

int main(int argc, char **argv) {
  // 프록시 듣기 식별자, 프록시 연결 식별자
  int listenfd, connfd;
  // 클라이언트에게 받은 uil 정보를 담을 공간
  char hostname[MAXLINE], port[MAXLINE];
  // 소켓 길이를 저장할 구조체
  socklen_t clientlen;
  pthread_t tid;
  // 소켓 구조체 - clientaddress
  struct sockaddr_storage clientaddr;

  cache_init();

  /* Check command line args */
  // 명령줄 인수를 확인하여 서버가 사용할 포트 번호를 결정
  // 포트 번호를 받지 않으면 사용법을 출력하고 프로그램을 종료
  // 입력인자가 2개인지 확인
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port> \n", argv[0]);
    exit(1);
  }

  Signal(SIGPIPE, SIG_IGN); 

  // 서버 소켓을 연다.
  // 지정된 포트 번호에서 클라이언트의 연결을 수신하기 위한 소켓 생성 후 반환
  listenfd = Open_listenfd(argv[1]);

  // 웹 서버의 핵심 로직
  // 무한 루프를 실행하여 클라이언트의 연결을 수락하고 처리
  while (1) {
    clientlen = sizeof(clientaddr);
    // 클라이언트의 연결을 수락
    // 수락된 연결 소켓 connfd를 반환한다.
    // 소켓 어드레스(SA) - 포트 번호는 서버가 정하고 있고 사용자가 주소를 입력 시 가져와서 비교 후 수락을 시도한다. 
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);  // line:netp:tiny:accept
    // Getnameinfo를 호출하여 클라이언트의 IP 주소를
    // 호스트 이름과 포트 번호로 변환하고, 호스트 이름과 포트 번호를 출력
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s %s).\n", hostname, port);

    // 쓰레드 식별자, 쓰레드 특성, 쓰레드 함수, 쓰레드 함수 매개변수
    Pthread_create(&tid, NULL, thread, (void *)connfd);
  }
  return 0;
}

void *thread(void *vargsp) {
  int connfd = (int)vargsp;
  Pthread_detach(pthread_self());
  doit(connfd);
  Close(connfd);
}

void doit(int connfd) {
  int end_serverfd;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char endserver_http_header[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE];
  int port;
  // 입출력 버퍼 초기화
  rio_t rio;
  rio_t server_rio;

  // 클라이언트와의 통신을 위한 소켓 파일 디스크립터를 받는다. 
  Rio_readinitb(&rio, connfd);
  // 요청 라인을 읽고 HTTP 요청 메소드, URI, 버전을 파싱한다.
  Rio_readlineb(&rio, buf, MAXLINE);

  // 클라이언트의 요청 라인을 읽는다.
  sscanf(buf, "%s %s %s", method, uri, version);

  if (strcasecmp(method, "GET")) {
    printf("Proxy does not implement the method");
    return;
  }

  char url_store[100];

  strcpy(url_store, uri);

  int cache_index;
  if ((cache_index=cache_find(url_store)) != -1)
  {
    readerPre(cache_index);
    Rio_writen(connfd, cache.cacheobjs[cache_index].cache_obj, strlen(cache.cacheobjs[cache_index].cache_obj));
    readerAfter(cache_index);
    return;
  }
  
  parse_uri(uri, hostname, path, &port);

  build_http_header(endserver_http_header, hostname, path, port, &rio);

  end_serverfd = connect_endServer(hostname, port, endserver_http_header);
  if (end_serverfd < 0)
  {
    printf("connection failed\n");
    return;
  }

  Rio_readinitb(&server_rio, end_serverfd);

  Rio_writen(end_serverfd, endserver_http_header, strlen(endserver_http_header));

  char cachebuf[MAX_OBJECT_SIZE];
  int sizebuf = 0;
  size_t n;
  while ((n=Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
  {
    sizebuf += n;
    if (sizebuf < MAX_OBJECT_SIZE)
      strcat(cachebuf, buf);
    Rio_writen(connfd, buf, n);
  }
  Close(end_serverfd);

  // store it
  if (sizebuf < MAX_OBJECT_SIZE)
  {
    cache_uri(url_store, cachebuf);
  }
}


void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio) 
{
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
  
  sprintf(request_hdr, requestline_hdr_format, path);

  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) 
  {
    if (strcmp(buf, endof_hdr) == 0)
      break;
    
    if (!strncasecmp(buf, host_key, strlen(host_key)))
    {
      strcpy(host_hdr, buf);
      continue;
    }

    if (strncasecmp(buf, connection_key, strlen(connection_key))
        &&strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
        &&strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
        strcat(other_hdr, buf);
      }
  }
  if (strlen(host_hdr) == 0) {
    sprintf(host_hdr, host_hdr_format, hostname);
  }
  sprintf(http_header, "%s%s%s%s%s%s%s",
          request_hdr,
          host_hdr,
          conn_hdr,
          prox_hdr,
          user_agent_hdr,
          other_hdr,
          endof_hdr);
  return;
}

inline int connect_endServer(char *hostname, int port, char *http_header)
{
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr);
}

void parse_uri(char *uri, char *hostname, char *path, int *port)
{
  *port = 80;
  char *pos = strstr(uri, "//");

  pos = pos!=NULL? pos+2:uri;

  char *pos2 = strstr(pos, ":");

  if (pos2 != NULL) {
    *pos2 = '\0';
    sscanf(pos, "%s", hostname);
    sscanf(pos2+1, "%d%s", port, path);
  } else {
    pos2 = strstr(pos, "/");
    if (pos2 != NULL) {
      *pos2 = '\0';
      sscanf(pos, "%s", hostname);
      *pos2 = '/';
      sscanf(pos2, "%s", path);
    } else {
      scanf(pos, "%s", hostname);
    }
  }
  return;
}

void cache_init()
{
  cache.cache_num = 0;
  int i;
  for (i=0; i<CACHE_OBJS_COUNT; i++) 
  {
    cache.cacheobjs[i].LRU = 0;
    cache.cacheobjs[i].isEmpty = 1;
    Sem_init(&cache.cacheobjs[i].wmutex, 0, 1);
    Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1);
    cache.cacheobjs[i].readCnt = 0;
  }
}

void readerPre(int i) 
{
  P(&cache.cacheobjs[i].rdcntmutex);
  cache.cacheobjs[i].readCnt++;
  if (cache.cacheobjs[i].readCnt == 1)
    P(&cache.cacheobjs[i].wmutex);
  V(&cache.cacheobjs[i].rdcntmutex);
}

void readerAfter(int i) 
{
  P(&cache.cacheobjs[i].rdcntmutex);
  cache.cacheobjs[i].readCnt--;
  if (cache.cacheobjs[i].readCnt == 0)
    V(&cache.cacheobjs[i].wmutex);
  V(&cache.cacheobjs[i].rdcntmutex);
}

int cache_find(char *url) 
{
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++) 
  {
    readerPre(i);
    if (cache.cacheobjs[i].isEmpty == 0 && strcmp(url, cache.cacheobjs[i].cache_url) == 0)
    {
      readerAfter(i);
      return i;
    }
    readerAfter(i);
  }
  return -1;
}

int cache_eviction() 
{
  int min = LRU_MAGIC_NUMBER;
  int minindex = 0;
  int i;
  for (i=0; i<CACHE_OBJS_COUNT; i++) 
  {
    readerPre(i);
    if (cache.cacheobjs[i].isEmpty == 1) 
    {
      minindex = i;
      readerAfter(i);
      break;
    }
    if (cache.cacheobjs[i].LRU < min) 
    {
      minindex = i;
      min = cache.cacheobjs[i]. LRU;
      readerAfter(i);
      continue;
    }
    readerAfter(i);
  }
  return minindex;
}

void writePre(int i) 
{
  P(&cache.cacheobjs[i].wmutex);
}

void writeAfter(int i) 
{
  V(&cache.cacheobjs[i].wmutex);
}

void cache_LRU(int index) 
{
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++) 
  {
    if (i == index) 
    {
      continue;
    }
    writePre(i);
    if (cache.cacheobjs[i].isEmpty == 0) 
    {
      cache.cacheobjs[i].LRU--;
    }
    writeAfter(i);
  }
}

void cache_uri(char *uri, char *buf) 
{
  int i = cache_eviction();
  
  writePre(i);

  strcpy(cache.cacheobjs[i].cache_obj, buf);
  strcpy(cache.cacheobjs[i].cache_url, uri);
  cache.cacheobjs[i].isEmpty = 0;
  cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER;
  cache_LRU(i);

  writeAfter(i);
}
