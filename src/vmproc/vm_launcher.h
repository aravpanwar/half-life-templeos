/* vm_launcher.h - spawn/kill the QEMU child process hosting TempleOS. */
#ifndef TOSHL_VM_LAUNCHER_H
#define TOSHL_VM_LAUNCHER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Launch QEMU headless with a VNC server on 127.0.0.1:<5900+display>.
 * `mod_dir` = absolute path to the mod folder (contains vm/TempleOS.iso and
 * vm/qemu path config). Uses a Win32 Job Object with KILL_ON_JOB_CLOSE so
 * the VM can never outlive hl.exe, even on a crash.
 */
bool vm_launch(const char *mod_dir, int vnc_display);
bool vm_is_running(void);
void vm_kill(void);

#ifdef __cplusplus
}
#endif
#endif
