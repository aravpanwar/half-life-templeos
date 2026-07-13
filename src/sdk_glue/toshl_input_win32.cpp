/*
 * toshl_input_win32.cpp - Win32 keyboard glue for TempleOS-HL1.
 *
 * Subclasses the GoldSrc engine window so that, while the player is driving a
 * terminal, keystrokes route to TempleOS (over RFB) instead of the game. When
 * not in terminal mode, TOSHL_HandleKey returns false and every message is
 * passed straight through to the engine's original window proc.
 *
 * This TU includes the full <windows.h> (VK_/WM_ codes) and NO SDK headers,
 * because the SDK's PlatformHeaders.h disables those definitions. Keeping it
 * separate from toshl_glue.cpp is deliberate.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
// Defined in terminal_mode.cpp. Returns true if the event was consumed.
bool TOSHL_HandleKey(UINT msg, WPARAM wp, LPARAM lp);

void TOSHL_InstallInput(void);
void TOSHL_RemoveInput(void);
}

static WNDPROC g_origProc = nullptr;
static HWND g_wnd = nullptr;

struct find_ctx
{
	DWORD pid;
	HWND hwnd;
};

static BOOL CALLBACK enum_proc(HWND h, LPARAM lp)
{
	find_ctx* c = (find_ctx*)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr)
	{
		c->hwnd = h;
		return FALSE; // stop
	}
	return TRUE;
}

static HWND find_game_window(void)
{
	// Classic GoldSrc registers its main window as class "Valve001".
	HWND h = FindWindowA("Valve001", nullptr);
	if (h)
		return h;
	// Fallback: first visible top-level window owned by this process.
	find_ctx c;
	c.pid = GetCurrentProcessId();
	c.hwnd = nullptr;
	EnumWindows(enum_proc, (LPARAM)&c);
	return c.hwnd;
}

static LRESULT CALLBACK toshl_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (TOSHL_HandleKey(msg, wp, lp))
		return 0; // swallowed: do not let the engine see it
	return CallWindowProc(g_origProc, h, msg, wp, lp);
}

extern "C" void TOSHL_InstallInput(void)
{
	if (g_wnd)
		return; // already installed
	HWND h = find_game_window();
	if (!h)
		return;
	g_wnd = h;
	g_origProc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)toshl_wndproc);
}

extern "C" void TOSHL_RemoveInput(void)
{
	if (g_wnd && g_origProc)
		SetWindowLongPtrA(g_wnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
	g_wnd = nullptr;
	g_origProc = nullptr;
}
