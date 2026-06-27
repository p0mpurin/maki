#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
} TlsCtx;

static bool soc_initialized = false;
static bool soc_failed = false;
static u32 soc_buffer[0x40000] __attribute__((aligned(0x1000)));
static char http_err[256];

const char *http_last_error(void) {
    return http_err;
}

static void set_err(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(http_err, sizeof(http_err), fmt, args);
    va_end(args);
}

static int ensure_soc(void) {
    if (soc_failed) return -1;
    if (soc_initialized) return 0;

    Result res = socInit(soc_buffer, sizeof(soc_buffer));
    if (R_FAILED(res)) {
        set_err("soc: socInit failed 0x%08lx", res);
        soc_failed = true;
        return -1;
    }

    soc_initialized = true;
    return 0;
}

int http_soc_init(void) {
    return ensure_soc();
}

int http_stream_open(const char *url, const char *agent) {
    if (ensure_soc() < 0) return -1;

    const char *h = url + 7;
    const char *p = strchr(h, '/');
    char host[256]; int hl = p ? (int)(p-h) : (int)strlen(h);
    if (hl >= 256) hl = 255; memcpy(host, h, hl); host[hl] = '\0';
    int port = 80; char *ps = strchr(host, ':');
    if (ps) { *ps = '\0'; port = atoi(ps+1); }

    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) < 0 && errno != EINPROGRESS)
        { close(fd); return -1; }

    char req[1024]; snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\n\r\n",
        p ? p : "/", h, agent);
    if (send(fd, req, strlen(req), 0) < 0) { close(fd); return -1; }

    return fd;
}

int http_stream_read(int fd, uint8_t *buf, int len) {
    int n = recv(fd, buf, len, 0);
    if (n > 0) return n;
    if (n == 0) return -2;
    if (errno == EWOULDBLOCK || errno == EAGAIN) return 0;
    return -1;
}

void http_stream_close(int fd) {
    if (fd >= 0) close(fd);
}

static int tls_connect(HttpConn *conn) {
    TlsCtx *tls = (TlsCtx *)calloc(1, sizeof(TlsCtx));
    if (!tls) return -1;

    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_entropy_init(&tls->entropy);
    mbedtls_ctr_drbg_init(&tls->ctr_drbg);

    int ret = mbedtls_ctr_drbg_seed(&tls->ctr_drbg, mbedtls_entropy_func,
                                     &tls->entropy, NULL, 0);
    if (ret != 0) {
        free(tls);
        return -1;
    }

    ret = mbedtls_ssl_config_defaults(&tls->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        free(tls);
        return -1;
    }

    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->ctr_drbg);

    ret = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
    if (ret != 0) {
        free(tls);
        return -1;
    }

    mbedtls_ssl_set_bio(&tls->ssl, &conn->fd,
                         mbedtls_net_send, mbedtls_net_recv, NULL);

    ret = mbedtls_ssl_set_hostname(&tls->ssl, conn->host);
    if (ret != 0) {
        set_err("tls: set_hostname failed -0x%04x", -ret);
        mbedtls_ssl_free(&tls->ssl);
        mbedtls_ssl_config_free(&tls->conf);
        mbedtls_entropy_free(&tls->entropy);
        mbedtls_ctr_drbg_free(&tls->ctr_drbg);
        free(tls);
        return -1;
    }

    while ((ret = mbedtls_ssl_handshake(&tls->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            set_err("tls: handshake failed -0x%04x", -ret);
            mbedtls_ssl_free(&tls->ssl);
            mbedtls_ssl_config_free(&tls->conf);
            mbedtls_entropy_free(&tls->entropy);
            mbedtls_ctr_drbg_free(&tls->ctr_drbg);
            free(tls);
            return -1;
        }
    }

    conn->ssl_ctx = tls;
    conn->use_tls = true;
    return 0;
}

