/*
AutoHotkey

Copyright 2003 Chris Mallett

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "stdafx.h" // pre-compiled headers
#include "keyboard.h"
#include "globaldata.h" // for g.KeyDelay
#include "application.h" // for MsgSleep()
#include "util.h"  // for strlicmp()
#include "window.h" // for MsgBox() (debug)


inline void DoKeyDelay(int aDelay = g.KeyDelay)
// A small inline to help with tracking things in our effort to track the physical
// state of the modifier keys, since GetAsyncKeyState() does not appear to be
// reliable (not properly implemented), at least on Windows XP.  UPDATE: Tracking the
// modifiers this way would require that the hotkey's modifiers be put back down between
// every key event, which can sometimes interfere with the send itself (i.e. since the ALT
// key can activate the menu bar in the foreground window).  So using the new physical
// modifier tracking method instead.
{
	if (aDelay < 0) // To support user-specified KeyDelay of -1 (fastest send rate).
		return;
	if (g_os.IsWin9x())
	{
		// Do a true sleep on Win9x because the MsgSleep() method is very inaccurate on Win9x
		// for some reason (a MsgSleep(1) causes a sleep between 10 and 55ms, for example):
		Sleep(aDelay);
		return;
	}
	SLEEP_WITHOUT_INTERRUPTION(aDelay);
}



void SendKeys(char *aKeys, HWND aTargetWindow)
// The <aKeys> string must be modifiable (not constant), since for performance reasons,
// it's allowed to be temporarily altered by this function.  mThisHotkeyModifiersLR, if non-zero,
// is the set of modifiers used to trigger the hotkey that called the subroutine
// containing the Send that got us here.  If any of those modifiers are still down,
// they will be released prior to sending the batch of keys specified in <aKeys>.
{
	if (!aKeys || !*aKeys) return;

	// Maybe best to call immediately so that the amount of time during which we haven't been pumping
	// messsages is more accurate:
	LONG_OPERATION_INIT

	modLR_type modifiersLR_current = GetModifierLRState(); // Current "logical" modifier state.

	// Make a best guess of what the physical state of the keys is prior to starting,
	// since GetAsyncKeyState() is unreliable (it seems to always report the logical vs.
	// physical state, at least under Windows XP).  Note: We're only want those physical
	// keys that are also logically down (it's possible for a key to be down physically
	// but not logically such as well R-control, for example, is a suffix hotkey and the
	// user is physically holding it down):
	modLR_type modifiersLR_down_physically_and_logically, modifiersLR_down_physically_but_not_logically;
	if (g_hhkLowLevelKeybd)
	{
		// Since hook is installed, use its more reliable tracking to determine which
		// modifiers are down.
		// Update: modifiersLR_down_physically_but_not_logically is now used to distinguish
		// between the following two cases, allowing modifiers to be properly restored to
		// the down position when the hook is installed:
		// 1) naked modifier key used only as suffix: when the user phys. presses it, it isn't
		//    logically down because the hook suppressed it.
		// 2) A modifier that is a prefix, that triggers a hotkey via a suffix, and that hotkey sends
		//    that modifier.  The modifier will go back up after the SEND, so the key will be physically
		//    down but not logically.
		modifiersLR_down_physically_but_not_logically = g_modifiersLR_physical & ~g_modifiersLR_logical;
		modifiersLR_down_physically_and_logically = g_modifiersLR_physical & g_modifiersLR_logical; // intersect
	}
	else // Use best-guess instead.
	{
		modifiersLR_down_physically_but_not_logically = 0; // There's no way of knowing, so assume none.
		// Even if TickCount has wrapped due to system being up more than about 49 days,
		// DWORD math still gives the right answer as long as g_script.mThisHotkeyStartTime
		// itself isn't more than about 49 days ago:
		if ((GetTickCount() - g_script.mThisHotkeyStartTime) < (DWORD)g_HotkeyModifierTimeout) // Elapsed time < timeout-value
			modifiersLR_down_physically_and_logically = modifiersLR_current & g_script.mThisHotkeyModifiersLR; // Bitwise AND is set intersection.
		else
			// Since too much time as passed since the user pressed the hotkey, it seems best,
			// based on the action that will occur below, to assume that no hotkey modifiers
			// are physically down:
			modifiersLR_down_physically_and_logically = 0;
	}
	// Any of the external modifiers that are down but NOT due to the hotkey are probably
	// logically down rather than physically (perhaps from a prior command such as
	// "Send, {CtrlDown}".  Since there's no way to be sure due to the unreliability of
	// GetAsyncKeyState() under XP and perhaps other OSes, it seems best to assume that
	// they are logically vs. physically down.  This value contains the modifiers that
	// we will not attempt to change (e.g. "Send, A" will not release the LWin
	// before sending "A" if this value indicates that LWin is down).  The below sets
	// the value to be all the down-keys in modifiersLR_current except any that are physically
	// down due to the hotkey itself:
	modLR_type modifiersLR_persistent = modifiersLR_current & ~modifiersLR_down_physically_and_logically;
	mod_type modifiers_persistent = ConvertModifiersLR(modifiersLR_persistent);
	// The above two variables should be kept in sync with each other from now on.

//MsgBox(GetTickCount() - g_script.mThisHotkeyStartTime);
//char mod_str[256];
//MsgBox(ModifiersLRToText(aModifiersLR, mod_str));
//MsgBox(ModifiersLRToText(modifiersLR_current, mod_str));
//MsgBox(ModifiersLRToText(modifiersLR_down_physically_and_logically, mod_str));
//MsgBox(ModifiersLRToText(modifiersLR_persistent, mod_str));

	// Might be better to do this prior to changing capslock state:
	bool threads_are_attached = false; // Set default.
	DWORD my_thread, target_thread;
	if (aTargetWindow)
	{
		my_thread  = GetCurrentThreadId();
		target_thread = GetWindowThreadProcessId(aTargetWindow, NULL);
		if (target_thread && target_thread != my_thread && !IsWindowHung(aTargetWindow))
			threads_are_attached = AttachThreadInput(my_thread, target_thread, TRUE) != 0;
	}

	// The default behavior is to turn the capslock key off prior to sending any keys
	// because otherwise lowercase letters would come through as uppercase:
	ToggleValueType prior_capslock_state;
	if (threads_are_attached || !g_os.IsWin9x())
		// Only under either of the above conditions can the state of Capslock be reliably
		// retrieved and changed:
		prior_capslock_state = g.StoreCapslockMode ? ToggleKeyState(VK_CAPITAL, TOGGLED_OFF) : TOGGLE_INVALID;
	else // OS is Win9x and threads are not attached.
	{
		// Attempt to turn off capslock, but never attempt to turn it back on because we can't
		// reliably detect whether it was on beforehand.  Update: This didn't do any good, so
		// it's disabled for now:
		//CapslockOffWin9x();
		prior_capslock_state = TOGGLE_INVALID;
	}

	char single_char_string[2];
	vk_type vk = 0;
	sc_type sc = 0;
	mod_type modifiers_for_next_key = 0;
	modLR_type key_as_modifiersLR = 0;

	for (; *aKeys; ++aKeys)
	{
		LONG_OPERATION_UPDATE_FOR_SENDKEYS
		// No, it's much better to allow literal spaces even though {SPACE} is also
		// supported:
		//if (IS_SPACE_OR_TAB(*aKeys))
		//	continue;
		switch (*aKeys)
		{
		case '^':
			if (!(modifiers_persistent & MOD_CONTROL))
				modifiers_for_next_key |= MOD_CONTROL;
			// else don't add it, because the value of modifiers_for_next_key may also used to determine
			// which keys to release after the key to which this modifier applies is sent.
			// We don't want persistent modifiers to ever be released because that's how
			// AutoIt2 behaves and it seems like a reasonable standard.
			break;
		case '+':
			if (!(modifiers_persistent & MOD_SHIFT))
				modifiers_for_next_key |= MOD_SHIFT;
			break;
		case '!':
			if (!(modifiers_persistent & MOD_ALT))
				modifiers_for_next_key |= MOD_ALT;
			break;
		case '#':
			if (g_script.mIsAutoIt2) // Since AutoIt2 ignores these, ignore them if script is in AutoIt2 mode.
				break;
			if (!(modifiers_persistent & MOD_WIN))
				modifiers_for_next_key |= MOD_WIN;
			break;
		case '}': break;  // Important that these be ignored.  Be very careful about changing this, see below.
		case '{':
		{
			char *end_pos = strchr(aKeys + 1, '}');
			if (!end_pos)
				break;  // do nothing, just ignore it and continue.
			size_t key_text_length = end_pos - aKeys - 1;
			if (!key_text_length)
			{
				if (end_pos[1] == '}')
				{
					// The literal string "{}}" has been encountered, which is interpreted as a single "}".
					++end_pos;
					key_text_length = 1;
				}
				else // Empty braces {} were encountered.
					break;  // do nothing: let it proceed to the }, which will then be ignored.
			}
			size_t key_name_length = key_text_length; // Set default.

			*end_pos = '\0';  // temporarily terminate the string here.
			UINT repeat_count = 1;
			KeyEventTypes event_type = KEYDOWNANDUP; // Set default.
			char old_char;
			char *space_pos = StrChrAny(aKeys + 1, " \t");  // Relies on the fact that {} key names contain no spaces.
			if (space_pos)
			{
				old_char = *space_pos;
				*space_pos = '\0';  // Temporarily terminate here so that TextToVK() can properly resolve a single char.
				key_name_length = space_pos - aKeys - 1; // Override the default value set above.
				char *next_word = omit_leading_whitespace(space_pos + 1);
				UINT next_word_length = (UINT)(end_pos - next_word);
				if (next_word_length > 0)
				{
					if (!stricmp(next_word, "down"))
						event_type = KEYDOWN;
					else if (!stricmp(next_word, "up"))
						event_type = KEYUP;
					else
					{
						repeat_count = ATOI(next_word);
						if (repeat_count < 0) // But seems best to allow zero itself, for possibly use with environment vars
							repeat_count = 0;
					}
				}
			}

			vk = TextToVK(aKeys + 1, &modifiers_for_next_key, true);
			sc = vk ? 0 : TextToSC(aKeys + 1);  // If sc is 0, it will be resolved by KeyEvent() later.
			if (space_pos)  // undo the temporary termination
				*space_pos = old_char;
			*end_pos = '}';  // undo the temporary termination

			if (vk || sc)
			{
				if (repeat_count)
				{
					if (key_as_modifiersLR = KeyToModifiersLR(vk, sc)) // Assign
					{
						if (event_type == KEYDOWN) // i.e. make {Shift down} have the same effect {ShiftDown}
							modifiers_persistent = ConvertModifiersLR(modifiersLR_persistent |= key_as_modifiersLR);
						else if (event_type == KEYUP)
							modifiers_persistent = ConvertModifiersLR(modifiersLR_persistent &= ~key_as_modifiersLR);
						// else must never change modifiers_persistent in response to KEYDOWNANDUP
						// because that would break existing scripts.  This is because that same
						// modifier key may have been pushed down via {ShiftDown} rather than "{Shift Down}".
						// In other words, {Shift} should never undo the effects of a prior {ShiftDown}
						// or {Shift down}.
					}
					// Below: modifiers_persistent stays in effect (pressed down) even if the key
					// being sent includes that same modifier.  Surprisingly, this is how AutoIt2
					// behaves also, which is good.  Example: Send, {AltDown}!f  ; this will cause
					// Alt to still be down after the command is over, even though F is modified
					// by Alt.
					SendKey(vk, sc, modifiers_for_next_key, modifiersLR_persistent, repeat_count
						, event_type, key_as_modifiersLR, aTargetWindow);
				}
				modifiers_for_next_key = 0;  // reset after each, and even if no valid vk was found (should be just like AutoIt).
				aKeys = end_pos;  // In prep for aKeys++ at the bottom of the loop.
				break;
			}

			// If no vk was found and the key name is of length 1, the only chance is to try sending it
			// as a special character:
			if (key_name_length == 1)
			{
				if (repeat_count)
					SendKeySpecial(aKeys[1], modifiers_for_next_key, modifiersLR_persistent, repeat_count
						, event_type, aTargetWindow);
				modifiers_for_next_key = 0;  // reset after each, and even if no valid vk was found (should be just like AutoIt).
				aKeys = end_pos;  // In prep for aKeys++ at the bottom of the loop.
				break;
			}

			// Otherwise, since no vk was found, check it against list of special keys:
			int special_key = TextToSpecial(aKeys + 1, (UINT)key_text_length, modifiersLR_persistent, modifiers_persistent);
			if (special_key)
				for (UINT i = 0; i < repeat_count; ++i)
				{
					// Don't tell it to save & restore modifiers because special keys like this one
					// should have maximum flexibility (i.e. nothing extra should be done so that the
					// user can have more control):
					KeyEvent(special_key > 0 ? KEYDOWN : KEYUP, abs(special_key), 0, aTargetWindow, true);
					LONG_OPERATION_UPDATE_FOR_SENDKEYS
				}
			else // Check if it's "{ASC nnnn}"
			{
				// Include the trailing space in "ASC " to increase uniqueness (selectivity).
				// Also, sending the ASC sequence to window doesn't work, so don't even try:
				if (key_text_length > 4 && !strnicmp(aKeys + 1, "ASC ", 4) && !aTargetWindow)
				{
					SendASC(omit_leading_whitespace(aKeys + 4), aTargetWindow); // aTargetWindow is always NULL, it's just for maintainability.
					// Do this only once at the end of the sequence:
					DoKeyDelay();
				}
			}
			// If what's between {} is unrecognized, such as {Bogus}, it's safest not to send
			// the contents literally since that's almost certainly not what the user intended.
			// In addition, reset the modifiers, since they were intended to apply only to
			// the key inside {}:
			modifiers_for_next_key = 0;
			aKeys = end_pos;  // In prep for aKeys++ below.
			break;
		}
		default:
			// Best to call this separately, rather than as first arg in SendKey, since it changes the
			// value of modifiers and the updated value is *not* guaranteed to be passed.
			// In other words, SendKey(TextToVK(...), modifiers, ...) would often send the old
			// value for modifiers.
			single_char_string[0] = *aKeys;
			single_char_string[1] = '\0';
			vk = TextToVK(single_char_string, &modifiers_for_next_key, true);
			sc = 0;
			if (vk)
				SendKey(vk, sc, modifiers_for_next_key, modifiersLR_persistent, 1, KEYDOWNANDUP, 0, aTargetWindow);
			else // Try to send it by alternate means.
				SendKeySpecial(*aKeys, modifiers_for_next_key, modifiersLR_persistent, 1, KEYDOWNANDUP, aTargetWindow);
			modifiers_for_next_key = 0;  // Safest to reset this regardless of whether a key was sent.
			// break;  Not needed in "default".
		} // switch()
	} // for()

	// Don't press back down the modifiers that were used to trigger this hotkey if there's
	// any doubt that they're still down, since doing so when they're not physically down
	// would cause them to be stuck down, which might cause unwanted behavior when the unsuspecting
	// user resumes typing:
	if (g_hhkLowLevelKeybd
		|| g_HotkeyModifierTimeout < 0 // User specified that the below should always be done.
		|| (GetTickCount() - g_script.mThisHotkeyStartTime) < (DWORD)g_HotkeyModifierTimeout) // Elapsed time < timeout-value
	{
		// If possible, update the set of modifier keys that are being physically held down.
		// This is done because the user may have released some keys during the send operation
		// (especially if KeyDelay > 0 and the Send is a large one).
		// Update: Include all keys that are phsyically down except those that were down
		// physically but not logically at the *start* of the send operation (since the send
		// operation may have changed the logical state).  In other words, we want to restore
		// the keys to their former logical-down position to match the fact that the user is still
		// holding them down physically.  The previously-down keys we don't do this for are those 
		// that were physically but not logically down, such as a naked Control key that's used
		// as a suffix without being a prefix.  See above comments for more details:
		if (g_hhkLowLevelKeybd)
			modifiersLR_down_physically_and_logically = g_modifiersLR_physical
				& ~modifiersLR_down_physically_but_not_logically; // intersect
		// Restore the state of the modifiers to be those believed to be physically held down
		// by the user.  Do not restore any that were logically "persistent", as detected upon
		// entrance to this function (e.g. due to something such as a prior "Send, {LWinDown}"),
		// since their state should already been correct if things above are designed right:
		modifiersLR_current = GetModifierLRState();
		modLR_type keys_to_press_down = modifiersLR_down_physically_and_logically & ~modifiersLR_current;
		SetModifierLRStateSpecific(keys_to_press_down, modifiersLR_current, KEYDOWN);
	}

	if (prior_capslock_state == TOGGLED_ON) // The current user setting requires us to turn it back on.
		ToggleKeyState(VK_CAPITAL, TOGGLED_ON);

	// Might be better to do this after changing capslock state, since having the threads attached
	// tends to help with updating the global state of keys (perhaps only under Win9x in this case):
	if (threads_are_attached)
		AttachThreadInput(my_thread, target_thread, FALSE);
}



int SendKey(vk_type aVK, sc_type aSC, mod_type aModifiers, modLR_type aModifiersLRPersistent
	, int aRepeatCount, KeyEventTypes aEventType, modLR_type aKeyAsModifiersLR, HWND aTargetWindow)
// vk or sc may be zero, but not both.
// Returns the number of keys actually sent for caller convenience.
// The function is reponsible for first setting the correct modifier state,
// as specified by the caller, before sending the key.  After sending,
// it should put the system's modifier state back to the way it was
// originally, except, for safely, it seems best not to put back down
// any modifiers that were originally down unless those keys are physically
// down.
{
	if (!aVK && !aSC) return 0;
	if (aRepeatCount <= 0) return aRepeatCount;

	// Maybe best to call immediately so that the amount of time during which we haven't been pumping
	// messsages is more accurate:
	LONG_OPERATION_INIT

	// I thought maybe it might be best not to release unwanted modifier keys that are already down
	// (perhaps via something like "Send, {altdown}{esc}{altup}"), but that harms the case where
	// modifier keys are down somehow, unintentionally: The send command wouldn't behave as expected.
	// e.g. "Send, abc" while the control key is held down by other means, would send ^a^b^c,
	// possibly dangerous.  So it seems best to default to making sure all modifiers are in the
	// proper down/up position prior to sending any Keybd events.  UPDATE: This has been changed
	// so that only modifiers that were actually used to trigger that hotkey are released during
	// the send.  Other modifiers that are down may be down intentially, e.g. due to a previous
	// call to send, something like Send, {ShiftDown}.
	// UPDATE: It seems best to save the initial state only once, prior to sending the key-group,
	// because only at the beginning can the original state be determined without having to
	// save and restore it in each loop iteration.
	// UPDATE: Not saving and restoring at all anymore, due to interference (side-effects)
	// caused by the extra keybd events.

	mod_type modifiers_specified = aModifiers | ConvertModifiersLR(aModifiersLRPersistent);

	for (int i = 0; i < aRepeatCount; ++i)
	{
		LONG_OPERATION_UPDATE_FOR_SENDKEYS
		// These modifiers above stay in effect for each of these keypresses.
		// Always on the first iteration, and thereafter only if the send won't be essentially
		// instantaneous.  The modifiers are checked before every key is sent because
		// if a high repeat-count was specified, the user may have time to release one or more
		// of the modifier keys that were used to trigger a hotkey.  That physical release
		// will cause a key-up event which will cause the state of the modifiers, as seen
		// by the system, to change.  Example: If user releases control-key during the operation,
		// some of the D's won't be control-D's:
		// ^c::Send,^{d 15}
		// Also: Seems best to do SetModifierState() even if Keydelay < 0:
		// Update: If this key is itself a modifier, don't change the state of the other
		// modifier keys just for it, since most of the time that is unnecessary and in
		// some cases, the extra generated keystrokes would cause complications/side-effects:
		if (!aKeyAsModifiersLR)
			SetModifierState(modifiers_specified, GetModifierLRState());
		KeyEvent(aEventType, aVK, aSC, aTargetWindow, true);
	}

	// The final iteration by the above loop does its key delay prior to us changing the
	// modifiers below.  This is a good thing because otherwise the modifiers would sometimes
	// be released so soon after the keys they modify that the modifiers are not in effect.
	// This can be seen sometimes when/ ctrl-shift-tabbing back through a multi-tabbed dialog:
	// The last ^+{tab} might otherwise not take effect because the CTRL key would be released
	// too quickly.

	// Release any modifiers that were pressed down just for the sake of the above
	// event (i.e. leave any persistent modifiers pressed down).  The caller should
	// already have verified that aModifiers does not contain any of the modifiers
	// in aModifiersLRPersistent.  Also, call GetModifierLRState() again explicitly
	// rather than trying to use a saved value from above, in case the above itself
	// changed the value of the modifiers (i.e. aVk/aSC is a modifier).  Admittedly,
	// that would be pretty strange but it seems the most correct thing to do.
	if (!aKeyAsModifiersLR) // See prior use of this var for explanation.
		SetModifierLRState(aModifiersLRPersistent, GetModifierLRState());
	return aRepeatCount;
}



int SendKeySpecial(char aChar, mod_type aModifiers, modLR_type aModifiersLRPersistent
	, int aRepeatCount, KeyEventTypes aEventType, HWND aTargetWindow)
// Returns the number of keys actually sent for caller convenience.
// This function has been adapted from the AutoIt3 source.
// This function uses some of the same code as SendKey() above, so maintain them together.
{
	if (aRepeatCount <= 0) return aRepeatCount;
	// My: It looks like the "P�" in the below belongs there, as does "��", which
	// seems to be a single unit of some kind.  Also, it seems that Alt+146 and
	// Alt+0198 (must have the zero) both yield � ... while Alt+145 and Alt+0230
	// both yield �.
	// AutoIt3: Note that the cent and yen symbol are missing and replaced with space - 
	// this will never be matched as this function will never be called with a space char

	LONG_OPERATION_INIT

	char szSpecials[] = "��������������������������� � P������Ѫ���������";
	// simulation using diadic keystroke
	char diadic[64] =
/*192 */ {	'`', '�', '^', '~', '�', '�', ' ', ' ', '`', '�', '^', '�', '`', '�', '^', '�',
/*208 */	' ', '~', '`', '�', '^', '~', '�', ' ', ' ', '`', '�', '^', '�', '�', ' ', ' ',
/*224 */	'`', '�', '^', '~', '�', '�', ' ', ' ', '`', '�', '^', '�', '`', '�', '^', '�',
/*240 */	' ', '~', '`', '�', '^', '~', '�', ' ', ' ', '`', '�', '^', '�', '�', ' ', '�'
		};
	char letter[64] =
