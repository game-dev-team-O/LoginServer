#pragma comment(lib, "winmm.lib" )
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "libmysql.lib")
//#include <cpp_redis/cpp_redis>
#pragma comment (lib, "cpp_redis.lib")
#pragma comment (lib, "tacopie.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dbghelp.h>
#include <list>
#include <random>
#include <locale.h>
#include <process.h>
#include <string>
#include <stdlib.h>
#include <iostream>
#include <unordered_map>
#include <strsafe.h>
#include <conio.h>
#include <mysql.h>
#include <errmsg.h>
#include "log.h"
#include "ringbuffer.h"
#include "MemoryPoolBucket.h"
#include "Packet.h"
#include "profiler.h"
#include "dumpClass.h"
#include "LockFreeQueue.h"
#include "LockFreeStack.h"
#include "CDBConnector.h"
#include "CommonProtocol.h"
#include "CNetServer.h"
#include "LoginServer.h"


using namespace std;



CLoginServer::CLoginServer()
{
	ShutDownFlag = false;
	wcscpy_s(ChatServerList[0].serverIP.IP, L"10.0.1.1");
	ChatServerList[0].serverPort = 6000;
	wcscpy_s(ChatServerList[1].serverIP.IP, L"10.0.2.1");
	ChatServerList[1].serverPort = 6000;
	wcscpy_s(ChatServerList[2].serverIP.IP, L"127.0.0.1");
	ChatServerList[2].serverPort = 6000;

	lastTime = 0;
	pNetServer = NULL;
	hJobEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (hJobEvent == NULL)
	{
		CrashDump::Crash();
	}

}


DWORD WINAPI CLoginServer::LogicThread(CLoginServer* pLoginServer)
{
	while (!pLoginServer->ShutDownFlag)
	{
		//시간 쟤서 모든세션의 lastPacket 확인 -> 40초가 지났다면 그세션끊기
		AcquireSRWLockShared(&pLoginServer->PlayerListLock);
		ULONGLONG curTime = GetTickCount64();
		pLoginServer->Interval = curTime - pLoginServer->lastTime;
		pLoginServer->lastTime = curTime;
		st_Session* pSession;
		for (auto iter = pLoginServer->PlayerList.begin(); iter != pLoginServer->PlayerList.end(); iter++)
		{
			st_Player& player = *iter->second;
			if (player.isPacketRecv == TRUE)
			{
				continue;
			}
			if (curTime > player.lastTime + 50000)
			{
				if (pLoginServer->pNetServer->findSession(player.sessionID, &pSession) == true)
				{
					systemLog(L"TimeOut", dfLOG_LEVEL_DEBUG, L"over time : %lld", curTime - player.lastTime);
					pLoginServer->pNetServer->disconnectSession(pSession);
					if (InterlockedDecrement(&pSession->IOcount) == 0)
					{
						pLoginServer->pNetServer->releaseRequest(pSession);
					}
				}
			}
		}
		ReleaseSRWLockShared(&pLoginServer->PlayerListLock);

		Sleep(10000);
	}
	return true;
}

