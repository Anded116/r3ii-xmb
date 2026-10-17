#include "spawn.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_OPEN 16

static struct {
	FILE *f;
	pid_t pid;
} open_[MAX_OPEN];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* в потомке: всё, кроме 0–2, закрыть и выполнить команду через sh */
static void child(const char *cmd)
{
	long max = sysconf(_SC_OPEN_MAX);
	for (int fd = 3; fd < (max > 0 && max < 4096 ? max : 1024); fd++)
		close(fd);
	signal(SIGPIPE, SIG_DFL);   /* XMB его игнорирует (Bluetooth), командам — как обычно */
	execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
	_exit(127);
}

FILE *sp_popen(const char *cmd, const char *mode)
{
	int w = mode[0] == 'w', p[2];
	if (pipe(p))
		return NULL;
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return NULL;
	}
	if (!pid) {
		dup2(w ? p[0] : p[1], w ? 0 : 1);
		child(cmd);
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
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (!pid)
		child(cmd);
	int st = -1;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	return st;
}
