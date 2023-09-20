// proxy - cache 구현

#include <stdio.h>

#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
// 캐시 블록당 저장할 수 있는 최대 객체 크기
#define MAX_OBJECT_SIZE 102400

// LRU(Least Recently Used) 알고리즘 - 가장 오랫동안 참조되지 않은 페이지를 교체하는 기법
#define LRU_MAGIC_NUMBER 9999

// 캐시 블록의 총 개수
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

// 캐쉬 블록
// 개별 캐시 블록의 데이터 상태를 관리한다.
// 동시성 문제를 처리하기 위해 세마포어를 사용해서
// 쓰기 및 읽기 연산을 동기화한다.
typedef struct
{
  // 캐시에 저장된 객체의 실제 데이터를 저장하는 버퍼
  // MAX_OBJECT_SIZE - 캐시 블록당 저장할 수 있는 최대 객체 크기
  char cache_obj[MAX_OBJECT_SIZE];
  // 캐시에 저장된 URL을 저장하는 문자열 버퍼
  // MAXLINE - URL 문자열의 최대 길이를 나타내는 상수
  char cache_url[MAXLINE];
  // LRU 알고리즘 - 가장 오래된 캐시 블록을 대체한다.
  // 알고리즘에 따라 캐시 블록의 상대적 빈도를 나타내는 값
  int LRU;
  // 캐시 블록이 비어 있는지 여부를 나타내는 플래그
  // 1 또는 0 - 비어 있으면 1
  int isEmpty;
  // 현재 읽기 작업 중인 클라이언트의 수를 저장하는 변수
  int readCnt;
  // 쓰기 연산을 위한 뮤텍스 세마포어
  // 캐시 블록에 데이터를 쓰는 동안
  // 다른 쓰레드가 쓰기 시도하지 못하도록 동시성을 제어하는 데 사용
  sem_t wmutex;
  // 현재 읽는 클라이언트의 수를 제어하는 뮤텍스 세마포어
  // 여러 클라이언트가 동시에 읽기 작업을 수행 시
  // 값을 업데이트하고 동시성을 제어한다.
  sem_t rdcntmutex;
}cache_block;


// 캐쉬 구조체 정의
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

  // 캐쉬 초기화
  cache_init();

  /* Check command line args */
  // 명령줄 인수를 확인하여 서버가 사용할 포트 번호를 결정
  // 포트 번호를 받지 않으면 사용법을 출력하고 프로그램을 종료
  // 입력인자가 2개인지 확인
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port> \n", argv[0]);
    exit(1);
  }

  // 프로세스가 SIGPIPE 신호를 무시하도록 설정하는 역할
  // 한 프로세스가 소켓 등의 통신 매체를 통해 데이터를 보내려고 시도하지만
  // 데이터를 읽는 프로세스가 이미 종료된 경우 발생하는 시그널을 처리한다.
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

// 스레드를 생성하고 실행하는 함수
void *thread(void *vargsp) {
  // 클라이언트와의 연결을 나타내는 파일 디스크립터(소켓)
  int connfd = (int)vargsp;
  // 현재 스레드를 분리한다(detach)
  // 스레드를 분리하면 해당 스레드가 종료 시 스스로 리소스를 정리한다.
  // 메인 스레드나 다른 스레드와 독립적으로 실행되는 스레드의 경우
  Pthread_detach(pthread_self());
  // 클라이언트와 통신
  doit(connfd);
  // 클라이언트와 연결 종료
  Close(connfd);
}