//JOB Count, NumOfWSFO는 main에서 반영, 
DWORD WINAPI CLoginServer::MemoryDBThread(CLoginServer* pLoginServer)
{
	WORD version = MAKEWORD(2, 2);
	WSADATA data;
	WSAStartup(version, &data);

	cpp_redis::client client;

	client.connect();

	st_JobItem jobItem;
	while (!pLoginServer->ShutDownFlag)
	{
		while (pLoginServer->JobQueue.Dequeue(&jobItem) == true)
		{
			pLoginServer->Temp_JobCount++;
			pLoginServer->Temp_JobCountperCycle++;
			INT64 sessionID = jobItem.SessionID;
			INT64 AccountNo = jobItem.AccountNo;
			CPacket* pPacket = jobItem.pPacket;
			st_Player* pPlayer = jobItem.pPlayer;

			//패킷뺴기
			st_SessionKey SessionKey;
			*pPacket >> SessionKey;
			if (pPacket->subRef() == 0)
			{
				CPacket::mFree(pPacket);
			}

			st_Session* pSession;
			if (pLoginServer->pNetServer->findSession(sessionID, &pSession) == false)
			{
				continue;
			}
			
			//레디스에 쓰기
			std::string temp_string;
			for (int i = 0; i < 64; i++)
			{
				temp_string += SessionKey.sessionKey[i];
			}
			client.set_advanced(to_string(AccountNo), temp_string, true, 600);
			client.sync_commit();

			//응답패킷 보내기
			switch (pPlayer->JoinFlag)
			{
			case TARGET_SERVER_1:
				pLoginServer->CS_LOGIN_RES_LOGIN(sessionID, dfLOGIN_STATUS_OK, AccountNo, pPlayer->ID, pPlayer->Nickname, pLoginServer->ChatServerList[0].serverIP, pLoginServer->ChatServerList[0].serverPort, pLoginServer->ChatServerList[0].serverIP, pLoginServer->ChatServerList[0].serverPort);
				break;

			case TARGET_SERVER_2:
				pLoginServer->CS_LOGIN_RES_LOGIN(sessionID, dfLOGIN_STATUS_OK, AccountNo, pPlayer->ID, pPlayer->Nickname, pLoginServer->ChatServerList[1].serverIP, pLoginServer->ChatServerList[1].serverPort, pLoginServer->ChatServerList[1].serverIP, pLoginServer->ChatServerList[1].serverPort);
				break;

			case TARGET_SERVER_3:
				pLoginServer->CS_LOGIN_RES_LOGIN(sessionID, dfLOGIN_STATUS_OK, AccountNo, pPlayer->ID, pPlayer->Nickname, pLoginServer->ChatServerList[2].serverIP, pLoginServer->ChatServerList[2].serverPort, pLoginServer->ChatServerList[2].serverIP, pLoginServer->ChatServerList[2].serverPort);
				break;

			default:
				break;
			}

			if (InterlockedDecrement(&pSession->IOcount) == 0)
			{
				pLoginServer->pNetServer->releaseRequest(pSession);
			}
		}
		pLoginServer->JobCountperCycle = pLoginServer->Temp_JobCountperCycle;
		pLoginServer->Temp_JobCountperCycle = 0;
		pLoginServer->Temp_NumOfWFSO++;
		WaitForSingleObject(pLoginServer->hJobEvent, INFINITE);
	}
	return true;
}

bool CLoginServer::Start()
{
	maxPlayer = pNetServer->getMaxSession();
	hLogicThread = (HANDLE)_beginthreadex(NULL, 0, (_beginthreadex_proc_type)&LogicThread, this, 0, 0);
	hMemoryDBThread = (HANDLE)_beginthreadex(NULL, 0, (_beginthreadex_proc_type)&MemoryDBThread, this, 0, 0);
	if (hLogicThread == NULL || hMemoryDBThread == NULL)
	{
		wprintf(L"Thread init error"); // 로그로대체
		return false;
	}

	return true;
}

bool CLoginServer::Stop()
{
	ShutDownFlag = true;
	SetEvent(hJobEvent);
	WaitForSingleObject(hLogicThread, INFINITE);
	WaitForSingleObject(hMemoryDBThread, INFINITE);
	return true;
}


void CLoginServer::CS_LOGIN_LOGINSERVER_RES(INT64 SessionID, char ResponseType, st_SessionKey& SessionKey, st_IP& GameServerIP, int GameServerPort)
{
	//en_LOGIN_LOGINSERVER_RES = 2,
	//------------------------------------------------------------
	// Login Server → Client 로그인 응답
	//
	//	{
	//		SHORT	Type
	//
	//		char    ResponseType //  만료(1), 잘못된토큰(2), 없는계정(3), 너는 밴됐다(4), 성공(0)
	//		char	SessionKey[64]; // 인증키. 게임서버로 로그인시 해당 키를 같이 송신한다
	//		WCHAR   IP[20]  // 접속할 게임서버의 ip
	//		int     Port	// 접속할 게임서버의 port
	//	}
	//
	//------------------------------------------------------------
	SHORT Type = en_LOGIN_LOGINSERVER_RES;

	CPacket* pPacket = CPacket::mAlloc();
	pPacket->Clear();
	pPacket->addRef(1);

	*pPacket << Type;
	*pPacket << ResponseType;
	*pPacket << SessionKey;
	*pPacket << GameServerIP;
	*pPacket << GameServerPort;

	pNetServer->sendPacket(SessionID, pPacket);
	if (pPacket->subRef() == 0)
	{
		CPacket::mFree(pPacket);
	}
}

