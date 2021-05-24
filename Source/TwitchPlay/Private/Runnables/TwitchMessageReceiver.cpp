// Fill out your copyright notice in the Description page of Project Settings.


#include "Runnables/TwitchMessageReceiver.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

inline FString ANSIBytesToString(const uint8* In, int32 Count)
{
	FString Result;
	Result.Empty(Count);

	while (Count)
	{
		Result += static_cast<ANSICHAR>(*In);

		++In;
		Count--;
	}
	return Result;
}

FTwitchMessageReceiver::FTwitchMessageReceiver()
	: SendingQueue(MakeUnique<FTwitchSendMessagesQueue>())
	, ReceivingQueue(MakeUnique<FTwitchReceiveMessagesQueue>())
	, ConnectionQueue(MakeUnique<FTwitchConnectionQueue>())
	, ConnectionSocket(nullptr)
	, MessagesThread(nullptr)
	, bShouldExit(false)
	, bWaitingForAuth(false)
	, NumAuthWaits(0)
	, AccumulationTime(0)
	, TimeBetweenMessages(1.2f)
	, NextSendMessageTime(0)
{
	
}

FTwitchMessageReceiver::~FTwitchMessageReceiver()
{
	if (ConnectionSocket != nullptr)
	{
		ConnectionSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket);
		ConnectionSocket = nullptr;
	}

	SendingQueue = nullptr;
	ReceivingQueue = nullptr;
	ConnectionQueue = nullptr;
	MessagesThread = nullptr;
}

void FTwitchMessageReceiver::StartConnection(const FString& oauth, const FString& username, const FString& channel, const float timeBetweenMessages)
{
	checkf(!MessagesThread, TEXT("FTwitchMessageReceiver::StartConnection called more than once?"));
	OAuth = oauth;
	Username = username.ToLower();
	Channel = channel.ToLower();
	TimeBetweenMessages = timeBetweenMessages;
	MessagesThread = FRunnableThread::Create(this, TEXT("FTwitchMessageReceiver"));
}

