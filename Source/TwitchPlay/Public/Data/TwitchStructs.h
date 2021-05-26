// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "TwitchEnums.h"

// Blob of user messages received
struct FTwitchReceiveMessages
{
	TArray<FString> Usernames;
	TArray<FString> Messages;
};

struct FTwitchConnection
{
	FTwitchConnection(): Type()
	{
	}

	FTwitchConnection(const ETwitchConnectionMessageType Type,FString Message)
	{
		this->Type = Type;
		this->Message = Message;
	}
	
	ETwitchConnectionMessageType Type;
	FString Message;
};

struct FTwitchSendMessage
{
	// The message type
	ETwitchSendMessageType Type;
	// The message
	FString Message;
	// The channel (can be empty)
	FString Channel;
};