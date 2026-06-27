#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int  fd;
    char host[256];
    int  port;
    bool connected;
    bool use_tls;
    void *ssl_ctx;
} HttpConn;

int  http_connect(HttpConn *conn, const char *host, int port);
void http_disconnect(HttpConn *conn);
int  http_post(HttpConn *conn, const char *path, const char *host_header,
               const char *referer, const char *agent, const char *body,
               char *out_buf, int out_buf_size);
int  http_get(const char *url, const char *referer, const char *agent,
              char *out_buf, int out_buf_size);
int  http_get_stream(HttpConn *conn, const char *url, const char *referer,
                     const char *agent);
int  http_read_stream(HttpConn *conn, uint8_t *buf, int len);
int  http_read_chunked(HttpConn *conn, uint8_t *buf, int len);
void http_chunked_reset(void);
const char *http_last_error(void);
int  httpc_get(const char *url, const char *agent, char *out_buf, int out_buf_size);
int  httpc_download(const char *url, const char *agent, uint8_t *out_buf, int out_buf_size);
int  httpc_post_json(const char *url, const char *agent, const char *body, char *out_buf, int out_buf_size);
typedef void (*httpc_progress_cb)(int downloaded, int total, void *userdata);
int  httpc_download_file(const char *url, const char *agent, const char *filepath,
                         httpc_progress_cb cb, void *userdata);
int  sock_get(const char *url, const char *agent, uint8_t *out_buf, int out_buf_size);
int  http_soc_init(void);
int  http_stream_open(const char *url, const char *agent);
int  http_stream_read(int fd, uint8_t *buf, int len);
void http_stream_close(int fd);

#endif