// 프록시 서버의 핵심 로직
// 클라이언트 요청을 처리하고 원격 서버로 전달하는 과정을 담당한다.
// 캐시를 사용하여 이전에 가져온 데이터를 다시 사용함으로써
// 서버의 응답 속도를 향상시킨다.
void doit(int connfd) {
  // 원결 서버와의 통신을 위한 소켓 파일 디스크립터를 저장할 변수
  int end_serverfd;
  // 버퍼와 HTTP 요청 메서드, URI, 버전을 저장할 변수
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  // 원격 서버에 보낸 HTTP 헤더를 저장할 변수
  char endserver_http_header[MAXLINE];
  // URI의 호스트 이름과 경로를 저장할 변수
  char hostname[MAXLINE], path[MAXLINE];
  // 원격 서버의 포트 번호를 저장할 변수
  int port;
  // 입출력 버퍼 구조체 생성
  rio_t rio;
  // 입출력 서버 버퍼 구조체 선언
  rio_t server_rio;

  // 클라이언트와의 통신을 위한 소켓 파일 디스크립터를 받는다. 
  Rio_readinitb(&rio, connfd);
  // 요청 라인을 읽고 HTTP 요청 메소드, URI, 버전을 파싱한다.
  Rio_readlineb(&rio, buf, MAXLINE);

  // 클라이언트의 요청 라인을 읽는다.
  sscanf(buf, "%s %s %s", method, uri, version);

  // 요청 메서드가 GET이 아닌 경우
  // 프록시 서버가 해당 메서드를 지원하지 않음을 알리고 함수를 종료합니다.
  if (strcasecmp(method, "GET")) {
    printf("Proxy does not implement the method");
    return;
  }

  // 클라이언트의 요청 URI를 임시로 저장할 변수를 선언합니다.
  char url_store[100];

  // uri에 저장된 클라이언트의 요청 URI를 url_store에 복사해서
  // 나중에 캐시 검사에서 사용할 수 있도록 한다.
  // 클라이언트의 요청 uri를 임시로 저장함으로써
  // 캐시 시스템에 이후에 해당 uri를 찾고 캐시된 데이터를 반환한다.
  strcpy(url_store, uri);

  int cache_index;
  // 캐시 검사
  // 요청된 URI의 캐시를 검색한다.
  if ((cache_index=cache_find(url_store)) != -1)
  {
    // 캐시가 존재하는 경우
    // 해당 캐시를 클라이언트에게 전송하고
    // 함수를 종료한다.
    readerPre(cache_index);
    // 클라이언트에게 캐시된 데이터를 전송한다.
    // 소켓 파일 디스크립터, 캐시 블록에서 읽은 데이터, 캐시 블록 데이터의 길이
    Rio_writen(connfd, cache.cacheobjs[cache_index].cache_obj, strlen(cache.cacheobjs[cache_index].cache_obj));
    // 캐시 블록에 대한 읽기 작업을 완료하고 동기화를 해제 또는 정리 작업
    readerAfter(cache_index);
    return;
  }
  
  // 요청된 URI를 파싱하여 호스트 이름, 경로 및 포트 번호를 추출한다.
  parse_uri(uri, hostname, path, &port);

  // 원격 서버에 전송할 HTTP 헤더를 생성한다.
  build_http_header(endserver_http_header, hostname, path, port, &rio);

  // 원격 서버에 연결한다
  end_serverfd = connect_endServer(hostname, port, endserver_http_header);
  // 연결에 실패하면 함수를 종료한다.
  if (end_serverfd < 0)
  {
    printf("connection failed\n");
    return;
  }

  // 원격 서버와의 통신을 위해 server_rio 버퍼를 초기화한다.
  Rio_readinitb(&server_rio, end_serverfd);

  // 생성된 HTTP 헤더를 원격 서버에 전송한다.
  Rio_writen(end_serverfd, endserver_http_header, strlen(endserver_http_header));

  // 캐시에 저장할 데이터를 임시로 저장하기 위한 문자열 버퍼
  char cachebuf[MAX_OBJECT_SIZE];
  int sizebuf = 0;
  size_t n;
  
  // 원격 서버로부터 데이터를 읽는 루프
  // Rio_readlineb 함수를 사용해서 원격 서버로부터 한 줄씩 데이터를 읽고
  while ((n=Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
  {
    // 읽은 데이터의 크기를 n에 저장
    sizebuf += n;
    // 동시에 데이터를 cachebuf에 저장
    if (sizebuf < MAX_OBJECT_SIZE)
      // cachebuf에 원격 서버에서 읽은 데이터를 누적시킨다.
      strcat(cachebuf, buf);
    // 원격 서버로부터 데이터를 읽어 클라이언트에게 전송
    Rio_writen(connfd, buf, n);
  }
  // 원격 서버와의 통신이 완료되면 연결을 닫는다.
  Close(end_serverfd);

  // 데이터 크기가 MAX_OBJECT_SIZE를 초과하지 않으면
  if (sizebuf < MAX_OBJECT_SIZE)
  {
    // cache_uri 함수를 호출하여 데이터를 캐시에 저장한다.
    cache_uri(url_store, cachebuf);
  }
}

// HTTP 헤더를 구성하는 함수
// 호스트 이름, 경로, 포트 번호 및 클라이언트로부터 받은 헤더 정보를 사용해서
// 완전한 HTTP 요청 헤더를 생성한다.
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio) 
{
  // 버퍼, 요청 라인, 다른 헤더, 호스트 헤더를 선언한다.
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
  
  // 요청 라인를 생성한다.
  // requestline_hdr_format - 요청 라인의 포맷 문자열 포함
  // path - 요청할 자원의 경로
  sprintf(request_hdr, requestline_hdr_format, path);

  // 클라이언트로부터 헤더 라인을 읽는다.
  // Rio_readlineb - 한 라인씩 읽는다.
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) 
  {
    // endof_hdr 문자열일 경우 루프 종료한다.
    if (strcmp(buf, endof_hdr) == 0)
      break;
    
    // 헤더 라인을 분석하고 필요한 정보를 추출한다.
    if (!strncasecmp(buf, host_key, strlen(host_key)))
    {
      // 호스트 헤더를 찾아서 저장한다.
      strcpy(host_hdr, buf);
      continue;
    }
    // 헤더 라인을 분석하고 필요한 정보를 추출한다.
    // Connection, Proxy-Connection, User-Agent 제외한 나머지 헤더를
    // other_hdr에 추가하는 부분을 나타낸다.
    // strncasecmp(buf, connection_key, strlen(connection_key) - 현재 읽은 헤더 라인 buf와 connection_key를 대소문자 구분 없이 비교
    // strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key)) - 현재 읽은 헤더 라인 버퍼와 proxy_connection_key를 대소문자 구분 없이 비교
    // strncasecmp(buf, user_agent_key, strlen(user_agent_key)) - 현재 읽은 헤더 라인 버퍼와, user)agent_key를 대소문자 구분 없이 비교
    if (strncasecmp(buf, connection_key, strlen(connection_key))
        &&strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
        &&strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
        // HTTP 요청 헤더에서 특정 헤더 필드를 필터링하고
        // 나머지 헤더 필드를 other_hdr에 추가
        strcat(other_hdr, buf);
      }
  }
  // 호스트 헤더가 없을 경우
  if (strlen(host_hdr) == 0) {
    // 호스트 헤더를 생성한다.
    sprintf(host_hdr, host_hdr_format, hostname);
  }
  // 최종 HTTP 헤더를 생성한다.
  // %s 포맷 문자열을 사용해서 각각의 헤더 부분을 합니다.
  // 생성된 헤더는 http_header 문자열에 저장된다.
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

// 원격 서버에 연결하기 위한 함수
// 호스트 이름, 포트 번호, HTTP 요청 헤더를 사용하여
// 원격 서버에 연결하고 연결된 소켓 파일 디스크립터를 반환한다.
inline int connect_endServer(char *hostname, int port, char *http_header)
{
  // 문자열 형태로 포트 번호를 저장하기 위한 버퍼
  char portStr[100];
  // 호스트와 연결 시 포트 번호를 문자열 형태로 사용한다.
  // 정수형 포트 번호를 문자열로 변환한다.
  sprintf(portStr, "%d", port);
  // 변환된 포트 번호를 사용해서 호스트에 연견한다.
  // 호스트와 연결된 소켓 파일 디스크립터를 반환한다.
  // 연결 실패 시 음수 값을 반환한다.
  return Open_clientfd(hostname, portStr);
}

// URI 문자열을 파싱하여 호스트 이름, 포스, 경로를 분리하는 역할을 수행하는 함수
void parse_uri(char *uri, char *hostname, char *path, int *port)
{
  // HTTP 기본 포트 80번을 지정한다.
  *port = 80;
  // // 호스트 이름과 포트를 구분하는 구분자
  // //를 찾아서 해당 위치를 포인터에 저장한다.
  char *pos = strstr(uri, "//");

  // NULL이 아니면 // 이후 문자열로 이동
  // NULL이면 URL 전체를 가리키도록 설정한다.
  pos = pos!=NULL? pos+2:uri;

  // : 로 호스트 이름과 포트를 구분하기 위한 구분자
  // : 문자열을 찾아서 해당 위치를 포인터에 저장한다.
  char *pos2 = strstr(pos, ":");

  // pos2가 NULL이 아니면
  // URI에 포트 번호가 지정되어 있어서 호스트 이름과 포트 번호를 추출한다.
  if (pos2 != NULL) {
    // pos2가 가리키는 위치에 NULL 삽입하여 호스트 이름을 종료
    *pos2 = '\0';
    // 호스트 이름을 읽어 변수에 저장한다.
    sscanf(pos, "%s", hostname);
    // 다음 문자열에서 포트 번호와 경로를 읽어 포트와 경로를 저장한다.
    sscanf(pos2+1, "%d%s", port, path);
  }
  // 포트 번호가 지정x
  // URI에는 호스트 이름만 지정된다.
  // 호스트 이름과 경로를 추출한다.
  else {
    // /을 찾아서 pos2 포인터에 저장한다.
    pos2 = strstr(pos, "/");
    if (pos2 != NULL) {
      // pos2 가리키는 위치에 NULL을 삽입하여 호스트 이름을 종료
      *pos2 = '\0';
      // 호스트 이름을 읽어 변수에 저장한다.
      sscanf(pos, "%s", hostname);
      // NULL을 /로 변경하여 원래 문자열을 복원
      *pos2 = '/';
      // 경로를 읽어서 저장한다.
      sscanf(pos2, "%s", path);
    } else {
      scanf(pos, "%s", hostname);
    }
  }
  return;
}

// 캐쉬를 초기화하는 함수
// 캐시 데이터를 구조를 초기화하고
// 캐시 내의 각 캐시 블록에 대한 초기 설정한다.
void cache_init()
{
  // 구조체의 멤버를 0으로 저장하여
  // 현재 캐시에 저장된 객체의 수를 나타낸다.
  cache.cache_num = 0;
  int i;
  // 캐시 내의 각 캐시 블록을 초기화한다.
  // 캐시 블록의 개수만큼 반복한다.
  for (i=0; i<CACHE_OBJS_COUNT; i++) 
  {
    // 각 캐시 블록의 LRU 멤버를 0으로 초기화한다.
    cache.cacheobjs[i].LRU = 0;
    // 캐시 블록이 비어 있는지 여부를 표시한다.
    // 초기에는 모든 블록이 비어 있다.
    cache.cacheobjs[i].isEmpty = 1;
    // 캐시 블록에 대한 쓰기 작업을 동기화하기 위해 사용한다.
    // 캐시 블록의 쓰기 뮤텍스를 초기화한다.
    Sem_init(&cache.cacheobjs[i].wmutex, 0, 1);
    // 읽기 작업을 동시에 수행할 시 클라이언트 수를 제어하기 위해 사용
    // 캐시 블록의 쓰기 뮤텍스를 초기화한다.
    Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1);
    // 현재 읽는 클라이언트의 수를 추적한다.
    // 각 캐시 블록의 readcnt 멤버를 0으로 초기화한다.
    cache.cacheobjs[i].readCnt = 0;
  }
}

