/* xmbget [-T сек] [-c cacert.pem] -O файл URL — скачать по HTTP/HTTPS (для подкастов и текстов песен).
 * busybox wget прошивки не договаривается по TLS с CDN (Akamai, Spotify — рукопожатие висит до таймаута), а системные
 * libcurl/libssl — под glibc, к статическому XMB их не подключить. Здесь — mbedTLS: TLS 1.2 и 1.3, SNI, проверка
 * сертификата по набору Mozilla (cacert.pem рядом с программой), переходы по Location, chunked.
 * Код возврата: 0 — скачано (200), 1 — ошибка (сообщение в stderr), 2 — сервер ответил «нет» (4xx: 404 и т. п.),
 * 3 — сервер сбоит (5xx: 503 и т. п.) — стоит повторить позже. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"

static int timeout_s = 20;
static int dbg;   /* XMBGET_DEBUG=1 — шаги в stderr */
#define DBG(...) do { if (dbg) fprintf(stderr, "xmbget: " __VA_ARGS__); } while (0)
static const char *ca_file;

typedef struct {
	int fd, tls;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
} conn_t;

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt ca;
static int ca_loaded;

static int net_send(void *ctx, const unsigned char *buf, size_t len)
{
	ssize_t n = send(*(int *)ctx, buf, len, MSG_NOSIGNAL);
	if (n < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED;
	return (int)n;
}

static int net_recv(void *ctx, unsigned char *buf, size_t len)
{
	ssize_t n = recv(*(int *)ctx, buf, len, 0);
	if (n < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
	return (int)n;
}

/* подключение с таймаутом: неблокирующий connect, дальше чтение/запись — с SO_RCVTIMEO/SO_SNDTIMEO */
static int tcp_connect(const char *host, const char *port)
{
	struct addrinfo hints = { 0 }, *res, *a;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	DBG("адрес %s…\n", host);
	if (getaddrinfo(host, port, &hints, &res)) {
		fprintf(stderr, "xmbget: адрес %s не найден\n", host);
		return -1;
	}
	int fd = -1;
	for (a = res; a && fd < 0; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0)
			continue;
		int fl = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		int r = connect(fd, a->ai_addr, a->ai_addrlen);
		if (r < 0 && errno == EINPROGRESS) {
			struct pollfd p = { fd, POLLOUT, 0 };
			int err = 0;
			socklen_t el = sizeof(err);
			r = poll(&p, 1, timeout_s * 1000) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) && !err ? 0 : -1;
		}
		fcntl(fd, F_SETFL, fl);
		if (r < 0) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(res);
	DBG("подключение: %s\n", fd < 0 ? "нет" : "есть");
	if (fd < 0) {
		fprintf(stderr, "xmbget: не подключиться к %s:%s\n", host, port);
		return -1;
	}
	struct timeval tv = { timeout_s, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	return fd;
}

static int c_write(conn_t *c, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	while (len) {
		int n = c->tls ? mbedtls_ssl_write(&c->ssl, p, len) : (int)send(c->fd, p, len, MSG_NOSIGNAL);
		if (c->tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE))
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* прочитать что есть: > 0 — байт, 0 — соединение закрыто, < 0 — ошибка */
static int c_read(conn_t *c, void *buf, size_t len)
{
	for (;;) {
		int n = c->tls ? mbedtls_ssl_read(&c->ssl, buf, len) : (int)recv(c->fd, buf, len, 0);
		if (c->tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
			       n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET))
			continue;
		if (c->tls && n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		return n;
	}
}

static void c_close(conn_t *c)
{
	if (c->tls) {
		mbedtls_ssl_close_notify(&c->ssl);
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->conf);
	}
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}

static int c_open(conn_t *c, int tls, const char *host, const char *port)
{
	memset(c, 0, sizeof(*c));
	c->fd = tcp_connect(host, port);
	if (c->fd < 0)
		return -1;
	c->tls = tls;
	if (!tls)
		return 0;
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &drbg);
	if (ca_loaded) {
		mbedtls_ssl_conf_ca_chain(&c->conf, &ca, NULL);
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	} else {
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
	}
	if (mbedtls_ssl_setup(&c->ssl, &c->conf) || mbedtls_ssl_set_hostname(&c->ssl, host)) {
		c_close(c);
		return -1;
	}
	mbedtls_ssl_set_bio(&c->ssl, &c->fd, net_send, net_recv, NULL);
	DBG("TLS…\n");
	int r;
	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
			char e[128];
			mbedtls_strerror(r, e, sizeof(e));
			fprintf(stderr, "xmbget: TLS с %s: %s\n", host, e);
			c_close(c);
			return -1;
		}
	DBG("TLS готов: %s\n", mbedtls_ssl_get_version(&c->ssl));
	return 0;
}

