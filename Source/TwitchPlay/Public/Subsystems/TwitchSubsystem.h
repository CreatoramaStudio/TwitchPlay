// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Runnables/TwitchMessageReceiver.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TwitchSubsystem.generated.h"

/**
* Declaration of delegate type for messages received from chat.
* Delegate signature should receive two parameters:
* _message (const FString&) - Message received.
*/
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwitchMessageReceived, const FTwitchChatMessage&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTwitchConnectionMessage, const ETwitchConnectionMessageType, Type, const FString&, Message);


/**
* Declaration of delegate type for commands received from chat.
* Delegate signature should receive three parameters:
* commandName (const FString&) - Name of the command received.
* commandOptions (const TArray<FString>&) - Additional array of options for the command being invoked.
* senderUsername (const FString&) - Username of who triggered the command.
*/
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnCommandReceived, const FString&, CommandName, const TArray<FString>&, CommandOptions, const FString&, SenderUsername);

/**
 * 
 */
UCLASS()
class TWITCHPLAY_API UTwitchSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	// Event called each time a message is received
	UPROPERTY(BlueprintAssignable, Category = "Twitch|Message Events")
	FTwitchMessageReceived OnMessageReceived;

	// Event called each time a connection message occurs.
	// Use this to determine if the connection was successful, or was disconnected, or an error occured.
	// Also includes general server messages from connection commands, join commands, etc.
	UPROPERTY(BlueprintAssignable, Category = "Twitch|Message Events")
	FTwitchConnectionMessage OnConnectionMessage;

	// The seconds delay between sending chat messages. This is set to a safe time by default, but if your bot has elevated
	// permissions you might be able to set this to a shorter time.
	UPROPERTY(EditAnywhere, Category = "Twitch|Setup")
	float TimeBetweenChatMessages;

	
/////////////////// Commands	
	
	// Character to use for command encapsulation. Commands will be read in the form CHAR_Command_CHAR (no spaces or underscores!)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Twitch|Commands Setup")
	FString CommandEncapsulationChar = "!";

	/**
	* Character to use for command options encapsulation. Commands will be read in the form CHAR_Option1[,Option2,..]_CHAR (no spaces or underscores!)
	* Multiple options can be specified and will be split into an FString array upon parsing
	*/
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Twitch|Commands Setup")
	FString OptionsEncapsulationChar = "#";

protected:

	/**
	* Map of the command events currently bound.
	* Each time a new command event is subscribed to, a new map entry is added.
	* For each command only one function will be bound.
	* NOTE: Only methods marked as UFUNCTION can be bound dynamically!
	*
	* TODO: Unbind all events on component destruction? I don't know if it would generate memory leaks if not done
	*/
	UPROPERTY()
	TMap<FString, FOnCommandReceived> BoundEvents;

	

	// Message receiver runnable
	TUniquePtr<FTwitchMessageReceiver> TwitchMessageReceiver;