/*192 */ {	'A', 'A', 'A', 'A', 'A', 'A', ' ', ' ', 'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I',
/*208 */	' ', 'N', 'O', 'O', 'O', 'O', 'O', ' ', ' ', 'U', 'U', 'U', 'U', 'Y', ' ', ' ',
/*224 */	'a', 'a', 'a', 'a', 'a', 'a', ' ', ' ', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
/*240 */	' ', 'n', 'o', 'o', 'o', 'o', 'o', ' ', ' ', 'u', 'u', 'u', 'u', 'y', ' ', 'y'
		};

	int pair_index;
	char ascii[8] = "", ascii_diadic[8] = "";  // Make it a string since leading zeros in it are significant.

	switch(aChar)
	{
	case '�': strlcpy(ascii, "0248", sizeof(ascii)); break;  // Must have leading zero.
	case '�': strlcpy(ascii, "0216", sizeof(ascii)); break;  // Must have leading zero.
	}

	if (!*ascii)
	{
		// AutoIt3: // simulation using {ASC nnn}
		// Only the char code between {asc 128} and {asc 175} can be sent
		char *cp = strchr(szSpecials, aChar);
		if (cp)
		{
			if (aTargetWindow)
				// For now, don't support the ASC method for sending directly to windows.
				// AutoIt3 takes a stab at supporting it, by activating the parent window I think,
				// but this seems pointless to me.  So for now, the user shouldn't be using the
				// ControlSend command to send anything that requires the ASC method:
				return 0;
			_itoa(128 + (int)(cp - szSpecials), ascii, 10);
		}
		else // ASCII codes between 192 and 255 inclusive (a total of 64 characters).
		{
			// Leave ascii as empty string to indicate to the below that the alternate method will be used instead.
			if (aChar >= '�')
			{
				pair_index = aChar - '�'; // '�' expressed as a signed char is -64.
				// Sanity check to prevent any chance of buffer underrun or overrun (probably impossible
				// given the above checks):
				if (pair_index < 0 || pair_index >= sizeof(diadic))
					return 0;
				if (diadic[pair_index] == ' ') // This is one of the ASCII codes between 192 and 256 than CAN'T be sent.
					return 0;
				// else AutoIt3: something can be try to send diadic followed by non accent char
				// The diadic itself might require the ASC method:
				if (cp = strchr(szSpecials, diadic[pair_index]))
				{
					if (aTargetWindow) // Not supported, see above.
						return 0;
					_itoa(128 + (int)(cp - szSpecials), ascii_diadic, 10);
				}
				// else leave ascii_diadic set to empty string.
			}
		}
	}

	mod_type modifiers_specified = aModifiers | ConvertModifiersLR(aModifiersLRPersistent);

	for (int i = 0; i < aRepeatCount; ++i)
	{
		LONG_OPERATION_UPDATE_FOR_SENDKEYS
		if (*ascii) // Method #1
		{
			SendASC(ascii, aTargetWindow); // aTargetWindow is always NULL, it's just for maintainability.
			DoKeyDelay();
		}
		else // Method #2
		{
			if (*ascii_diadic)
				SendASC(ascii_diadic, aTargetWindow); // aTargetWindow is always NULL, it's just for maintainability.
			else
				SendChar(diadic[pair_index], modifiers_specified, KEYDOWNANDUP, aTargetWindow);
			SendChar(letter[pair_index], modifiers_specified, aEventType, aTargetWindow);
			DoKeyDelay();
		}
	}
	SetModifierLRState(aModifiersLRPersistent, GetModifierLRState()); // See notes in SendKey().
	return aRepeatCount;
}



