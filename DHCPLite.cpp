#include "DHCPLite.h"
#include <tchar.h>
#include <assert.h>
#include <iphlpapi.h>
#include <iprtrmib.h>

using namespace DHCPLite;

size_t DHCPMessage::SetOptionList(std::vector<BYTE> options) {
	size_t size = 0;
	for (size_t i = 0; i < options.size(); i++) { // RFC 2132
		switch (options[i]) {
		case MsgOption_PAD:
			continue;
			break;
		case MsgOption_END:
			optionList[options[i]] = std::vector<BYTE>{}; // MsgOption_END
			return size;
			break;
		default:
		{
			if (i + 1 >= options.size()) {
				assert(!(TEXT("Invalid option data (not enough room for required length byte).")));
				break;
			}

			BYTE optionLen = options[i + 1];
			std::vector<BYTE> data(optionLen);
			std::copy_n(options.begin() + (i + 2), optionLen, data.begin());
			optionList[options[i]] = data;

			i += 1; // lenght bit
			i += optionLen; // data bit
			size++;
			break;
		}
		}
	}
	return size;
}

DHCPMessage::DHCPMessage() : body() {
	BYTE *pBody = reinterpret_cast<BYTE *>(&body);
	std::fill_n(pBody, sizeof(MessageBody), 0);
}

DHCPMessage::DHCPMessage(std::vector<BYTE> data) {
	SetData(data);
}

std::vector<BYTE> DHCPMessage::GetData() {
	std::vector<BYTE> data(sizeof(MessageBody));
	BYTE *pBody = reinterpret_cast<BYTE *>(&body);
	std::copy_n(pBody, sizeof(MessageBody), data.begin());

	for (auto &&option : optionList) {
		data.push_back(option.first);

		std::vector<BYTE> optionData = option.second;
		auto optionDataSize = optionData.size();
		if (optionDataSize == 0) continue;

		data.push_back(static_cast<BYTE>(optionDataSize));

		auto dataSize = data.size();
		data.resize(dataSize + optionDataSize);
		std::copy_n(optionData.begin(), optionDataSize, data.begin() + dataSize);
	}

	return data;
}

void DHCPMessage::SetData(std::vector<BYTE> data) {
	// Take into account mandatory DHCP magic cookie values in options array (RFC 2131 section 3)
	if (data.size() < sizeof(MessageBody))
		throw MessageException("Invalid DHCP message (failed initial checks).");

	BYTE *pBody = reinterpret_cast<BYTE *>(&body);
	std::copy_n(data.begin(), sizeof(MessageBody), pBody);

	std::vector<BYTE> options(data.size() - sizeof(MessageBody));
	options.assign(data.begin() + sizeof(MessageBody), data.end());
	SetOptionList(options);
}

std::vector<BYTE> DHCPMessage::GetOptionRaw(MessageOptionValues option) {
	if (optionList.find(option) != optionList.end()) {
		return optionList[option];
	}

	return std::vector<BYTE>{};
}

template <class T> T DHCPMessage::GetOption(MessageOptionValues option) {
	auto raw = GetOptionRaw(option);
	auto rawSize = raw.size();
	if (rawSize <= 0) return T{};

	if (rawSize < sizeof(T))
		throw MessageException("Invalid DHCP message option (size exceeds actual size).");

	if (std::is_same_v<T, BYTE>) {
		return raw[0];
	}
	else if (std::is_same_v<T, WORD>) {
		return static_cast<T>(*reinterpret_cast<WORD *>(raw.data()));
	}
	else if (std::is_same_v<T, DWORD>) {
		return static_cast<T>(*reinterpret_cast<DWORD *>(raw.data()));
	}

	return T{};
}

void DHCPMessage::SetOptionRaw(MessageOptionValues option, std::vector<BYTE> data) {
	optionList[option] = data;
}

template <class T> void DHCPMessage::SetOption(MessageOptionValues option, T data) {
	if (std::is_same_v<T, BYTE>) {
		optionList[option] = PByteToVByte(reinterpret_cast<BYTE *>(&data), 1);
	}
	else if (std::is_same_v<T, WORD>) {
		optionList[option] = PByteToVByte(reinterpret_cast<BYTE*>(&data), 2);
	}
	else if (std::is_same_v<T, DWORD>) {
		optionList[option] = PByteToVByte(reinterpret_cast<BYTE *>(&data), 4);
	}
	else {
		throw MessageException("Invalid DHCP message option type.");
	}
}