static void tls_disconnect(HttpConn *conn) {
    if (!conn->ssl_ctx) return;
    TlsCtx *tls = (TlsCtx *)conn->ssl_ctx;
    mbedtls_ssl_close_notify(&tls->ssl);
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_entropy_free(&tls->entropy);
    mbedtls_ctr_drbg_free(&tls->ctr_drbg);
    free(tls);
    conn->ssl_ctx = NULL;
    conn->use_tls = false;
}

static int tls_send(HttpConn *conn, const void *buf, size_t len) {
    if (!conn->use_tls || !conn->ssl_ctx) return send(conn->fd, buf, len, 0);
    TlsCtx *tls = (TlsCtx *)conn->ssl_ctx;
    return mbedtls_ssl_write(&tls->ssl, (const unsigned char *)buf, len);
}

static int tls_recv_timeout(HttpConn *conn, void *buf, size_t len, int timeout_ms) {
    if (!conn->use_tls || !conn->ssl_ctx) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(conn->fd, &fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(conn->fd + 1, &fds, NULL, NULL, &tv) <= 0) return 0;
        return recv(conn->fd, buf, len, 0);
    }
    TlsCtx *tls = (TlsCtx *)conn->ssl_ctx;
    return mbedtls_ssl_read(&tls->ssl, (unsigned char *)buf, len);
}

int http_connect(HttpConn *conn, const char *host, int port) {
    if (ensure_soc() < 0) return -1;

    memset(conn, 0, sizeof(HttpConn));
    conn->fd = -1;
    strncpy(conn->host, host, sizeof(conn->host) - 1);
    conn->port = port;

    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int ret = getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0) {
        set_err("dns: getaddrinfo(%s) failed: %s", host, gai_strerror(ret));
        return -1;
    }

    int last_errno = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        conn->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (conn->fd < 0) {
            last_errno = errno;
            continue;
        }

        if (connect(conn->fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }

        last_errno = errno;
        close(conn->fd);
        conn->fd = -1;
    }

    freeaddrinfo(res);

    if (!ai) {
        set_err("connect: connect(%s:%d) failed errno=%d", host, port, last_errno);
        return -1;
    }

    if (port == 443) {
        if (tls_connect(conn) < 0) {
            close(conn->fd);
            conn->fd = -1;
            return -1;
        }
    }

    conn->connected = true;
    return 0;
}

void http_disconnect(HttpConn *conn) {
    if (!conn) return;
    tls_disconnect(conn);
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->connected = false;
}

static int tls_recv_all(HttpConn *conn, char *buf, int size, int timeout_ms) {
    int total = 0;
    while (total < size) {
        int n = tls_recv_timeout(conn, buf + total, size - total, timeout_ms);
        if (n <= 0) return total > 0 ? total : n;
        total += n;
    }
    return total;
}

static int parse_chunked(HttpConn *conn, char *out, int out_size, int total, int timeout_ms) {
    char line[64];
    int line_pos;

    while (1) {
        line_pos = 0;
        while (line_pos < (int)sizeof(line) - 1) {
            int n = tls_recv_timeout(conn, line + line_pos, 1, timeout_ms);
            if (n <= 0) return total;
            line_pos++;
            if (line_pos >= 2 && line[line_pos-2] == '\r' && line[line_pos-1] == '\n') break;
        }
        line[line_pos - 2] = '\0';

        int chunk_size = (int)strtol(line, NULL, 16);
        if (chunk_size == 0) {
            tls_recv_all(conn, line, 2, timeout_ms);
            return total;
        }

        if (total + chunk_size > out_size) break;
        int n = tls_recv_all(conn, out + total, chunk_size, timeout_ms);
        if (n <= 0) return total;
        total += n;

        tls_recv_all(conn, line, 2, timeout_ms);
    }
    return total;
}

static int contains_header_value_ci(const char *headers, const char *name,
                                    const char *value) {
    size_t name_len = strlen(name);
    const char *line = headers;
    while (*line) {
        const char *line_end = strstr(line, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);
        if (line_len > name_len + 1 &&
            strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *v = line + name_len + 1;
            while (*v == ' ' || *v == '\t') v++;
            if (strncasecmp(v, value, strlen(value)) == 0) return 1;
        }
        if (!line_end) break;
        line = line_end + 2;
    }
    return 0;
}