int SendASC(char *aAscii, HWND aTargetWindow)
// aAscii is a string to support explicit leading zeros because sending 216, for example, is not
// the same as sending 0216.
// Returns the number of keys sent (doesn't need to be exact).
{
	// This is just here to catch bugs in callers who do it wrong.  See notes in SendKeys() for explanation:
	if (aTargetWindow) return 0;

	int value = ATOI(aAscii);
	if (value < 0 || value > 255) return 0; // Sanity check.

	// Known issue: If the hotkey that triggers this Send command is CONTROL-ALT
	// (and maybe either CTRL or ALT separately, as well), the {ASC nnnn} method
	// might not work reliably due to strangeness with that OS feature, at least on
	// WinXP.  I already tried adding delays between the keystrokes and it didn't help.

	// Make sure modifier state is correct: ALT pressed down and other modifiers UP
	// because CTRL and SHIFT seem to interfere with this technique if they are down,
	// at least under WinXP (though the Windows key doesn't seem to be a problem):
	modLR_type modifiersLR_to_release = GetModifierLRState()
		& (MOD_LCONTROL | MOD_RCONTROL | MOD_LSHIFT | MOD_RSHIFT);
	if (modifiersLR_to_release)
		// Note: It seems best never to put them back down, because the act of doing so
		// may do more harm than good (i.e. the keystrokes may caused unexpected
		// side-effects:
		SetModifierLRStateSpecific(modifiersLR_to_release, GetModifierLRState(), KEYUP);

	int keys_sent = 0;  // Track this value and return it to the caller.

	if (!(GetModifierState() & MOD_ALT)) // Neither ALT key is down.
	{
		KeyEvent(KEYDOWN, VK_MENU);
		++keys_sent;
	}

	// Caller relies upon us to stop upon reaching the first non-digit character:
	for (char *cp = aAscii; *cp >= '0' && *cp <= '9'; ++cp)
	{
		// A comment from AutoIt3: ASCII 0 is 48, NUMPAD0 is 96, add on 48 to the ASCII.
		// Also, don't do WinDelay after each keypress in this case because it would make
		// such keys take up to 3 or 4 times as long to send (AutoIt3 avoids doing the
		// delay also):
		KeyEvent(KEYDOWNANDUP, *cp + 48);
		++keys_sent;
	}

	// Must release the key regardless of whether it was already down, so that the
	// sequence will take effect immediately rather than waiting for the user to
	// release the ALT key (if it's physically down).  It's the caller's responsibility
	// to put it back down if it needs to be:
	KeyEvent(KEYUP, VK_MENU);
	return ++keys_sent;
}



int SendChar(char aChar, mod_type aModifiers, KeyEventTypes aEventType, HWND aTargetWindow)
// Returns the number of keys sent (doesn't need to be exact).
{
	SHORT mod_plus_vk = VkKeyScan(aChar);
	char keyscan_modifiers = HIBYTE(mod_plus_vk);
	if (keyscan_modifiers == -1) // No translation could be made.
		return 0;

	// Combine the modifiers needed to enact this key with those that the caller wanted to be in effect:
	if (keyscan_modifiers & 0x01)
		aModifiers |= MOD_SHIFT;
	if (keyscan_modifiers & 0x02)
		aModifiers |= MOD_CONTROL;
	if (keyscan_modifiers & 0x04)
		aModifiers |= MOD_ALT;

	// It's the caller's responsibility to restore the modifiers if it needs to:
	SetModifierState(aModifiers, GetModifierLRState());
	KeyEvent(aEventType, LOBYTE(mod_plus_vk), 0, aTargetWindow, true);
	return 1;
}



ResultType KeyEvent(KeyEventTypes aEventType, vk_type aVK, sc_type aSC, HWND aTargetWindow, bool aDoKeyDelay)
// sc or vk, but not both, can be zero to indicate unspecified.
// For keys like NumpadEnter -- that have have a unique scancode but a non-unique virtual key --
// caller can just specify the sc.  In addition, the scan code should be specified for keys
// like NumpadPgUp and PgUp.  In that example, the caller would send the same scan code for
// both except that PgUp would be extended.   g_sc_to_vk would map both of them to the same
// virtual key, which is fine since it's the scan code that matters to apps that can
// differentiate between keys with the same vk.

