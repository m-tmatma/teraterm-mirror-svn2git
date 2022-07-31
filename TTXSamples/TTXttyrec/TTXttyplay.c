#include "teraterm.h"
#include "tttypes.h"
#include "ttplugin.h"
#include "tt_res.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "inifile_com.h"

#include "gettimeofday.h"

#define ORDER 6001
#define ID_MENU_REPLAY 55302
#define ID_MENU_AGAIN  55303

#define BUFFSIZE 2000

#define INISECTION "ttyplay"

static HANDLE hInst; /* Instance handle of TTX*.DLL */

enum ParseMode {
  MODE_FIRST,
  MODE_ESC,
  MODE_CSI,
  MODE_STRING,
  MODE_STR_ESC
};

struct recheader {
	struct timeval tv;
	int len;
};

typedef struct {
	PTTSet ts;
	PComVar cv;
	TCreateFile origPCreateFile;
	TReadFile origPReadFile;
	TWriteFile origPWriteFile;
	PParseParam origParseParam;
	PReadIniFile origReadIniFile;
	BOOL enable;
	BOOL ChangeTitle;
	BOOL ReplaceHostDlg;
	BOOL played;
	HMENU FileMenu;
	HMENU ControlMenu;
	int maxwait;
	int speed;
	BOOL pause;
	BOOL nowait;
	BOOL open_error;
	struct timeval last;
	struct timeval wait;
	char openfn[MAX_PATH];
	char origTitle[TitleBuffSize];
	char origOLDTitle[TitleBuffSize];
} TInstVar;

static TInstVar *pvar;
static TInstVar InstVar;

#define GetFileMenu(menu)	GetSubMenuByChildID(menu, ID_FILE_NEWCONNECTION)
#define GetEditMenu(menu)	GetSubMenuByChildID(menu, ID_EDIT_COPY2)
#define GetSetupMenu(menu)	GetSubMenuByChildID(menu, ID_SETUP_TERMINAL)
#define GetControlMenu(menu)	GetSubMenuByChildID(menu, ID_CONTROL_RESETTERMINAL)
#define GetHelpMenu(menu)	GetSubMenuByChildID(menu, ID_HELP_ABOUT)

void RestoreOLDTitle() {
	strncpy_s(pvar->ts->Title, sizeof(pvar->ts->Title), pvar->origOLDTitle, _TRUNCATE);
	pvar->ChangeTitle = TRUE;
	SendMessage(pvar->cv->HWin, WM_COMMAND, MAKELONG(ID_SETUP_WINDOW, 0), 0);
}

void ChangeTitleStatus() {
  char tbuff[TitleBuffSize];

  _snprintf_s(tbuff, sizeof(tbuff), _TRUNCATE, "Speed: %d, Pause: %s", pvar->speed, pvar->pause ? "ON": "OFF");
  strncpy_s(pvar->ts->Title, sizeof(pvar->ts->Title), tbuff, _TRUNCATE);
  pvar->ChangeTitle = TRUE;
  SendMessage(pvar->cv->HWin, WM_COMMAND, MAKELONG(ID_SETUP_WINDOW, 0), 0);
}

HMENU GetSubMenuByChildID(HMENU menu, UINT id) {
  int i, j, items, subitems, cur_id;
  HMENU m;

  items = GetMenuItemCount(menu);

  for (i=0; i<items; i++) {
    if (m = GetSubMenu(menu, i)) {
      subitems = GetMenuItemCount(m);
      for (j=0; j<subitems; j++) {
        cur_id = GetMenuItemID(m, j);
	if (cur_id == id) {
	  return m;
	}
      }
    }
  }
  return NULL;
}

static void PASCAL TTXInit(PTTSet ts, PComVar cv) {
	pvar->ts = ts;
	pvar->cv = cv;
	pvar->origPCreateFile = NULL;
	pvar->origPReadFile = NULL;
	pvar->origPWriteFile = NULL;
	pvar->enable = FALSE;
	pvar->ChangeTitle = FALSE;
	pvar->ReplaceHostDlg = FALSE;
	pvar->played = FALSE;
	gettimeofday(&(pvar->last) /*, NULL*/ );
	pvar->wait.tv_sec = 0;
	pvar->wait.tv_usec = 1;
	pvar->pause = FALSE;
	pvar->nowait = FALSE;
	pvar->open_error = FALSE;
}

