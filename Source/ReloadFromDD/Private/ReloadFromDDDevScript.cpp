#include "ReloadFromDDDevScript.h"

#if RELOADFROMDD_DEV_COMMANDS

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "ReloadFromDDLog.h"

namespace RFDD::DevScript
{
	namespace
	{
		struct FState
		{
			TArray<FString> Lines;
			int32 Next = 0;
			double ResumeAt = 0.0;
			double WaitStarted = 0.0;
			TWeakObjectPtr<UWorld> World;
			FTSTicker::FDelegateHandle Ticker;
			FString PendingFile; // armed by RFDD.ScriptOnLoad
		};
		FState GState;

		void Stop(const TCHAR* Why)
		{
			if (GState.Ticker.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(GState.Ticker);
				GState.Ticker.Reset();
			}
			UE_LOG(LogReloadFromDD, Log, TEXT("[script] stopped: %s"), Why);
			GState.Lines.Reset();
			GState.Next = 0;
		}

		bool Tick(float)
		{
			UWorld* World = GState.World.Get();
			if (!World)
			{
				Stop(TEXT("world ended"));
				return false;
			}
			const double Now = FPlatformTime::Seconds();
			while (GState.Next < GState.Lines.Num() && Now >= GState.ResumeAt)
			{
				const FString Line = GState.Lines[GState.Next++].TrimStartAndEnd();
				if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
				{
					continue;
				}
				if (Line.StartsWith(TEXT("wait "), ESearchCase::IgnoreCase))
				{
					GState.ResumeAt = Now + FCString::Atod(*Line.Mid(5));
					break;
				}
				if (Line.StartsWith(TEXT("waitplayers "), ESearchCase::IgnoreCase))
				{
					// waitplayers <count> [timeout seconds]: holds until that many player pawns exist (or the timeout passes).
					TArray<FString> Parts;
					Line.ParseIntoArrayWS(Parts);
					const int32 Wanted = Parts.Num() > 1 ? FCString::Atoi(*Parts[1]) : 1;
					const double Timeout = Parts.Num() > 2 ? FCString::Atod(*Parts[2]) : 300.0;
					int32 Pawns = 0;
					for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
					{
						Pawns += (It->Get() && It->Get()->GetPawn()) ? 1 : 0;
					}
					if (Pawns < Wanted)
					{
						if (GState.WaitStarted == 0.0)
						{
							GState.WaitStarted = Now;
						}
						if (Now - GState.WaitStarted < Timeout)
						{
							--GState.Next; // retry this line next tick
							GState.ResumeAt = Now + 1.0;
							break;
						}
						UE_LOG(LogReloadFromDD, Warning, TEXT("[script] waitplayers %d timed out with %d"), Wanted, Pawns);
					}
					else
					{
						UE_LOG(LogReloadFromDD, Log, TEXT("[script] %d player(s) present"), Pawns);
					}
					GState.WaitStarted = 0.0;
					continue;
				}
				if (Line.StartsWith(TEXT("log "), ESearchCase::IgnoreCase))
				{
					UE_LOG(LogReloadFromDD, Log, TEXT("[script] ==== %s"), *Line.Mid(4));
					continue;
				}
				UE_LOG(LogReloadFromDD, Log, TEXT("[script] > %s"), *Line);
				GEngine->Exec(World, *Line, *GLog);
				if (!GState.World.IsValid())
				{
					break; // the command ended the world (e.g. quit)
				}
			}
			if (GState.Next >= GState.Lines.Num())
			{
				Stop(TEXT("end of script"));
				return false;
			}
			return true;
		}

		bool Start(UWorld* World, const FString& File)
		{
			TArray<FString> Lines;
			if (!World || !FFileHelper::LoadFileToStringArray(Lines, *File))
			{
				UE_LOG(LogReloadFromDD, Error, TEXT("[script] could not read %s"), *File);
				return false;
			}
			if (GState.Ticker.IsValid())
			{
				Stop(TEXT("replaced by a new script"));
			}
			GState.Lines = MoveTemp(Lines);
			GState.Next = 0;
			GState.ResumeAt = 0.0;
			GState.World = World;
			GState.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.0f);
			UE_LOG(LogReloadFromDD, Log, TEXT("[script] started %s (%d lines)"), *File, GState.Lines.Num());
			return true;
		}

		FAutoConsoleCommandWithWorldAndArgs GScriptCommand(
			TEXT("RFDD.Script"),
			TEXT("[dev] RFDD.Script <file> - runs a file of console commands (wait <s>, log <text>, # comments) in this world."),
			FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{
				if (Args.Num() > 0)
				{
					Start(World, FString::Join(Args, TEXT(" ")));
				}
			}));

		FAutoConsoleCommandWithWorldAndArgs GScriptOnLoadCommand(
			TEXT("RFDD.ScriptOnLoad"),
			TEXT("[dev] RFDD.ScriptOnLoad <file> - runs the script when the next game world begins (use from -ExecCmds)."),
			FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
			{
				if (Args.Num() > 0)
				{
					GState.PendingFile = FString::Join(Args, TEXT(" "));
					UE_LOG(LogReloadFromDD, Log, TEXT("[script] armed %s for the next game world"), *GState.PendingFile);
				}
			}));
	}

	void OnGameWorldBegan(UWorld* World)
	{
		if (!GState.PendingFile.IsEmpty())
		{
			const FString File = MoveTemp(GState.PendingFile);
			GState.PendingFile.Reset();
			Start(World, File);
		}
	}
}

#endif
