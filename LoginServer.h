#pragma once

#define MAX_SERVERNUM 10
using namespace std;

enum {
    TARGET_SERVER,
    TARGET_SERVER_1,
    TARGET_SERVER_2,
    TARGET_SERVER_3
};

struct st_IP
{
    WCHAR IP[16];
};

struct st_ServerAddress
{
    st_IP serverIP;
    int serverPort;
};

class CLoginServer
{
    friend class CContentsHandler;

public:

    struct st_UserName
    {
        WCHAR name[20];
    };

    struct st_SessionKey
    {
        char sessionKey[64];
    };

    struct st_Email
    {
        WCHAR Email[50];
    };

    struct st_Player
    {
        BOOL isPacketRecv;
        INT64 AccountNo;
        st_Email Email;
        INT64 JoinFlag;
        st_UserName Nickname;
        INT64 sessionID;
        ULONGLONG lastTime;
    };

    struct st_JobItem
    {
        INT64 AccountNo;
        INT64 SessionID;
        st_Player* pPlayer;
        CPacket* pPacket;
    };


    CLoginServer();
    void attachServerInstance(CNetServer* networkServer)
    {
        pNetServer = networkServer;
    }
    static DWORD WINAPI LogicThread(CLoginServer* pLoginServer);
    static DWORD WINAPI MemoryDBThread(CLoginServer* pLoginServer);
    bool Start();
    bool Stop();

    //패킷 프로시저들!!
    void CS_LOGIN_LOGINSERVER_RES(INT64 SessionID, char ResponseType, st_SessionKey& sessionKey, st_IP& GameServerIP, int GameServerPort);
    bool packetProc_CS_LOGIN_LOGINSERVER_REQ(st_Player* pPlayer, CPacket* pPacket, INT64 SessionID);
    bool packetProc_CS_LOGIN_REQ_LOGIN(st_Player* pPlayer, CPacket* pPacket, INT64 SessionID);
    bool PacketProc(st_Player* pPlayer, WORD PacketType, CPacket* pPacket, INT64 SessionID);

    size_t getCharacterNum(void);
    LONG getPlayerPoolUseSize(void);
    LONG getJobQueueUseSize(void);
    void updateJobCount(void);
    LONG getJobCountperCycle(void);
    void updateNumOfWFSO(void);
    LONG getNumOfWFSO(void);
    ULONGLONG Interval = 0;

    int JobCount = 0;
    int NumOfWFSO = 0;
    int JobCountperCycle = 0;

    int Temp_JobCount = 0;
    int Temp_NumOfWFSO = 0;
    int Temp_JobCountperCycle = 0;

    st_ServerAddress ChatServerList[MAX_SERVERNUM];
    st_ServerAddress GameServerList[MAX_SERVERNUM];

private:
  
    HANDLE hLogicThread;
    HANDLE hMemoryDBThread;
    volatile bool ShutDownFlag;
    int maxPlayer;

    ULONGLONG lastTime;
    CNetServer* pNetServer;
    unordered_map<INT64, st_Player*> PlayerList; // key : sessionID, value : Player*, 모든 로직 사용
    alignas(64) SRWLOCK PlayerListLock;
    alignas(64) LockFreeQueue<st_JobItem> JobQueue;
    alignas(64) CMemoryPool<CLoginServer::st_Player> PlayerPool;
    HANDLE hJobEvent;
};

class CContentsHandler : public CNetServerHandler
{
public:
    void attachServerInstance(CNetServer* networkServer, CLoginServer* contentsServer)
    {
        pNetServer = networkServer;
        pLoginServer = contentsServer;
    }
    virtual bool OnConnectionRequest(WCHAR* IP, int* outParam) 
    { 
        if (wcsncmp(IP, L"10.0.1.2", 16) == 0)
        {
            *outParam = TARGET_SERVER_1;
        }

        else if (wcsncmp(IP, L"10.0.2.2", 16) == 0)
        {
            *outParam = TARGET_SERVER_2;
        }

        else if (wcsncmp(IP, L"127.0.0.1", 16) == 0)
        {
            *outParam = TARGET_SERVER_3;
        }

        return true; 
    }

    virtual void OnClientJoin(INT64 sessionID, int JoinFlag)
    {
        //캐릭터 정보를 담을 구조체 초기화, 안에 넣어줌

        CLoginServer::st_Player* pNewPlayer;
        pLoginServer->PlayerPool.mAlloc(&pNewPlayer);
        pNewPlayer->isPacketRecv = FALSE;
        pNewPlayer->AccountNo = 0;
        pNewPlayer->JoinFlag = JoinFlag;
        wcscpy_s(pNewPlayer->ID.name, L"NULL");
        wcscpy_s(pNewPlayer->Nickname.name, L"NULL");
        pNewPlayer->sessionID = sessionID;
        pNewPlayer->lastTime = GetTickCount64();

        AcquireSRWLockExclusive(&pLoginServer->PlayerListLock);
        pLoginServer->PlayerList.insert(make_pair(sessionID, pNewPlayer));
        ReleaseSRWLockExclusive(&pLoginServer->PlayerListLock);
    }

