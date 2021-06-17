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
		TSharedRef<FInternetAddr> ConnectionAddr = SocketSubsystem->CreateInternetAddr();

		FAddressInfoResult GAIResult = SocketSubsystem->GetAddressInfo(TEXT("irc.chat.twitch.tv"),nullptr,EAddressInfoFlags::Default,NAME_None);
		if (GAIResult.Results.Num() == 0)
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::FAILED_TO_CONNECT, TEXT("Could not resolve hostname!"));
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
			return 1; // if the host could not be resolved return false
		}

		ConnectionAddr->SetRawIp(GAIResult.Results[0].Address->GetRawIp());

		// Set connection port
		// HTTPS 6697
		// HTTP 6667
		const int32 Port = 6667;
		ConnectionAddr->SetPort(Port);

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
		int32 SizeOut;
		retSocket->SetReceiveBufferSize(2 * 1024 * 1024, SizeOut);
		retSocket->SetReuseAddr(true);

		// Try connection
		const bool bHasConnected = retSocket->Connect(*ConnectionAddr);

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

			// Request tags capability (If the user has extended bot permissions this means something, else it is mostly ignored)
			SendIRCMessage(TEXT("CAP REQ :twitch.tv/tags"));
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
				ParseMessage(connectionMessage);
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

void FTwitchMessageReceiver::ParseMessage(const FString& Message) const
{
	FTwitchReceiveMessages TwitchMessages;
	
	TArray<FString> MessageLines;
	Message.ParseIntoArrayLines(MessageLines); // A single "message" from Twitch IRC could include multiple lines. Split them now

	// Parse each line into its parts
	// Each line from Twitch contains meta information and content
	// Also need to check if the message is a PING sent from Twitch to check if the connection is alive
	// This is in the form "PING :tmi.twitch.tv" to which we need to reply with "PONG :tmi.twitch.tv"
	for (int32 CycleLine = 0; CycleLine < MessageLines.Num(); CycleLine++)
	{
		// If we receive a PING immediately reply with a PONG and skip the line parsing
		if (MessageLines[CycleLine].Equals("PING :tmi.twitch.tv"))
		{
			SendIRCMessage("PONG :tmi.twitch.tv");
			continue; // Skip line parsing
		}

		if (!MessageLines[CycleLine].IsEmpty())
		{
			const FTwitchConnection Connection(ETwitchConnectionMessageType::MESSAGE, MessageLines[CycleLine]);
			ConnectionQueue->Enqueue(Connection);
			ReceiveConnections(Connection);
		}

		// Parsing line
		// IRC tags docs: https://dev.twitch.tv/docs/irc/tags
		// Message form with tag is:

		// Example of a non-Bits message: The first Kappa (emote ID 25) is from character 0 (K) to character 4 (a), and the other Kappa is from 12 to 16.
			// @badge-info=subscriber/11;badges=subscriber/6,premium/1,global_mod/1,turbo/1;color=#0D4200;display-name=ronni;emotes=25:0-4,12-16/1902:6-10;id=b34ccfc7-4977-403a-8a94-33c6bac34fb8;mod=0;room-id=1337;subscriber=0;tmi-sent-ts=1507246572675;turbo=1;user-id=1337;user-type=global_mod :ronni!ronni@ronni.tmi.twitch.tv PRIVMSG #ronni :Kappa Keepo Kappa

		// Example of a Bits message:
			// @badge-info=subscriber/11;badges=subscriber/6,premium/1,staff/1,bits/1000;bits=100;color=#1E90FF;display-name=ronni;emotes=;id=b34ccfc7-4977-403a-8a94-33c6bac34fb8;mod=0;room-id=1337;subscriber=0;tmi-sent-ts=1507246572675;turbo=1;user-id=1337;user-type=staff :ronni!ronni@ronni.tmi.twitch.tv PRIVMSG #ronni :cheer100

		if (MessageLines[CycleLine].StartsWith("@badge-info") && MessageLines[CycleLine].Contains("PRIVMSG"))
		{
			FTwitchChatMessage ChatMessage;

			TArray<FString> messageParts;
			MessageLines[CycleLine].ParseIntoArray(messageParts, TEXT(" :"));

			//Tags
			TArray<FString> Tags;
			messageParts[0].ParseIntoArray(Tags, TEXT(";"));
			
			for (auto Tag : Tags)
			{
				if (Tag.StartsWith("@badge-info"))
				{
					FString Values;
					Tag.Split("=", nullptr, &Values);
					if (Values.IsEmpty())
					{
						continue;
					}
					TArray<FString> BadgesInfo;
					Values.ParseIntoArray(BadgesInfo, TEXT(","));

					for (auto BadgeInfo : BadgesInfo)
					{
						if (BadgeInfo.StartsWith("admin"))
						{
							
							continue;
						}

						if (BadgeInfo.StartsWith("bits"))
						{
							
							continue;
						}

						if (BadgeInfo.StartsWith("broadcaster"))
						{
							
							continue;
						}

						if (BadgeInfo.StartsWith("global_mod"))
						{
							
							continue;
						}

						if (BadgeInfo.StartsWith("moderator"))
						{
							
							continue;
						}

						if (BadgeInfo.StartsWith("subscriber"))
						{
							FString Subscriber;
							BadgeInfo.Split("/", nullptr, &Subscriber);
							if (Subscriber.IsNumeric())
							{
								ChatMessage.bIsSubbed = FCString::Atof(*Subscriber) > 0;
							}
							continue;
						}

						if (BadgeInfo.StartsWith("premium"))
						{
							FString Premium;
							BadgeInfo.Split("/", nullptr, &Premium);
							if (Premium.IsNumeric())
							{
								ChatMessage.bIsSubbed = FCString::Atof(*Premium) > 0;
							}
							continue;
						}

						if (BadgeInfo.StartsWith("staff"))
						{
							
							continue;
						}

						if (BadgeInfo.StartsWith("turbo"))
						{
							
							continue;
						}
					}
					continue;
				}
				
				if (Tag.StartsWith("badges"))
				{
					FString Values;
					Tag.Split("=", nullptr, &Values);
					if (Values.IsEmpty())
					{
						continue;
					}
					TArray<FString> Badges;
					Values.ParseIntoArray(Badges, TEXT(","));

					for (auto Badge : Badges)
					{
						{
							if (Badge.StartsWith("admin"))
							{
							
								continue;
							}

							if (Badge.StartsWith("bits"))
							{
							
								continue;
							}

							if (Badge.StartsWith("broadcaster"))
							{
							
								continue;
							}

							if (Badge.StartsWith("global_mod"))
							{
							
								continue;
							}

							if (Badge.StartsWith("moderator"))
							{
							
								continue;
							}

							if (Badge.StartsWith("subscriber"))
							{
								FString Subscriber;
								Badge.Split("/", nullptr, &Subscriber);
								if (Subscriber.IsNumeric())
								{
									ChatMessage.bIsSubbed = FCString::Atof(*Subscriber) > 0;
								}
								continue;
							}

							if (Badge.StartsWith("premium"))
							{
								FString Premium;
								Badge.Split("/", nullptr, &Premium);
								if (Premium.IsNumeric())
								{
									ChatMessage.bIsSubbed = FCString::Atof(*Premium) > 0;
								}
								continue;
							}

							if (Badge.StartsWith("staff"))
							{
							
								continue;
							}

							if (Badge.StartsWith("turbo"))
							{
							
								continue;
							}
						}
					}
					continue;
				}
				
				if (Tag.StartsWith("bits"))
				{
					FString Bits;
					Tag.Split("=", nullptr, &Bits);
					if (Bits.IsNumeric())
					{
						ChatMessage.bBits = true;
						ChatMessage.Bits = FCString::Atof(*Bits);
					}
					continue;
				}
				
				if (Tag.StartsWith("color"))
				{
					FString Color;
					Tag.Split("=", nullptr, &Color);
					if (!Color.IsEmpty())
					{
						ChatMessage.UserColor = FColor::FromHex(Color);
					}
					continue;
				}
				
				if (Tag.StartsWith("display-name"))
				{
					FString DisplayName;
					Tag.Split("=", nullptr, &DisplayName);
					
					if (!DisplayName.IsEmpty())
					{
						ChatMessage.Username = DisplayName;
					}
					continue;
				}
				
				if (Tag.StartsWith("emotes"))
				{

					continue;
				}
				
				if (Tag.StartsWith("flags"))
				{

					continue;
				}
				
				if (Tag.StartsWith("id"))
				{

					continue;
				}
				
				if (Tag.StartsWith("mod"))
				{

					continue;
				}
				
				if (Tag.StartsWith("room-id"))
				{

					continue;
				}
				
				if (Tag.StartsWith("tmi-sent-ts"))
				{

					continue;
				}
				
				if (Tag.StartsWith("user-id"))
				{

					continue;
				}
			}
			
			
			// Username
			if (!ChatMessage.Username.IsEmpty() && messageParts.Num() >= 2)
			{
				FString SenderUsername;
				messageParts[1].Split("!", &SenderUsername, nullptr);

				ChatMessage.Username = SenderUsername;
			}

			//Message
			if (!ChatMessage.Username.IsEmpty() && messageParts.Num() >= 3)
			{
				ChatMessage.Message = messageParts[2];
			}
			
			TwitchMessages.Messages.Add(ChatMessage.Message);
			TwitchMessages.Usernames.Add(ChatMessage.Username);

			ReceiveMessages(ChatMessage);
		}
		
	}

	if(TwitchMessages.Messages.Num())
	{
		ReceivingQueue->Enqueue(TwitchMessages);
	}
}

