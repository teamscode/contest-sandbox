#define _GNU_SOURCE
#define _POSIX_SOURCE


#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <wait.h>
#include <errno.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>

#include "runner.h"
#include "killer.h"
#include "child.h"
#include "logger.h"

#define STACK_SIZE (2 * 1024 * 1024)

void init_result(struct result *_result) {
    _result->error = SUCCESS;
    _result->cpu_time = _result->real_time = _result->signal = _result->exit_code = 0;
    _result->memory = 0;
}


void run(struct config *_config, struct result *_result) {
    // init log fp
    FILE *log_fp = log_open(_config->log_path);

    // init result
    init_result(_result);

    // check whether current user is root
    uid_t uid = getuid();
    if (uid != 0) {
        ERROR_EXIT(ROOT_REQUIRED);
    }

    // check args
    if ((_config->max_cpu_time < 1 && _config->max_cpu_time != UNLIMITED) ||
        (_config->max_real_time < 1 && _config->max_real_time != UNLIMITED) ||
        (_config->max_memory < 1 && _config->max_memory != UNLIMITED) ||
        (_config->max_process_number < 1 && _config->max_process_number != UNLIMITED) ||
        (_config->max_output_size < 1 && _config->max_output_size != UNLIMITED)) {
        ERROR_EXIT(INVALID_CONFIG);
    }

    // malloc stack for child process
    char *stack = NULL;
    stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        ERROR_EXIT(CLONE_FAILED);
    }

    // record current time
    struct timeval start, end;
    gettimeofday(&start, NULL);

    // clone
    child_args args;
    args._config = _config;
    args.log_fp = log_fp;

    pid_t child_pid = clone(child_process, stack + STACK_SIZE, SIGCHLD, (void *) (&args));

    // pid < 0 shows clone failed
    if (child_pid < 0) {
        ERROR_EXIT(CLONE_FAILED);
    }
    else {
        // create new thread to monitor process running time
        pthread_t tid = 0;
        if (_config->max_real_time != UNLIMITED) {
            struct timeout_killer_args killer_args;

            killer_args.timeout = _config->max_real_time;
            killer_args.pid = child_pid;
            if (pthread_create(&tid, NULL, timeout_killer, (void *) (&killer_args)) != 0) {
                kill_pid(child_pid);
                ERROR_EXIT(PTHREAD_FAILED);
            }
        }

        int status;
        struct rusage resource_usage;

        // wait for child process to terminate
        // on success, returns the process ID of the child whose state has changed;
        // On error, -1 is returned.
        if (wait4(child_pid, &status, 0, &resource_usage) == -1) {
            kill_pid(child_pid);
            ERROR_EXIT(WAIT_FAILED);
        }

        // process exited, we may need to cancel timeout killer thread
        if (_config->max_real_time != UNLIMITED) {
            if (pthread_cancel(tid) != 0) {
                // todo logging
            };
        }

        _result->exit_code = WEXITSTATUS(status);
        _result->cpu_time = (int) (resource_usage.ru_utime.tv_sec * 1000 +
                                  resource_usage.ru_utime.tv_usec / 1000 +
                                  resource_usage.ru_stime.tv_sec * 1000 +
                                  resource_usage.ru_stime.tv_usec / 1000);
        _result->memory = resource_usage.ru_maxrss * 1024;

        // if signaled
        if (WIFSIGNALED(status) != 0) {
            LOG_DEBUG(log_fp, "signal: %d", WTERMSIG(status));
            _result->signal = WTERMSIG(status);
        }

        // get end time
        gettimeofday(&end, NULL);
        _result->real_time = (int) (end.tv_sec * 1000 + end.tv_usec / 1000 - start.tv_sec * 1000 - start.tv_usec / 1000);
        log_close(log_fp);
    }
}