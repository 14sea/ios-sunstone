/*
 * sunstone M1: the first program of ours on the iOS side. Started by launchd
 * (sweettest.plist, installed by the kernel's SWEETDRP drop); stdout goes to
 * /dev/console and so into the persistent kernel log.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

/* IOKit has no headers in the iPhoneOS SDK */
typedef mach_port_t io_object_t, io_service_t;
typedef const struct __CFDictionary *CFDictionaryRef;
extern const mach_port_t kIOMainPortDefault;
extern CFDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
extern kern_return_t IOObjectRelease(io_object_t object);

int
main(void)
{
	struct utsname u;
	char model[64] = "";
	size_t len = sizeof(model);

	setvbuf(stdout, NULL, _IOLBF, 0);
	uname(&u);
	sysctlbyname("hw.model", model, &len, NULL, 0);
	printf("SWEETTEST: hello from user space: pid %d uid %d, %s %s %s, hw.model %s\n",
	    getpid(), getuid(), u.sysname, u.release, u.machine, model);
	const char *names[] = { "AppleCLCD", "IOSurfaceRoot", "IOAcceleratorES" };
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(names[i]));
		printf("SWEETTEST: IOKit %s: %s\n", names[i], s ? "found" : "not found");
		if (s)
			IOObjectRelease(s);
	}
	printf("SWEETTEST: done\n");
	return 0;
}