void DHCPLite::DHCPMessage::SetOption(MessageOptionValues option) {
	optionList[option] = std::vector<BYTE>{};
}

std::vector<BYTE> DHCPMessage::PByteToVByte(const BYTE *data, int size) {
	std::vector<BYTE> bytes(size);
	std::copy_n(data, size, bytes.begin());
	return bytes;
}

int DHCPServer::FindIndexOf(const VectorAddressInUseInformation *const pvAddressesInUse, FindIndexOfFilter pFilter) {
	assert((0 != pvAddressesInUse) && (0 != pFilter));

	for (size_t i = 0; i < pvAddressesInUse->size(); i++) {
		if (pFilter(pvAddressesInUse->at(i))) {
			return (int)i;
		}
	}
	return -1;
}

bool DHCPServer::InitializeDHCPServer() {
	// Determine server hostname
	if (NO_ERROR != gethostname(pcsServerHostName, sizeof(pcsServerHostName))) {
		pcsServerHostName[0] = '\0';
	}

	// Open socket and set broadcast option on it
	sServerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (INVALID_SOCKET == sServerSocket) {
		throw SocketException("Unable to open server socket (port 67).");
	}

	SOCKADDR_IN saServerAddress{};
	saServerAddress.sin_family = AF_INET;
	saServerAddress.sin_addr.s_addr = config.addrInfo.address;  // Already in network byte order
	saServerAddress.sin_port = htons((u_short)DHCP_SERVER_PORT);
	const int iServerAddressSize = sizeof(saServerAddress);
	if (SOCKET_ERROR == bind(sServerSocket, (SOCKADDR *)(&saServerAddress), iServerAddressSize)) {
		throw SocketException("Unable to bind to server socket (port 67).");
	}

	int iBroadcastOption = TRUE;
	if (NO_ERROR != setsockopt(sServerSocket, SOL_SOCKET, SO_BROADCAST, (char *)(&iBroadcastOption), sizeof(iBroadcastOption))) {
		throw SocketException("Unable to set socket options.");
	}

	return true;
}

