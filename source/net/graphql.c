#include "graphql.h"
#include "../util/cjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#define MAX_RESPONSE (256 * 1024)
#define ALLANIME_SOURCE_QUERY_HASH "d405d0edd690624b66baba3068e0edc3ac90f1597d898a1ec8db4e5c43c00fec"

static cJSON *get_payload_root(char *resp, char **decoded_out) {
    *decoded_out = NULL;

    cJSON *root = cJSON_Parse(resp);
    if (!root) return NULL;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *wrapped = data ? cJSON_GetObjectItem(data, "tobeparsed") : NULL;
    if (!wrapped || !cJSON_IsString(wrapped)) return root;

    const char *encoded = wrapped->valuestring;
    size_t encoded_len = strlen(encoded);
    unsigned char *decoded = (unsigned char *)malloc(encoded_len + 1);
    if (!decoded) return root;

    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, encoded_len + 1, &decoded_len,
                              (const unsigned char *)encoded, encoded_len) != 0 ||
        decoded_len <= 29) {
        free(decoded);
        return root;
    }

    size_t cipher_len = decoded_len - 13 - 16;
    char *plain = (char *)malloc(cipher_len + 1);
    if (!plain) {
        free(decoded);
        return root;
    }

    unsigned char key[32];
    mbedtls_sha256_ret((const unsigned char *)"Xot36i3lK3:v1", 12, key, 0);

    unsigned char nonce_counter[16] = {0};
    memcpy(nonce_counter, decoded + 1, 12);
    nonce_counter[15] = 2;

    unsigned char stream_block[16] = {0};
    size_t nc_off = 0;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 256);
    mbedtls_aes_crypt_ctr(&aes, cipher_len, &nc_off, nonce_counter,
                          stream_block, decoded + 13,
                          (unsigned char *)plain);
    mbedtls_aes_free(&aes);
    free(decoded);

    plain[cipher_len] = '\0';
    cJSON *decoded_root = cJSON_Parse(plain);
    if (!decoded_root) {
        free(plain);
        return root;
    }

    cJSON_Delete(root);
    *decoded_out = plain;
    return decoded_root;
}

static cJSON *data_or_root(cJSON *root) {
    cJSON *data = cJSON_GetObjectItem(root, "data");
    return data ? data : root;
}

static void url_encode(const char *in, char *out, int out_size) {
    static const char hex[] = "0123456789ABCDEF";
    int pos = 0;
    while (*in && pos < out_size - 1) {
        unsigned char c = (unsigned char)*in++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[pos++] = (char)c;
        } else if (c == ' ') {
            out[pos++] = '+';
        } else if (pos < out_size - 3) {
            out[pos++] = '%';
            out[pos++] = hex[c >> 4];
            out[pos++] = hex[c & 15];
        } else {
            break;
        }
    }
    out[pos] = '\0';
}

