#include "subproc.h"
#include "blackbox.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_OPEN 16

static struct {
	FILE *f;
	pid_t pid;
} open_[MAX_OPEN];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

extern char **environ;

/* sh -c cmd без копии XMB: posix_spawn (в musl — clone с общей памятью, как vfork) вместо fork. fork копировал
 * адресное пространство вместе с отображёнными страницами PCM (tinyalsa держит их mmap) — в момент старта звука это
 * подозревается в падениях ядра (XMB-0024), и на 16 МБ XMB он заметно дорог. В потомке закрыто всё, кроме 0–2 (и
 * pipe для popen: io — какой конец стать stdin/stdout), SIGPIPE — по умолчанию (XMB его игнорирует ради Bluetooth) */
static pid_t spawn(const char *cmd, int io, int io_to)
{
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	posix_spawn_file_actions_init(&fa);
	posix_spawnattr_init(&at);
	if (io >= 0)
		posix_spawn_file_actions_adddup2(&fa, io, io_to);
	long max = sysconf(_SC_OPEN_MAX);
	for (int fd = 3; fd < (max > 0 && max < 4096 ? max : 1024); fd++)
		if (fcntl(fd, F_GETFD) != -1)
			posix_spawn_file_actions_addclose(&fa, fd);
	sigset_t def;
	sigemptyset(&def);
	sigaddset(&def, SIGPIPE);
	posix_spawnattr_setsigdefault(&at, &def);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF);
	bb("команда: %.80s", cmd);
	char *argv[] = { "sh", "-c", (char *)cmd, NULL };
	/* без служебных XMB_*: иначе их унаследует, например, перезапущенный adbd — и сценарий проверки (XMB_SCRIPT,
	 * XMB_FORCE_MEM) достанется каждому следующему запуску XMB из adb */
	char *env[256];
	int n = 0;
	for (char **e = environ; *e && n < 255; e++)
		if (strncmp(*e, "XMB_", 4))
			env[n++] = *e;
	env[n] = NULL;
	pid_t pid;
	if (posix_spawn(&pid, "/bin/sh", &fa, &at, argv, env))
		pid = -1;
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	return pid;
}

FILE *sp_popen(const char *cmd, const char *mode)
{
	int w = mode[0] == 'w', p[2];
	if (pipe(p))
		return NULL;
	pid_t pid = spawn(cmd, w ? p[0] : p[1], w ? 0 : 1);
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return NULL;
	}
	close(w ? p[0] : p[1]);
	FILE *f = fdopen(w ? p[1] : p[0], w ? "w" : "r");
	if (!f) {
		close(w ? p[1] : p[0]);
		waitpid(pid, NULL, 0);
		return NULL;
	}
	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_OPEN; i++)
		if (!open_[i].f) {
			open_[i].f = f;
			open_[i].pid = pid;
			break;
		}
	pthread_mutex_unlock(&mu);
	return f;
}

int sp_pclose(FILE *f)
{
	pid_t pid = -1;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_OPEN; i++)
		if (open_[i].f == f) {
			pid = open_[i].pid;
			open_[i].f = NULL;
			break;
		}
	pthread_mutex_unlock(&mu);
	fclose(f);
	int st = -1;
	while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	return st;
}

const char *sp_xmbget(void)
{
	static const char *const where[] = { "/tmp/xmb/xmbget", "/usr/data/xmb/xmbget", "/data/mnt/sd_0/hiby_dev/xmb/xmbget" };
	for (unsigned i = 0; i < sizeof(where) / sizeof(where[0]); i++)
		if (access(where[i], X_OK) == 0)
			return where[i];
	return NULL;
}

int sp_system(const char *cmd)
{
	pid_t pid = spawn(cmd, -1, 0);
	if (pid < 0)
		return -1;
	int st = -1;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	return st;
}