void RestoreTitle() {
	ChangeTitleStatus ();
/*
	strncpy_s(pvar->ts->Title, sizeof(pvar->ts->Title), pvar->origTitle, _TRUNCATE);
	pvar->ChangeTitle = TRUE;
	SendMessage(pvar->cv->HWin, WM_COMMAND, MAKELONG(ID_SETUP_WINDOW, 0), 0);
*/
}

void ChangeTitle(char *title) {
	strncpy_s(pvar->origTitle, sizeof(pvar->origTitle), pvar->ts->Title, _TRUNCATE);
	strncpy_s(pvar->ts->Title, sizeof(pvar->ts->Title), title, _TRUNCATE);
	pvar->ChangeTitle = TRUE;
	SendMessage(pvar->cv->HWin, WM_COMMAND, MAKELONG(ID_SETUP_WINDOW, 0), 0);
}

static HANDLE PASCAL TTXCreateFile(LPCTSTR FName, DWORD AcMode, DWORD ShMode,
    LPSECURITY_ATTRIBUTES SecAttr, DWORD CreateDisposition, DWORD FileAttr, HANDLE Template) {

	HANDLE ret;

	ret = pvar->origPCreateFile(FName, AcMode, ShMode, SecAttr, CreateDisposition, FileAttr, Template);

	if (pvar->enable) {
		if (ret == INVALID_HANDLE_VALUE) {
			pvar->enable = FALSE;
			pvar->open_error = TRUE;
			pvar->played = FALSE;
		}
		else {
			pvar->open_error = FALSE;
		}
	}

	return ret;
}

static BOOL PASCAL TTXReadFile(HANDLE fh, LPVOID obuff, DWORD oblen, LPDWORD rbytes, LPOVERLAPPED rol) {
	static struct recheader prh = { 0, 0, 0 };
	static DWORD lbytes;
	static char ibuff[BUFFSIZE];
	static BOOL title_changed = FALSE, first_title_changed = FALSE;

	int b[3];
	DWORD rsize;
	struct recheader h;
	struct timeval curtime;
	struct timeval tdiff;

	*rbytes = 0;

	if (pvar->pause) {
		SetLastError(ERROR_IO_PENDING);
		return FALSE;
	}

	if (!pvar->enable) {
		return pvar->origPReadFile(fh, obuff, oblen, rbytes, rol);
	}

	if (!first_title_changed) {
		ChangeTitleStatus ();
		first_title_changed = TRUE;
	}

	if (prh.len == 0 && lbytes == 0) {
		if (!pvar->origPReadFile(fh, b, sizeof(b), &rsize, rol)) {
			return FALSE;
		}
		if (rsize == 0) {
			// EOF reached
			return TRUE;
		}
		else if (rsize != sizeof(b)) {
			MessageBox(pvar->cv->HWin, "rsize != sizeof(b), Disabled.", "TTXttyplay", MB_ICONEXCLAMATION);
			pvar->enable = FALSE;
			RestoreOLDTitle();
			return FALSE;
		}
		h.tv.tv_sec = b[0];
		h.tv.tv_usec = b[1];
		h.len = b[2];
		if (prh.tv.tv_sec != 0) {
			pvar->wait = tvshift(tvdiff(prh.tv, h.tv), pvar->speed);
			if (pvar->maxwait != 0 && pvar->wait.tv_sec >= pvar->maxwait) {
				char tbuff[TitleBuffSize];
				_snprintf_s(tbuff, sizeof(tbuff), _TRUNCATE, "%d.%06d secs idle. trim to %d secs.",
				pvar->wait.tv_sec, pvar->wait.tv_usec, pvar->maxwait);
				pvar->wait.tv_sec = pvar->maxwait;
				pvar->wait.tv_usec = 0;
				title_changed = TRUE;
				ChangeTitle(tbuff);
			}
		}
		prh = h;
	}

	if (!pvar->nowait) {
		gettimeofday(&curtime /*, NULL*/ );
		tdiff = tvdiff(pvar->last, curtime);
	}

	if (pvar->nowait || tdiff.tv_sec > pvar->wait.tv_sec ||
	  (tdiff.tv_sec == pvar->wait.tv_sec && tdiff.tv_usec >= pvar->wait.tv_usec)) {
		if (title_changed) {
			RestoreTitle();
			title_changed = FALSE;
		}
		if (pvar->wait.tv_sec != 0 || pvar->wait.tv_usec != 0) {
			pvar->wait.tv_sec = 0;
			pvar->wait.tv_usec = 0;
			pvar->last = curtime;
		}

		if (prh.len != 0 && lbytes == 0) {
			if (!pvar->origPReadFile(fh, ibuff, (prh.len<BUFFSIZE) ? prh.len : BUFFSIZE, &lbytes, rol)) {
				return FALSE;
			}
			else if (lbytes == 0) {
				// EOF reached
				return TRUE;
			}
			prh.len -= lbytes;
		}

		if (lbytes > 0) {
			if (lbytes < oblen) {
				memcpy(obuff, ibuff, lbytes);
				*rbytes = lbytes;
				lbytes = 0;
			}
			else {
				memcpy(obuff, ibuff, oblen);
				lbytes -= oblen;
				memcpy(ibuff, ibuff+oblen, lbytes);
				*rbytes = oblen;
			}
			return TRUE;
		}
	}

	SetLastError(ERROR_IO_PENDING);
	return FALSE;
}

