/*
 * Copyright (C) 1994-1998 T. Teranishi
 * (C) 2004- TeraTerm Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* TERATERM.EXE, VT terminal emulation */
#include "teraterm.h"
#include "tttypes.h"
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#if !defined(_CRTDBG_MAP_ALLOC)
#define _CRTDBG_MAP_ALLOC
#endif
#include <stdlib.h>
#include <crtdbg.h>
#include <assert.h>

#include "buffer.h"
#include "ttwinman.h"
#include "ttcommon.h"
#include "commlib.h"
#include "vtdisp.h"
#include "keyboard.h"
#include "ttlib.h"
#include "filesys.h"
#include "teraprn.h"
#include "telnet.h"
#include "ttime.h"
#include "clipboar.h"
#include "codeconv.h"
#include "unicode.h"
#include "ttdde.h"
#include "checkeol.h"
#include "asprintf.h"

#include "vtterm.h"

#include "unicode_test.h"
// #define DEBUG_DUMP_INPUTCODE 1

static void ParseFirst(BYTE b);

#define Accept8BitCtrl ((VTlevel >= 2) && (ts.TermFlag & TF_ACCEPT8BITCTRL))

  /* Parsing modes */
#define ModeFirst 0
#define ModeESC   1
#define ModeDCS   2
#define ModeDCUserKey 3
#define ModeSOS   4
#define ModeCSI   5
#define ModeXS    6
#define ModeDLE   7
#define ModeCAN   8
#define ModeIgnore 9

#define NParamMax  16
#define NSParamMax 16
#define IntCharMax  5

/* DEC Locator Flag */
#define DecLocatorOneShot    1
#define DecLocatorPixel      2
#define DecLocatorButtonDown 4
#define DecLocatorButtonUp   8
#define DecLocatorFiltered   16

void RingBell(int type);
void VisualBell();
BOOL DecLocatorReport(int Event, int Button);

/* character attribute */
static TCharAttr CharAttr;

/* various modes of VT emulation */
static BOOL RelativeOrgMode;
static BOOL InsertMode;
static BOOL LFMode;
static BOOL ClearThenHome;
static BOOL AutoWrapMode;
static BOOL FocusReportMode;
static BOOL AltScr;
static BOOL LRMarginMode;
static BOOL RectangleMode;
static BOOL BracketedPaste;

static char BracketStart[] = "\033[200~";
static char BracketEnd[] = "\033[201~";
static int BracketStartLen = (sizeof(BracketStart)-1);
static int BracketEndLen = (sizeof(BracketEnd)-1);

static int VTlevel;

static BOOL AcceptWheelToCursor;

// save/restore cursor
typedef struct {
	int CursorX, CursorY;
	TCharAttr Attr;
	int Glr[2], Gn[4]; // G0-G3, GL & GR
	BOOL AutoWrapMode;
	BOOL RelativeOrgMode;
} TStatusBuff;
typedef TStatusBuff *PStatusBuff;

// currently only used for AUTO CR/LF receive mode
static BYTE PrevCharacter;
static BOOL PrevCRorLFGeneratedCRLF;	  // indicates that previous CR or LF really generated a CR+LF

static BYTE LastPutCharacter;

// status buffer for main screen & status line
static TStatusBuff SBuff1, SBuff2, SBuff3;

static BOOL ESCFlag, JustAfterESC;
static BOOL KanjiIn;				// TRUE = MBCSの1byte目を受信している
static BOOL EUCkanaIn, EUCsupIn;
static int  EUCcount;
static BOOL Special;

static int Param[NParamMax+1];
static int SubParam[NParamMax+1][NSParamMax+1];
static int NParam, NSParam[NParamMax+1];
static BOOL FirstPrm;
static BYTE IntChar[IntCharMax+1];
static int ICount;
static BYTE Prv;
static int ParseMode;
static int ChangeEmu;

typedef struct tstack {
	char *title;
	struct tstack *next;
} TStack;
typedef TStack *PTStack;
static PTStack TitleStack = NULL;

/* user defined keys */
static BOOL WaitKeyId, WaitHi;

/* GL, GR code group */
static int Glr[2];
/* G0, G1, G2, G3 code group */
static int Gn[4];
/* GL for single shift 2/3 */
static int GLtmp;
/* single shift 2/3 flag */
static BOOL SSflag;
/* JIS -> SJIS conversion flag */
static BOOL ConvJIS;
static WORD Kanji;
static BOOL Fallbacked;

// variables for status line mode
static int StatusX=0;
static BOOL StatusWrap=FALSE;
static BOOL StatusCursor=TRUE;
static int MainX, MainY; //cursor registers
static int MainTop, MainBottom; // scroll region registers
static BOOL MainWrap;
static BOOL MainCursor=TRUE;

/* status for printer escape sequences */
static BOOL PrintEX = TRUE; // printing extent
			    // (TRUE: screen, FALSE: scroll region)
static BOOL AutoPrintMode = FALSE;
static BOOL PrinterMode = FALSE;
static BOOL DirectPrn = FALSE;
PrintFile *PrintFile_;

/* User key */
static BYTE NewKeyStr[FuncKeyStrMax];
static int NewKeyId, NewKeyLen;

/* Mouse Report */
static int MouseReportMode;
static int MouseReportExtMode;
static unsigned int DecLocatorFlag;
static int LastX, LastY;
static int ButtonStat;
static int FilterTop, FilterBottom, FilterLeft, FilterRight;

/* Saved IME status */
static BOOL SavedIMEstatus;

/* Beep over-used */
static DWORD BeepStartTime = 0;
static DWORD BeepSuppressTime = 0;
static DWORD BeepOverUsedCount = 0;

static _locale_t CLocale = NULL;

typedef struct {
	CheckEOLData_t *check_eol;
	int log_cr_type;
} vtterm_work_t;

static vtterm_work_t vtterm_work;

void ClearParams()
{
	ICount = 0;
	NParam = 1;
	NSParam[1] = 0;
	Param[1] = 0;
	Prv = 0;
}

void ResetSBuffer(PStatusBuff sbuff)
{
	sbuff->CursorX = 0;
	sbuff->CursorY = 0;
	sbuff->Attr = DefCharAttr;
	if (ts.Language==IdJapanese) {
		sbuff->Gn[0] = IdASCII;
		sbuff->Gn[1] = IdKatakana;
		sbuff->Gn[2] = IdKatakana;
		sbuff->Gn[3] = IdKanji;
		sbuff->Glr[0] = 0;
		if ((ts.KanjiCode==IdJIS) && (ts.JIS7Katakana==0))
			sbuff->Glr[1] = 2;	// 8-bit katakana
		else
			sbuff->Glr[1] = 3;
	}
	else {
		sbuff->Gn[0] = IdASCII;
		sbuff->Gn[1] = IdSpecial;
		sbuff->Gn[2] = IdASCII;
		sbuff->Gn[3] = IdASCII;
		sbuff->Glr[0] = 0;
		sbuff->Glr[1] = 0;
	}
	sbuff->AutoWrapMode = TRUE;
	sbuff->RelativeOrgMode = FALSE;
}

void ResetAllSBuffers()
{
	ResetSBuffer(&SBuff1);
	// copy SBuff1 to SBuff2
	SBuff2 = SBuff1;
	SBuff3 = SBuff1;
}

void ResetCurSBuffer()
{
	PStatusBuff Buff;

	if (AltScr) {
		Buff = &SBuff3; // Alternate screen buffer
	}
	else {
		Buff = &SBuff1; // Normal screen buffer
	}
	ResetSBuffer(Buff);
	SBuff2 = *Buff;
}

void ResetTerminal() /*reset variables but don't update screen */
{
	DispReset();
	BuffReset();

	/* Attribute */
	CharAttr = DefCharAttr;
	Special = FALSE;
	BuffSetCurCharAttr(CharAttr);

	/* Various modes */
	InsertMode = FALSE;
	LFMode = (ts.CRSend == IdCRLF);
	AutoWrapMode = TRUE;
	AppliKeyMode = FALSE;
	AppliCursorMode = FALSE;
	AppliEscapeMode = FALSE;
	AcceptWheelToCursor = ts.TranslateWheelToCursor;
	RelativeOrgMode = FALSE;
	ts.ColorFlag &= ~CF_REVERSEVIDEO;
	AutoRepeatMode = TRUE;
	FocusReportMode = FALSE;
	MouseReportMode = IdMouseTrackNone;
	MouseReportExtMode = IdMouseTrackExtNone;
	DecLocatorFlag = 0;
	ClearThenHome = FALSE;
	RectangleMode = FALSE;

	ChangeTerminalID();

	LastX = 0;
	LastY = 0;
	ButtonStat = 0;

	if (CLocale == NULL) {
		CLocale = _create_locale(LC_ALL, "C");
	}

	/* Character sets */
	ResetCharSet();

	/* ESC flag for device control sequence */
	ESCFlag = FALSE;
	/* for TEK sequence */
	JustAfterESC = FALSE;

	/* Parse mode */
	ParseMode = ModeFirst;

	/* Clear printer mode */
	PrinterMode = FALSE;

	// status buffers
	ResetAllSBuffers();

	// Alternate Screen Buffer
	AltScr = FALSE;

	// Left/Right Margin Mode
	LRMarginMode = FALSE;

	// Bracketed Paste Mode
	BracketedPaste = FALSE;

	// Saved IME Status
	SavedIMEstatus = FALSE;

	// previous received character
	PrevCharacter = -1;	// none
	PrevCRorLFGeneratedCRLF = FALSE;

	LastPutCharacter = 0;

	// Beep over-used
	BeepStartTime = GetTickCount();
	BeepSuppressTime = BeepStartTime - ts.BeepSuppressTime * 1000;
	BeepStartTime -= (ts.BeepOverUsedTime * 1000);
	BeepOverUsedCount = ts.BeepOverUsedCount;

	{
		vtterm_work_t *vtterm = &vtterm_work;
		vtterm->check_eol = CheckEOLCreate();
		vtterm->log_cr_type = 0;
	}
}

void ResetCharSet()
{
	if (ts.Language==IdJapanese) {
		Gn[0] = IdASCII;
		Gn[1] = IdKatakana;
		Gn[2] = IdKatakana;
		Gn[3] = IdKanji;
		Glr[0] = 0;
		if ((ts.KanjiCode==IdJIS) && (ts.JIS7Katakana==0))
			Glr[1] = 2;	// 8-bit katakana
		else
			Glr[1] = 3;
	}
	else {
		Gn[0] = IdASCII;
		Gn[1] = IdSpecial;
		Gn[2] = IdASCII;
		Gn[3] = IdASCII;
		Glr[0] = 0;
		Glr[1] = 0;
		cv.SendCode = IdASCII;
		cv.EchoCode = IdASCII;
	}
	/* Kanji flag */
	KanjiIn = FALSE;
	EUCkanaIn = FALSE;
	EUCsupIn = FALSE;
	SSflag = FALSE;
	ConvJIS = FALSE;
	Fallbacked = FALSE;

	cv.Language = ts.Language;
	cv.CRSend = ts.CRSend;
	cv.KanjiCodeEcho = ts.KanjiCode;
	cv.JIS7KatakanaEcho = ts.JIS7Katakana;
	cv.KanjiCodeSend = ts.KanjiCodeSend;
	cv.JIS7KatakanaSend = ts.JIS7KatakanaSend;
	cv.KanjiIn = ts.KanjiIn;
	cv.KanjiOut = ts.KanjiOut;
}

void ResetKeypadMode(BOOL DisabledModeOnly)
{
	if (!DisabledModeOnly || ts.DisableAppKeypad)
		AppliKeyMode = FALSE;
	if (!DisabledModeOnly || ts.DisableAppCursor)
		AppliCursorMode = FALSE;
}

void MoveToMainScreen()
{
	StatusX = CursorX;
	StatusWrap = Wrap;
	StatusCursor = IsCaretEnabled();

	CursorTop = MainTop;
	CursorBottom = MainBottom;
	Wrap = MainWrap;
	DispEnableCaret(MainCursor);
	MoveCursor(MainX, MainY); // move to main screen
}

/**
 *	1キャラクタ(unsigned int, char32_t)をmacroへ出力
 */
static void DDEPut1U32(unsigned int u32)
{
	if (DDELog) {
		// UTF-8 で出力する
		char u8_buf[4];
		size_t u8_len = UTF32ToUTF8(u32, u8_buf, _countof(u8_buf));
		size_t i;
		for (i = 0; i < u8_len; i++) {
			BYTE b = u8_buf[i];
			DDEPut1(b);
		}
	}
}

/**
 *	ログへ設定された改行コードを出力
 */
static void OutputLogNewLine(vtterm_work_t *vtterm)
{
	switch(vtterm->log_cr_type) {
	case 0:
		// CR + LF
		FLogPutUTF32(CR);
		FLogPutUTF32(LF);
		break;
	case 1:
		// CR
		FLogPutUTF32(CR);
		break;
	case 2:
		// LF
		FLogPutUTF32(LF);
		break;
	}
}

/**
 *	1キャラクタ(unsigned int, char32_t)をログ(or/and macro送信バッファ)へ出力
 *		出力先
 *			ログファイル
 *			macro送信バッファ(DDEPut1())
 *			プリント用
 */
static void OutputLogUTF32(unsigned int u32)
{
	vtterm_work_t *vtterm;
	CheckEOLRet r;

	if (!FLogIsOpendText() && !DDELog && !PrinterMode) {
		return;
	}
	vtterm = &vtterm_work;
	r = CheckEOLCheck(vtterm->check_eol, u32);

	// ログ
	if (FLogIsOpendText()) {
		if ((r & CheckEOLOutputEOL) != 0) {
			// 改行を出力
			OutputLogNewLine(vtterm);
		}

		if ((r & CheckEOLOutputChar) != 0) {
			// u32を出力
			FLogPutUTF32(u32);
		}
	}

	// マクロ出力
	if (DDELog) {
		if ((r & CheckEOLOutputEOL) != 0) {
			// 改行を出力
			DDEPut1(CR);
			DDEPut1(LF);
		}

		// u32を出力
		if ((r & CheckEOLOutputChar) != 0) {
			DDEPut1U32(u32);
		}
	}

	// プリント
	if (PrinterMode) {
		if ((r & CheckEOLOutputEOL) != 0) {
			// 改行を出力
			WriteToPrnFile(PrintFile_, CR,TRUE);
			WriteToPrnFile(PrintFile_, LF,TRUE);
		}

		// u32を出力
		if ((r & CheckEOLOutputChar) != 0) {
			WriteToPrnFileUTF32(PrintFile_, u32, TRUE);
		}
	}
}

/**
 *	1キャラクタ(BYTE)をログ(or/and macro送信バッファ)へ出力
 */
static void OutputLogByte(BYTE b)
{
	OutputLogUTF32(b);
}

/**
 *	ログ(or/and Macro送信バッファ)出力の必要があるか?
 */
static BOOL NeedsOutputBufs(void)
{
	return FLogIsOpendText() || DDELog;
}

void MoveToStatusLine()
{
	MainX = CursorX;
	MainY = CursorY;
	MainTop = CursorTop;
	MainBottom = CursorBottom;
	MainWrap = Wrap;
	MainCursor = IsCaretEnabled();

	DispEnableCaret(StatusCursor);
	MoveCursor(StatusX, NumOfLines-1); // move to status line
	CursorTop = NumOfLines-1;
	CursorBottom = CursorTop;
	Wrap = StatusWrap;
}

void HideStatusLine()
{
	if (isCursorOnStatusLine)
		MoveToMainScreen();
	StatusX = 0;
	StatusWrap = FALSE;
	StatusCursor = TRUE;
	ShowStatusLine(0); //hide
}

void ChangeTerminalSize(int Nx, int Ny)
{
	BuffChangeTerminalSize(Nx, Ny);
	StatusX = 0;
	MainX = 0;
	MainY = 0;
	MainTop = 0;
	MainBottom = NumOfLines-StatusLine-1;
}

void SendCSIstr(char *str, int len) {
	size_t l;

	if (str == NULL || len < 0)
		return;

	if (len == 0) {
		l = strlen(str);
	}
	else {
		l = len;
	}

	if (Send8BitMode)
		CommBinaryOut(&cv,"\233", 1);
	else
		CommBinaryOut(&cv,"\033[", 2);

	CommBinaryOut(&cv, str, l);
}

static void SendOSCstr(char *str, int len, char TermChar) {
	size_t l;

	if (str == NULL || len < 0)
		return;

	if (len == 0) {
		l = strlen(str);
	}
	else {
		l = len;
	}

	if (TermChar == BEL) {
		CommBinaryOut(&cv,"\033]", 2);
		CommBinaryOut(&cv, str, l);
		CommBinaryOut(&cv,"\007", 1);
	}
	else if (Send8BitMode) {
		CommBinaryOut(&cv,"\235", 1);
		CommBinaryOut(&cv, str, l);
		CommBinaryOut(&cv,"\234", 1);
	}
	else {
		CommBinaryOut(&cv,"\033]", 2);
		CommBinaryOut(&cv, str, l);
		CommBinaryOut(&cv,"\033\\", 2);
	}

}

void SendDCSstr(char *str, int len) {
	size_t l;

	if (str == NULL || len < 0)
		return;

	if (len == 0) {
		l = strlen(str);
	}
	else {
		l = len;
	}

	if (Send8BitMode) {
		CommBinaryOut(&cv,"\220", 1);
		CommBinaryOut(&cv, str, l);
		CommBinaryOut(&cv,"\234", 1);
	}
	else {
		CommBinaryOut(&cv,"\033P", 2);
		CommBinaryOut(&cv, str, l);
		CommBinaryOut(&cv,"\033\\", 2);
	}

}

void BackSpace()
{
	if (CursorX == CursorLeftM || CursorX == 0) {
		if (CursorY > 0 && (ts.TermFlag & TF_BACKWRAP)) {
			MoveCursor(CursorRightM, CursorY-1);
			if (NeedsOutputBufs() && !ts.LogTypePlainText) OutputLogByte(BS);
		}
	}
	else if (CursorX > 0) {
		MoveCursor(CursorX-1, CursorY);
		if (NeedsOutputBufs() && !ts.LogTypePlainText) OutputLogByte(BS);
	}
}

static void CarriageReturn(BOOL logFlag)
{
	if (!ts.EnableContinuedLineCopy || logFlag)
		if (NeedsOutputBufs()) OutputLogByte(CR);

	if (RelativeOrgMode || CursorX > CursorLeftM)
		MoveCursor(CursorLeftM, CursorY);
	else if (CursorX < CursorLeftM)
		MoveCursor(0, CursorY);

	Fallbacked = FALSE;
}

static void LineFeed(BYTE b, BOOL logFlag)
{
	/* for auto print mode */
	if ((AutoPrintMode) &&
		(b>=LF) && (b<=FF))
		BuffDumpCurrentLine(PrintFile_, b);

	if (!ts.EnableContinuedLineCopy || logFlag)
		if (NeedsOutputBufs()) OutputLogByte(LF);

	if (CursorY < CursorBottom)
		MoveCursor(CursorX,CursorY+1);
	else if (CursorY == CursorBottom) BuffScrollNLines(1);
	else if (CursorY < NumOfLines-StatusLine-1)
		MoveCursor(CursorX,CursorY+1);

	ClearLineContinued();

	if (LFMode) CarriageReturn(logFlag);

	Fallbacked = FALSE;
}

void Tab()
{
	if (Wrap && !ts.VTCompatTab) {
		CarriageReturn(FALSE);
		LineFeed(LF,FALSE);
		if (ts.EnableContinuedLineCopy) {
			SetLineContinued();
		}
		Wrap = FALSE;
	}
	CursorForwardTab(1, AutoWrapMode);
	if (NeedsOutputBufs()) OutputLogByte(HT);
}

void RepeatChar(BYTE b, int count)
{
	int i;
	BOOL SpecialNew;
	TCharAttr CharAttrTmp, CharAttrWrap;

	if (b <= US || b == DEL)
		return;

	CharAttrTmp = CharAttr;
	LastPutCharacter = 0;

	SpecialNew = FALSE;
	if ((b>0x5F) && (b<0x80)) {
		if (SSflag)
			SpecialNew = (Gn[GLtmp]==IdSpecial);
		else
			SpecialNew = (Gn[Glr[0]]==IdSpecial);
	}
	else if (b>0xDF) {
		if (SSflag)
			SpecialNew = (Gn[GLtmp]==IdSpecial);
		else
			SpecialNew = (Gn[Glr[1]]==IdSpecial);
	}

	if (SpecialNew != Special) {
		UpdateStr();
		Special = SpecialNew;
	}

	if (Special) {
		b = b & 0x7F;
		CharAttrTmp.Attr |= AttrSpecial;
	}
	else
		CharAttrTmp.Attr |= CharAttr.Attr;
	CharAttrTmp.AttrEx = CharAttrTmp.Attr;

	CharAttrWrap = CharAttrTmp;
	CharAttrWrap.Attr |= ts.EnableContinuedLineCopy ? AttrLineContinued : 0;

	for (i=0; i<count; i++) {
		if (Wrap) {
			CarriageReturn(FALSE);
			LineFeed(LF,FALSE);
		}

		BuffPutChar(b, Wrap ? CharAttrWrap : CharAttrTmp, InsertMode);

		if (CursorX == CursorRightM || CursorX >= NumOfColumns-1) {
			UpdateStr();
			Wrap = AutoWrapMode;
		}
		else {
			Wrap = FALSE;
			MoveRight();
		}
	}
}

static void PutChar(BYTE b)
{
	BOOL SpecialNew;
	TCharAttr CharAttrTmp;

	CharAttrTmp = CharAttr;

	LastPutCharacter = b;

	if (PrinterMode) { // printer mode
		WriteToPrnFile(PrintFile_, b,TRUE);
		return;
	}

	if (Wrap) {
		TCharAttr t = BuffGetCursorCharAttr(CursorX, CursorY);
		t.Attr |= AttrLineContinued;
		t.AttrEx = t.Attr;
		BuffSetCursorCharAttr(CursorX, CursorY, t);
		CarriageReturn(FALSE);
		LineFeed(LF,FALSE);
		CharAttrTmp.Attr |= AttrLineContinued;
		t.AttrEx = t.Attr;
	}

	if (NeedsOutputBufs()) {
		// (2005.2.20 yutaka)
		if (ts.LogTypePlainText) {
			if (__isascii(b) && !isprint(b)) {
				// ASCII文字で、非表示な文字はログ採取しない。
			} else {
				OutputLogByte(b);
			}
		} else {
			OutputLogByte(b);
		}
	}

	Wrap = FALSE;

	SpecialNew = FALSE;
	if ((b>0x5F) && (b<0x80)) {
		if (SSflag)
			SpecialNew = (Gn[GLtmp]==IdSpecial);
		else
			SpecialNew = (Gn[Glr[0]]==IdSpecial);
	}
	else if (b>0xDF) {
		if (SSflag)
			SpecialNew = (Gn[GLtmp]==IdSpecial);
		else
			SpecialNew = (Gn[Glr[1]]==IdSpecial);
	}

	if (SpecialNew != Special) {
		UpdateStr();
		Special = SpecialNew;
	}

	if (Special) {
		b = b & 0x7F;
		CharAttrTmp.Attr |= AttrSpecial;
	}
	else
		CharAttrTmp.Attr |= CharAttr.Attr;

#if 0
	if (CursorX == CursorRightM || CursorX >= NumOfColumns - 1) {
		CharAttrTmp.Attr |= AttrLineContinued;
	}
#endif

	CharAttrTmp.AttrEx = CharAttrTmp.Attr;
	if (ts.Language == IdJapanese) {
		unsigned long u32;
		switch (ts.KanjiCode) {
//		case IdJIS:
//			b = JIS2SJIS(b);
		case IdSJIS:
			u32 = MBCP_UTF32(b, 932);
			BuffPutUnicode(u32, CharAttrTmp, InsertMode);
			break;
		case IdUTF8:
			BuffPutUnicode(b, CharAttrTmp, InsertMode);
			break;
		default:
			BuffPutUnicode(b, CharAttrTmp, InsertMode);
			break;
		}
	} else if (ts.Language == IdRussian) {
		BYTE c = RussConv(ts.KanjiCode, IdWindows, b);
		unsigned long u32 = MBCP_UTF32(c, 1251);
		BuffPutUnicode(u32, CharAttrTmp, InsertMode);
	} else {
		BuffPutUnicode(b, CharAttrTmp, InsertMode);
	}

	if (CursorX == CursorRightM || CursorX >= NumOfColumns-1) {
		UpdateStr();
		Wrap = AutoWrapMode;
	}
	else {
		MoveRight();
	}
}

