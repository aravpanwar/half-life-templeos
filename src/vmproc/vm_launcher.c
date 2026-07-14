/* vm_launcher.c - see header. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "vm_launcher.h"

static HANDLE g_proc;
static HANDLE g_job;
static DWORD  g_launch_tick;      /* GetTickCount at the last CreateProcess */
static int    g_launch_had_audio; /* whether that launch requested audio   */
static int    g_audio_disabled;   /* set once an audio launch dies fast    */

/*
 * Build the QEMU command line.
 *
 * With `audio`, route the emulated PC speaker (which is all TempleOS uses: the
 * beeps and the hymns) to the host so you can hear it. We use the SDL audio
 * backend rather than DirectSound: dsound needs a window handle to initialise,
 * but QEMU is spawned windowless (CREATE_NO_WINDOW), whereas SDL audio needs no
 * window and works headless.
 *
 * Boot the TempleOS live CD (installing to a QEMU disk and booting that back is
 * unreliable: TempleOS's HDD boot loader hangs under SeaBIOS). The live CD
 * reliably reaches the 640x480 desktop; the orchestrator auto-answers 'N' to
 * the one-time install prompt (see terminal_mode).
 *   -snapshot           : ephemeral; a game crash can't corrupt the media
 *   -rtc base=localtime : TempleOS shows the clock; make it right
 */
static void build_cmd(char *cmd, size_t n, const char *qemu, const char *iso,
                      int vnc_display, int audio)
{
    const char *audio_args =
        audio ? "-audiodev sdl,id=snd0 -machine pcspk-audiodev=snd0 " : "";
    _snprintf(cmd, n,
        "\"%s\" -m 512 -cpu qemu64 -smp 1 "
        "%s"
        "-drive file=\"%s\",media=cdrom "
        "-boot d -snapshot "
        "-rtc base=localtime "
        "-display none -vnc 127.0.0.1:%d "
        "-name TempleOS-HL1",
        qemu, audio_args, iso, vnc_display);
}

bool vm_launch(const char *mod_dir, int vnc_display, int audio) {
    if (vm_is_running()) return true;

    /* If our previous attempt asked for audio and the VM died within a few
       seconds, audio is unavailable on this host. Drop it so TempleOS still
       boots (silent) rather than failing to launch at all. */
    if (g_proc && g_launch_had_audio && !g_audio_disabled &&
        (GetTickCount() - g_launch_tick) < 5000) {
        g_audio_disabled = 1;
    }

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

    _snprintf(iso, sizeof(iso), "%s\\vm\\TempleOS.iso", mod_dir);

    int use_audio = audio && !g_audio_disabled;
    build_cmd(cmd, sizeof(cmd), qemu, iso, vnc_display, use_audio);

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

    if (g_proc) CloseHandle(g_proc); /* release the previous (dead) handle */
    g_proc = pi.hProcess;
    g_launch_tick = GetTickCount();
    g_launch_had_audio = use_audio;
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