// Later, switch to using SendInput() on OS's that support it.
{
	if (!aVK && !aSC) return FAIL;
	DWORD aExtraInfo = KEYIGNORE;  // Formerly a param, but it was never called that way so got rid of it.
	//if (aExtraInfo && aExtraInfo != KEYIGNORE)
	//	aExtraInfo = KEYIGNORE;  // In case caller called it wrong.

	// Even if the g_sc_to_vk mapping results in a zero-value vk, don't return.
	// I think it may be valid to send keybd_events	that have a zero vk.
	// In any case, it's unlikely to hurt anything:
	if (!aVK)
		aVK = g_sc_to_vk[aSC].a;
	else
		if (!aSC)
			// In spite of what the MSDN docs say, the scan code parameter *is* used, as evidenced by
			// the fact that the hook receives the proper scan code as sent by the below, rather than
			// zero like it normally would.  Even though the hook would try to use MapVirtualKey() to
			// convert zero-value scan codes, it's much better to send it here also for full compatibility
			// with any apps that may rely on scan code (and such would be the case if the hook isn't
			// active because the user doesn't need it; also for some games maybe).  In addition, if the
			// current OS is Win9x, we must map it here manually (above) because otherwise the hook
			// wouldn't be able to differentiate left/right on keys such as RCONTROL, which is detected
			// via its scan code.
			aSC = g_vk_to_sc[aVK].a;

	// Do this only after the above, so that the SC is left/right specific if the VK was such,
	// even on Win9x (though it's probably never called that way for Win9x; it's probably aways
	// called with either just the proper left/right SC or that plus the neutral VK).
	// Under WinNT/2k/XP, sending VK_LCONTROL and such result in the high-level (but not low-level
	// I think) hook receiving VK_CONTROL.  So somewhere interally it's being translated (probably
	// by keybd_event itself).  In light of this, translate the keys here manually to ensure full
	// support under Win9x (which might not support this internal translation).  The scan code
	// looked up above should still be correct for left-right centric keys even under Win9x.
	if (g_os.IsWin9x())
	{
		// Convert any non-neutral VK's to neutral for these OSes, since apps and the OS itself
		// can't be expected to support left/right specific VKs while running under Win9x:
		switch(aVK)
		{
		case VK_LCONTROL:
		case VK_RCONTROL: aVK = VK_CONTROL; break;
		case VK_LSHIFT:
		case VK_RSHIFT: aVK = VK_SHIFT; break;
		case VK_LMENU:
		case VK_RMENU: aVK = VK_MENU; break;
		}
	}

	if (aTargetWindow && KeyToModifiersLR(aVK, aSC))
	{
		// When sending modifier keystrokes directly to a window, use the AutoIt3 SetKeyboardState()
		// technique to improve the reliability of changes to modifier states.  If this is not done,
		// sometimes the state of the SHIFT key (and perhaps other modifiers) will get out-of-sync
		// with what's intended, resulting in uppercase vs. lowercase problems (and that's probably
		// just the tip of the iceberg).  For this to be helpful, our caller must have ensured that
		// our thread is attached to aTargetWindow's (but it seems harmless to do the below even if
		// that wasn't done for any reason).  Doing this here in this function rather than at a
		// higher level probably isn't best in terms of performance (e.g. in the case where more
		// than one modifier is being changed, the multiple calls to Get/SetKeyboardState() could
		// be consolidated into one call), but it is much easier to code and maintain this way
		// since many different functions might call us to change the modifier state:
		BYTE state[256];
		GetKeyboardState((PBYTE)&state);
		if (aEventType == KEYDOWN)
			state[aVK] |= 0x80;
		else if (aEventType == KEYUP)
			state[aVK] &= ~0x80;
		// else KEYDOWNANDUP, in which case it seems best (for now) not to change the state at all.
		// It's rarely if ever called that way anyway.

		// If aVK is a left/right specific key, be sure to also update the state of the neutral key:
		switch(aVK)
		{
		case VK_LCONTROL: 
		case VK_RCONTROL:
			if ((state[VK_LCONTROL] & 0x80) || (state[VK_RCONTROL] & 0x80))
				state[VK_CONTROL] |= 0x80;
			else
				state[VK_CONTROL] &= ~0x80;
			break;
		case VK_LSHIFT:
		case VK_RSHIFT:
			if ((state[VK_LSHIFT] & 0x80) || (state[VK_RSHIFT] & 0x80))
				state[VK_SHIFT] |= 0x80;
			else
				state[VK_SHIFT] &= ~0x80;
			break;
		case VK_LMENU:
		case VK_RMENU:
			if ((state[VK_LMENU] & 0x80) || (state[VK_RMENU] & 0x80))
				state[VK_MENU] |= 0x80;
			else
				state[VK_MENU] &= ~0x80;
			break;
		}

		SetKeyboardState((PBYTE)&state);
		// Even after doing the above, we still continue on to send the keystrokes
		// themselves to the window, for greater reliability (same as AutoIt3).
	}

	if (aTargetWindow)
	{
		// lowest 16 bits: repeat count: always 1 for up events, probably 1 for down in our case.
		// highest order bits: 11000000 (0xC0) for keyup, usually 00000000 (0x00) for keydown.
		LPARAM lParam = (LPARAM)(aSC << 16);
		if (aEventType != KEYUP)  // i.e. always do it for KEYDOWNANDUP
			PostMessage(aTargetWindow, WM_KEYDOWN, aVK, lParam | 0x00000001);
		if (aEventType != KEYDOWN)  // i.e. always do it for KEYDOWNANDUP
			PostMessage(aTargetWindow, WM_KEYUP, aVK, lParam | 0xC0000001);
	}
	else
	{
		if (aEventType != KEYUP)  // i.e. always do it for KEYDOWNANDUP
		{
			keybd_event(aVK
				, LOBYTE(aSC)  // naked scan code (the 0xE0 prefix, if any, is omitted)
				, (HIBYTE(aSC) ? KEYEVENTF_EXTENDEDKEY : 0)
				, aExtraInfo);

			if (aVK == VK_NUMLOCK && g_os.IsWin9x()) // Under Win9x, Numlock needs special treatment.
				ToggleNumlockWin9x();

			if (!g_hhkLowLevelKeybd) // Hook isn't logging, so we'll log just the keys we send, here.
			{
				#define UpdateKeyEventHistory(aKeyUp) \
				{\
					g_KeyHistory[g_KeyHistoryNext].vk = aVK;\
					g_KeyHistory[g_KeyHistoryNext].sc = aSC;\
					g_KeyHistory[g_KeyHistoryNext].key_up = aKeyUp;\
					g_KeyHistory[g_KeyHistoryNext].event_type = 'i';\
					g_HistoryTickNow = GetTickCount();\
					g_KeyHistory[g_KeyHistoryNext].elapsed_time = (g_HistoryTickNow - g_HistoryTickPrev) / (float)1000;\
					g_HistoryTickPrev = g_HistoryTickNow;\
					HWND fore_win = GetForegroundWindow();\
					if (fore_win)\
						GetWindowText(fore_win, g_KeyHistory[g_KeyHistoryNext].target_window, sizeof(g_KeyHistory[g_KeyHistoryNext].target_window));\
					else\
						*g_KeyHistory[g_KeyHistoryNext].target_window = '\0';\
					if (++g_KeyHistoryNext >= MAX_HISTORY_KEYS)\
						g_KeyHistoryNext = 0;\
				}
				UpdateKeyEventHistory(false);
			}
		}
		if (aEventType != KEYDOWN)  // i.e. always do it for KEYDOWNANDUP
		{
			keybd_event(aVK, LOBYTE(aSC), (HIBYTE(aSC) ? KEYEVENTF_EXTENDEDKEY : 0)
				| KEYEVENTF_KEYUP, aExtraInfo);
			if (!g_hhkLowLevelKeybd) // Hook isn't logging, so we'll log just the keys we send, here.
				UpdateKeyEventHistory(true);
		}
	}

	if (aDoKeyDelay)
		DoKeyDelay();
	return OK;
}



ToggleValueType ToggleKeyState(vk_type aVK, ToggleValueType aToggleValue)
// Toggle the given aVK into another state.  For performance, it is the caller's responsibility to
// ensure that aVK is a toggleable key such as capslock, numlock, or scrolllock.
// Returns the state the key was in before it was changed (but it's only a best-guess under Win9x).
{
	// Can't use GetAsyncKeyState() because it doesn't have this info:
	ToggleValueType starting_state = IsKeyToggledOn(aVK) ? TOGGLED_ON : TOGGLED_OFF;
	if (aToggleValue != TOGGLED_ON && aToggleValue != TOGGLED_OFF) // Shouldn't be called this way.
		return starting_state;
	if (starting_state == aToggleValue) // It's already in the desired state, so just return the state.
		return starting_state;
	if (aVK == VK_NUMLOCK)
	{
		if (g_os.IsWin9x())
		{
			// For Win9x, we want to set the state unconditionally to be sure it's right.  This is because
			// the retrieval of the Capslock state, for example, is unreliable, at least under Win98se
			// (probably due to lack of an AttachThreadInput() having been done).  Although the
			// SetKeyboardState() method used by ToggleNumlockWin9x is not required for caps & scroll lock keys,
			// it is required for Numlock:
			ToggleNumlockWin9x();
			return starting_state;  // Best guess, but might be wrong.
		}
		// Otherwise, NT/2k/XP:
		// Sending an extra up-event first seems to prevent the problem where the Numlock
		// key's indicator light doesn't change to reflect its true state (and maybe its
		// true state doesn't change either).  This problem tends to happen when the key
		// is pressed while the hook is forcing it to be either ON or OFF (or it suppresses
		// it because it's a hotkey).  Needs more testing on diff. keyboards & OSes:
		KeyEvent(KEYUP, aVK);
	}
	// Since it's not already in the desired state, toggle it:
	KeyEvent(KEYDOWNANDUP, aVK);
	return starting_state;
}



void ToggleNumlockWin9x()
// Numlock requires a special method to toggle the state and its indicator light under Win9x.
// Capslock and Scrolllock do not need this method, since keybd_event() works for them.
{
	BYTE state[256];
	GetKeyboardState((PBYTE)&state);
	state[VK_NUMLOCK] ^= 0x01;  // Toggle the low-order bit to the opposite state.
	SetKeyboardState((PBYTE)&state);
}



//void CapslockOffWin9x()
//{
//	BYTE state[256];
//	GetKeyboardState((PBYTE)&state);
//	state[VK_CAPITAL] &= ~0x01;
//	SetKeyboardState((PBYTE)&state);
//}



/*
void SetKeyState (vk_type vk, int aKeyUp)
// Later need to adapt this to support Win9x by using SetKeyboardState for those OSs.
{
	if (!vk) return;
	int key_already_up = !(GetKeyState(vk) & 0x8000);
	if ((key_already_up && aKeyUp) || (!key_already_up && !aKeyUp))
		return;
	KeyEvent(aKeyUp, vk);
}
*/