    virtual void OnClientLeave(INT64 sessionID)
    {
        AcquireSRWLockExclusive(&pLoginServer->PlayerListLock);
        auto item = pLoginServer->PlayerList.find(sessionID);
        if (item == pLoginServer->PlayerList.end())
        {
            ReleaseSRWLockExclusive(&pLoginServer->PlayerListLock);
            return;
        }
        else
        {
            CLoginServer::st_Player* pPlayer = item->second;
            pLoginServer->PlayerList.erase(item);
            ReleaseSRWLockExclusive(&pLoginServer->PlayerListLock);
            pLoginServer->PlayerPool.mFree(pPlayer);
        }
    }

    virtual bool OnRecv(INT64 SessionID, CPacket* pPacket)
    {
        pPacket->addRef(1);
        WORD packetType;
        *pPacket >> packetType;

        AcquireSRWLockShared(&pLoginServer->PlayerListLock);
        auto item = pLoginServer->PlayerList.find(SessionID);
        if (item == pLoginServer->PlayerList.end())
        {
            ReleaseSRWLockShared(&pLoginServer->PlayerListLock);
            if (pPacket->subRef() == 0)
            {
                CPacket::mFree(pPacket);
            }
            return false;
        }

        CLoginServer::st_Player& player = *item->second;
        ReleaseSRWLockShared(&pLoginServer->PlayerListLock);

        //패킷 프로시져 타기
        player.lastTime = GetTickCount64();
        bool ret = pLoginServer->PacketProc(&player, packetType, pPacket, SessionID);
        if (ret == false)
        {
            //아래부분 함수로 래핑
            st_Session* pSession;
            if (pLoginServer->pNetServer->findSession(player.sessionID, &pSession) == true)
            {
                pLoginServer->pNetServer->disconnectSession(pSession);
                if (InterlockedDecrement(&pSession->IOcount) == 0)
                {
                    pLoginServer->pNetServer->releaseSession(player.sessionID);
                }
            }
        }

        if (pPacket->subRef() == 0)
        {
            CPacket::mFree(pPacket);
        }


        return true;
        
    }

    virtual void OnError(int errorCode)
    {

    }

private:
    CNetServer* pNetServer;
    CLoginServer* pLoginServer;
};

inline CPacket& operator << (CPacket& packet, CLoginServer::st_UserName& userName)
{

    if (packet.GetLeftUsableSize() >= sizeof(CLoginServer::st_UserName))
    {
        memcpy(packet.GetWriteBufferPtr(), userName.name, sizeof(CLoginServer::st_UserName));
        packet.MoveWritePos(sizeof(CLoginServer::st_UserName));
    }
    return packet;
}

inline CPacket& operator << (CPacket& packet, CLoginServer::st_SessionKey& SessionKey)
{

    if (packet.GetLeftUsableSize() >= sizeof(CLoginServer::st_SessionKey))
    {
        memcpy(packet.GetWriteBufferPtr(), SessionKey.sessionKey, sizeof(CLoginServer::st_SessionKey));
        packet.MoveWritePos(sizeof(CLoginServer::st_SessionKey));
    }
    return packet;
}

inline CPacket& operator << (CPacket& packet, st_IP& IP)
{

    if (packet.GetLeftUsableSize() >= sizeof(st_IP))
    {
        memcpy(packet.GetWriteBufferPtr(), IP.IP, sizeof(st_IP));
        packet.MoveWritePos(sizeof(st_IP));
    }
    return packet;
}

inline CPacket& operator >> (CPacket& packet, CLoginServer::st_UserName& userName)
{
    if (packet.GetDataSize() >= sizeof(CLoginServer::st_UserName))
    {
        memcpy(userName.name, packet.GetReadBufferPtr(), sizeof(CLoginServer::st_UserName));
        packet.MoveReadPos(sizeof(CLoginServer::st_UserName));
    }
    return packet;
}


inline CPacket& operator >> (CPacket& packet, CLoginServer::st_SessionKey& SessionKey)
{
    if (packet.GetDataSize() >= sizeof(CLoginServer::st_SessionKey))
    {
        memcpy(SessionKey.sessionKey, packet.GetReadBufferPtr(), sizeof(CLoginServer::st_SessionKey));
        packet.MoveReadPos(sizeof(CLoginServer::st_SessionKey));
    }
    return packet;
}

inline CPacket& operator >> (CPacket& packet, CLoginServer::st_Email& Email)
{
    if (packet.GetDataSize() >= sizeof(CLoginServer::st_Email))
    {
        memcpy(Email.Email, packet.GetReadBufferPtr(), sizeof(CLoginServer::st_Email));
        packet.MoveReadPos(sizeof(CLoginServer::st_Email));
    }
    return packet;
}