void DHCPServer::ProcessDHCPClientRequest(const BYTE *const pbData, const int iDataSize) {
	const BYTE MAGIC_COOKIE[4]{ 0x63, 0x82, 0x53, 0x63 }; // DHCP magic cookie values

	DHCPMessage requestMessage(DHCPMessage::PByteToVByte(pbData, iDataSize));

	DHCPMessage::MessageTypes messageType =
		static_cast<DHCPMessage::MessageTypes>(requestMessage.GetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE));

	if (requestMessage.body.op != DHCPMessage::MsgOp_BOOT_REQUEST
		|| 0 != std::memcmp(MAGIC_COOKIE, &requestMessage.body.magicCookie, sizeof(MAGIC_COOKIE)))
		throw MessageException("Invalid DHCP message (failed initial checks).");
	if (messageType <= 0 || messageType > 8)
		throw MessageException("Invalid DHCP message (invalid or missing DHCP message type).");

	// Determine client host name
	char pcsClientHostName[MAX_HOSTNAME_LENGTH]{};
	pcsClientHostName[0] = '\0';
	auto hostName = requestMessage.GetOptionRaw(DHCPMessage::MsgOption_HOSTNAME);
	const BYTE *pbRequestHostNameData = hostName.data();
	unsigned int iRequestHostNameDataSize = static_cast<u_int>(hostName.size());

	const unsigned int stHostNameCopySize = min(iRequestHostNameDataSize + 1,
		static_cast<u_int>(sizeof(pcsClientHostName)));
	_tcsncpy_s(pcsClientHostName, stHostNameCopySize, (char *)pbRequestHostNameData, _TRUNCATE);

	if ('\0' != pcsServerHostName[0] && 0 == _stricmp(pcsClientHostName, pcsServerHostName)) {
		// Ignore attempts by the DHCP server to obtain a DHCP address (possible if its current address was obtained by auto-IP) because this would invalidate dwServerAddr
	}

	// Determine client identifier in proper RFC 2131 order (client identifier option then chaddr)
	const BYTE *pbRequestClientIdentifierData;
	unsigned int iRequestClientIdentifierDataSize;
	auto clientIdentifier = requestMessage.GetOptionRaw(DHCPMessage::MsgOption_CLIENT_IDENTIFIER);
	if (clientIdentifier.size() > 0) {
		pbRequestClientIdentifierData = clientIdentifier.data();
		iRequestClientIdentifierDataSize = static_cast<int>(clientIdentifier.size());
	}
	else {
		pbRequestClientIdentifierData = requestMessage.body.chaddr;
		iRequestClientIdentifierDataSize = sizeof(requestMessage.body.chaddr);
	}

	// Determine if we've seen this client before
	bool bSeenClientBefore = false;
	DWORD dwClientPreviousOfferAddr = (DWORD)INADDR_BROADCAST;  // Invalid IP address for later comparison
	auto cid = std::make_tuple(pbRequestClientIdentifierData, (DWORD)iRequestClientIdentifierDataSize);
	const int iIndex = FindIndexOf(&vAddressesInUse, [=](const AddressInUseInformation &raiui) {
		return (0 != raiui.dwClientIdentifierSize) && (iRequestClientIdentifierDataSize == raiui.dwClientIdentifierSize)
			&& (0 == memcmp(pbRequestClientIdentifierData, raiui.pbClientIdentifier, iRequestClientIdentifierDataSize));
		});
	if (-1 != iIndex) {
		const AddressInUseInformation aiui = vAddressesInUse.at((size_t)iIndex);
		dwClientPreviousOfferAddr = ValuetoIP(aiui.dwAddrValue);
		bSeenClientBefore = true;
	}
	// Server message handling
	// RFC 2131 section 4.3
	DHCPMessage replyMessage;
	replyMessage.body.op = DHCPMessage::MsgOp_BOOT_REPLY;
	replyMessage.body.htype = requestMessage.body.htype;
	replyMessage.body.hlen = requestMessage.body.hlen;
	// replyMessage.body.hops = 0;
	replyMessage.body.xid = requestMessage.body.xid;
	// replyMessage.body.ciaddr = 0;
	// replyMessage.body.yiaddr = 0;  Or changed below
	// replyMessage.body.siaddr = 0;
	replyMessage.body.flags = requestMessage.body.flags;
	replyMessage.body.giaddr = requestMessage.body.giaddr;

	std::copy_n(requestMessage.body.chaddr, sizeof(replyMessage.body.chaddr), replyMessage.body.chaddr);
	int snameSize = sizeof(replyMessage.body.sname);
	if (serverName.size() < snameSize) snameSize = static_cast<int>(serverName.size());
	strncpy_s((char *)(replyMessage.body.sname), snameSize, serverName.c_str(), _TRUNCATE);
	// replyMessage.body.file = 0;
	// set options below
	replyMessage.body.magicCookie = *reinterpret_cast<const DWORD*>(MAGIC_COOKIE);
	// DHCP Message Type - RFC 2132 section 9.6
	replyMessage.SetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE, DHCPMessage::MsgType_DISCOVER);
	// IP Address Lease Time - RFC 2132 section 9.2
	C_ASSERT(sizeof(u_long) == 4);
	replyMessage.SetOption(DHCPMessage::MsgOption_ADDRESS_LEASETIME, htonl(1 * 60 * 60)); // One hour
	// Subnet Mask - RFC 2132 section 3.3
	replyMessage.SetOption(DHCPMessage::MsgOption_SUBNET_MASK, config.addrInfo.mask); // Already in network order
	// Server Identifier - RFC 2132 section 9.7
	replyMessage.SetOption(DHCPMessage::MsgOption_SERVER_IDENTIFIER, config.addrInfo.address); // Already in network order
	// END
	replyMessage.SetOption(DHCPMessage::MsgOption_END);

	bool bSendDHCPMessage = false;
	switch (messageType) {
	case DHCPMessage::MsgType_DISCOVER:
	{
		// RFC 2131 section 4.3.1
		// UNSUPPORTED: Requested IP Address option
		static DWORD dwServerLastOfferAddrValue = IPtoValue(config.maxAddr);  // Initialize to max to wrap and offer min first
		const DWORD dwMinAddrValue = IPtoValue(config.minAddr);
		const DWORD dwMaxAddrValue = IPtoValue(config.maxAddr);
		DWORD dwOfferAddrValue;
		bool bOfferAddrValueValid = false;
		if (bSeenClientBefore) {
			dwOfferAddrValue = IPtoValue(dwClientPreviousOfferAddr);
			bOfferAddrValueValid = true;
		}
		else {
			dwOfferAddrValue = dwServerLastOfferAddrValue + 1;
		}
		// Search for an available address if necessary
		const DWORD dwInitialOfferAddrValue = dwOfferAddrValue;
		bool bOfferedInitialValue = false;
		while (!bOfferAddrValueValid && !(bOfferedInitialValue && (dwInitialOfferAddrValue == dwOfferAddrValue)))  // Detect address exhaustion
		{
			if (dwMaxAddrValue < dwOfferAddrValue) {
				assert(dwMaxAddrValue + 1 == dwOfferAddrValue);
				dwOfferAddrValue = dwMinAddrValue;
			}
			bOfferAddrValueValid = (-1 == FindIndexOf(&vAddressesInUse, [=](const AddressInUseInformation &raiui) {
				return dwOfferAddrValue == raiui.dwAddrValue;
				}));
			bOfferedInitialValue = true;
			if (!bOfferAddrValueValid) {
				dwOfferAddrValue++;
			}
		}
		if (!bOfferAddrValueValid) {
			throw RequestException("No more IP addresses available for client.");
		}
		dwServerLastOfferAddrValue = dwOfferAddrValue;
		const DWORD dwOfferAddr = ValuetoIP(dwOfferAddrValue);
		assert((0 != iRequestClientIdentifierDataSize) && (0 != pbRequestClientIdentifierData));
		AddressInUseInformation aiuiClientAddress{};
		aiuiClientAddress.dwAddrValue = dwOfferAddrValue;
		aiuiClientAddress.pbClientIdentifier = (BYTE *)LocalAlloc(LMEM_FIXED, iRequestClientIdentifierDataSize);
		if (0 != aiuiClientAddress.pbClientIdentifier) {
			CopyMemory(aiuiClientAddress.pbClientIdentifier, pbRequestClientIdentifierData, iRequestClientIdentifierDataSize);
			aiuiClientAddress.dwClientIdentifierSize = iRequestClientIdentifierDataSize;

			vAddressesInUse.push_back(aiuiClientAddress);
			replyMessage.body.yiaddr = dwOfferAddr;
			replyMessage.SetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE, DHCPMessage::MsgType_OFFER);
			bSendDHCPMessage = true;

			MessageCallback_Discover(pcsClientHostName, dwOfferAddr);

			if (bSeenClientBefore) {
				LocalFree(aiuiClientAddress.pbClientIdentifier);
			}
		}
		else {
			LocalFree(aiuiClientAddress.pbClientIdentifier);
			throw RequestException("Insufficient memory to add client address.");
		}
	}
	break;
	case DHCPMessage::MsgType_REQUEST:
	{
		// RFC 2131 section 4.3.2
		// Determine requested IP address
		DWORD dwRequestedIPAddress = INADDR_BROADCAST;  // Invalid IP address for later comparison
		auto requestedIPAddressRaw = requestMessage.GetOptionRaw(DHCPMessage::MsgOption_REQUESTED_ADDRESS);
		if (requestedIPAddressRaw.size() > 0) {
			dwRequestedIPAddress = *reinterpret_cast<DWORD*>(requestedIPAddressRaw.data());
		}

		// Determine server identifier
		auto serverIdentifier = requestMessage.GetOption<DWORD>(DHCPMessage::MsgOption_SERVER_IDENTIFIER);
		if (serverIdentifier != config.addrInfo.address) {
			// Response to OFFER
			// DHCPREQUEST generated during SELECTING state
			assert(0 == requestMessage.body.ciaddr);
			if (bSeenClientBefore) {
				// Already have an IP address for this client - ACK it
				replyMessage.SetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE, DHCPMessage::MsgType_ACK);
				// Will set other options below
			}
			else {
				// Haven't seen this client before - NAK it
				replyMessage.SetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE, DHCPMessage::MsgType_NAK);
				// Will clear invalid options and prepare to send message below
			}
		}
		else {
			// Request to verify or extend
			if (dwRequestedIPAddress == INADDR_BROADCAST && (dwRequestedIPAddress != INADDR_BROADCAST || requestMessage.body.ciaddr == 0)) {
				assert(!(TEXT("Invalid DHCP message (invalid data).")));
			}
			// DHCPREQUEST generated during INIT-REBOOT state - Some clients set ciaddr in this case, so deviate from the spec by allowing it
			// Unicast -> DHCPREQUEST generated during RENEWING state / Broadcast -> DHCPREQUEST generated during REBINDING state
			if (bSeenClientBefore && ((dwClientPreviousOfferAddr == dwRequestedIPAddress) || (dwClientPreviousOfferAddr == requestMessage.body.ciaddr))) {
				// Already have an IP address for this client - ACK it
				replyMessage.SetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE, DHCPMessage::MsgType_ACK);
				// Will set other options below
			}
			else {
				// Haven't seen this client before or requested IP address is invalid
				replyMessage.SetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE, DHCPMessage::MsgType_NAK);
				// Will clear invalid options and prepare to send message below
			}
		}
		switch (replyMessage.GetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE)) {
		case DHCPMessage::MsgType_ACK:
			assert(INADDR_BROADCAST != dwClientPreviousOfferAddr);

			replyMessage.body.ciaddr = dwClientPreviousOfferAddr;
			replyMessage.body.yiaddr = dwClientPreviousOfferAddr;
			bSendDHCPMessage = true;

			MessageCallback_ACK(pcsClientHostName, dwClientPreviousOfferAddr);
			break;
		case DHCPMessage::MsgType_NAK:
			C_ASSERT(0 == DHCPMessage::MsgOption_PAD);
			bSendDHCPMessage = true;

			MessageCallback_NAK(pcsClientHostName, dwClientPreviousOfferAddr);
			break;
		default:
			// Nothing to do
			break;
		}
	}
	break;
	case DHCPMessage::MsgType_DECLINE:
		// Fall-through
	case DHCPMessage::MsgType_RELEASE:
		// UNSUPPORTED: Mark address as unused
		break;
	case DHCPMessage::MsgType_INFORM:
		// Unsupported DHCP message type - fail silently
		break;
	case DHCPMessage::MsgType_OFFER:
	case DHCPMessage::MsgType_ACK:
	case DHCPMessage::MsgType_NAK:
		assert(!(TEXT("Unexpected DHCP message type.")));
		break;
	default:
		assert(!"Invalid DHCPMessageType");
		break;
	}
	if (bSendDHCPMessage) {
		// Must have set an option if we're going to be sending this message
		assert(0 != replyMessage.GetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE));
		// Determine how to send the reply
		// RFC 2131 section 4.1
		u_long ulAddr = INADDR_LOOPBACK;  // Invalid value
		if (0 == requestMessage.body.giaddr) {
			switch (replyMessage.GetOption<BYTE>(DHCPMessage::MsgOption_MESSAGE_TYPE)) {
			case DHCPMessage::MsgType_OFFER:
				// Fall-through
			case DHCPMessage::MsgType_ACK:
			{
				if (0 == requestMessage.body.ciaddr) {
					if (0 != (BROADCAST_FLAG & requestMessage.body.flags)) {
						ulAddr = INADDR_BROADCAST;
					}
					else {
						ulAddr = requestMessage.body.yiaddr;  // Already in network order
						if (0 == ulAddr) {
							// UNSUPPORTED: Unicast to hardware address
							// Instead, broadcast the response and rely on other DHCP clients to ignore it
							ulAddr = INADDR_BROADCAST;
						}
					}
				}
				else {
					ulAddr = requestMessage.body.ciaddr;  // Already in network order
				}
			}
			break;
			case DHCPMessage::MsgType_NAK:
			{
				ulAddr = INADDR_BROADCAST;
			}
			break;
			default:
				assert(!"Invalid DHCPMessageType");
				break;
			}
		}
		else {
			ulAddr = requestMessage.body.giaddr;  // Already in network order
			replyMessage.body.flags |= BROADCAST_FLAG;  // Indicate to the relay agent that it must broadcast
		}
		assert((INADDR_LOOPBACK != ulAddr) && (0 != ulAddr));
		SOCKADDR_IN saClientAddress{};
		saClientAddress.sin_family = AF_INET;
		saClientAddress.sin_addr.s_addr = ulAddr;
		saClientAddress.sin_port = htons((u_short)DHCP_CLIENT_PORT);
		auto replyMessageData = replyMessage.GetData();
		assert(SOCKET_ERROR != sendto(sServerSocket, reinterpret_cast<char*>(replyMessageData.data()),
			static_cast<int>(replyMessageData.size()), 0, (SOCKADDR *)&saClientAddress, sizeof(saClientAddress)));
	}
}