// 캐시 블록에 대한 읽기 동작을 관리한다.
// 읽기 작업을 동기화하고
// 여러 클라이언트가 동시에 읽기를 수행 시 문제를 방지한다.
void readerPre(int i) 
{
  // 캐시 블록의 읽는 클라이언트 수를 조정하기 위해
  // rdcntmutex를 잠근다.
  P(&cache.cacheobjs[i].rdcntmutex);
  // 현재 읽는 클라이언트의 수를 증가 시킨다.
  cache.cacheobjs[i].readCnt++;
  // 다른 클라이언트가 동시에 쓰기 작업을 시도x
  // 만약 현재 읽는 클라이언트가 첫 번째 일 경우
  // wmutex 뮤텍스를 잠근다.
  if (cache.cacheobjs[i].readCnt == 1)
    P(&cache.cacheobjs[i].wmutex);
  // rdcntmutex 뮤텍스를 해제한다.
  V(&cache.cacheobjs[i].rdcntmutex);
}
void readerAfter(int i) 
{
  // 캐시 블록의 읽는 클라이언트 수를 조정하기 위해
  // rdcntmutex를 잠근다.
  P(&cache.cacheobjs[i].rdcntmutex);
  // 현재 읽는 클라이언트의 수를 감소 시킨다.
  cache.cacheobjs[i].readCnt--;
  // 다른 클라이언트가 동시에 쓰기 작업을 시도x
  // 만약 현재 읽는 클라이언트가 첫 번째 일 경우
  // wmutex 뮤텍스를 잠근다.
  if (cache.cacheobjs[i].readCnt == 0)
    V(&cache.cacheobjs[i].wmutex);
  // rdcntmutex 뮤텍스를 해제한다.
  V(&cache.cacheobjs[i].rdcntmutex);
}