modLR_type SetModifierState(mod_type aModifiersNew, modLR_type aModifiersLRnow)
// Returns the new modifierLR state (i.e. the state after the action here has occurred).
{
	// Can't do this because the two values aren't compatible (one is LR and the other neutral):
	//if (aModifiersNew == aModifiersLRnow) return aModifiersLRnow
/*
char error_text[512];
snprintf(error_text, sizeof(error_text), "new=%02X, LRnow=%02X", aModifiersNew, aModifiersLRnow);
MsgBox(error_text);
*/
	// It's done this way in case RSHIFT, for example, is down, thus giving us the shift key
	// already without having to put the (normal/default) LSHIFT key down.
	mod_type modifiers_now = ConvertModifiersLR(aModifiersLRnow);
	modLR_type modifiersLRnew = aModifiersLRnow; // Start with what they are now.

	// If neither should be on, turn them both off.  If one should be on, turn on only one.
	// But if both are on when only one should be (rare), leave them both on:
	if ((modifiers_now & MOD_CONTROL) && !(aModifiersNew & MOD_CONTROL))
		modifiersLRnew &= ~(MOD_LCONTROL | MOD_RCONTROL);
	else if (!(modifiers_now & MOD_CONTROL) && (aModifiersNew & MOD_CONTROL))
		modifiersLRnew |= MOD_LCONTROL;
	if ((modifiers_now & MOD_ALT) && !(aModifiersNew & MOD_ALT))
		modifiersLRnew &= ~(MOD_LALT | MOD_RALT);
	else if (!(modifiers_now & MOD_ALT) && (aModifiersNew & MOD_ALT))
		modifiersLRnew |= MOD_LALT;
	if ((modifiers_now & MOD_WIN) && !(aModifiersNew & MOD_WIN))
		modifiersLRnew &= ~(MOD_LWIN | MOD_RWIN);
	else if (!(modifiers_now & MOD_WIN) && (aModifiersNew & MOD_WIN))
		modifiersLRnew |= MOD_LWIN;
	if ((modifiers_now & MOD_SHIFT) && !(aModifiersNew & MOD_SHIFT))
		modifiersLRnew &= ~(MOD_LSHIFT | MOD_RSHIFT);
	else if (!(modifiers_now & MOD_SHIFT) && (aModifiersNew & MOD_SHIFT))
		modifiersLRnew |= MOD_LSHIFT;

	return SetModifierLRState(modifiersLRnew, aModifiersLRnow);
}



modLR_type SetModifierLRState(modLR_type modifiersLRnew, modLR_type aModifiersLRnow)
{
/*
char buf[2048];
char *marker = buf;
ModifiersLRToText(aModifiersLRnow, marker);
marker += strlen(marker);
strcpy(marker, " --> ");
marker += strlen(marker);
ModifiersLRToText(modifiersLRnew, marker);
FileAppend("c:\\templog.txt", buf);
*/
	// KeyEvent() is used so that hotkeys handled by the hook
	// won't accidentally be fired off by the key events in here.  This only applies
	// to hotkeys whose suffix is a modifier key (e.g. +lwin=calc).  This won't affect
	// send cmd's ability to launch hotkeys explicitly, it only prevents them from
	// firing as a direct result of this function itself.  However, this prevention won't
	// stop normal hotkeys, such as those created by RegisterHotkey(), from firing.
	// When calling KeyEvent(), probably best not to specify a scan code unless
	// absolutely necessary, since some keyboards may have non-standard scan codes
	// which KeyEvent() will resolve into the proper vk tranlations for us.
	// Decided not to Sleep() between keystrokes, even zero, out of concern that this
	// would result in a significant delay (perhaps more than 10ms) while the system
	// is under load.

	if ((aModifiersLRnow & MOD_LCONTROL) && !(modifiersLRnew & MOD_LCONTROL))
		KeyEvent(KEYUP, VK_LCONTROL);
	else if (!(aModifiersLRnow & MOD_LCONTROL) && (modifiersLRnew & MOD_LCONTROL))
		KeyEvent(KEYDOWN, VK_LCONTROL);
	if ((aModifiersLRnow & MOD_RCONTROL) && !(modifiersLRnew & MOD_RCONTROL))
		KeyEvent(KEYUP, VK_RCONTROL);
	else if (!(aModifiersLRnow & MOD_RCONTROL) && (modifiersLRnew & MOD_RCONTROL))
		KeyEvent(KEYDOWN, VK_RCONTROL);
	
	if ((aModifiersLRnow & MOD_LALT) && !(modifiersLRnew & MOD_LALT))
		KeyEvent(KEYUP, VK_LMENU);
	else if (!(aModifiersLRnow & MOD_LALT) && (modifiersLRnew & MOD_LALT))
		KeyEvent(KEYDOWN, VK_LMENU);
	if ((aModifiersLRnow & MOD_RALT) && !(modifiersLRnew & MOD_RALT))
		KeyEvent(KEYUP, VK_RMENU);
	else if (!(aModifiersLRnow & MOD_RALT) && (modifiersLRnew & MOD_RALT))
		KeyEvent(KEYDOWN, VK_RMENU);

	// Use this to determine whether to put the shift key down temporarily
	// ourselves.  It would be bad not to check this because then, in these
	// cases, the shift key(s) would always wind up being up upon return,
	// which might violate state of the modifiers the caller wanted us to
	// set in the first place.
	bool shift_not_down_now = !((aModifiersLRnow & MOD_LSHIFT) || (aModifiersLRnow & MOD_RSHIFT));

	if ((aModifiersLRnow & MOD_LWIN) && !(modifiersLRnew & MOD_LWIN))
	{
		if (shift_not_down_now)  // Prevents Start Menu from appearing.
			KeyEvent(KEYDOWN, VK_SHIFT);
		KeyEvent(KEYUP, VK_LWIN);
		if (shift_not_down_now)
			KeyEvent(KEYUP, VK_SHIFT);
	}
	else if (!(aModifiersLRnow & MOD_LWIN) && (modifiersLRnew & MOD_LWIN))
	{
		if (shift_not_down_now)  // Prevents Start Menu from appearing.
			KeyEvent(KEYDOWN, VK_SHIFT);
		KeyEvent(KEYDOWN, VK_LWIN);
		if (shift_not_down_now)
			KeyEvent(KEYUP, VK_SHIFT);
	}
	if ((aModifiersLRnow & MOD_RWIN) && !(modifiersLRnew & MOD_RWIN))
	{
		if (shift_not_down_now)
			KeyEvent(KEYDOWN, VK_SHIFT);
		KeyEvent(KEYUP, VK_RWIN);
		if (shift_not_down_now)
			KeyEvent(KEYUP, VK_SHIFT);
	}
	else if (!(aModifiersLRnow & MOD_RWIN) && (modifiersLRnew & MOD_RWIN))
	{
		if (shift_not_down_now)
			KeyEvent(KEYDOWN, VK_SHIFT);
		KeyEvent(KEYDOWN, VK_RWIN);
		if (shift_not_down_now)
			KeyEvent(KEYUP, VK_SHIFT);
	}
	
	// Do SHIFT last because the above relies upon its prior state
	if ((aModifiersLRnow & MOD_LSHIFT) && !(modifiersLRnew & MOD_LSHIFT))
		KeyEvent(KEYUP, VK_LSHIFT);
	else if (!(aModifiersLRnow & MOD_LSHIFT) && (modifiersLRnew & MOD_LSHIFT))
		KeyEvent(KEYDOWN, VK_LSHIFT);
	if ((aModifiersLRnow & MOD_RSHIFT) && !(modifiersLRnew & MOD_RSHIFT))
		KeyEvent(KEYUP, VK_RSHIFT);
	else if (!(aModifiersLRnow & MOD_RSHIFT) && (modifiersLRnew & MOD_RSHIFT))
		KeyEvent(KEYDOWN, VK_RSHIFT);

	return modifiersLRnew;
}



void SetModifierLRStateSpecific(modLR_type aModifiersLR, modLR_type aModifiersLRnow, KeyEventTypes aKeyUp)
// Press or release only the specific keys whose bits are set to 1
// in aModifiersLR.
// Technically, there is no need to release both keys of a pair
// if both are down because all current OS's see, for example,
// that both ALT keys are UP if either one goes up, regardless
// of whether the other is still down.  But that behavior may
// change in future OS's.
// Notes for the down-version, which was previously a sep. function:
// Similar to the reasoning described in SetModifierLRStateUp.
// Previously, this only put the keys back down if the user is still
// physically holding them down (e.g. in case the Send command took a
// long time to finish, during which time the hotkey combo was released).
// However, it turns out that GetAsyncKeyState() does not work
// as advertised, at least on my XP system with the std. keyboard driver
// for an MS Natural Elite.  On mine, and possibly all other XP/2k/NT
// systems, GetAsyncKeyState() reports the key is up after
// any keybd_event() put them up, even if the key is physically down!
{
	if (aKeyUp && aKeyUp != KEYUP) aKeyUp = KEYUP;  // In case caller called it wrong.
	if (aModifiersLR & MOD_LSHIFT) KeyEvent(aKeyUp, VK_LSHIFT);
	if (aModifiersLR & MOD_RSHIFT) KeyEvent(aKeyUp, VK_RSHIFT);
	if (aModifiersLR & MOD_LCONTROL) KeyEvent(aKeyUp, VK_LCONTROL);
	if (aModifiersLR & MOD_RCONTROL) KeyEvent(aKeyUp, VK_RCONTROL);
	if (aModifiersLR & MOD_LALT) KeyEvent(aKeyUp, VK_LMENU);
	if (aModifiersLR & MOD_RALT) KeyEvent(aKeyUp, VK_RMENU);

	bool shift_not_down_now = !((aModifiersLRnow & MOD_LSHIFT) || (aModifiersLRnow & MOD_RSHIFT));
	if (aModifiersLR & MOD_LWIN)
	{
		if (shift_not_down_now)  // Prevents Start Menu from appearing.
			KeyEvent(KEYDOWN, VK_SHIFT);
		KeyEvent(aKeyUp, VK_LWIN);
		if (shift_not_down_now)
			KeyEvent(KEYUP, VK_SHIFT);
	}
	if (aModifiersLR & MOD_RWIN)
	{
		if (shift_not_down_now)  // Prevents Start Menu from appearing.
			KeyEvent(KEYDOWN, VK_SHIFT);
		KeyEvent(aKeyUp, VK_RWIN);
		if (shift_not_down_now)
			KeyEvent(KEYUP, VK_SHIFT);
	}
}



