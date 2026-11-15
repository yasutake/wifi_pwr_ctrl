#include "common.h"
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LIMIT (1024 * 1024)

struct buffer { unsigned char *data; size_t len; };

static size_t receive_body(char *ptr, size_t size, size_t nmemb, void *arg)
{
    struct buffer *b = arg;
    if (size && nmemb > (LIMIT - b->len) / size) return 0;
    size_t n = size * nmemb;
    unsigned char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(p + b->len, ptr, n);
    b->len += n;
    p[b->len] = 0;
    return n;
}

static size_t receive_header(char *ptr, size_t size, size_t nmemb, void *arg)
{
    struct klap *s = arg;
    size_t n = size * nmemb;
    const char prefix[] = "Set-Cookie: TP_SESSIONID=";
    if (n > sizeof(prefix) - 1 && !strncasecmp(ptr, prefix, sizeof(prefix) - 1)) {
        size_t start = sizeof(prefix) - 1, end = start;
        while (end < n && ptr[end] != ';' && ptr[end] != '\r' && ptr[end] != '\n') end++;
        if (end > start && end - start < sizeof(s->cookie) - 14) {
            memcpy(s->cookie, "TP_SESSIONID=", 13);
            memcpy(s->cookie + 13, ptr + start, end - start);
            s->cookie[13 + end - start] = 0;
        }
    }
    return n;
}

static int post(struct klap *s, const char *path, const void *data, size_t len,
                struct buffer *out)
{
    CURL *c = s->curl;
    char url[256], err[CURL_ERROR_SIZE] = {0};
    long status = 0;
    snprintf(url, sizeof(url), "%s%s", s->base, path);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(c, CURLOPT_COOKIE, s->cookie[0] ? s->cookie : NULL);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_PROXY, "");
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        fprintf(stderr, "Tapo network request failed: %s\n", err[0] ? err : curl_easy_strerror(rc));
        return 0;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) { fprintf(stderr, "Tapo HTTP error: %ld\n", status); return 0; }
    return 1;
}

static int digest(const EVP_MD *md, const void *data, size_t len, unsigned char *out)
{
    unsigned int n;
    if (!EVP_Digest(data, len, out, &n, md, NULL)) {
        fprintf(stderr, "Cryptographic digest failed\n");
        return 0;
    }
    return 1;
}

static int derive(const char *prefix, const unsigned char *local,
                  const unsigned char *remote, const unsigned char *auth,
                  unsigned char *out)
{
    unsigned char data[67];
    size_t n = strlen(prefix);
    memcpy(data, prefix, n);
    memcpy(data + n, local, 16);
    memcpy(data + n + 16, remote, 16);
    memcpy(data + n + 32, auth, 32);
    return digest(EVP_sha256(), data, n + 64, out);
}

int klap_open(struct klap *s, const char *ip, int port,
              const char *username, const char *password)
{
    unsigned char local[16], remote[16], hashes[40], auth[32], tmp[64], d[32], material[96];
    struct buffer response = {0};
    int ok = 0;
    memset(s, 0, sizeof(*s));
    snprintf(s->base, sizeof(s->base), "http://%s:%d/app/", ip, port);
    s->curl = curl_easy_init();
    if (!s->curl) { fprintf(stderr, "Unable to initialize curl\n"); goto done; }
    if (RAND_bytes(local, sizeof(local)) != 1 ||
        !digest(EVP_sha1(), username, strlen(username), hashes) ||
        !digest(EVP_sha1(), password, strlen(password), hashes + 20) ||
        !digest(EVP_sha256(), hashes, sizeof(hashes), auth)) {
        fprintf(stderr, "Unable to initialize KLAP authentication\n"); goto done;
    }
    if (RAND_bytes(tmp, 16) != 1 || EVP_EncodeBlock((unsigned char *)s->terminal_uuid, tmp, 16) != 24) {
        fprintf(stderr, "Unable to generate terminal ID\n"); goto done;
    }
    if (!post(s, "handshake1", local, sizeof(local), &response)) goto done;
    if (response.len != 48 || !s->cookie[0]) {
        fprintf(stderr, "Invalid KLAP handshake response or missing session cookie\n"); goto done;
    }
    memcpy(remote, response.data, 16);
    memcpy(material, local, 16);
    memcpy(material + 16, remote, 16);
    memcpy(material + 32, auth, 32);
    if (!digest(EVP_sha256(), material, 64, d)) goto done;
    if (CRYPTO_memcmp(d, response.data + 16, 32)) {
        fprintf(stderr, "KLAP authentication failed; check credentials and protocol\n"); goto done;
    }
    memcpy(tmp, remote, 16);
    memcpy(tmp + 16, local, 16);
    memcpy(tmp + 32, auth, 32);
    if (!digest(EVP_sha256(), tmp, sizeof(tmp), d)) goto done;
    free(response.data); response = (struct buffer){0};
    if (!post(s, "handshake2", d, 32, &response)) goto done;
    if (!derive("lsk", local, remote, auth, d)) goto done;
    memcpy(s->key, d, 16);
    if (!derive("iv", local, remote, auth, d)) goto done;
    memcpy(s->iv, d, 12);
    s->seq = ((uint32_t)d[28] << 24) | ((uint32_t)d[29] << 16) |
             ((uint32_t)d[30] << 8) | d[31];
    if (!derive("ldk", local, remote, auth, d)) goto done;
    memcpy(s->sig, d, 28);
    ok = 1;
done:
    OPENSSL_cleanse(hashes, sizeof(hashes));
    OPENSSL_cleanse(auth, sizeof(auth));
    OPENSSL_cleanse(material, sizeof(material));
    OPENSSL_cleanse(tmp, sizeof(tmp));
    free(response.data);
    if (!ok) klap_close(s);
    return ok;
}

