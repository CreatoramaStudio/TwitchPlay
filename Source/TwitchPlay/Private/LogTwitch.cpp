#include "LogTwitch.h"

DEFINE_LOG_CATEGORY(LogTwitchPlay);

void FLogTwitchPlay::Info(const FString& String)
{
	UE_LOG(LogTwitchPlay, Display, TEXT("%s"), *String);
}

void FLogTwitchPlay::Warning(const FString& String)
{
	UE_LOG(LogTwitchPlay, Warning, TEXT("%s"), *String);
}

void FLogTwitchPlay::Error(const FString& String)
{
	UE_LOG(LogTwitchPlay, Error, TEXT("%s"), *String);
}