static int content_length_from_headers(const char *headers) {
    const char *line = headers;
    while (*line) {
        const char *line_end = strstr(line, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);
        if (line_len > 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            return atoi(line + 15);
        }
        if (!line_end) break;
        line = line_end + 2;
    }
    return -1;
}

static int status_from_headers(const char *headers) {
    int status = 0;
    sscanf(headers, "HTTP/%*s %d", &status);
    return status;
}

static int read_response_body(HttpConn *conn, const char *header_buf,
                              char *out_buf, int out_buf_size) {
    int content_length = content_length_from_headers(header_buf);
    int is_chunked = contains_header_value_ci(header_buf, "Transfer-Encoding", "chunked");

    int total = 0;
    if (is_chunked) {
        total = parse_chunked(conn, out_buf, out_buf_size, 0, 30000);
    } else if (content_length > 0) {
        total = tls_recv_all(conn, out_buf,
                             content_length > out_buf_size ? out_buf_size : content_length,
                             30000);
    } else {
        total = tls_recv_all(conn, out_buf, out_buf_size - 1, 10000);
    }

    if (total >= 0 && total < out_buf_size) out_buf[total] = '\0';
    return total;
}

int http_post(HttpConn *conn, const char *path, const char *host_header,
              const char *referer, const char *agent, const char *body,
              char *out_buf, int out_buf_size) {
    char request[4096];
    int req_len = snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Referer: %s\r\n"
        "Origin: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host_header, agent, referer, referer, (int)strlen(body), body);

    if (tls_send(conn, request, req_len) != req_len) return -1;

    char header_buf[4096];
    int header_pos = 0;
    int header_done = 0;

    while (!header_done && header_pos < (int)sizeof(header_buf) - 1) {
        int n = tls_recv_timeout(conn, header_buf + header_pos, 1, 10000);
        if (n <= 0) return -1;
        header_pos++;
        if (header_pos >= 4 &&
            header_buf[header_pos-4] == '\r' && header_buf[header_pos-3] == '\n' &&
            header_buf[header_pos-2] == '\r' && header_buf[header_pos-1] == '\n') {
            header_done = 1;
        }
    }
    header_buf[header_pos] = '\0';

    int status = status_from_headers(header_buf);
    int total = read_response_body(conn, header_buf, out_buf, out_buf_size);
    if (status < 200 || status >= 300) {
        if (strstr(header_buf, "Cf-Mitigated: challenge") ||
            strstr(header_buf, "cf-mitigated: challenge")) {
            set_err("http: %d Cloudflare challenge", status);
        } else {
            set_err("http: status %d", status);
        }
        return -1;
    }
    return total;
}

int http_get(const char *url, const char *referer, const char *agent,
             char *out_buf, int out_buf_size) {
    HttpConn conn;
    memset(&conn, 0, sizeof(conn));

    const char *host_part = url;
    if (strncmp(url, "https://", 8) == 0) {
        host_part = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        host_part = url + 7;
    }

    const char *path_part = strchr(host_part, '/');
    if (path_part) {
        size_t host_len = path_part - host_part;
        if (host_len >= sizeof(conn.host)) host_len = sizeof(conn.host) - 1;
        memcpy(conn.host, host_part, host_len);
        conn.host[host_len] = '\0';
    } else {
        strncpy(conn.host, host_part, sizeof(conn.host) - 1);
        path_part = "/";
    }

    int port = strncmp(url, "https://", 8) == 0 ? 443 : 80;
    char *port_sep = strchr(conn.host, ':');
    if (port_sep) {
        *port_sep = '\0';
        port = atoi(port_sep + 1);
    }
    if (http_connect(&conn, conn.host, port) < 0) return -1;

    char request[4096];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Referer: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path_part ? path_part : "/", conn.host, agent, referer);

    if (tls_send(&conn, request, strlen(request)) < 0) {
        http_disconnect(&conn);
        return -1;
    }

    char header_buf[4096];
    int header_pos = 0;
    int header_done = 0;

    while (!header_done && header_pos < (int)sizeof(header_buf) - 1) {
        int n = tls_recv_timeout(&conn, header_buf + header_pos, 1, 30000);
        if (n <= 0) {
            http_disconnect(&conn);
            return -1;
        }
        header_pos++;
        if (header_pos >= 4 &&
            header_buf[header_pos-4] == '\r' && header_buf[header_pos-3] == '\n' &&
            header_buf[header_pos-2] == '\r' && header_buf[header_pos-1] == '\n') {
            header_done = 1;
        }
    }
    header_buf[header_pos] = '\0';

    int status = status_from_headers(header_buf);
    int total = read_response_body(&conn, header_buf, out_buf, out_buf_size);
    http_disconnect(&conn);

    if (status < 200 || status >= 300) {
        set_err("http: status %d", status);
        return -1;
    }

    return total;
}