bool DHCPServer::ReadDHCPClientRequests() {
	BYTE *const pbReadBuffer = (BYTE *)LocalAlloc(LMEM_FIXED, MAX_UDP_MESSAGE_SIZE);
	if (0 == pbReadBuffer) {
		throw RequestException("Unable to allocate memory for client datagram read buffer.");
	}

	int iLastError = 0;
	while (WSAENOTSOCK != iLastError) {
		SOCKADDR_IN saClientAddress{};
		int iClientAddressSize = sizeof(saClientAddress);
		const int iBytesReceived = recvfrom(sServerSocket, (char *)pbReadBuffer, MAX_UDP_MESSAGE_SIZE, 0, (SOCKADDR *)(&saClientAddress), &iClientAddressSize);
		if (SOCKET_ERROR != iBytesReceived) {
			// assert(DHCP_CLIENT_PORT == ntohs(saClientAddress.sin_port));  // Not always the case
			ProcessDHCPClientRequest(pbReadBuffer, iBytesReceived);
		}
		else {
			iLastError = WSAGetLastError();
			if (iLastError != WSAENOTSOCK && iLastError != WSAEINTR) {
				LocalFree(pbReadBuffer);
				throw SocketException("Call to recvfrom returned error.");
			}
		}
	}
	LocalFree(pbReadBuffer);
	return true;
}


