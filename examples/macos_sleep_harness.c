// macos_sleep_harness calls __semwait_signal (XNU syscall 334) the way the macOS
// bare runtime's bf_nanosleep does: no semaphores, a relative timeout of 300 ms.
// Build and run on macOS:
//   clang -w -Wno-deprecated-declarations examples/macos_sleep_harness.c -o sleep_harness && ./sleep_harness
// Expected: rc=-1 errno=60 (ETIMEDOUT) and elapsed_ms close to 300.
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	long rc = syscall(334, 0, 0, 1, 1, (long long)0, (int)300000000);
	int err = errno;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	printf("rc=%ld errno=%d elapsed_ms=%ld\n", rc, err, ms);
	return 0;
}