static void PutDecSp(BYTE b)
{
	TCharAttr CharAttrTmp;

	CharAttrTmp = CharAttr;

	if (PrinterMode) { // printer mode
		WriteToPrnFile(PrintFile_, b, TRUE);
		return;
	}

	if (Wrap) {
		CarriageReturn(FALSE);
		LineFeed(LF, FALSE);
		CharAttrTmp.Attr |= ts.EnableContinuedLineCopy ? AttrLineContinued : 0;
	}

	if (NeedsOutputBufs()) OutputLogByte(b);
/*
	if (ts.LogTypePlainText && __isascii(b) && !isprint(b)) {
		// ASCII文字で、非表示な文字はログ採取しない。
	} else {
		if (NeedsOutputBufs()) OutputLogByte(b);
	}
 */

	Wrap = FALSE;

	if (!Special) {
		UpdateStr();
		Special = TRUE;
	}

	CharAttrTmp.Attr |= AttrSpecial;
	CharAttrTmp.AttrEx = CharAttrTmp.Attr;
	BuffPutChar(b, CharAttrTmp, InsertMode);

	if (CursorX == CursorRightM || CursorX >= NumOfColumns-1) {
		UpdateStr();
		Wrap = AutoWrapMode;
	}
	else {
		MoveRight();
	}
}

/**
 *	mbcsを出力
 */
static void PutKanji(BYTE b)
{
	int LineEnd;
	TCharAttr CharAttrTmp;
	CharAttrTmp = CharAttr;

	Kanji = Kanji + b;

	if (PrinterMode && DirectPrn) {
		WriteToPrnFile(PrintFile_, HIBYTE(Kanji),FALSE);
		WriteToPrnFile(PrintFile_, LOBYTE(Kanji),TRUE);
		return;
	}

	if (ConvJIS)
		Kanji = JIS2SJIS((WORD)(Kanji & 0x7f7f));

	if (PrinterMode) { // printer mode
		WriteToPrnFile(PrintFile_, HIBYTE(Kanji),FALSE);
		WriteToPrnFile(PrintFile_, LOBYTE(Kanji),TRUE);
		return;
	}

	if (CursorX > CursorRightM)
		LineEnd = NumOfColumns - 1;
	else
		LineEnd = CursorRightM;

	if (Wrap) {
		CarriageReturn(FALSE);
		LineFeed(LF,FALSE);
		if (ts.EnableContinuedLineCopy) {
			CharAttrTmp.Attr |= AttrLineContinued;
			CharAttrTmp.AttrEx = CharAttrTmp.Attr;
		}
	}
	else if (CursorX > LineEnd - 1) {
		if (AutoWrapMode) {
			if (ts.EnableContinuedLineCopy) {
				CharAttrTmp.Attr |= AttrLineContinued;
				CharAttrTmp.AttrEx = CharAttrTmp.Attr;
				if (CursorX == LineEnd)
					BuffPutChar(0x20, CharAttr, FALSE);
			}
			CarriageReturn(FALSE);
			LineFeed(LF,FALSE);
		}
		else {
			return;
		}
	}

	Wrap = FALSE;

	if (NeedsOutputBufs()) {
		OutputLogByte(HIBYTE(Kanji));
		OutputLogByte(LOBYTE(Kanji));
	}

	if (Special) {
		UpdateStr();
		Special = FALSE;
	}

	{
		// codepage一覧
		// https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-ucoderef/28fefe92-d66c-4b03-90a9-97b473223d43
		unsigned long u32 = 0;
		switch (ts.Language) {
		case IdJapanese:
			// ここに来た時点でCP932になっている
			u32 = CP932ToUTF32(Kanji);
			break;
		case IdKorean:
			if (ts.KanjiCode == IdKoreanCP51949) {
				// CP51949
				u32 = MBCP_UTF32(Kanji, 51949);
			}
			else {
				assert(FALSE);
			}
			break;
		case IdChinese:
			if (ts.KanjiCode == IdCnGB2312) {
				// CP936 GB2312
				u32 = MBCP_UTF32(Kanji, 936);
			}
			else if (ts.KanjiCode == IdCnBig5) {
				// CP950 Big5
				u32 = MBCP_UTF32(Kanji, 950);
			}
			else {
				assert(FALSE);
			}
			break;
		default:
			assert(FALSE);
			break;
		}
		CharAttrTmp.AttrEx = CharAttrTmp.Attr;
		BuffPutUnicode(u32, CharAttrTmp, InsertMode);
	}

	if (CursorX < LineEnd - 1) {
		MoveRight();
		MoveRight();
	}
	else {
		if (CursorX == LineEnd - 1) {
			MoveRight();
		}
		UpdateStr();
		Wrap = AutoWrapMode;
	}
}

void PutDebugChar(BYTE b)
{
	static BYTE buff[3];
	int i;
	BOOL svInsertMode, svAutoWrapMode;
	BYTE svCharAttr;

	if (DebugFlag!=DEBUG_FLAG_NOUT) {
		svInsertMode = InsertMode;
		svAutoWrapMode = AutoWrapMode;
		InsertMode = FALSE;
		AutoWrapMode = TRUE;

		svCharAttr = CharAttr.Attr;
		if (CharAttr.Attr != AttrDefault) {
			UpdateStr();
			CharAttr.Attr = AttrDefault;
		}

		if (DebugFlag==DEBUG_FLAG_HEXD) {
			_snprintf(buff, 3, "%02X", (unsigned int) b);

			for (i=0; i<2; i++)
				PutChar(buff[i]);
			PutChar(' ');
		}
		else if (DebugFlag==DEBUG_FLAG_NORM) {

			if ((b & 0x80) == 0x80) {
				UpdateStr();
				CharAttr.Attr = AttrReverse;
				b = b & 0x7f;
			}

			if (b<=US) {
				PutChar('^');
				PutChar((char)(b+0x40));
			}
			else if (b==DEL) {
				PutChar('<');
				PutChar('D');
				PutChar('E');
				PutChar('L');
				PutChar('>');
			}
			else
				PutChar(b);
		}

		if (CharAttr.Attr != svCharAttr) {
			UpdateStr();
			CharAttr.Attr = svCharAttr;
		}
		InsertMode = svInsertMode;
		AutoWrapMode = svAutoWrapMode;
	}
}

static void PrnParseControl(BYTE b) // printer mode
{
	switch (b) {
	case NUL:
		return;
	case SO:
		if ((ts.ISO2022Flag & ISO2022_SO) && ! DirectPrn) {
			if ((ts.Language==IdJapanese) &&
			    (ts.KanjiCode==IdJIS) &&
			    (ts.JIS7Katakana==1) &&
			    ((ts.TermFlag & TF_FIXEDJIS)!=0))
			{
				Gn[1] = IdKatakana;
			}
			Glr[0] = 1; /* LS1 */
			return;
		}
		break;
	case SI:
		if ((ts.ISO2022Flag & ISO2022_SI) && ! DirectPrn) {
			Glr[0] = 0; /* LS0 */
			return;
		}
		break;
	case DC1:
	case DC3:
		return;
	case ESC:
		ICount = 0;
		JustAfterESC = TRUE;
		ParseMode = ModeESC;
		WriteToPrnFile(PrintFile_, 0, TRUE); // flush prn buff
		return;
	case CSI:
		if (! Accept8BitCtrl) {
			PutChar(b); /* Disp C1 char in VT100 mode */
			return;
		}
		ClearParams();
		FirstPrm = TRUE;
		ParseMode = ModeCSI;
		WriteToPrnFile(PrintFile_, 0, TRUE); // flush prn buff
		WriteToPrnFile(PrintFile_, b, FALSE);
		return;
	}
	/* send the uninterpreted character to printer */
	WriteToPrnFile(PrintFile_, b, TRUE);
}

static void ParseControl(BYTE b)
{
	if (PrinterMode) { // printer mode
		PrnParseControl(b);
		return;
	}

	if (b>=0x80) { /* C1 char */
		if (ts.Language==IdEnglish) { /* English mode */
			if (!Accept8BitCtrl) {
				PutChar(b); /* Disp C1 char in VT100 mode */
				return;
			}
		}
		else { /* Japanese mode */
			if ((ts.TermFlag & TF_ACCEPT8BITCTRL)==0) {
				return; /* ignore C1 char */
			}
			/* C1 chars are interpreted as C0 chars in VT100 mode */
			if (VTlevel < 2) {
				b = b & 0x7F;
			}
		}
	}
	switch (b) {
	/* C0 group */
	case ENQ:
		CommBinaryOut(&cv, &(ts.Answerback[0]), ts.AnswerbackLen);
		break;
	case BEL:
		if (ts.Beep != IdBeepOff)
			RingBell(ts.Beep);
		break;
	case BS:
		BackSpace();
		break;
	case HT:
		Tab();
		break;
	case LF:
		if (ts.CRReceive == IdLF) {
			// 受信時の改行コードが LF の場合は、サーバから LF のみが送られてくると仮定し、
			// CR+LFとして扱うようにする。
			// cf. http://www.neocom.ca/forum/viewtopic.php?t=216
			// (2007.1.21 yutaka)
			CarriageReturn(TRUE);
			LineFeed(b, TRUE);
			break;
		}
		else if (ts.CRReceive == IdAUTO) {
			// 9th Apr 2012: AUTO CR/LF mode (tentner)
			// a CR or LF will generated a CR+LF, if the next character is the opposite, it will be ignored
			if(PrevCharacter != CR || !PrevCRorLFGeneratedCRLF) {
				CarriageReturn(TRUE);
				LineFeed(b, TRUE);
				PrevCRorLFGeneratedCRLF = TRUE;
			}
			else {
				PrevCRorLFGeneratedCRLF = FALSE;
			}
			break;
		}

	case VT:
		LineFeed(b, TRUE);
		break;

	case FF:
		if ((ts.AutoWinSwitch>0) && JustAfterESC) {
			CommInsert1Byte(&cv, b);
			CommInsert1Byte(&cv, ESC);
			ChangeEmu = IdTEK;	/* Enter TEK Mode */
		}
		else
			LineFeed(b, TRUE);
		break;
	case CR:
		if (ts.CRReceive == IdAUTO) {
			// 9th Apr 2012: AUTO CR/LF mode (tentner)
			// a CR or LF will generated a CR+LF, if the next character is the opposite, it will be ignored
			if(PrevCharacter != LF || !PrevCRorLFGeneratedCRLF) {
				CarriageReturn(TRUE);
				LineFeed(b, TRUE);
				PrevCRorLFGeneratedCRLF = TRUE;
			}
			else {
				PrevCRorLFGeneratedCRLF = FALSE;
			}
		}
		else {
			CarriageReturn(TRUE);
			if (ts.CRReceive==IdCRLF) {
				CommInsert1Byte(&cv, LF);
			}
		}
		break;
	case SO: /* LS1 */
		if (ts.ISO2022Flag & ISO2022_SO) {
			if ((ts.Language==IdJapanese) &&
			    (ts.KanjiCode==IdJIS) &&
			    (ts.JIS7Katakana==1) &&
			    ((ts.TermFlag & TF_FIXEDJIS)!=0))
			{
				Gn[1] = IdKatakana;
			}

			Glr[0] = 1;
		}
		break;
	case SI: /* LS0 */
		if (ts.ISO2022Flag & ISO2022_SI) {
			Glr[0] = 0;
		}
		break;
	case DLE:
		if ((ts.FTFlag & FT_BPAUTO)!=0)
			ParseMode = ModeDLE; /* Auto B-Plus activation */
		break;
	case CAN:
		if ((ts.FTFlag & FT_ZAUTO)!=0)
			ParseMode = ModeCAN; /* Auto ZMODEM activation */
//		else if (ts.AutoWinSwitch>0)
//			ChangeEmu = IdTEK;	/* Enter TEK Mode */
		else
			ParseMode = ModeFirst;
		break;
	case SUB:
		ParseMode = ModeFirst;
		break;
	case ESC:
		ICount = 0;
		JustAfterESC = TRUE;
		ParseMode = ModeESC;
		break;
	case FS:
	case GS:
	case RS:
	case US:
		if (ts.AutoWinSwitch>0) {
			CommInsert1Byte(&cv, b);
			ChangeEmu = IdTEK;	/* Enter TEK Mode */
		}
		break;

	/* C1 char */
	case IND:
		LineFeed(0, TRUE);
		break;
	case NEL:
		LineFeed(0, TRUE);
		CarriageReturn(TRUE);
		break;
	case HTS:
		if (ts.TabStopFlag & TABF_HTS8)
			SetTabStop();
		break;
	case RI:
		CursorUpWithScroll();
		break;
	case SS2:
		if (ts.ISO2022Flag & ISO2022_SS2) {
			GLtmp = 2;
			SSflag = TRUE;
		}
		break;
	case SS3:
		if (ts.ISO2022Flag & ISO2022_SS3) {
			GLtmp = 3;
			SSflag = TRUE;
		}
		break;
	case DCS:
		ClearParams();
		ESCFlag = FALSE;
		ParseMode = ModeDCS;
		break;
	case SOS:
		ESCFlag = FALSE;
		ParseMode = ModeIgnore;
		break;
	case CSI:
		ClearParams();
		FirstPrm = TRUE;
		ParseMode = ModeCSI;
		break;
	case OSC:
		ClearParams();
		ParseMode = ModeXS;
		break;
	case PM:
	case APC:
		ESCFlag = FALSE;
		ParseMode = ModeIgnore;
		break;
	}
}

void SaveCursor()
{
	int i;
	PStatusBuff Buff;

	if (isCursorOnStatusLine)
		Buff = &SBuff2; // for status line
	else if (AltScr)
		Buff = &SBuff3; // for alternate screen
	else
		Buff = &SBuff1; // for main screen

	Buff->CursorX = CursorX;
	Buff->CursorY = CursorY;
	Buff->Attr = CharAttr;

	Buff->Glr[0] = Glr[0];
	Buff->Glr[1] = Glr[1];
	for (i=0 ; i<=3; i++)
		Buff->Gn[i] = Gn[i];

	Buff->AutoWrapMode = AutoWrapMode;
	Buff->RelativeOrgMode = RelativeOrgMode;
}

void RestoreCursor()
{
	int i;
	PStatusBuff Buff;

	UpdateStr();

	if (isCursorOnStatusLine)
		Buff = &SBuff2; // for status line
	else if (AltScr)
		Buff = &SBuff3; // for alternate screen
	else
		Buff = &SBuff1; // for main screen

	if (Buff->CursorX > NumOfColumns-1)
		Buff->CursorX = NumOfColumns-1;
	if (Buff->CursorY > NumOfLines-1-StatusLine)
		Buff->CursorY = NumOfLines-1-StatusLine;
	MoveCursor(Buff->CursorX, Buff->CursorY);

	CharAttr = Buff->Attr;
	BuffSetCurCharAttr(CharAttr);

	Glr[0] = Buff->Glr[0];
	Glr[1] = Buff->Glr[1];
	for (i=0 ; i<=3; i++)
		Gn[i] = Buff->Gn[i];

	AutoWrapMode = Buff->AutoWrapMode;
	RelativeOrgMode = Buff->RelativeOrgMode;
}

static void AnswerTerminalType(void)
{
	char Tmp[50];

	if (ts.TerminalID<IdVT320 || !Send8BitMode)
		strncpy_s(Tmp, sizeof(Tmp),"\033[?", _TRUNCATE);
	else
		strncpy_s(Tmp, sizeof(Tmp),"\233?", _TRUNCATE);

	switch (ts.TerminalID) {
	case IdVT100:
		strncat_s(Tmp,sizeof(Tmp),"1;2",_TRUNCATE);
		break;
	case IdVT100J:
		strncat_s(Tmp,sizeof(Tmp),"5;2",_TRUNCATE);
		break;
	case IdVT101:
		strncat_s(Tmp,sizeof(Tmp),"1;0",_TRUNCATE);
		break;
	case IdVT102:
		strncat_s(Tmp,sizeof(Tmp),"6",_TRUNCATE);
		break;
	case IdVT102J:
		strncat_s(Tmp,sizeof(Tmp),"15",_TRUNCATE);
		break;
	case IdVT220J:
		strncat_s(Tmp,sizeof(Tmp),"62;1;2;5;6;7;8",_TRUNCATE);
		break;
	case IdVT282:
		strncat_s(Tmp,sizeof(Tmp),"62;1;2;4;5;6;7;8;10;11",_TRUNCATE);
		break;
	case IdVT320:
		strncat_s(Tmp,sizeof(Tmp),"63;1;2;6;7;8",_TRUNCATE);
		break;
	case IdVT382:
		strncat_s(Tmp,sizeof(Tmp),"63;1;2;4;5;6;7;8;10;15",_TRUNCATE);
		break;
	case IdVT420:
		strncat_s(Tmp,sizeof(Tmp),"64;1;2;7;8;9;15;18;21",_TRUNCATE);
		break;
	case IdVT520:
		strncat_s(Tmp,sizeof(Tmp),"65;1;2;7;8;9;12;18;19;21;23;24;42;44;45;46",_TRUNCATE);
		break;
	case IdVT525:
		strncat_s(Tmp,sizeof(Tmp),"65;1;2;7;9;12;18;19;21;22;23;24;42;44;45;46",_TRUNCATE);
		break;
	}
	strncat_s(Tmp,sizeof(Tmp),"c",_TRUNCATE);

	CommBinaryOut(&cv,Tmp,strlen(Tmp)); /* Report terminal ID */
}

static void ESCSpace(BYTE b)
{
	switch (b) {
	case 'F':  // S7C1T
		Send8BitMode = FALSE;
		break;
	case 'G':  // S8C1T
		if (VTlevel >= 2) {
			Send8BitMode = TRUE;
		}
		break;
	}
}

static void ESCSharp(BYTE b)
{
	switch (b) {
	case '8':  /* Fill screen with "E" (DECALN) */
		BuffUpdateScroll();
		BuffFillWithE();
		CursorTop = 0;
		CursorBottom = NumOfLines-1-StatusLine;
		CursorLeftM = 0;
		CursorRightM = NumOfColumns - 1;
		MoveCursor(0, 0);
		ParseMode = ModeFirst;
		break;
	}
}

/* select double byte code set */
static void ESCDBCSSelect(BYTE b)
{
	int Dist;

	if (ts.Language!=IdJapanese) return;

	switch (ICount) {
		case 1:
			if ((b=='@') || (b=='B'))
			{
				Gn[0] = IdKanji; /* Kanji -> G0 */
				if ((ts.TermFlag & TF_AUTOINVOKE)!=0)
					Glr[0] = 0; /* G0->GL */
			}
			break;
		case 2:
			/* Second intermediate char must be
				 '(' or ')' or '*' or '+'. */
			Dist = (IntChar[2]-'(') & 3; /* G0 - G3 */
			if ((b=='1') || (b=='3') ||
					(b=='@') || (b=='B'))
			{
				Gn[Dist] = IdKanji; /* Kanji -> G0-3 */
				if (((ts.TermFlag & TF_AUTOINVOKE)!=0) &&
						(Dist==0))
					Glr[0] = 0; /* G0->GL */
			}
			break;
	}
}

static void ESCSelectCode(BYTE b)
{
	switch (b) {
		case '0':
			if (ts.AutoWinSwitch>0)
				ChangeEmu = IdTEK; /* enter TEK mode */
			break;
	}
}

	/* select single byte code set */
static void ESCSBCSSelect(BYTE b)
{
	int Dist;

	/* Intermediate char must be '(' or ')' or '*' or '+'. */
	Dist = (IntChar[1]-'(') & 3; /* G0 - G3 */

	switch (b) {
	case '0': Gn[Dist] = IdSpecial; break;
	case '<': Gn[Dist] = IdASCII; break;
	case '>': Gn[Dist] = IdASCII; break;
	case 'A': Gn[Dist] = IdASCII; break;
	case 'B': Gn[Dist] = IdASCII; break;
	case 'H': Gn[Dist] = IdASCII; break;
	case 'I':
		if (ts.Language==IdJapanese)
			Gn[Dist] = IdKatakana;
		break;
	case 'J': Gn[Dist] = IdASCII; break;
	}

	if (((ts.TermFlag & TF_AUTOINVOKE)!=0) && (Dist==0))
		Glr[0] = 0;  /* G0->GL */
}

void PrnParseEscape(BYTE b) // printer mode
{
	int i;

	ParseMode = ModeFirst;
	switch (ICount) {
	/* no intermediate char */
	case 0:
		switch (b) {
		case '[': /* CSI */
			ClearParams();
			FirstPrm = TRUE;
			WriteToPrnFile(PrintFile_, ESC,FALSE);
			WriteToPrnFile(PrintFile_, '[',FALSE);
			ParseMode = ModeCSI;
			return;
		} /* end of case Icount=0 */
		break;
	/* one intermediate char */
	case 1:
		switch (IntChar[1]) {
		case '$':
			if (! DirectPrn) {
				ESCDBCSSelect(b);
				return;
			}
			break;
		case '(':
		case ')':
		case '*':
		case '+':
			if (! DirectPrn) {
				ESCSBCSSelect(b);
				return;
			}
			break;
		}
		break;
	/* two intermediate char */
	case 2:
		if ((! DirectPrn) &&
		    (IntChar[1]=='$') &&
		    ('('<=IntChar[2]) &&
		    (IntChar[2]<='+'))
		{
			ESCDBCSSelect(b);
			return;
		}
		break;
	}
	// send the uninterpreted sequence to printer
	WriteToPrnFile(PrintFile_, ESC,FALSE);
	for (i=1; i<=ICount; i++)
		WriteToPrnFile(PrintFile_, IntChar[i],FALSE);
	WriteToPrnFile(PrintFile_, b,TRUE);
}

