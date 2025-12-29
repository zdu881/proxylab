#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
    "Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct cache_entry {
    char *key;
    char *data;
    int size;
    struct cache_entry *prev;
    struct cache_entry *next;
} cache_entry_t;

static cache_entry_t *cache_head = NULL;
static cache_entry_t *cache_tail = NULL;
static int cache_bytes = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void cache_move_to_front(cache_entry_t *entry)
{
    if (!entry || entry == cache_head) {
        return;
    }

    if (entry->prev) {
        entry->prev->next = entry->next;
    }
    if (entry->next) {
        entry->next->prev = entry->prev;
    }
    if (entry == cache_tail) {
        cache_tail = entry->prev;
    }

    entry->prev = NULL;
    entry->next = cache_head;
    if (cache_head) {
        cache_head->prev = entry;
    }
    cache_head = entry;
    if (!cache_tail) {
        cache_tail = entry;
    }
}

static void cache_remove_entry(cache_entry_t *entry)
{
    if (!entry) {
        return;
    }

    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache_head = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache_tail = entry->prev;
    }

    cache_bytes -= entry->size;
    Free(entry->key);
    Free(entry->data);
    Free(entry);
}

static void cache_evict_until_fit(int needed)
{
    while (cache_tail && (cache_bytes + needed) > MAX_CACHE_SIZE) {
        cache_remove_entry(cache_tail);
    }
}

static int cache_get_copy(const char *key, char **out_data, int *out_size)
{
    cache_entry_t *cur;

    *out_data = NULL;
    *out_size = 0;

    pthread_mutex_lock(&cache_mutex);
    cur = cache_head;
    while (cur) {
        if (!strcmp(cur->key, key)) {
            char *copy = Malloc(cur->size);
            memcpy(copy, cur->data, cur->size);
            *out_data = copy;
            *out_size = cur->size;
            cache_move_to_front(cur);
            pthread_mutex_unlock(&cache_mutex);
            return 1;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

static void cache_insert(const char *key, const char *data, int size)
{
    cache_entry_t *cur;
    cache_entry_t *entry;

    if (size <= 0 || size > MAX_OBJECT_SIZE) {
        return;
    }

    pthread_mutex_lock(&cache_mutex);

    cur = cache_head;
    while (cur) {
        if (!strcmp(cur->key, key)) {
            cache_remove_entry(cur);
            break;
        }
        cur = cur->next;
    }

    cache_evict_until_fit(size);
    if (size > MAX_CACHE_SIZE) {
        pthread_mutex_unlock(&cache_mutex);
        return;
    }

    entry = Malloc(sizeof(cache_entry_t));
    entry->key = Malloc(strlen(key) + 1);
    strcpy(entry->key, key);
    entry->data = Malloc(size);
    memcpy(entry->data, data, size);
    entry->size = size;
    entry->prev = NULL;
    entry->next = cache_head;
    if (cache_head) {
        cache_head->prev = entry;
    }
    cache_head = entry;
    if (!cache_tail) {
        cache_tail = entry;
    }
    cache_bytes += size;

    pthread_mutex_unlock(&cache_mutex);
}

static int starts_with_icase(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncasecmp(s, prefix, n) == 0;
}

static void parse_uri(const char *uri, char *hostname, char *port, char *path)
{
    const char *p = uri;
    const char *slash;
    char hostport[MAXLINE];
    const char *colon;
    size_t hostport_len;

    hostname[0] = '\0';
    strcpy(port, "80");
    strcpy(path, "/");

    if (starts_with_icase(p, "http://")) {
        p += 7;
    }

    if (*p == '/') {
        strncpy(path, p, MAXLINE - 1);
        path[MAXLINE - 1] = '\0';
        return;
    }

    slash = strchr(p, '/');
    if (slash) {
        hostport_len = (size_t)(slash - p);
        if (hostport_len >= sizeof(hostport)) {
            hostport_len = sizeof(hostport) - 1;
        }
        memcpy(hostport, p, hostport_len);
        hostport[hostport_len] = '\0';
        strncpy(path, slash, MAXLINE - 1);
        path[MAXLINE - 1] = '\0';
    } else {
        strncpy(hostport, p, sizeof(hostport) - 1);
        hostport[sizeof(hostport) - 1] = '\0';
        strcpy(path, "/");
    }

    colon = strchr(hostport, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - hostport);
        if (host_len >= MAXLINE) {
            host_len = MAXLINE - 1;
        }
        memcpy(hostname, hostport, host_len);
        hostname[host_len] = '\0';
        strncpy(port, colon + 1, MAXLINE - 1);
        port[MAXLINE - 1] = '\0';
    } else {
        strncpy(hostname, hostport, MAXLINE - 1);
        hostname[MAXLINE - 1] = '\0';
    }
}

static void build_cache_key(char *key, const char *hostname, const char *port, const char *path)
{
    snprintf(key, MAXLINE, "%s:%s%s", hostname, port, path);
}

static void read_request_headers(rio_t *client_rio, char *other_hdrs, size_t other_sz,
                                 char *host_hdr, size_t host_sz)
{
    char buf[MAXLINE];

    other_hdrs[0] = '\0';
    host_hdr[0] = '\0';

    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) {
            break;
        }

        if (starts_with_icase(buf, "Host:")) {
            strncpy(host_hdr, buf + 5, host_sz - 1);
            host_hdr[host_sz - 1] = '\0';
            continue;
        }
        if (starts_with_icase(buf, "User-Agent:")) {
            continue;
        }
        if (starts_with_icase(buf, "Connection:")) {
            continue;
        }
        if (starts_with_icase(buf, "Proxy-Connection:")) {
            continue;
        }

        if (strlen(other_hdrs) + strlen(buf) + 1 < other_sz) {
            strcat(other_hdrs, buf);
        }
    }
}

