// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

UENUM(BlueprintType)
enum class ETwitchConnectionMessageType : uint8
{
	// A connection and authentication was established.
	CONNECTED,
	// Failed to connect.
	FAILED_TO_CONNECT,
	// Failed to authenticate.
	FAILED_TO_AUTHENTICATE,
	// A general error, doesn't mean the connection was terminated.
	ERROR,
	// General message from the server
	MESSAGE,
	// Disconnected from server.
	DISCONNECTED
};

enum class ETwitchSendMessageType : uint8
{
	// User Chat Message
	CHAT_MESSAGE,
	// Join new channel message
	JOIN_MESSAGE,
};