void ParseEscape(BYTE b) /* b is the final char */
{
	if (PrinterMode) { // printer mode
		PrnParseEscape(b);
		return;
	}

	switch (ICount) {
	case 0: /* no intermediate char */
		switch (b) {
		case '6': // DECBI
			if (CursorY >= CursorTop && CursorY <= CursorBottom &&
			    CursorX >= CursorLeftM && CursorX <= CursorRightM) {
				if (CursorX == CursorLeftM)
					BuffScrollRight(1);
				else
					MoveCursor(CursorX-1, CursorY);
			}
		break;
		case '7': SaveCursor(); break;
		case '8': RestoreCursor(); break;
		case '9': // DECFI
			if (CursorY >= CursorTop && CursorY <= CursorBottom &&
			    CursorX >= CursorLeftM && CursorX <= CursorRightM) {
				if (CursorX == CursorRightM)
					BuffScrollLeft(1);
				else
					MoveCursor(CursorX+1, CursorY);
			}
			break;
		case '=': AppliKeyMode = TRUE; break;
		case '>': AppliKeyMode = FALSE; break;
		case 'D': /* IND */
			LineFeed(0,TRUE);
			break;
		case 'E': /* NEL */
			MoveCursor(0,CursorY);
			LineFeed(0,TRUE);
			break;
		case 'H': /* HTS */
			if (ts.TabStopFlag & TABF_HTS7)
				SetTabStop();
			break;
		case 'M': /* RI */
			CursorUpWithScroll();
			break;
		case 'N': /* SS2 */
			if (ts.ISO2022Flag & ISO2022_SS2) {
				GLtmp = 2;
				SSflag = TRUE;
			}
			break;
		case 'O': /* SS3 */
			if (ts.ISO2022Flag & ISO2022_SS3) {
				GLtmp = 3;
				SSflag = TRUE;
			}
			break;
		case 'P': /* DCS */
			ClearParams();
			ESCFlag = FALSE;
			ParseMode = ModeDCS;
			return;
		case 'X': /* SOS */
		case '^': /* APC */
		case '_': /* PM  */
			ESCFlag = FALSE;
			ParseMode = ModeIgnore;
			return;
		case 'Z': /* DECID */
			AnswerTerminalType();
			break;
		case '[': /* CSI */
			ClearParams();
			FirstPrm = TRUE;
			ParseMode = ModeCSI;
			return;
		case '\\': break; /* ST */
		case ']': /* XTERM sequence (OSC) */
			ClearParams();
			ParseMode = ModeXS;
			return;
		case 'c': /* Hardware reset */
			HideStatusLine();
			ResetTerminal();
			ClearUserKey();
			ClearBuffer();
			if (ts.PortType==IdSerial) // reset serial port
				CommResetSerial(&ts, &cv, TRUE);
			break;
		case 'g': /* Visual Bell (screen original?) */
			RingBell(IdBeepVisual);
			break;
		case 'n': /* LS2 */
			if (ts.ISO2022Flag & ISO2022_LS2) {
				Glr[0] = 2;
			}
			break;
		case 'o': /* LS3 */
			if (ts.ISO2022Flag & ISO2022_LS3) {
				Glr[0] = 3;
			}
			break;
		case '|': /* LS3R */
			if (ts.ISO2022Flag & ISO2022_LS3R) {
				Glr[1] = 3;
			}
			break;
		case '}': /* LS2R */
			if (ts.ISO2022Flag & ISO2022_LS2R) {
				Glr[1] = 2;
			}
			break;
		case '~': /* LS1R */
			if (ts.ISO2022Flag & ISO2022_LS1R) {
				Glr[1] = 1;
			}
			break;
		}
		break;
		/* end of case Icount=0 */

	case 1: /* one intermediate char */
		switch (IntChar[1]) {
		case ' ': ESCSpace(b); break;
		case '#': ESCSharp(b); break;
		case '$': ESCDBCSSelect(b); break;
		case '%': break;
		case '(':
		case ')':
		case '*':
		case '+':
			ESCSBCSSelect(b);
			break;
		}
		break;

	case 2: /* two intermediate char */
		if ((IntChar[1]=='$') && ('('<=IntChar[2]) && (IntChar[2]<='+'))
			ESCDBCSSelect(b);
		else if ((IntChar[1]=='%') && (IntChar[2]=='!'))
			ESCSelectCode(b);
		break;
	}
	ParseMode = ModeFirst;
}

void EscapeSequence(BYTE b)
{
	if (b<=US)
		ParseControl(b);
	else if ((b>=0x20) && (b<=0x2F)) {
		// TODO: ICount が IntCharMax に達した時、最後の IntChar を置き換えるのは妥当?
		if (ICount<IntCharMax)
			ICount++;
		IntChar[ICount] = b;
	}
	else if ((b>=0x30) && (b<=0x7E))
		ParseEscape(b);
	else if ((b>=0x80) && (b<=0x9F))
		ParseControl(b);
	else if (b>=0xA0) {
		ParseMode=ModeFirst;
		ParseFirst(b);
	}

	JustAfterESC = FALSE;
}

#define CheckParamVal(p,m) \
	if ((p) == 0) { \
		(p) = 1; \
	} \
	else if ((p) > (m) || p < 0) { \
		(p) = (m); \
	}

#define CheckParamValMax(p,m) \
	if ((p) > (m) || p <= 0) { \
		(p) = (m); \
	}

#define RequiredParams(n) \
	if ((n) > 1) { \
		while (NParam < n) { \
			NParam++; \
			Param[NParam] = 0; \
			NSParam[NParam] = 0; \
		} \
	}

// ICH
static void CSInsertCharacter(void)
{
	// Insert space characters at cursor
	CheckParamVal(Param[1], NumOfColumns);

	BuffUpdateScroll();
	BuffInsertSpace(Param[1]);
}

void CSCursorUp(BOOL AffectMargin)	// CUU / VPB
{
	int topMargin, NewY;

	CheckParamVal(Param[1], CursorY);

	if (AffectMargin && CursorY >= CursorTop)
		topMargin = CursorTop;
	else
		topMargin = 0;

	NewY = CursorY - Param[1];
	if (NewY < topMargin)
		NewY = topMargin;

	MoveCursor(CursorX, NewY);
}

void CSCursorUp1()			// CPL
{
	MoveCursor(CursorLeftM, CursorY);
	CSCursorUp(TRUE);
}

void CSCursorDown(BOOL AffectMargin)	// CUD / VPR
{
	int bottomMargin, NewY;

	if (AffectMargin && CursorY <= CursorBottom)
		bottomMargin = CursorBottom;
	else
		bottomMargin = NumOfLines-StatusLine-1;

	CheckParamVal(Param[1], bottomMargin);

	NewY = CursorY + Param[1];
	if (NewY > bottomMargin)
		NewY = bottomMargin;

	MoveCursor(CursorX, NewY);
}

void CSCursorDown1()			// CNL
{
	MoveCursor(CursorLeftM, CursorY);
	CSCursorDown(TRUE);
}

void CSScreenErase()
{
	BuffUpdateScroll();
	switch (Param[1]) {
	  case 0:
		// <ESC>[H(Cursor in left upper corner)によりカーソルが左上隅を指している場合、
		// <ESC>[Jは<ESC>[2Jと同じことなので、処理を分け、現行バッファをスクロールアウト
		// させるようにする。(2005.5.29 yutaka)
		// コンフィグレーションで切り替えられるようにした。(2008.5.3 yutaka)
		if (ts.ScrollWindowClearScreen &&
			(CursorX == 0 && CursorY == 0)) {
			// Erase screen (scroll out)
			BuffClearScreen();
			UpdateWindow(HVTWin);

		} else {
			// Erase characters from cursor to the end of screen
			BuffEraseCurToEnd();
		}
		break;

	  case 1:
		// Erase characters from home to cursor
		BuffEraseHomeToCur();
		break;

	  case 2:
		// Erase screen (scroll out)
		BuffClearScreen();
		UpdateWindow(HVTWin);
		if (ClearThenHome && !isCursorOnStatusLine) {
			if (RelativeOrgMode) {
				MoveCursor(0, 0);
			}
			else {
				MoveCursor(CursorLeftM, CursorTop);
			}
		}
		break;

	  case 3:
		if (ts.TermFlag & TF_REMOTECLEARSBUFF) {
			ClearBuffer();
		}
		break;
	}
}

void CSQSelScreenErase()
{
	BuffUpdateScroll();
	switch (Param[1]) {
	  case 0:
		// Erase characters from cursor to end
		BuffSelectedEraseCurToEnd();
		break;

	  case 1:
		// Erase characters from home to cursor
		BuffSelectedEraseHomeToCur();
		break;

	  case 2:
		// Erase entire screen
		BuffSelectedEraseScreen();
		break;

	  case 3:
		if (ts.TermFlag & TF_REMOTECLEARSBUFF) {
			ClearBuffer();
		}
		break;
	}
}

void CSInsertLine()
{
	// Insert lines at current position
	int Count, YEnd;

	if (CursorY < CursorTop || CursorY > CursorBottom) {
		return;
	}

	CheckParamVal(Param[1], NumOfLines);

	Count = Param[1];

	YEnd = CursorBottom;
	if (CursorY > YEnd)
		YEnd = NumOfLines-1-StatusLine;

	if (Count > YEnd+1 - CursorY)
		Count = YEnd+1 - CursorY;

	BuffInsertLines(Count,YEnd);
}

void CSLineErase()
{
	BuffUpdateScroll();
	switch (Param[1]) {
	  case 0: /* erase char from cursor to end of line */
		BuffEraseCharsInLine(CursorX,NumOfColumns-CursorX);
		break;

	  case 1: /* erase char from start of line to cursor */
		BuffEraseCharsInLine(0,CursorX+1);
		break;

	  case 2: /* erase entire line */
		BuffEraseCharsInLine(0,NumOfColumns);
		break;
	}
}

static void CSQSelLineErase(void)
{
	BuffUpdateScroll();
	switch (Param[1]) {
	  case 0: /* erase char from cursor to end of line */
		BuffSelectedEraseCharsInLine(CursorX,NumOfColumns-CursorX);
		break;

	  case 1: /* erase char from start of line to cursor */
		BuffSelectedEraseCharsInLine(0,CursorX+1);
		break;

	  case 2: /* erase entire line */
		BuffSelectedEraseCharsInLine(0,NumOfColumns);
		break;
	}
}

void CSDeleteNLines()
// Delete lines from current line
{
	int Count, YEnd;

	if (CursorY < CursorTop || CursorY > CursorBottom) {
		return;
	}

	CheckParamVal(Param[1], NumOfLines);
	Count = Param[1];

	YEnd = CursorBottom;
	if (CursorY > YEnd)
		YEnd = NumOfLines-1-StatusLine;

	if (Count > YEnd+1-CursorY)
		Count = YEnd+1-CursorY;

	BuffDeleteLines(Count,YEnd);
}

// DCH
static void CSDeleteCharacter(void)
{
// Delete characters in current line from cursor
	CheckParamVal(Param[1], NumOfColumns);

	BuffUpdateScroll();
	BuffDeleteChars(Param[1]);
}

// ECH
static void CSEraseCharacter(void)
{
	CheckParamVal(Param[1], NumOfColumns);

	BuffUpdateScroll();
	BuffEraseChars(Param[1]);
}

void CSRepeatCharacter()
{
	CheckParamVal(Param[1], NumOfColumns * NumOfLines);

	BuffUpdateScroll();
	RepeatChar(LastPutCharacter, Param[1]);
}

void CSScrollUp()
{
	// TODO: スクロールの最大値を端末行数に制限すべきか要検討
	CheckParamVal(Param[1], INT_MAX);

	BuffUpdateScroll();
	BuffRegionScrollUpNLines(Param[1]);
}

void CSScrollDown()
{
	CheckParamVal(Param[1], NumOfLines);

	BuffUpdateScroll();
	BuffRegionScrollDownNLines(Param[1]);
}

void CSForwardTab()
{
	CheckParamVal(Param[1], NumOfColumns);
	CursorForwardTab(Param[1], AutoWrapMode);
}

void CSBackwardTab()
{
	CheckParamVal(Param[1], NumOfColumns);
	CursorBackwardTab(Param[1]);
}

void CSMoveToColumnN()		// CHA / HPA
{
	CheckParamVal(Param[1], NumOfColumns);

	Param[1]--;

	if (RelativeOrgMode) {
		if (CursorLeftM + Param[1] > CursorRightM )
			MoveCursor(CursorRightM, CursorY);
		else
			MoveCursor(CursorLeftM + Param[1], CursorY);
	}
	else {
		MoveCursor(Param[1], CursorY);
	}
}

void CSCursorRight(BOOL AffectMargin)	// CUF / HPR
{
	int NewX, rightMargin;

	CheckParamVal(Param[1], NumOfColumns);

	if (AffectMargin && CursorX <= CursorRightM) {
		rightMargin = CursorRightM;
	}
	else {
		rightMargin = NumOfColumns-1;
	}

	NewX = CursorX + Param[1];
	if (NewX > rightMargin)
		NewX = rightMargin;

	MoveCursor(NewX, CursorY);
}

void CSCursorLeft(BOOL AffectMargin)	// CUB / HPB
{
	int NewX, leftMargin;

	CheckParamVal(Param[1], NumOfColumns);

	if (AffectMargin && CursorX >= CursorLeftM) {
		leftMargin = CursorLeftM;
	}
	else {
		leftMargin = 0;
	}

	NewX = CursorX  - Param[1];
	if (NewX < leftMargin) {
		NewX = leftMargin;
	}

	MoveCursor(NewX, CursorY);
}

void CSMoveToLineN()		// VPA
{
	CheckParamVal(Param[1], NumOfLines-StatusLine);

	if (RelativeOrgMode) {
		if (CursorTop+Param[1]-1 > CursorBottom)
			MoveCursor(CursorX,CursorBottom);
		else
			MoveCursor(CursorX,CursorTop+Param[1]-1);
	}
	else {
		if (Param[1] > NumOfLines-StatusLine)
			MoveCursor(CursorX,NumOfLines-1-StatusLine);
		else
			MoveCursor(CursorX,Param[1]-1);
	}
	Fallbacked = FALSE;
}

void CSMoveToXY()		// CUP / HVP
{
	int NewX, NewY;

	RequiredParams(2);
	CheckParamVal(Param[1], NumOfLines-StatusLine);
	CheckParamVal(Param[2], NumOfColumns);

	NewY = Param[1] - 1;
	NewX = Param[2] - 1;

	if (isCursorOnStatusLine)
		NewY = CursorY;
	else if (RelativeOrgMode) {
		NewX += CursorLeftM;
		if (NewX > CursorRightM)
			NewX = CursorRightM;

		NewY += CursorTop;
		if (NewY > CursorBottom)
			NewY = CursorBottom;
	}
	else {
		if (NewY > NumOfLines-1-StatusLine)
			NewY = NumOfLines-1-StatusLine;
	}

	MoveCursor(NewX, NewY);
	Fallbacked = FALSE;
}

void CSDeleteTabStop()
{
	ClearTabStop(Param[1]);
}

void CS_h_Mode()		// SM
{
	switch (Param[1]) {
	  case 2:	// KAM
		KeybEnabled = FALSE; break;
	  case 4:	// IRM
		InsertMode = TRUE; break;
	  case 12:	// SRM
		ts.LocalEcho = 0;
		if (cv.Ready && cv.TelFlag && (ts.TelEcho>0))
			TelChangeEcho();
		break;
	  case 20:	// LF/NL
		LFMode = TRUE;
		ts.CRSend = IdCRLF;
		cv.CRSend = IdCRLF;
		break;
	  case 33:	// WYSTCURM
		if (ts.WindowFlag & WF_CURSORCHANGE) {
			ts.NonblinkingCursor = TRUE;
			ChangeCaret();
		}
		break;
	  case 34:	// WYULCURM
		if (ts.WindowFlag & WF_CURSORCHANGE) {
			ts.CursorShape = IdHCur;
			ChangeCaret();
		}
		break;
	}
}

void CS_i_Mode()		// MC
{
	switch (Param[1]) {
	  /* print screen */
	  //  PrintEX -- TRUE: print screen
	  //             FALSE: scroll region
	  case 0:
		if (ts.TermFlag&TF_PRINTERCTRL) {
			BuffPrint(! PrintEX);
		}
		break;
	  /* printer controller mode off */
	  case 4: break; /* See PrnParseCS() */
	  /* printer controller mode on */
	  case 5:
		if (ts.TermFlag&TF_PRINTERCTRL) {
			if (! AutoPrintMode)
				PrintFile_ = OpenPrnFile();
			DirectPrn = (ts.PrnDev[0]!=0);
			PrinterMode = TRUE;
		}
		break;
	}
}

void CS_l_Mode()		// RM
{
	switch (Param[1]) {
	  case 2:	// KAM
		KeybEnabled = TRUE; break;
	  case 4:	// IRM
		InsertMode = FALSE; break;
	  case 12:	// SRM
		ts.LocalEcho = 1;
		if (cv.Ready && cv.TelFlag && (ts.TelEcho>0))
			TelChangeEcho();
		break;
	  case 20:	// LF/NL
		LFMode = FALSE;
		ts.CRSend = IdCR;
		cv.CRSend = IdCR;
		break;
	  case 33:	// WYSTCURM
		if (ts.WindowFlag & WF_CURSORCHANGE) {
			ts.NonblinkingCursor = FALSE;
			ChangeCaret();
		}
		break;
	  case 34:	// WYULCURM
		if (ts.WindowFlag & WF_CURSORCHANGE) {
			ts.CursorShape = IdBlkCur;
			ChangeCaret();
		}
		break;
	}
}

void CS_n_Mode()		// DSR
{
	char Report[16];
	int X, Y, len;

	switch (Param[1]) {
	  case 5:
		/* Device Status Report -> Ready */
		SendCSIstr("0n", 0);
		break;
	  case 6:
		/* Cursor Position Report */
		if (isCursorOnStatusLine) {
			X = CursorX + 1;
			Y = 1;
		}
		else if (RelativeOrgMode) {
			X = CursorX - CursorLeftM + 1;
			Y = CursorY - CursorTop + 1;
		}
		else {
			X = CursorX + 1;
			Y = CursorY+1;
		}
		len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "%u;%uR", CLocale, Y, X);
		SendCSIstr(Report, len);
		break;
	}
}

void ParseSGRParams(PCharAttr attr, PCharAttr mask, int start)
{
	int i, j, P, r, g, b, color;
	TCharAttr dummy;

	if (mask == NULL) {
		mask = &dummy;
	}

	for (i=start ; i<=NParam ; i++) {
		P = Param[i];
		switch (P) {
		  case   0:	/* Clear all */
			attr->Attr = DefCharAttr.Attr;
			attr->Attr2 = DefCharAttr.Attr2 | (attr->Attr2&Attr2Protect);
			attr->AttrEx = attr->Attr;
			attr->Fore = DefCharAttr.Fore;
			attr->Back = DefCharAttr.Back;
			mask->Attr = AttrSgrMask;
			mask->Attr2 = Attr2ColorMask;
			break;

		  case   1:	/* Bold */
			attr->Attr |= AttrBold;
			mask->Attr |= AttrBold;
			break;

		  case   4:	/* Under line */
			attr->Attr |= AttrUnder;
			mask->Attr |= AttrUnder;
			break;

		  case   5:	/* Blink */
			attr->Attr |= AttrBlink;
			mask->Attr |= AttrBlink;
			break;

		  case   7:	/* Reverse */
			attr->Attr |= AttrReverse;
			mask->Attr |= AttrReverse;
			break;

		  case  22:	/* Bold off */
			attr->Attr &= ~ AttrBold;
			mask->Attr |= AttrBold;
			break;

		  case  24:	/* Under line off */
			attr->Attr &= ~ AttrUnder;
			mask->Attr |= AttrUnder;
			break;

		  case  25:	/* Blink off */
			attr->Attr &= ~ AttrBlink;
			mask->Attr |= AttrBlink;
			break;

		  case  27:	/* Reverse off */
			attr->Attr &= ~ AttrReverse;
			mask->Attr |= AttrReverse;
			break;

		  case  30:
		  case  31:
		  case  32:
		  case  33:
		  case  34:
		  case  35:
		  case  36:
		  case  37:	/* text color */
			attr->Attr2 |= Attr2Fore;
			mask->Attr2 |= Attr2Fore;
			attr->Fore = P - 30;
			break;

		  case  38:	/* text color (256color mode) */
			if (ts.ColorFlag & CF_XTERM256) {
				/*
				 * Change foreground color. accept following formats.
				 *
				 * 38 ; 2 ; r ; g ; b
				 * 38 ; 2 : r : g : b
				 * 38 : 2 : r : g : b
				 * 38 ; 5 ; idx
				 * 38 ; 5 : idx
				 * 38 : 5 : idx
				 *
				 */
				color = -1;
				j = 0;
				if (NSParam[i] > 0) {
					P = SubParam[i][1];
					j++;
				}
				else if (i < NParam) {
					P = Param[i+1];
					if (P == 2 || P == 5) {
						i++;
					}
				}
				switch (P) {
				  case 2:
					r = g = b = 0;
					if (NSParam[i] > 0) {
						if (j < NSParam[i]) {
							r = SubParam[i][++j];
							if (j < NSParam[i]) {
								g = SubParam[i][++j];
							}
							if (j < NSParam[i]) {
								b = SubParam[i][++j];
							}
							color = DispFindClosestColor(r, g, b);
						}
					}
					else if (i < NParam && NSParam[i+1] > 0) {
						r = Param[++i];
						g = SubParam[i][1];
						if (NSParam[i] > 1) {
							b = SubParam[i][2];
						}
						color = DispFindClosestColor(r, g, b);
					}
					else if (i+2 < NParam) {
						r = Param[++i];
						g = Param[++i];
						b = Param[++i];
						color = DispFindClosestColor(r, g, b);
					}
					break;
				  case 5:
					if (NSParam[i] > 0) {
						if (j < NSParam[i]) {
							color = SubParam[i][++j];
						}
					}
					else if (i < NParam) {
						color = Param[++i];
					}
					break;
				}
				if (color >= 0 && color < 256) {
					attr->Attr2 |= Attr2Fore;
					mask->Attr2 |= Attr2Fore;
					attr->Fore = color;
				}
			}
			break;

		  case  39:	/* Reset text color */
			attr->Attr2 &= ~ Attr2Fore;
			mask->Attr2 |= Attr2Fore;
			attr->Fore = AttrDefaultFG;
			break;

		  case  40:
		  case  41:
		  case  42:
		  case  43:
		  case  44:
		  case  45:
		  case  46:
		  case  47:	/* Back color */
			attr->Attr2 |= Attr2Back;
			mask->Attr2 |= Attr2Back;
			attr->Back = P - 40;
			break;

		  case  48:	/* Back color (256color mode) */
			if (ts.ColorFlag & CF_XTERM256) {
				color = -1;
				j = 0;
				if (NSParam[i] > 0) {
					P = SubParam[i][1];
					j++;
				}
				else if (i < NParam) {
					P = Param[i+1];
					if (P == 2 || P == 5) {
						i++;
					}
				}
				switch (P) {
				  case 2:
					r = g = b = 0;
					if (NSParam[i] > 0) {
						if (j < NSParam[i]) {
							r = SubParam[i][++j];
							if (j < NSParam[i]) {
								g = SubParam[i][++j];
							}
							if (j < NSParam[i]) {
								b = SubParam[i][++j];
							}
							color = DispFindClosestColor(r, g, b);
						}
					}
					else if (i < NParam && NSParam[i+1] > 0) {
						r = Param[++i];
						g = SubParam[i][1];
						if (NSParam[i] > 1) {
							b = SubParam[i][2];
						}
						color = DispFindClosestColor(r, g, b);
					}
					else if (i+2 < NParam) {
						r = Param[++i];
						g = Param[++i];
						b = Param[++i];
						color = DispFindClosestColor(r, g, b);
					}
					break;
				  case 5:
					if (NSParam[i] > 0) {
						if (j < NSParam[i]) {
							color = SubParam[i][++j];
						}
					}
					else if (i < NParam) {
						color = Param[++i];
					}
					break;
				}
				if (color >= 0 && color < 256) {
					attr->Attr2 |= Attr2Back;
					mask->Attr2 |= Attr2Back;
					attr->Back = color;
				}
			}
			break;

		  case  49:	/* Reset back color */
			attr->Attr2 &= ~ Attr2Back;
			mask->Attr2 |= Attr2Back;
			attr->Back = AttrDefaultBG;
			break;

		  case 90:
		  case 91:
		  case 92:
		  case 93:
		  case 94:
		  case 95:
		  case 96:
		  case 97:	/* aixterm style text color */
			if (ts.ColorFlag & CF_AIXTERM16) {
				attr->Attr2 |= Attr2Fore;
				mask->Attr2 |= Attr2Fore;
				attr->Fore = P - 90 + 8;
			}
			break;

		  case 100:
			if (! (ts.ColorFlag & CF_AIXTERM16)) {
				/* Reset text and back color */
				attr->Attr2 &= ~ (Attr2Fore | Attr2Back);
				mask->Attr2 |= Attr2ColorMask;
				attr->Fore = AttrDefaultFG;
				attr->Back = AttrDefaultBG;
				break;
			}
			/* fall through to aixterm style back color */

		  case 101:
		  case 102:
		  case 103:
		  case 104:
		  case 105:
		  case 106:
		  case 107:	/* aixterm style back color */
			if (ts.ColorFlag & CF_AIXTERM16) {
				attr->Attr2 |= Attr2Back;
				mask->Attr2 |= Attr2Back;
				attr->Back = P - 100 + 8;
			}
			break;
		}
	}
}

