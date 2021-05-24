// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/TwitchEnums.h"
#include "Data/TwitchStructs.h"

class UTwitchSubsystem;

/**
 * Twitch messages receiver runnable
 */
class FTwitchMessageReceiver : public FRunnable
{
public:	

	TFunction<void(const FTwitchReceiveMessages& Messages)> ReceiveMessages;
	TFunction<void(const FTwitchConnection& Connections)> ReceiveConnections;

	using FTwitchReceiveMessagesQueue = TQueue<FTwitchReceiveMessages, EQueueMode::Spsc>;
	using FTwitchSendMessagesQueue = TQueue<FTwitchSendMessage, EQueueMode::Spsc>;
	using FTwitchConnectionQueue = TQueue<FTwitchConnection, EQueueMode::Spsc>;

protected:

private:
	
	// Sending and receiving queues
	TUniquePtr<FTwitchSendMessagesQueue> SendingQueue;
	TUniquePtr<FTwitchReceiveMessagesQueue> ReceivingQueue;

	// Connection status queue
	TUniquePtr<FTwitchConnectionQueue> ConnectionQueue;

	FSocket* ConnectionSocket;

	FRunnableThread* MessagesThread;

	FThreadSafeBool bShouldExit;

	FThreadSafeBool bIsConnected;

	// Authentication token. Need to get it from official Twitch API
	FString OAuth;

	// Username. Must be in lowercase
	FString Username;

	// Channel to join upon successful connection	
	FString Channel;

	// True while we are waiting for the auth reply from the server
	bool bWaitingForAuth;

	// The number of times auth has slept
	int32 NumAuthWaits;

	// A time accumulator while the thread is running. Precision isn't great, but accurate enough for general timing.
	float AccumulationTime;

	// The set time between messages
	float TimeBetweenMessages;

	// The next time to send a message
	float NextSendMessageTime;

public:

	FTwitchMessageReceiver();
	virtual ~FTwitchMessageReceiver() override;

	void StartConnection(const FString& auth, const FString& username, const FString& channel, const float timeBetweenMessages);

	// FRunnable interface.
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

	void PullMessages(TArray<FString>& OutUsernames, TArray<FString>& OutMessages) const;
	void SendMessage(const ETwitchSendMessageType type, const FString& message, const FString& channel) const;
	bool PullConnectionMessage(ETwitchConnectionMessageType& OutStatus, FString& OutMessage) const;

	void StopConnection(bool bWaitTillComplete);

	bool IsConnected() const
	{
		return bIsConnected;
	}

	void GetConnectionInfo(FString& OutOAuth, FString& OutUsername, FString& OutChannel) const
	{
		OutOAuth = OAuth;
		OutUsername = Username;
		OutChannel = Channel;
	}

protected:

private:

	void SleepReceiver(float seconds);

	FString ReceiveFromConnection() const;

	/**
	* Parses the message received from Twitch IRC chat in order to only get the content of the message.
	* Since a single "message" could actually include multiple lines an array of strings is returned.
	*
	* @param message - Message to parse
	* @param OutSenderUsername - The username(s) of the message sender(s). In sync with the return array.
	* @param OutMessages - Parsed messages.
	*
	*/
	void ParseMessage(const FString& message, TArray<FString>& OutSenderUsername, TArray<FString>& OutMessages) const;

	/**
	* Send a message on the connected socket
	* @param message - The message to send
	* @param channel - The channel (or user) to send this message to
	*/
	bool SendIRCMessage(const FString& message, const FString channel = TEXT("")) const;
};