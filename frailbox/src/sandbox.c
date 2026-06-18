#define _GNU_SOURCE
#include "sandbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

static sandbox_rule_t *sandbox_clone_rules(const sandbox_rule_t *rules) {
    sandbox_rule_t *head = NULL;
    sandbox_rule_t **tail = &head;

    while (rules) {
        sandbox_rule_t *copy = calloc(1, sizeof(sandbox_rule_t));
        if (!copy) {
            while (head) {
                sandbox_rule_t *next = head->next;
                free(head);
                head = next;
            }
            return NULL;
        }

        *copy = *rules;
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
        rules = rules->next;
    }

    return head;
}

sandbox_t *sandbox_create(const sandbox_config_t *config) {
    if (!config) return NULL;

    sandbox_t *sandbox = calloc(1, sizeof(sandbox_t));
    if (!sandbox) return NULL;

    sandbox->config = *config;
    sandbox->config.rules = sandbox_clone_rules(config->rules);
    if (config->rules && !sandbox->config.rules) {
        free(sandbox);
        return NULL;
    }
    sandbox->active = 0;
    sandbox->pid = -1;
    sandbox->start_time = 0;
    sandbox->cpu_time_used = 0;
    sandbox->memory_used = 0;
    sandbox->violations = 0;
    sandbox->seccomp_fd = -1;
    sandbox->event_fd = -1;

    return sandbox;
}

int sandbox_apply(sandbox_t *sandbox) {
    if (!sandbox) return -1;

    if (sandbox->config.type == SANDBOX_NONE) {
        sandbox->active = 1;
        return 0;
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "warning: PR_SET_NO_NEW_PRIVS failed: %s\n",
                strerror(errno));
    }

    if (sandbox->config.memory_limit_bytes > 0) {
        struct rlimit rl = {
            .rlim_cur = sandbox->config.memory_limit_bytes,
            .rlim_max = sandbox->config.memory_limit_bytes,
        };
        if (setrlimit(RLIMIT_AS, &rl) < 0) {
            fprintf(stderr, "warning: setrlimit(RLIMIT_AS) failed: %s\n",
                    strerror(errno));
        }
    }

    if (sandbox->config.max_processes > 0) {
        struct rlimit rl = {
            .rlim_cur = sandbox->config.max_processes,
            .rlim_max = sandbox->config.max_processes,
        };
        if (setrlimit(RLIMIT_NPROC, &rl) < 0) {
            fprintf(stderr, "warning: setrlimit(RLIMIT_NPROC) failed: %s\n",
                    strerror(errno));
        }
    }

    if (sandbox->config.max_open_fds > 0) {
        struct rlimit rl = {
            .rlim_cur = sandbox->config.max_open_fds,
            .rlim_max = sandbox->config.max_open_fds,
        };
        if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
            fprintf(stderr, "warning: setrlimit(RLIMIT_NOFILE) failed: %s\n",
                    strerror(errno));
        }
    }

    sandbox->active = 1;
    sandbox->start_time = (uint64_t)time(NULL);

    return 0;
}

void sandbox_destroy(sandbox_t *sandbox) {
    if (!sandbox) return;

    sandbox->active = 0;
    sandbox->pid = -1;

    if (sandbox->seccomp_fd >= 0) {
        close(sandbox->seccomp_fd);
        sandbox->seccomp_fd = -1;
    }
    if (sandbox->event_fd >= 0) {
        close(sandbox->event_fd);
        sandbox->event_fd = -1;
    }

    sandbox_rule_t *rule = sandbox->config.rules;
    while (rule) {
        sandbox_rule_t *next = rule->next;
        free(rule);
        rule = next;
    }
    sandbox->config.rules = NULL;
    sandbox->config.rule_count = 0;

    free(sandbox);
}

int sandbox_add_rule(sandbox_t *sandbox, sandbox_capability_t cap, sandbox_action_t action) {
    if (!sandbox) return -1;

    sandbox_rule_t *rule = calloc(1, sizeof(sandbox_rule_t));
    if (!rule) return -1;

    rule->capability = cap;
    rule->action = action;
    rule->syscall_number = 0;
    rule->next = sandbox->config.rules;
    sandbox->config.rules = rule;
    sandbox->config.rule_count++;

    return 0;
}

int sandbox_remove_rule(sandbox_t *sandbox, sandbox_capability_t cap) {
    if (!sandbox || !sandbox->config.rules) return -1;

    sandbox_rule_t *prev = NULL;
    sandbox_rule_t *rule = sandbox->config.rules;

    while (rule) {
        if (rule->capability == cap) {
            if (prev) {
                prev->next = rule->next;
            } else {
                sandbox->config.rules = rule->next;
            }
            free(rule);
            sandbox->config.rule_count--;
            return 0;
        }
        prev = rule;
        rule = rule->next;
    }

    return -1;
}

int sandbox_exec(sandbox_t *sandbox, const char *path, char *const argv[], char *const envp[]) {
    (void)sandbox;
    (void)path;
    (void)argv;
    (void)envp;
    return -1;
}

int sandbox_kill(sandbox_t *sandbox, int signal) {
    if (!sandbox || sandbox->pid < 0) return -1;
    return kill(sandbox->pid, signal);
}

uint64_t sandbox_get_usage(sandbox_t *sandbox) {
    if (!sandbox) return 0;
    return sandbox->memory_used;
}

int sandbox_get_violations(sandbox_t *sandbox) {
    if (!sandbox) return -1;
    return (int)sandbox->violations;
}

int sandbox_is_active(const sandbox_t *sandbox) {
    return sandbox ? sandbox->active : 0;
}

int sandbox_set_memory_limit(sandbox_t *sandbox, uint64_t limit_bytes) {
    if (!sandbox) return -1;
    sandbox->config.memory_limit_bytes = limit_bytes;

    struct rlimit rl = {
        .rlim_cur = limit_bytes,
        .rlim_max = limit_bytes,
    };
    return setrlimit(RLIMIT_AS, &rl);
}

int sandbox_set_cpu_limit(sandbox_t *sandbox, uint64_t limit_ns) {
    if (!sandbox) return -1;
    sandbox->config.cpu_limit_ns = limit_ns;
    return 0;
}

int sandbox_set_process_limit(sandbox_t *sandbox, uint32_t max_procs) {
    if (!sandbox) return -1;
    sandbox->config.max_processes = max_procs;

    struct rlimit rl = {
        .rlim_cur = max_procs,
        .rlim_max = max_procs,
    };
    return setrlimit(RLIMIT_NPROC, &rl);
}