static void CSSetAttr(void)		// SGR
{
	UpdateStr();
	ParseSGRParams(&CharAttr, NULL, 1);
	BuffSetCurCharAttr(CharAttr);
}

void CSSetScrollRegion()	// DECSTBM
{
	if (isCursorOnStatusLine) {
		MoveCursor(0,CursorY);
		return;
	}

	RequiredParams(2);
	CheckParamVal(Param[1], NumOfLines-StatusLine);
	CheckParamValMax(Param[2], NumOfLines-StatusLine);

	if (Param[1] >= Param[2])
		return;

	CursorTop = Param[1] - 1;
	CursorBottom = Param[2] - 1;

	if (RelativeOrgMode)
		// TODO: 左マージンを無視してる。要実機確認。
		MoveCursor(0, CursorTop);
	else
		MoveCursor(0, 0);
}

void CSSetLRScrollRegion()	// DECSLRM
{
//	TODO: ステータスライン上での挙動確認。
//	if (isCursorOnStatusLine) {
//		MoveCursor(0,CursorY);
//		return;
//	}

	RequiredParams(2);
	CheckParamVal(Param[1], NumOfColumns);
	CheckParamValMax(Param[2], NumOfColumns);

	if (Param[1] >= Param[2])
		return;

	CursorLeftM = Param[1] - 1;
	CursorRightM = Param[2] - 1;

	if (RelativeOrgMode)
		MoveCursor(CursorLeftM, CursorTop);
	else
		MoveCursor(0, 0);
}

void CSSunSequence() /* Sun terminal private sequences */
{
	int x, y, len;
	char Report[TitleBuffSize*2+10];
	PTStack t;

	switch (Param[1]) {
	  case 1: // De-iconify window
		if (ts.WindowFlag & WF_WINDOWCHANGE)
			DispShowWindow(WINDOW_RESTORE);
		break;

	  case 2: // Iconify window
		if (ts.WindowFlag & WF_WINDOWCHANGE)
			DispShowWindow(WINDOW_MINIMIZE);
		break;

	  case 3: // set window position
		if (ts.WindowFlag & WF_WINDOWCHANGE) {
			RequiredParams(3);
			DispMoveWindow(Param[2], Param[3]);
		}
		break;

	  case 4: // set window size
		if (ts.WindowFlag & WF_WINDOWCHANGE) {
			RequiredParams(3);
			DispResizeWin(Param[3], Param[2]);
		}
		break;

	  case 5: // Raise window
		if (ts.WindowFlag & WF_WINDOWCHANGE)
			DispShowWindow(WINDOW_RAISE);
		break;

	  case 6: // Lower window
		if (ts.WindowFlag & WF_WINDOWCHANGE)
			DispShowWindow(WINDOW_LOWER);
		break;

	  case 7: // Refresh window
		if (ts.WindowFlag & WF_WINDOWCHANGE)
			DispShowWindow(WINDOW_REFRESH);
		break;

	  case 8: /* set terminal size */
		if (ts.WindowFlag & WF_WINDOWCHANGE) {
			RequiredParams(3);
			if (Param[2] <= 1) Param[2] = 24;
			if (Param[3] <= 1) Param[3] = 80;
			ChangeTerminalSize(Param[3], Param[2]);
		}
		break;

	  case 9: // Maximize/Restore window
		if (ts.WindowFlag & WF_WINDOWCHANGE) {
			RequiredParams(2);
			if (Param[2] == 0) {
				DispShowWindow(WINDOW_RESTORE);
			}
			else if (Param[2] == 1) {
				DispShowWindow(WINDOW_MAXIMIZE);
			}
		}
		break;

	  case 10: // Full-screen
		/*
		 * 本来ならば PuTTY のようなフルスクリーンモードを実装するべきだが、
		 * とりあえずは手抜きで最大化を利用する
		 */
		if (ts.WindowFlag & WF_WINDOWCHANGE) {
			RequiredParams(2);
			switch (Param[2]) {
			  case 0:
			    DispShowWindow(WINDOW_RESTORE);
			    break;
			  case 1:
			    DispShowWindow(WINDOW_MAXIMIZE);
			    break;
			  case 2:
			    DispShowWindow(WINDOW_TOGGLE_MAXIMIZE);
			    break;
			}
		}
		break;

	  case 11: // Report window state
		if (ts.WindowFlag & WF_WINDOWREPORT) {
			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "%dt", CLocale, DispWindowIconified()?2:1);
			SendCSIstr(Report, len);
		}
		break;

	  case 13: // Report window position
		if (ts.WindowFlag & WF_WINDOWREPORT) {
			RequiredParams(2);
			switch (Param[2]) {
			  case 0:
			  case 1:
				DispGetWindowPos(&x, &y, FALSE);
				break;
			  case 2:
				DispGetWindowPos(&x, &y, TRUE);
				break;
			  default:
				return;
			}
			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "3;%u;%ut", CLocale, (unsigned int)x, (unsigned int)y);
			SendCSIstr(Report, len);
		}
		break;

	  case 14: /* get window size */
		if (ts.WindowFlag & WF_WINDOWREPORT) {
			RequiredParams(2);
			switch (Param[2]) {
			  case 0:
			  case 1:
				DispGetWindowSize(&x, &y, TRUE);
				break;
			  case 2:
				DispGetWindowSize(&x, &y, FALSE);
				break;
			  default:
				return;
			}

			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "4;%d;%dt", CLocale, y, x);
			SendCSIstr(Report, len);
		}
		break;

	  case 15: // Report display size (pixel)
		if (ts.WindowFlag & WF_WINDOWREPORT) {
			DispGetRootWinSize(&x, &y, TRUE);
			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "5;%d;%dt", CLocale, y, x);
			SendCSIstr(Report, len);
		}
		break;

	  case 16: // Report character cell size (pixel)
		if (ts.WindowFlag & WF_WINDOWREPORT) {
			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "6;%d;%dt", CLocale, FontHeight, FontWidth);
			SendCSIstr(Report, len);
		}
		break;

	  case 18: /* get terminal size */
		if (ts.WindowFlag & WF_WINDOWREPORT) {
			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "8;%u;%ut", CLocale,
			                    NumOfLines-StatusLine, NumOfColumns);
			SendCSIstr(Report, len);
		}
		break;

	  case 19: // Report display size (character)
		if (ts.WindowFlag & WF_WINDOWREPORT) {
			DispGetRootWinSize(&x, &y, FALSE);
			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "9;%d;%dt", CLocale, y, x);
			SendCSIstr(Report, len);
		}
		break;

	  case 20: // Report icon label
		switch (ts.WindowFlag & WF_TITLEREPORT) {
		  case IdTitleReportIgnore:
			// nothing to do
			break;

		  case IdTitleReportAccept:
			switch (ts.AcceptTitleChangeRequest) {
			  case IdTitleChangeRequestOff:
				len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "L%s", CLocale, ts.Title);
				break;

			  case IdTitleChangeRequestAhead:
				len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "L%s %s", CLocale, cv.TitleRemote, ts.Title);
				break;

			  case IdTitleChangeRequestLast:
				len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "L%s %s", CLocale, ts.Title, cv.TitleRemote);
				break;

			  default:
				if (cv.TitleRemote[0] == 0) {
					len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "L%s", CLocale, ts.Title);
				}
				else {
					len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "L%s", CLocale, cv.TitleRemote);
				}
			}
			SendOSCstr(Report, len, ST);
			break;

		  default: // IdTitleReportEmpty:
			SendOSCstr("L", 0, ST);
			break;
		}
		break;

	  case 21: // Report window title
		switch (ts.WindowFlag & WF_TITLEREPORT) {
		  case IdTitleReportIgnore:
			// nothing to do
			break;

		  case IdTitleReportAccept:
			switch (ts.AcceptTitleChangeRequest) {
			  case IdTitleChangeRequestOff:
				len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "l%s", CLocale, ts.Title);
				break;

			  case IdTitleChangeRequestAhead:
				len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "l%s %s", CLocale, cv.TitleRemote, ts.Title);
				break;

			  case IdTitleChangeRequestLast:
				len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "l%s %s", CLocale, ts.Title, cv.TitleRemote);
				break;

			  default:
				if (cv.TitleRemote[0] == 0) {
					len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "l%s", CLocale, ts.Title);
				}
				else {
					len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "l%s", CLocale, cv.TitleRemote);
				}
			}
			SendOSCstr(Report, len, ST);
			break;

		  default: // IdTitleReportEmpty:
			SendOSCstr("l", 0, ST);
			break;
		}
		break;

	  case 22: // Push Title
		RequiredParams(2);
		switch (Param[2]) {
		  case 0:
		  case 1:
		  case 2:
			if (ts.AcceptTitleChangeRequest && (t=malloc(sizeof(TStack))) != NULL) {
				if ((t->title = _strdup(cv.TitleRemote)) != NULL) {
					t->next = TitleStack;
					TitleStack = t;
				}
				else {
					free(t);
				}
			}
			break;
		}
		break;

	  case 23: // Pop Title
		RequiredParams(2);
		switch (Param[2]) {
		  case 0:
		  case 1:
		  case 2:
			if (ts.AcceptTitleChangeRequest && TitleStack != NULL) {
				t = TitleStack;
				TitleStack = t->next;
				strncpy_s(cv.TitleRemote, sizeof(cv.TitleRemote), t->title, _TRUNCATE);
				ChangeTitle();
				free(t->title);
				free(t);
			}
			break;
		}
	}
}

void CSLT(BYTE b)
{
	switch (b) {
	  case 'r':
		if (CanUseIME()) {
			SetIMEOpenStatus(HVTWin, SavedIMEstatus);
		}
		break;

	  case 's':
		if (CanUseIME()) {
			SavedIMEstatus = GetIMEOpenStatus(HVTWin);
		}
		break;

	  case 't':
		if (CanUseIME()) {
			SetIMEOpenStatus(HVTWin, Param[1] == 1);
		}
		break;
	}
}

void CSEQ(BYTE b)
{
	char Report[16];
	int len;

	switch (b) {
	  case 'c': /* Tertiary terminal report (Tertiary DA) */
		if (Param[1] == 0) {
			len = _snprintf_s_l(Report, sizeof(Report), _TRUNCATE, "!|%8s", CLocale, ts.TerminalUID);
			SendDCSstr(Report, len);
		}
		break;
	}
}

void CSGT(BYTE b)
{
	switch (b) {
	  case 'c': /* second terminal report (Secondary DA) */
		if (Param[1] == 0) {
			SendCSIstr(">32;331;0c", 0); /* VT382(>32) + xterm rev 331 */
		}
		break;

	  case 'J': // IO-8256 terminal
		if (Param[1]==3) {
			RequiredParams(5);
			CheckParamVal(Param[2], NumOfLines-StatusLine);
			CheckParamVal(Param[3], NumOfColumns);
			CheckParamValMax(Param[4], NumOfLines-StatusLine);
			CheckParamValMax(Param[5], NumOfColumns);

			if (Param[2] > Param[4] || Param[3] > Param[5]) {
				return;
			}

			BuffEraseBox(Param[3]-1, Param[2]-1, Param[5]-1, Param[4]-1);
		}
		break;

	  case 'K': // IO-8256 terminal
		switch (Param[1]) {
		  case 3:
			RequiredParams(3);
			CheckParamVal(Param[2], NumOfColumns);
			CheckParamVal(Param[3], NumOfColumns);

			if (Param[2] > Param[3]) {
				return;
			}

			BuffEraseCharsInLine(Param[2]-1, Param[3]-Param[2]+1);
			break;

		  case 5:
			RequiredParams(3);
			switch (Param[2]) {
			  case 3:
			  case 4:
			  case 5:
			  case 6: // Draw Line
				BuffDrawLine(CharAttr, Param[2], Param[3]);
				break;

			  case 12: // Text color
				if ((Param[3]>=0) && (Param[3]<=7)) {
					switch (Param[3]) {
					  case 3: CharAttr.Fore = IdBlue; break;
					  case 4: CharAttr.Fore = IdCyan; break;
					  case 5: CharAttr.Fore = IdYellow; break;
					  case 6: CharAttr.Fore = IdMagenta; break;
					  default: CharAttr.Fore = Param[3]; break;
					}
					CharAttr.Attr2 |= Attr2Fore;
					BuffSetCurCharAttr(CharAttr);
				}
				break;
			}
			break;
		}
		break;
	}
}

// DECSCNM / Visual Bell
static void CSQExchangeColor(void)
{
	BuffUpdateScroll();

	ts.ColorFlag ^= CF_REVERSEVIDEO;

	DispChangeBackground();
	UpdateWindow(HVTWin);
}

void CSQChangeColumnMode(int width)		// DECCOLM
{
	ChangeTerminalSize(width, NumOfLines-StatusLine);
	LRMarginMode = FALSE;

	// DECCOLM では画面がクリアされるのが仕様
	// ClearOnResize が off の時はここでクリアする。
	// ClearOnResize が on の時は ChangeTerminalSize() を呼ぶとクリアされるので、
	// 余計なスクロールを避ける為にここではクリアしない。
	if ((ts.TermFlag & TF_CLEARONRESIZE) == 0) {
		MoveCursor(0, 0);
		BuffClearScreen();
		UpdateWindow(HVTWin);
	}
}

void CSQ_h_Mode() // DECSET
{
	int i;

	for (i = 1 ; i<=NParam ; i++) {
		switch (Param[i]) {
		  case 1: AppliCursorMode = TRUE; break;		// DECCKM
		  case 3: CSQChangeColumnMode(132); break;		// DECCOLM
		  case 5: /* Reverse Video (DECSCNM) */
			if (!(ts.ColorFlag & CF_REVERSEVIDEO))
				CSQExchangeColor(); /* Exchange text/back color */
			break;
		  case 6: // DECOM
			if (isCursorOnStatusLine)
				MoveCursor(0,CursorY);
			else {
				RelativeOrgMode = TRUE;
				MoveCursor(0,CursorTop);
			}
			break;
		  case 7: AutoWrapMode = TRUE; break;		// DECAWM
		  case 8: AutoRepeatMode = TRUE; break;		// DECARM
		  case 9: /* X10 Mouse Tracking */
			if (ts.MouseEventTracking)
				MouseReportMode = IdMouseTrackX10;
			break;
		  case 12: /* att610 cursor blinking */
			if (ts.WindowFlag & WF_CURSORCHANGE) {
				ts.NonblinkingCursor = FALSE;
				ChangeCaret();
			}
			break;
		  case 19: PrintEX = TRUE; break;		// DECPEX
		  case 25: DispEnableCaret(TRUE); break;	// cursor on (DECTCEM)
		  case 38: // DECTEK
			if (ts.AutoWinSwitch>0)
				ChangeEmu = IdTEK; /* Enter TEK Mode */
			break;
		  case 47: // Alternate Screen Buffer
			if ((ts.TermFlag & TF_ALTSCR) && !AltScr) {
				BuffSaveScreen();
				AltScr = TRUE;
			}
			break;
		  case 59:
			if (ts.Language==IdJapanese) {
				/* kanji terminal */
				Gn[0] = IdASCII;
				Gn[1] = IdKatakana;
				Gn[2] = IdKatakana;
				Gn[3] = IdKanji;
				Glr[0] = 0;
				if ((ts.KanjiCode==IdJIS) &&
				    (ts.JIS7Katakana==0))
					Glr[1] = 2;  // 8-bit katakana
				else
					Glr[1] = 3;
			}
			break;
		  case 66: AppliKeyMode = TRUE; break;		// DECNKM
		  case 67: ts.BSKey = IdBS; break;		// DECBKM
		  case 69: LRMarginMode = TRUE; break;		// DECLRMM (DECVSSM)
		  case 1000: // Mouse Tracking
			if (ts.MouseEventTracking)
				MouseReportMode = IdMouseTrackVT200;
			break;
		  case 1001: // Hilite Mouse Tracking
			if (ts.MouseEventTracking)
				MouseReportMode = IdMouseTrackVT200Hl;
			break;
		  case 1002: // Button-Event Mouse Tracking
			if (ts.MouseEventTracking)
				MouseReportMode = IdMouseTrackBtnEvent;
			break;
		  case 1003: // Any-Event Mouse Tracking
			if (ts.MouseEventTracking)
				MouseReportMode = IdMouseTrackAllEvent;
			break;
		  case 1004: // Focus Report
			if (ts.MouseEventTracking)
				FocusReportMode = TRUE;
			break;
		  case 1005: // Extended Mouse Tracking (UTF-8)
			if (ts.MouseEventTracking)
				MouseReportExtMode = IdMouseTrackExtUTF8;
			break;
		  case 1006: // Extended Mouse Tracking (SGR)
			if (ts.MouseEventTracking)
				MouseReportExtMode = IdMouseTrackExtSGR;
			break;
		  case 1015: // Extended Mouse Tracking (rxvt-unicode)
			if (ts.MouseEventTracking)
				MouseReportExtMode = IdMouseTrackExtURXVT;
			break;
		  case 1047: // Alternate Screen Buffer
			if ((ts.TermFlag & TF_ALTSCR) && !AltScr) {
				BuffSaveScreen();
				AltScr = TRUE;
			}
			break;
		  case 1048: // Save Cursor Position (Alternate Screen Buffer)
			if (ts.TermFlag & TF_ALTSCR) {
				SaveCursor();
			}
			break;
		  case 1049: // Alternate Screen Buffer
			if ((ts.TermFlag & TF_ALTSCR) && !AltScr) {
				SaveCursor();
				BuffSaveScreen();
				BuffClearScreen();
				AltScr = TRUE;
			}
			break;
		  case 2004: // Bracketed Paste Mode
			BracketedPaste = TRUE;
			break;
		  case 7727: // mintty Application Escape Mode
			AppliEscapeMode = 1;
			break;
		  case 7786: // Wheel to Cursor translation
			if (ts.TranslateWheelToCursor) {
				AcceptWheelToCursor = TRUE;
			}
			break;
		  case 8200: // ClearThenHome
			ClearThenHome = TRUE;
			break;
		  case 14001: // NetTerm mouse mode
			if (ts.MouseEventTracking)
				MouseReportMode = IdMouseTrackNetTerm;
			break;
		  case 14002: // test Application Escape Mode 2
		  case 14003: // test Application Escape Mode 3
		  case 14004: // test Application Escape Mode 4
			AppliEscapeMode = Param[i] - 14000;
			break;
		}
	}
}

static void PrintFileFinish(PrintFile *handle)
{
	PrnFinish(handle);
	PrintFile_ = NULL;
}

static void CSQ_i_Mode(void)		// DECMC
{
	switch (Param[1]) {
	  case 1:
		if (ts.TermFlag&TF_PRINTERCTRL) {
			PrintFile_ = OpenPrnFile();
			BuffDumpCurrentLine(PrintFile_, LF);
			if (! AutoPrintMode) {
				ClosePrnFile(PrintFile_, PrintFileFinish);
			}
		}
		break;
	  /* auto print mode off */
	  case 4:
		if (AutoPrintMode) {
			ClosePrnFile(PrintFile_, PrintFileFinish);
			AutoPrintMode = FALSE;
		}
		break;
	  /* auto print mode on */
	  case 5:
		if (ts.TermFlag&TF_PRINTERCTRL) {
			if (! AutoPrintMode) {
				PrintFile_ = OpenPrnFile();
				AutoPrintMode = TRUE;
			}
		}
		break;
	}
}

void CSQ_l_Mode()		// DECRST
{
	int i;

	for (i = 1 ; i <= NParam ; i++) {
		switch (Param[i]) {
		  case 1: AppliCursorMode = FALSE; break;	// DECCKM
		  case 3: CSQChangeColumnMode(80); break;	// DECCOLM
		  case 5: /* Normal Video (DECSCNM) */
			if (ts.ColorFlag & CF_REVERSEVIDEO)
				CSQExchangeColor(); /* Exchange text/back color */
			break;
		  case 6: // DECOM
			if (isCursorOnStatusLine)
				MoveCursor(0,CursorY);
			else {
				RelativeOrgMode = FALSE;
				MoveCursor(0,0);
			}
			break;
		  case 7: AutoWrapMode = FALSE; break;		// DECAWM
		  case 8: AutoRepeatMode = FALSE; break;	// DECARM
		  case 9: MouseReportMode = IdMouseTrackNone; break; /* X10 Mouse Tracking */
		  case 12: /* att610 cursor blinking */
			if (ts.WindowFlag & WF_CURSORCHANGE) {
				ts.NonblinkingCursor = TRUE;
				ChangeCaret();
			}
			break;
		  case 19: PrintEX = FALSE; break;		// DECPEX
		  case 25: DispEnableCaret(FALSE); break;	// cursor off (DECTCEM)
		  case 47: // Alternate Screen Buffer
			if ((ts.TermFlag & TF_ALTSCR) && AltScr) {
				BuffRestoreScreen();
				AltScr = FALSE;
			}
			break;
		  case 59:
			if (ts.Language==IdJapanese) {
				/* katakana terminal */
				Gn[0] = IdASCII;
				Gn[1] = IdKatakana;
				Gn[2] = IdKatakana;
				Gn[3] = IdKanji;
				Glr[0] = 0;
				if ((ts.KanjiCode==IdJIS) &&
				    (ts.JIS7Katakana==0))
					Glr[1] = 2;	// 8-bit katakana
				else
					Glr[1] = 3;
			}
			break;
		  case 66: AppliKeyMode = FALSE; break;		// DECNKM
		  case 67: ts.BSKey = IdDEL; break;		// DECBKM
		  case 69: // DECLRMM (DECVSSM)
			LRMarginMode = FALSE;
			CursorLeftM = 0;
			CursorRightM = NumOfColumns - 1;
			break;
		  case 1000: // Mouse Tracking
		  case 1001: // Hilite Mouse Tracking
		  case 1002: // Button-Event Mouse Tracking
		  case 1003: // Any-Event Mouse Tracking
			MouseReportMode = IdMouseTrackNone;
			break;
		  case 1004: // Focus Report
			FocusReportMode = FALSE;
			break;
		  case 1005: // Extended Mouse Tracking (UTF-8)
		  case 1006: // Extended Mouse Tracking (SGR)
		  case 1015: // Extended Mouse Tracking (rxvt-unicode)
			MouseReportExtMode = IdMouseTrackExtNone;
			break;
		  case 1047: // Alternate Screen Buffer
			if ((ts.TermFlag & TF_ALTSCR) && AltScr) {
				BuffClearScreen();
				BuffRestoreScreen();
				AltScr = FALSE;
			}
			break;
		  case 1048: // Save Cursor Position (Alternate Screen Buffer)
			if (ts.TermFlag & TF_ALTSCR) {
				RestoreCursor();
			}
			break;
		  case 1049: // Alternate Screen Buffer
			if ((ts.TermFlag & TF_ALTSCR) && AltScr) {
				BuffClearScreen();
				BuffRestoreScreen();
				AltScr = FALSE;
				RestoreCursor();
			}
			break;
		  case 2004: // Bracketed Paste Mode
			BracketedPaste = FALSE;
			break;
		  case 7727: // mintty Application Escape Mode
			AppliEscapeMode = 0;
			break;
		  case 7786: // Wheel to Cursor translation
			AcceptWheelToCursor = FALSE;
			break;
		  case 8200: // ClearThenHome
			ClearThenHome = FALSE;
			break;
		  case 14001: // NetTerm mouse mode
			MouseReportMode = IdMouseTrackNone;
			break;
		  case 14002: // test Application Escape Mode 2
		  case 14003: // test Application Escape Mode 3
		  case 14004: // test Application Escape Mode 4
			AppliEscapeMode = 0;
			break;
		}
	}
}