// 주어진 URL을 가진 객체가 캐시에 존재를 확인한다.
int cache_find(char *url) 
{
  int i;
  // 캐시 블록의 개수에 대한 루프를 수행한다.
  for (i = 0; i < CACHE_OBJS_COUNT; i++) 
  {
    // 현재 캐시 블록에 대한 읽기 작업을 시작
    // 다른 클라이언트가 동시에 캐시를 읽을 수 있도록 한다.
    readerPre(i);
    // 현재 캐시 블록이 비어x
    // 멤버가 0이면 캐시 블록이 비어x
    if (cache.cacheobjs[i].isEmpty == 0 && strcmp(url, cache.cacheobjs[i].cache_url) == 0)
    {
      // 다른 클라이언트가 캐시를 읽을 수 있는 상태로 만든다.
      // 현재 캐시 블록에 대한 읽기 작업을 완료
      readerAfter(i);
      // URL이 캐시에 존재하는 경우 해당 캐시 블록의 인덱스를 반환
      return i;
    }
    // 다른 클라이언트가 캐시를 읽을 수 있는 상태로 만든다.
    readerAfter(i);
  }
  // URL이 캐시에 존재x - -1반환
  return -1;
}

// LRU 알고리즘을 기반으로 캐시 객체를 삭제한다.
// 삭제할 캐시를 후보를 선택한다.
int cache_eviction() 
{
  // 초기에는 큰 값으로 설정하여 첫 번째 빈 블록을 선택하는 것을 보장한다.
  // 현재까지 확인한 캐시 블록 중 LRU 값
  int min = LRU_MAGIC_NUMBER;
  // 선택한 후보 블록의 인덱스
  int minindex = 0;
  int i;
  // 모든 캐시 블록에 대한 루프
  for (i=0; i<CACHE_OBJS_COUNT; i++) 
  {
    // 현재 캐시 블록에 대한 읽기 작업
    readerPre(i);
    // 현재 캐시 블록이 비어 있는 경우
    // 맴버가 1이면 캐시 블록이 비어 있다.
    if (cache.cacheobjs[i].isEmpty == 1) 
    {
      // 현재 블록의 인덱스로 설정
      minindex = i;
      // 읽기 작업을 완료하고 루프 종료
      readerAfter(i);
      break;
    }
    // 현재 캐시 블록의 LRU 값이 현재까지 확인한 최소 LRU 값보다 작은 경우
    if (cache.cacheobjs[i].LRU < min) 
    {
      // 현재 블록의 인덱스로 설정
      minindex = i;
      // min을 현재 블록의 LRU값으로 변경한다.
      min = cache.cacheobjs[i]. LRU;
      // 읽기 작업을 완료한다.
      readerAfter(i);
      continue;
    }
    // 읽기 작업을 완료한다.
    readerAfter(i);
  }
  // 삭제할 블록을 선택한다.
  // LRU 알고리즘으로 선정된 후보 블록의 인덱스를 반환한다.
  return minindex;
}