inline mod_type GetModifierState()
{
	return ConvertModifiersLR(GetModifierLRState());
}



inline modLR_type GetModifierLRState(bool aExplicitlyGet)
// Try to report a more reliable state of the modifier keys than GetKeyboardState
// alone could.
{
	// Rather than old/below method, in light of the fact that new low-level hook is being tried,
	// try relying on only the hook's tracked value rather than calling Get() (if if the hook
	// is active:
	if (g_hhkLowLevelKeybd && !aExplicitlyGet)
		return g_modifiersLR_logical;

	// I decided to call GetKeyboardState() rather than tracking the state of these keys with the
	// hook itself because that method wasn't reliable.  Hopefully, this method will always
	// report the correct physical state of the keys (unless the OS itself thinks they're stuck
	// down even when they're physically up, which seems to happen sometimes on some keyboards).
	// It's probably better to make any vars (large ones at least) static for performance reasons.
	// Normal/automatic vars have to be reallocated on the stack every time the function is called.
	// This should be safe because it now seems that a KeyboardProc() is not called re-entrantly
	// (though it is possible it's called that way sometimes?  Couldn't find an answer).
	// This is the alternate, lower-performance method.

	// Now, at the last possible moment (for performance), set the correct status for all
	// the bits in g_modifiersLR_get.

	// Use GetKeyState() rather than GetKeyboardState() because it's the only way to get
	// accurate key state when a console window is active, it seems.  I've also seen other
	// cases where GetKeyboardState() is incorrect (at least under WinXP) when GetKeyState(),
	// in its place, yields the correct info.  Very strange.

	modLR_type modifiersLR = 0;  // Allows all to default to up/off to simplify the below.
	if (g_os.IsWin9x())
	{
		// Assume it's the left key since there's no way to tell which of the pair it
		// is? (unless the hook is installed, in which case it's value would have already
		// been returned, above).
		if (IsKeyDown9x(VK_SHIFT)) modifiersLR |= MOD_LSHIFT;
		if (IsKeyDown9x(VK_CONTROL)) modifiersLR |= MOD_LCONTROL;
		if (IsKeyDown9x(VK_MENU)) modifiersLR |= MOD_LALT;
		if (IsKeyDown9x(VK_LWIN)) modifiersLR |= MOD_LWIN;
		if (IsKeyDown9x(VK_RWIN)) modifiersLR |= MOD_RWIN;
	}
	else
	{
		if (IsKeyDownNT(VK_LSHIFT)) modifiersLR |= MOD_LSHIFT;
		if (IsKeyDownNT(VK_RSHIFT)) modifiersLR |= MOD_RSHIFT;
		if (IsKeyDownNT(VK_LCONTROL)) modifiersLR |= MOD_LCONTROL;
		if (IsKeyDownNT(VK_RCONTROL)) modifiersLR |= MOD_RCONTROL;
		if (IsKeyDownNT(VK_LMENU)) modifiersLR |= MOD_LALT;
		if (IsKeyDownNT(VK_RMENU)) modifiersLR |= MOD_RALT;
		if (IsKeyDownNT(VK_LWIN)) modifiersLR |= MOD_LWIN;
		if (IsKeyDownNT(VK_RWIN)) modifiersLR |= MOD_RWIN;
	}

	return modifiersLR;

	// Only consider a modifier key to be really down if both the hook's tracking of it
	// and GetKeyboardState() agree that it should be down.  The should minimize the impact
	// of the inherent unreliability present in each method (and each method is unreliable in
	// ways different from the other).  I have verified through testing that this eliminates
	// many misfires of hotkeys.  UPDATE: Both methods are fairly reliable now due to starting
	// to send scan codes with keybd_events(), using MapVirtualKey to resolve zero-value scan
	// codes in the keyboardproc(), and using GetKeyState() rather than GetKeyboardState().
	// There are still a few cases when they don't agree, so return the bitwise-and of both
	// if the keyboard hook is active.  Bitwise and is used because generally it's safer
	// to assume a modifier key is up, when in doubt (e.g. to avoid firing unwanted hotkeys):
//	return g_hhkLowLevelKeybd ? (g_modifiersLR_logical & g_modifiersLR_get) : g_modifiersLR_get;
}



modLR_type KeyToModifiersLR(vk_type aVK, sc_type aSC, bool *pIsNeutral)
// Convert the given virtual key / scan code to its equivalent bitwise modLR value.
// Callers rely upon the fact that we convert a neutral key such as VK_SHIFT into MOD_LSHIFT,
// not the bitwise combo of MOD_LSHIFT|MOD_RSHIFT.
{
	if (pIsNeutral) *pIsNeutral = false;  // Set default for this output param, unless overridden later.
	if (!aVK && !aSC) return 0;

	if (aVK) // Have vk take precedence over any non-zero sc.
		switch(aVK)
		{
		case VK_SHIFT: if (pIsNeutral) *pIsNeutral = true; return MOD_LSHIFT;
		case VK_LSHIFT: return MOD_LSHIFT;
		case VK_RSHIFT:	return MOD_RSHIFT;
		case VK_CONTROL: if (pIsNeutral) *pIsNeutral = true; return MOD_LCONTROL;
		case VK_LCONTROL: return MOD_LCONTROL;
		case VK_RCONTROL: return MOD_RCONTROL;
		case VK_MENU: if (pIsNeutral) *pIsNeutral = true; return MOD_LALT;
		case VK_LMENU: return MOD_LALT;
		case VK_RMENU: return MOD_RALT;
		case VK_LWIN: return MOD_LWIN;
		case VK_RWIN: return MOD_RWIN;
		default: return 0;
		}
	// If above didn't return, rely on the non-zero sc instead:
	switch(aSC)
	{
	case SC_LSHIFT: return MOD_LSHIFT;
	case SC_RSHIFT:	return MOD_RSHIFT;
	case SC_LCONTROL: return MOD_LCONTROL;
	case SC_RCONTROL: return MOD_RCONTROL;
	case SC_LALT: return MOD_LALT;
	case SC_RALT: return MOD_RALT;
	case SC_LWIN: return MOD_LWIN;
	case SC_RWIN: return MOD_RWIN;
	}
	return 0;
}



modLR_type ConvertModifiers(mod_type aModifiers)
// Convert the input param to a modifiersLR value and return it.
{
	modLR_type modifiersLR = 0;
	if (aModifiers & MOD_WIN) modifiersLR |= (MOD_LWIN | MOD_RWIN);
	if (aModifiers & MOD_ALT) modifiersLR |= (MOD_LALT | MOD_RALT);
	if (aModifiers & MOD_CONTROL) modifiersLR |= (MOD_LCONTROL | MOD_RCONTROL);
	if (aModifiers & MOD_SHIFT) modifiersLR |= (MOD_LSHIFT | MOD_RSHIFT);
	return modifiersLR;
}



mod_type ConvertModifiersLR(modLR_type aModifiersLR)
// Convert the input param to a normal modifiers value and return it.
{
	mod_type modifiers = 0;
	if (aModifiersLR & MOD_LWIN || aModifiersLR & MOD_RWIN) modifiers |= MOD_WIN;
	if (aModifiersLR & MOD_LALT || aModifiersLR & MOD_RALT) modifiers |= MOD_ALT;
	if (aModifiersLR & MOD_LSHIFT || aModifiersLR & MOD_RSHIFT) modifiers |= MOD_SHIFT;
	if (aModifiersLR & MOD_LCONTROL || aModifiersLR & MOD_RCONTROL) modifiers |= MOD_CONTROL;
	return modifiers;
}



char *ModifiersLRToText(modLR_type aModifiersLR, char *aBuf)
{
	if (!aBuf) return 0;
	*aBuf = '\0';
	if (aModifiersLR & MOD_LWIN) strcat(aBuf, "LWin ");
	if (aModifiersLR & MOD_RWIN) strcat(aBuf, "RWin ");
	if (aModifiersLR & MOD_LSHIFT) strcat(aBuf, "LShift ");
	if (aModifiersLR & MOD_RSHIFT) strcat(aBuf, "RShift ");
	if (aModifiersLR & MOD_LCONTROL) strcat(aBuf, "LCtrl ");
	if (aModifiersLR & MOD_RCONTROL) strcat(aBuf, "RCtrl ");
	if (aModifiersLR & MOD_LALT) strcat(aBuf, "LAlt ");
	if (aModifiersLR & MOD_RALT) strcat(aBuf, "RAlt ");
	return aBuf;
}


//----------------------------------------------------------------------------------