void CSQ_n_Mode()		// DECDSR
{
	switch (Param[1]) {
	  case 53:
	  case 55:
		/* Locator Device Status Report -> Ready */
		SendCSIstr("?50n", 0);
		break;
	}
}

static void CSQuest(BYTE b)
{
	switch (b) {
	  case 'J': CSQSelScreenErase(); break;	// DECSED
	  case 'K': CSQSelLineErase(); break;	// DECSEL
	  case 'h': CSQ_h_Mode(); break;	// DECSET
	  case 'i': CSQ_i_Mode(); break;	// DECMC
	  case 'l': CSQ_l_Mode(); break;	// DECRST
	  case 'n': CSQ_n_Mode(); break;	// DECDSR
	}
}

void SoftReset()
// called by software-reset escape sequence handler
{
	UpdateStr();
	AutoRepeatMode = TRUE;
	DispEnableCaret(TRUE); // cursor on
	InsertMode = FALSE;
	RelativeOrgMode = FALSE;
	AppliKeyMode = FALSE;
	AppliCursorMode = FALSE;
	AppliEscapeMode = FALSE;
	AcceptWheelToCursor = ts.TranslateWheelToCursor;
	if (isCursorOnStatusLine)
		MoveToMainScreen();
	CursorTop = 0;
	CursorBottom = NumOfLines-1-StatusLine;
	CursorLeftM = 0;
	CursorRightM = NumOfColumns - 1;
	ResetCharSet();

	/* Attribute */
	CharAttr = DefCharAttr;
	Special = FALSE;
	BuffSetCurCharAttr(CharAttr);

	// status buffers
	ResetCurSBuffer();

	// Saved IME status
	IMEstat = FALSE;
}

void CSExc(BYTE b)
{
	switch (b) {
	  case 'p':
		/* Software reset */
		SoftReset();
		break;
	}
}

void CSDouble(BYTE b)
{
	switch (b) {
	  case 'p': // DECSCL
		/* Select terminal mode (software reset) */
		RequiredParams(2);

		SoftReset();
		ChangeTerminalID();
		if (Param[1] >= 61 && Param[1] <= 65) {
			if (VTlevel > Param[1] - 60) {
				VTlevel = Param[1] - 60;
			}
		}
		else {
			VTlevel = 1;
		}

		if (VTlevel < 2 || Param[2] == 1)
			Send8BitMode = FALSE;
		else
			Send8BitMode = TRUE;
		break;

	  case 'q': // DECSCA
		switch (Param[1]) {
		  case 0:
		  case 2:
			CharAttr.Attr2 &= ~Attr2Protect;
			BuffSetCurCharAttr(CharAttr);
			break;
		  case 1:
			CharAttr.Attr2 |= Attr2Protect;
			BuffSetCurCharAttr(CharAttr);
			break;
		  default:
			/* nothing to do */
			break;
		}
		break;
	}
}

static void CSDolRequestMode(void) // DECRQM
{
	char buff[256];
	char *pp;
	int len, resp = 0;

	switch (Prv) {
	  case 0: /* ANSI Mode */
		resp = 4;
		pp = "";
		switch (Param[1]) {
		  case 2:	// KAM
			if (KeybEnabled)
				resp = 2;
			else
				resp = 1;
			break;
		  case 4:	// IRM
			if (InsertMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 12:	// SRM
			if (ts.LocalEcho)
				resp = 2;
			else
				resp = 1;
			break;
		  case 20:	// LNM
			if (LFMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 33:	// WYSTCURM
			if (ts.NonblinkingCursor)
				resp = 1;
			else
				resp = 2;
			if ((ts.WindowFlag & WF_CURSORCHANGE) == 0)
				resp += 2;
			break;
		  case 34:	// WYULCURM
			if (ts.CursorShape == IdHCur)
				resp = 1;
			else
				resp = 2;
			if ((ts.WindowFlag & WF_CURSORCHANGE) == 0)
				resp += 2;
			break;
		}
		break;

	  case '?': /* DEC Mode */
		pp = "?";
		switch (Param[1]) {
		  case 1:	// DECCKM
			if (AppliCursorMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 3:	// DECCOLM
			if (NumOfColumns == 132)
				resp = 1;
			else
				resp = 2;
			break;
		  case 5:	// DECSCNM
			if (ts.ColorFlag & CF_REVERSEVIDEO)
				resp = 1;
			else
				resp = 2;
			break;
		  case 6:	// DECOM
			if (RelativeOrgMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 7:	// DECAWM
			if (AutoWrapMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 8:	// DECARM
			if (AutoRepeatMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 9:	// XT_MSE_X10 -- X10 Mouse Tracking
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportMode == IdMouseTrackX10)
				resp = 1;
			else
				resp = 2;
			break;
		  case 12:	// XT_CBLINK -- att610 cursor blinking
			if (ts.NonblinkingCursor)
				resp = 2;
			else
				resp = 1;
			if ((ts.WindowFlag & WF_CURSORCHANGE) == 0)
				resp += 2;
			break;
		  case 19:	// DECPEX
			if (PrintEX)
				resp = 1;
			else
				resp = 2;
			break;
		  case 25:	// DECTCEM
			if (IsCaretEnabled())
				resp = 1;
			else
				resp = 2;
			break;
		  case 38:	// DECTEK
			resp = 4;
			break;
		  case 47:	// XT_ALTSCRN -- Alternate Screen / (DECGRPM)
			if ((ts.TermFlag & TF_ALTSCR) == 0)
				resp = 4;
			else if (AltScr)
				resp = 1;
			else
				resp = 2;
			break;
		  case 59:	// DECKKDM
			if (ts.Language!=IdJapanese)
				resp = 0;
			else if ((ts.KanjiCode == IdJIS) && (!ts.JIS7Katakana))
				resp = 4;
			else
				resp = 3;
			break;
		  case 66:	// DECNKM
			if (AppliKeyMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 67:	// DECBKM
			if (ts.BSKey==IdBS)
				resp = 1;
			else
				resp = 2;
			break;
		  case 69:	// DECRQM
			if (LRMarginMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case	1000:	// XT_MSE_X11
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportMode == IdMouseTrackVT200)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1001:	// XT_MSE_HL
#if 0
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportMode == IdMouseTrackVT200Hl)
				resp = 1;
			else
				resp = 2;
#else
			resp = 4;
#endif
			break;
		  case 1002:	// XT_MSE_BTN
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportMode == IdMouseTrackBtnEvent)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1003:	// XT_MSE_ANY
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportMode == IdMouseTrackAllEvent)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1004:	// XT_MSE_WIN
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (FocusReportMode)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1005:	// XT_MSE_UTF
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportExtMode == IdMouseTrackExtUTF8)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1006:	// XT_MSE_SGR
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportExtMode == IdMouseTrackExtSGR)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1015:	// urxvt-style extended mouse tracking
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportExtMode == IdMouseTrackExtURXVT)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1047:	// XT_ALTS_47
			if ((ts.TermFlag & TF_ALTSCR) == 0)
				resp = 4;
			else if (AltScr)
				resp = 1;
			else
				resp = 2;
			break;
		  case 1048:
			if ((ts.TermFlag & TF_ALTSCR) == 0)
				resp = 4;
			else
				resp = 1;
			break;
		  case 1049:	// XT_EXTSCRN
			if ((ts.TermFlag & TF_ALTSCR) == 0)
				resp = 4;
			else if (AltScr)
				resp = 1;
			else
				resp = 2;
			break;
		  case 2004:	// RL_BRACKET
			if (BracketedPaste)
				resp = 1;
			else
				resp = 2;
			break;
		  case 7727:	// MinTTY Application Escape Mode
			if (AppliEscapeMode == 1)
				resp = 1;
			else
				resp = 2;
			break;
		  case 7786:	// MinTTY Mousewheel reporting
			if (!ts.TranslateWheelToCursor)
				resp = 4;
			else if (AcceptWheelToCursor)
				resp = 1;
			else
				resp = 2;
			break;
		  case 8200:	// ClearThenHome
			if (ClearThenHome)
				resp = 1;
			else
				resp = 2;
			break;
		  case 14001:	// NetTerm Mouse Reporting (TT)
			if (!ts.MouseEventTracking)
				resp = 4;
			else if (MouseReportMode == IdMouseTrackNetTerm)
				resp = 1;
			else
				resp = 2;
			break;
		  case 14002:	// test Application Escape Mode 2
		  case 14003:	// test Application Escape Mode 3
		  case 14004:	// test Application Escape Mode 4
			if (AppliEscapeMode == Param[1] - 14000)
				resp = 1;
			else
				resp = 2;
			break;
		}
		break;
	  default:
		assert(FALSE);
		return;
	}

	len = _snprintf_s(buff, sizeof(buff), _TRUNCATE, "%s%d;%d$y", pp, Param[1], resp);
	SendCSIstr(buff, len);
}

void CSDol(BYTE b)
{
	TCharAttr attr, mask;
	attr = DefCharAttr;
	mask = DefCharAttr;

	switch (b) {
	  case 'p': // DECRQM
		CSDolRequestMode();
		break;

	  case 'r': // DECCARA
	  case 't': // DECRARA
		RequiredParams(4);
		CheckParamVal(Param[1], NumOfLines-StatusLine);
		CheckParamVal(Param[2], NumOfColumns);
		CheckParamValMax(Param[3], NumOfLines-StatusLine);
		CheckParamValMax(Param[4], NumOfColumns);

		if (Param[1] > Param[3] || Param[2] > Param[4]) {
			return;
		}

		if (RelativeOrgMode) {
			Param[1] += CursorTop;
			if (Param[1] > CursorBottom) {
				Param[1] = CursorBottom + 1;
			}
			Param[3] += CursorTop;
			if (Param[3] > CursorBottom) {
				Param[3] = CursorBottom + 1;
			}

			// TODO: 左右マージンのチェックを行う。
		}

		ParseSGRParams(&attr, &mask, 5);
		if (b == 'r') { // DECCARA
			attr.Attr &= AttrSgrMask;
			mask.Attr &= AttrSgrMask;
			attr.Attr2 &= Attr2ColorMask;
			attr.AttrEx = attr.Attr;
			mask.Attr2 &= Attr2ColorMask;
			if (RectangleMode) {
				BuffChangeAttrBox(Param[2]-1, Param[1]-1, Param[4]-1, Param[3]-1, &attr, &mask);
			}
			else {
				BuffChangeAttrStream(Param[2]-1, Param[1]-1, Param[4]-1, Param[3]-1, &attr, &mask);
			}
		}
		else { // DECRARA
			attr.Attr &= AttrSgrMask;
			if (RectangleMode) {
			    BuffChangeAttrBox(Param[2]-1, Param[1]-1, Param[4]-1, Param[3]-1, &attr, NULL);
			}
			else {
			    BuffChangeAttrStream(Param[2]-1, Param[1]-1, Param[4]-1, Param[3]-1, &attr, NULL);
			}
		}
		break;

	  case 'v': // DECCRA
		RequiredParams(8);
		CheckParamVal(Param[1], NumOfLines-StatusLine);		// Src Y-start
		CheckParamVal(Param[2], NumOfColumns);			// Src X-start
		CheckParamValMax(Param[3], NumOfLines-StatusLine);	// Src Y-end
		CheckParamValMax(Param[4], NumOfColumns);		// Src X-end
		CheckParamVal(Param[5], 1);				// Src Page
		CheckParamVal(Param[6], NumOfLines-StatusLine);		// Dest Y
		CheckParamVal(Param[7], NumOfColumns);			// Dest X
		CheckParamVal(Param[8], 1);				// Dest Page

		if (Param[1] > Param[3] || Param[2] > Param[4]) {
			return;
		}

		if (RelativeOrgMode) {
			Param[1] += CursorTop;
			if (Param[1] > CursorBottom) {
				Param[1] = CursorBottom + 1;
			}
			Param[3] += CursorTop;
			if (Param[3] > CursorBottom) {
				Param[3] = CursorBottom + 1;
			}
			Param[6] += CursorTop;
			if (Param[6] > CursorBottom) {
				Param[6] = CursorBottom + 1;
			}
			if (Param[6] + Param[3] - Param[1] > CursorBottom) {
				Param[3] = Param[1] + CursorBottom - Param[6] + 1;
			}

			// TODO: 左右マージンのチェックを行う。
		}

		// TODO: 1 origin になっている。0 origin に直す。
		BuffCopyBox(Param[2], Param[1], Param[4], Param[3], Param[5], Param[7], Param[6], Param[8]);
		break;

	  case 'x': // DECFRA
		RequiredParams(5);
		if (Param[1] < 32 || (Param[1] > 127 && Param[1] < 160) || Param[1] > 255) {
			return;
		}
		CheckParamVal(Param[2], NumOfLines-StatusLine);
		CheckParamVal(Param[3], NumOfColumns);
		CheckParamValMax(Param[4], NumOfLines-StatusLine);
		CheckParamValMax(Param[5], NumOfColumns);

		if (Param[2] > Param[4] || Param[3] > Param[5]) {
			return;
		}

		if (RelativeOrgMode) {
			Param[2] += CursorTop;
			if (Param[2] > CursorBottom) {
				Param[2] = CursorBottom + 1;
			}
			Param[4] += CursorTop;
			if (Param[4] > CursorBottom) {
				Param[4] = CursorBottom + 1;
			}

			// TODO: 左右マージンのチェックを行う。
		}

		BuffFillBox(Param[1], Param[3]-1, Param[2]-1, Param[5]-1, Param[4]-1);
		break;

	  case 'z': // DECERA
	  case '{': // DECSERA
		RequiredParams(4);
		CheckParamVal(Param[1], NumOfLines-StatusLine);
		CheckParamVal(Param[2], NumOfColumns);
		CheckParamValMax(Param[3], NumOfLines-StatusLine);
		CheckParamValMax(Param[4], NumOfColumns);

		if (Param[1] > Param[3] || Param[2] > Param[4]) {
			return;
		}

		if (RelativeOrgMode) {
			Param[1] += CursorTop;
			if (Param[1] > CursorBottom) {
				Param[1] = CursorBottom + 1;
			}
			Param[3] += CursorTop;
			if (Param[3] > CursorBottom) {
				Param[3] = CursorBottom + 1;
			}

			// TODO: 左右マージンのチェックを行う。
		}

		if (b == 'z') {
			BuffEraseBox(Param[2]-1, Param[1]-1, Param[4]-1, Param[3]-1);
		}
		else {
			BuffSelectiveEraseBox(Param[2]-1, Param[1]-1, Param[4]-1, Param[3]-1);
		}
		break;

	  case '}': // DECSASD
		if ((ts.TermFlag & TF_ENABLESLINE) == 0 || !StatusLine) {
			return;
		}

		switch (Param[1]) {
		  case 0:
			if (isCursorOnStatusLine) {
				MoveToMainScreen();
			}
			break;

		  case 1:
			if (!isCursorOnStatusLine) {
				MoveToStatusLine();
			}
			break;
		}
		break;

	  case '~': // DECSSDT
		if ((ts.TermFlag & TF_ENABLESLINE)==0) {
			return;
		}

		switch (Param[1]) {
		  case 0:
		  case 1:
			HideStatusLine();
			break;
		  case 2:
			if (!StatusLine) {
				ShowStatusLine(1); // show
			}
			break;
		}
		break;
	}
}

void CSQDol(BYTE b)
{
	switch (b) {
	  case 'p':
		CSDolRequestMode();
		break;
	}
}

void CSQuote(BYTE b)
{
	int i, x, y;
	switch (b) {
	  case 'w': // Enable Filter Rectangle (DECEFR)
		if (MouseReportMode == IdMouseTrackDECELR) {
			RequiredParams(4);
			if (DecLocatorFlag & DecLocatorPixel) {
				x = LastX + 1;
				y = LastY + 1;
			}
			else {
				DispConvWinToScreen(LastX, LastY, &x, &y, NULL);
				x++;
				y++;
			}
			FilterTop    = (Param[1]==0)? y : Param[1];
			FilterLeft   = (Param[2]==0)? x : Param[2];
			FilterBottom = (Param[3]==0)? y : Param[3];
			FilterRight  = (Param[4]==0)? x : Param[4];
			if (FilterTop > FilterBottom) {
				i = FilterTop; FilterTop = FilterBottom; FilterBottom = i;
			}
			if (FilterLeft > FilterRight) {
				i = FilterLeft; FilterLeft = FilterRight; FilterRight = i;
			}
			DecLocatorFlag |= DecLocatorFiltered;
			DecLocatorReport(IdMouseEventMove, 0);
		}
		break;

	  case 'z': // Enable DEC Locator reporting (DECELR)
		switch (Param[1]) {
		  case 0:
			if (MouseReportMode == IdMouseTrackDECELR) {
				MouseReportMode = IdMouseTrackNone;
			}
			break;
		  case 1:
			if (ts.MouseEventTracking) {
				MouseReportMode = IdMouseTrackDECELR;
				DecLocatorFlag &= ~DecLocatorOneShot;
			}
			break;
		  case 2:
			if (ts.MouseEventTracking) {
				MouseReportMode = IdMouseTrackDECELR;
				DecLocatorFlag |= DecLocatorOneShot;
			}
			break;
		}
		if (NParam > 1 && Param[2] == 1) {
			DecLocatorFlag |= DecLocatorPixel;
		}
		else {
			DecLocatorFlag &= ~DecLocatorPixel;
		}
		break;

	  case '{': // Select Locator Events (DECSLE)
		for (i=1; i<=NParam; i++) {
			switch (Param[i]) {
			  case 0:
				DecLocatorFlag &= ~(DecLocatorButtonUp | DecLocatorButtonDown | DecLocatorFiltered);
				break;
			  case 1:
				DecLocatorFlag |= DecLocatorButtonDown;
				break;
			  case 2:
				DecLocatorFlag &= ~DecLocatorButtonDown;
				break;
			  case 3:
				DecLocatorFlag |= DecLocatorButtonUp;
				break;
			  case 4:
				DecLocatorFlag &= ~DecLocatorButtonUp;
				break;
			}
		}
		break;

	  case '|': // Request Locator Position (DECRQLP)
		DecLocatorReport(IdMouseEventCurStat, 0);
		break;
	}
}

void CSSpace(BYTE b) {
	switch (b) {
	  case 'q': // DECSCUSR
		if (ts.WindowFlag & WF_CURSORCHANGE) {
			switch (Param[1]) {
			  case 0:
			  case 1:
				ts.CursorShape = IdBlkCur;
				ts.NonblinkingCursor = FALSE;
				break;
			  case 2:
				ts.CursorShape = IdBlkCur;
				ts.NonblinkingCursor = TRUE;
				break;
			  case 3:
				ts.CursorShape = IdHCur;
				ts.NonblinkingCursor = FALSE;
				break;
			  case 4:
				ts.CursorShape = IdHCur;
				ts.NonblinkingCursor = TRUE;
				break;
			  case 5:
				ts.CursorShape = IdVCur;
				ts.NonblinkingCursor = FALSE;
				break;
			  case 6:
				ts.CursorShape = IdVCur;
				ts.NonblinkingCursor = TRUE;
				break;
			  default:
				return;
			}
			ChangeCaret();
		}
		break;
	}
}

void CSAster(BYTE b)
{
	switch (b) {
	  case 'x': // DECSACE
		switch (Param[1]) {
		  case 0:
		  case 1:
			RectangleMode = FALSE;
			break;
		  case 2:
			RectangleMode = TRUE;
			break;
		}
		break;
	}
}

void PrnParseCS(BYTE b) // printer mode
{
	ParseMode = ModeFirst;
	switch (ICount) {
	  /* no intermediate char */
	  case 0:
		switch (Prv) {
		/* no private parameter */
		  case 0:
			switch (b) {
			  case 'i':
				if (Param[1]==4) {
					PrinterMode = FALSE;
					// clear prn buff
					WriteToPrnFile(PrintFile_, 0,FALSE);
					if (! AutoPrintMode) {
						ClosePrnFile(PrintFile_, PrintFileFinish);
					}
					return;
				}
				break;
			} /* of case Prv=0 */
			break;
		}
		break;
	  /* one intermediate char */
	  case 1: break;
	} /* of case Icount */

	WriteToPrnFile(PrintFile_, b,TRUE);
}

static void ParseCS(BYTE b) /* b is the final char */
{
	if (PrinterMode) { // printer mode
		PrnParseCS(b);
		return;
	}

	switch (ICount) {
	  case 0: /* no intermediate char */
		switch (Prv) {
		  case 0: /* no private parameter */
			switch (b) {
			// ISO/IEC 6429 / ECMA-48 Sequence
			  case '@': CSInsertCharacter(); break;   // ICH
			  case 'A': CSCursorUp(TRUE); break;      // CUU
			  case 'B': CSCursorDown(TRUE); break;    // CUD
			  case 'C': CSCursorRight(TRUE); break;   // CUF
			  case 'D': CSCursorLeft(TRUE); break;    // CUB
			  case 'E': CSCursorDown1(); break;       // CNL
			  case 'F': CSCursorUp1(); break;         // CPL
			  case 'G': CSMoveToColumnN(); break;     // CHA
			  case 'H': CSMoveToXY(); break;          // CUP
			  case 'I': CSForwardTab(); break;        // CHT
			  case 'J': CSScreenErase(); break;       // ED
			  case 'K': CSLineErase(); break;         // EL
			  case 'L': CSInsertLine(); break;        // IL
			  case 'M': CSDeleteNLines(); break;      // DL
//			  case 'N': break;                        // EF   -- Not support
//			  case 'O': break;                        // EA   -- Not support
			  case 'P': CSDeleteCharacter(); break;   // DCH
//			  case 'Q': break;                        // SEE  -- Not support
//			  case 'R': break;                        // CPR  -- Report only, ignore.
			  case 'S': CSScrollUp(); break;          // SU
			  case 'T': CSScrollDown(); break;        // SD
//			  case 'U': break;                        // NP   -- Not support
//			  case 'V': break;                        // PP   -- Not support
//			  case 'W': break;                        // CTC  -- Not support
			  case 'X': CSEraseCharacter(); break;    // ECH
//			  case 'Y': break;                        // CVT  -- Not support
			  case 'Z': CSBackwardTab(); break;       // CBT
//			  case '[': break;                        // SRS  -- Not support
//			  case '\\': break;                       // PTX  -- Not support
//			  case ']': break;                        // SDS  -- Not support
//			  case '^': break;                        // SIMD -- Not support
			  case '`': CSMoveToColumnN(); break;     // HPA
			  case 'a': CSCursorRight(FALSE); break;  // HPR
			  case 'b': CSRepeatCharacter(); break;   // REP
			  case 'c': AnswerTerminalType(); break;  // DA
			  case 'd': CSMoveToLineN(); break;       // VPA
			  case 'e': CSCursorDown(FALSE); break;   // VPR
			  case 'f': CSMoveToXY(); break;          // HVP
			  case 'g': CSDeleteTabStop(); break;     // TBC
			  case 'h': CS_h_Mode(); break;           // SM
			  case 'i': CS_i_Mode(); break;           // MC
			  case 'j': CSCursorLeft(FALSE); break;   // HPB
			  case 'k': CSCursorUp(FALSE); break;     // VPB
			  case 'l': CS_l_Mode(); break;           // RM
			  case 'm': CSSetAttr(); break;           // SGR
			  case 'n': CS_n_Mode(); break;           // DSR
//			  case 'o': break;                        // DAQ  -- Not support

			// Private Sequence
			  case 'r': CSSetScrollRegion(); break;   // DECSTBM
			  case 's':
				if (LRMarginMode)
					CSSetLRScrollRegion();    // DECSLRM
				else
					SaveCursor();             // SCP (Save cursor (ANSI.SYS/SCO?))
				break;
			  case 't': CSSunSequence(); break;       // DECSLPP / Window manipulation(dtterm?)
			  case 'u': RestoreCursor(); break;       // RCP (Restore cursor (ANSI.SYS/SCO))
			}
			break; /* end of case Prv=0 */
		  case '<': CSLT(b); break;    /* private parameter = '<' */
		  case '=': CSEQ(b); break;    /* private parameter = '=' */
		  case '>': CSGT(b); break;    /* private parameter = '>' */
		  case '?': CSQuest(b); break; /* private parameter = '?' */
		} /* end of switch (Prv) */
		break; /* end of no intermediate char */
	  case 1: /* one intermediate char */
		switch (Prv) {
		  case 0:
			switch (IntChar[1]) {
			  case ' ': CSSpace(b); break;  /* intermediate char = ' ' */
			  case '!': CSExc(b); break;    /* intermediate char = '!' */
			  case '"': CSDouble(b); break; /* intermediate char = '"' */
			  case '$': CSDol(b); break;    /* intermediate char = '$' */
			  case '*': CSAster(b); break;  /* intermediate char = '*' */
			  case '\'': CSQuote(b); break; /* intermediate char = '\'' */
			}
			break; /* end of case Prv=0 */
		  case '?':
			switch (IntChar[1]) {
			  case '$': CSQDol(b); break;    /* intermediate char = '$' */
			}
			break; /* end of case Prv=0 */
		} /* end of switch (Prv) */
		break; /* end of one intermediate char */
	} /* end of switch (Icount) */

	ParseMode = ModeFirst;
}

