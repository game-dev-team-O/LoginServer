#pragma comment(lib, "winmm.lib" )
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "libmysql.lib")
#include <cpp_redis/cpp_redis>
#pragma comment (lib, "cpp_redis.lib")
#pragma comment (lib, "tacopie.lib")
#include <openssl/sha.h>
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
#include <unordered_set>
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
#include <jwt-cpp/jwt.h>


using namespace std;



CLoginServer::CLoginServer()
{
	ShutDownFlag = false;

	lastTime = 0;
	pNetServer = NULL;
	hJobEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (hJobEvent == NULL)
	{
		CrashDump::Crash();
	}
	InitializeSRWLock(&PlayerListLock);
	InitializeSRWLock(&BanListLock);
}


DWORD WINAPI CLoginServer::LogicThread(CLoginServer* pLoginServer)
{
	while (!pLoginServer->ShutDownFlag)
	{
		//시간 쟤서 모든세션의 lastPacket 확인 -> 3분이 지났다면 그세션끊기
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
			if (curTime > player.lastTime + 300000)
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
			INT64 SessionID = jobItem.SessionID;
			INT64 AccountNo = jobItem.AccountNo;
			st_Player* pPlayer = jobItem.pPlayer;

			st_Session* pSession;
			if (pLoginServer->pNetServer->findSession(SessionID, &pSession) == false)
			{
				continue;
			}
			
			st_SessionKey SessionKey;
			for (int i = 0; i < 64; i++)
			{
				SessionKey.sessionKey[i] = 'a' + rand() % 26;
			}

			//레디스에 쓰기
			std::string sessionKey_string;
			for (int i = 0; i < 64; i++)
			{
				sessionKey_string += SessionKey.sessionKey[i];
			}
			char Email[50] = { 0, };
			WideCharToMultiByte(CP_ACP, 0, pPlayer->Email.Email, -1, Email, 50, NULL, NULL);
			std::string Email_string(Email);

			client.set_advanced(Email_string, sessionKey_string, true, 600);
			client.sync_commit();

			//응답패킷 보내기
			pLoginServer->CS_LOGIN_LOGINSERVER_RES(SessionID, df_RES_SUCESS, SessionKey, pLoginServer->GameServerList[0].serverIP, pLoginServer->GameServerList[0].serverPort);

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
	WSACleanup();
	return true;
}

bool CLoginServer::Start()
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		int InitErrorCode = WSAGetLastError();
		wprintf(L"Loginserver wsastartup error : %u\n", InitErrorCode);
		return false;
	}

	maxPlayer = pNetServer->getMaxSession();

	CDBConnector dbConnector(this->DB_IP, this->DB_User, this->DB_Password, this->DB_Name, this->DB_Port);
	bool ret = dbConnector.Connect();
	if (ret != true)
	{
		WCHAR ErrorMsg[128];
		wcscpy_s(ErrorMsg, dbConnector.GetLastErrorMsg());
		systemLog(L"DB Connect Error", dfLOG_LEVEL_ERROR, L"%S", ErrorMsg);
		return false;
	}

	bool queryret = dbConnector.sendQuery_Save(L"SELECT * FROM BanList");
	if (queryret == false)
	{
		WCHAR ErrorMsg[128];
		wcscpy_s(ErrorMsg, dbConnector.GetLastErrorMsg());
		systemLog(L"DB Send Query Error", dfLOG_LEVEL_ERROR, L"%S", ErrorMsg);
		return false;
	}

	AcquireSRWLockExclusive(&BanListLock);
	MYSQL_ROW sql_row;
	while ((sql_row = dbConnector.FetchRow())!= NULL)
	{
		BanIPList.insert(atol(sql_row[0]));
	}
	ReleaseSRWLockExclusive(&BanListLock);

	dbConnector.FreeResult();
	dbConnector.Disconnect();

	hLogicThread = (HANDLE)_beginthreadex(NULL, 0, (_beginthreadex_proc_type)&LogicThread, this, 0, 0);
	hMemoryDBThread = (HANDLE)_beginthreadex(NULL, 0, (_beginthreadex_proc_type)&MemoryDBThread, this, 0, 0);
	if (hLogicThread == NULL || hMemoryDBThread == NULL)
	{
		wprintf(L"Thread init error");
		systemLog(L"LoginServer Thread init error", dfLOG_LEVEL_ERROR, L"");
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
	WSACleanup();
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
	if (ResponseType == 0)
	{
		*pPacket << SessionKey;
		*pPacket << GameServerIP;
		*pPacket << GameServerPort;
	}
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

	char LoginType;
	st_Email Email;
	st_SessionKey SessionKey;
	st_IP GameServerIP;
	int GameServerPort = 0;

	*pPacket >> LoginType;
	*pPacket >> Email;
	//여기서 원래 Email과 받은 Email이 같지 않다면 FailCount 초기화
	if (LoginType > 2 || LoginType <= 0)
	{
		return false;
	}

	if (wcscmp(Email.Email, pPlayer->Email.Email) != 0)
	{
		pPlayer->Failcount = 0;
	}

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

	bool queryret = pDBConnector->sendQuery_Save(L"SELECT * FROM AccountInfo WHERE email = \"%s\"", Email.Email);
	if (queryret == false)
	{
		WCHAR ErrorMsg[128];
		wcscpy_s(ErrorMsg, pDBConnector->GetLastErrorMsg());
		return false;
		//에러 로그찍기
	}

	MYSQL_ROW sql_row;
	sql_row = pDBConnector->FetchRow();
	
	
	if (sql_row == NULL)
	{
		//없는유저
		//reply send
		CS_LOGIN_LOGINSERVER_RES(SessionID, df_RES_WRONG_EMAIL, SessionKey, GameServerIP, GameServerPort);
		pPlayer->Failcount++;
		if (pPlayer->Failcount == MAX_FAILCOUNT)
		{
			int BanIP = pNetServer->getSessionIP(SessionID);
			addBanIP(BanIP);
			pDBConnector->sendQuery(L"INSERT INTO BanList VALUES (%d)", BanIP);
		}
		return true;
	}
	

	//DB에서 accountNo, PW, State 가져옴
	unsigned char encryptedPW[32] = { 0, };
	INT64 accountNo = atoll(sql_row[0]);
	if (sql_row[2] != NULL)
	{
		memcpy(encryptedPW, sql_row[2], 32);
	}
	char state = (char)atoi(sql_row[3]);
	pDBConnector->FreeResult();

	if (state != 0)
	{
		//계정정지상태
		//reply send
		CS_LOGIN_LOGINSERVER_RES(SessionID, df_RES_BAN_EMAIL, SessionKey, GameServerIP, GameServerPort);
		pPlayer->Failcount++;
		if (pPlayer->Failcount == MAX_FAILCOUNT)
		{
			int BanIP = pNetServer->getSessionIP(SessionID);
			addBanIP(BanIP);
			pDBConnector->sendQuery(L"INSERT INTO BanList VALUES (%d)", BanIP);
		}
		return true;
	}

	//토큰 분석
	short TokenLength;

	*pPacket >> TokenLength;
	if (TokenLength <= 0 || TokenLength > 2048 || (pPacket->GetDataSize() != TokenLength))
	{
		// 잘못된 토큰길이
		//reply send
		CS_LOGIN_LOGINSERVER_RES(SessionID, df_RES_WRONG_TOKEN, SessionKey, GameServerIP, GameServerPort);
		pPlayer->Failcount++;
		if (pPlayer->Failcount == MAX_FAILCOUNT)
		{
			int BanIP = pNetServer->getSessionIP(SessionID);
			addBanIP(BanIP);
			pDBConnector->sendQuery(L"INSERT INTO BanList VALUES (%d)", BanIP);
		}
		return true;
	}


	//일반로그인이면 여기서 pw확인, 구글로그인이면 api 호출
	switch (LoginType)
	{
	//if 구글로그인 -> 토큰 유효성 검사 -> 성공시 답장쏴줌. 실패시 FailCount++
	case df_LOGIN_GOOGLE_LOGIN:
	{
		std::string aud = "953975431943-c483uk1qdkaj5qs5grgs51fa1ghlagci.apps.googleusercontent.com";

		char Token[2048];
		pPacket->GetData((char*)Token, TokenLength);

		string JWT_token = Token;
		auto decoded = jwt::decode(JWT_token);
		
		bool audFlag = false;
		bool expFlag = false;

		for (auto& e : decoded.get_payload_json())
		{
			if (e.first == "aud")
			{
				std::string tempstring = e.second.to_str();
				if (aud == tempstring)
				{
					//aud 인증 완료
					audFlag = true;
				}
				else
				{
					//aud 인증 실패, 실패 토큰 보내기
					CS_LOGIN_LOGINSERVER_RES(SessionID, df_RES_WRONG_TOKEN, SessionKey, GameServerIP, GameServerPort);
					return true;
				}
			}
			else if (e.first == "exp")
			{
				time_t seconds;
				time(&seconds);
				int seconds_int = (int)seconds;

				std::string tempstring = e.second.to_str();
				int exptime = std::stoi(tempstring);

				expFlag = true;
				/*
				if (seconds_int > exptime)
				{
					//토큰 만료
					CS_LOGIN_LOGINSERVER_RES(SessionID, df_RES_WRONG_TOKEN, SessionKey, GameServerIP, GameServerPort);
					return true;
				}
				else
				{
					expFlag = true;
				}
				*/
			}
		}

		if (audFlag == false || expFlag == false)
		{
			return true;
		}
		break;
	}
	//if 자체로그인 -> 받은 PW 암호화, encrypted PW와 비교 -> 성공시 답장쏴줌, 실패시 FailCount++
	case df_LOGIN_SELF_LOGIN:
	{
		unsigned char Token[2048];
		pPacket->GetData((char*)Token, TokenLength);

		unsigned char encryptedToken[32] = { 0, };

		SHA256_CTX sha256;
		SHA256_Init(&sha256);
		SHA256_Update(&sha256, Token, TokenLength);
		SHA256_Final(encryptedToken, &sha256);

		if (memcmp(encryptedPW, encryptedToken, 32) != 0)
		{
			// 잘못된 패스워드
			//	reply send
			CS_LOGIN_LOGINSERVER_RES(SessionID, df_RES_WRONG_TOKEN, SessionKey, GameServerIP, GameServerPort);
			pPlayer->Failcount++;
			if (pPlayer->Failcount == MAX_FAILCOUNT)
			{
				int BanIP = pNetServer->getSessionIP(SessionID);
				addBanIP(BanIP);
				pDBConnector->sendQuery(L"INSERT INTO BanList VALUES (%d)", BanIP);
				return true;
			}
		}
		break;
	}
	}

	//FailCount == 5일시 Ban List에 추가 및 DB에 올림. Ban List는 서버 시작시 한번 가져옴 << 레드블랙트리(unordered set) 사용
	//인증 완료

	//redis 저장 스레드에 잡 만들어서 toss
	st_JobItem jobItem;
	jobItem.AccountNo = accountNo;
	jobItem.SessionID = SessionID;
	jobItem.pPlayer = pPlayer;
	JobQueue.Enqueue(jobItem);
	SetEvent(hJobEvent);
	return true;

}