typedef struct {
	char scheme[8], host[256], port[8], path[2048];
} url_t;

static int parse_url(const char *u, url_t *o, const url_t *base)
{
	memset(o, 0, sizeof(*o));
	if (u[0] == '/' && base) {   /* относительный Location */
		*o = *base;
		snprintf(o->path, sizeof(o->path), "%s", u);
		return 0;
	}
	const char *p = strstr(u, "://");
	if (!p)
		return -1;
	snprintf(o->scheme, sizeof(o->scheme), "%.*s", (int)(p - u), u);
	p += 3;
	const char *slash = strchr(p, '/');
	int hl = slash ? (int)(slash - p) : (int)strlen(p);
	char hp[300];
	snprintf(hp, sizeof(hp), "%.*s", hl, p);
	char *colon = strrchr(hp, ':');
	if (colon && !strchr(colon, ']')) {
		*colon = 0;
		snprintf(o->port, sizeof(o->port), "%s", colon + 1);
	} else {
		snprintf(o->port, sizeof(o->port), "%s", !strcasecmp(o->scheme, "https") ? "443" : "80");
	}
	snprintf(o->host, sizeof(o->host), "%s", hp);
	snprintf(o->path, sizeof(o->path), "%s", slash ? slash : "/");
	return strcasecmp(o->scheme, "https") && strcasecmp(o->scheme, "http") ? -1 : 0;
}

/* буферизованное чтение ответа: строки заголовка и тело */
static unsigned char rbuf[16384];
static size_t rpos, rlen;

static int rfill(conn_t *c)
{
	if (rpos < rlen)
		return 1;
	int n = c_read(c, rbuf, sizeof(rbuf));
	if (n < 0)
		DBG("чтение: ошибка -0x%04x\n", -n);
	if (n <= 0)
		return n;
	rpos = 0;
	rlen = (size_t)n;
	return 1;
}

static int rline(conn_t *c, char *out, int cap)
{
	int k = 0;
	for (;;) {
		if (rfill(c) <= 0)
			return k ? k : -1;
		char ch = (char)rbuf[rpos++];
		if (ch == '\n')
			break;
		if (ch != '\r' && k < cap - 1)
			out[k++] = ch;
	}
	out[k] = 0;
	return k;
}

/* тело: n байт (n < 0 — до закрытия) в f */
static long long body(conn_t *c, FILE *f, long long n)
{
	long long got = 0;
	while (n < 0 || got < n) {
		int r = rfill(c);
		if (r <= 0)
			return n < 0 && r == 0 ? got : -1;
		size_t take = rlen - rpos;
		if (n >= 0 && (long long)take > n - got)
			take = (size_t)(n - got);
		if (fwrite(rbuf + rpos, 1, take, f) != take)
			return -1;
		rpos += take;
		got += (long long)take;
	}
	return got;
}