static bool httpc_ready = false;

int httpc_get(const char *url, const char *agent, char *out_buf, int out_buf_size) {
    if (!httpc_ready) {
        Result r = httpcInit(0x100000);
        if (R_FAILED(r)) {
            set_err("httpc: init failed 0x%08lx", r);
            return -1;
        }
        httpc_ready = true;
    }

    httpcContext ctx;
    Result r = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(r)) {
        set_err("httpc: open failed 0x%08lx", r);
        return -1;
    }

    httpcAddRequestHeaderField(&ctx, "User-Agent", agent);
    httpcAddRequestHeaderField(&ctx, "Connection", "close");

    r = httpcBeginRequest(&ctx);
    if (R_FAILED(r)) {
        set_err("httpc: begin failed 0x%08lx", r);
        httpcCloseContext(&ctx);
        return -1;
    }

    u32 status = 0;
    r = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(r)) {
        set_err("httpc: status code failed 0x%08lx", r);
        httpcCloseContext(&ctx);
        return -1;
    }
    if (status < 200 || status >= 300) {
        set_err("httpc: status %lu", status);
        httpcCloseContext(&ctx);
        return -1;
    }

    int buf_off = 0;
    do {
        u32 chunk_size = 0;
        r = httpcDownloadData(&ctx, (u8 *)(out_buf + buf_off),
                              out_buf_size - 1 - buf_off, &chunk_size);
        buf_off += chunk_size;
        if (buf_off >= out_buf_size - 1) break;
    } while (r == HTTPC_RESULTCODE_DOWNLOADPENDING);

    httpcCloseContext(&ctx);
    out_buf[buf_off] = '\0';
    return buf_off;
}

int httpc_download(const char *url, const char *agent, uint8_t *out_buf, int out_buf_size) {
    if (!httpc_ready) {
        Result r = httpcInit(0x100000);
        if (R_FAILED(r)) { set_err("httpc: init failed 0x%08lx", r); return -1; }
        httpc_ready = true;
    }

    httpcContext ctx;
    Result r = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(r)) { set_err("httpc: open failed 0x%08lx", r); return -1; }

    httpcAddRequestHeaderField(&ctx, "User-Agent", agent);
    httpcAddRequestHeaderField(&ctx, "Connection", "close");

    r = httpcBeginRequest(&ctx);
    if (R_FAILED(r)) { set_err("httpc: begin failed 0x%08lx", r); httpcCloseContext(&ctx); return -1; }

    u32 status = 0;
    r = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(r) || status < 200 || status >= 300) {
        set_err("httpc: status %lu", status);
        httpcCloseContext(&ctx);
        return -1;
    }

    int buf_off = 0;
    do {
        u32 chunk_size = 0;
        r = httpcDownloadData(&ctx, out_buf + buf_off, out_buf_size - buf_off, &chunk_size);
        buf_off += chunk_size;
        if (buf_off >= out_buf_size) break;
    } while (r == HTTPC_RESULTCODE_DOWNLOADPENDING);

    httpcCloseContext(&ctx);
    return buf_off;
}

