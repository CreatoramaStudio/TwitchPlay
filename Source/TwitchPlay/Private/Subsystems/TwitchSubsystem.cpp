// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystems/TwitchSubsystem.h"
#include "Runnables/TwitchMessageReceiver.h"

void UTwitchSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	TimeBetweenChatMessages = 1.2f;
	TwitchMessageReceiver = nullptr;

	BoundEvents = TMap<FString, FOnCommandReceived>();
	OnMessageReceived.AddDynamic(this, &UTwitchSubsystem::MessageReceivedHandler);
}

void UTwitchSubsystem::Deinitialize()
{
	if(TwitchMessageReceiver.IsValid())
	{
		TwitchMessageReceiver->StopConnection(true);
	}
}

void UTwitchSubsystem::Connect(const FString& OAuth, const FString& Username, const FString& Channel)
{
	if(TwitchMessageReceiver.IsValid())
	{
		OnConnectionMessage.Broadcast(ETwitchConnectionMessageType::ERROR, TEXT("Already connected / connecting / pending!"));
		return;
	}
	if(OAuth.IsEmpty() || Username.IsEmpty())
	{
		OnConnectionMessage.Broadcast(ETwitchConnectionMessageType::ERROR, TEXT("Invalid connection parameters. Check your strings."));
		return;
	}

	// Create the connection and messaging thread
	TwitchMessageReceiver = MakeUnique<FTwitchMessageReceiver>();
	TwitchMessageReceiver->StartConnection(OAuth, Username, Channel, TimeBetweenChatMessages);

	
	TwitchMessageReceiver->ReceiveMessages = [&](const FTwitchChatMessage& Messages)
	{
		OnMessageReceived.Broadcast(Messages);
	};

	TwitchMessageReceiver->ReceiveConnections = [&](const FTwitchConnection& Connection)
	{
		OnConnectionMessage.Broadcast(Connection.Type,Connection.Message);
	};
}

bool UTwitchSubsystem::SendChatMessage(const FString& Message, const FString Channel)
{
	if(TwitchMessageReceiver.IsValid())
	{
		TwitchMessageReceiver->SendMessage(ETwitchSendMessageType::CHAT_MESSAGE, Message, Channel);
		return true;
	}

	return false;
}

bool UTwitchSubsystem::SendWhisper(const FString& Username, const FString& Message, const FString Channel)
{
	if(TwitchMessageReceiver.IsValid())
	{
		const FString whisperMessage = FString::Printf(TEXT("/w %s %s"), *Username, *Message);
		TwitchMessageReceiver->SendMessage(ETwitchSendMessageType::CHAT_MESSAGE, whisperMessage, Channel);
		return true;
	}

	return false;
}

void UTwitchSubsystem::JoinChannel(const FString& Channel)
{
	if(!TwitchMessageReceiver.IsValid())
	{
		return;
	}

	TwitchMessageReceiver->SendMessage(ETwitchSendMessageType::JOIN_MESSAGE, TEXT(""), Channel);
}

void UTwitchSubsystem::Disconnect()
{
	if(!TwitchMessageReceiver.IsValid())
	{
		return;
	}
	
	TwitchMessageReceiver->StopConnection(false);
}

bool UTwitchSubsystem::IsConnected() const
{
	return TwitchMessageReceiver.IsValid() && TwitchMessageReceiver->IsConnected();
}

bool UTwitchSubsystem::IsPendingConnection() const
{
	return TwitchMessageReceiver.IsValid() && !TwitchMessageReceiver->IsConnected();
}

bool UTwitchSubsystem::GetConnectionInfo(FString& OutOAuth, FString& OutUsername, FString& OutChannel) const
{
	if(!TwitchMessageReceiver.IsValid())
	{
		return false;
	}

	TwitchMessageReceiver->GetConnectionInfo(OutOAuth, OutUsername, OutChannel);
	return true;
}

void UTwitchSubsystem::SetupEncapsulationChars(const FString& CommandChar, const FString& OptionsChar)
{
	CommandEncapsulationChar = CommandChar;
	OptionsEncapsulationChar = OptionsChar;
}

