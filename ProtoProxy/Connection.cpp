
// Connection.cpp

// Interfaces to the cConnection class representing a single pair of connected sockets

#include "Globals.h"
#include "Connection.h"
#include "Server.h"
#include <iostream>





#define HANDLE_CLIENT_PACKET_READ(Proc, Type, Var) \
	Type Var; \
	{ \
		if (!m_ClientBuffer.Proc(Var)) \
		{ \
			return false; \
		} \
	}

#define HANDLE_SERVER_PACKET_READ(Proc, Type, Var) \
	Type Var; \
	{ \
		if (!m_ServerBuffer.Proc(Var)) \
		{ \
			return false; \
		} \
	}

#define CLIENTSEND(...) SendData(m_ClientSocket, __VA_ARGS__, "Client")
#define SERVERSEND(...) SendData(m_ServerSocket, __VA_ARGS__, "Server")
#define CLIENTENCRYPTSEND(...) SendEncryptedData(m_ClientSocket, m_ClientEncryptor, __VA_ARGS__, "Client")
#define SERVERENCRYPTSEND(...) SendEncryptedData(m_ServerSocket, m_ServerEncryptor, __VA_ARGS__, "Server")

#define COPY_TO_SERVER() \
	{ \
		AString ToServer; \
		m_ClientBuffer.ReadAgain(ToServer); \
		if (m_ServerState == csUnencrypted) \
		{ \
			SERVERSEND(ToServer.data(), ToServer.size()); \
		} \
		else \
		{ \
			SERVERENCRYPTSEND(ToServer.data(), ToServer.size()); \
		} \
	}

#define COPY_TO_CLIENT() \
	{ \
		AString ToClient; \
		m_ServerBuffer.ReadAgain(ToClient); \
		if (m_ClientState == csUnencrypted) \
		{ \
			CLIENTSEND(ToClient.data(), ToClient.size()); \
		} \
		else \
		{ \
			CLIENTENCRYPTSEND(ToClient.data(), ToClient.size()); \
		} \
	}

#define HANDLE_CLIENT_READ(Proc) \
	{ \
		if (!Proc()) \
		{ \
			AString Leftover; \
			m_ClientBuffer.ReadAgain(Leftover); \
			DataLog(Leftover.data(), Leftover.size(), "Leftover data after client packet parsing, %d bytes:", Leftover.size()); \
			m_ClientBuffer.ResetRead(); \
			return true; \
		} \
	}
	
#define HANDLE_SERVER_READ(Proc) \
	{ \
		if (!Proc()) \
		{ \
			m_ServerBuffer.ResetRead(); \
			return true; \
		} \
	}
	
	
#define MAX_ENC_LEN 1024




typedef unsigned char Byte;





enum
{
	PACKET_KEEPALIVE               = 0x00,
	PACKET_LOGIN                   = 0x01,
	PACKET_HANDSHAKE               = 0x02,
	PACKET_CHAT_MESSAGE            = 0x03,
	PACKET_TIME_UPDATE             = 0x04,
	PACKET_ENTITY_EQUIPMENT        = 0x05,
	PACKET_COMPASS                 = 0x06,
	PACKET_UPDATE_HEALTH           = 0x08,
	PACKET_PLAYER_ON_GROUND        = 0x0a,
	PACKET_PLAYER_POSITION         = 0x0b,
	PACKET_PLAYER_LOOK             = 0x0c,
	PACKET_PLAYER_POSITION_LOOK    = 0x0d,
	PACKET_BLOCK_PLACE             = 0x0f,
	PACKET_SLOT_SELECT             = 0x10,
	PACKET_ANIMATION               = 0x12,
	PACKET_MAP_CHUNK               = 0x33,
	PACKET_MULTI_BLOCK_CHANGE      = 0x34,
	PACKET_BLOCK_CHANGE            = 0x35,
	PACKET_WINDOW_CONTENTS         = 0x68,
	PACKET_UPDATE_SIGN             = 0x82,
	PACKET_PLAYER_LIST_ITEM        = 0xc9,
	PACKET_PLAYER_ABILITIES        = 0xca,
	PACKET_LOCALE_AND_VIEW         = 0xcc,
	PACKET_CLIENT_STATUSES         = 0xcd,
	PACKET_ENCRYPTION_KEY_RESPONSE = 0xfc,
	PACKET_ENCRYPTION_KEY_REQUEST  = 0xfd,
	PACKET_PING                    = 0xfe,
	PACKET_KICK                    = 0xff,
} ;





///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cConnection:

cConnection::cConnection(SOCKET a_ClientSocket, cServer & a_Server) :
	m_Server(a_Server),
	m_LogFile(NULL),
	m_ClientSocket(a_ClientSocket),
	m_ServerSocket(-1),
	m_BeginTick(clock()),
	m_ClientState(csUnencrypted),
	m_ServerState(csUnencrypted),
	m_ClientBuffer(1024 KiB),
	m_ServerBuffer(1024 KiB),
	m_Nonce(0)
{
	AString fnam;
	Printf(fnam, "Log_%d.log", (int)time(NULL));
	m_LogFile = fopen(fnam.c_str(), "w");
	Log("Log file created");
}





cConnection::~cConnection()
{
	fclose(m_LogFile);
}





