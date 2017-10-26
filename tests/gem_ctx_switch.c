/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include "drm.h"

#define LOCAL_I915_EXEC_NO_RELOC (1<<11)
#define LOCAL_I915_EXEC_HANDLE_LUT (1<<12)

#define INTERRUPTIBLE 1

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return ((end->tv_sec - start->tv_sec) +
		(end->tv_nsec - start->tv_nsec)*1e-9);
}

static void single(int fd, uint32_t handle,
		   const struct intel_execution_engine *e,
		   unsigned flags,
		   const int ncpus,
		   int timeout)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	uint32_t contexts[64];
	int n;

	gem_require_ring(fd, e->exec_id | e->flags);

	igt_require(__gem_context_create(fd, &contexts[0]) == 0);
	for (n = 1; n < 64; n++)
		contexts[n] = gem_context_create(fd);

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	if (flags & INTERRUPTIBLE) {
		/* Be tricksy and force a relocation every batch so that
		 * we don't emit the batch but just do MI_SET_CONTEXT
		 */
		memset(&reloc, 0, sizeof(reloc));
		reloc.offset = 1024;
		reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		obj.relocs_ptr = to_user_pointer(&reloc);
		obj.relocation_count = 1;
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = contexts[0];
	execbuf.flags = e->exec_id | e->flags;
	execbuf.flags |= LOCAL_I915_EXEC_HANDLE_LUT;
	execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;
	igt_require(__gem_execbuf(fd, &execbuf) == 0);
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = e->exec_id | e->flags;
		reloc.target_handle = obj.handle;
		gem_execbuf(fd, &execbuf);
	}
	gem_sync(fd, handle);

	igt_fork(child, ncpus) {
		struct timespec start, now;
		unsigned int count = 0;

		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			igt_while_interruptible(flags & INTERRUPTIBLE) {
				for (int loop = 0; loop < 64; loop++) {
					execbuf.rsvd1 = contexts[loop % 64];
					reloc.presumed_offset = -1;
					gem_execbuf(fd, &execbuf);
				}
				count += 64;
			}
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (elapsed(&start, &now) < timeout);
		gem_sync(fd, handle);
		clock_gettime(CLOCK_MONOTONIC, &now);

		igt_info("[%d] %s: %'u cycles: %.3fus%s\n",
			 child, e->name, count, elapsed(&start, &now)*1e6 / count,
			 flags & INTERRUPTIBLE ? " (interruptible)" : "");
	}
	igt_waitchildren();

	for (n = 0; n < 64; n++)
		gem_context_destroy(fd, contexts[n]);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const struct intel_execution_engine *e;
	uint32_t light = 0, heavy;
	int fd = -1;

	igt_fixture {
		const uint32_t bbe = MI_BATCH_BUFFER_END;

		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		light = gem_create(fd, 4096);
		gem_write(fd, light, 0, &bbe, sizeof(bbe));

		heavy = gem_create(fd, 4096*1024);
		gem_write(fd, heavy, 4096*1024-sizeof(bbe), &bbe, sizeof(bbe));

		igt_fork_hang_detector(fd);
	}

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("%s%s", e->exec_id == 0 ? "basic-" : "", e->name)
			single(fd, light, e, 0, 1, 5);

		igt_skip_on_simulation();

		igt_subtest_f("%s%s-heavy", e->exec_id == 0 ? "basic-" : "", e->name)
			single(fd, heavy, e, 0, 1, 5);
		igt_subtest_f("%s-interruptible", e->name)
			single(fd, light, e, INTERRUPTIBLE, 1, 150);
		igt_subtest_f("forked-%s", e->name)
			single(fd, light, e, 0, ncpus, 150);
		igt_subtest_f("forked-%s-heavy", e->name)
			single(fd, heavy, e, 0, ncpus, 150);
		igt_subtest_f("forked-%s-interruptible", e->name)
			single(fd, light, e, INTERRUPTIBLE, ncpus, 150);
	}

	igt_fixture {
		igt_stop_hang_detector();
		gem_close(fd, heavy);
		gem_close(fd, light);
		close(fd);
	}
}