private:

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	virtual void Deinitialize() override;

	/**
	* Creates a socket and tries to connect to Twitch IRC server.
	*
	* @param OAuth - Oauth token to use. Get one from official Twitch APIs.
	* @param Username - Username to login with. All low caps.
	* @param Channel - The channel to join upon connection. (optional, can call JoinChannel later)
	*/
	UFUNCTION(BlueprintCallable, Category = "Twitch|Setup")
    void Connect(const FString& OAuth, const FString& Username, const FString& Channel);
	
	/**
	 * Send a message on the connected socket
	 * @param Message - The message
	 * @param Channel - The channel (or user channel) to send this message to
	 * @return Whether the message was sent to the worker thread. Check your connection callback for errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Twitch|Messages")
	bool SendChatMessage(const FString& Message, const FString Channel = "");

	/**
	* Send a whisper message to a specific user on a channel on the connected socket
	* NOTE: The user account being used as a bot must have command rights for whispers to work. See the connection
	* log to find out if your user is unable to send whispers in this way.
	* To request bot extended privileges, see https://dev.twitch.tv/limit-increase
	* @param Username - The user to whisper to
	* @param Message - The message
	* @param Channel - The channel (or user channel) to send this message to
	* @return Whether the message was sent to the worker thread. Check your connection callback for errors.
	*/
	UFUNCTION(BlueprintCallable, Category = "Twitch|Messages")
	bool SendWhisper(const FString& Username, const FString& Message, const FString Channel = "");

	/**
	 * If connected, join a new channel. If already in a channel, will leave it before joining the new one.
	 */
	UFUNCTION(BlueprintCallable, Category = "Twitch|Setup")
	void JoinChannel(const FString& Channel);

	/**
	 * If connected, disconnects
	 */
	UFUNCTION(BlueprintCallable, Category = "Twitch|Setup")
	void Disconnect();

	/**
	 * Has a connection been established? Not Pending?
	 */
	UFUNCTION(BlueprintPure, Category = "Twitch|Info")
	bool IsConnected() const;

	/**
	* Establishing a connection?
	* Returns false if connected.
	*/
	UFUNCTION(BlueprintPure, Category = "Twitch|Info")
	bool IsPendingConnection() const;

	/**
	 * Get the current connection info
	 * returns false if not connected
	 */
	UFUNCTION(BlueprintPure, Category = "Twitch|Info")
    bool GetConnectionInfo(FString& OutOAuth, FString& OutUsername, FString& OutChannel) const;


/////////////////// Commands

	/**
	* Setups the encapsulation characters to use for commands and options.
	*
	* @param CommandChar - Character(s) to use to encapsulate commands.
	* @param OptionsChar - Character(s) to use to encapsulate command options.
	*/
	UFUNCTION(BlueprintCallable, Category = "Twitch|Commands")
	void SetupEncapsulationChars(const FString& CommandChar, const FString& OptionsChar);

	/**
	* Registers a command to receive an event whenever that command is called via chat.
	* Only one function associated with a single object can be registered per command (as a delegate pointer!).
	* You actually need to create a delegate, bind it to a function and pass that in by reference.
	* If you try to register another function or another object with the same command the new function of that object will replace the previous one.
	* If you need to fire multiple events when a single command is received consider having just one event calling all the others.
	*
	* @param CommandName - The command to register (CASE SENSITIVE).
	* @param Callback - The function to fire when the event rises.
	*
	* @return Whether the registration was successfully completed.
	*/
	UFUNCTION(BlueprintCallable, Category = "Twitch|Commands")
	bool RegisterCommand(const FString& CommandName, const FOnCommandReceived& Callback);

	/**
	* Unregisters a command to stop receiving events whenever that command is called via chat.
	* Keep in mind that since each command can only be bound to a single function (and single object) unregistering that command will remove any function from any object.
	*
	* @param CommandName - The command to unregister (CASE SENSITIVE).
	*
	* @return Whether the unregistration was successfully completed.
	*/
	UFUNCTION(BlueprintCallable, Category = "Twitch|Commands")
	bool UnregisterCommand(const FString& CommandName);

	UFUNCTION(BlueprintCallable, Category = "Twitch|Commands")
	void UnregisterAllCommands();

	UFUNCTION(BlueprintPure, Category = "Twitch|Commands")
	TArray<FString> GetAllCommandNames() const;

protected:

	/**
	* Handler for when a message is received.
	* Should call the parsing method to search for commands/options and fire the corresponding event.
	*
	* @param Message - The message that was received.
	*
	* NOTE: Method must be marked as UFUNCTION in order to bind a dynamic delegate to it!
	*/
	UFUNCTION()
	void MessageReceivedHandler(const FTwitchChatMessage& Message);

	static FString GetDelimitedString(const FString & InString, const FString & Delimiter);

	/**
	* Parses the message and returns any command associated with the message.
	*
	* @param Message - The message to parse.
	*
	* @return The command found, if any. Returns "" if no command was found.
	*/
	FString GetCommandString(const FString& Message) const;

	/**
	* Parses the message and returns any command options associated with the message.
	*
	* @param Message - The message to parse.
	* @param OutOptions - The array of options found, if any. Returns an empty array if no command option was found.
	*
	*/
	void GetCommandOptionsStrings(const FString& Message, TArray<FString>& OutOptions) const;

private:
	
};