void cConnection::Run(void)
{
	if (!ConnectToServer())
	{
		Log("Cannot connect to server; aborting");
		return;
	}
	
	while (true)
	{
		fd_set ReadFDs;
		FD_ZERO(&ReadFDs);
		FD_SET(m_ServerSocket, &ReadFDs);
		FD_SET(m_ClientSocket, &ReadFDs);
		int res = select(2, &ReadFDs, NULL, NULL, NULL);
		if (res <= 0)
		{
			printf("select() failed: %d; aborting client", WSAGetLastError());
			break;
		}
		if (FD_ISSET(m_ServerSocket, &ReadFDs))
		{
			if (!RelayFromServer())
			{
				break;
			}
		}
		if (FD_ISSET(m_ClientSocket, &ReadFDs))
		{
			if (!RelayFromClient())
			{
				break;
			}
		}
	}
	Log("Relaying ended, closing sockets");
	closesocket(m_ServerSocket);
	closesocket(m_ClientSocket);
}





void cConnection::Log(const char * a_Format, ...)
{
	va_list args;
	va_start(args, a_Format);
	AString msg;
	AppendVPrintf(msg, a_Format, args);
	va_end(args);
	AString FullMsg;
	Printf(FullMsg, "[%5.3f] %s\n", GetRelativeTime(), msg.c_str());
	
	// Log to file:
	cCSLock Lock(m_CSLog);
	fputs(FullMsg.c_str(), m_LogFile);
	
	// Log to screen:
	std::cout << FullMsg;
}





void cConnection::DataLog(const void * a_Data, int a_Size, const char * a_Format, ...)
{
	va_list args;
	va_start(args, a_Format);
	AString msg;
	AppendVPrintf(msg, a_Format, args);
	va_end(args);
	AString FullMsg;
	AString Hex;
	Printf(FullMsg, "[%5.3f] %s\n%s\n", GetRelativeTime(), msg.c_str(), CreateHexDump(Hex, a_Data, a_Size, 16).c_str());
	
	// Log to file:
	cCSLock Lock(m_CSLog);
	fputs(FullMsg.c_str(), m_LogFile);
	
	/*
	// Log to screen:
	std::cout << FullMsg;
	//*/
}





bool cConnection::ConnectToServer(void)
{
	m_ServerSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_ServerSocket == INVALID_SOCKET)
	{
		return false;
	}
	sockaddr_in localhost;
	localhost.sin_family = AF_INET;
	localhost.sin_port = htons(m_Server.GetConnectPort());
	localhost.sin_addr.s_addr = htonl(0x7f000001);  // localhost
	if (connect(m_ServerSocket, (sockaddr *)&localhost, sizeof(localhost)) != 0)
	{
		printf("connection to server failed: %d\n", WSAGetLastError());
		return false;
	}
	Log("Connected to SERVER");
	return true;
}





bool cConnection::RelayFromServer(void)
{
	char Buffer[64 KiB];
	int res = recv(m_ServerSocket, Buffer, sizeof(Buffer), 0);
	if (res <= 0)
	{
		Log("Server closed the socket: %d; %d; aborting connection", res, WSAGetLastError());
		return false;
	}
	
	DataLog(Buffer, res, "Received %d bytes from the SERVER", res);
	
	switch (m_ServerState)
	{
		case csUnencrypted:
		{
			return DecodeServersPackets(Buffer, res);
		}
		case csEncryptedUnderstood:
		{
			m_ServerDecryptor.ProcessData((byte *)Buffer, (byte *)Buffer, res);
			DataLog(Buffer, res, "Decrypted %d bytes from the SERVER", res);
			return DecodeServersPackets(Buffer, res);
		}
		case csEncryptedUnknown:
		{
			m_ServerDecryptor.ProcessData((byte *)Buffer, (byte *)Buffer, res);
			DataLog(Buffer, res, "Decrypted %d bytes from the SERVER", res);
			m_ClientEncryptor.ProcessData((byte *)Buffer, (byte *)Buffer, res);
			return CLIENTSEND(Buffer, res);
		}
	}
	
	return true;
}





bool cConnection::RelayFromClient(void)
{
	char Buffer[64 KiB];
	int res = recv(m_ClientSocket, Buffer, sizeof(Buffer), 0);
	if (res <= 0)
	{
		Log("Client closed the socket: %d; %d; aborting connection", res, WSAGetLastError());
		return false;
	}
	
	DataLog(Buffer, res, "Received %d bytes from the CLIENT", res);
	
	switch (m_ClientState)
	{
		case csUnencrypted:
		{
			return DecodeClientsPackets(Buffer, res);
		}
		case csEncryptedUnderstood:
		{
			m_ClientDecryptor.ProcessData((byte *)Buffer, (byte *)Buffer, res);
			DataLog(Buffer, res, "Decrypted %d bytes from the CLIENT", res);
			return DecodeClientsPackets(Buffer, res);
		}
		case csEncryptedUnknown:
		{
			m_ClientDecryptor.ProcessData((byte *)Buffer, (byte *)Buffer, res);
			DataLog(Buffer, res, "Decrypted %d bytes from the CLIENT", res);
			m_ServerEncryptor.ProcessData((byte *)Buffer, (byte *)Buffer, res);
			return SERVERSEND(Buffer, res);
		}
	}
	
	return true;
}





