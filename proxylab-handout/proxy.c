#include <stdio.h>
#include "csapp.h"
#include <string.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define LRU_NUMBER 9999
#define CACHE_COUNT 10

/* You won't lose style points for including this long line in your code */
//static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
//static const char *accept_hdr = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n";
//static const char *accept_encoding_hdr = "Accept-Encoding: gzip, deflate\r\n";

//typedef for cache
typedef struct {
    char cacheobj[MAX_OBJECT_SIZE];
    char cacheurl[MAXLINE];
    sem_t wmutex;
    sem_t rdcntmutex; // readcnt 접근 제한
    int isempty; // cache 접근 제한
    int lru; // LRU 값
    int readcnt; 

}Cacheitem;

typedef struct {
    Cacheitem cacheobjs[CACHE_COUNT];
    int cachenum;
}Cache;

//functions for cache
void initcache();
int findcache(char *url);
int evictcache();
void lrucache(int index);
void uricache(char *uri, char *buf);
void beforeread(int i);
void afterread(int i);
void beforewrtie(int i);
void afterwrite(int i);

//functions
void proxy(int fd);
void *thread(void *v);
void parseuri(char *uri, char *hostname, char *path, int *port);
void httpheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connectserver(char *hostname, int port, char *http_header);

//global varialbles
Cache cache;
static const char *connection_key = "Connection";
static const char *user_key= "User-Agent";
static const char *proxy_key = "Proxy-Connection";
static const char *host_key = "Host";
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_hdr = "Proxy-Connection: close\r\n";
static const char *host_form = "Host: %s\r\n";
static const char *request_form = "GET %s HTTP/1.0\r\n";
static const char *end_hdr = "\r\n";

int main(int argc,char **argv)
{
    int listenfd;
    int connfd;
    char hostname[MAXLINE];
    char port[MAXLINE];
    struct sockaddr_storage clientaddr;
    socklen_t  clientlen;
    pthread_t thr;
    
    //argument가 제대로 주어지지 않으면 exit
    if(argc != 2){
        fprintf(stderr,"usage :%s <port> \n",argv[0]);
        exit(1);
    }

    //cache 초기화
    initcache();
    //ignore SIGPIPE signal
    Signal(SIGPIPE, SIG_IGN);
    listenfd = Open_listenfd(argv[1]);

    //csapp 교재 참고해서 작성
    while(1){
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA*)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        Pthread_create(&thr, NULL, thread, (void *)connfd); // Part 2: concurrent request 처리
    }

    return 0;
}

// thread -> concurrent
void *thread(void *v)
{
    int fd = (int)v;
    Pthread_detach(pthread_self());
    proxy(fd);
    Close(fd);
}

// client HTTP transaction을 처리하는 함수
void proxy(int fd)
{
    int endserver;
    int port;
    int cacheidx;
    int n;
    int sizebuf = 0;

    char buf[MAXLINE];
    char method[MAXLINE];
    char uri[MAXLINE];
    char version[MAXLINE];
    char http_header[MAXLINE];
    char hostname[MAXLINE];
    char path[MAXLINE];
    char cachebuf[MAX_OBJECT_SIZE];

    rio_t rio; // for client 
    rio_t serverrio; // for server

    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);

    //client 요청을 읽는다
    sscanf(buf,"%s %s %s", method, uri, version);

    //원래 url 저장
    char urlsave[100];
    strcpy(urlsave, uri);

    // GET 요청만 받아들인다
    if(strcasecmp(method, "GET")){
        printf("only accept <GET>");
        return;
    }

    // uri가 cache에 있는지 확인하고 없으면 caching 
    if((cacheidx = findcache(urlsave)) != -1) {
         beforeread(cacheidx);
         Rio_writen(fd, cache.cacheobjs[cacheidx].cacheobj, strlen(cache.cacheobjs[cacheidx].cacheobj));
         afterread(cacheidx);
         return;
    }
    // hostname, port, path 정보를 알아내기 위해 uri parsing 
    parseuri(uri, hostname, path, &port);
    //end server로 보낼 http header 생성
    httpheader(http_header, hostname, path, port, &rio);
    //end server 연결
    endserver = connectserver(hostname, port, http_header);

    //연결 실패 시 메세지 출력
    if(endserver < 0){
        printf("fail the connection\n");
        return;
    }


    Rio_readinitb(&serverrio, endserver);
    Rio_writen(endserver, http_header, strlen(http_header));

    //end server로 부터 메세지 받아서 client에게 전달
    while((n=Rio_readlineb(&serverrio, buf, MAXLINE)) != 0)
    {
        sizebuf += n;
        if(sizebuf < MAX_OBJECT_SIZE) strcat(cachebuf, buf);
        Rio_writen(fd, buf, n);
    }

    Close(endserver);

    //store
    if(sizebuf < MAX_OBJECT_SIZE){
        uricache(urlsave, cachebuf);
    }
}

void httpheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE];
    char request_hdr[MAXLINE]; //request line
    char other_hdr[MAXLINE];
    char host_hdr[MAXLINE];
    sprintf(request_hdr, request_form, path);

    //다른 requeset header get 후 change
    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
	//EOF case
        if(strcmp(buf,end_hdr) == 0) {
		break;
	}

        if(!strncasecmp(buf, host_key, strlen(host_key)))
        {
            strcpy(host_hdr,buf);
            continue;
        }

        if(!strncasecmp(buf, connection_key, strlen(connection_key)) && !strncasecmp(buf, proxy_key, strlen(proxy_key)) && !strncasecmp(buf, user_agent_hdr, strlen(user_agent_hdr)))
        {
            strcat(other_hdr,buf);
        }
    }
    if(strlen(host_hdr) == 0)
    {
        sprintf(host_hdr, host_form, hostname);
    }
    sprintf(http_header, "%s%s%s%s%s%s%s", request_hdr, host_hdr, connection_hdr, proxy_hdr, user_agent_hdr, other_hdr, end_hdr);

    return;
}

// server connection 담당 함수
inline int connectserver(char *hostname, int port, char *http_header)
{
    char tempport[100];
    sprintf(tempport, "%d", port);
    return Open_clientfd(hostname, tempport);
}

//uri parsing을 담당하는 함수
void parseuri(char *uri, char *hostname, char *path, int *port)
{
    char* pos = strstr(uri,"//");

    if(pos == NULL) {
	    pos = uri;
    }
    else {
	    pos += 2;
    }

    *port = 80;
    char*pos_ = strstr(pos,":");

    if(pos_ == NULL) {
	pos_ = strstr(pos, "/");
	if(pos_ == NULL) {
	    sscanf(pos, "%s", hostname);
        }
        else {
            *pos_ = '\0';
            sscanf(pos,"%s", hostname);
            *pos_ = '/';
            sscanf(pos_,"%s", path);
        }

    }
    else {
	*pos_ = '\0';
        sscanf(pos, "%s", hostname);
        sscanf(pos_+1, "%d%s", port, path);
    }
    return;
}

//cache 초기화 함수
void initcache() 
{
    cache.cachenum = 0;
    for(int i=0; i<CACHE_COUNT; i++) {
        cache.cacheobjs[i].lru = 0;
	cache.cacheobjs[i].readcnt = 0;
        cache.cacheobjs[i].isempty = 1;
	Sem_init(&cache.cacheobjs[i].wmutex, 0, 1);
        Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1);
    }
}

//uri cache를 담당하는 함수
void uricache(char *uri, char *buf)
{
    int i = evictcache();
    beforewrite(i);
    strcpy(cache.cacheobjs[i].cacheurl, uri);
    strcpy(cache.cacheobjs[i].cacheobj, buf);
    cache.cacheobjs[i].lru = LRU_NUMBER;
    cache.cacheobjs[i].isempty = 0;
    lrucache(i);
    afterwrite(i);
}

//주어진 url이 cache에 있는지 없는지 확인하는 함수
int findcache(char *url)
{
    int i;
    for(i=0; i<CACHE_COUNT; i++) {
        beforeread(i);
        if((strcmp(url,cache.cacheobjs[i].cacheurl)==0) && (cache.cacheobjs[i].isempty==0)) {
		break;
	}
        afterread(i);
    }

    if(i >= CACHE_COUNT) {
	    return -1;
    }

    return i;
}

//empty cache object를 찾거나 또는 eviction 되어야하는 cache object를 찾는 함수
int evictcache()
{
    int min = LRU_NUMBER;
    int minidx = 0;
    for(int i=0; i<CACHE_COUNT; i++)
    {
        beforeread(i);
	if(cache.cacheobjs[i].lru < min) {
	    afterread(i);
            minidx = i;
            continue;
        }
        if(cache.cacheobjs[i].isempty == 1) {
            afterread(i);
	    minidx = i;
            break;
        }
        afterread(i);
    }

    return minidx;
}

//lru number 업데이트 담당 함수
void lrucache(int idx)
{
    int i;	
    for(i=0; i<idx; i++) {
        beforewrite(i);
        if((idx != i) && (cache.cacheobjs[i].isempty == 0)) {
            cache.cacheobjs[i].lru--;
        }
        afterwrite(i);
    }

    for(i++; i<CACHE_COUNT; i++)    {
        beforewrite(i);
        if((i != idx) && (cache.cacheobjs[i].isempty == 0)) {
            cache.cacheobjs[i].lru--;
        }
        afterwrite(i);
    }
}

//PV 작업을 담당하는 함수들
void beforewrite(int i) {
    P(&cache.cacheobjs[i].wmutex);
}

void beforeread(int i)
{
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readcnt++;
    if(cache.cacheobjs[i].readcnt == 1) {
            P(&cache.cacheobjs[i].wmutex);
    }
    V(&cache.cacheobjs[i].rdcntmutex);
}

void afterwrite(int i) {
    V(&cache.cacheobjs[i].wmutex);
}

void afterread(int i)
{
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readcnt--;
    if(cache.cacheobjs[i].readcnt==0) {
            V(&cache.cacheobjs[i].wmutex);
    }
    V(&cache.cacheobjs[i].rdcntmutex);
}