void init_vk_to_sc()
{
	ZeroMemory(g_vk_to_sc, sizeof(g_vk_to_sc));  // Sets default.

	// These are mapped manually because MapVirtualKey() doesn't support them correctly, at least
	// on some -- if not all -- OSs.

	// Try to minimize the number of keys set manually because MapVirtualKey is a more reliable
	// way to get the mapping if user has a non-English or non-standard keyboard.

	// MapVirtualKey() should include 0xE0 in HIBYTE if key is extended, UPDATE: BUT IT DOESN'T.

	// Because MapVirtualKey can only accept (and return) naked scan codes (the low-order byte),
	// handle extended scan codes that have a non-extended counterpart manually.
	// In addition, according to http://support.microsoft.com/default.aspx?scid=kb;en-us;72583
	// most or all numeric keypad keys cannot be mapped reliably under any OS.
	// This article is a little unclear about which direction, if any, that MapVirtualKey does
	// work in for the numpad keys, so for peace-of-mind map them all manually for now.

	// Even though Map() should work for these under Win2k/XP, I'm not sure it would work
	// under all versions of NT.  Therefore, for now just standardize to be sure they're
	// the same -- and thus the program will behave the same -- across the board, on all OS's.
	g_vk_to_sc[VK_LCONTROL].a = SC_LCONTROL;
	g_vk_to_sc[VK_RCONTROL].a = SC_RCONTROL;
	g_vk_to_sc[VK_LSHIFT].a = SC_LSHIFT;  // Map() wouldn't work for these two under Win9x.
	g_vk_to_sc[VK_RSHIFT].a = SC_RSHIFT;
	g_vk_to_sc[VK_LMENU].a = SC_LALT;
	g_vk_to_sc[VK_RMENU].a = SC_RALT;
	// Lwin and Rwin have their own VK's so should be supported, except maybe on Win95?
	// VK_CONTROL/SHIFT/MENU will be handled by Map(), which should assign the left scan code by default?

	g_vk_to_sc[VK_NUMPAD0].a = SC_NUMPAD0;
	g_vk_to_sc[VK_NUMPAD1].a = SC_NUMPAD1;
	g_vk_to_sc[VK_NUMPAD2].a = SC_NUMPAD2;
	g_vk_to_sc[VK_NUMPAD3].a = SC_NUMPAD3;
	g_vk_to_sc[VK_NUMPAD4].a = SC_NUMPAD4;
	g_vk_to_sc[VK_NUMPAD5].a = SC_NUMPAD5;
	g_vk_to_sc[VK_NUMPAD6].a = SC_NUMPAD6;
	g_vk_to_sc[VK_NUMPAD7].a = SC_NUMPAD7;
	g_vk_to_sc[VK_NUMPAD8].a = SC_NUMPAD8;
	g_vk_to_sc[VK_NUMPAD9].a = SC_NUMPAD9;
	g_vk_to_sc[VK_DECIMAL].a = SC_NUMPADDOT;

	g_vk_to_sc[VK_NUMLOCK].a = SC_NUMLOCK;
	g_vk_to_sc[VK_DIVIDE].a = SC_NUMPADDIV;
	g_vk_to_sc[VK_MULTIPLY].a = SC_NUMPADMULT;
	g_vk_to_sc[VK_SUBTRACT].a = SC_NUMPADSUB;
	g_vk_to_sc[VK_ADD].a = SC_NUMPADADD;

	// Use the OS API call to resolve any not manually set above:
	vk_type vk;
	for (vk = 0; vk < VK_MAX; ++vk)
		if (!g_vk_to_sc[vk].a)
			g_vk_to_sc[vk].a = MapVirtualKey(vk, 0);

	// Just in case the above didn't find a mapping for these, perhaps due to Win95 not supporting them:
	if (!g_vk_to_sc[VK_LWIN].a)
		g_vk_to_sc[VK_LWIN].a = SC_LWIN;
	if (!g_vk_to_sc[VK_RWIN].a)
		g_vk_to_sc[VK_RWIN].a = SC_RWIN;

	// There doesn't appear to be any built-in function to determine whether a vk's scan code
	// is extended or not.  See MSDN topic "keyboard input" for the below list.
	// Note: NumpadEnter is probably the only extended key that doesn't have a unique VK of its own.
	// So in that case, probably safest not to set the extended flag.  To send a true NumpadEnter,
	// as well as a true NumPadDown and any other key that shares the same VK with another, the
	// caller should specify the sc param to circumvent the need for KeyEvent() to use the below:
	// Turn on the extended flag for those that need it.  Do this for all known extended keys
	// even if it was already done due to a manual assignment above.  That way, this list can always
	// be defined as the list of all keys with extended scan codes:
	g_vk_to_sc[VK_LWIN].a |= 0x0100;
	g_vk_to_sc[VK_RWIN].a |= 0x0100;
	g_vk_to_sc[VK_APPS].a |= 0x0100;  // Application key on keyboards with LWIN/RWIN/Apps.  Not listed in MSDN?
	g_vk_to_sc[VK_RMENU].a |= 0x0100;
	g_vk_to_sc[VK_RCONTROL].a |= 0x0100;
	g_vk_to_sc[VK_RSHIFT].a |= 0x0100; // WinXP needs this to be extended for keybd_event() to work properly.
	g_vk_to_sc[VK_CANCEL].a |= 0x0100; // Ctrl-break
	g_vk_to_sc[VK_SNAPSHOT].a |= 0x0100;  // PrintScreen
	g_vk_to_sc[VK_NUMLOCK].a |= 0x0100;
	g_vk_to_sc[VK_DIVIDE].a |= 0x0100; // NumpadDivide (slash)
	// In addition, these VKs have more than one physical key:
	g_vk_to_sc[VK_RETURN].b = g_vk_to_sc[VK_RETURN].a | 0x0100;
	g_vk_to_sc[VK_INSERT].b = g_vk_to_sc[VK_INSERT].a | 0x0100;
	g_vk_to_sc[VK_DELETE].b = g_vk_to_sc[VK_DELETE].a | 0x0100;
	g_vk_to_sc[VK_PRIOR].b = g_vk_to_sc[VK_PRIOR].a | 0x0100; // PgUp
	g_vk_to_sc[VK_NEXT].b = g_vk_to_sc[VK_NEXT].a | 0x0100;  // PgDn
	g_vk_to_sc[VK_HOME].b = g_vk_to_sc[VK_HOME].a | 0x0100;
	g_vk_to_sc[VK_END].b = g_vk_to_sc[VK_END].a | 0x0100;
	g_vk_to_sc[VK_UP].b = g_vk_to_sc[VK_UP].a | 0x0100;
	g_vk_to_sc[VK_DOWN].b = g_vk_to_sc[VK_DOWN].a | 0x0100;
	g_vk_to_sc[VK_LEFT].b = g_vk_to_sc[VK_LEFT].a | 0x0100;
	g_vk_to_sc[VK_RIGHT].b = g_vk_to_sc[VK_RIGHT].a | 0x0100;
}



void init_sc_to_vk()
{
	ZeroMemory(g_sc_to_vk, sizeof(g_sc_to_vk));

	// These are mapped manually because MapVirtualKey() doesn't support them correctly, at least
	// on some -- if not all -- OSs.  The main app also relies upon the values assigned below to
	// determine which keys should be handled by scan code rather than vk:
	g_sc_to_vk[SC_NUMLOCK].a = VK_NUMLOCK;
	g_sc_to_vk[SC_NUMPADDIV].a = VK_DIVIDE;
	g_sc_to_vk[SC_NUMPADMULT].a = VK_MULTIPLY;
	g_sc_to_vk[SC_NUMPADSUB].a = VK_SUBTRACT;
	g_sc_to_vk[SC_NUMPADADD].a = VK_ADD;
	g_sc_to_vk[SC_NUMPADENTER].a = VK_RETURN;

	// The following are ambiguous because each maps to more than one VK.  But be careful
	// changing the value to the other choice due to the above comment:
	g_sc_to_vk[SC_NUMPADDEL].a = VK_DELETE; g_sc_to_vk[SC_NUMPADDEL].b = VK_DECIMAL;
	g_sc_to_vk[SC_NUMPADCLEAR].a = VK_CLEAR; g_sc_to_vk[SC_NUMPADCLEAR].b = VK_NUMPAD5; // Same key as Numpad5 on most keyboards?
	g_sc_to_vk[SC_NUMPADINS].a = VK_INSERT; g_sc_to_vk[SC_NUMPADINS].b = VK_NUMPAD0;
	g_sc_to_vk[SC_NUMPADUP].a = VK_UP; g_sc_to_vk[SC_NUMPADUP].b = VK_NUMPAD8;
	g_sc_to_vk[SC_NUMPADDOWN].a = VK_DOWN; g_sc_to_vk[SC_NUMPADDOWN].b = VK_NUMPAD2;
	g_sc_to_vk[SC_NUMPADLEFT].a = VK_LEFT; g_sc_to_vk[SC_NUMPADLEFT].b = VK_NUMPAD4;
	g_sc_to_vk[SC_NUMPADRIGHT].a = VK_RIGHT; g_sc_to_vk[SC_NUMPADRIGHT].b = VK_NUMPAD6;
	g_sc_to_vk[SC_NUMPADHOME].a = VK_HOME; g_sc_to_vk[SC_NUMPADHOME].b = VK_NUMPAD7;
	g_sc_to_vk[SC_NUMPADEND].a = VK_END; g_sc_to_vk[SC_NUMPADEND].b = VK_NUMPAD1;
	g_sc_to_vk[SC_NUMPADPGUP].a = VK_PRIOR; g_sc_to_vk[SC_NUMPADPGUP].b = VK_NUMPAD9;
	g_sc_to_vk[SC_NUMPADPGDN].a = VK_NEXT; g_sc_to_vk[SC_NUMPADPGDN].b = VK_NUMPAD3;

	// Even though neither of the SHIFT keys are extended, and thus could be mapped with
	// MapVirtualKey(), it seems better to define them explicitly because under Win9x (maybe just Win95),
	// I'm pretty sure MapVirtualKey() would return VK_SHIFT instead of the left/right VK.
	g_sc_to_vk[SC_LSHIFT].a = VK_LSHIFT;
	g_sc_to_vk[SC_RSHIFT].a = VK_RSHIFT;
	g_sc_to_vk[SC_LCONTROL].a = VK_LCONTROL;
	g_sc_to_vk[SC_RCONTROL].a = VK_RCONTROL;
	g_sc_to_vk[SC_LALT].a = VK_LMENU;
	g_sc_to_vk[SC_RALT].a = VK_RMENU;

	// Use the OS API call to resolve any not manually set above.  This should correctly
	// resolve even elements such as g_sc_to_vk[SC_INSERT], which is an extended scan code,
	// because it passes in only the low-order byte which is SC_NUMPADINS.  In that
	// example, Map() will return the same vk for both, which is correct.
	sc_type sc;
	for (sc = 0; sc < SC_MAX; ++sc)
		if (!g_sc_to_vk[sc].a)
			// Only pass the LOBYTE because I think it fails to work properly otherwise.
			// Also, DO NOT pass 3 for the 2nd param of MapVirtualKey() because apparently
			// that is not compatible with Win9x so it winds up returning zero for keys
			// such as UP, LEFT, HOME, and PGUP (maybe other sorts of keys too).  This
			// should be okay even on XP because the left/right specific keys have already
			// been resolved above so don't need to be looked up here (LWIN and RWIN
			// each have their own VK's so shouldn't be problem for the below call to resolve):
			g_sc_to_vk[sc].a = MapVirtualKey((BYTE)sc, 1);
}



sc_type TextToSC(char *aText)
{
	if (!aText || !*aText) return 0;
	for (int i = 0; i < g_key_to_sc_count; ++i)
		if (!stricmp(g_key_to_sc[i].key_name, aText))
			return g_key_to_sc[i].sc;
	// Do this only after the above, in case any valid key names ever start with SC:
	if (toupper(*aText) == 'S' && toupper(*(aText + 1)) == 'C')
	{
		int sc; // UINT in case sscanf() requires a larger storage area than sc_type (even though it is currently UINT).
		sscanf(aText + 2, "%X", &sc); // Convert hexadecimal text string to word/UINT/sc_type.
		return sc;
	}
	return 0; // I don't think zero is a valid scan code, but might want to confirm.
}



