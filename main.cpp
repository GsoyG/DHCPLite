#include "DHCPLite.h"
#include <iostream>
#include <windows.h>

BOOL WINAPI ConsoleCtrlHandlerRoutine(DWORD dwCtrlType) {
	if ((CTRL_C_EVENT == dwCtrlType) || (CTRL_BREAK_EVENT == dwCtrlType)) {
		Close();
		std::cout << "Stopping server request handler.\n";

		return TRUE;
	}
	return FALSE;
}

DHCPConfig GetIPAddrInfo() {
	auto addrInfoList = GetIPAddrInfoList();
	if (2 != addrInfoList.size()) {
		std::cout << "Too many or too few IP addresses are present on this machine. [Routing can not be bypassed.]\n";
		return DHCPConfig{};
	}

	const bool loopbackAtIndex0 = ValuetoIP(0x7f000001) == addrInfoList[0].address;
	const bool loopbackAtIndex1 = ValuetoIP(0x7f000001) == addrInfoList[1].address;
	if (loopbackAtIndex0 == loopbackAtIndex1) {
		std::cout << "Unsupported IP address configuration. [Expected to find loopback address and one other.]\n";
		return DHCPConfig{};
	}

	const int tableIndex = loopbackAtIndex1 ? 0 : 1;
	std::cout << "IP Address being used:\n";
	const DWORD dwAddr = addrInfoList[tableIndex].address;
	if (0 == dwAddr) {
		std::cout << "IP Address is 0.0.0.0 - no network is available on this machine. [APIPA (Auto-IP) may not have assigned an IP address yet.]\n";
		return DHCPConfig{};
	}

	const DWORD dwMask = addrInfoList[tableIndex].mask;
	const DWORD dwAddrValue = IPtoValue(dwAddr);
	const DWORD dwMaskValue = IPtoValue(dwMask);
	const DWORD dwMinAddrValue = ((dwAddrValue & dwMaskValue) | 2);  // Skip x.x.x.1 (default router address)
	const DWORD dwMaxAddrValue = ((dwAddrValue & dwMaskValue) | (~(dwMaskValue | 1)));
	const DWORD dwMinAddr = ValuetoIP(dwMinAddrValue);
	const DWORD dwMaxAddr = ValuetoIP(dwMaxAddrValue);

	std::cout << IPAddrToString(dwAddr) << " - Subnet:" << IPAddrToString(dwMask) << " - Range:["
		<< IPAddrToString(dwMinAddr) << "-" << IPAddrToString(dwMaxAddr) << "]\n";

	if (dwMinAddrValue > dwMaxAddrValue) {
		std::cout << "No network is available on this machine. [The subnet mask is incorrect.]\n";
		return DHCPConfig{};
	}

	return DHCPConfig{ dwAddr, dwMask, dwMinAddr, dwMaxAddr };
}

int main(int /*argc*/, char ** /*argv*/) {
	std::cout << "DHCPLite\n2016-04-02\n";
	std::cout << "Copyright (c) 2001-2016 by David Anson (http://dlaa.me/)\n\n";

	if (!SetConsoleCtrlHandler(ConsoleCtrlHandlerRoutine, TRUE)) {
		std::cout << "[Error] Unable to set Ctrl-C handler.\n";
		system("pause");
		return 1;
	}

	DHCPConfig config{};
	try {
		config = GetIPAddrInfo();
		if (config.addrInfo.address == 0) {
			system("pause");
			return 1;
		}

		SetDiscoverCallback([](char *clientHostName, DWORD offerAddr) {
			std::cout << "Offering client \"" << clientHostName << "\" IP address " << IPAddrToString(offerAddr) << "\n";
		});

		SetACKCallback([](char *clientHostName, DWORD offerAddr) {
			std::cout << "Acknowledging client \"" << clientHostName << "\" has IP address " << IPAddrToString(offerAddr) << "\n";
		});

		SetNAKCallback([](char *clientHostName, DWORD offerAddr) {
			std::cout << "Denying client \"" << clientHostName << "\" unoffered IP address.\n";
		});

		Init(config.addrInfo.address);
		std::cout << "Server is running...  (Press Ctrl+C to shutdown.)\n";
		Start(config);
	}
	catch (GetIPAddrException e) {
		std::cout << "[Error] " << e.what() << "\n";
	}

	Cleanup();

	system("pause");
	return 0;
}