static int jikan_search_fallback(const Config *cfg, const char *query,
                                 AnimeResult *results, int max_results) {
    char encoded[512];
    url_encode(query, encoded, sizeof(encoded));

    char url[768];
    snprintf(url, sizeof(url), "https://api.jikan.moe/v4/anime?q=%s&limit=%d",
             encoded, max_results);

    char *resp = (char *)malloc(MAX_RESPONSE);
    if (!resp) return -1;

    int len = http_get(url, cfg->referer, cfg->agent, resp, MAX_RESPONSE);
    if (len <= 0) {
        free(resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!data || !cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, data) {
        if (count >= max_results) break;
        cJSON *mal_id = cJSON_GetObjectItem(item, "mal_id");
        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *episodes = cJSON_GetObjectItem(item, "episodes");
        if (!title || !cJSON_IsString(title)) continue;

        if (mal_id && cJSON_IsNumber(mal_id)) {
            snprintf(results[count].id, sizeof(results[count].id),
                     "JIKAN:%d", mal_id->valueint);
        } else {
            snprintf(results[count].id, sizeof(results[count].id),
                     "JIKAN:%d", count);
        }
        strncpy(results[count].name, title->valuestring, 255);
        results[count].name[255] = '\0';
        results[count].episode_count =
            (episodes && cJSON_IsNumber(episodes)) ? episodes->valueint : 0;
        count++;
    }

    cJSON_Delete(root);
    return count;
}

static int proxy_get_json(const Config *cfg, const char *path,
                          cJSON **root_out, char **resp_out) {
    *root_out = NULL;
    *resp_out = NULL;
    if (cfg->proxy_base[0] == '\0') return -1;

    char url[1024];
    snprintf(url, sizeof(url), "%s%s", cfg->proxy_base, path);

    char *resp = (char *)malloc(MAX_RESPONSE);
    if (!resp) return -1;

    int len = httpc_get(url, cfg->agent, resp, MAX_RESPONSE);
    if (len <= 0) {
        free(resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        free(resp);
        return -1;
    }

    *root_out = root;
    *resp_out = resp;
    return 0;
}

static int proxy_search(const Config *cfg, const char *query,
                        AnimeResult *results, int max_results) {
    char encoded[512];
    url_encode(query, encoded, sizeof(encoded));

    char path[768];
    snprintf(path, sizeof(path), "/search?q=%s", encoded);

    cJSON *root = NULL;
    char *resp = NULL;
    if (proxy_get_json(cfg, path, &root, &resp) < 0) return -1;

    cJSON *items = cJSON_GetObjectItem(root, "results");
    if (!items || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        free(resp);
        return -1;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (count >= max_results) break;
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *episode_count = cJSON_GetObjectItem(item, "episode_count");
        if (!id || !cJSON_IsString(id) || !name || !cJSON_IsString(name)) continue;

        strncpy(results[count].id, id->valuestring, 63);
        results[count].id[63] = '\0';
        strncpy(results[count].name, name->valuestring, 255);
        results[count].name[255] = '\0';
        results[count].episode_count =
            (episode_count && cJSON_IsNumber(episode_count)) ? episode_count->valueint : 0;
        count++;
    }

    cJSON_Delete(root);
    free(resp);
    return count;
}

static int proxy_episodes(const Config *cfg, const char *show_id,
                          Episode *episodes, int max_episodes) {
    char encoded[128];
    url_encode(show_id, encoded, sizeof(encoded));

    char path[256];
    snprintf(path, sizeof(path), "/episodes?id=%s", encoded);

    cJSON *root = NULL;
    char *resp = NULL;
    if (proxy_get_json(cfg, path, &root, &resp) < 0) return -1;

    cJSON *items = cJSON_GetObjectItem(root, "episodes");
    if (!items || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        free(resp);
        return -1;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (count >= max_episodes) break;
        if (cJSON_IsString(item)) {
            strncpy(episodes[count].number, item->valuestring, 15);
            episodes[count].number[15] = '\0';
            count++;
        }
    }

    cJSON_Delete(root);
    free(resp);
    return count;
}

static int proxy_sources(const Config *cfg, const char *show_id,
                         const char *episode_str, EmbedSource *sources,
                         int max_sources) {
    char encoded_id[128];
    char encoded_ep[64];
    url_encode(show_id, encoded_id, sizeof(encoded_id));
    url_encode(episode_str, encoded_ep, sizeof(encoded_ep));

    char path[320];
    snprintf(path, sizeof(path), "/sources?id=%s&episode=%s",
             encoded_id, encoded_ep);

    cJSON *root = NULL;
    char *resp = NULL;
    if (proxy_get_json(cfg, path, &root, &resp) < 0) return -1;

    cJSON *items = cJSON_GetObjectItem(root, "sources");
    if (!items || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        free(resp);
        return -1;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (count >= max_sources) break;
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!url || !cJSON_IsString(url)) continue;
        strncpy(sources[count].url, url->valuestring, 1023);
        sources[count].url[1023] = '\0';
        if (name && cJSON_IsString(name)) {
            strncpy(sources[count].source_name, name->valuestring, 63);
            sources[count].source_name[63] = '\0';
        }
        count++;
    }

    cJSON_Delete(root);
    free(resp);
    return count;
}

static int graphql_post(HttpConn *conn, const Config *cfg, const char *json_body,
                        char *out_buf, int out_buf_size) {
    char host[256];
    snprintf(host, sizeof(host), "%s", cfg->api_base);

    int ret = http_connect(conn, host, 443);
    if (ret < 0) return -1;

    ret = http_post(conn, "/api", cfg->api_base, cfg->referer, cfg->agent,
                    json_body, out_buf, out_buf_size);
    http_disconnect(conn);
    return ret;
}

int graphql_search(HttpConn *conn, const Config *cfg, const char *query,
                   AnimeResult *results, int max_results) {
    if (cfg->proxy_base[0] != '\0') {
        int proxy_count = proxy_search(cfg, query, results, max_results);
        if (proxy_count ==  0) {
            int jc = jikan_search_fallback(cfg, query, results, max_results);
            return jc > 0 ? jc : 0;
        }
        return proxy_count;
    }

    char body[4096];
    snprintf(body, sizeof(body),
        "{\"query\":\"query($search:SearchInput$limit:Int)"
        "{shows(search:$search limit:$limit){edges{_id name availableEpisodes}}}\","
        "\"variables\":{\"search\":{\"query\":\"%s\"},\"limit\":%d}}",
        query, max_results);

    char *resp = (char *)malloc(MAX_RESPONSE);
    if (!resp) return -1;

    int len = graphql_post(conn, cfg, body, resp, MAX_RESPONSE);
    if (len <= 0) {
        free(resp);
        return jikan_search_fallback(cfg, query, results, max_results);
    }

    char *decoded = NULL;
    cJSON *root = get_payload_root(resp, &decoded);
    free(resp);
    if (!root) return jikan_search_fallback(cfg, query, results, max_results);

    cJSON *data = data_or_root(root);
    if (!data) {
        cJSON_Delete(root);
        free(decoded);
        return jikan_search_fallback(cfg, query, results, max_results);
    }

    cJSON *shows = cJSON_GetObjectItem(data, "shows");
    if (!shows) {
        cJSON_Delete(root);
        free(decoded);
        return jikan_search_fallback(cfg, query, results, max_results);
    }

    cJSON *edges = cJSON_GetObjectItem(shows, "edges");
    if (!edges || !cJSON_IsArray(edges)) {
        cJSON_Delete(root);
        free(decoded);
        return jikan_search_fallback(cfg, query, results, max_results);
    }

    int count = 0;
    cJSON *edge = NULL;
    cJSON_ArrayForEach(edge, edges) {
        if (count >= max_results) break;
        cJSON *id = cJSON_GetObjectItem(edge, "_id");
        cJSON *name = cJSON_GetObjectItem(edge, "name");
        cJSON *eps = cJSON_GetObjectItem(edge, "availableEpisodes");

        if (!id || !cJSON_IsString(id) || !name || !cJSON_IsString(name)) continue;
        strncpy(results[count].id, id->valuestring, 63);
        results[count].id[63] = '\0';
        strncpy(results[count].name, name->valuestring, 255);
        results[count].name[255] = '\0';
        results[count].episode_count = 0;
        if (eps && cJSON_IsNumber(eps)) {
            results[count].episode_count = eps->valueint;
        } else if (eps && cJSON_IsObject(eps)) {
            cJSON *sub = cJSON_GetObjectItem(eps, "sub");
            cJSON *dub = cJSON_GetObjectItem(eps, "dub");
            if (sub && cJSON_IsNumber(sub)) results[count].episode_count = sub->valueint;
            else if (dub && cJSON_IsNumber(dub)) results[count].episode_count = dub->valueint;
        }
        count++;
    }

    cJSON_Delete(root);
    free(decoded);
    return count > 0 ? count : jikan_search_fallback(cfg, query, results, max_results);
}

int graphql_episodes(HttpConn *conn, const Config *cfg, const char *show_id,
                     Episode *episodes, int max_episodes) {
    if (cfg->proxy_base[0] != '\0') {
        int proxy_count = proxy_episodes(cfg, show_id, episodes, max_episodes);
        if (proxy_count > 0) return proxy_count;
    }

    char body[2048];
    snprintf(body, sizeof(body),
        "{\"query\":\"query($id:String!){show(_id:$id){availableEpisodesDetail}}\","
        "\"variables\":{\"id\":\"%s\"}}",
        show_id);

    char *resp = (char *)malloc(MAX_RESPONSE);
    if (!resp) return -1;

    int len = graphql_post(conn, cfg, body, resp, MAX_RESPONSE);
    if (len <= 0) { free(resp); return -1; }

    char *decoded = NULL;
    cJSON *root = get_payload_root(resp, &decoded);
    free(resp);
    if (!root) return -1;

    cJSON *data = data_or_root(root);
    if (!data) { cJSON_Delete(root); return -1; }

    cJSON *show = cJSON_GetObjectItem(data, "show");
    if (!show) { cJSON_Delete(root); return -1; }

    cJSON *ep_detail = cJSON_GetObjectItem(show, "availableEpisodesDetail");
    cJSON *ep_list = ep_detail;
    if (ep_detail && cJSON_IsObject(ep_detail)) {
        ep_list = cJSON_GetObjectItem(ep_detail, "sub");
        if (!ep_list || !cJSON_IsArray(ep_list)) {
            ep_list = cJSON_GetObjectItem(ep_detail, "dub");
        }
    }
    if (!ep_list || !cJSON_IsArray(ep_list)) { cJSON_Delete(root); return -1; }

    int count = 0;
    cJSON *ep = NULL;
    cJSON_ArrayForEach(ep, ep_list) {
        if (count >= max_episodes) break;
        if (cJSON_IsString(ep)) {
            strncpy(episodes[count].number, ep->valuestring, 15);
            episodes[count].number[15] = '\0';
        } else if (cJSON_IsNumber(ep)) {
            snprintf(episodes[count].number, 15, "%d", ep->valueint);
        }
        count++;
    }

    cJSON_Delete(root);
    free(decoded);
    return count;
}

int graphql_sources(HttpConn *conn, const Config *cfg, const char *show_id,
                    const char *episode_str, EmbedSource *sources, int max_sources) {
    if (cfg->proxy_base[0] != '\0') {
        int proxy_count = proxy_sources(cfg, show_id, episode_str, sources, max_sources);
        if (proxy_count > 0) return proxy_count;
    }

    char body[2048];
    snprintf(body, sizeof(body),
        "{\"variables\":{\"showId\":\"%s\",\"translationType\":\"sub\","
        "\"episodeString\":\"%s\"},"
        "\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"%s\"}}}",
        show_id, episode_str, ALLANIME_SOURCE_QUERY_HASH);

    char fallback_body[2048];
    snprintf(fallback_body, sizeof(fallback_body),
        "{\"query\":\"query($showId:String!$translationType:VaildTranslationTypeEnumType!"
        "$episodeString:String!){episode(showId:$showId translationType:$translationType"
        " episodeString:$episodeString){sourceUrls}}\","
        "\"variables\":{\"showId\":\"%s\",\"translationType\":\"sub\","
        "\"episodeString\":\"%s\"}}",
        show_id, episode_str);

    char *resp = (char *)malloc(MAX_RESPONSE);
    if (!resp) return -1;

    int len = graphql_post(conn, cfg, body, resp, MAX_RESPONSE);
    if (len <= 0) { free(resp); return -1; }

    char *decoded = NULL;
    cJSON *root = get_payload_root(resp, &decoded);
    if (root) {
        cJSON *payload = data_or_root(root);
        cJSON *episode_check = cJSON_GetObjectItem(payload, "episode");
        if (!episode_check || cJSON_IsNull(episode_check)) {
            cJSON_Delete(root);
            free(decoded);
            decoded = NULL;
            len = graphql_post(conn, cfg, fallback_body, resp, MAX_RESPONSE);
            if (len <= 0) { free(resp); return -1; }
            root = get_payload_root(resp, &decoded);
        }
    }
    free(resp);
    if (!root) return -1;

    cJSON *data = data_or_root(root);
    if (!data) { cJSON_Delete(root); return -1; }

    cJSON *episode = cJSON_GetObjectItem(data, "episode");
    if (!episode) { cJSON_Delete(root); return -1; }

    cJSON *src_urls = cJSON_GetObjectItem(episode, "sourceUrls");
    if (!src_urls || !cJSON_IsArray(src_urls)) { cJSON_Delete(root); return -1; }

    int count = 0;
    cJSON *src = NULL;
    cJSON_ArrayForEach(src, src_urls) {
        if (count >= max_sources) break;
        cJSON *url = cJSON_GetObjectItem(src, "sourceUrl");
        cJSON *name = cJSON_GetObjectItem(src, "sourceName");

        if (!url || !cJSON_IsString(url)) continue;
        strncpy(sources[count].url, url->valuestring, 1023);
        sources[count].url[1023] = '\0';
        if (name && cJSON_IsString(name)) {
            strncpy(sources[count].source_name, name->valuestring, 63);
            sources[count].source_name[63] = '\0';
        }
        count++;
    }

    cJSON_Delete(root);
    free(decoded);
    return count;
}