bool CLoginServer::PacketProc(st_Player* pPlayer, WORD PacketType, CPacket* pPacket, INT64 SessionID)
{
	switch (PacketType)
	{
	case en_LOGIN_LOGINSERVER_REQ:
		return packetProc_CS_LOGIN_LOGINSERVER_REQ(pPlayer, pPacket, SessionID);
		break;

	default:
		return false;
	}
}

void CLoginServer::setDBInfo(WCHAR* DB_IP, WCHAR* DB_User, WCHAR* DB_Password, WCHAR* DB_Name, int DB_Port)
{
	wcscpy_s(this->DB_IP, DB_IP);
	wcscpy_s(this->DB_User, DB_User);
	wcscpy_s(this->DB_Password, DB_Password);
	wcscpy_s(this->DB_Name, DB_Name);
	this->DB_Port = DB_Port;
}

void CLoginServer::setGameServerInfo(WCHAR* GameServer_IP, int GameServer_Port)
{
	wcscpy_s(this->GameServerList[0].serverIP.IP, GameServer_IP);
	this->GameServerList[0].serverPort = GameServer_Port;
}

void CLoginServer::addBanIP(int BanIP)
{
	AcquireSRWLockExclusive(&BanListLock);
	BanIPList.insert(BanIP);
	ReleaseSRWLockExclusive(&BanListLock);
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