// 다중 스레드 환경에서 캐시 블록에 대한 쓰기 작업을 동기화한다.
// 캐시 블록에 대한 쓰기 작업을 시작 전에
// 해당 캐시 블록의 wmutex 뮤텍스를 잠근다.
// 다른 스레드와의 동시적인 쓰기 작업 충돌을 방지한다.
void writePre(int i) 
{
  // P함수를 호출하여 캐시 블록의 뮤텍스를 잠근다.
  // 현재 쓰기 작업을 시작한 스레드가 쓰기 뮤텍스를 소유
  // 다른 쓰레드는 쓰기 작업이 완료 시까지 대기한다.
  P(&cache.cacheobjs[i].wmutex);
}

// 다중 스레드 환경에서 캐시 블록에 대한 쓰기 작업을 동기화한다.
// 캐시 블록에 대한 쓰기 작업이 완료 시 
// 해당 캐시 블록의 wmutex 뮤텍스를 잠근다.
// 쓰기 작업의 충돌을 방지하고 쓰기 뮤텍스를 다른 스레드에게 양보한다.
void writeAfter(int i) 
{
  // V함수를 호출하여 캐시 블록의 wmutex 뮤텍스를 해제한다.
  // 쓰기 작업이 완료된 스레드가 쓰기 뮤텍스를 해제하고
  // 다른 쓰레드는 쓰기 작업을 수행한다.
  V(&cache.cacheobjs[i].wmutex);
}