uint32 FTwitchMessageReceiver::Run()
{
	if(!ConnectionSocket)
	{
		// Create the server connection
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> connectionAddr = SocketSubsystem->CreateInternetAddr();

		FAddressInfoResult GAIResult = SocketSubsystem->GetAddressInfo(TEXT("irc.twitch.tv"),
											     nullptr,
											     EAddressInfoFlags::Default,
											     NAME_None);
		if (GAIResult.Results.Num() == 0)
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::FAILED_TO_CONNECT, TEXT("Could not resolve hostname!"));
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			return 1; // if the host could not be resolved return false
		}

		connectionAddr->SetRawIp(GAIResult.Results[0].Address->GetRawIp());

		// Set connection port
		// HTTPS 6697
		// HTTP 6667
		const int32 port = 6697;
		connectionAddr->SetPort(port);

		FSocket* retSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("TwitchPlay Socket"), false);

		// Socket creation might fail on certain subsystems
		if (retSocket == nullptr)
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::FAILED_TO_CONNECT, TEXT("Could not create socket!"));
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			return 1;
		}

		// Setting underlying connection parameters
		int32 sizeOut;
		retSocket->SetReceiveBufferSize(2 * 1024 * 1024, sizeOut);
		retSocket->SetReuseAddr(true);

		// Try connection
		const bool bHasConnected = retSocket->Connect(*connectionAddr);

		// If we cannot connect destroy the socket and return
		if (!bHasConnected)
		{
			retSocket->Close();
			SocketSubsystem->DestroySocket(retSocket);

			const FTwitchConnection Connection(ETwitchConnectionMessageType::FAILED_TO_CONNECT, TEXT("Connection to Twitch IRC failed!"));
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			return 1;
		}

		ConnectionSocket = retSocket;

		const bool bPassOK = SendIRCMessage(TEXT("PASS ") + OAuth);
		const bool bNickOK = SendIRCMessage(TEXT("NICK ") + Username);
		const bool bSuccess = bPassOK && bNickOK;
		if(bSuccess)
		{
			bWaitingForAuth = true;
		}
		else
		{
			retSocket->Close();
			SocketSubsystem->DestroySocket(retSocket);

			const FTwitchConnection Connection(ETwitchConnectionMessageType::FAILED_TO_CONNECT,TEXT("Could not send initial PASS and NICK messages for Auth"));
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			return 1;
		}
	}

	while(bWaitingForAuth && !bShouldExit)
	{
		FString connectionMessage = ReceiveFromConnection();
		if(!connectionMessage.IsEmpty())
		{
			if(!(connectionMessage.StartsWith(TEXT(":tmi.twitch.tv 001")) && connectionMessage.Contains(TEXT(":Welcome, GLHF!"))))
			{
				ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
				ConnectionSocket->Close();
				SocketSubsystem->DestroySocket(ConnectionSocket);
				ConnectionSocket = nullptr;
			
				const FTwitchConnection Connection(ETwitchConnectionMessageType::FAILED_TO_AUTHENTICATE, connectionMessage);
				ConnectionQueue->Enqueue(Connection);
				ReceiveConnections(Connection);
				return 1;
			}

			const FTwitchConnection Connection(ETwitchConnectionMessageType::CONNECTED, connectionMessage);
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			
			bWaitingForAuth = false;

			if(!Channel.IsEmpty())
			{
				const bool joinOK = SendIRCMessage(TEXT("JOIN #") + Channel);
				if (!joinOK)
				{
					ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
					ConnectionSocket->Close();
					SocketSubsystem->DestroySocket(ConnectionSocket);
					ConnectionSocket = nullptr;

					const FTwitchConnection Connection2(ETwitchConnectionMessageType::FAILED_TO_AUTHENTICATE, TEXT("Failed to join channel"));
					ConnectionQueue->Enqueue(Connection2);
					ReceiveConnections(Connection2);
					return 1;
				}
			}

			bIsConnected = true;

			// Request command capability (If the user has extended bot permissions this means something, else it is mostly ignored)
			// This allows whispers to function, if the bot account has extended permissions.
			SendIRCMessage(TEXT("CAP REQ :twitch.tv/commands"));
		}
		else
		{
			// Wait a bit
			SleepReceiver(0.5f);
			++NumAuthWaits;
			if(NumAuthWaits > 4)
			{
				NumAuthWaits = 0;
				bShouldExit = true;

				const FTwitchConnection Connection(ETwitchConnectionMessageType::FAILED_TO_AUTHENTICATE, TEXT("Server did not respond"));
				ConnectionQueue->Enqueue(Connection);
				ReceiveConnections(Connection);
			}
		}
	}

	while(ConnectionSocket != nullptr && !bShouldExit)
	{
		if(ConnectionSocket->GetConnectionState() == ESocketConnectionState::SCS_Connected)
		{
			FString connectionMessage = ReceiveFromConnection();
			if (!connectionMessage.IsEmpty())
			{
				FTwitchReceiveMessages newMessages;
				ParseMessage(connectionMessage, newMessages.Usernames, newMessages.Messages);
				if(newMessages.Messages.Num())
				{
					ReceivingQueue->Enqueue(newMessages);
					ReceiveMessages(newMessages);
				}
			}

			if(NextSendMessageTime <= AccumulationTime)
			{
				// Send our messages
				FTwitchSendMessage sendMessage;
				if(SendingQueue->Dequeue(sendMessage))
				{
					if(sendMessage.Type == ETwitchSendMessageType::CHAT_MESSAGE)
					{
						if(!sendMessage.Channel.IsEmpty())
						{
							// Specific user private message
							SendIRCMessage(sendMessage.Message, sendMessage.Channel);
						}
						else if(!Channel.IsEmpty())
						{
							// To the currently joined channel
							SendIRCMessage(sendMessage.Message, Channel);
						}
						else
						{
							const FTwitchConnection Connection(ETwitchConnectionMessageType::ERROR,TEXT("Cannot send message. No channel specified, and not joined to a channel."));
							ConnectionQueue->Enqueue(Connection);
							ReceiveConnections(Connection);
						}
					}
					else if(sendMessage.Type == ETwitchSendMessageType::JOIN_MESSAGE)
					{
						if(!Channel.IsEmpty())
						{
							SendIRCMessage(TEXT("PART #") + Channel);
						}
						Channel = sendMessage.Channel;
						if(!Channel.IsEmpty())
						{
							SendIRCMessage(TEXT("JOIN #") + Channel);
						}
					}

					NextSendMessageTime = AccumulationTime + TimeBetweenMessages;
				}
			}
			
			// Sleep a bit before pulling more messages
			SleepReceiver(0.2f);
		}
		else
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::DISCONNECTED, TEXT("Lost connection to server"));
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			bShouldExit = true;
			bIsConnected = false;
		}
	}

	bIsConnected = false;
	if(ConnectionSocket)
	{
		if(ConnectionSocket->GetConnectionState() == ESocketConnectionState::SCS_Connected)
		{
			if(!Channel.IsEmpty())
			{
				// Part ways
				SendIRCMessage(TEXT("PART #") + Channel);
			}
			
			const FTwitchConnection Connection(ETwitchConnectionMessageType::DISCONNECTED, TEXT("Diconnected by request gracefully"));
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
		}

		ConnectionSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket);
		ConnectionSocket = nullptr;
	}
	
	return 0;
}

