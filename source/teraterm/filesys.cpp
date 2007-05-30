/* Tera Term
 Copyright(C) 1994-1998 T. Teranishi
 All rights reserved. */

/* TERATERM.EXE, file transfer routines */
#include "stdafx.h"
#include "teraterm.h"
#include "tttypes.h"
#include "ttftypes.h"
#ifdef TERATERM32
#include "tt_res.h"
#else
#include "tt_res16.h"
#endif
#include "ftdlg.h"
#include "protodlg.h"
#include "ttwinman.h"
#include "commlib.h"
#include "ttcommon.h"
#include "ttdde.h"
#include "ttlib.h"
#include "helpid.h"

#include "filesys.h"

PFileVar LogVar = NULL;
PFileVar SendVar = NULL;
PFileVar FileVar = NULL;
static PCHAR ProtoVar = NULL;
static int ProtoId;

static BYTE LogLast = 0;
BOOL FileLog = FALSE;
BOOL BinLog = FALSE;
BOOL DDELog = FALSE;
static BOOL FileRetrySend, FileRetryEcho, FileCRSend;
static BYTE FileByte;

static BOOL FSend = FALSE;

static HMODULE HTTFILE = NULL;
static int TTFILECount = 0;

PGetSetupFname GetSetupFname;
PGetTransFname GetTransFname;
PGetMultiFname GetMultiFname;
PGetGetFname GetGetFname;
PSetFileVar SetFileVar;
PGetXFname GetXFname;
PProtoInit ProtoInit;
PProtoParse ProtoParse;
PProtoTimeOutProc ProtoTimeOutProc;
PProtoCancel ProtoCancel;

#define IdGetSetupFname  1
#define IdGetTransFname  2
#define IdGetMultiFname  3
#define IdGetGetFname	 4
#define IdSetFileVar	 5
#define IdGetXFname	 6

#define IdProtoInit	 7
#define IdProtoParse	 8
#define IdProtoTimeOutProc 9
#define IdProtoCancel	 10

/*
   Line Head flag for timestamping
   2007.05.24 Gentaro
*/
enum enumLineEnd {
	Line_Other = 0,
	Line_LineHead = 1,
	Line_FileHead = 2,
};

enum enumLineEnd eLineEnd = Line_LineHead;

BOOL LoadTTFILE()
{
  BOOL Err;

#ifdef TERATERM32
  if (HTTFILE != NULL)
#else
  if (HTTFILE >= HINSTANCE_ERROR)
#endif
  {
    TTFILECount++;
    return TRUE;
  }
  else
    TTFILECount = 0;

#ifdef TERATERM32
  HTTFILE = LoadLibrary("TTPFILE.DLL");
  if (HTTFILE == NULL) return FALSE;
#else
  HTTFILE = LoadLibrary("TTFILE.DLL");
  if (HTTFILE < HINSTANCE_ERROR) return FALSE;
#endif

  Err = FALSE;
  GetSetupFname = (PGetSetupFname)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdGetSetupFname));
  if (GetSetupFname==NULL) Err = TRUE;

  GetTransFname = (PGetTransFname)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdGetTransFname));
  if (GetTransFname==NULL) Err = TRUE;

  GetMultiFname = (PGetMultiFname)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdGetMultiFname));
  if (GetMultiFname==NULL) Err = TRUE;

  GetGetFname = (PGetGetFname)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdGetGetFname));
  if (GetGetFname==NULL) Err = TRUE;

  SetFileVar = (PSetFileVar)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdSetFileVar));
  if (SetFileVar==NULL) Err = TRUE;

  GetXFname = (PGetXFname)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdGetXFname));
  if (GetXFname==NULL) Err = TRUE;

  ProtoInit = (PProtoInit)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdProtoInit));
  if (ProtoInit==NULL) Err = TRUE;

  ProtoParse = (PProtoParse)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdProtoParse));
  if (ProtoParse==NULL) Err = TRUE;

  ProtoTimeOutProc = (PProtoTimeOutProc)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdProtoTimeOutProc));
  if (ProtoTimeOutProc==NULL) Err = TRUE;

  ProtoCancel = (PProtoCancel)GetProcAddress(HTTFILE,
    MAKEINTRESOURCE(IdProtoCancel));
  if (ProtoCancel==NULL) Err = TRUE;

  if (Err)
  {
    FreeLibrary(HTTFILE);
    HTTFILE = NULL;
    return FALSE; 
  }
  else {
    TTFILECount = 1;
    return TRUE;
  }
}

BOOL FreeTTFILE()
{
  if (TTFILECount==0) return FALSE;
  TTFILECount--;
  if (TTFILECount>0) return TRUE;
#ifdef TERATERM32
  if (HTTFILE!=NULL)
#else
  if (HTTFILE>=HINSTANCE_ERROR)
#endif
  {
    FreeLibrary(HTTFILE);
    HTTFILE = NULL;
  }
  return TRUE;
}

static PFileTransDlg FLogDlg = NULL;
static PFileTransDlg SendDlg = NULL;
static PProtoDlg PtDlg = NULL;