DWORD DHCPServer::IPtoValue(DWORD ip) {
	// Convert between big and small endian order
	DWORD value = 0;
	BYTE *valueBytes = (BYTE *)&value;
	BYTE *ipBytes = (BYTE *)&ip;

	for (size_t i = 0; i < 4; i++)
		valueBytes[i] = ipBytes[3 - i];

	return value;
}

DWORD DHCPServer::ValuetoIP(DWORD value) {
	return IPtoValue(value);
}

std::string DHCPServer::IPAddrToString(DWORD address) {
	BYTE *addrBytes = (BYTE *)&address;

	std::string str = "";
	for (size_t i = 0; i < 3; i++) {
		str.append(std::to_string(addrBytes[i]) + ".");
	}
	str.append(std::to_string(addrBytes[3]));

	return str;
}

std::vector<DHCPServer::IPAddrInfo> DHCPServer::GetIPAddrInfoList() {
	std::vector<IPAddrInfo> infoList;

	MIB_IPADDRTABLE miatIpAddrTable;
	ULONG ulIpAddrTableSize = sizeof(miatIpAddrTable);
	DWORD dwGetIpAddrTableResult = GetIpAddrTable(&miatIpAddrTable, &ulIpAddrTableSize, FALSE);
	// Technically, if NO_ERROR was returned, we don't need to allocate a buffer - but it's easier to do so anyway - and because we need more data than fits in the default buffer, this would only be wasteful in the error case
	if ((NO_ERROR != dwGetIpAddrTableResult) && (ERROR_INSUFFICIENT_BUFFER != dwGetIpAddrTableResult)) {
		throw IPAddrException("Unable to query IP address table.");
	}

	const ULONG ulIpAddrTableSizeAllocated = ulIpAddrTableSize;
	BYTE *const pbIpAddrTableBuffer = (BYTE *)LocalAlloc(LMEM_FIXED, ulIpAddrTableSizeAllocated);
	if (nullptr == pbIpAddrTableBuffer) {
		LocalFree(pbIpAddrTableBuffer);
		throw IPAddrException("Insufficient memory for IP address table.");
	}

	dwGetIpAddrTableResult = GetIpAddrTable((MIB_IPADDRTABLE *)pbIpAddrTableBuffer, &ulIpAddrTableSize, FALSE);
	if ((NO_ERROR != dwGetIpAddrTableResult) || (ulIpAddrTableSizeAllocated > ulIpAddrTableSize)) {
		LocalFree(pbIpAddrTableBuffer);
		throw IPAddrException("Unable to query IP address table.");
	}

	const MIB_IPADDRTABLE *const pmiatIpAddrTable = (MIB_IPADDRTABLE *)pbIpAddrTableBuffer;

	for (size_t i = 0; i < pmiatIpAddrTable->dwNumEntries; i++) {
		infoList.push_back(IPAddrInfo{ pmiatIpAddrTable->table[i].dwAddr, pmiatIpAddrTable->table[i].dwMask });
	}

	LocalFree(pbIpAddrTableBuffer);
	return infoList;
}