double cConnection::GetRelativeTime(void)
{
	return (double)(clock() - m_BeginTick) / CLOCKS_PER_SEC;
	
}





bool cConnection::SendData(SOCKET a_Socket, const char * a_Data, int a_Size, const char * a_Peer)
{
	DataLog(a_Data, a_Size, "Sending data to %s", a_Peer);
	
	int res = send(a_Socket, a_Data, a_Size, 0);
	if (res <= 0)
	{
		Log("%s closed the socket: %d, %d; aborting connection", a_Peer, res, WSAGetLastError());
		return false;
	}
	return true;
}





bool cConnection::SendData(SOCKET a_Socket, cByteBuffer & a_Data, const char * a_Peer)
{
	AString All;
	a_Data.ReadAll(All);
	a_Data.CommitRead();
	return SendData(a_Socket, All.data(), All.size(), a_Peer);
}





bool cConnection::SendEncryptedData(SOCKET a_Socket, Encryptor & a_Encryptor, const char * a_Data, int a_Size, const char * a_Peer)
{
	DataLog(a_Data, a_Size, "Encrypting %d bytes to %s", a_Size, a_Peer);
	const byte * Data = (const byte *)a_Data;
	while (a_Size > 0)
	{
		byte Buffer[64 KiB];
		int NumBytes = (a_Size > sizeof(Buffer)) ? sizeof(Buffer) : a_Size;
		a_Encryptor.ProcessData(Buffer, Data, NumBytes);
		bool res = SendData(a_Socket, (const char *)Buffer, NumBytes, a_Peer);
		if (!res)
		{
			return false;
		}
		Data += NumBytes;
		a_Size -= NumBytes;
	}
	return true;
}





bool cConnection::SendEncryptedData(SOCKET a_Socket, Encryptor & a_Encryptor, cByteBuffer & a_Data, const char * a_Peer)
{
	AString All;
	a_Data.ReadAll(All);
	a_Data.CommitRead();
	return SendEncryptedData(a_Socket, a_Encryptor, All.data(), All.size(), a_Peer);
}





bool cConnection::DecodeClientsPackets(const char * a_Data, int a_Size)
{
	if (!m_ClientBuffer.Write(a_Data, a_Size))
	{
		Log("Too much queued data for the server, aborting connection");
		return false;
	}
	
	while (m_ClientBuffer.CanReadBytes(1))
	{
		Log("Decoding client's packets, there are now %d bytes in the queue", m_ClientBuffer.GetReadableSpace());
		unsigned char PacketType;
		m_ClientBuffer.ReadByte(PacketType);
		switch (PacketType)
		{
			case PACKET_ANIMATION:               HANDLE_CLIENT_READ(HandleClientAnimation); break;
			case PACKET_BLOCK_PLACE:             HANDLE_CLIENT_READ(HandleClientBlockPlace); break;
			case PACKET_CLIENT_STATUSES:         HANDLE_CLIENT_READ(HandleClientClientStatuses); break;
			case PACKET_ENCRYPTION_KEY_RESPONSE: HANDLE_CLIENT_READ(HandleClientEncryptionKeyResponse); break;
			case PACKET_HANDSHAKE:               HANDLE_CLIENT_READ(HandleClientHandshake); break;
			case PACKET_KEEPALIVE:               HANDLE_CLIENT_READ(HandleClientKeepAlive); break;
			case PACKET_LOCALE_AND_VIEW:         HANDLE_CLIENT_READ(HandleClientLocaleAndView); break;
			case PACKET_PING:                    HANDLE_CLIENT_READ(HandleClientPing); break;
			case PACKET_PLAYER_LOOK:             HANDLE_CLIENT_READ(HandleClientPlayerLook); break;
			case PACKET_PLAYER_ON_GROUND:        HANDLE_CLIENT_READ(HandleClientPlayerOnGround); break;
			case PACKET_PLAYER_POSITION:         HANDLE_CLIENT_READ(HandleClientPlayerPosition); break;
			case PACKET_PLAYER_POSITION_LOOK:    HANDLE_CLIENT_READ(HandleClientPlayerPositionLook); break;
			case PACKET_SLOT_SELECT:             HANDLE_CLIENT_READ(HandleClientSlotSelect); break;
			case PACKET_UPDATE_SIGN:             HANDLE_CLIENT_READ(HandleClientUpdateSign); break;
			default:
			{
				if (m_ClientState == csEncryptedUnderstood)
				{
					Log("Unknown packet 0x%02x from the client while encrypted; continuing to relay blind only", PacketType);
					AString Data;
					m_ClientBuffer.ResetRead();
					m_ClientBuffer.ReadAll(Data);
					DataLog(Data.data(), Data.size(), "Current data in the client packet queue: %d bytes", Data.size());
					m_ClientState = csEncryptedUnknown;
					m_ClientBuffer.ResetRead();
					if (m_ServerState == csUnencrypted)
					{
						SERVERSEND(m_ClientBuffer);
					}
					else
					{
						SERVERENCRYPTSEND(m_ClientBuffer);
					}
					return true;
				}
				else
				{
					Log("Unknown packet 0x%02x from the client while unencrypted; aborting connection", PacketType);
					return false;
				}
			}
		}  // switch (PacketType)
		m_ClientBuffer.CommitRead();
	}  // while (CanReadBytes(1))
	return true;
}





