/* vm_launcher.c - see header. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "vm_launcher.h"

static HANDLE g_proc;
static HANDLE g_job;

bool vm_launch(const char *mod_dir, int vnc_display) {
    if (vm_is_running()) return true;

    char qemu[MAX_PATH], iso[MAX_PATH], cmd[2048];

    /* Allow overriding qemu location via <mod>/vm/qemu_path.txt (one line). */
    _snprintf(qemu, sizeof(qemu), "%s\\vm\\qemu_path.txt", mod_dir);
    FILE *f = fopen(qemu, "r");
    if (f) {
        if (!fgets(qemu, sizeof(qemu), f)) qemu[0] = 0;
        fclose(f);
        char *nl = strpbrk(qemu, "\r\n"); if (nl) *nl = 0;
    } else {
        strcpy(qemu, "qemu-system-x86_64.exe"); /* hope it's on PATH */
    }

    /*
     * -snapshot        : never persist writes; every launch is a clean temple
     * -vnc ...,lossy=off is default; loopback only, no auth needed
     * -rtc base=localtime : TempleOS shows the clock; make it right
     * -audiodev none   : PC-speaker hymns silenced (wire to dsound if brave)
     */
    _snprintf(iso, sizeof(iso), "%s\\vm\\TempleOS.iso", mod_dir);

    _snprintf(cmd, sizeof(cmd),
        "\"%s\" -m 512 -cpu qemu64 -smp 1 "
        "-drive file=\"%s\",media=cdrom "
        "-boot d -snapshot "
        "-rtc base=localtime "
        "-display none -vnc 127.0.0.1:%d "
        "-name TempleOS-HL1",
        qemu, iso, vnc_display);

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi))
        return false;

    /* Job object: VM dies when hl.exe dies, no orphaned gods. */
    if (!g_job) {
        g_job = CreateJobObjectA(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof(li));
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation,
                                &li, sizeof(li));
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    g_proc = pi.hProcess;
    return true;
}

bool vm_is_running(void) {
    if (!g_proc) return false;
    DWORD code = 0;
    return GetExitCodeProcess(g_proc, &code) && code == STILL_ACTIVE;
}

void vm_kill(void) {
    if (g_proc) {
        TerminateProcess(g_proc, 0);
        CloseHandle(g_proc);
        g_proc = NULL;
    }
}
