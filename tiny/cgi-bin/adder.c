/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
 /* $begin adder */
 // 웹 서버와 함께 동작하는 CGI 프로그램
 // 웹 브라우저에서 프로그램 호출 시 
 // 쿼리 문자열에서 추출한 두 숫자를 더한 결과를 HTML 페이지로 반환한다.
#include "csapp.h"

int main(void) {
    char* buf, * p, * method;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
    int n1 = 0, n2 = 0;

    /* Extract the two arguments */
    // 환경 변수 QUERY_STRING을 사용하여 
    // 웹 브라우저부터 전달된 쿼리 문자열을 가져온다.
    if ((buf = getenv("QUERY_STRING")) != NULL) {
        // 쿼리 문자열에서 두 개의 인자를 추출한다.
        // & 문자를 기준으로 두 순자를 분리한다.
        p = strchr(buf, '&');
        *p = '\0';
        strcpy(arg1, buf);
        strcpy(arg2, p + 1);
        // atoi 함수를 사용하여 정수로 변환한다.
        n1 = atoi(arg1);
        n2 = atoi(arg2);
    }

    /* Make the response body */
    // HTTP 응답의 본문을 생성한다.
    // HTML 형식의 텍스트를 생성하여 두 숫자를 더한 결과를 포함한다.
    sprintf(content, "QUERY_STRING=%s", buf);
    sprintf(content, "Welcome to add.com: ");
    sprintf(content, "%sThe Internet addition portal.\r\n<p>", content);
    sprintf(content, "%sThe answer is: %d + %d = %d\r\n<p>", content, n1, n2, n1 + n2);
    sprintf(content, "%sThanks for visiting!\r\n", content);

    /* Generate the HTTP response */
    // HTTP 응답 헤더를 생성하고, 내용 출력한다.
    // printf 함수를 사용해서 HTTP 헤더와 본문을 웹 브라우저로 전송한다.
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);

    /*
    // method�� GET�� ��쿡�� body�� ����
    if (strcasecmp(method, "HEAD") != 0)
        printf("%s", content);
    */
    // 출력 버퍼를 비우고 프로그램을 종료
    fflush(stdout);

    exit(0);
}
/* $end adder */