bool cConnection::DecodeServersPackets(const char * a_Data, int a_Size)
{
	if (!m_ServerBuffer.Write(a_Data, a_Size))
	{
		Log("Too much queued data for the client, aborting connection");
		return false;
	}
	
	if (
		(m_ServerState == csEncryptedUnderstood) &&
		(m_ClientState == csUnencrypted)
	)
	{
		// Client hasn't finished encryption handshake yet, don't send them any data yet
	}
	
	while (m_ServerBuffer.CanReadBytes(1))
	{
		Log("Decoding server's packets, there are now %d bytes in the queue", m_ServerBuffer.GetReadableSpace());
		unsigned char PacketType;
		m_ServerBuffer.ReadByte(PacketType);
		switch (PacketType)
		{
			case PACKET_BLOCK_CHANGE:            HANDLE_SERVER_READ(HandleServerBlockChange); break;
			case PACKET_CHAT_MESSAGE:            HANDLE_SERVER_READ(HandleServerChatMessage); break;
			case PACKET_COMPASS:                 HANDLE_SERVER_READ(HandleServerCompass); break;
			case PACKET_ENCRYPTION_KEY_REQUEST:  HANDLE_SERVER_READ(HandleServerEncryptionKeyRequest); break;
			case PACKET_ENCRYPTION_KEY_RESPONSE: HANDLE_SERVER_READ(HandleServerEncryptionKeyResponse); break;
			case PACKET_ENTITY_EQUIPMENT:        HANDLE_SERVER_READ(HandleServerEntityEquipment); break;
			case PACKET_KEEPALIVE:               HANDLE_SERVER_READ(HandleServerKeepAlive); break;
			case PACKET_KICK:                    HANDLE_SERVER_READ(HandleServerKick); break;
			case PACKET_LOGIN:                   HANDLE_SERVER_READ(HandleServerLogin); break;
			case PACKET_MAP_CHUNK:               HANDLE_SERVER_READ(HandleServerMapChunk); break;
			case PACKET_MULTI_BLOCK_CHANGE:      HANDLE_SERVER_READ(HandleServerMultiBlockChange); break;
			case PACKET_PLAYER_ABILITIES:        HANDLE_SERVER_READ(HandleServerPlayerAbilities); break;
			case PACKET_PLAYER_LIST_ITEM:        HANDLE_SERVER_READ(HandleServerPlayerListItem); break;
			case PACKET_PLAYER_POSITION_LOOK:    HANDLE_SERVER_READ(HandleServerPlayerPositionLook); break;
			case PACKET_TIME_UPDATE:             HANDLE_SERVER_READ(HandleServerTimeUpdate); break;
			case PACKET_UPDATE_HEALTH:           HANDLE_SERVER_READ(HandleServerUpdateHealth); break;
			case PACKET_UPDATE_SIGN:             HANDLE_SERVER_READ(HandleServerUpdateSign); break;
			case PACKET_WINDOW_CONTENTS:         HANDLE_SERVER_READ(HandleServerWindowContents); break;
			default:
			{
				if (m_ServerState == csEncryptedUnderstood)
				{
					Log("Unknown packet 0x%02x from the server while encrypted; continuing to relay blind only", PacketType);
					AString Data;
					m_ServerBuffer.ResetRead();
					m_ServerBuffer.ReadAll(Data);
					DataLog(Data.data(), Data.size(), "Current data in the server packet queue: %d bytes", Data.size());
					m_ServerState = csEncryptedUnknown;
					m_ServerBuffer.ResetRead();
					if (m_ClientState == csUnencrypted)
					{
						CLIENTSEND(m_ServerBuffer);
					}
					else
					{
						CLIENTENCRYPTSEND(m_ServerBuffer);
					}
					return true;
				}
				else
				{
					Log("Unknown packet 0x%02x from the server while unencrypted; aborting connection", PacketType);
					return false;
				}
			}
		}  // switch (PacketType)
		m_ServerBuffer.CommitRead();
	}  // while (CanReadBytes(1))
	return true;
}





