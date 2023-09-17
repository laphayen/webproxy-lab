/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */

#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv) {
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  // 명령줄 인수를 확인하여 서버가 사용할 포트 번호를 결정
  // 포트 번호를 받지 않으면 사용법을 출력하고 프로그램을 종료
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
}

// 클라이언트의 HTTP 요청을 처리하는 역할
// 한 개의 HTTP 트랜잭션을 처리한다.
void doit(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  // 입출력 버퍼 초기화
  rio_t rio;

  // 클라이언트와의 통신을 위한 소켓 파일 디스크립터를 받는다. 
  Rio_readinitb(&rio, fd); 
  // 요청 라인을 읽고 HTTP 요청 메소드, URI, 버전을 파싱한다.
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers: \n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);
  // Tiny는 HTTP 메소드 중 GET 메소드만 지원
  // 클라이언트가 다른 메소드를 요청 시, 에러 메시지 전송 후 메인으로 돌아오고, 그 후 연결을 닫고 다음 연결 요청을 기다린다.
  if (strcasecmp(method, "GET"))
  {
    clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
    return;
  }
  // 다른 요청 헤더들을 무시한다.
  read_requesthdrs(&rio);

  // URI를 분석하여 정직, 동적 컨텐츠를 판단한다.
  // URI를 파일 이름과 비어 있을 수 있는 CGI 인자 스트링으로 분석하고
  // 요청이 정적 또는 동적 컨텐츠를 위한 것인지 나타내는 플래그를 설정한다.
  // 만일 이 파일이 디스크 상에 있지 않으면, 에러 메시지를 즉시 클라이언트에게 보내고 리턴한다.
  is_static = parse_uri(uri, filename, cgiargs);
  if (stat(filename, &sbuf) < 0)
  {
    clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  if (is_static)
  {
    // 정적 컨텐츠를 요청한 경우
    // 읽기 권한을 가지고 있는지를 검증한다.
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
    {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    // 정적 컨텐츠를 클라이언트에게 제공한다.
    serve_static(fd, filename, sbuf.st_size);
  }
  else
  {
    // 만일 요청이 동적 컨텐츠에 대한 것이라면 이 파일이 실행 가능한지 검증하고
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode))
    {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    // 동적 컨텐츠를 클라이언트에게 제공한다.
    serve_dynamic(fd, filename, sbuf.st_size);
  }
}

// 클라이언트에게 에러 응답을 보내는 역할
// HTTP 응답을 응답 라인에 적절한 상태 코드와 상태 메시지와 함께 클라이언트에게 전송
// 브라우저 사용자에게 에러를 설명하는 응답 본체에 HTML 파일도 보낸다.
// fd, 에러 원인 설명, HTTP 응답코드, 간단한 상태 메시지, 긴 설명 메시지
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

// 클라이언트로부터 수신한 HTTP 요청의 헤더를 읽고 무시
// 구조체로 초기화된 입력 버퍼 rp를 사용하여 읽는다.
void read_requesthdrs(rio_t *rp)
{
  // 헤더 라인을 읽는다.
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  // 빈줄을 만날 때까지 헤더를 읽는다.(헤더의 끝)
  while(strcmp(buf, "\r\n"))
  {
    Rio_readlineb(rp, buf, MAXLINE);
    // 헤더의 내용 표시 - 디버깅 및 로깅 목적 사용
    printf("%s", buf);
  }
  return;
}