static void normalize_host_from_header(const char *host_hdr, char *hostname, char *port)
{
    char tmp[MAXLINE];
    char *p;
    char *colon;

    if (!host_hdr || !*host_hdr) {
        return;
    }

    while (*host_hdr && isspace((unsigned char)*host_hdr)) {
        host_hdr++;
    }

    strncpy(tmp, host_hdr, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    p = tmp;
    while (*p && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (!*p) {
        return;
    }

    char *end = p + strlen(p);
    while (end > p && (end[-1] == '\r' || end[-1] == '\n' || isspace((unsigned char)end[-1]))) {
        end[-1] = '\0';
        end--;
    }

    colon = strchr(p, ':');
    if (colon) {
        *colon = '\0';
        strncpy(hostname, p, MAXLINE - 1);
        hostname[MAXLINE - 1] = '\0';
        strncpy(port, colon + 1, MAXLINE - 1);
        port[MAXLINE - 1] = '\0';
    } else {
        strncpy(hostname, p, MAXLINE - 1);
        hostname[MAXLINE - 1] = '\0';
    }
}

static void forward_request(int clientfd)
{
    rio_t client_rio;
    rio_t server_rio;
    char buf[MAXLINE];
    char method[MAXLINE];
    char uri[MAXLINE];
    char version[MAXLINE];
    char hostname[MAXLINE];
    char port[MAXLINE];
    char path[MAXLINE];
    char other_hdrs[32768];
    char host_hdr[MAXLINE];
    char request_hdrs[40960];
    char cache_key[MAXLINE];

    char *cached;
    int cached_size;

    int serverfd;

    Rio_readinitb(&client_rio, clientfd);
    if (Rio_readlineb(&client_rio, buf, MAXLINE) <= 0) {
        return;
    }

    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
        return;
    }

    if (strcasecmp(method, "GET")) {
        return;
    }

    parse_uri(uri, hostname, port, path);
    read_request_headers(&client_rio, other_hdrs, sizeof(other_hdrs), host_hdr, sizeof(host_hdr));

    if (hostname[0] == '\0') {
        normalize_host_from_header(host_hdr, hostname, port);
    }
    if (hostname[0] == '\0') {
        return;
    }

    build_cache_key(cache_key, hostname, port, path);
    if (cache_get_copy(cache_key, &cached, &cached_size)) {
        Rio_writen(clientfd, cached, cached_size);
        Free(cached);
        return;
    }

    serverfd = Open_clientfd(hostname, port);
    if (serverfd < 0) {
        return;
    }

    request_hdrs[0] = '\0';
    snprintf(request_hdrs, sizeof(request_hdrs), "GET %s HTTP/1.0\r\n", path);

    if (!strcmp(port, "80")) {
        snprintf(request_hdrs + strlen(request_hdrs), sizeof(request_hdrs) - strlen(request_hdrs),
                 "Host: %s\r\n", hostname);
    } else {
        snprintf(request_hdrs + strlen(request_hdrs), sizeof(request_hdrs) - strlen(request_hdrs),
                 "Host: %s:%s\r\n", hostname, port);
    }

    snprintf(request_hdrs + strlen(request_hdrs), sizeof(request_hdrs) - strlen(request_hdrs),
             "%s", user_agent_hdr);
    snprintf(request_hdrs + strlen(request_hdrs), sizeof(request_hdrs) - strlen(request_hdrs),
             "Connection: close\r\nProxy-Connection: close\r\n");
    if (other_hdrs[0] != '\0') {
        strncat(request_hdrs, other_hdrs, sizeof(request_hdrs) - strlen(request_hdrs) - 1);
    }
    strncat(request_hdrs, "\r\n", sizeof(request_hdrs) - strlen(request_hdrs) - 1);

    Rio_writen(serverfd, request_hdrs, strlen(request_hdrs));

    Rio_readinitb(&server_rio, serverfd);
    {
        char objbuf[MAX_OBJECT_SIZE];
        int objsize = 0;
        int cacheable = 1;
        ssize_t n;

        while ((n = Rio_readnb(&server_rio, buf, MAXBUF)) > 0) {
            Rio_writen(clientfd, buf, n);
            if (cacheable) {
                if (objsize + n <= MAX_OBJECT_SIZE) {
                    memcpy(objbuf + objsize, buf, n);
                    objsize += (int)n;
                } else {
                    cacheable = 0;
                }
            }
        }
        if (cacheable && objsize > 0) {
            cache_insert(cache_key, objbuf, objsize);
        }
    }

    Close(serverfd);
}

static void *thread_main(void *arg)
{
    int connfd = *((int *)arg);
    Free(arg);

    Pthread_detach(Pthread_self());
    forward_request(connfd);
    Close(connfd);
    return NULL;
}

int main(int argc, char **argv)
{
    int listenfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, SIG_IGN);
    listenfd = Open_listenfd(argv[1]);

    while (1) {
        int *connfdp = Malloc(sizeof(int));
        clientlen = sizeof(clientaddr);
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread_main, connfdp);
    }
}