DHCPServer::DHCPConfig DHCPServer::GetDHCPConfig() {
	auto addrInfoList = DHCPServer::GetIPAddrInfoList();
	if (2 != addrInfoList.size()) {
		throw IPAddrException("Too many or too few IP addresses are present on this machine. [Routing can not be bypassed.]");
	}

	const bool loopbackAtIndex0 = DHCPServer::ValuetoIP(0x7f000001) == addrInfoList[0].address;
	const bool loopbackAtIndex1 = DHCPServer::ValuetoIP(0x7f000001) == addrInfoList[1].address;
	if (loopbackAtIndex0 == loopbackAtIndex1) {
		throw IPAddrException("Unsupported IP address configuration. [Expected to find loopback address and one other.]");
	}

	const int tableIndex = loopbackAtIndex1 ? 0 : 1;
	const DWORD dwAddr = addrInfoList[tableIndex].address;
	if (0 == dwAddr) {
		throw IPAddrException("IP Address is 0.0.0.0 - no network is available on this machine. [APIPA (Auto-IP) may not have assigned an IP address yet.]");
	}

	const DWORD dwMask = addrInfoList[tableIndex].mask;
	const DWORD dwAddrValue = DHCPServer::IPtoValue(dwAddr);
	const DWORD dwMaskValue = DHCPServer::IPtoValue(dwMask);
	const DWORD dwMinAddrValue = ((dwAddrValue & dwMaskValue) | 2);  // Skip x.x.x.1 (default router address)
	const DWORD dwMaxAddrValue = ((dwAddrValue & dwMaskValue) | (~(dwMaskValue | 1)));
	const DWORD dwMinAddr = DHCPServer::ValuetoIP(dwMinAddrValue);
	const DWORD dwMaxAddr = DHCPServer::ValuetoIP(dwMaxAddrValue);

	if (dwMinAddrValue > dwMaxAddrValue) {
		throw IPAddrException("No network is available on this machine. [The subnet mask is incorrect.]");
	}

	return DHCPServer::DHCPConfig{ dwAddr, dwMask, dwMinAddr, dwMaxAddr };
}

