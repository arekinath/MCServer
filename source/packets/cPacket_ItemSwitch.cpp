
#include "Globals.h"  // NOTE: MSVC stupidness requires this to be the same across all modules

#include "cPacket_ItemSwitch.h"





int cPacket_ItemSwitch::Parse(cByteBuffer & a_Buffer)
{
	int TotalBytes = 0;
	HANDLE_PACKET_READ(ReadBEShort, m_SlotNum, TotalBytes);
	return TotalBytes;
}




