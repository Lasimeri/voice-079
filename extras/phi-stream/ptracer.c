/* ptracer: LD_PRELOAD shim that lets a debugger attach to this process under
 * Yama ptrace_scope=1 (where only an ancestor may), with no change to the
 * program: prctl(PR_SET_PTRACER, ANY) at load. The Phi Stream watchdog uses
 * it to take every thread's stack (eu-stack) the moment the service wedges. */
#include <sys/prctl.h>
__attribute__((constructor)) static void allow_ptrace(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
}
