#include "filesave.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JOBS 8

typedef struct {
	char *path, *data;
	size_t len;
} job_t;

static job_t jobs[JOBS];
static int njobs, busy, want_sync, started;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static void write_file(const job_t *j)
{
	char tmp[1200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", j->path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		perror(tmp);
		return;
	}
	fwrite(j->data, 1, j->len, f);
	fclose(f);
	if (rename(tmp, j->path))
		perror(j->path);
}

static void *worker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&mu);
	for (;;) {
		while (!njobs && !want_sync)
			pthread_cond_wait(&cv, &mu);
		busy = 1;
		if (njobs) {
			job_t j = jobs[0];
			memmove(jobs, jobs + 1, sizeof(job_t) * (size_t)--njobs);
			want_sync = 1;
			pthread_mutex_unlock(&mu);
			write_file(&j);
			free(j.path);
			free(j.data);
		} else {
			want_sync = 0;
			pthread_mutex_unlock(&mu);
			sync();
		}
		pthread_mutex_lock(&mu);
		busy = 0;
		pthread_cond_broadcast(&cv);
	}
	return NULL;
}

void fsave_init(void)
{
	pthread_t t;
	started = pthread_create(&t, NULL, worker, NULL) == 0;
}

void fsave(const char *path, const char *data, size_t len)
{
	if (!started) {   /* потока нет — как раньше, сразу */
		job_t j = { (char *)path, (char *)data, len };
		write_file(&j);
		return;
	}
	pthread_mutex_lock(&mu);
	int i = 0;
	while (i < njobs && strcmp(jobs[i].path, path))
		i++;
	if (i < njobs) {   /* тот же файл ещё не записан — только новое содержимое */
		free(jobs[i].data);
	} else {
		while (njobs == JOBS)
			pthread_cond_wait(&cv, &mu);
		i = njobs++;
		jobs[i].path = strdup(path);
	}
	jobs[i].data = malloc(len ? len : 1);
	memcpy(jobs[i].data, data, len);
	jobs[i].len = len;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

void fsave_sync(void)
{
	if (!started) {
		sync();
		return;
	}
	pthread_mutex_lock(&mu);
	want_sync = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

void fsave_flush(void)
{
	if (!started) {
		sync();
		return;
	}
	pthread_mutex_lock(&mu);
	while (njobs || busy || want_sync)
		pthread_cond_wait(&cv, &mu);
	pthread_mutex_unlock(&mu);
}