bool cConnection::HandleClientAnimation(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEInt, int,  EntityID);
	HANDLE_CLIENT_PACKET_READ(ReadChar,  char, Animation);
	Log("Received a PACKET_ANIMATION from the client:");
	Log("  EntityID: %d", EntityID);
	Log("  Animation: %d", Animation);
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientBlockPlace(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEInt, int,  BlockX);
	HANDLE_CLIENT_PACKET_READ(ReadByte,  Byte, BlockY);
	HANDLE_CLIENT_PACKET_READ(ReadBEInt, int,  BlockZ);
	HANDLE_CLIENT_PACKET_READ(ReadChar,  char, Face);
	AString Desc;
	if (!ParseSlot(m_ClientBuffer, Desc))
	{
		return false;
	}
	HANDLE_CLIENT_PACKET_READ(ReadChar,  char, CursorX);
	HANDLE_CLIENT_PACKET_READ(ReadChar,  char, CursorY);
	HANDLE_CLIENT_PACKET_READ(ReadChar,  char, CursorZ);
	Log("Received a PACKET_BLOCK_PLACE from the client:");
	Log("  Block = {%d, %d, %d}", BlockX, BlockY, BlockZ);
	Log("  Face = %d", Face);
	Log("  Item = %s", Desc.c_str());
	Log("  Cursor = <%d, %d, %d>", CursorX, CursorY, CursorZ);
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientClientStatuses(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadChar, char, Statuses);
	Log("Received a PACKET_CLIENT_STATUSES from the CLIENT:");
	Log("  Statuses = %d", Statuses);
	
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientEncryptionKeyResponse(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEShort, short, EncKeyLength);
	AString EncKey;
	if (!m_ClientBuffer.ReadString(EncKey, EncKeyLength))
	{
		return true;
	}
	HANDLE_CLIENT_PACKET_READ(ReadBEShort, short, EncNonceLength);
	AString EncNonce;
	if (!m_ClientBuffer.ReadString(EncNonce, EncNonceLength))
	{
		return true;
	}
	if ((EncKeyLength > MAX_ENC_LEN) || (EncNonceLength > MAX_ENC_LEN))
	{
		Log("Client: Too long encryption params");
		return true;
	}
	StartClientEncryption(EncKey, EncNonce);
	return true;
}





bool cConnection::HandleClientHandshake(void)
{
	// Read the packet from the client:
	HANDLE_CLIENT_PACKET_READ(ReadByte,            Byte,    ProtocolVersion);
	HANDLE_CLIENT_PACKET_READ(ReadBEUTF16String16, AString, Username);
	HANDLE_CLIENT_PACKET_READ(ReadBEUTF16String16, AString, ServerHost);
	HANDLE_CLIENT_PACKET_READ(ReadBEInt,           int,     ServerPort);
	m_ClientBuffer.CommitRead();
	
	// Send the same packet to the server, but with our port:
	cByteBuffer ToServer(512);
	ToServer.WriteByte           (PACKET_HANDSHAKE);
	ToServer.WriteByte           (ProtocolVersion);
	ToServer.WriteBEUTF16String16(Username);
	ToServer.WriteBEUTF16String16(ServerHost);
	ToServer.WriteBEInt          (m_Server.GetConnectPort());
	SERVERSEND(ToServer);
	return true;
}





bool cConnection::HandleClientKeepAlive(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEInt, int, ID);
	Log("Received a PACKET_KEEPALIVE from the client");
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientLocaleAndView(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEUTF16String16, AString, Locale);
	HANDLE_CLIENT_PACKET_READ(ReadChar,            char,    ViewDistance);
	HANDLE_CLIENT_PACKET_READ(ReadChar,            char,    ChatFlags);
	HANDLE_CLIENT_PACKET_READ(ReadChar,            char,    Difficulty);
	Log("Received a PACKET_LOCALE_AND_VIEW from the client");
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientPing(void)
{
	Log("Received a PACKET_PING from the client");
	m_ClientBuffer.ResetRead();
	SERVERSEND(m_ClientBuffer);
	return true;
}





bool cConnection::HandleClientPlayerLook(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEFloat, float, Yaw);
	HANDLE_CLIENT_PACKET_READ(ReadBEFloat, float, Pitch);
	HANDLE_CLIENT_PACKET_READ(ReadChar,    char,  OnGround);
	Log("Received a PACKET_PLAYER_LOOK from the client");
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientPlayerOnGround(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadChar,    char,  OnGround);
	Log("Received a PACKET_PLAYER_ON_GROUND from the client");
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientPlayerPosition(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, PosX);
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, Stance);
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, PosY);
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, PosZ);
	HANDLE_CLIENT_PACKET_READ(ReadChar,     char,   IsOnGround);
	Log("Received a PACKET_PLAYER_POSITION from the client");

	// TODO: list packet contents
	
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientPlayerPositionLook(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, PosX);
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, Stance);
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, PosY);
	HANDLE_CLIENT_PACKET_READ(ReadBEDouble, double, PosZ);
	HANDLE_CLIENT_PACKET_READ(ReadBEFloat,  float,  Yaw);
	HANDLE_CLIENT_PACKET_READ(ReadBEFloat,  float,  Pitch);
	HANDLE_CLIENT_PACKET_READ(ReadChar,     char,   IsOnGround);
	Log("Received a PACKET_PLAYER_POSITION_LOOK from the client");

	// TODO: list packet contents
	
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientSlotSelect(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEShort, short, SlotNum);
	Log("Received a PACKET_SLOT_SELECT from the client");
	Log("  SlotNum = %d", SlotNum);
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleClientUpdateSign(void)
{
	HANDLE_CLIENT_PACKET_READ(ReadBEInt,           int,   BlockX);
	HANDLE_CLIENT_PACKET_READ(ReadBEShort,         short, BlockY);
	HANDLE_CLIENT_PACKET_READ(ReadBEInt,           int,   BlockZ);
	HANDLE_CLIENT_PACKET_READ(ReadBEUTF16String16, AString, Line1);
	HANDLE_CLIENT_PACKET_READ(ReadBEUTF16String16, AString, Line2);
	HANDLE_CLIENT_PACKET_READ(ReadBEUTF16String16, AString, Line3);
	HANDLE_CLIENT_PACKET_READ(ReadBEUTF16String16, AString, Line4);
	Log("Received a PACKET_UPDATE_SIGN from the client:");
	Log("  Block = {%d, %d, %d}", BlockX, BlockY, BlockZ);
	Log("  Lines = \"%s\", \"%s\", \"%s\", \"%s\"", Line1.c_str(), Line2.c_str(), Line3.c_str(), Line4.c_str());
	COPY_TO_SERVER();
	return true;
}