void ControlSequence(BYTE b)
{
	if ((b<=US) || (b>=0x80) && (b<=0x9F))
		ParseControl(b); /* ctrl char */
	else if ((b>=0x40) && (b<=0x7E))
		ParseCS(b); /* terminate char */
	else {
		if (PrinterMode)
			WriteToPrnFile(PrintFile_, b,FALSE);

		if ((b>=0x20) && (b<=0x2F)) { /* intermediate char */
			if (ICount<IntCharMax) ICount++;
			IntChar[ICount] = b;
		}
		else if ((b>=0x30) && (b<=0x39)) { /* parameter value */
#define ParamIncr(p, b) \
	do { \
		unsigned int ptmp; \
		if ((p) != (int)UINT_MAX) { \
			ptmp = (unsigned int)(p); \
			if (ptmp > UINT_MAX / 10 || ptmp * 10 > UINT_MAX - (b - 0x30)) { \
				(p) = (int)UINT_MAX; \
			} \
			else { \
				(p) = (int)(ptmp * 10 + b - 0x30); \
			} \
		} \
	} while (0);
			if (NSParam[NParam] > 0) {
				ParamIncr(SubParam[NParam][NSParam[NParam]], b);
			}
			else {
				ParamIncr(Param[NParam], b);
			}
		}
		else if (b==0x3A) { /* ':' Subparameter delimiter */
			if (NSParam[NParam] < NSParamMax) {
				NSParam[NParam]++;
				SubParam[NParam][NSParam[NParam]] = 0;
			}
		}
		else if (b==0x3B) { /* ';' Parameter delimiter */
			if (NParam < NParamMax) {
				NParam++;
				Param[NParam] = 0;
				NSParam[NParam] = 0;
			}
		}
		else if ((b>=0x3C) && (b<=0x3F)) { /* private char */
			if (FirstPrm) Prv = b;
		}
		else if (b>0xA0) {
			ParseMode=ModeFirst;
			ParseFirst(b);
		}
	}
	FirstPrm = FALSE;
}

int CheckUTF8Seq(BYTE b, int utf8_stat)
{
	if (ts.Language == IdUtf8 || (ts.Language==IdJapanese && ts.KanjiCode==IdUTF8)) {
		if (utf8_stat > 0) {
			if (b >= 0x80 && b < 0xc0) {
				utf8_stat -= 1;
			}
			else { // Invalid UTF-8 sequence
				utf8_stat = 0;
			}
		}
		else if (b < 0xc0) {
			; // nothing to do
		}
		else if (b < 0xe0) { // 2byte sequence
			utf8_stat = 1;
		}
		else if (b < 0xf0) { // 3byte sequence
			utf8_stat = 2;
		}
		else if (b < 0xf8) { // 4byte sequence
			utf8_stat = 3;
		}
	}
	return utf8_stat;
}

void IgnoreString(BYTE b)
{
	static int utf8_stat = 0;

	if ((ESCFlag && (b=='\\')) ||
	    (b<=US && b!=ESC && b!=HT) ||
	    (b==ST && ts.KanjiCode!=IdSJIS && utf8_stat == 0)) {
		ParseMode = ModeFirst;
	}

	if (b==ESC) {
		ESCFlag = TRUE;
	}
	else {
		ESCFlag = FALSE;
	}

	utf8_stat = CheckUTF8Seq(b, utf8_stat);
}

static void RequestStatusString(const unsigned char *StrBuff, int StrLen)	// DECRQSS
{
	char RepStr[256];
	int len = 0;
	int tmp = 0;

	switch (StrBuff[0]) {
	  case ' ':
		switch (StrBuff[1]) {
		  case 'q': // DECSCUSR
			switch (ts.CursorShape) {
			  case IdBlkCur:
				tmp = 1;
				break;
			  case IdHCur:
				tmp = 3;
				break;
			  case IdVCur:
				tmp = 5;
				break;
			  default:
				tmp = 1;
			}
			if (ts.NonblinkingCursor) {
				tmp++;
			}
			len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r%d q", CLocale, tmp);
		}
		break;
	  case '"':
		switch (StrBuff[1]) {
		  case 'p': // DECSCL
			if (VTlevel > 1 && Send8BitMode) {
				len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r6%d;0\"p", CLocale, VTlevel);
			}
			else {
				len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r6%d;1\"p", CLocale, VTlevel);
			}
			break;

		  case 'q': // DECSCA
			if (CharAttr.Attr2 & Attr2Protect) {
				len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r1\"q", CLocale);
			}
			else {
				len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r0\"q", CLocale);
			}
			break;
		}
		break;
	  case '*':
		switch (StrBuff[1]) {
		  case 'x': // DECSACE
			len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r%d*x", CLocale, RectangleMode?2:0);
			break;
		}
		break;
	  case 'm':	// SGR
		if (StrBuff[1] == 0) {
			len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r0", CLocale);
			if (CharAttr.Attr & AttrBold) {
				len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";1", CLocale);
			}
			if (CharAttr.Attr & AttrUnder) {
				len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";4", CLocale);
			}
			if (CharAttr.Attr & AttrBlink) {
				len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";5", CLocale);
			}
			if (CharAttr.Attr & AttrReverse) {
				len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";7", CLocale);
			}
			if (CharAttr.Attr2 & Attr2Fore && ts.ColorFlag & CF_ANSICOLOR) {
				int color = CharAttr.Fore;
				if (color <= 7 && (CharAttr.Attr & AttrBold) && (ts.ColorFlag & CF_PCBOLD16)) {
					color += 8;
				}

				if (color <= 7) {
					len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";3%d", CLocale, color);
				}
				else if (color <= 15) {
					if (ts.ColorFlag & CF_AIXTERM16) {
						len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";9%d", CLocale, color-8);
					}
					else if (ts.ColorFlag & CF_XTERM256) {
						len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";38;5;%d", CLocale, color);
					}
					else if (ts.ColorFlag & CF_PCBOLD16) {
						len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";3%d", CLocale, color-8);
					}
				}
				else if (ts.ColorFlag & CF_XTERM256) {
					len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";38;5;%d", CLocale, color);
				}
			}
			if (CharAttr.Attr2 & Attr2Back && ts.ColorFlag & CF_ANSICOLOR) {
				int color = CharAttr.Back;
				if (color <= 7 && (CharAttr.Attr & AttrBlink) && (ts.ColorFlag & CF_PCBOLD16)) {
					color += 8;
				}
				if (color <= 7) {
					len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";4%d", CLocale, color);
				}
				else if (color <= 15) {
					if (ts.ColorFlag & CF_AIXTERM16) {
						len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";10%d", CLocale, color-8);
					}
					else if (ts.ColorFlag & CF_XTERM256) {
						len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";48;5;%d", CLocale, color);
					}
					else if (ts.ColorFlag & CF_PCBOLD16) {
						len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";4%d", CLocale, color-8);
					}
				}
				else if (ts.ColorFlag & CF_XTERM256) {
					len += _snprintf_s_l(&RepStr[len], sizeof(RepStr) - len, _TRUNCATE, ";48;5;%d", CLocale, color);
				}
			}
			RepStr[len++] = 'm';
			RepStr[len] = 0;
		}
		break;
	  case 'r':	// DECSTBM
		if (StrBuff[1] == 0) {
			len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r%d;%dr", CLocale, CursorTop+1, CursorBottom+1);
		}
		break;
	  case 's':	// DECSLRM
		if (StrBuff[1] == 0) {
			len = _snprintf_s_l(RepStr, sizeof(RepStr), _TRUNCATE, "1$r%d;%ds", CLocale, CursorLeftM+1, CursorRightM+1);
		}
		break;
	}
	if (len == 0) {
		if (strncpy_s(RepStr, sizeof(RepStr), "0$r", _TRUNCATE)) {
			return;
		}
		len = 3;
	}
	if (ts.TermFlag & TF_INVALIDDECRPSS) {
		if (RepStr[0] == '0') {
			RepStr[0] = '1';
		}
		else {
			RepStr[0] = '0';
		}
	}
	SendDCSstr(RepStr, len);
}

int toHexStr(unsigned char *buff, int buffsize, unsigned char *str)
{
	int len, i, copylen = 0;
	unsigned char c;

	len = strlen(str);

	if (buffsize < len*2) {
		return -1;
	}

	for (i=0; i<len; i++) {
		c = str[i] >> 4;
		if (c <= 9) {
			c += '0';
		}
		else {
			c += 'a' - 10;
		}
		buff[copylen++] = c;

		c = str[i] & 0xf;
		if (c <= 9) {
			c += '0';
		}
		else {
			c += 'a' - 10;
		}
		buff[copylen++] = c;
	}

	return copylen;
}

static int TermcapString(unsigned char *buff, int buffsize, unsigned char *capname)
{
	int len = 0, l;
	unsigned char *capval = NULL;

	if (strcmp(capname, "Co") == 0 || strcmp(capname, "colors") == 0) {
		if ((ts.ColorFlag & CF_ANSICOLOR) == 0) {
			return 0;
		}

		if (ts.ColorFlag & CF_XTERM256) {
			capval = "256";
		}
		else if (ts.ColorFlag & CF_FULLCOLOR) {
			capval = "16";
		}
		else {
			capval = "8";
		}
	}

	if (capval) {
		if ((len = toHexStr(buff, buffsize, capname)) < 0) {
			return 0;
		}

		if (buffsize <= len) {
			return 0;
		}
		buff[len++] = '=';

		if ((l = toHexStr(&buff[len], buffsize-len, capval)) < 0) {
			return 0;
		}
		len += l;
	}

	return len;
}

static void RequestTermcapString(unsigned char *StrBuff, int StrLen)	// xterm experimental
{
	unsigned char RepStr[256];
	unsigned char CapName[16];
	int i, len, replen, caplen = 0;

	RepStr[0] = '1';
	RepStr[1] = '+';
	RepStr[2] = 'r';
	replen = 3;

	for (i=0; i<StrLen; i++) {
		if (StrBuff[i] == ';') {
			if (replen >= sizeof(RepStr)) {
				caplen = 0;
				break;
			}
			if (replen > 3) {
				RepStr[replen++] = ';';
			}
			if (caplen > 0 && caplen < sizeof(CapName)) {
				CapName[caplen] = 0;
				len = TermcapString(&RepStr[replen], sizeof(RepStr)-replen, CapName);
				replen += len;
				caplen = 0;
				if (len == 0) {
					break;
				}
			}
			else {
				caplen = 0;
				break;
			}
		}
		else if (i+1 < StrLen && isxdigit(StrBuff[i]) && isxdigit(StrBuff[i+1])
		  && caplen < sizeof(CapName)-1) {
			if (isdigit(StrBuff[i])) {
				CapName[caplen] = (StrBuff[i] - '0') * 16;
			}
			else {
				CapName[caplen] = ((StrBuff[i] | 0x20) - 'a' + 10) * 16;
			}
			i++;
			if (isdigit(StrBuff[i])) {
				CapName[caplen] += (StrBuff[i] - '0');
			}
			else {
				CapName[caplen] += ((StrBuff[i] | 0x20) - 'a' + 10);
			}
			caplen++;
		}
		else {
			caplen = 0;
			break;
		}
	}

	if (caplen && caplen < sizeof(CapName) && replen < sizeof(RepStr)) {
		if (replen > 3) {
			RepStr[replen++] = ';';
		}
		CapName[caplen] = 0;
		len = TermcapString(&RepStr[replen], sizeof(RepStr)-replen, CapName);
		replen += len;
	}

	if (replen == 3) {
		RepStr[0] = '0';
	}
	SendDCSstr(RepStr, replen);
}

static void ParseDCS(BYTE Cmd, unsigned char *StrBuff, int len) {
	switch (ICount) {
	  case 0:
		break;
	  case 1:
		switch (IntChar[1]) {
		  case '!':
			if (Cmd == '{') { // DECSTUI
				if (! (ts.TermFlag & TF_LOCKTUID)) {
					int i;
					for (i=0; i<8 && isxdigit(StrBuff[i]); i++) {
						if (islower(StrBuff[i])) {
							StrBuff[i] = toupper(StrBuff[i]);
						}
					}
					if (len == 8 && i == 8) {
						strncpy_s(ts.TerminalUID, sizeof(ts.TerminalUID), StrBuff, _TRUNCATE);
					}
				}
			}
			break;
		  case '$':
			if (Cmd == 'q')  { // DECRQSS
				RequestStatusString(StrBuff, len);
			}
			break;
		  case '+':
			if (Cmd == 'q') { // Request termcap/terminfo string (xterm)
				RequestTermcapString(StrBuff, len);
			}
			break;
		  default:
			break;
		}
		break;
	  default:
		break;
	}
}

#define ModeDcsFirst     1
#define ModeDcsString    2
void DeviceControl(BYTE b)
{
	static unsigned char StrBuff[256];
	static int DcsParseMode = ModeDcsFirst;
	static int StrLen;
	static int utf8_stat = 0;
	static BYTE Cmd;

	if ((ESCFlag && (b=='\\')) || (b==ST && ts.KanjiCode!=IdSJIS && utf8_stat == 0)) {
		if (DcsParseMode == ModeDcsString) {
			StrBuff[StrLen] = 0;
			ParseDCS(Cmd, StrBuff, StrLen);
		}
		ESCFlag = FALSE;
		ParseMode = ModeFirst;
		DcsParseMode = ModeDcsFirst;
		StrLen = 0;
		utf8_stat = 0;
		return;
	}

	if (b==ESC) {
		ESCFlag = TRUE;
		utf8_stat = 0;
		return;
	}
	else {
		ESCFlag = FALSE;
	}

	utf8_stat = CheckUTF8Seq(b, utf8_stat);

	switch (DcsParseMode) {
	  case ModeDcsFirst:
		if (b<=US) {
			ParseControl(b);
		}
		else if ((b>=0x20) && (b<=0x2F)) {
			if (ICount<IntCharMax) ICount++;
			IntChar[ICount] = b;
		}
		else if ((b>=0x30) && (b<=0x39)) {
			Param[NParam] = Param[NParam]*10 + b - 0x30;
		}
		else if (b==0x3B) {
			if (NParam < NParamMax) {
				NParam++;
				Param[NParam] = 0;
			}
		}
		else if ((b>=0x40) && (b<=0x7E)) {
			if (ICount == 0 && b=='|') {
				ParseMode = ModeDCUserKey;
				if (Param[1] < 1) ClearUserKey();
				WaitKeyId = TRUE;
				NewKeyId = 0;
			}
			else {
				Cmd = b;
				DcsParseMode = ModeDcsString;
			}
		}
		else {
			ParseMode = ModeIgnore;
			utf8_stat = 0;
			IgnoreString(b);
		}
		break;

	  case ModeDcsString:
		if (b <= US && b != HT && b != CR) {
			ESCFlag = FALSE;
			ParseMode = ModeFirst;
			DcsParseMode = ModeDcsFirst;
			StrLen = 0;
		}
		else if (StrLen < sizeof(StrBuff)-1) {
			StrBuff[StrLen++] = b;
		}
		break;
	}
}

void DCUserKey(BYTE b)
{
	static int utf8_stat = 0;

	if (ESCFlag && (b=='\\') || (b==ST && ts.KanjiCode!=IdSJIS && utf8_stat == 0)) {
		if (! WaitKeyId) DefineUserKey(NewKeyId,NewKeyStr,NewKeyLen);
		ESCFlag = FALSE;
		ParseMode = ModeFirst;
		return;
	}

	if (b==ESC) {
		ESCFlag = TRUE;
		return;
	}
	else ESCFlag = FALSE;

	utf8_stat = CheckUTF8Seq(b, utf8_stat);

	if (WaitKeyId) {
		if ((b>=0x30) && (b<=0x39)) {
			if (NewKeyId<1000)
				NewKeyId = NewKeyId*10 + b - 0x30;
		}
		else if (b==0x2F) {
			WaitKeyId = FALSE;
			WaitHi = TRUE;
			NewKeyLen = 0;
		}
	}
	else {
		if (b==0x3B) {
			DefineUserKey(NewKeyId,NewKeyStr,NewKeyLen);
			WaitKeyId = TRUE;
			NewKeyId = 0;
		}
		else {
			if (NewKeyLen < FuncKeyStrMax) {
				if (WaitHi) {
					NewKeyStr[NewKeyLen] = ConvHexChar(b) << 4;
					WaitHi = FALSE;
				}
				else {
					NewKeyStr[NewKeyLen] = NewKeyStr[NewKeyLen] + ConvHexChar(b);
					WaitHi = TRUE;
					NewKeyLen++;
				}
			}
		}
	}
}

BOOL XsParseColor(char *colspec, COLORREF *color)
{
	unsigned int r, g, b;
//	double dr, dg, db;

	r = g = b = 255;

	if (colspec == NULL || color == NULL) {
		return FALSE;
	}

	if (_strnicmp(colspec, "rgb:", 4) == 0) {
		switch (strlen(colspec)) {
		  case  9:	// rgb:R/G/B
			if (sscanf(colspec, "rgb:%1x/%1x/%1x", &r, &g, &b) != 3) {
				return FALSE;
			}
			r *= 17; g *= 17; b *= 17;
			break;
		  case 12:	// rgb:RR/GG/BB
			if (sscanf(colspec, "rgb:%2x/%2x/%2x", &r, &g, &b) != 3) {
				return FALSE;
			}
			break;
		  case 15:	// rgb:RRR/GGG/BBB
			if (sscanf(colspec, "rgb:%3x/%3x/%3x", &r, &g, &b) != 3) {
				return FALSE;
			}
			r >>= 4; g >>= 4; b >>= 4;
			break;
		  case 18:	// rgb:RRRR/GGGG/BBBB
			if (sscanf(colspec, "rgb:%4x/%4x/%4x", &r, &g, &b) != 3) {
				return FALSE;
			}
			r >>= 8; g >>= 8; b >>= 8;
			break;
		  default:
			return FALSE;
		}
	}
//	else if (_strnicmp(colspec, "rgbi:", 5) == 0) {
//		; /* nothing to do */
//	}
	else if (colspec[0] == '#') {
		switch (strlen(colspec)) {
		  case  4:	// #RGB
			if (sscanf(colspec, "#%1x%1x%1x", &r, &g, &b) != 3) {
				return FALSE;
			}
			r <<= 4; g <<= 4; b <<= 4;
			break;
		  case  7:	// #RRGGBB
			if (sscanf(colspec, "#%2x%2x%2x", &r, &g, &b) != 3) {
				return FALSE;
			}
			break;
		  case 10:	// #RRRGGGBBB
			if (sscanf(colspec, "#%3x%3x%3x", &r, &g, &b) != 3) {
				return FALSE;
			}
			r >>= 4; g >>= 4; b >>= 4;
			break;
		  case 13:	// #RRRRGGGGBBBB
			if (sscanf(colspec, "#%4x%4x%4x", &r, &g, &b) != 3) {
				return FALSE;
			}
			r >>= 8; g >>= 8; b >>= 8;
			break;
		  default:
			return FALSE;
		}
	}
	else {
		return FALSE;
	}

	if (r > 255 || g > 255 || b > 255) {
		return FALSE;
	}

	*color = RGB(r, g, b);
	return TRUE;
}

static unsigned int XtColor2TTColor(int mode, unsigned int xt_color)
{
	unsigned int colornum = CS_UNSPEC;

	switch ((mode>=100) ? mode-100 : mode) {
	  case 4:
		switch (xt_color) {
		  case 256:
			colornum = CS_VT_BOLDFG;
			break;
		  case 257:
			colornum = CS_VT_UNDERFG;
			break;
		  case 258:
			colornum = CS_VT_BLINKFG;
			break;
		  case 259:
			colornum = CS_VT_REVERSEBG;
			break;
		  case CS_UNSPEC:
			if (mode == 104) {
				colornum = CS_ANSICOLOR_ALL;
			}
			break;
		  default:
			if (xt_color <= 255) {
				colornum = xt_color;
			}
		}
		break;
	  case 5:
		switch (xt_color) {
		  case 0:
			colornum = CS_VT_BOLDFG;
			break;
		  case 1:
			colornum = CS_VT_UNDERFG;
			break;
		  case 2:
			colornum = CS_VT_BLINKFG;
			break;
		  case 3:
			colornum = CS_VT_REVERSEBG;
			break;
		  case CS_UNSPEC:
			if (mode == 105) {
				colornum = CS_SP_ALL;
			}
			break;
		}
		break;
	  case 10:
		colornum = CS_VT_NORMALFG;
		break;
	  case 11:
		colornum = CS_VT_NORMALBG;
		break;
	  case 15:
		colornum = CS_TEK_FG;
		break;
	  case 16:
		colornum = CS_TEK_BG;
		break;
	}
	return colornum;
}

static void XsProcColor(int mode, unsigned int ColorNumber, char *ColorSpec, BYTE TermChar) {
	COLORREF color;
	char StrBuff[256];
	unsigned int colornum;
	int len;

	colornum = XtColor2TTColor(mode, ColorNumber);

	if (colornum != CS_UNSPEC) {
		if (strcmp(ColorSpec, "?") == 0) {
			color = DispGetColor(colornum);
			if (mode == 4 || mode == 5) {
				len =_snprintf_s_l(StrBuff, sizeof(StrBuff), _TRUNCATE,
					"%d;%d;rgb:%04x/%04x/%04x", CLocale, mode, ColorNumber,
					GetRValue(color)*257, GetGValue(color)*257, GetBValue(color)*257);
			}
			else {
				len =_snprintf_s_l(StrBuff, sizeof(StrBuff), _TRUNCATE,
					"%d;rgb:%04x/%04x/%04x", CLocale, mode,
					GetRValue(color)*257, GetGValue(color)*257, GetBValue(color)*257);
			}
			SendOSCstr(StrBuff, len, TermChar);
		}
		else if (XsParseColor(ColorSpec, &color)) {
			DispSetColor(colornum, color);
		}
	}
}