// 캐시 블록 내의 객체의 LRU 값을 업데이트한다.
// 새로운 객체가 캐시에 저장되거나 읽혔을 때 LRU 값을 조정한다.
void cache_LRU(int index) 
{
  int i;
  // 모든 캐시 블록에 대한 루프 수행
  for (i = 0; i < CACHE_OBJS_COUNT; i++) 
  {
    // 현재 반복이 인덱스와 일치하는 경우 다음 반복으로 넘어간다.
    // 현재 객체의 LRU값을 업데이트x
    if (i == index) 
    {
      continue;
    }
    // 현재 캐시 블록에 대한 쓰기 작업
    writePre(i);
    // 현재 블록이 비어있지 않는 경우
    // 해당 캐시 블록의 LRU 값을 감소한다.
    // LRU 값이 낮아지면서 해당 객체가 더 최근에 사용되었을 표시한다.
    if (cache.cacheobjs[i].isEmpty == 0) 
    {
      cache.cacheobjs[i].LRU--;
    }
    // 현재 캐시 블록에 대한 쓰기 작업을 완료한다.
    writeAfter(i);
  }
}

// URI에 대한 캐시 업데이트 작업
void cache_uri(char *uri, char *buf) 
{
  // 캐시에서 삭제할 후보 블록의 인덱스를 가져온다.
  int i = cache_eviction();
  
  // 선택된 캐시 블록에 대한 쓰기 작업 수행
  // 다른 스레드가 동시에 캐시 블록을 수정x
  writePre(i);

  // 캐시에 데이터 저장 - 선택된 캐시 블록에 버퍼 내용을 복사
  strcpy(cache.cacheobjs[i].cache_obj, buf);
  // 캐시에 URI 식별 - 선택된 캐시 블록에 URI를 복사
  strcpy(cache.cacheobjs[i].cache_url, uri);
  // 선택된 캐시 블록이 비어 있지 않는 상태 표시
  cache.cacheobjs[i].isEmpty = 0;
  // 객체가 최근에 사용됨을 표시한다.
  // 선택된 캐시 블록의 LRU 값을 LRU 매직 넘버로 설정
  cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER;
  // 현재 객체가 가장 최근에 사용됨을 표시 - LRU값 업데이트
  cache_LRU(i);
  // 쓰기 작업을 완료
  // 다른 쓰레드가 캐시 블록에 대한 작업을 수행 가능 상태
  writeAfter(i);
}