bool cConnection::HandleServerBlockChange(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   BlockX);
	HANDLE_SERVER_PACKET_READ(ReadByte,    Byte,  BlockY);
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   BlockZ);
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, BlockType);
	HANDLE_SERVER_PACKET_READ(ReadChar,    char,  BlockMeta);
	Log("Received a PACKET_BLOCK_CHANGE from the server");
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerChatMessage(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, Message);
	Log("Received a PACKET_CHAT_MESSAGE from the server:");
	Log("  Message = \"%s\"", Message);
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerCompass(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt, int, SpawnX);
	HANDLE_SERVER_PACKET_READ(ReadBEInt, int, SpawnY);
	HANDLE_SERVER_PACKET_READ(ReadBEInt, int, SpawnZ);
	Log("Received PACKET_COMPASS from the server:");
	Log("  Spawn = {%d, %d, %d}", SpawnX, SpawnY, SpawnZ);
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerEncryptionKeyRequest(void)
{
	// Read the packet from the server:
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, ServerID);
	HANDLE_SERVER_PACKET_READ(ReadBEShort,         short,   PublicKeyLength);
	AString PublicKey;
	if (!m_ServerBuffer.ReadString(PublicKey, PublicKeyLength))
	{
		return false;
	}
	HANDLE_SERVER_PACKET_READ(ReadBEShort,         short,   NonceLength);
	AString Nonce;
	if (!m_ServerBuffer.ReadString(Nonce, NonceLength))
	{
		return false;
	}
	Log("Got PACKET_ENCRYPTION_KEY_REQUEST from the SERVER:");
	Log("  ServerID = %s", ServerID.c_str());
	
	// Reply to the server:
	SendEncryptionKeyResponse(PublicKey, Nonce);
	
	// Send a 0xFD Encryption Key Request http://wiki.vg/Protocol#0xFD to the client, using our own key:
	Log("Sending PACKET_ENCRYPTION_KEY_REQUEST to the CLIENT");
	AString key;
	StringSink sink(key);  // GCC won't allow inline instantiation in the following line, damned temporary refs
	m_Server.GetPublicKey().Save(sink);
	cByteBuffer ToClient(512);
	ToClient.WriteByte           (PACKET_ENCRYPTION_KEY_REQUEST);
	ToClient.WriteBEUTF16String16(ServerID);
	ToClient.WriteBEShort        ((short)key.size());
	ToClient.WriteBuf            (key.data(), key.size());
	ToClient.WriteBEShort        (4);
	ToClient.WriteBEInt          (m_Nonce);  // Using 'this' as the cryptographic nonce, so that we don't have to generate one each time :)
	CLIENTSEND(ToClient);
	return true;
}





bool cConnection::HandleServerEntityEquipment(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   EntityID);
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, SlotNum);
	AString Item;
	if (!ParseSlot(m_ServerBuffer, Item))
	{
		return false;
	}
	Log("Received a PACKET_ENTITY_EQUIPMENT from the server:");
	Log("  EntityID = %d", EntityID);
	Log("  SlotNum = %d", SlotNum);
	Log("  Item = %s", Item.c_str());
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerKeepAlive(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt, int, PingID);
	Log("Received a PACKET_KEEP_ALIVE from the server:");
	Log("  ID = %d", PingID);
	COPY_TO_CLIENT()
	return true;
}





bool cConnection::HandleServerEncryptionKeyResponse(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt, int, Lengths);
	if (Lengths != 0)
	{
		Log("Lengths are not zero!");
		return true;
	}
	Log("Server communication is now encrypted");
	m_ServerState = csEncryptedUnderstood;
	return true;
}





bool cConnection::HandleServerKick(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, Reason);
	Log("Received PACKET_KICK from the SERVER:");
	Log("  Reason = \"%s\"", Reason.c_str());
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerLogin(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt,           int,     EntityID);
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, LevelType);
	HANDLE_SERVER_PACKET_READ(ReadChar,            char,    GameMode);
	HANDLE_SERVER_PACKET_READ(ReadChar,            char,    Dimension);
	HANDLE_SERVER_PACKET_READ(ReadChar,            char,    Difficulty);
	HANDLE_SERVER_PACKET_READ(ReadChar,            char,    Unused);
	HANDLE_SERVER_PACKET_READ(ReadChar,            char,    MaxPlayers);
	Log("Received a PACKET_LOGIN from the server:");
	Log("  EntityID = %d",      EntityID);
	Log("  LevelType = \"%s\"", LevelType.c_str());
	Log("  GameMode = %d",      GameMode);
	Log("  Dimension = %d",     Dimension);
	Log("  Difficulty = %d",    Difficulty);
	Log("  Unused = %d",        Unused);
	Log("  MaxPlayers = %d",    MaxPlayers);
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerMapChunk(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   ChunkX);
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   ChunkZ);
	HANDLE_SERVER_PACKET_READ(ReadChar,    char,  IsContiguous);
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, PrimaryBitmap);
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, AdditionalBitmap);
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   CompressedSize);
	AString CompressedData;
	if (!m_ServerBuffer.ReadString(CompressedData, CompressedSize))
	{
		return false;
	}
	Log("Received a PACKET_MAP_CHUNK from the server:");
	Log("  ChunkPos = [%d, %d]", ChunkX, ChunkZ);
	Log("  Compressed size = %d (0x%x)", CompressedSize, CompressedSize);
	
	// TODO: Save the compressed data into a file for later analysis
	
	COPY_TO_CLIENT()
	return true;
}





