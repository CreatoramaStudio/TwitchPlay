// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "TwitchEnums.h"
#include "TwitchStructs.generated.h"

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

USTRUCT(BlueprintType)
struct FTwitchChatMessage
{
	GENERATED_BODY()

public:
	UPROPERTY(Category = "Message", EditAnywhere, BlueprintReadWrite)
	FString Username = "";
	
	UPROPERTY(Category = "Message", EditAnywhere, BlueprintReadWrite)
	FString Message = "";
	
	UPROPERTY(Category = "Message", EditAnywhere, BlueprintReadWrite)
	bool bIsSubbed = false;
	
	UPROPERTY(Category = "Message", EditAnywhere, BlueprintReadWrite)
	bool bBits = false;
	
	UPROPERTY(Category = "Message", EditAnywhere, BlueprintReadWrite)
	float Bits = 0;
	
	UPROPERTY(Category = "Message", EditAnywhere, BlueprintReadWrite)
	FColor UserColor = FColor::White;
};