void DHCPServer::SetDiscoverCallback(MessageCallback callback) {
	MessageCallback_Discover = callback;
}

void DHCPServer::SetACKCallback(MessageCallback callback) {
	MessageCallback_ACK = callback;
}

void DHCPServer::SetNAKCallback(MessageCallback callback) {
	MessageCallback_NAK = callback;
}

DHCPServer::DHCPServer(DHCPConfig config) {
	Init(config);
}

bool DHCPServer::Init() {
	return Init(GetDHCPConfig());
}

bool DHCPServer::Init(DHCPConfig config) {
	DHCPServer::config = config;

	AddressInUseInformation aiuiServerAddress{};
	aiuiServerAddress.dwAddrValue = IPtoValue(config.addrInfo.address);
	aiuiServerAddress.pbClientIdentifier = 0; // Server entry is only entry without a client ID
	aiuiServerAddress.dwClientIdentifierSize = 0;
	vAddressesInUse.push_back(aiuiServerAddress);

	WSADATA wsaData;
	if (NO_ERROR != WSAStartup(MAKEWORD(1, 1), &wsaData)) {
		throw SocketException("Unable to initialize WinSock.");
	}

	return InitializeDHCPServer();
}

void DHCPServer::Start() {
	assert(ReadDHCPClientRequests());
}

void DHCPServer::Close() {
	if (INVALID_SOCKET != sServerSocket) {
		assert(NO_ERROR == closesocket(sServerSocket));
		sServerSocket = INVALID_SOCKET;
	}
}

bool DHCPServer::Cleanup() {
	if (!WSACleanup()) return false;

	for (size_t i = 0; i < vAddressesInUse.size(); i++) {
		AddressInUseInformation aiuiServerAddress{};
		aiuiServerAddress = vAddressesInUse.at(i);
		if (aiuiServerAddress.pbClientIdentifier != 0) {
			LocalFree(aiuiServerAddress.pbClientIdentifier);
		}
	}

	return true;
}

bool DHCPServer::SetServerName(std::string name) {
	if (name.size() > 64)
		return false;

	serverName = name;
	return true;
}