void klap_close(struct klap *s)
{
    if (s->curl) curl_easy_cleanup(s->curl);
    OPENSSL_cleanse(s, sizeof(*s));
}

static int crypt(int encrypt, const unsigned char *key, const unsigned char *iv,
                 const unsigned char *input, int len, unsigned char *out, int *outlen)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, m = 0;
    int ok = ctx && EVP_CipherInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv, encrypt) &&
             EVP_CipherUpdate(ctx, out, &n, input, len) &&
             EVP_CipherFinal_ex(ctx, out + n, &m);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { fprintf(stderr, "KLAP encryption/decryption failed\n"); return 0; }
    *outlen = n + m;
    return 1;
}

json_object *klap_query(struct klap *s, const char *method, json_object *params)
{
    json_object *req = json_object_new_object(), *reply = NULL;
    unsigned char iv[16], digest_input[28 + 4 + LIMIT], hash[32];
    unsigned char *payload = NULL, *plain = NULL;
    struct buffer response = {0};
    int encrypted_len, plain_len;
    char path[80];
    if (!req) { fprintf(stderr, "Out of memory\n"); goto done; }
    json_object_object_add(req, "method", json_object_new_string(method));
    json_object_object_add(req, "request_time_milis", json_object_new_int64((int64_t)time(NULL) * 1000));
    json_object_object_add(req, "terminal_uuid", json_object_new_string(s->terminal_uuid));
    if (params) json_object_object_add(req, "params", json_object_get(params));
    const char *text = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
    size_t len = strlen(text);
    if (len > LIMIT - 16) { fprintf(stderr, "KLAP request too large\n"); goto done; }
    payload = malloc(len + 48);
    if (!payload) { perror("malloc"); goto done; }
    s->seq++;
    memcpy(iv, s->iv, 12);
    iv[12] = (unsigned char)(s->seq >> 24);
    iv[13] = (unsigned char)(s->seq >> 16);
    iv[14] = (unsigned char)(s->seq >> 8);
    iv[15] = (unsigned char)s->seq;
    if (!crypt(1, s->key, iv, (const unsigned char *)text, (int)len, payload + 32, &encrypted_len)) goto done;
    memcpy(digest_input, s->sig, 28);
    memcpy(digest_input + 28, iv + 12, 4);
    memcpy(digest_input + 32, payload + 32, (size_t)encrypted_len);
    if (!digest(EVP_sha256(), digest_input, 32 + (size_t)encrypted_len, payload)) goto done;
    snprintf(path, sizeof(path), "request?seq=%lld",
             (long long)(s->seq <= INT32_MAX ? (int64_t)s->seq : (int64_t)s->seq - 4294967296LL));
    if (!post(s, path, payload, 32 + (size_t)encrypted_len, &response)) goto done;
    if (response.len < 48 || (response.len - 32) % 16) {
        fprintf(stderr, "Invalid KLAP response length\n"); goto done;
    }
    memcpy(digest_input + 32, response.data + 32, response.len - 32);
    if (!digest(EVP_sha256(), digest_input, response.len, hash)) goto done;
    if (CRYPTO_memcmp(hash, response.data, 32)) {
        fprintf(stderr, "Invalid KLAP response signature\n"); goto done;
    }
    plain = malloc(response.len);
    if (!plain) { perror("malloc"); goto done; }
    if (!crypt(0, s->key, iv, response.data + 32, (int)response.len - 32, plain, &plain_len)) goto done;
    reply = parse_json(plain, (size_t)plain_len);
    if (reply && !device_ok(reply, method, "error_code")) {
        json_object_put(reply);
        reply = NULL;
    }
done:
    json_object_put(req);
    free(payload);
    free(response.data);
    free(plain);
    return reply;
}
