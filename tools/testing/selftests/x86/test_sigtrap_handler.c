// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (C) 2025 Intel Corporation
 */
#define _GNU_SOURCE

#include <err.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>

#ifdef __x86_64__
# define REG_IP REG_RIP
#else
# define REG_IP REG_EIP
#endif

static void sethandler(int sig, void (*handler)(int, siginfo_t *, void *), int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | flags;
	sigemptyset(&sa.sa_mask);

	if (sigaction(sig, &sa, 0))
		err(1, "sigaction");

	return;
}

static unsigned int loop_count_on_same_ip;

static void sigtrap(int sig, siginfo_t *info, void *ctx_void)
{
	ucontext_t *ctx = (ucontext_t *)ctx_void;
	static unsigned long last_trap_ip;

	if (last_trap_ip == ctx->uc_mcontext.gregs[REG_IP]) {
		printf("trapped on %016lx\n", last_trap_ip);

		if (++loop_count_on_same_ip > 10) {
			printf("trap loop detected, test failed\n");
			exit(2);
		}

		return;
	}

	loop_count_on_same_ip = 0;
	last_trap_ip = ctx->uc_mcontext.gregs[REG_IP];
	printf("trapped on %016lx\n", last_trap_ip);
}

int main(int argc, char *argv[])
{
	sethandler(SIGTRAP, sigtrap, 0);

	asm volatile(
#ifdef __x86_64__
		/* Avoid clobbering the redzone */
		"sub $128, %rsp\n\t"
#endif
		"push $0x302\n\t"
		"popf\n\t"
		"nop\n\t"
		"nop\n\t"
		"push $0x202\n\t"
		"popf\n\t"
#ifdef __x86_64__
		"add $128, %rsp\n\t"
#endif
	);

	printf("test passed\n");
	return 0;
}
