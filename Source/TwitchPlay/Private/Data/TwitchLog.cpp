#include "Data/TwitchLog.h"

DEFINE_LOG_CATEGORY(LogTwitchPlay);

void PrintTwitchPlay(const FString& String)
{
	UE_LOG(LogTwitchPlay, Warning, TEXT("%s"), *String);
}
