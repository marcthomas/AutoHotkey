/*
AutoHotkey

Copyright 2003-2005 Chris Mallett

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef wingroup_h
#define wingroup_h

#include "defines.h"
#include "SimpleHeap.h" // for WinGroup's overloaded new/delete operators.


// NOTE: This module is separate from the others because:
// 1) If it were in script.h/cpp it would add clutter to that already large file.
// 2) If it were in window.h/cpp it causes mutual-dependency problems between header
//    (probably correctible, but it's fine this way).


class WindowSpec
{
public:
	char *mTitle, *mText, *mExcludeTitle, *mExcludeText;
	void *mJumpToLine; // It's void to avoid making this module dependent on scrip.h.
	WindowSpec *mNextWindow;  // Next item in linked list.
	WindowSpec(char *aTitle = "", char *aText = "", void *aJumpToLine = NULL
		, char *aExcludeTitle = "", char *aExcludeText = "")
		// Caller should have allocated some dynamic memory for the given args if they're not
		// the empty string.  We just set our member variables to be equal to the given pointers.
		: mTitle(aTitle), mText(aText), mExcludeTitle(aExcludeTitle), mExcludeText(aExcludeText)
		, mJumpToLine(aJumpToLine), mNextWindow(NULL)
	{}
	void *operator new(size_t aBytes) {return SimpleHeap::Malloc(aBytes);}
	void *operator new[](size_t aBytes) {return SimpleHeap::Malloc(aBytes);}
	void operator delete(void *aPtr) {}
	void operator delete[](void *aPtr) {}
};



class WinGroup
{
private:
	// The maximum number of windows to keep track of:
	#define MAX_ALREADY_VISITED 500
	static WinGroup *sGroupLastUsed;
	static HWND *sAlreadyVisited;  // Array.  It will be dynamically allocated on first use.
	static int sAlreadyVisitedCount;
	bool mIsModeActivate;

	static void MarkAsVisited(HWND aWnd)
	{
		// For peace of mind, do dupe-check to rule out, once and for all,
		// the possibility of any unusual conditions where the list might get
		// quickly filled up with dupes:
		for (int i = 0; i < sAlreadyVisitedCount; ++i)
			if (sAlreadyVisited[i] == aWnd) // It's already in the list.
				return;
		if (sAlreadyVisitedCount >= MAX_ALREADY_VISITED)
			// Can't store any more. Don't bother displaying an error (too unlikely).
			return;
		sAlreadyVisited[sAlreadyVisitedCount++] = aWnd;
	}

	ResultType Update(bool aIsModeActivate);

public:
	char *mName;    // The name of the group.
	WindowSpec *mFirstWindow, *mLastWindow;
	WinGroup *mNextGroup;  // Next item in linked list.
	UINT mWindowCount;

	ResultType AddWindow(char *aTitle, char *aText, void *aJumpToLine, char *aExcludeTitle, char *aExcludeText);
	ResultType ActUponAll(ActionTypeType aActionType, int aTimeToWaitForClose);
	ResultType CloseAndGoToNext(bool aStartWithMostRecent);
	ResultType Activate(bool aStartWithMostRecent, WindowSpec *aWinSpec = NULL, void **aJumpToLine = NULL);
	ResultType Deactivate(bool aStartWithMostRecent);
	bool IsEmpty() {return mFirstWindow == NULL;}
	WindowSpec *IsMember(HWND aWnd);
	WinGroup(char *aGroupName)
		// The caller must ensure that aGroupName is non-null and non-empty-string.
		: mName(aGroupName) // Caller gave us a pointer to dynamic memory for this.
		, mFirstWindow(NULL), mLastWindow(NULL)
		, mWindowCount(0)
		, mNextGroup(NULL)
		, mIsModeActivate(true) // arbitrary default.
	{}
	void *operator new(size_t aBytes) {return SimpleHeap::Malloc(aBytes);}
	void *operator new[](size_t aBytes) {return SimpleHeap::Malloc(aBytes);}
	void operator delete(void *aPtr) {}
	void operator delete[](void *aPtr) {}
};


BOOL CALLBACK EnumParentFindAnyExcept(HWND aWnd, LPARAM lParam);
BOOL CALLBACK EnumParentActUponAll(HWND aWnd, LPARAM lParam);

#endif