bool FTwitchMessageReceiver::SendIRCMessage(const FString& message, const FString channel) const
{
	// Only operate on existing and connected sockets
	if (ConnectionSocket != nullptr && ConnectionSocket->GetConnectionState() == ESocketConnectionState::SCS_Connected)
	{
		FString messageOut = message;
		// If the user specified a receiver format the message appropriately ("PRIVMSG")
		if (!channel.IsEmpty())
		{
			messageOut = FString::Printf(TEXT("PRIVMSG #%s :%s"), *channel, *message);
		}
		messageOut += TEXT("\r\n");
		const TCHAR* serializedMessage = GetData(messageOut);
		const int32 size = FCString::Strlen(serializedMessage);
		int32 sentOut;
		return ConnectionSocket->Send(reinterpret_cast<const uint8*>(TCHAR_TO_UTF8(serializedMessage)), size, sentOut);
	}
	
	return false;
}

void FTwitchMessageReceiver::Stop()
{
	bShouldExit = true;
}

void FTwitchMessageReceiver::Exit()
{
}

void FTwitchMessageReceiver::PullMessages(TArray<FString>& OutUsernames, TArray<FString>& OutMessages) const
{
	if(ReceivingQueue.IsValid() && !ReceivingQueue->IsEmpty())
	{
		FTwitchReceiveMessages message;
		while(ReceivingQueue->Dequeue(message))
		{
			OutUsernames.Append(message.Usernames);
			OutMessages.Append(message.Messages);
		}
	}
}

void FTwitchMessageReceiver::SendMessage(const ETwitchSendMessageType type, const FString& message, const FString& channel) const
{
	if(SendingQueue.IsValid())
	{
		SendingQueue->Enqueue(FTwitchSendMessage {type, message, channel});
	}
}

bool FTwitchMessageReceiver::PullConnectionMessage(ETwitchConnectionMessageType& OutStatus, FString& OutMessage) const
{
	FTwitchConnection ConnectionMessage;
	if(ConnectionQueue->Dequeue(ConnectionMessage))
	{
		OutStatus = ConnectionMessage.Type;
		OutMessage = ConnectionMessage.Message;
		return true;
	}

	return false;
}

void FTwitchMessageReceiver::StopConnection(bool bWaitTillComplete)
{
	if(MessagesThread)
	{
		bShouldExit = true;
		if(bWaitTillComplete)
		{
			MessagesThread->Kill(true);
		}
	}
}