bool cConnection::HandleServerMultiBlockChange(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   ChunkX);
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   ChunkZ);
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, NumBlocks);
	HANDLE_SERVER_PACKET_READ(ReadBEInt,   int,   DataSize);
	AString BlockChangeData;
	if (!m_ServerBuffer.ReadString(BlockChangeData, DataSize))
	{
		return false;
	}
	Log("Received a PACKET_MULTI_BLOCK_CHANGE packet from the server:");
	Log("  Chunk = [%d, %d]", ChunkX, ChunkZ);
	Log("  NumBlocks = %d", NumBlocks);
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerPlayerAbilities(void)
{
	HANDLE_SERVER_PACKET_READ(ReadChar, char, Flags);
	HANDLE_SERVER_PACKET_READ(ReadChar, char, FlyingSpeed);
	HANDLE_SERVER_PACKET_READ(ReadChar, char, WalkingSpeed);
	Log("Received a PACKET_PLAYER_ABILITIES from the server:");
	Log("  Flags = 0x%x", Flags);
	Log("  FlyingSpeed = %d", FlyingSpeed);
	Log("  WalkingSpeed = %d", WalkingSpeed);
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerPlayerListItem(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, PlayerName);
	HANDLE_SERVER_PACKET_READ(ReadChar,            char,    IsOnline);
	HANDLE_SERVER_PACKET_READ(ReadBEShort,         short,   Ping);
	Log("Received a PACKET_PLAYERLIST_ITEM from the server:");
	Log("  PlayerName = \"%s\"", PlayerName.c_str());
	Log("  Ping = %d", Ping);
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerPlayerPositionLook(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEDouble, double, PosX);
	HANDLE_SERVER_PACKET_READ(ReadBEDouble, double, Stance);
	HANDLE_SERVER_PACKET_READ(ReadBEDouble, double, PosY);
	HANDLE_SERVER_PACKET_READ(ReadBEDouble, double, PosZ);
	HANDLE_SERVER_PACKET_READ(ReadBEFloat,  float,  Yaw);
	HANDLE_SERVER_PACKET_READ(ReadBEFloat,  float,  Pitch);
	HANDLE_SERVER_PACKET_READ(ReadChar,     char,   IsOnGround);
	Log("Received a PACKET_PLAYER_POSITION_LOOK from the server");

	// TODO: list packet contents
	
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerTimeUpdate(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt64, Int64, Time);
	Log("Received a PACKET_TIME_UPDATE from the server");
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerUpdateHealth(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, Health);
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, Food);
	HANDLE_SERVER_PACKET_READ(ReadBEFloat, float, Saturation);
	Log("Received a PACKET_UPDATE_HEALTH from the server");
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerUpdateSign(void)
{
	HANDLE_SERVER_PACKET_READ(ReadBEInt,           int,   BlockX);
	HANDLE_SERVER_PACKET_READ(ReadBEShort,         short, BlockY);
	HANDLE_SERVER_PACKET_READ(ReadBEInt,           int,   BlockZ);
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, Line1);
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, Line2);
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, Line3);
	HANDLE_SERVER_PACKET_READ(ReadBEUTF16String16, AString, Line4);
	Log("Received a PACKET_UPDATE_SIGN from the server:");
	Log("  Block = {%d, %d, %d}", BlockX, BlockY, BlockZ);
	Log("  Lines = \"%s\", \"%s\", \"%s\", \"%s\"", Line1.c_str(), Line2.c_str(), Line3.c_str(), Line4.c_str());
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::HandleServerWindowContents(void)
{
	HANDLE_SERVER_PACKET_READ(ReadChar, char, WindowID);
	HANDLE_SERVER_PACKET_READ(ReadBEShort, short, NumSlots);
	AStringVector Items;
	for (short i = 0; i < NumSlots; i++)
	{
		AString Item;
		if (!ParseSlot(m_ServerBuffer, Item))
		{
			return false;
		}
		Items.push_back(Item);
	}
	Log("Received a PACKET_WINDOW_CONTENTS from the server:");
	Log("  WindowID = %d", WindowID);
	Log("  NumSlots = %d", NumSlots);

	// TODO: list items
	
	COPY_TO_CLIENT();
	return true;
}