// HTTP URI를 분석한다.
// 요청이 정적, 동적인지 결정하고 해당 파일의 이름과 CGI 인자를 추출
int parse_uri(char *uri, char * filename, char *cgiargs)
{
  char *ptr;
  
  // URI 문자열을 검사하여 cgi-bin 문자열이 포함 여부를 확인
  // 미포함 시 정적으로 간주하고 1을 반환한다.
  if(!strstr(uri, "cgi-bin")){
    // 정적 컨텐츠 요청 처리
    strcpy(cgiargs, "");
    strcpy(filename, ".");
    strcat(filename, uri);
    if (uri[strlen(uri)-1] == '/') 
      strcat(filename, "home.html");
    return 1;
  }
  // cgi-bin 문자열이 포함 시
  // CGI 인자를 추출하고 해당 부분을 NULL로 종료하여 
  // URI 문자열을 파일 이름으로 변환한다.
  // URI에서 ? 문자을 찾아서 그 위치를 기준으로 URI를 두 부분으로 나누고
  // 앞 부분을 파일 이름으로 설정
  // 뒷 부분을 CGI 인자로 설정
  else {
    ptr = index(uri, '?');
    if (ptr){
      strcpy(cgiargs, ptr+1);
      // ? 문자를 NULL로 대체하여 파일 이름을 종료한다.
      *ptr = '\0';
    }
    else{
      strcpy(cgiargs, "");
    }
    // 나머지 URI 부분을 상대 리눅스 파일 이름으로 변환한다.
    strcpy(filename, ".");
    strcat(filename, uri);
    return 0;
  }
}

// 주어진 파일 이름을 기바능로 해당 파일의 MIME 유형을 결정하고
// filetype 문자열에 해당 MIME 유형을 설정하는 역할을 한다.
void get_filetype(char * filename, char *filetype)
{
  if(strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if(strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if(strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if(strstr(filename,".mp4"))
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
}

// 정적 파일을 클라이언트에게 제공하는 역할
// 파일 이름과 크기에 따라 HTTP 응답 헤더를 생성하고
// 파일 내용을 클라이언트에게 전송한다.
void serve_static(int fd, char *filename, int filesize)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];
  
  // 파일 이름을 기반으로 MIME 유형을 결정
  // filetype 변수에 저아
  // MIME - 클라이언트에게 전달되는 파일의 종류를 나타낸다.
  get_filetype(filename, filetype);
  // 음답 코드로 성공
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  // 웹 서버 소프트웨어 정보
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  // 연결을 닫을 것임을 나타낸다.
  sprintf(buf, "%sConnection: Close\r\n", buf);
  // 전송될 파일의 크기
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  // 파일의 MIME 유형을 나타낸다.
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers: \n");
  printf("%s", buf);

  // 요청된 파일을 읽기 전용으로 열고 읽는다.
  // srcfd 파일 디스크립터를 통해 열린다.
  srcfd = Open(filename, O_RDONLY, 0);
  // 파일 내용을 메모리로 매핑
  // 메모리 매핑 포인터인 srcp를 얻는다.
  srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  Close(srcfd);
  // 파일 내용을 클라이언트에게 전송 - 파일 내용을 클라이언트에게 쓰고, 파일 크기만큼 전송
  Rio_writen(fd, srcp, filesize);
  // 파일 내용을 클라이언트에게 성공적으로 전송한 후, 메모리 매핑을 해제하고 파일을 닫는다.
  Munmap(srcp, filesize);
}

// 동적 컨텐츠 처리하고 CGI 실행하여 결과를 클라이언트에게 전달하는 역할
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
  char buf[MAXLINE], *emptylist[] = { NULL };

  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server : Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  // 자식 프로세스 생성 - CGI 프로그램을 실행할 역할.
  if(Fork() == 0){
    // QUERY_STRING 환경 변수를 설정 - CGI 프로그램에게 클라이언트로부터 전달된 CGI인자 전달
    setenv("QUERY_STRING", cgiargs, 1);
    // 자식 프로세스의 표준출력을 클라이언트 소켓 파일 디스크립터(fd)로 리디렉션
    // CGI 프로그램 출력이 클라이언트 전송
    Dup2(fd, STDOUT_FILENO);
    // CGI 프로그램 실행
    // filename - 실행할 프로그램의 경로, emptylist - 인자리스트, environ - 환경변수 리스트
    Execve(filename, emptylist, environ);
  }
  // 자식 프로세스 대기
  // 부모 프로세스에서 자식 프로세스의 실행이 완료될 때까지 대기
  // CGI 프로그램의 실행이 완료되면 자식 프로세스가 종료
  Wait(NULL);
}