bool UTwitchSubsystem::RegisterCommand(const FString& CommandName, const FOnCommandReceived& Callback, FString& OutResult)
{
	// No reason to register an empty command
	if (CommandName.IsEmpty())
	{
		OutResult = TEXT("Command type string is invalid");
		return false;
	}

	// Pointer to the command in the event map, if present
	// If the command is found I can use this to switch from the previous function and bind the new one
	FOnCommandReceived* RegisteredCommand = BoundEvents.Find(CommandName);

	// If the command we want to register is already in the event map 
	// copy the new delegate object info into it   
	// For optimization purposes don't delete the entry in order to create a new one.
	if (RegisteredCommand != nullptr)
	{
		*RegisteredCommand = Callback;
		OutResult = CommandName + TEXT(" command registered. It overwrote a previous registration of the same type");
	}
	else
	{
		// If the command is not registered yet create a new entry for it
		// and copy the incoming delegate object info to the new delegate object
		BoundEvents.Add(CommandName, Callback);
		OutResult = CommandName + TEXT(" command registered");
	}
	return true;
}

bool UTwitchSubsystem::UnregisterCommand(const FString& CommandName, FString& OutResult)
{
	// No reason to unregister an empty command 
	if (CommandName == "")
	{
		OutResult = TEXT("Command type string is invalid");
		return false;
	}

	if (BoundEvents.Remove(CommandName) == 0)
	{
		OutResult = TEXT("No command of this type was registered");
		return false;
	}
	
	OutResult = CommandName + TEXT(" unregistered");
	return true;
}

void UTwitchSubsystem::MessageReceivedHandler(const FTwitchChatMessage& Message)
{
	const FString Command = GetCommandString(Message.Message);

	// No reason to search for the command in the event map, there isn't any
	if (Command.IsEmpty())
	{
		return;
	}

	FOnCommandReceived* RegisteredCommand = BoundEvents.Find(Command);

	// If the command was registered proceed with finding any command options
	// Then fire the event
	if (RegisteredCommand != nullptr)
	{
		TArray<FString> CommandOptions;
		GetCommandOptionsStrings(Message.Message, CommandOptions);
		RegisteredCommand->ExecuteIfBound(Command, CommandOptions, Message.Username);
	}
}

FString UTwitchSubsystem::GetDelimitedString(const FString& InString, const FString& Delimiter)
{
	// No delimited string can be found on an empty string
	if (InString.IsEmpty())
	{
		return TEXT("");
	}

	// Where does the delimiter start?
	// Remember that the delimiter can be more than 1 character, so we need to add
	// the delimiter length to find the actual start of the delimited string
	const int32 CommandStartIndex = InString.Find(Delimiter);

	// If the message did not contain any start delimiter no command can be found
	// Also, if the start delimiter is at the end of the string no command can be found
	if (CommandStartIndex == INDEX_NONE || CommandStartIndex + Delimiter.Len() == InString.Len())
	{
		return TEXT("");
	}

	// Search for the end of the command delimiter
	// The starting position for the search is the index of the previous delimiter plus 
	// the actual length of the delimiter (start search from at least one char ahead)
	const int32 CommandEndIndex = InString.Find(Delimiter, ESearchCase::IgnoreCase, ESearchDir::FromStart, CommandStartIndex + Delimiter.Len());

	// If we did not find an end delimiter no encapsulated string can be found
	if (CommandEndIndex == INDEX_NONE)
	{
		return TEXT("");
	}

	// If we have the two delimiter positions get the string in between them
	return InString.Mid(CommandStartIndex + Delimiter.Len(), CommandEndIndex - (CommandStartIndex + Delimiter.Len()));
}

FString UTwitchSubsystem::GetCommandString(const FString& Message) const
{
	// Only the first command is accepted
	FString RetCommand = GetDelimitedString(Message, CommandEncapsulationChar);
	return RetCommand;
}

void UTwitchSubsystem::GetCommandOptionsStrings(const FString& Message, TArray<FString>& OutOptions) const
{
	const FString Options = GetDelimitedString(Message, OptionsEncapsulationChar);
	Options.ParseIntoArray(OutOptions, TEXT(","));
}