static BOOL PASCAL TTXWriteFile(HANDLE fh, LPCVOID buff, DWORD len, LPDWORD wbytes, LPOVERLAPPED wol) {
	char tmpbuff[2048];
	unsigned int spos, dpos;
	char *ptr;
	enum ParseMode mode = MODE_FIRST;
	BOOL speed_changed = FALSE;

	ptr = (char *)buff;
	*wbytes = 0;

	for (spos = dpos = 0; spos < len; spos++, ptr++) {
		switch (mode) {
		case MODE_FIRST:
			switch (*ptr) {
			  case '1':
				pvar->speed = 0;
				speed_changed = TRUE;
				break;
			  case 'f':
			  case 'F':
			  case '+':
				if (pvar->speed < 8) {
					pvar->speed++;
					speed_changed = TRUE;
				}
				break;
			  case 's':
			  case 'S':
			  case '-':
				if (pvar->speed > -8) {
					pvar->speed--;
					speed_changed = TRUE;
				}
				break;
			  case 'p':
			  case 'P':
				pvar->pause = !(pvar->pause);
				speed_changed = TRUE;
				break;
			  case ' ':
			  case '.':
				pvar->wait.tv_sec = 0;
				break;
			  case ESC:
				mode = MODE_ESC;
				break;
			  default:
				if (dpos < sizeof(tmpbuff)) {
					tmpbuff[dpos++] = *ptr;
				}
			}
			break;
		case MODE_ESC:
			switch (*ptr) {
			case '[':
				mode = MODE_CSI;
				break;
			case 'P': // DCS
			case ']': // OSC
			case 'X': // SOS
			case '^': // PM
			case '_': // APC
				mode = MODE_STRING;
				break;
			default:
				mode = MODE_FIRST;
				break;
			}
			break;
		case MODE_CSI:
			if (*ptr < ' ' || *ptr > '?') {
				mode = MODE_FIRST;
			}
			break;
		case MODE_STRING:
			if (*ptr == ESC) {
				mode = MODE_STR_ESC;
			}
			else if (*ptr == BEL) {
				mode = MODE_FIRST;
			}
			break;
		case MODE_STR_ESC:
			if (*ptr == '\\') {
				mode = MODE_FIRST;
			}
			else if (*ptr != ESC) {
				mode = MODE_STRING;
			}
			break;
		}
	}

	if (speed_changed) {
		ChangeTitleStatus ();
	}
	if (dpos > 0) {
		pvar->origPWriteFile(fh, tmpbuff, dpos, wbytes, wol);
	}
	*wbytes = len;
	return TRUE;
}