bool cConnection::ParseSlot(cByteBuffer & a_Buffer, AString & a_ItemDesc)
{
	short ItemType;
	if (!a_Buffer.ReadBEShort(ItemType))
	{
		return false;
	}
	if (ItemType <= 0)
	{
		a_ItemDesc = "<empty>";
		return true;
	}
	if (!a_Buffer.CanReadBytes(5))
	{
		return false;
	}
	char ItemCount;
	short ItemDamage;
	short MetadataLength;
	a_Buffer.ReadChar(ItemCount);
	a_Buffer.ReadBEShort(ItemDamage);
	a_Buffer.ReadBEShort(MetadataLength);
	Printf(a_ItemDesc, "%d:%d * %d", ItemType, ItemDamage, ItemCount);
	if (MetadataLength <= 0)
	{
		return true;
	}
	AppendPrintf(a_ItemDesc, " (%d bytes of meta)", MetadataLength);
	if (!a_Buffer.SkipRead(MetadataLength))
	{
		return false;
	}
	return true;
}





void cConnection::SendEncryptionKeyResponse(const AString & a_ServerPublicKey, const AString & a_Nonce)
{
	// Generate the shared secret and encrypt using the server's public key
	byte SharedSecret[16];
	byte EncryptedSecret[128];
	memset(SharedSecret, 0, sizeof(SharedSecret));  // Use all zeroes for the initial secret
	RSA::PublicKey pk;
	CryptoPP::StringSource src(a_ServerPublicKey, true);
	ByteQueue bq;
	src.TransferTo(bq);
	bq.MessageEnd();
	pk.Load(bq);
	RSAES<PKCS1v15>::Encryptor rsaEncryptor(pk);
	RandomPool rng;
	time_t CurTime = time(NULL);
	rng.Put((const byte *)&CurTime, sizeof(CurTime));
	int EncryptedLength = rsaEncryptor.FixedCiphertextLength();
	ASSERT(EncryptedLength <= sizeof(EncryptedSecret));
	rsaEncryptor.Encrypt(rng, SharedSecret, sizeof(SharedSecret), EncryptedSecret);
	m_ServerEncryptor.SetKey(SharedSecret, 16, MakeParameters(Name::IV(), ConstByteArrayParameter(SharedSecret, 16))(Name::FeedbackSize(), 1));
	m_ServerDecryptor.SetKey(SharedSecret, 16, MakeParameters(Name::IV(), ConstByteArrayParameter(SharedSecret, 16))(Name::FeedbackSize(), 1));
	
	// Encrypt the nonce:
	byte EncryptedNonce[128];
	rsaEncryptor.Encrypt(rng, (const byte *)(a_Nonce.data()), a_Nonce.size(), EncryptedNonce);
	
	// Send the packet to the server:
	Log("Sending PACKET_ENCRYPTION_KEY_RESPONSE to the SERVER");
	cByteBuffer ToServer(1024);
	ToServer.WriteByte(PACKET_ENCRYPTION_KEY_RESPONSE);
	ToServer.WriteBEShort(EncryptedLength);
	ToServer.WriteBuf(EncryptedSecret, EncryptedLength);
	ToServer.WriteBEShort(EncryptedLength);
	ToServer.WriteBuf(EncryptedNonce, EncryptedLength);
	SERVERSEND(ToServer);
}





void cConnection::StartClientEncryption(const AString & a_EncKey, const AString & a_EncNonce)
{
	// Decrypt EncNonce using privkey
	RSAES<PKCS1v15>::Decryptor rsaDecryptor(m_Server.GetPrivateKey());
	time_t CurTime = time(NULL);
	RandomPool rng;
	rng.Put((const byte *)&CurTime, sizeof(CurTime));
	byte DecryptedNonce[MAX_ENC_LEN];
	DecodingResult res = rsaDecryptor.Decrypt(rng, (const byte *)a_EncNonce.data(), a_EncNonce.size(), DecryptedNonce);
	if (!res.isValidCoding || (res.messageLength != 4))
	{
		Log("Client: Bad nonce length");
		return;
	}
	if (ntohl(*((int *)DecryptedNonce)) != m_Nonce)
	{
		Log("Bad nonce value");
		return;
	}
	
	// Decrypt the symmetric encryption key using privkey:
	byte SharedSecret[MAX_ENC_LEN];
	res = rsaDecryptor.Decrypt(rng, (const byte *)a_EncKey.data(), a_EncKey.size(), SharedSecret);
	if (!res.isValidCoding || (res.messageLength != 16))
	{
		Log("Bad key length");
		return;
	}
	
	// Send encryption key response:
	cByteBuffer ToClient(6);
	ToClient.WriteByte((char)0xfc);
	ToClient.WriteBEShort(0);
	ToClient.WriteBEShort(0);
	CLIENTSEND(ToClient);
	
	// Start the encryption:
	m_ClientEncryptor.SetKey(SharedSecret, 16, MakeParameters(Name::IV(), ConstByteArrayParameter(SharedSecret, 16))(Name::FeedbackSize(), 1));
	m_ClientDecryptor.SetKey(SharedSecret, 16, MakeParameters(Name::IV(), ConstByteArrayParameter(SharedSecret, 16))(Name::FeedbackSize(), 1));
	Log("Client connection is now encrypted");
	m_ClientState = csEncryptedUnderstood;
	
	// Handle all postponed server data
	DecodeServersPackets(NULL, 0);
}



