#include "TwitchLog.h"

DEFINE_LOG_CATEGORY(LogTwitchPlay);

void PrintInfoTwitchPlay(const FString& String)
{
	UE_LOG(LogTwitchPlay, Display, TEXT("%s"), *String);
}

void PrintWarningTwitchPlay(const FString& String)
{
	UE_LOG(LogTwitchPlay, Warning, TEXT("%s"), *String);
}

void PrintErrorTwitchPlay(const FString& String)
{
	UE_LOG(LogTwitchPlay, Error, TEXT("%s"), *String);
}
