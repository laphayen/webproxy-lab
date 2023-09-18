#include <stdio.h>

#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

void doit(int fd);
int parse_uri(char *uri, char *request_ip, char *port, char *filename);
void header_make(char *method, char *request_ip, char *user_agent_hdr, char *version, int clientfd, char *filename);
void serve_static(int clientfd, int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv) {
  // 프록시 듣기 식별자, 프록시 연결 식별자
  int listenfd, connfd;
  // 클라이언트에게 받은 uil 정보를 담을 공간
  char hostname[MAXLINE], port[MAXLINE];
  // 소켓 길이를 저장할 구조체
  socklen_t clientlen;
  // 소켓 구조체 - clientaddress
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  // 명령줄 인수를 확인하여 서버가 사용할 포트 번호를 결정
  // 포트 번호를 받지 않으면 사용법을 출력하고 프로그램을 종료
  // 입력인자가 2개인지 확인
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

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
    printf("Accepted connection from (%s, %s)\n", hostname, port);

    // 현재 연결에 대한 처리 
    doit(connfd);   // line:netp:tiny:doit
    // 연결 처리 후 소켓을 닫는다.
    Close(connfd);  // line:netp:tiny:close
  }
  return 0;
}

void doit(int fd)
{
  int clientfd;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char request_ip[MAXLINE], port[MAXLINE], filename[MAXLINE];
  // 입출력 버퍼 초기화
  rio_t rio;

  // 클라이언트와의 통신을 위한 소켓 파일 디스크립터를 받는다. 
  Rio_readinitb(&rio, fd); 
  // 요청 라인을 읽고 HTTP 요청 메소드, URI, 버전을 파싱한다.
  Rio_readlineb(&rio, buf, MAXLINE);

  printf("Request headers: \n");
  printf("%s", buf);

  sscanf(buf, "%s %s %s", method, uri, version);

  if (!(strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0))
  {
    clienterror(fd, method, "501", "Not impemented",
                "Tiny does not implement this method");
    return;
  }

  // 파싱 시작
  parse_uri(uri, request_ip, port, filename);

  // 헤더랑 응답 라인
  clientfd = Open_clientfd(request_ip, port);
  header_make(method, request_ip, user_agent_hdr, version, clientfd, filename);

  serve_static(clientfd, fd);
  Close(clientfd);
}

int parse_uri(char *uri, char *request_ip, char *port, char *filename)
{
  char *ptr;

  // :의 아스키 코드
  ptr = strchr(uri, 58);
  // 포트가 있는 경우
  if(ptr != NULL)
  {
    *ptr = '\0';
    // 포트 앞의 아이피를 가져온다.
    strcpy(request_ip, uri);
    // 뒤의 포트와 나머지 경로를 가져온다.
    strcpy(port, ptr+1);

    // /의 아스키 코드 - 경로
    ptr = strchr(port, 47);
    // 경로가 있을 경우
    if (ptr != NULL){
      strcpy(filename, ptr);
      *ptr = '\0';
      strcpy(port, port);
    }else{ // 경로가 없을 경우
      strcpy(port, port);
    }
  }
  // 포트가 없을 경우
  else
  { 
    strcpy(request_ip, uri);
    ptr = strchr(request_ip, 47);
    strcpy(filename, ptr+1);
    port = "80";
  }
}

void header_make(char *method, char *request_ip, char *user_agent_hdr, char *version, int clientfd, char *filename)
{
  char buf[MAXLINE];
  
  sprintf(buf, "%s %s %s\r\n", method, filename, "HTTP/1.0");
  sprintf(buf, "%sHost: %s\r\n", buf, request_ip);
  sprintf(buf, "%s%s", buf, user_agent_hdr);
  sprintf(buf, "%sConnection: %s\r\n", buf, "close");
  sprintf(buf, "%sProxy-Connection: %s\r\n\r\n", buf, "close");

  Rio_writen(clientfd, buf, strlen(buf));
}

// 정적 파일을 클라이언트에게 제공하는 역할
// 파일 이름과 크기에 따라 HTTP 응답 헤더를 생성하고
// 파일 내용을 클라이언트에게 전송한다.
void serve_static(int clientfd, int fd)
{
  int src_size;
  char *srcp, *p, content_length[MAXLINE], buf[MAXBUF];
  rio_t server_rio;
  
  // 버퍼 초기화 함수
  Rio_readinitb(&server_rio, clientfd);
  // 읽기
  // 파일 fd에서 n바이트 크기를 읽어온다.
  Rio_readlineb(&server_rio, buf, MAXLINE);
  // 파일 내용을 클라이언트에게 전송 - 파일 내용을 클라이언트에게 쓰고, 파일 크기만큼 전송
  Rio_writen(fd, buf, strlen(buf));

  while(strcmp(buf, "\r\n"))
  {
    if (strncmp(buf, "Content-length:", 15) == 0)
    {
      p = index(buf, 32);
      strcpy(content_length, p+1);
      src_size = atoi(content_length);
    }
  }
  Rio_readlineb(&server_rio, buf, MAXLINE);
  Rio_writen(fd, buf, strlen(buf));

  // body
  srcp = malloc(src_size);
  Rio_readnb(&server_rio, srcp, src_size);
  Rio_writen(fd, srcp, src_size);
  free(srcp);
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor=""fffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  
  // HTTP 응답 헤더와 본문은 클라이언트에게 전송한다.
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}