int httpc_post_json(const char *url, const char *agent, const char *body,
                    char *out_buf, int out_buf_size) {
    if (!httpc_ready) {
        Result r = httpcInit(0x100000);
        if (R_FAILED(r)) { set_err("httpc: init failed 0x%08lx", r); return -1; }
        httpc_ready = true;
    }

    httpcContext ctx;
    Result r = httpcOpenContext(&ctx, HTTPC_METHOD_POST, url, 0);
    if (R_FAILED(r)) { set_err("httpc: open failed 0x%08lx", r); return -1; }

    httpcAddRequestHeaderField(&ctx, "User-Agent", agent);
    httpcAddRequestHeaderField(&ctx, "Content-Type", "application/json");
    httpcAddRequestHeaderField(&ctx, "Connection", "close");
    httpcAddPostDataRaw(&ctx, (const u32 *)body, strlen(body));

    r = httpcBeginRequest(&ctx);
    if (R_FAILED(r)) { set_err("httpc: begin failed 0x%08lx", r); httpcCloseContext(&ctx); return -1; }

    u32 status = 0;
    r = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(r) || status < 200 || status >= 300) {
        set_err("httpc: status %lu", status);
        httpcCloseContext(&ctx);
        return -1;
    }

    int buf_off = 0;
    do {
        u32 chunk_size = 0;
        r = httpcDownloadData(&ctx, (u8 *)(out_buf + buf_off),
                              out_buf_size - 1 - buf_off, &chunk_size);
        buf_off += chunk_size;
        if (buf_off >= out_buf_size - 1) break;
    } while (r == HTTPC_RESULTCODE_DOWNLOADPENDING);

    httpcCloseContext(&ctx);
    out_buf[buf_off] = '\0';
    return buf_off;
}

int httpc_download_file(const char *url, const char *agent, const char *filepath,
                        httpc_progress_cb cb, void *userdata) {
    if (!httpc_ready) {
        Result r = httpcInit(0x100000);
        if (R_FAILED(r)) { set_err("httpc: init failed 0x%08lx", r); return -1; }
        httpc_ready = true;
    }

    httpcContext ctx;
    Result r = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(r)) { set_err("httpc: open failed 0x%08lx", r); return -1; }

    httpcAddRequestHeaderField(&ctx, "User-Agent", agent);
    httpcAddRequestHeaderField(&ctx, "Connection", "close");

    r = httpcBeginRequest(&ctx);
    if (R_FAILED(r)) { set_err("httpc: begin failed 0x%08lx", r); httpcCloseContext(&ctx); return -1; }

    u32 status = 0;
    r = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(r) || status < 200 || status >= 300) {
        set_err("httpc: status %lu", status);
        httpcCloseContext(&ctx);
        return -1;
    }

    u32 downloadSize = 0, contentSize = 0;
    httpcGetDownloadSizeState(&ctx, &downloadSize, &contentSize);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        set_err("fopen errno=%d", errno);
        httpcCloseContext(&ctx);
        return -1;
    }

    uint8_t *chunk = (uint8_t *)malloc(65536);
    if (!chunk) { fclose(fp); httpcCloseContext(&ctx); set_err("httpc: no memory"); return -1; }
    int total = 0;
    do {
        u32 chunk_size = 0;
        r = httpcDownloadData(&ctx, chunk, 65536, &chunk_size);
        if (chunk_size > 0) {
            fwrite(chunk, 1, chunk_size, fp);
            total += chunk_size;
            if (cb) cb(total, contentSize, userdata);
        }
    } while (r == HTTPC_RESULTCODE_DOWNLOADPENDING);

    free(chunk);
    fclose(fp);
    httpcCloseContext(&ctx);

    if (total <= 0) { set_err("httpc: download empty"); remove(filepath); return -1; }
    return total;
}

