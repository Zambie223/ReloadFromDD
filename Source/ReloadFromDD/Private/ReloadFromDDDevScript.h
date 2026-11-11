#pragma once

#include "CoreMinimal.h"

#if RELOADFROMDD_DEV_COMMANDS

class UWorld;

/**
 * Development aid: runs a text file of console commands in the game world, for tests without input.
 * Lines: a console command, "wait <seconds>", "log <text>", or "# comment". Each command's output goes to the log.
 * RFDD.ScriptOnLoad <file> (typed at the main menu, e.g. via -ExecCmds) starts it when the next game world begins.
 */
namespace RFDD::DevScript
{
	/** Called by the settings subsystem when a game world begins play on the authority. */
	void OnGameWorldBegan(UWorld* World);
}

#endif