BOOL OpenFTDlg(PFileVar fv)
{
  PFileTransDlg FTDlg;
#ifdef I18N
  HWND HFTDlg;
#endif

  FTDlg = new CFileTransDlg();
  
  if (FTDlg!=NULL)
  {
#ifdef I18N
    FTDlg->Create(fv, &cv, &ts);
#else
    FTDlg->Create(fv, &cv);
#endif
    FTDlg->RefreshNum();
    if (fv->OpId == OpLog)
      FTDlg->ShowWindow(SW_MINIMIZE);
  }

  if (fv->OpId==OpLog)
    FLogDlg = FTDlg; /* Log */
  else
    SendDlg = FTDlg; /* File send */

#ifdef I18N
  HFTDlg=FTDlg->GetSafeHwnd();

  GetDlgItemText(HFTDlg, IDC_TRANS_FILENAME, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_FILETRANS_FILENAME", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(HFTDlg, IDC_TRANS_FILENAME, ts.UIMsg);
  GetDlgItemText(HFTDlg, IDC_FULLPATH_LABEL, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_FILETRANS_FULLPATH", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(HFTDlg, IDC_FULLPATH_LABEL, ts.UIMsg);
  GetDlgItemText(HFTDlg, IDC_TRANS_TRANS, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_FILETRANS_TRNAS", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(HFTDlg, IDC_TRANS_TRANS, ts.UIMsg);
  GetDlgItemText(HFTDlg, IDCANCEL, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("BTN_CANCEL", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(HFTDlg, IDCANCEL, ts.UIMsg);
  GetDlgItemText(HFTDlg, IDC_TRANSPAUSESTART, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_FILETRANS_PAUSE", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(HFTDlg, IDC_TRANSPAUSESTART, ts.UIMsg);
  GetDlgItemText(HFTDlg, IDC_TRANSHELP, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("BTN_HELP", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(HFTDlg, IDC_TRANSHELP, ts.UIMsg);
#endif

  return (FTDlg!=NULL);
}

BOOL NewFileVar(PFileVar *fv)
{
  if ((*fv)==NULL)
  {
    *fv = (PFileVar)malloc(sizeof(TFileVar));
    if ((*fv)!=NULL)
    {
      memset(*fv, 0, sizeof(TFileVar));
      strcpy((*fv)->FullName,ts.FileDir);
      AppendSlash((*fv)->FullName);
      (*fv)->DirLen = strlen((*fv)->FullName);
      (*fv)->FileOpen = FALSE;
      (*fv)->OverWrite = ((ts.FTFlag & FT_RENAME) == 0);
      (*fv)->HMainWin = HVTWin;
      (*fv)->Success = FALSE;
      (*fv)->NoMsg = FALSE;
    }
  }

  return ((*fv)!=NULL);
}

void FreeFileVar(PFileVar *fv)
{
  if ((*fv)!=NULL)
  {
    if ((*fv)->FileOpen) _lclose((*fv)->FileHandle);
    if ((*fv)->FnStrMemHandle>0)
    {
      GlobalUnlock((*fv)->FnStrMemHandle);
      GlobalFree((*fv)->FnStrMemHandle);
    }
    free(*fv);
    *fv = NULL;
  }
}

// &h をホスト名に置換 (2007.5.14)
void ConvertLogname(char *c)
{
  char buf[MAXPATHLEN], buf2[MAXPATHLEN], *p = c;

  memset(buf, 0, sizeof(buf));

  while(*p != '\0') {
    if (*p == '&' && *(p+1) != '\0') {
      switch (*(p+1)) {
        case 'h':
          if (cv.Open) {
            if (cv.PortType == IdTCPIP) {
              _snprintf(buf2, sizeof(buf2), "%s%s", buf, ts.HostName);
              strncpy(buf, buf2, sizeof(buf)-strlen(buf)-1);
            }
            else if (cv.PortType == IdSerial) {
              _snprintf(buf2, sizeof(buf2), "%sCOM%d", buf, ts.ComPort);
              strncpy(buf, buf2, sizeof(buf)-strlen(buf)-1);
            }
          }
          break;
        default:
          if (strlen(buf) < sizeof(buf)-3) {
            strncat(buf, p, 2);
          }
      }
      p++;
    }
    else {
          if (strlen(buf) < sizeof(buf)-2) {
            strncat(buf, p, 1);
          }
    }
    p++;
  }
  strcpy(c, buf);
}

extern "C" {
void LogStart()
{
	LONG Option;
	char *logdir;

	if ((FileLog) || (BinLog)) return;

	if (! LoadTTFILE()) return;
	if (! NewFileVar(&LogVar))
	{
		FreeTTFILE();
		return;
	}
	LogVar->OpId = OpLog;

	if (strlen(ts.LogDefaultPath) > 0) {
		logdir = ts.LogDefaultPath;
	}
	else if (strlen(ts.FileDir) > 0) {
		logdir = ts.FileDir;
	}
	else {
		logdir = ts.HomeDir;
	}

	if (strlen(&(LogVar->FullName[LogVar->DirLen]))==0)
	{
		// LOWORD
		// 0x0001 = Binary
		// HIWORD
		// 0x0001 = Append
		// 0x1000 = plain text (2005.2.20 yutaka)
		// 0x2000 = timestamp (2006.7.23 maya)
		// teraterm.iniの設定を見てからデフォルトオプションを決める。(2005.5.7 yutaka)
		Option = MAKELONG(ts.TransBin,ts.Append |
						  (0x1000 * ts.LogTypePlainText) |
						  (0x2000 * ts.LogTimestamp));

		// ログのデフォルトファイル名を設定 (2006.8.28 maya)
		strncat(LogVar->FullName, ts.LogDefaultName, sizeof(LogVar->FullName));

		ParseStrftimeFileName(LogVar->FullName);

		// &h をホスト名に置換 (2007.5.14)
		ConvertLogname(LogVar->FullName);

		if (! (*GetTransFname)(LogVar, logdir, GTF_LOG, &Option))
		{
			FreeFileVar(&LogVar);
			FreeTTFILE();
			return;
		}
		ts.TransBin = LOWORD(Option);
		ts.Append = HIWORD(Option);

		if (ts.Append & 0x1000) {
			ts.LogTypePlainText = 1;
		} else {
			ts.LogTypePlainText = 0;
		}

		if (ts.Append & 0x2000) {
			ts.LogTimestamp = 1;
		}
		else {
			ts.LogTimestamp = 0;
		}

		ts.Append &= 0x1; // 1bitにマスクする

	}
	else {
		// LogVar->DirLen = 0 だとここに来る
		// フルパス・相対パスともに LogVar->FullName に入れておく必要がある
		char FileName[MAX_PATH];

		// フルパス化
		strncpy(FileName, LogVar->FullName, sizeof(FileName)-1);
		ConvFName(logdir,FileName,"",LogVar->FullName);

		ParseStrftimeFileName(LogVar->FullName);

		// &h をホスト名に置換 (2007.5.14)
		ConvertLogname(LogVar->FullName);
		(*SetFileVar)(LogVar);
	}

	if (ts.TransBin > 0)
	{
		BinLog = TRUE;
		FileLog = FALSE;
		if (! CreateBinBuf())
		{
			FileTransEnd(OpLog);
			return;
		}
	}
	else {
		BinLog = FALSE;
		FileLog = TRUE;
		if (! CreateLogBuf())
		{
			FileTransEnd(OpLog);
			return;
		}
	}
	cv.LStart = cv.LogPtr;
	cv.LCount = 0;

	HelpId = HlpFileLog;
	/* 2007.05.24 Gentaro */
	eLineEnd = Line_LineHead;

	if (ts.Append > 0)
	{
		LogVar->FileHandle = _lopen(LogVar->FullName,OF_WRITE);
		if (LogVar->FileHandle>0){
			_llseek(LogVar->FileHandle,0,2);
			/* 2007.05.24 Gentaro
				If log file already exists,
				a newline is inserted before the first timestamp.
			*/
			eLineEnd = Line_FileHead;
		}
		else
			LogVar->FileHandle = _lcreat(LogVar->FullName,0);
	}
	else
		LogVar->FileHandle = _lcreat(LogVar->FullName,0);
	LogVar->FileOpen = (LogVar->FileHandle>0);
	if (! LogVar->FileOpen)
	{
		FileTransEnd(OpLog);
		return;
	}
	LogVar->ByteCount = 0;

	if (! OpenFTDlg(LogVar))
		FileTransEnd(OpLog);
}
}

void LogPut1(BYTE b)
{
  LogLast = b;
  cv.LogBuf[cv.LogPtr] = b;
  cv.LogPtr++;
  if (cv.LogPtr>=InBuffSize)
    cv.LogPtr = cv.LogPtr-InBuffSize;

  if (FileLog)
  {
    if (cv.LCount>=InBuffSize)
    {
      cv.LCount = InBuffSize;
      cv.LStart = cv.LogPtr;
    }
    else cv.LCount++;
  }
  else
    cv.LCount = 0;

  if (DDELog)
  {
    if (cv.DCount>=InBuffSize)
    {
      cv.DCount = InBuffSize;
      cv.DStart = cv.LogPtr;
    }
    else cv.DCount++;
  }
  else {
    cv.DCount = 0;
	// ログ採取中にマクロがストールする問題への修正。
	// ログ採取中に一度マクロを止めると、バッファのインデックスが同期取れなくなり、
	// 再度マクロを流しても正しいデータが送れないのが原因。
	// マクロを停止させた状態でもインデックスの同期を取るようにした。
	// (2006.12.26 yutaka)
    cv.DStart = cv.LogPtr;
  }
}

void Log1Byte(BYTE b)
{
  if (b==0x0d)
  {
    LogLast = b;
    return;
  }
  if ((b==0x0a) && (LogLast==0x0d))
    LogPut1(0x0d);
  LogPut1(b);
}

  static BOOL Get1(PCHAR Buf, int *Start, int *Count, PBYTE b)
  {
    if (*Count<=0) return FALSE;
    *b = Buf[*Start];
    (*Start)++;
    if (*Start>=InBuffSize)
      *Start = *Start-InBuffSize;
    (*Count)--;
    return TRUE;
  }



static CRITICAL_SECTION g_filelog_lock;   /* ロック用変数 */

void logfile_lock_initialize(void)
{
	InitializeCriticalSection(&g_filelog_lock);
}

static inline void logfile_lock(void)
{
	EnterCriticalSection(&g_filelog_lock);
}

static inline void logfile_unlock(void)
{
	LeaveCriticalSection(&g_filelog_lock);
}

 // コメントをログへ追加する
void CommentLogToFile(char *buf, int size)
{
	DWORD wrote;

	if (LogVar == NULL || !LogVar->FileOpen) {
#ifdef I18N
		char uimsg[MAX_UIMSG];
		strcpy(uimsg, "ERROR");
		get_lang_msg("MSG_ERROR", uimsg, ts.UILanguageFile);
		strcpy(ts.UIMsg, "It is not opened by the log file yet.");
		get_lang_msg("MSG_COMMENT_LOG_OPEN_ERROR", ts.UIMsg, ts.UILanguageFile);
		::MessageBox(NULL, ts.UIMsg, uimsg, MB_OK|MB_ICONEXCLAMATION);
#else
		::MessageBox(NULL, "It is not opened by the log file yet.", "ERROR", MB_OK|MB_ICONEXCLAMATION);
#endif
		return;
	}

	logfile_lock();
	WriteFile((HANDLE)LogVar->FileHandle, buf, size, &wrote, NULL);
	WriteFile((HANDLE)LogVar->FileHandle, "\r\n", 2, &wrote, NULL); // 改行
	/* Set Line End Flag
		2007.05.24 Gentaro
	*/
	eLineEnd = Line_LineHead;
	logfile_unlock();
}

void LogToFile()
{
	PCHAR Buf;
	int Start, Count;
	BYTE b;

	if (! LogVar->FileOpen) return;
	if (FileLog)
	{
		Buf = cv.LogBuf;
		Start = cv.LStart;
		Count = cv.LCount;
	}
	else if (BinLog)
	{
		Buf = cv.BinBuf;
		Start = cv.BStart;
		Count = cv.BCount;
	}
	else
		return;

	if (Buf==NULL) return;
	if (Count==0) return;

	// ロックを取る(2004.8.6 yutaka)
	logfile_lock();

	while (Get1(Buf,&Start,&Count,&b))
	{
		if (((cv.FilePause & OpLog)==0) && (! cv.ProtoFlag))
		{
			// 時刻を書き出す(2006.7.23 maya)
			// 日付フォーマットを日本ではなく世界標準に変更した (2006.7.23 yutaka)
			/* 2007.05.24 Gentaro */
			if ( ts.LogTimestamp && eLineEnd ) {
#if 0
				SYSTEMTIME	LocalTime;
				GetLocalTime(&LocalTime);
				char strtime[27];

				// format time
				sprintf(strtime, "[%04d/%02d/%02d %02d:%02d:%02d.%03d] ",
						LocalTime.wYear, LocalTime.wMonth,LocalTime.wDay,
						LocalTime.wHour, LocalTime.wMinute, LocalTime.wSecond,
						LocalTime.wMilliseconds);
#else
					time_t tick = time(NULL);
					char *strtime = ctime(&tick); 
#endif
				/* 2007.05.24 Gentaro */
				if( eLineEnd == Line_FileHead ){
					_lwrite(LogVar->FileHandle,"\r\n",2);
				}
				_lwrite(LogVar->FileHandle,"[",1);
				// 変換した文字列の終端に \n が含まれているので取り除く。
				_lwrite(LogVar->FileHandle, strtime, strlen(strtime) - 1);
				_lwrite(LogVar->FileHandle,"] ",2);
			}
			
			/* 2007.05.24 Gentaro */
			if( b == 0x0a ){
				eLineEnd = Line_LineHead; /* set endmark*/
			}
			else {
				eLineEnd = Line_Other; /* clear endmark*/
			}

			_lwrite(LogVar->FileHandle,(PCHAR)&b,1);
			(LogVar->ByteCount)++;
		}
	}

	logfile_unlock();

	if (FileLog)
	{
		cv.LStart = Start;
		cv.LCount = Count;
	}
	else {
		cv.BStart = Start;
		cv.BCount = Count;
	}
	if (((cv.FilePause & OpLog) !=0) || cv.ProtoFlag) return;
	if (FLogDlg!=NULL)
		FLogDlg->RefreshNum();
}

BOOL CreateLogBuf()
{
  if (cv.HLogBuf==NULL)
  {
    cv.HLogBuf = GlobalAlloc(GMEM_MOVEABLE,InBuffSize);
    cv.LogBuf = NULL;
    cv.LogPtr = 0;
    cv.LStart = 0;
    cv.LCount = 0;
    cv.DStart = 0;
    cv.DCount = 0;
  }
  return (cv.HLogBuf!=NULL);
}

void FreeLogBuf()
{
  if ((cv.HLogBuf==NULL) || FileLog || DDELog) return;
  if (cv.LogBuf!=NULL) GlobalUnlock(cv.HLogBuf);
  GlobalFree(cv.HLogBuf);
  cv.HLogBuf = NULL;
  cv.LogBuf = NULL;
  cv.LogPtr = 0;
  cv.LStart = 0;
  cv.LCount = 0;
  cv.DStart = 0;
  cv.DCount = 0;
}

BOOL CreateBinBuf()
{
  if (cv.HBinBuf==NULL)
  {
    cv.HBinBuf = GlobalAlloc(GMEM_MOVEABLE,InBuffSize);
    cv.BinBuf = NULL;
    cv.BinPtr = 0;
    cv.BStart = 0;
    cv.BCount = 0;
  }
  return (cv.HBinBuf!=NULL);
}

void FreeBinBuf()
{
  if ((cv.HBinBuf==NULL) || BinLog) return;
  if (cv.BinBuf!=NULL) GlobalUnlock(cv.HBinBuf);
  GlobalFree(cv.HBinBuf);
  cv.HBinBuf = NULL;
  cv.BinBuf = NULL;
  cv.BinPtr = 0;
  cv.BStart = 0;
  cv.BCount = 0;
}

extern "C" {
void FileSendStart()
{
  LONG Option;

  if (! cv.Ready || FSend) return;
  if (cv.ProtoFlag)
  {
    FreeFileVar(&SendVar);
    return;
  }

  if (! LoadTTFILE()) return;
  if (! NewFileVar(&SendVar))
  {
    FreeTTFILE();
    return;
  }
  SendVar->OpId = OpSendFile;

  FSend = TRUE;

  if (strlen(&(SendVar->FullName[SendVar->DirLen]))==0)
  {
    Option = MAKELONG(ts.TransBin,0);
	SendVar->FullName[0] = 0;
    if (! (*GetTransFname)(SendVar, ts.FileDir, GTF_SEND, &Option))
	{
      FileTransEnd(OpSendFile);
      return;
    }
    ts.TransBin = LOWORD(Option);
  }
  else
    (*SetFileVar)(SendVar);

  SendVar->FileHandle = _lopen(SendVar->FullName,OF_READ);
  SendVar->FileOpen = (SendVar->FileHandle>0);
  if (! SendVar->FileOpen)
  {
    FileTransEnd(OpSendFile);
    return;
  }
  SendVar->ByteCount = 0;

  TalkStatus = IdTalkFile;
  FileRetrySend = FALSE;
  FileRetryEcho = FALSE;
  FileCRSend = FALSE;

  if (! OpenFTDlg(SendVar))
    FileTransEnd(OpSendFile);
}
}

void FileTransEnd(WORD OpId)
/* OpId = 0: close Log and FileSend
      OpLog: close Log
 OpSendFile: close FileSend */
{
  if (((OpId==0) || (OpId==OpLog)) &&
      (FileLog || BinLog))
  {
    FileLog = FALSE;
    BinLog = FALSE;
    if (FLogDlg!=NULL)
    {
      FLogDlg->DestroyWindow();
      FLogDlg = NULL;
    }
    FreeFileVar(&LogVar);
    FreeLogBuf();
    FreeBinBuf();
    FreeTTFILE();
  }

  if (((OpId==0) || (OpId==OpSendFile)) &&
      FSend)
  {
    FSend = FALSE;
    TalkStatus = IdTalkKeyb;
    if (SendDlg!=NULL)
    {
      SendDlg->DestroyWindow();
      SendDlg = NULL;
    }
    FreeFileVar(&SendVar);
    FreeTTFILE();
  }

  EndDdeCmnd(0);
}

// vtwin.cpp からコールしたいので、extern "C"を付ける。(2004.11.3 yutaka)
extern "C" {
  int FSOut1(BYTE b)
  {
    if (ts.TransBin > 0)
      return CommBinaryOut(&cv,(PCHAR)&b,1);
    else if ((b>=0x20) || (b==0x09) || (b==0x0A) || (b==0x0D))
      return CommTextOut(&cv,(PCHAR)&b,1);
    else
      return 1;
  }

  int FSEcho1(BYTE b)
  {
    if (ts.TransBin > 0)
      return CommBinaryEcho(&cv,(PCHAR)&b,1);
    else
      return CommTextEcho(&cv,(PCHAR)&b,1);
  }
}

extern "C" {
void FileSend()
{
  WORD c, fc;
  LONG BCOld;

  if ((SendDlg==NULL) ||
      ((cv.FilePause & OpSendFile) !=0)) return;

  BCOld = SendVar->ByteCount;

  if (FileRetrySend)
  {
    FileRetryEcho = (ts.LocalEcho>0);
    c = FSOut1(FileByte);
    FileRetrySend = (c==0);
    if (FileRetrySend) return;
  }

  if (FileRetryEcho)
  {
    c = FSEcho1(FileByte);
    FileRetryEcho = (c==0);
    if (FileRetryEcho) return;
  }

  do {
    fc = _lread(SendVar->FileHandle,&FileByte,1);
    SendVar->ByteCount = SendVar->ByteCount + fc;

    if (FileCRSend && (fc==1) && (FileByte==0x0A))
    {
      fc = _lread(SendVar->FileHandle,&FileByte,1);
      SendVar->ByteCount = SendVar->ByteCount + fc;
    }

    if (fc!=0)
    {
      c = FSOut1(FileByte);
      FileCRSend = (ts.TransBin==0) && (FileByte==0x0D);
      FileRetrySend = (c==0);
      if (FileRetrySend)
      {
	if (SendVar->ByteCount != BCOld)
	  SendDlg->RefreshNum();
	  return;
      }
      if (ts.LocalEcho>0)
      {
	c = FSEcho1(FileByte);
	FileRetryEcho = (c==0);
	if (FileRetryEcho) return;
      }
    }
    if ((fc==0) || (SendVar->ByteCount % 100 == 0))
    {
      SendDlg->RefreshNum();
      BCOld = SendVar->ByteCount;
      if (fc!=0) return;
    }
  } while (fc!=0);

  FileTransEnd(OpSendFile);
}
}

extern "C" {
void FLogChangeButton(BOOL Pause)
{
  if (FLogDlg!=NULL)
    FLogDlg->ChangeButton(Pause);
}
}

extern "C" {
void FLogRefreshNum()
{
  if (FLogDlg!=NULL)
    FLogDlg->RefreshNum();
}
}

BOOL OpenProtoDlg(PFileVar fv, int IdProto, int Mode, WORD Opt1, WORD Opt2)
{
  int vsize;
  PProtoDlg pd;
#ifdef I18N
  HWND Hpd;
#endif

  ProtoId = IdProto;

  switch (ProtoId) {
    case PROTO_KMT:
      vsize = sizeof(TKmtVar);
      break;
    case PROTO_XM:
      vsize = sizeof(TXVar);
      break;
    case PROTO_ZM:
      vsize = sizeof(TZVar);
      break;
    case PROTO_BP:
      vsize = sizeof(TBPVar);
      break;
    case PROTO_QV:
      vsize = sizeof(TQVVar);
      break;
  }
  ProtoVar = (PCHAR)malloc(vsize);
  if (ProtoVar==NULL) return FALSE;

  switch (ProtoId) {
    case PROTO_KMT:
      ((PKmtVar)ProtoVar)->KmtMode = Mode;
      break;
    case PROTO_XM:
      ((PXVar)ProtoVar)->XMode = Mode;
      ((PXVar)ProtoVar)->XOpt = Opt1;
      ((PXVar)ProtoVar)->TextFlag = 1 - (Opt2 & 1);
      break;
    case PROTO_ZM:
      ((PZVar)ProtoVar)->BinFlag = (Opt1 & 1) != 0;
      ((PZVar)ProtoVar)->ZMode = Mode;
      break;
    case PROTO_BP:
      ((PBPVar)ProtoVar)->BPMode = Mode;
      break;
    case PROTO_QV:
      ((PQVVar)ProtoVar)->QVMode = Mode;
      break;
  }

  pd = new CProtoDlg();
  if (pd==NULL)
  {
    free(ProtoVar);
    ProtoVar = NULL;
    return FALSE;
  }
#ifdef I18N
  pd->Create(fv,&ts);
#else
  pd->Create(fv);
#endif

#ifdef I18N
  Hpd=pd->GetSafeHwnd();

  GetDlgItemText(Hpd, IDC_PROT_FILENAME, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_PROT_FIELNAME", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(Hpd, IDC_PROT_FILENAME, ts.UIMsg);
  GetDlgItemText(Hpd, IDC_PROT_PROT, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_PROT_PROTO", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(Hpd, IDC_PROT_PROT, ts.UIMsg);
  GetDlgItemText(Hpd, IDC_PROT_PACKET, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_PROT_PACKET", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(Hpd, IDC_PROT_PACKET, ts.UIMsg);
  GetDlgItemText(Hpd, IDC_PROT_TRANS, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("DLG_PROT_TRANS", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(Hpd, IDC_PROT_TRANS, ts.UIMsg);
  GetDlgItemText(Hpd, IDCANCEL, ts.UIMsg, sizeof(ts.UIMsg));
  get_lang_msg("BTN_CANCEL", ts.UIMsg, ts.UILanguageFile);
  SetDlgItemText(Hpd, IDCANCEL, ts.UIMsg);
#endif
  
  (*ProtoInit)(ProtoId,FileVar,ProtoVar,&cv,&ts);

  PtDlg = pd;
  return TRUE;
}

extern "C" {
void CloseProtoDlg()
{
  if (PtDlg!=NULL)
  {
    PtDlg->DestroyWindow();
    PtDlg = NULL;

    ::KillTimer(FileVar->HMainWin,IdProtoTimer);
    if ((ProtoId==PROTO_QV) &&
	(((PQVVar)ProtoVar)->QVMode==IdQVSend))
      CommTextOut(&cv,"\015",1);
    if (FileVar->LogFlag)
      _lclose(FileVar->LogFile); 
    FileVar->LogFile = 0;
    if (ProtoVar!=NULL)
    {
      free(ProtoVar);
      ProtoVar = NULL;
    }
  }
}
}

BOOL ProtoStart()
{
  if (cv.ProtoFlag) return FALSE;
  if (FSend)
  {
    FreeFileVar(&FileVar);
    return FALSE;
  }

  if (! LoadTTFILE()) return FALSE;
  NewFileVar(&FileVar);

  if (FileVar==NULL)
  {
    FreeTTFILE();
    return FALSE;
  }
  cv.ProtoFlag = TRUE;
  return TRUE;
}

void ProtoEnd()
{
  if (! cv.ProtoFlag) return;
  cv.ProtoFlag = FALSE;

  /* Enable transmit delay (serial port) */
  cv.DelayFlag = TRUE;
  TalkStatus = IdTalkKeyb;

  CloseProtoDlg();

  if ((FileVar!=NULL) && FileVar->Success)
    EndDdeCmnd(1);
  else
    EndDdeCmnd(0);

  FreeTTFILE();
  FreeFileVar(&FileVar);
}

extern "C" {
int ProtoDlgParse()
{
  int P;

  P = ActiveWin;
  if (PtDlg==NULL) return P;

  if ((*ProtoParse)(ProtoId,FileVar,ProtoVar,&cv))
    P = 0; /* continue */
  else {
    CommSend(&cv);
    ProtoEnd();
  }
  return P;
}
}

extern "C" {
void ProtoDlgTimeOut()
{
  if (PtDlg!=NULL)
    (*ProtoTimeOutProc)(ProtoId,FileVar,ProtoVar,&cv);
}
}

extern "C" {
void ProtoDlgCancel()
{
  if ((PtDlg!=NULL) &&
      (*ProtoCancel)(ProtoId,FileVar,ProtoVar,&cv))
    ProtoEnd();
}
}

extern "C" {
void KermitStart(int mode)
{
  WORD w;

  if (! ProtoStart()) return;

  switch (mode) {
    case IdKmtSend:
      FileVar->OpId = OpKmtSend;
      if (strlen(&(FileVar->FullName[FileVar->DirLen]))==0)
      {
	if (! (*GetMultiFname)(FileVar,ts.FileDir,GMF_KERMIT,&w) ||
	    (FileVar->NumFname==0))
	{
	  ProtoEnd();
	  return;
	}
      }
      else
	(*SetFileVar)(FileVar);
	  break;
    case IdKmtReceive:
      FileVar->OpId = OpKmtRcv;
      break;
    case IdKmtGet:
      FileVar->OpId = OpKmtSend;
      if (strlen(&(FileVar->FullName[FileVar->DirLen]))==0)
      {
	if (! (*GetGetFname)(FileVar->HMainWin,FileVar) || 
	  (strlen(FileVar->FullName)==0))
	{
	  ProtoEnd();
	  return;
	}
      }
      else
	(*SetFileVar)(FileVar);
	  break;
    case IdKmtFinish:
      FileVar->OpId = OpKmtFin;
      break;
    default:
      ProtoEnd();
      return;
  }
  TalkStatus = IdTalkQuiet;

  /* disable transmit delay (serial port) */
  cv.DelayFlag = FALSE;

  if (! OpenProtoDlg(FileVar,PROTO_KMT,mode,0,0))
    ProtoEnd();
}
}

extern "C" {
void XMODEMStart(int mode)
{
  LONG Option;

  if (! ProtoStart()) return;

  if (mode==IdXReceive)
    FileVar->OpId = OpXRcv;
  else
    FileVar->OpId = OpXSend;

  if (strlen(&(FileVar->FullName[FileVar->DirLen]))==0)
  {
    Option = MAKELONG(ts.XmodemBin,ts.XmodemOpt);
    if (! (*GetXFname)(FileVar->HMainWin,
      mode==IdXReceive,&Option,FileVar,ts.FileDir))
    {
      ProtoEnd();
      return;
    }
    ts.XmodemOpt = HIWORD(Option);
    ts.XmodemBin = LOWORD(Option);
  }
  else
    (*SetFileVar)(FileVar);

  if (mode==IdXReceive)
    FileVar->FileHandle = _lcreat(FileVar->FullName,0);
  else
    FileVar->FileHandle = _lopen(FileVar->FullName,OF_READ);

  FileVar->FileOpen = FileVar->FileHandle>0;
  if (! FileVar->FileOpen)
  {
    ProtoEnd();
    return;
  }
  TalkStatus = IdTalkQuiet;

  /* disable transmit delay (serial port) */
  cv.DelayFlag = FALSE;

  if (! OpenProtoDlg(FileVar,PROTO_XM,mode,
		     ts.XmodemOpt,ts.XmodemBin))
    ProtoEnd();
}
}

extern "C" {
void ZMODEMStart(int mode)
{
  WORD Opt;

  if (! ProtoStart()) return;

  if (mode==IdZSend)
  {
    Opt = ts.XmodemBin;
    FileVar->OpId = OpZSend;
    if (strlen(&(FileVar->FullName[FileVar->DirLen]))==0)
    {
      if (! (*GetMultiFname)(FileVar,ts.FileDir,GMF_Z,&Opt) ||
	  (FileVar->NumFname==0))
      {
	ProtoEnd();
	return;
      }
      ts.XmodemBin = Opt;
    }
    else
      (*SetFileVar)(FileVar);
  }
  else /* IdZReceive or IdZAuto */
    FileVar->OpId = OpZRcv;

  TalkStatus = IdTalkQuiet;

  /* disable transmit delay (serial port) */
  cv.DelayFlag = FALSE;

  if (! OpenProtoDlg(FileVar,PROTO_ZM,mode,Opt,0))
    ProtoEnd();
}
}

extern "C" {
void BPStart(int mode)
{
  LONG Option;

  if (! ProtoStart()) return;
  if (mode==IdBPSend)
  {
    FileVar->OpId = OpBPSend;
    if (strlen(&(FileVar->FullName[FileVar->DirLen]))==0)
    {
	  FileVar->FullName[0] = 0;
      if (! (*GetTransFname)(FileVar, ts.FileDir, GTF_BP, &Option))
	  {
	ProtoEnd();
	return;
      }
    }
    else
      (*SetFileVar)(FileVar);
  }
  else /* IdBPReceive or IdBPAuto */
    FileVar->OpId = OpBPRcv;

  TalkStatus = IdTalkQuiet;

  /* disable transmit delay (serial port) */
  cv.DelayFlag = FALSE;

  if (! OpenProtoDlg(FileVar,PROTO_BP,mode,0,0))
    ProtoEnd();
}
}

extern "C" {
void QVStart(int mode)
{
  WORD W;

  if (! ProtoStart()) return;

  if (mode==IdQVSend)
  {
    FileVar->OpId = OpQVSend;
    if (strlen(&(FileVar->FullName[FileVar->DirLen]))==0)
    {
      if (! (*GetMultiFname)(FileVar,ts.FileDir,GMF_QV, &W) ||
	  (FileVar->NumFname==0))
      {
	ProtoEnd();
	return;
      }
    }
    else
      (*SetFileVar)(FileVar);
  }
  else
    FileVar->OpId = OpQVRcv;

  TalkStatus = IdTalkQuiet;

  /* disable transmit delay (serial port) */
  cv.DelayFlag = FALSE;

  if (! OpenProtoDlg(FileVar,PROTO_QV,mode,0,0))
    ProtoEnd();
}
}

/*
 * $Log: not supported by cvs2svn $
 * Revision 1.12  2007/05/25 09:56:05  yutakapon
 * タイムスタンプ付きログで1KBごとに不要な改行が入るバグを修正。
 *
 * Revision 1.11  2007/05/14 14:07:14  maya
 * バッファをクリアしていないので落ちる問題を修正した。
 *
 * Revision 1.10  2007/05/14 13:29:58  maya
 * ログファイル名中の &h を、接続中のホスト名に変換する機能を追加した。
 *
 * Revision 1.9  2007/01/21 16:18:35  maya
 * 表示メッセージの読み込み対応
 *
 * Revision 1.8  2007/01/04 15:11:44  maya
 * 表示メッセージの読み込み対応
 *
 * Revision 1.7  2006/12/25 16:13:54  yutakapon
 * ログ採取中にマクロがストールする問題への修正。
 * ログ採取中に一度マクロを止めると、バッファのインデックスが同期取れなくなり、
 * 再度マクロを流しても正しいデータが送れないのが原因。
 * マクロを停止させた状態でもインデックスの同期を取るようにした。
 *
 * Revision 1.6  2006/08/28 12:27:16  maya
 * デフォルトのログファイル名を指定できるようにした。
 *   エディットコントロールを "Additional settings" ダイアログに追加した。
 *   teraterm.ini ファイルに LogDefaultName エントリを追加した。
 *   ファイル名に strftime のフォーマットを使えるようにした。
 *
 * Revision 1.5  2006/07/23 14:12:26  yutakakn
 * ログに含める日付フォーマットを世界標準書式に変更した。
 *
 * Revision 1.4  2006/07/22 16:15:54  maya
 * ログ記録時に時刻も書き込む機能を追加した。
 *
 * Revision 1.3  2005/05/07 09:49:24  yutakakn
 * teraterm.iniに LogTypePlainText を追加した。
 *
 * Revision 1.2  2005/02/20 14:51:29  yutakakn
 * ログファイルの種別に"plain text"を追加。このオプションが有効の場合は、ログファイルに
 * ASCII非表示文字の採取をしない。
 *
 * 現在、無視するキャラクタは以下のとおり。
 * 　・BS
 * 　・ASCII(0x00-0x1f)のうち非表示なもの
 *
 * ただし、例外として以下のものはログ採取対象。
 * 　・HT
 * 　・CR
 * 　・LF
 * 
 */