int sock_get(const char *url, const char *agent, uint8_t *out_buf, int out_buf_size) {
    const char *host_start = url + 7;
    const char *path_start = strchr(host_start, '/');
    char host_clean[256];
    int host_len = path_start ? (int)(path_start - host_start) : (int)strlen(host_start);
    if (host_len >= 256) host_len = 255;
    memcpy(host_clean, host_start, host_len);
    host_clean[host_len] = '\0';

    int port = 80;
    char *port_sep = strchr(host_clean, ':');
    if (port_sep) { *port_sep = '\0'; port = atoi(port_sep + 1); }
    if (!path_start) path_start = "/";

    set_err("sock: connecting %s:%d", host_clean, port);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host_clean);

    if (addr.sin_addr.s_addr == INADDR_NONE) {
        set_err("sock: bad addr %s", host_clean);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_err("sock: socket failed errno=%d", errno); return -1; }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            set_err("sock: connect failed errno=%d", errno);
            close(fd); return -1;
        }
        fd_set wfds; struct timeval tv = {10, 0};
        FD_ZERO(&wfds); FD_SET(fd, &wfds);
        if (select(fd+1, NULL, &wfds, NULL, &tv) <= 0) {
            set_err("sock: connect timeout");
            close(fd); return -1;
        }
    }

    char host_hdr[300];
    snprintf(host_hdr, sizeof(host_hdr), "%s:%d", host_clean, port);

    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\n\r\n",
        path_start, host_hdr, agent);
    if (send(fd, req, strlen(req), 0) < 0) {
        set_err("sock: send failed errno=%d", errno);
        close(fd); return -1;
    }

    int total = 0, n;
    while (total < out_buf_size) {
        fd_set rfds; struct timeval tv = {10, 0};
        FD_ZERO(&rfds); FD_SET(fd, &rfds);
        if (select(fd+1, &rfds, NULL, NULL, &tv) <= 0) break;
        n = recv(fd, (char*)out_buf + total, out_buf_size - total, 0);
        if (n <= 0) break;
        total += n;
    }

    if (total <= 0) {
        set_err("sock: recv got %d bytes errno=%d", total, errno);
        close(fd); return -1;
    }
    close(fd);

    int body_off = 0;
    for (int i = 0; i < total - 3; i++) {
        if (out_buf[i]=='\r' && out_buf[i+1]=='\n' && out_buf[i+2]=='\r' && out_buf[i+3]=='\n')
        { body_off = i + 4; break; }
    }
    if (body_off == 0) { set_err("sock: no header end found"); return -1; }

    int body_len = total - body_off;
    if (body_len > 0) memmove(out_buf, out_buf + body_off, body_len);
    return body_len;
}

int http_get_stream(HttpConn *conn, const char *url, const char *referer, const char *agent) {
    if (ensure_soc() < 0) return -1;
    memset(conn, 0, sizeof(HttpConn));
    conn->fd = -1;

    const char *host_part = url;
    if (strncmp(url, "https://", 8) == 0) {
        host_part = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        host_part = url + 7;
    }

    const char *path_part = strchr(host_part, '/');
    if (path_part) {
        size_t host_len = path_part - host_part;
        if (host_len >= sizeof(conn->host)) host_len = sizeof(conn->host) - 1;
        memcpy(conn->host, host_part, host_len);
        conn->host[host_len] = '\0';
    } else {
        strncpy(conn->host, host_part, sizeof(conn->host) - 1);
        path_part = "/";
    }

    int port = 443;
    if (strncmp(url, "https://", 8) != 0) {
        port = 80;
    }
    char *port_sep = strchr(conn->host, ':');
    if (port_sep) {
        *port_sep = '\0';
        port = atoi(port_sep + 1);
    }

    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int ret = getaddrinfo(conn->host, port_str, &hints, &res);
    if (ret != 0) {
        set_err("dns: getaddrinfo(%s) failed: %s", conn->host, gai_strerror(ret));
        return -1;
    }

    int last_errno = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        conn->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (conn->fd < 0) {
            last_errno = errno;
            continue;
        }

        if (connect(conn->fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }

        last_errno = errno;
        close(conn->fd);
        conn->fd = -1;
    }
    freeaddrinfo(res);

    if (!ai) {
        set_err("connect: connect(%s:%d) failed errno=%d", conn->host, port, last_errno);
        return -1;
    }

    if (port == 443) {
        if (tls_connect(conn) < 0) {
            close(conn->fd);
            conn->fd = -1;
            return -1;
        }
    }

    conn->connected = true;

    char request[4096];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Referer: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path_part ? path_part : "/", conn->host, agent, referer);

    if (tls_send(conn, request, strlen(request)) < 0) {
        http_disconnect(conn);
        return -1;
    }

    char header_buf[4096];
    int header_pos = 0;
    int header_done = 0;

    while (!header_done && header_pos < (int)sizeof(header_buf) - 1) {
        int n = tls_recv_timeout(conn, header_buf + header_pos, 1, 10000);
        if (n <= 0) {
            http_disconnect(conn);
            return -1;
        }
        header_pos++;
        if (header_pos >= 4 &&
            header_buf[header_pos-4] == '\r' && header_buf[header_pos-3] == '\n' &&
            header_buf[header_pos-2] == '\r' && header_buf[header_pos-1] == '\n') {
            header_done = 1;
        }
    }

    fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

