#pragma comment(lib, "winmm.lib" )
#pragma comment(lib, "ws2_32")
#pragma comment(lib,"Pdh.lib")
#pragma comment(lib, "libmysql.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dbghelp.h>
#include <list>
#include <locale.h>
#include <random>
#include <process.h>
#include <stdlib.h>
#include <iostream>
#include <Pdh.h>
#include <strsafe.h>
#include <unordered_map>
#include <conio.h>
#include <mysql.h>
#include <errmsg.h>
#include "log.h"
#include "ringbuffer.h"
#include "MemoryPoolBucket.h"
#include "Packet.h"
#include "profiler.h"
#include "dumpClass.h"
#include "CDBConnector.h"
#include "LockFreeQueue.h"
#include "LockFreeStack.h"
#include "CNetServer.h"
#include "CommonProtocol.h"
#include "LoginServer.h"
#include "HardwareMonitor.h"
#include "ProcessMonitor.h"

using namespace std;

CrashDump myDump;

WCHAR IPaddress[20] = L"0.0.0.0";

CInitParam initParam(IPaddress, 10000, 100, 3, true, 15000);
CNetServer NetServer(&initParam);
CLoginServer LoginServer;

CHardwareMonitor Hardware_Monitor;
CProcessMonitor Process_Monitor(GetCurrentProcess());


int main()
{
	PRO_INIT();

	volatile bool g_ShutDown = false;
	logInit();

	CContentsHandler HandleInstance;
	HandleInstance.attachServerInstance(&NetServer, &LoginServer);

	NetServer.attachHandler(&HandleInstance);
	LoginServer.attachServerInstance(&NetServer);

	if (LoginServer.Start() == false)
	{
		wprintf(L"ChatServer Thread init error");
		systemLog(L"Start Error", dfLOG_LEVEL_ERROR, L"ChatServer Thread init Error");
		return false;
	}

	if (NetServer.Start() == false)
	{
		systemLog(L"Start Error", dfLOG_LEVEL_ERROR, L"NetServer Init Error, ErrorNo : %u, ErrorCode : %d", NetServer.InitErrorNum, NetServer.InitErrorCode);
		return false;
	}

	ULONGLONG startTime = GetTickCount64();
	ULONGLONG lastTime = 0;
	ULONGLONG nowTime = 0;
	ULONGLONG interval = 0;
	while (!g_ShutDown)
	{
		if (_kbhit())
		{
			WCHAR ControlKey = _getwch();
			if (L'q' == ControlKey || L'Q' == ControlKey)
			{
				g_ShutDown = true;
			}
		}
		LoginServer.updateJobCount();
		LoginServer.updateNumOfWFSO();

		Hardware_Monitor.Update();
		Process_Monitor.Update();

		wprintf(L"======================\n");
		wprintf(L"session number : %d\n", NetServer.getSessionCount());
		wprintf(L"Character Number : %lld\n", LoginServer.getCharacterNum());
		wprintf(L"Accept Sum : %lld\n", NetServer.getAcceptSum());
		wprintf(L"Accept TPS : %d\n", NetServer.getAcceptTPS());
		wprintf(L"Disconnect TPS : %d\n", NetServer.getDisconnectTPS());
		wprintf(L"Send TPS : %d\n", NetServer.getSendMessageTPS());
		wprintf(L"Recv TPS : %d\n", NetServer.getRecvMessageTPS());
		wprintf(L"JobQueue UseSize : %d\n", LoginServer.getJobQueueUseSize());
		wprintf(L"Job TPS : %d\n", LoginServer.JobCount);
		wprintf(L"Number Of Sleep per second : %d\n", LoginServer.getNumOfWFSO());
		wprintf(L"Job Count Per Cycle : %d\n", LoginServer.getJobCountperCycle());
		wprintf(L"PacketPool UseSize : %d\n", CPacket::getPoolUseSize() * POOL_BUCKET_SIZE);
		wprintf(L"PlayerPool UseSize : %d\n", LoginServer.getPlayerPoolUseSize());
		wprintf(L"Time Check Interval : %lld\n", LoginServer.Interval);
		wprintf(L"======================\n");
		wprintf(L"Process User Memory : %lld Bytes\n", (INT64)Process_Monitor.getProcessUserMemory());
		wprintf(L"Process Nonpaged Memory : %lld Bytes\n", (INT64)Process_Monitor.getProcessNonpagedMemory());
		wprintf(L"Process : %f %%, ", Process_Monitor.getProcessTotal());
		wprintf(L"ProcessKernel : %f %%, ", Process_Monitor.getProcessKernel());
		wprintf(L"ProcessUser : %f %%\n", Process_Monitor.getProcessUser());
		wprintf(L"======================\n");
		wprintf(L"Available Memory : %d MBytes\n", (int)Hardware_Monitor.getAvailableMemory());
		wprintf(L"Nonpaged Memory : %lld Bytes\n", (INT64)Hardware_Monitor.getNonpagedMemory());
		wprintf(L"Processor : %f%%, ", Hardware_Monitor.getProcessorTotal());
		wprintf(L"ProcessorKernel : %f%%, ", Hardware_Monitor.getProcessorKernel());
		wprintf(L"ProcessorUser : %f%% \n", Hardware_Monitor.getProcessorUser());
		wprintf(L"NetWork RecvBytes : %d Bytes\n", (int)Hardware_Monitor.getRecvBytes());
		wprintf(L"NetWork SendBytes : %d Bytes\n", (int)Hardware_Monitor.getSendBytes());
		wprintf(L"======================\n");
		Sleep(1000);
	}

	LoginServer.Stop();
	NetServer.Stop();

	PRO_LOG();
	return 0;
}