void FTwitchMessageReceiver::SleepReceiver(float seconds)
{
	FPlatformProcess::Sleep(seconds);
	AccumulationTime += seconds;
}

FString FTwitchMessageReceiver::ReceiveFromConnection() const
{
	TArray<uint8> data;
	uint32 dataSize;
	bool received = false;
	if (ConnectionSocket->HasPendingData(dataSize))
	{
		received = true;
		data.SetNumUninitialized(dataSize); // Make space for the data
		int32 dataRead;
		ConnectionSocket->Recv(data.GetData(), data.Num(), dataRead); // Receive the data. Hopefully the buffer is large enough
	}

	FString connectionMessage;
	if (received)
	{
		connectionMessage = ANSIBytesToString(data.GetData(), data.Num());
	}

	return connectionMessage;
}

void FTwitchMessageReceiver::ParseMessage(const FString& message, TArray<FString>& OutSenderUsername, TArray<FString>& OutMessages) const
{
	OutMessages.Reset();
	
	TArray<FString> messageLines;
	message.ParseIntoArrayLines(messageLines); // A single "message" from Twitch IRC could include multiple lines. Split them now

	// Parse each line into its parts
	// Each line from Twitch contains meta information and content
	// Also need to check if the message is a PING sent from Twitch to check if the connection is alive
	// This is in the form "PING :tmi.twitch.tv" to which we need to reply with "PONG :tmi.twitch.tv"
	for (int32 cycleLine = 0; cycleLine < messageLines.Num(); cycleLine++)
	{
		// If we receive a PING immediately reply with a PONG and skip the line parsing
		if (messageLines[cycleLine] == TEXT("PING :tmi.twitch.tv"))
		{
			SendIRCMessage(TEXT("PONG :tmi.twitch.tv"));
			continue; // Skip line parsing
		}

		// Parsing line
		// Basic message form is ":twitch_username!twitch_username@twitch_username.tmi.twitch.tv PRIVMSG #channel :message here"
		// So we can split the message into two parts based off the ":" character: meta[0] and content[1..n]
		// Also have to account for	possible ":" inside the content itself
		TArray<FString> messageParts;
		messageLines[cycleLine].ParseIntoArray(messageParts, TEXT(":"));
		if(!messageParts.Num())
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::MESSAGE, messageLines[cycleLine]);
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			continue;
		}

		// Meta parsing
		// Meta info is split by whitespaces
		TArray<FString> meta;
		messageParts[0].ParseIntoArrayWS(meta);
		if(meta.Num() < 2)
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::MESSAGE, messageLines[cycleLine]);
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			continue;
		}

		// Assume at this point the message is from a user, but just in case set it beforehand
		// This is so that we can return an "empty" user if the message was of any other kind
		// For example, messages from the server (like upon connection) don't have a username
		FString senderUsername;
		if (meta[1] == TEXT("PRIVMSG")) // Type of message should always be in position 1 (or at least I hope so)
		{
			// Username should be the first part before the first "!"
			meta[0].Split(TEXT("!"), &senderUsername, nullptr);
		}

		if (senderUsername.IsEmpty())
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::MESSAGE, messageLines[cycleLine]);
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			continue; // Skip line
		}

		// Some messages correspond to events sent by the server (JOIN etc.)
		// In that case the message part is only one
		if (messageParts.Num() > 1)
		{
			// Content of the message is composed by all parts of the message from messageParts[1] on
			FString messageContent = messageParts[1];
			if (messageParts.Num() > 2)
			{
				for (int32 cycleContent = 2; cycleContent < messageParts.Num(); cycleContent++)
				{
					// Add back the ":" that was used as the splitter
					messageContent += TEXT(":") + messageParts[cycleContent];
				}
			}
			OutMessages.Add(messageContent);
			OutSenderUsername.Add(senderUsername);
		}
		else if(messageParts.Num())
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::MESSAGE, messageParts[0]);
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
		}
	}
}