vk_type TextToVK(char *aText, mod_type *pModifiers, bool aExcludeThoseHandledByScanCode)
// If modifiers_p is non-NULL, place the modifiers that are needed to realize the key in there.
// e.g. M is really +m (shift-m), # is really shift-3.
{
	if (!aText || !*aText) return 0;

	// Don't trim() aText or modify it because that will mess up the caller who expects it to be unchanged.
	// Instead, for now, just check it as-is.  The only extra whitespace that should exist, due to trimming
	// of text during load, is that on either side of the COMPOSITE_DELIMITER (e.g. " then ").

	if (strlen(aText) == 1)
	{
		SHORT mod_plus_vk = VkKeyScan(*aText);
		char keyscan_modifiers = HIBYTE(mod_plus_vk);
		if (keyscan_modifiers == -1) // No translation could be made.
			return 0;

		// The win docs for VkKeyScan() are a bit confusing, referring to flag "bits" when it should really
		// say flag "values".  In addition, it seems that these flag values are incompatible with
		// MOD_ALT, MOD_SHIFT, and MOD_CONTROL, so they must be translated:
		if (pModifiers) // The caller wants this info added to the output param
		{
			// Best not to reset this value because some callers want to retain what was in it before,
			// merely merging these new values into it:
			//*pModifiers = 0;
			if (keyscan_modifiers & 0x01)
				*pModifiers |= MOD_SHIFT;
			if (keyscan_modifiers & 0x02)
				*pModifiers |= MOD_CONTROL;
			if (keyscan_modifiers & 0x04)
				*pModifiers |= MOD_ALT;
		}
		return LOBYTE(mod_plus_vk);  // The virtual key.
	}

// Use above in favor of this:
//	if (strlen(text) == 1 && toupper(*text) >= 'A' && toupper(*text) <= 'Z')
//		return toupper(*text);  // VK is the same as the ASCII code in this case, maybe for other chars too?

	for (int i = 0; i < g_key_to_vk_count; ++i)
		if (!stricmp(g_key_to_vk[i].key_name, aText))
			return g_key_to_vk[i].vk;

	if (aExcludeThoseHandledByScanCode)
		return 0; // Zero is not a valid virtual key, so it should be a safe failure indicator.

	// Otherwise check if aText is the name of a key handled by scan code and if so, map that
	// scan code to its corresponding virtual key:
	sc_type sc = TextToSC(aText);
	return sc ? g_sc_to_vk[sc].a : 0;
}



int TextToSpecial(char *aText, UINT aTextLength, modLR_type &aModifiersLR, mod_type &aModifiers)
// Returns vk for key-down, negative vk for key-up, or zero if no translation.
// We also update whatever's in *pModifiers and *pModifiersLR to reflect the type of key-action
// specified in <aText>.  This makes it so that {altdown}{esc}{altup} behaves the same as !{esc}.
// Note that things like LShiftDown are not supported because: 1) they are rarely needed; and 2)
// they can be down via "lshift down".
{
	if (!aTextLength || !aText || !*aText) return 0;

	if (!strlicmp(aText, "ALTDOWN", aTextLength))
	{
		if (!(aModifiersLR & (MOD_LALT | MOD_RALT))) // i.e. do nothing if either left or right is already present.
			aModifiersLR |= MOD_LALT; // If neither is down, use the left one because it's more compatible.
		aModifiers |= MOD_ALT;
		return VK_MENU;
	}
	if (!strlicmp(aText, "ALTUP", aTextLength))
	{
		// Unlike for Lwin/Rwin, it seems best to have these neutral keys (e.g. ALT vs. LALT or RALT)
		// restore either or both of the ALT keys into the up position.  The user can use {LAlt Up}
		// to be more specific and avoid this behavior:
		aModifiersLR &= ~(MOD_LALT | MOD_RALT);
		aModifiers &= ~MOD_ALT;
		return -VK_MENU;
	}
	if (!strlicmp(aText, "SHIFTDOWN", aTextLength))
	{
		if (!(aModifiersLR & (MOD_LSHIFT | MOD_RSHIFT))) // i.e. do nothing if either left or right is already present.
			aModifiersLR |= MOD_LSHIFT; // If neither is down, use the left one because it's more compatible.
		aModifiers |= MOD_SHIFT;
		return VK_SHIFT;
	}
	if (!strlicmp(aText, "SHIFTUP", aTextLength))
	{
		aModifiersLR &= ~(MOD_LSHIFT | MOD_RSHIFT); // See "ALTUP" for explanation.
		aModifiers &= ~MOD_SHIFT;
		return -VK_SHIFT;
	}
	if (!strlicmp(aText, "CTRLDOWN", aTextLength) || !strlicmp(aText, "CONTROLDOWN", aTextLength))
	{
		if (!(aModifiersLR & (MOD_LCONTROL | MOD_RCONTROL))) // i.e. do nothing if either left or right is already present.
			aModifiersLR |= MOD_LCONTROL; // If neither is down, use the left one because it's more compatible.
		aModifiers |= MOD_CONTROL;
		return VK_CONTROL;
	}
	if (!strlicmp(aText, "CTRLUP", aTextLength) || !strlicmp(aText, "CONTROLUP", aTextLength))
	{
		aModifiersLR &= ~(MOD_LCONTROL | MOD_RCONTROL); // See "ALTUP" for explanation.
		aModifiers &= ~MOD_CONTROL;
		return -VK_CONTROL;
	}
	if (!strlicmp(aText, "LWINDOWN", aTextLength))
	{
		aModifiersLR |= MOD_LWIN;
		aModifiers |= MOD_WIN;
		return VK_LWIN;
	}
	if (!strlicmp(aText, "LWINUP", aTextLength))
	{
		aModifiersLR &= ~MOD_LWIN;
		if (!(aModifiersLR & MOD_RWIN))  // If both WIN keys are now up, the neutral modifier also is set to up.
			aModifiers &= ~MOD_WIN;
		return -VK_LWIN;
	}
	if (!strlicmp(aText, "RWINDOWN", aTextLength))
	{
		aModifiersLR |= MOD_RWIN;
		aModifiers |= MOD_WIN;
		return VK_RWIN;
	}
	if (!strlicmp(aText, "RWINUP", aTextLength))
	{
		aModifiersLR &= ~MOD_RWIN;
		if (!(aModifiersLR & MOD_LWIN))  // If both WIN keys are now up, the neutral modifier also is set to up.
			aModifiers &= ~MOD_WIN;
		return -VK_RWIN;
	}
	return 0;
}



#ifdef ENABLE_KEY_HISTORY_FILE
ResultType KeyHistoryToFile(char *aFilespec, char aType, bool aKeyUp, vk_type aVK, sc_type aSC)
{
	static char target_filespec[MAX_PATH] = "";
	static FILE *fp = NULL;
	static HWND last_foreground_window = NULL;
	static DWORD last_tickcount = GetTickCount();

	if (!aFilespec && !aVK && !aSC) // Caller is signaling to close the file if it's open.
	{
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		return OK;
	}

	if (aFilespec && *aFilespec && stricmp(aFilespec, target_filespec)) // Target filename has changed.
	{
		if (fp)
		{
			fclose(fp);
			fp = NULL;  // To indicate to future calls to this function that it's closed.
		}
		strlcpy(target_filespec, aFilespec, sizeof(target_filespec));
	}

	if (!aVK && !aSC) // Caller didn't want us to log anything this time.
		return OK;
	if (!*target_filespec)
		return OK; // No target filename has ever been specified, so don't even attempt to open the file.

	if (!aVK)
		aVK = g_sc_to_vk[aSC].a;
	else
		if (!aSC)
			aSC = g_vk_to_sc[aVK].a;

	char buf[2048] = "", win_title[1024] = "<Init>", key_name[128] = "";
	HWND curr_foreground_window = GetForegroundWindow();
	DWORD curr_tickcount = GetTickCount();
	bool log_changed_window = (curr_foreground_window != last_foreground_window);
	if (log_changed_window)
	{
		if (curr_foreground_window)
			GetWindowText(curr_foreground_window, win_title, sizeof(win_title));
		else
			strlcpy(win_title, "<None>", sizeof(win_title));
		last_foreground_window = curr_foreground_window;
	}

	snprintf(buf, sizeof(buf), "%02X" "\t%03X" "\t%0.2f" "\t%c" "\t%c" "\t%s" "%s%s\n"
		, aVK, aSC
		, (float)(curr_tickcount - last_tickcount) / (float)1000
		, aType
		, aKeyUp ? 'u' : 'd'
		, GetKeyName(aVK, aSC, key_name, sizeof(key_name))
		, log_changed_window ? "\t" : ""
		, log_changed_window ? win_title : ""
		);
	last_tickcount = curr_tickcount;
	if (!fp)
		if (   !(fp = fopen(target_filespec, "a"))   )
			return OK;
	fputs(buf, fp);
	return OK;
}
#endif



char *GetKeyName(vk_type aVK, sc_type aSC, char *aBuf, size_t aBuf_size)
{
	if (!aBuf || aBuf_size < 3) return aBuf;

	*aBuf = '\0'; // Set default.
	if (!aVK && !aSC)
		return aBuf;

	if (!aVK)
		aVK = g_sc_to_vk[aSC].a;
	else
		if (!aSC)
			aSC = g_vk_to_sc[aVK].a;

	// Use 0x02000000 to tell it that we want it to give left/right specific info, lctrl/rctrl etc.
	if (!aSC || !GetKeyNameText((long)(aSC) << 16, aBuf, (int)(aBuf_size/sizeof(TCHAR))))
	{
		for (int j = 0; j < g_key_to_vk_count; ++j)
			if (g_key_to_vk[j].vk == aVK)
				break;
		if (j < g_key_to_vk_count)
			strlcpy(aBuf, g_key_to_vk[j].key_name, aBuf_size);
		else
		{
			if (isprint(aVK))
			{
				aBuf[0] = aVK;
				aBuf[1] = '\0';
			}
			else
				strlcpy(aBuf, "not found", aBuf_size);
		}
	}
	return aBuf;
}