static void PASCAL TTXOpenFile(TTXFileHooks *hooks) {
	if (pvar->cv->PortType == IdFile && pvar->enable) {
		pvar->origPCreateFile = *hooks->PCreateFile;
		pvar->origPReadFile = *hooks->PReadFile;
		pvar->origPWriteFile = *hooks->PWriteFile;
		*hooks->PCreateFile = TTXCreateFile;
		*hooks->PReadFile = TTXReadFile;
		*hooks->PWriteFile = TTXWriteFile;

		strncpy_s(pvar->origOLDTitle, sizeof(pvar->origOLDTitle), pvar->ts->Title, _TRUNCATE);
	}
}

static void PASCAL TTXCloseFile(TTXFileHooks *hooks) {
	if (pvar->origPCreateFile) {
		*hooks->PCreateFile = pvar->origPCreateFile;
	}
	if (pvar->origPReadFile) {
		*hooks->PReadFile = pvar->origPReadFile;
	}
	if (pvar->origPWriteFile) {
		*hooks->PWriteFile = pvar->origPWriteFile;
	}
	if (pvar->enable) {
		RestoreOLDTitle();
		pvar->enable = FALSE;
		pvar->played = TRUE;
	}
}

static void PASCAL TTXModifyMenu(HMENU menu) {
	UINT flag = MF_BYCOMMAND | MF_STRING | MF_ENABLED;

	pvar->FileMenu = GetFileMenu(menu);
	pvar->ControlMenu = GetControlMenu(menu);

	if (pvar->enable) {
		flag |= MF_GRAYED;
	}
	InsertMenu(pvar->FileMenu, ID_FILE_PRINT2, flag, ID_MENU_REPLAY, "TTY R&eplay");
	if (pvar->played) {
		InsertMenu(pvar->FileMenu, ID_FILE_PRINT2, flag, ID_MENU_AGAIN, "Replay again");
	}
	InsertMenu(pvar->FileMenu, ID_FILE_PRINT2, MF_BYCOMMAND | MF_SEPARATOR, 0, NULL);

//	InsertMenu(menu, ID_HELPMENU, MF_ENABLED, ID_MENU_REPLAY, "&t");
}

static void PASCAL TTXModifyPopupMenu(HMENU menu) {
	if (menu==pvar->FileMenu) {
		if (pvar->enable) {
			EnableMenuItem(pvar->FileMenu, ID_MENU_REPLAY, MF_BYCOMMAND | MF_GRAYED);
		}
		else {
			EnableMenuItem(pvar->FileMenu, ID_MENU_REPLAY, MF_BYCOMMAND | MF_ENABLED);
		}
	}
}

static void PASCAL TTXParseParam(wchar_t *Param, PTTSet ts, PCHAR DDETopic) {
	wchar_t buff[1024];
	wchar_t *next;
	pvar->origParseParam(Param, ts, DDETopic);

	next = Param;
	while (next = GetParam(buff, sizeof(buff), next)) {
		if (_wcsnicmp(buff, L"/ttyplay-nowait", 16) == 0 || _wcsnicmp(buff, L"/tpnw", 6) == 0) {
			pvar->nowait = TRUE;
		}
		else if (_wcsnicmp(buff, L"/TTYPLAY", 9) == 0 || _wcsnicmp(buff, L"/TP", 4) == 0) {
			pvar->enable = TRUE;
			if (ts->PortType == IdFile && strlen(ts->HostName) > 0) {
				strncpy_s(pvar->openfn, sizeof(pvar->openfn), ts->HostName, _TRUNCATE);
			}
		}
	}
}

static void PASCAL TTXReadIniFile(const wchar_t *fn, PTTSet ts) {
	(pvar->origReadIniFile)(fn, ts);
//	ts->TitleFormat = 0;
	pvar->maxwait = GetPrivateProfileIntAFileW(INISECTION, "MaxWait", 0, fn);
	pvar->speed = GetPrivateProfileIntAFileW(INISECTION, "Speed", 0, fn);
}

static void PASCAL TTXGetSetupHooks(TTXSetupHooks *hooks) {
	pvar->origParseParam = *hooks->ParseParam;
	*hooks->ParseParam = TTXParseParam;
	pvar->origReadIniFile = *hooks->ReadIniFile;
	*hooks->ReadIniFile = TTXReadIniFile;
}