bool CLoginServer::packetProc_CS_LOGIN_LOGINSERVER_REQ(st_Player* pPlayer, CPacket* pPacket, INT64 SessionID)
{
	//------------------------------------------------------------
	// Client → Login Server 로그인 요청
	//
	//	{
	//		SHORT	Type
	//
	//		char    LoginType // google 로그인(1), 자체로그인(2)
	//		WCHAR	Email[50]; // 로그인용 이메일
	//		short   TokenLength // 구글 인증 토큰 길이(LoginType == 1), 자체로그인 비밀번호 길이(LoginType = 2)
	//		char	Token[...]	// 구글 인증 토큰 1~2048 bytes, 자체로그인 비밀번호
	//	}
	//
	//------------------------------------------------------------

	st_Email Email;
	*pPacket >> Email;

	wcscpy_s(pPlayer->Email.Email, Email.Email);

	//여기서 DB에 연결 및 인증
	//DB인증과정....
	CDBConnector* pDBConnector = (CDBConnector*)TlsGetValue(pNetServer->TLS_DBConnectIndex);
	ULONGLONG DBLastTime = (ULONGLONG)TlsGetValue(pNetServer->TLS_DBLastTimeIndex);
	ULONGLONG DBNowTime = GetTickCount64();
	if (DBNowTime - DBLastTime > 3600000)
	{
		pDBConnector->Ping();

	}
	TlsSetValue(pNetServer->TLS_DBLastTimeIndex, (LPVOID)DBNowTime);

	bool queryret = dbConnector.sendQuery_Save(L"SELECT * FROM AccountInfo WHERE email = %S", Email.Email);
	if (queryret == false)
	{
		WCHAR ErrorMsg[128];
		wcscpy_s(ErrorMsg, dbConnector.GetLastErrorMsg());
	}

	MYSQL_ROW sql_row;
	sql_row = dbConnector.FetchRow();

	//토큰 유효성 검사

	//accountNo, PW, State 가져옴
	//일반로그인이면 여기서 pw확인, 구글로그인이면 api 호출

	














	MYSQL* connection = (MYSQL*)TlsGetValue(pNetServer->TLS_DBConnectIndex);
	ULONGLONG DBLastTime = (ULONGLONG)TlsGetValue(pNetServer->TLS_DBLastTimeIndex);
	ULONGLONG DBNowTime = GetTickCount64();
	if (DBNowTime - DBLastTime > 3600000)
	{
		mysql_ping(connection);

	}
	TlsSetValue(pNetServer->TLS_DBLastTimeIndex, (LPVOID)DBNowTime);
	char query[100];
	sprintf_s(query, "SELECT * FROM AccountInfo WHERE email = %s", AccountNo);
	PRO_BEGIN("selectQuery");
	int query_stat = mysql_query(connection, query);
	if (query_stat != 0)
	{
		char m_LastErrorMsg[128];
		WCHAR	LastErrorMsg[128];
		strcpy_s(m_LastErrorMsg, mysql_error(connection));
		MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, m_LastErrorMsg, 128, LastErrorMsg, 128);
		systemLog(L"DB error", dfLOG_LEVEL_ERROR, L"Mysql query error : %s", LastErrorMsg);
	}
	PRO_END("selectQuery");

	MYSQL_RES* sql_result = mysql_store_result(connection);
	MYSQL_ROW sql_row;
	sql_row = mysql_fetch_row(sql_result);
	INT64 ret_accountNo = atoll(sql_row[0]);//sql_row[0];
	MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, sql_row[1], strlen(sql_row[1]) + 1, pPlayer->ID.name, sizeof(st_UserName));
	MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, sql_row[3], strlen(sql_row[3]) + 1, pPlayer->Nickname.name, sizeof(st_UserName));
	mysql_free_result(sql_result);

	//DB인증 완료


}