static int get(const char *u, const char *out)
{
	url_t url, next;
	if (parse_url(u, &url, NULL)) {
		fprintf(stderr, "xmbget: не адрес: %s\n", u);
		return 1;
	}
	for (int hop = 0; hop < 10; hop++) {
		conn_t c;
		if (c_open(&c, !strcasecmp(url.scheme, "https"), url.host, url.port))
			return 1;
		char req[2600];
		int len = snprintf(req, sizeof(req),
				   "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: XMB/1.0 (github.com/Anded116/r3ii-xmb)\r\n"
				   "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
				   url.path, url.host);
		rpos = rlen = 0;
		char line[4096], loc[2048] = "";
		int status = 0, chunked = 0;
		long long clen = -1;
		if (c_write(&c, req, (size_t)len) || rline(&c, line, sizeof(line)) < 0 ||
		    sscanf(line, "HTTP/%*s %d", &status) != 1) {
			fprintf(stderr, "xmbget: нет ответа от %s\n", url.host);
			c_close(&c);
			return 1;
		}
		while (rline(&c, line, sizeof(line)) > 0) {
			if (!strncasecmp(line, "Location:", 9))
				snprintf(loc, sizeof(loc), "%s", line + 9 + strspn(line + 9, " "));
			else if (!strncasecmp(line, "Content-Length:", 15))
				clen = atoll(line + 15);
			else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line, "chunked"))
				chunked = 1;
		}
		DBG("ответ %d, длина %lld%s\n", status, clen, chunked ? ", chunked" : "");
		if (status >= 300 && status < 400 && loc[0]) {
			c_close(&c);
			if (parse_url(loc, &next, &url)) {
				fprintf(stderr, "xmbget: переход по странному адресу: %s\n", loc);
				return 1;
			}
			url = next;
			continue;
		}
		if (status != 200) {
			fprintf(stderr, "xmbget: %s ответил %d\n", url.host, status);
			c_close(&c);
			return status >= 500 ? 3 : 2;
		}
		FILE *f = fopen(out, "wb");
		if (!f) {
			fprintf(stderr, "xmbget: не записать %s\n", out);
			c_close(&c);
			return 1;
		}
		long long got = 0;
		int ok = 1;
		if (chunked) {
			for (;;) {
				if (rline(&c, line, sizeof(line)) < 0) {
					ok = 0;
					break;
				}
				long long n = strtoll(line, NULL, 16);
				DBG("кусок «%s» = %lld\n", line, n);
				if (n <= 0)
					break;
				if (body(&c, f, n) != n || rline(&c, line, sizeof(line)) < 0) {
					ok = 0;
					break;
				}
				got += n;
			}
		} else {
			got = body(&c, f, clen);
			ok = got >= 0 && (clen < 0 || got == clen);
		}
		long long size = ftell(f);   /* сколько пришло — и при обрыве, когда body() вернула -1 */
		ok &= fclose(f) == 0;
		c_close(&c);
		if (!ok) {
			fprintf(stderr, "xmbget: оборвалось на %lld байтах\n", size);
			unlink(out);
			return 1;
		}
		return 0;
	}
	fprintf(stderr, "xmbget: слишком много переходов\n");
	return 1;
}

int main(int argc, char **argv)
{
	const char *out = NULL, *u = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-T") && i + 1 < argc)
			timeout_s = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			ca_file = argv[++i];
		else if (!strcmp(argv[i], "-O") && i + 1 < argc)
			out = argv[++i];
		else
			u = argv[i];
	}
	if (!out || !u) {
		fprintf(stderr, "xmbget [-T сек] [-c cacert.pem] -O файл URL\n");
		return 1;
	}
	dbg = getenv("XMBGET_DEBUG") != NULL;
	psa_crypto_init();
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"xmbget", 6)) {
		fprintf(stderr, "xmbget: нет случайных чисел\n");
		return 1;
	}
	mbedtls_x509_crt_init(&ca);
	if (!ca_file) {   /* набор сертификатов — рядом с программой */
		static char def[1100];
		ssize_t n = readlink("/proc/self/exe", def, sizeof(def) - 16);
		if (n > 0) {
			def[n] = 0;
			char *s = strrchr(def, '/');
			if (s) {
				strcpy(s + 1, "cacert.pem");
				ca_file = def;
			}
		}
	}
	ca_loaded = ca_file && mbedtls_x509_crt_parse_file(&ca, ca_file) >= 0 && ca.raw.len;
	DBG("сертификаты: %s\n", ca_loaded ? "есть" : "нет");
	if (!ca_loaded)
		fprintf(stderr, "xmbget: без проверки сертификатов (нет %s)\n", ca_file ? ca_file : "cacert.pem");
	return get(u, out);
}