static void XsProcClipboard(PCHAR buff)
{
	char *p;
	wchar_t *notify_buff, *notify_title;

	p = buff;
	while (strchr("cps01234567", *p)) {
		p++;
	}

	if (*p++ == ';') {
		if (*p == '?' && *(p+1) == 0) { // Read access
			if (ts.CtrlFlag & CSF_CBREAD) {
				char hdr[20];
				if (ts.NotifyClipboardAccess) {
					GetI18nStrWW("Tera Term", "MSG_CBACCESS_TITLE",
								 L"Clipboard Access", ts.UILanguageFileW, &notify_title);
					GetI18nStrWW("Tera Term", "MSG_CBACCESS_READ",
								 L"Remote host reads clipboard contents.", ts.UILanguageFileW, &notify_buff);
					NotifyInfoMessageW(&cv, notify_buff, notify_title);
					free(notify_title);
					free(notify_buff);
				}
				strncpy_s(hdr, sizeof(hdr), "\033]52;", _TRUNCATE);
				if (strncat_s(hdr, sizeof(hdr), buff, p - buff) == 0) {
					CBStartPasteB64(HVTWin, hdr, "\033\\");
				}
			}
			else if (ts.NotifyClipboardAccess) {
				GetI18nStrWW("Tera Term", "MSG_CBACCESS_REJECT_TITLE",
				             L"Rejected Clipboard Access", ts.UILanguageFileW, &notify_title);
				GetI18nStrWW("Tera Term", "MSG_CBACCESS_READ_REJECT",
				             L"Reject clipboard read access from remote.", ts.UILanguageFileW, &notify_buff);
				NotifyWarnMessageW(&cv, notify_buff, notify_title);
				free(notify_title);
				free(notify_buff);
			}
		}
		else if (ts.CtrlFlag & CSF_CBWRITE) { // Write access
			size_t len = strlen(buff);
			size_t blen = len * 3 / 4 + 1;
			char *cbbuff = malloc(blen);
			len = b64decode(cbbuff, blen, p);
			if (len < 0 || len >= blen) {
				free(cbbuff);
				return;
			}
			cbbuff[len] = 0;

			if (ts.NotifyClipboardAccess) {
				wchar_t *buf;
				GetI18nStrWW("Tera Term", "MSG_CBACCESS_TITLE",
				             L"Clipboard Access", ts.UILanguageFileW, &notify_title);
				GetI18nStrWW("Tera Term", "MSG_CBACCESS_WRITE",
							 L"Remote host wirtes clipboard.", ts.UILanguageFileW, &buf);
				aswprintf(&notify_buff, L"%s\n--\n%hs", buf, cbbuff);
				NotifyInfoMessageW(&cv, notify_buff, notify_title);
				free(buf);
				free(notify_title);
				free(notify_buff);
			}

			// cbbuff に入っている文字列をクリップボードにセットする
			{
				// wchar_t へ変換して設定
				//	とりあえずUTF-8 が入っている前提
				// 	TODO 受信文字コードに合わせて変更すればok?
				wchar_t *cbbuffW = ToWcharU8(cbbuff);
				CBSetTextW(NULL, cbbuffW, 0);
				free(cbbuffW);
			}
			free(cbbuff);
		}
		else if (ts.NotifyClipboardAccess) {
			GetI18nStrWW("Tera Term", "MSG_CBACCESS_REJECT_TITLE",
			             L"Rejected Clipboard Access", ts.UILanguageFileW, &notify_title);
			GetI18nStrWW("Tera Term", "MSG_CBACCESS_WRITE_REJECT",
						 L"Reject clipboard write access from remote.", ts.UILanguageFileW, &notify_buff);
			NotifyWarnMessageW(&cv, notify_buff, notify_title);
			free(notify_title);
			free(notify_buff);
		}
	}
}


// タイトルバーのCP932への変換を行う
// 現在、SJIS、EUCのみに対応。
// (2005.3.13 yutaka)
static void ConvertToCP932(char *str, int destlen)
{
#define IS_SJIS(n) (ts.KanjiCode == IdSJIS && IsDBCSLeadByte(n))
#define IS_EUC(n) (ts.KanjiCode == IdEUC && (n & 0x80))
	extern WORD PASCAL JIS2SJIS(WORD KCode);
	size_t len = strlen(str);
	char *cc = _alloca(len + 1);
	char *c = cc;
	size_t i;
	unsigned char b;
	WORD word;

	if (ts.Language == IdJapanese) {
		for (i = 0 ; i < len ; i++) {
			b = str[i];
			if (IS_SJIS(b) || IS_EUC(b)) {
				word = b<<8;

				if (i == len - 1) {
					*c++ = b;
					continue;
				}

				b = str[i + 1];
				word |= b;
				i++;

				if (ts.KanjiCode == IdSJIS) {
					// SJISはそのままCP932として出力する

				} else if (ts.KanjiCode == IdEUC) {
					// EUC -> SJIS
					word &= ~0x8080;
					word = JIS2SJIS(word);

				} else if (ts.KanjiCode == IdJIS) {

				} else if (ts.KanjiCode == IdUTF8) {

				} else {

				}

				*c++ = word >> 8;
				*c++ = word & 0xff;

			} else {
				*c++ = b;
			}
		}

		*c = '\0';
		strncpy_s(str, destlen, cc, _TRUNCATE);
	}
}

static void XSequence(BYTE b)
{
	static char *StrBuff = NULL;
	static unsigned int StrLen = 0, StrBuffSize = 0;
	static int utf8_stat = 0;
	static char realloc_failed = FALSE;
	static BOOL ESCflag = FALSE, HasParamStr = FALSE;
	char *p, *color_spec;
	unsigned int new_size;
	int color_num;
	BYTE TermChar;

	TermChar = 0;

	if (ESCflag) {
		ESCflag = FALSE;
		if (b == '\\') {
			TermChar = ST;
		}
		else {	// Invalid Sequence
			ParseMode = ModeIgnore;
			HasParamStr = FALSE;
			IgnoreString(b);
			return;
		}
	}
	else if (b == BEL) {
		TermChar = BEL;
	}
	else if (b==ST && Accept8BitCtrl && !(ts.Language==IdJapanese && ts.KanjiCode==IdSJIS) && utf8_stat==0) {
		TermChar = ST;
	}

	if (TermChar) {
		if (StrBuff) {
			if (StrLen < StrBuffSize) {
				StrBuff[StrLen] = '\0';
			}
			else {
				StrBuff[StrBuffSize-1] = '\0';
			}
		}
		switch (Param[1]) {
		  case 0: /* Change window title and icon name */
		  case 1: /* Change icon name */
		  case 2: /* Change window title */
			if (StrBuff && ts.AcceptTitleChangeRequest) {
				if (ts.KanjiCode == IdUTF8 || ts.Language == IdUtf8) {
					char *titleTmp;

					titleTmp = ToCharU8(StrBuff);
					if (titleTmp) {
						strncpy_s(cv.TitleRemote, sizeof(cv.TitleRemote), titleTmp, _TRUNCATE);
						free(titleTmp);
					}
				}
				else {
					strncpy_s(cv.TitleRemote, sizeof(cv.TitleRemote), StrBuff, _TRUNCATE);
					// (2006.6.15 maya) タイトルに渡す文字列をSJISに変換
					ConvertToCP932(cv.TitleRemote, sizeof(cv.TitleRemote));
				}
				ChangeTitle();
			}
			break;
		  case 4: /* Change/Query color palette */
		  case 5: /* Change/Query special color */
			if (StrBuff) {
				color_num = 0;
				color_spec = NULL;
				for (p = StrBuff; *p; p++) {
					if (color_spec == NULL) {
						if (isdigit(*p)) {
							color_num = color_num * 10 + *p - '0';
						}
						else if (*p == ';') {
							color_spec = p+1;
						}
						else {
							break;
						}
					}
					else {
						if (*p == ';') {
							*p = '\0';
							XsProcColor(Param[1], color_num, color_spec, TermChar);
							color_num = 0;
							color_spec = NULL;
						}
					}
				}
				if (color_spec) {
					XsProcColor(Param[1], color_num, color_spec, TermChar);
				}
			}
			break;
		  case 10: /* Change/Query VT-Window foreground color */
		  case 11: /* Change/Query VT-Window background color */
		  case 12: /* Change/Query VT-Window cursor color */
		  case 13: /* Change/Query mouse cursor foreground color */
		  case 14: /* Change/Query mouse cursor background color */
		  case 15: /* Change/Query Tek-Window foreground color */
		  case 16: /* Change/Query Tek-Window foreground color */
		  case 17: /* Change/Query highlight background color */
		  case 18: /* Change/Query Tek-Window cursor color */
		  case 19: /* Change/Query highlight foreground color */
			if (StrBuff) {
				color_num = Param[1];
				color_spec = StrBuff;
				for (p = StrBuff; *p; p++) {
					if (*p == ';') {
						*p = '\0';
						XsProcColor(color_num, 0, color_spec, TermChar);
						color_num++;
						color_spec = p+1;
					}
				}
				XsProcColor(color_num, 0, color_spec, TermChar);
			}
			break;
		  case 52: /* Manipulate Clipboard data */
			if (StrBuff) {
				XsProcClipboard(StrBuff);
			}
			break;
		  case 104: /* Reset color palette */
		  case 105: /* Reset special color */
			if (HasParamStr) {
				if (StrBuff) {
					color_num = 0;
					for (p = StrBuff; *p; p++) {
						if (isdigit(*p)) {
							color_num = color_num * 10 + *p - '0';
						}
						else if (*p == ';') {
							DispResetColor(XtColor2TTColor(Param[1], color_num));
							color_num = 0;
						}
						else {
							color_num = CS_UNSPEC;
						}
					}
					if (color_num != CS_UNSPEC) {
						DispResetColor(XtColor2TTColor(Param[1], color_num));
					}
				}
			}
			else {
				DispResetColor(XtColor2TTColor(Param[1], CS_UNSPEC));
			}
			break;
		  case 110: /* Reset VT-Window foreground color */
		  case 111: /* Reset VT-Window background color */
		  case 112: /* Reset VT-Window cursor color */
		  case 113: /* Reset mouse cursor foreground color */
		  case 114: /* Reset mouse cursor background color */
		  case 115: /* Reset Tek-Window foreground color */
		  case 116: /* Reset Tek-Window foreground color */
		  case 117: /* Reset highlight background color */
		  case 118: /* Reset Tek-Window cursor color */
		  case 119: /* Reset highlight foreground color */
			DispResetColor(XtColor2TTColor(Param[1], CS_UNSPEC));
			if (HasParamStr && StrBuff) {
				color_num = 0;
				for (p = StrBuff; *p; p++) {
					if (isdigit(*p)) {
						color_num = color_num * 10 + *p - '0';
					}
					else if (*p == ';') {
						DispResetColor(XtColor2TTColor(color_num, CS_UNSPEC));
						color_num = 0;
					}
					else {
						color_num = CS_UNSPEC;
						break;
					}
				}
				if (color_num != CS_UNSPEC) {
					DispResetColor(XtColor2TTColor(color_num, CS_UNSPEC));
				}
			}
			break;
		}
		if (StrBuff) {
			StrBuff[0] = '\0';
			StrLen = 0;
		}
		ParseMode = ModeFirst;
		HasParamStr = FALSE;
		utf8_stat = 0;
	}
	else if (b == ESC) {
		ESCflag = TRUE;
		utf8_stat = 0;
	}
	else if (b <= US) { // Invalid Character
		ParseMode = ModeFirst;
		HasParamStr = FALSE;
		utf8_stat = 0;
	}
	else if (HasParamStr) {
		utf8_stat = CheckUTF8Seq(b, utf8_stat);
		if (StrLen + 1 < StrBuffSize) {
			StrBuff[StrLen++] = b;
		}
		else if (!realloc_failed && StrBuffSize < ts.MaxOSCBufferSize) {
			if (StrBuff == NULL || StrBuffSize == 0) {
				new_size = sizeof(ts.Title);
			}
			else {
				new_size = StrBuffSize * 2;
			}
			if (new_size > ts.MaxOSCBufferSize) {
				new_size = ts.MaxOSCBufferSize;
			}

			p = realloc(StrBuff, new_size);
			if (p == NULL) {
				if (StrBuff==NULL) {
					StrBuffSize = 0;
					ParseMode = ModeIgnore;
					HasParamStr = FALSE;
					IgnoreString(b);
					return;
				}
				realloc_failed = TRUE;
			}
			else {
				StrBuff = p;
				StrBuffSize = new_size;
				if (StrLen + 1 < StrBuffSize) {
					StrBuff[StrLen++] = b;
				}
			}
		}
	}
	else if (isdigit(b)) {
		Param[1] = Param[1] * 10 + b - '0';
	}
	else if (b == ';') {
		HasParamStr = TRUE;
	}
	else {
		ParseMode = ModeIgnore;
		HasParamStr = FALSE;
		IgnoreString(b);
	}
}

void DLESeen(BYTE b)
{
	ParseMode = ModeFirst;
	if (((ts.FTFlag & FT_BPAUTO)!=0) && (b=='B'))
		BPStartReceive(FALSE, TRUE); /* Auto B-Plus activation */
	ChangeEmu = -1;
}

void CANSeen(BYTE b)
{
	static int state = 0;

	if (ts.FTFlag & FT_ZAUTO) {
		if (state == 0 && b == 'B') {
			state = 1;
		}
		else if (state == 1 && b == '0') {
			state = 2;
		}
		else {
			if (state == 2) {
				if (b =='0') { // ZRQINIT
					/* Auto ZMODEM activation (Receive) */
					ZMODEMStartReceive(FALSE, TRUE);
				}
				else if (b == '1') { // ZRINIT
					/* Auto ZMODEM activation (Send) */
					ZMODEMStartSend(NULL, 0, TRUE);
				}
			}
			ParseMode = ModeFirst;
			ChangeEmu = -1;
			state = 0;
		}
	}
	else {
		ParseMode = ModeFirst;
		ChangeEmu = -1;
	}
}

/**
 *	unicode(UTF-32,wchar_t)をバッファへ書き込む
 *	ログにも書き込む
 *
 *	PutChar() の UTF-32版
 */
static void PutU32(unsigned int code)
{
	unsigned short cset;
	int LineEnd;

	TCharAttr CharAttrTmp;
	CharAttrTmp = CharAttr;
	if (code <= US) {
		// U+0000 .. U+001f
		// C0制御文字, C0 Coontrols
		ParseControl(code);
		return;
	} else if ((0x80<=code) && (code<=0x9F)) {
		// U+0080 .. u+009f
		// C1制御文字, C1 Controls
		ParseControl(code);
		return;
	}

	{
		int r;
		BOOL is_update;

		// UnicodeからDEC特殊文字へのマッピング
		if (ts.UnicodeDecSpMapping) {
			cset = UTF32ToDecSp(code);
			if (((cset >> 8) & ts.UnicodeDecSpMapping) != 0) {
				code = cset & 0xff;
				CharAttrTmp.Attr |= AttrSpecial;
			}
		}

		if ((CharAttrTmp.Attr & AttrSpecial) == 0) {
			if (Special) {
				UpdateStr();
				Special = FALSE;
			}
		} else {
			if (!Special) {
				UpdateStr();
				Special = TRUE;
			}
		}

		if (CursorX > CursorRightM)
			LineEnd = NumOfColumns - 1;
		else
			LineEnd = CursorRightM;

		// Wrap処理、カーソル移動
		if (Wrap) {
			// 現在 Wrap 状態
			if (!BuffIsCombiningCharacter(CursorX, CursorY, code)) {
				// 文字コードが結合文字ではない = カーソルが移動する

				// カーソル位置に行継続アトリビュートを追加
				TCharAttr t = BuffGetCursorCharAttr(CursorX, CursorY);
				t.Attr |= AttrLineContinued;
				t.AttrEx = t.Attr;
				BuffSetCursorCharAttr(CursorX, CursorY, t);

				// 行継続アトリビュートをつける
				CharAttrTmp.Attr |= AttrLineContinued;
				CharAttrTmp.AttrEx = CharAttrTmp.Attr;

				// 次の行の行頭へ
				CarriageReturn(FALSE);
				LineFeed(LF,FALSE);
			}
		}

		// バッファに文字を入れる
		//	BuffPutUnicode()した戻り値で文字のセル数を知ることができる
		//		エラー時はカーソル位置を検討する
		is_update = FALSE;
		CharAttrTmp.AttrEx = CharAttrTmp.Attr;
	retry:
		r = BuffPutUnicode(code, CharAttrTmp, InsertMode);
		if (r == -1) {
			// 文字全角で行末、入力できない

			if (AutoWrapMode) {
				// 自動改行
				// 、wrap処理
				CharAttrTmp = CharAttr;
				CharAttrTmp.Attr |= AttrLineContinued;
				CharAttrTmp.AttrEx = CharAttrTmp.Attr | AttrPadding;
				// AutoWrapMode
				// ts.EnableContinuedLineCopy
				//if (CursorX != LineEnd){
				//&& BuffIsHalfWidthFromCode(&ts, code)) {

				// full width出力が半分出力にならないように0x20を出力
				BuffPutUnicode(0x20, CharAttrTmp, FALSE);
				CharAttrTmp.AttrEx = CharAttrTmp.AttrEx & ~AttrPadding;

				// 次の行の行頭へ
				CarriageReturn(FALSE);
				LineFeed(LF,FALSE);
			}
			else {
				// 行頭に戻す
				CursorX = 0;
			}

			//CharAttrTmp.Attr &= ~AttrLineContinued;
			goto retry;
		}
		else if (r == 0) {
			// カーソルの移動なし,結合文字,合字など
			// Wrap は変化しない
			UpdateStr();	// 「ほ」->「ぽ」など、変化することがあるので描画する
		} else if (r == 1) {
			// 半角(1セル)
			if (CursorX + 0 == CursorRightM || CursorX >= NumOfColumns - 1) {
				UpdateStr();
				Wrap = AutoWrapMode;
			} else {
				MoveRight();
				Wrap = FALSE;
			}
		} else if (r == 2) {
			// 全角(2セル)
			if (CursorX + 1 == CursorRightM || CursorX + 1 >= NumOfColumns - 1) {
				MoveRight();	// 全角の右側にカーソル移動
				UpdateStr();
				Wrap = AutoWrapMode;
			} else {
				MoveRight();
				MoveRight();
				Wrap = FALSE;
			}
		}
		else {
			assert(FALSE);
		}
	}

	// ログを出力
	OutputLogUTF32(code);
}

/**
 *	1byte目チェック
 */
static BOOL CheckFirstByte(BYTE b, int lang, int kanji_code)
{
	switch (lang) {
		case IdKorean:
			return __ismbblead(b, 51949);
		case IdChinese:
			if (kanji_code == IdCnGB2312) {
				return __ismbblead(b, 936);
			}
			else if (ts.KanjiCode == IdCnBig5) {
				return __ismbblead(b, 950);
			}
			break;
		default:
			assert(FALSE);
			break;
	}
	assert(FALSE);
	return FALSE;
}

/**
 *	ts.Language == IdJapanese 時
 *	1byte目チェック
 */
static BOOL CheckKanji(BYTE b)
{
	BOOL Check;

	if (ts.Language!=IdJapanese)
		return FALSE;

	ConvJIS = FALSE;

	if (ts.KanjiCode==IdSJIS ||
	   (ts.FallbackToCP932 && ts.KanjiCode==IdUTF8)) {
		if ((0x80<b) && (b<0xa0) || (0xdf<b) && (b<0xfd)) {
			Fallbacked = TRUE;
			return TRUE; // SJIS kanji
		}
		if ((0xa1<=b) && (b<=0xdf)) {
			return FALSE; // SJIS katakana
		}
	}

	if ((b>=0x21) && (b<=0x7e)) {
		Check = (Gn[Glr[0]]==IdKanji);
		ConvJIS = Check;
	}
	else if ((b>=0xA1) && (b<=0xFE)) {
		Check = (Gn[Glr[1]]==IdKanji);
		if (ts.KanjiCode==IdEUC) {
			Check = TRUE;
		}
		else if (ts.KanjiCode==IdJIS && ((ts.TermFlag & TF_FIXEDJIS)!=0) && (ts.JIS7Katakana==0)) {
			Check = FALSE; // 8-bit katakana
		}
		ConvJIS = Check;
	}
	else {
		Check = FALSE;
	}

	return Check;
}

static BOOL ParseFirstJP(BYTE b)
// returns TRUE if b is processed
//  (actually allways returns TRUE)
{
	if (KanjiIn) {
		if ((! ConvJIS) && (0x3F<b) && (b<0xFD) ||
		      ConvJIS && ( (0x20<b) && (b<0x7f) ||
		                   (0xa0<b) && (b<0xff) ))
		{
			PutKanji(b);
			KanjiIn = FALSE;
			return TRUE;
		}
		else if ((ts.TermFlag & TF_CTRLINKANJI)==0) {
			KanjiIn = FALSE;
		}
		else if ((b==CR) && Wrap) {
			CarriageReturn(FALSE);
			LineFeed(LF,FALSE);
			Wrap = FALSE;
		}
	}

	if (SSflag) {
		if (Gn[GLtmp] == IdKanji) {
			Kanji = b << 8;
			KanjiIn = TRUE;
			SSflag = FALSE;
			return TRUE;
		}
		else if (Gn[GLtmp] == IdKatakana) {
			b = b | 0x80;
		}

		PutChar(b);
		SSflag = FALSE;
		return TRUE;
	}

	if ((!EUCsupIn) && (!EUCkanaIn) && (!KanjiIn) && CheckKanji(b)) {
		Kanji = b << 8;
		KanjiIn = TRUE;
		return TRUE;
	}

	if (b<=US) {
		ParseControl(b);
	}
	else if (b==0x20) {
		PutChar(b);
	}
	else if ((b>=0x21) && (b<=0x7E)) {
		if (EUCsupIn) {
			EUCcount--;
			EUCsupIn = (EUCcount==0);
			return TRUE;
		}

		if ((Gn[Glr[0]] == IdKatakana) || EUCkanaIn) {
			b = b | 0x80;
			EUCkanaIn = FALSE;
			{
				// bはsjisの半角カタカナ
				unsigned long u32 = CP932ToUTF32(b);
				PutU32(u32);
			}
			return TRUE;
		}
		PutChar(b);
	}
	else if (b==0x7f) {
		return TRUE;
	}
	else if ((b>=0x80) && (b<=0x8D)) {
		ParseControl(b);
	}
	else if (b==0x8E) { // SS2
		switch (ts.KanjiCode) {
		case IdEUC:
			if (ts.ISO2022Flag & ISO2022_SS2) {
				EUCkanaIn = TRUE;
			}
			break;
		case IdUTF8:
			PutChar('?');
			break;
		default:
			ParseControl(b);
		}
	}
	else if (b==0x8F) { // SS3
		switch (ts.KanjiCode) {
		case IdEUC:
			if (ts.ISO2022Flag & ISO2022_SS3) {
				EUCcount = 2;
				EUCsupIn = TRUE;
			}
			break;
		case IdUTF8:
			PutChar('?');
			break;
		default:
			ParseControl(b);
		}
	}
	else if ((b>=0x90) && (b<=0x9F)) {
		ParseControl(b);
	}
	else if (b==0xA0) {
		PutChar(0x20);
	}
	else if ((b>=0xA1) && (b<=0xFE)) {
		if (EUCsupIn) {
			EUCcount--;
			EUCsupIn = (EUCcount==0);
			return TRUE;
		}

		if ((Gn[Glr[1]] != IdASCII) ||
		    (ts.KanjiCode==IdEUC) && EUCkanaIn ||
		    (ts.KanjiCode==IdSJIS) ||
		    (ts.KanjiCode==IdJIS) &&
		    (ts.JIS7Katakana==0) &&
		    ((ts.TermFlag & TF_FIXEDJIS)!=0)) {
			// bはsjisの半角カタカナ
			unsigned long u32 = CP932ToUTF32(b);
			PutU32(u32);
		} else {
			if (Gn[Glr[1]] == IdASCII) {
				b = b & 0x7f;
			}
			PutChar(b);
		}
		EUCkanaIn = FALSE;
	}
	else {
		PutChar(b);
	}

	return TRUE;
}