int http_read_stream(HttpConn *conn, uint8_t *buf, int len) {
    if (!conn || conn->fd < 0) return -1;

    if (conn->use_tls) {
        TlsCtx *tls = (TlsCtx *)conn->ssl_ctx;
        int n = mbedtls_ssl_read(&tls->ssl, buf, len);
        if (n <= 0) {
            if (n == MBEDTLS_ERR_SSL_WANT_READ ||
                n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return 0;
            }
            http_disconnect(conn);
            return -1;
        }
        return n;
    }

    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(conn->fd, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;

    int r = select(conn->fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) return 0;

    int n = recv(conn->fd, buf, len, 0);
    if (n <= 0) {
        http_disconnect(conn);
        return n;
    }
    return n;
}

/* streaming chunked transfer decoder — call after http_get_stream */
static int chunk_remaining = -1;
static int chunk_pos = 0;
static uint8_t chunk_buf[256];

void http_chunked_reset(void) {
    chunk_remaining = -1;
    chunk_pos = 0;
}

int http_read_chunked(HttpConn *conn, uint8_t *buf, int len) {
    if (!conn || conn->fd < 0) return -1;

    int total = 0;
    while (total < len) {
        if (chunk_remaining > 0) {
            int want = len - total;
            if (want > chunk_remaining) want = chunk_remaining;
            int n = tls_recv_timeout(conn, buf + total, want, 0);
            if (n <= 0) return total > 0 ? total : (n < 0 ? -1 : 0);
            total += n;
            chunk_remaining -= n;
            if (chunk_remaining == 0) chunk_remaining = -2;
            continue;
        }
        if (chunk_remaining == -2) {
            uint8_t crlf[2];
            tls_recv_timeout(conn, crlf, 1, 1000);
            tls_recv_timeout(conn, crlf + 1, 1, 1000);
            chunk_remaining = -1;
            continue;
        }
        chunk_pos = 0;
        while (chunk_pos < (int)sizeof(chunk_buf) - 1) {
            int n = tls_recv_timeout(conn, chunk_buf + chunk_pos, 1, 1000);
            if (n <= 0) return total > 0 ? total : (n < 0 ? -1 : 0);
            chunk_pos++;
            if (chunk_pos >= 2 && chunk_buf[chunk_pos-2] == '\r' && chunk_buf[chunk_pos-1] == '\n') break;
        }
        chunk_buf[chunk_pos - 2] = '\0';
        chunk_remaining = (int)strtol((char*)chunk_buf, NULL, 16);
        if (chunk_remaining == 0) {
            uint8_t crlf[2];
            tls_recv_timeout(conn, crlf, 1, 1000);
            tls_recv_timeout(conn, crlf + 1, 1, 1000);
            return total > 0 ? total : -2;
        }
    }
    return total;
}