static int PASCAL TTXProcessCommand(HWND hWin, WORD cmd) {
	OPENFILENAME ofn;

	switch (cmd) {
	case ID_MENU_REPLAY:
		if (!pvar->enable) {
			memset(&ofn, 0, sizeof(ofn));
			ofn.lStructSize = get_OPENFILENAME_SIZE();
			ofn.hwndOwner = hWin;
			ofn.lpstrFilter = "ttyrec(*.tty)\0*.tty\0All files(*.*)\0*.*\0\0";
			ofn.lpstrFile = pvar->openfn;
			ofn.nMaxFile = sizeof(pvar->openfn);
			ofn.lpstrDefExt = "tty";
			// ofn.lpstrTitle = "";
			ofn.Flags = OFN_FILEMUSTEXIST;

			if (GetOpenFileName(&ofn)) {
				pvar->ReplaceHostDlg = TRUE;
				// Call New-Connection dialog
				SendMessage(hWin, WM_COMMAND, MAKELONG(ID_FILE_NEWCONNECTION, 0), 0);
			}
		}
		return 1;
	case ID_MENU_AGAIN:
		if (pvar->played) {
			// Clear Buffer
			SendMessage(hWin, WM_COMMAND, MAKELONG(ID_EDIT_CLEARBUFFER, 0), 0);
			pvar->played = FALSE;
			pvar->ReplaceHostDlg = TRUE;
			// Call New-Connection dialog
			SendMessage(hWin, WM_COMMAND, MAKELONG(ID_FILE_NEWCONNECTION, 0), 0);
		}
		return 1;
	}
	return 0;
}

static BOOL PASCAL TTXSetupWin(HWND parent, PTTSet ts) {
	return (TRUE);
}

static BOOL PASCAL TTXGetHostName(HWND parent, PGetHNRec GetHNRec) {
	GetHNRec->PortType = IdTCPIP;
	_snwprintf_s(GetHNRec->HostName, MAXPATHLEN, _TRUNCATE, L"/R=\"%hs\" /TP", pvar->openfn);
	return (TRUE);
}

static void PASCAL TTXGetUIHooks(TTXUIHooks *hooks) {
	if (pvar->ChangeTitle) {
		pvar->ChangeTitle = FALSE;
		*hooks->SetupWin = TTXSetupWin;
	}
	if (pvar->ReplaceHostDlg) {
		pvar->ReplaceHostDlg = FALSE;
		*hooks->GetHostName = TTXGetHostName;
	}
	return;
}

static TTXExports Exports = {
	sizeof(TTXExports),
	ORDER,

	TTXInit,
	TTXGetUIHooks,
	TTXGetSetupHooks,
	NULL, // TTXOpenTCP,
	NULL, // TTXCloseTCP,
	NULL, // TTXSetWinSize,
	TTXModifyMenu,
	TTXModifyPopupMenu,
	TTXProcessCommand,
	NULL, // TTXEnd,
	NULL, // TTXSetCommandLine,
	TTXOpenFile,
	TTXCloseFile
};

BOOL __declspec(dllexport) PASCAL TTXBind(WORD Version, TTXExports *exports) {
	int size = sizeof(Exports) - sizeof(exports->size);

	if (size > exports->size) {
		size = exports->size;
	}
	memcpy((char *)exports + sizeof(exports->size),
		(char *)&Exports + sizeof(exports->size),
		size);
	return TRUE;
}

BOOL WINAPI DllMain(HANDLE hInstance,
		    ULONG ul_reason_for_call,
		    LPVOID lpReserved)
{
	switch( ul_reason_for_call ) {
		case DLL_THREAD_ATTACH:
			/* do thread initialization */
			break;
		case DLL_THREAD_DETACH:
			/* do thread cleanup */
			break;
		case DLL_PROCESS_ATTACH:
			/* do process initialization */
			hInst = hInstance;
			pvar = &InstVar;
			break;
		case DLL_PROCESS_DETACH:
			/* do process cleanup */
			break;
	}
	return TRUE;
}