static BOOL ParseFirstKR(BYTE b)
// returns TRUE if b is processed
//  (actually allways returns TRUE)
{
	if (KanjiIn) {
		if ((0x41<=b) && (b<=0x5A) ||
		    (0x61<=b) && (b<=0x7A) ||
		    (0x81<=b) && (b<=0xFE))
		{
			PutKanji(b);
			KanjiIn = FALSE;
			return TRUE;
		}
		else if ((ts.TermFlag & TF_CTRLINKANJI)==0) {
			KanjiIn = FALSE;
		}
		else if ((b==CR) && Wrap) {
			CarriageReturn(FALSE);
			LineFeed(LF,FALSE);
			Wrap = FALSE;
		}
	}

	if ((!KanjiIn) && CheckFirstByte(b, ts.Language, ts.KanjiCode)) {
		Kanji = b << 8;
		KanjiIn = TRUE;
		return TRUE;
	}

	if (b<=US) {
		ParseControl(b);
	}
	else if (b==0x20) {
		PutChar(b);
	}
	else if ((b>=0x21) && (b<=0x7E)) {
//		if (Gn[Glr[0]] == IdKatakana) {
//			b = b | 0x80;
//		}
		PutChar(b);
	}
	else if (b==0x7f) {
		return TRUE;
	}
	else if ((0x80<=b) && (b<=0x9F)) {
		ParseControl(b);
	}
	else if (b==0xA0) {
		PutChar(0x20);
	}
	else if ((b>=0xA1) && (b<=0xFE)) {
		if (Gn[Glr[1]] == IdASCII) {
			b = b & 0x7f;
		}
		PutChar(b);
	}
	else {
		PutChar(b);
	}

	return TRUE;
}

static BOOL ParseFirstCn(BYTE b)
// returns TRUE if b is processed
//  (actually allways returns TRUE)
{
	if (KanjiIn) {
		// TODO
		if ((0x40<=b) && (b<=0x7e) ||
		    (0xa1<=b) && (b<=0xFE))
		{
			PutKanji(b);
			KanjiIn = FALSE;
			return TRUE;
		}
		else if ((ts.TermFlag & TF_CTRLINKANJI)==0) {
			KanjiIn = FALSE;
		}
		else if ((b==CR) && Wrap) {
			CarriageReturn(FALSE);
			LineFeed(LF,FALSE);
			Wrap = FALSE;
		}
	}

	if ((!KanjiIn) && CheckFirstByte(b, ts.Language, ts.KanjiCode)) {
		Kanji = b << 8;
		KanjiIn = TRUE;
		return TRUE;
	}

	if (b<=US) {
		ParseControl(b);
	}
	else if (b==0x20) {
		PutChar(b);
	}
	else if ((b>=0x21) && (b<=0x7E)) {
//		if (Gn[Glr[0]] == IdKatakana) {
//			b = b | 0x80;
//		}
		PutChar(b);
	}
	else if (b==0x7f) {
		return TRUE;
	}
	else if ((0x80<=b) && (b<=0x9F)) {
		ParseControl(b);
	}
	else if (b==0xA0) {
		PutChar(0x20);
	}
	else if ((b>=0xA1) && (b<=0xFE)) {
		if (Gn[Glr[1]] == IdASCII) {
			b = b & 0x7f;
		}
		PutChar(b);
	}
	else {
		PutChar(b);
	}

	return TRUE;
}

static void ParseASCII(BYTE b)
{
	if (SSflag) {
		PutChar(b);
		SSflag = FALSE;
		return;
	}

	if (b<=US) {
		ParseControl(b);
	} else if ((b>=0x20) && (b<=0x7E)) {
		//Kanji = 0;
		//PutKanji(b);
		PutChar(b);
	} else if ((b==0x8E) || (b==0x8F)) {
		PutChar('?');
	} else if ((b>=0x80) && (b<=0x9F)) {
		ParseControl(b);
	} else if (b>=0xA0) {
		//Kanji = 0;
		//PutKanji(b);
		PutChar(b);
	}
}

// UTF-8で受信データを処理する
// returns TRUE if b is processed
//  (actually allways returns TRUE)
static BOOL ParseFirstUTF8(BYTE b)
{
	static BYTE buf[4];
	static int count = 0;

	unsigned int code;

	if (ts.FallbackToCP932 && Fallbacked) {
		return ParseFirstJP(b);
	}

	// UTF-8エンコード
	//	Unicode					1byte,		  2byte,	   3byte, 		  4byte
	//	U+0000  ... U+007f		0x00 .. 0x7f
	//	U+0080  ... U+07ff		0xc2 .. 0xdf, 0x80 .. 0xbf
	//	U+0800  ... U+ffff		0xe0 .. 0xef, 0x80 .. 0xbf, 0x80 .. 0xbf
	//	U+10000 ... U+10ffff	0xf0 .. 0xf4, 0x80 .. 0xbf, 0x80 .. 0xbf, 0x80 .. 0xbf
	// UTF-8でデコードできない場合
	//	- 1byte目
	//		- C1(0x80 - 0x9f)
	//		- 0xa0 - 0xc1
	//		- 0xf5 - 0xff
	//	- 2byte目以降
	//		- 0x00 - 0x7f
	//		--0xc0 - 0xff

	if ((b & 0x80) != 0x80 || ((b & 0xe0) == 0x80 && count == 0)) {
		// 1バイト目および2バイト目がASCIIの場合は、すべてASCII出力とする。
		// 1バイト目がC1制御文字(0x80-0x9f)の場合も同様。

		// 入力文字が 0x00 ... 0x7f
		if (count == 0 || count == 1) {
			if (count == 1) {
				ParseASCII(buf[0]);
			}
			ParseASCII(b);
			count = 0;  // reset counter
			return TRUE;
		}
	}

	buf[count++] = b;
	if (count < 2) {
		return TRUE;
	}

	// 2バイトコードの場合
	if ((buf[0] & 0xe0) == 0xc0) {
		if ((buf[1] & 0xc0) == 0x80) {

			code = ((buf[0] & 0x1f) << 6);
			code |= ((buf[1] & 0x3f));

			PutU32(code);
		}
		else {
			ParseASCII(buf[0]);
			ParseASCII(buf[1]);
		}
		count = 0;
		return TRUE;
	}

	if (count < 3) {
		return TRUE;
	}

	if ((buf[0] & 0xf0) == 0xe0 &&
		(buf[1] & 0xc0) == 0x80 &&
		(buf[2] & 0xc0) == 0x80) { // 3バイトコードの場合

		// UTF-8 BOM(Byte Order Mark)
		if (buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) {
			goto skip;
		}

		code = ((buf[0] & 0xf) << 12);
		code |= ((buf[1] & 0x3f) << 6);
		code |= ((buf[2] & 0x3f));

		PutU32(code);

skip:
		count = 0;

	}

	if (count < 4) {
		return TRUE;
	}

	if ((buf[0] & 0xf8) == 0xf0 &&
		(buf[1] & 0xc0) == 0x80 &&
		(buf[2] & 0xc0) == 0x80 &&
		(buf[3] & 0xc0) == 0x80)
	{	// 4バイトコードの場合
		code = ((buf[0] & 0x07) << 18);
		code |= ((buf[1] & 0x3f) << 12);
		code |= ((buf[2] & 0x3f) << 6);
		code |= (buf[3] & 0x3f);

		PutU32(code);
		count = 0;
		return TRUE;
	} else {
		ParseASCII(buf[0]);
		ParseASCII(buf[1]);
		ParseASCII(buf[2]);
		ParseASCII(buf[3]);
		count = 0;
	}

	return TRUE;
}


static BOOL ParseFirstRus(BYTE b)
// returns if b is processed
{
	if (b>=128) {
		PutChar(b);
		return TRUE;
	}
	return FALSE;
}

static BOOL ParseEnglish(BYTE b)
{
	unsigned short u16 = 0;
	int part = KanjiCodeToISO8859Part(ts.KanjiCode);
	int r = UnicodeFromISO8859(part, b, &u16);
	if (r == 0) {
		return FALSE;
	}
	if (u16 < 0x100) {
		ParseASCII((BYTE)u16);
	}
	else {
		PutU32(u16);
	}
	return TRUE;
}

static void ParseFirst(BYTE b)
{
	switch (ts.Language) {
	  case IdUtf8:
		  ParseFirstUTF8(b);
		return;

	  case IdJapanese:
		switch (ts.KanjiCode) {
		  case IdUTF8:
			  if (ParseFirstUTF8(b)) {
				return;
			}
			break;
		  default:
			if (ParseFirstJP(b))  {
				return;
			}
		}
		break;

	  case IdKorean:
		switch (ts.KanjiCode) {
		  case IdUTF8:
			if (ParseFirstUTF8(b)) {
				return;
			}
			break;
		  default:
			if (ParseFirstKR(b))  {
				return;
			}
		}
		break;

	  case IdRussian:
		switch (ts.KanjiCode) {
		case IdUTF8:
			if (ParseFirstUTF8(b)) {
				return;
			}
			break;
		default:
			if (ParseFirstRus(b)) {
				return;
			}
		}
		break;

	case IdChinese:
		switch (ts.KanjiCode) {
		case IdUTF8:
			if (ParseFirstUTF8(b)) {
				return;
			}
			break;
		default:
			if (ParseFirstCn(b)) {
				return;
			}
		}
		break;
	case IdEnglish: {
		if (ParseEnglish(b)) {
			return;
		}
		break;
	}
	}

	if (SSflag) {
		PutChar(b);
		SSflag = FALSE;
		return;
	}

	if (b<=US)
		ParseControl(b);
	else if ((b>=0x20) && (b<=0x7E))
		PutChar(b);
	else if ((b>=0x80) && (b<=0x9F))
		ParseControl(b);
	else if (b>=0xA0)
		PutChar(b);
}

/**
 *	1byteよみだし
 *	ただし次の場合、読み出しを行わない
 *		- macro送信バッファに余裕がない
 *		- ログバッファに余裕がない
 *
 */
static int CommRead1Byte_(PComVar cv, LPBYTE b)
{
	if (DDELog && DDEGetCount() >= InBuffSize - 10) {
		/* バッファに余裕がない場合 */
		Sleep(1);
		return 0;
	}

	if (FLogIsOpend() && FLogGetFreeCount() < FILESYS_LOG_FREE_SPACE) {
		// 自分のバッファに余裕がない場合は、CPUスケジューリングを他に回し、
		// CPUがストールするの防ぐ。
		// (2006.10.13 yutaka)
		Sleep(1);
		return 0;
	}

	return CommRead1Byte(cv, b);
}

int VTParse()
{
	BYTE b;
	int c;

	c = CommRead1Byte_(&cv,&b);

	if (c==0) return 0;

	CaretOff();
	UpdateCaretPosition(FALSE);	// 非アクティブの場合のみ再描画する

	ChangeEmu = 0;

	/* Get Device Context */
	DispInitDC();

	LockBuffer();

	while ((c>0) && (ChangeEmu==0)) {
#if defined(DEBUG_DUMP_INPUTCODE)
		{
			static DWORD prev_tick;
			DWORD now = GetTickCount();
			if (prev_tick == 0) prev_tick = now;
			if (now - prev_tick > 1*1000) {
				printf("\n");
				prev_tick = now;
			}
			printf("%02x(%c) ", b, isprint(b) ? b : '.');
		}
#endif
		if (DebugFlag!=DEBUG_FLAG_NONE)
			PutDebugChar(b);
		else {
			switch (ParseMode) {
			case ModeFirst:
				ParseFirst(b);
				break;
			case ModeESC:
				EscapeSequence(b);
				break;
			case ModeDCS:
				DeviceControl(b);
				break;
			case ModeDCUserKey:
				DCUserKey(b);
				break;
			case ModeSOS:
				IgnoreString(b);
				break;
			case ModeCSI:
				ControlSequence(b);
				break;
			case ModeXS:
				XSequence(b);
				break;
			case ModeDLE:
				DLESeen(b);
				break;
			case ModeCAN:
				CANSeen(b);
				break;
			case ModeIgnore:
				IgnoreString(b);
				break;
			default:
				ParseMode = ModeFirst;
				ParseFirst(b);
			}
		}

		PrevCharacter = b;		// memorize previous character for AUTO CR/LF-receive mode

		if (LastPutCharacter != b && !(ParseMode == ModeESC || ParseMode == ModeCSI)) {
			LastPutCharacter = 0;
		}

		if (ChangeEmu==0)
			c = CommRead1Byte_(&cv,&b);
	}

	BuffUpdateScroll();

	BuffSetCaretWidth();
	UnlockBuffer();

	/* release device context */
	DispReleaseDC();

	CaretOn();

	if (ChangeEmu > 0)
		ParseMode = ModeFirst;

	return ChangeEmu;
}

int MakeLocatorReportStr(char *buff, size_t buffsize, int event, int x, int y) {
	if (x < 0) {
		return _snprintf_s_l(buff, buffsize, _TRUNCATE, "%d;%d&w", CLocale, event, ButtonStat);
	}
	else {
		return _snprintf_s_l(buff, buffsize, _TRUNCATE, "%d;%d;%d;%d;0&w", CLocale, event, ButtonStat, y, x);
	}
}

BOOL DecLocatorReport(int Event, int Button) {
	int x, y, MaxX, MaxY, len = 0;
	char buff[24];

	if (DecLocatorFlag & DecLocatorPixel) {
		x = LastX + 1;
		y = LastY + 1;
		DispConvScreenToWin(NumOfColumns+1, NumOfLines+1, &MaxX, &MaxY);
		if (x < 1 || x > MaxX || y < 1 || y > MaxY) {
			x = -1;
		}
	}
	else {
		DispConvWinToScreen(LastX, LastY, &x, &y, NULL);
		x++; y++;
		if (x < 1 || x > NumOfColumns || y < 1 || y > NumOfLines) {
			x = -1;
		}
	}

	switch (Event) {
	case IdMouseEventCurStat:
		if (MouseReportMode == IdMouseTrackDECELR) {
			len = MakeLocatorReportStr(buff, sizeof(buff), 1, x, y);
		}
		else {
			len = _snprintf_s_l(buff, sizeof(buff), _TRUNCATE, "0&w", CLocale);
		}
		break;

	case IdMouseEventBtnDown:
		if (DecLocatorFlag & DecLocatorButtonDown) {
			len = MakeLocatorReportStr(buff, sizeof(buff), Button*2+2, x, y);
		}
		break;

	case IdMouseEventBtnUp:
		if (DecLocatorFlag & DecLocatorButtonUp) {
			len = MakeLocatorReportStr(buff, sizeof(buff), Button*2+3, x, y);
		}
		break;

	case IdMouseEventMove:
		if (DecLocatorFlag & DecLocatorFiltered) {
			if (y < FilterTop || y > FilterBottom || x < FilterLeft || x > FilterRight) {
				len = MakeLocatorReportStr(buff, sizeof(buff), 10, x, y);
				DecLocatorFlag &= ~DecLocatorFiltered;
			}
		}
		break;
	}

	if (len == 0) {
		return FALSE;
	}

	SendCSIstr(buff, len);

	if (DecLocatorFlag & DecLocatorOneShot) {
		MouseReportMode = IdMouseTrackNone;
	}
	return TRUE;
}

#define MOUSE_POS_LIMIT (255 - 32)
#define MOUSE_POS_EXT_LIMIT (2047 - 32)

int MakeMouseReportStr(char *buff, size_t buffsize, int mb, int x, int y) {
	char tmpx[3], tmpy[3];

	switch (MouseReportExtMode) {
	case IdMouseTrackExtNone:
		if (x >= MOUSE_POS_LIMIT) x = MOUSE_POS_LIMIT;
		if (y >= MOUSE_POS_LIMIT) y = MOUSE_POS_LIMIT;
		return _snprintf_s_l(buff, buffsize, _TRUNCATE, "M%c%c%c", CLocale, mb+32, x+32, y+32);
		break;

	case IdMouseTrackExtUTF8:
		if (x >= MOUSE_POS_EXT_LIMIT) x = MOUSE_POS_EXT_LIMIT;
		if (y >= MOUSE_POS_EXT_LIMIT) y = MOUSE_POS_EXT_LIMIT;
		x += 32;
		y += 32;
		if (x < 128) {
			tmpx[0] = x;
			tmpx[1] = 0;
		}
		else {
			tmpx[0] = (x >> 6) & 0x1f | 0xc0;
			tmpx[1] = x & 0x3f | 0x80;
			tmpx[2] = 0;
		}
		if (y < 128) {
			tmpy[0] = y;
			tmpy[1] = 0;
		}
		else {
			tmpy[0] = (x >> 6) & 0x1f | 0xc0;
			tmpy[1] = y & 0x3f | 0x80;
			tmpy[2] = 0;
		}
		return _snprintf_s_l(buff, buffsize, _TRUNCATE, "M%c%s%s", CLocale, mb+32, tmpx, tmpy);
		break;

	case IdMouseTrackExtSGR:
		return _snprintf_s_l(buff, buffsize, _TRUNCATE, "<%d;%d;%d%c", CLocale, mb&0x7f, x, y, (mb&0x80)?'m':'M');
		break;

	case IdMouseTrackExtURXVT:
		return _snprintf_s_l(buff, buffsize, _TRUNCATE, "%d;%d;%dM", CLocale, mb+32, x, y);
		break;
	}
	buff[0] = 0;
	return 0;
}

BOOL MouseReport(int Event, int Button, int Xpos, int Ypos) {
	char Report[32];
	int x, y, len, modifier;
	static int LastSendX = -1, LastSendY = -1, LastButton = IdButtonRelease;

	len = 0;

	switch (Event) {
	case IdMouseEventBtnDown:
		ButtonStat |= (8>>(Button+1));
		break;
	case IdMouseEventBtnUp:
		ButtonStat &= ~(8>>(Button+1));
		break;
	}
	LastX = Xpos;
	LastY = Ypos;

	if (MouseReportMode == IdMouseTrackNone)
		return FALSE;

	if (ts.DisableMouseTrackingByCtrl && ControlKey())
		return FALSE;

	if (MouseReportMode == IdMouseTrackDECELR)
		return DecLocatorReport(Event, Button);

	DispConvWinToScreen(Xpos, Ypos, &x, &y, NULL);
	x++; y++;

	if (x < 1)
		x = 1;
	else if (x > NumOfColumns)
		x = NumOfColumns;
	if (y < 1)
		y = 1;
	else if (y > NumOfLines)
		y = NumOfLines;

	if (ShiftKey())
		modifier = 4;
	else
		modifier = 0;

	if (ControlKey())
		modifier |= 8;

	if (AltKey())
		modifier |= 16;

	modifier = (ShiftKey()?4:0) | (AltKey()?8:0) | (ControlKey()?16:0);

	switch (Event) {
	case IdMouseEventBtnDown:
		switch (MouseReportMode) {
		case IdMouseTrackX10:
			len = MakeMouseReportStr(Report, sizeof Report, Button, x, y);
			break;

		case IdMouseTrackVT200:
		case IdMouseTrackBtnEvent:
		case IdMouseTrackAllEvent:
			len = MakeMouseReportStr(Report, sizeof Report, Button | modifier, x, y);
			LastSendX = x;
			LastSendY = y;
			LastButton = Button;
			break;

		case IdMouseTrackNetTerm:
			len = _snprintf_s_l(Report, sizeof Report, _TRUNCATE, "\033}%d,%d\r", CLocale, y, x);
			CommBinaryOut(&cv, Report, len);
			return TRUE;

		case IdMouseTrackVT200Hl: /* not supported yet */
		default:
			return FALSE;
		}
		break;

	case IdMouseEventBtnUp:
		switch (MouseReportMode) {
		case IdMouseTrackVT200:
		case IdMouseTrackBtnEvent:
		case IdMouseTrackAllEvent:
			if (MouseReportExtMode == IdMouseTrackExtSGR) {
				modifier |= 128;
			}
			else {
				Button = IdButtonRelease;
			}
			len = MakeMouseReportStr(Report, sizeof Report, Button | modifier, x, y);
			LastSendX = x;
			LastSendY = y;
			LastButton = IdButtonRelease;
			break;

		case IdMouseTrackX10:     /* nothing to do */
		case IdMouseTrackNetTerm: /* nothing to do */
			return TRUE;

		case IdMouseTrackVT200Hl: /* not supported yet */
		default:
			return FALSE;
		}
		break;

	case IdMouseEventMove:
		switch (MouseReportMode) {
		case IdMouseTrackBtnEvent:
			if (LastButton == 3) {
				return FALSE;
			}
			/* FALLTHROUGH */
		case IdMouseTrackAllEvent:
			if (x == LastSendX && y == LastSendY) {
				return FALSE;
			}
			len = MakeMouseReportStr(Report, sizeof Report, LastButton | modifier | 32, x, y);
			LastSendX = x;
			LastSendY = y;
		break;

		case IdMouseTrackVT200Hl: /* not supported yet */
		case IdMouseTrackX10:     /* nothing to do */
		case IdMouseTrackVT200:   /* nothing to do */
		case IdMouseTrackNetTerm: /* nothing to do */
		default:
			return FALSE;
		}
		break;

	case IdMouseEventWheel:
		switch (MouseReportMode) {
		case IdMouseTrackVT200:
		case IdMouseTrackBtnEvent:
		case IdMouseTrackAllEvent:
			len = MakeMouseReportStr(Report, sizeof Report, Button | modifier | 64, x, y);
			break;

		case IdMouseTrackX10:     /* nothing to do */
		case IdMouseTrackVT200Hl: /* not supported yet */
		case IdMouseTrackNetTerm: /* nothing to do */
			return FALSE;
		}
		break;
	}

	if (len == 0)
		return FALSE;

	SendCSIstr(Report, len);
	return TRUE;
}

void FocusReport(BOOL focus) {
	if (!FocusReportMode)
		return;

	if (focus) {
		// Focus In
		SendCSIstr("I", 0);
	} else {
		// Focus Out
		SendCSIstr("O", 0);
	}
}

void VisualBell() {
	CSQExchangeColor();
	Sleep(10);
	CSQExchangeColor();
}

void RingBell(int type) {
	DWORD now;

	now = GetTickCount();
	if (now - BeepSuppressTime < ts.BeepSuppressTime * 1000) {
		BeepSuppressTime = now;
	}
	else {
		if (now - BeepStartTime < ts.BeepOverUsedTime * 1000) {
			if (BeepOverUsedCount <= 1) {
				BeepSuppressTime = now;
			}
			else {
				BeepOverUsedCount--;
			}
		}
		else {
			BeepStartTime = now;
			BeepOverUsedCount = ts.BeepOverUsedCount;
		}

		switch (ts.Beep) {
		  case IdBeepOff:
			/* nothing to do */
			break;
		  case IdBeepOn:
			MessageBeep(0);
			break;
		  case IdBeepVisual:
			VisualBell();
			break;
		}
	}
}

void EndTerm() {
	if (CLocale) {
		_free_locale(CLocale);
	}
	CLocale = NULL;
}

BOOL BracketedPasteMode() {
	return BracketedPaste;
}

BOOL WheelToCursorMode() {
	return AcceptWheelToCursor && AppliCursorMode && !ts.DisableAppCursor && !(ControlKey() && ts.DisableWheelToCursorByCtrl);
}

void ChangeTerminalID() {
	switch (ts.TerminalID) {
	case IdVT220J:
	case IdVT282:
		VTlevel = 2;
		break;
	case IdVT320:
	case IdVT382:
		VTlevel = 3;
		break;
	case IdVT420:
		VTlevel = 4;
		break;
	case IdVT520:
	case IdVT525:
		VTlevel = 5;
		break;
	default:
		VTlevel = 1;
	}

	if (VTlevel == 1) {
		Send8BitMode = FALSE;
	}
	else {
		Send8BitMode = ts.Send8BitCtrl;
	}
}

void TermPasteStringNoBracket(const wchar_t *str, size_t len)
{
	CommTextOutW(&cv, str, len);
	if (ts.LocalEcho) {
		CommTextEchoW(&cv, str, len);
	}
}

void TermPasteString(const wchar_t *str, size_t len)
{
	TermSendStartBracket();
	TermPasteStringNoBracket(str, len);
	TermSendEndBracket();
}

void TermSendStartBracket()
{
	if (BracketedPaste) {
		CommBinaryOut(&cv, BracketStart, BracketStartLen);
	}

	return;
}

void TermSendEndBracket()
{
	if (BracketedPaste) {
		CommBinaryOut(&cv, BracketEnd, BracketEndLen);
	}

	return;
}