bool CLoginServer::packetProc_CS_LOGIN_REQ_LOGIN(st_Player* pPlayer, CPacket* pPacket, INT64 SessionID)
{
	//------------------------------------------------------------
	// 로그인 서버로 클라이언트 로그인 요청
	//
	//	{
	//		WORD	Type
	//
	//		INT64	AccountNo
	//		char	SessionKey[64]
	//	}
	//
	//------------------------------------------------------------
	INT64 AccountNo;
	*pPacket >> AccountNo;

	pPlayer->AccountNo = AccountNo;

	//여기서 DB에 연결 및 인증
	//DB인증과정....
	MYSQL* connection = (MYSQL*)TlsGetValue(pNetServer->TLS_DBConnectIndex);
	ULONGLONG DBLastTime = (ULONGLONG)TlsGetValue(pNetServer->TLS_DBLastTimeIndex);
	ULONGLONG DBNowTime = GetTickCount64();
	if (DBNowTime - DBLastTime > 3600000)
	{
		mysql_ping(connection);

	}
	TlsSetValue(pNetServer->TLS_DBLastTimeIndex, (LPVOID)DBNowTime);
	char query[100];
	sprintf_s(query, "SELECT * FROM account WHERE accountno = %lld", AccountNo);
	PRO_BEGIN("selectQuery");
	int query_stat = mysql_query(connection, query);
	if (query_stat != 0)
	{
		char m_LastErrorMsg[128];
		WCHAR	LastErrorMsg[128];
		strcpy_s(m_LastErrorMsg, mysql_error(connection));
		MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, m_LastErrorMsg, 128, LastErrorMsg, 128);
		systemLog(L"DB error", dfLOG_LEVEL_ERROR, L"Mysql query error : %s", LastErrorMsg);
	}
	PRO_END("selectQuery");

	MYSQL_RES* sql_result = mysql_store_result(connection);
	MYSQL_ROW sql_row;
	sql_row = mysql_fetch_row(sql_result);
	INT64 ret_accountNo = atoll(sql_row[0]);//sql_row[0];
	MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, sql_row[1], strlen(sql_row[1]) + 1, pPlayer->ID.name, sizeof(st_UserName));
	MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, sql_row[3], strlen(sql_row[3]) + 1, pPlayer->Nickname.name, sizeof(st_UserName));
	mysql_free_result(sql_result);

	//DB인증 완료
	//Redis 통신 스레드에 JOB Toss
	st_Session* pSession;
	pPacket->addRef(1);
	st_JobItem jobItem;
	jobItem.AccountNo = AccountNo;
	jobItem.SessionID = SessionID;
	jobItem.pPacket = pPacket;
	jobItem.pPlayer = pPlayer;
	//pPlayer도 넣는다, ID, Nickname 찾는용도 
	if (JobQueue.Enqueue(jobItem) == false)
	{
		if (pPacket->subRef() == 0)
		{
			CPacket::mFree(pPacket);
		}
		return false;
	}
	SetEvent(hJobEvent);
	return true;
}



bool CLoginServer::PacketProc(st_Player* pPlayer, WORD PacketType, CPacket* pPacket, INT64 SessionID)
{
	switch (PacketType)
	{
	case en_PACKET_CS_LOGIN_REQ_LOGIN:
		return packetProc_CS_LOGIN_REQ_LOGIN(pPlayer, pPacket, SessionID);
		break;

	default:
		return false;
	}
}

size_t CLoginServer::getCharacterNum(void) // 캐릭터수
{
	return PlayerList.size();
}

LONG CLoginServer::getPlayerPoolUseSize(void)
{
	return this->PlayerPool.getUseSize();
}

LONG CLoginServer::getJobQueueUseSize(void)
{
	return this->JobQueue.nodeCount;
}

void CLoginServer::updateJobCount(void)
{
	this->JobCount = this->Temp_JobCount;
	this->Temp_JobCount = 0;
}

void CLoginServer::updateNumOfWFSO(void)
{
	this->NumOfWFSO = this->Temp_NumOfWFSO;
	this->Temp_NumOfWFSO = 0;
}

LONG CLoginServer::getJobCountperCycle(void)
{
	return this->JobCountperCycle;
}

LONG CLoginServer::getNumOfWFSO(void)
{
	return this->NumOfWFSO;
}