#pragma once

#define MAX_SERVERNUM 10
#define MAX_FAILCOUNT 5
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
        SHORT Failcount;
        INT64 AccountNo;
        st_Email Email;
        INT64 JoinFlag;
        INT64 sessionID;
        ULONGLONG lastTime;
    };

    struct st_JobItem
    {
        INT64 AccountNo;
        INT64 SessionID;
        st_Player* pPlayer;
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
    bool PacketProc(st_Player* pPlayer, WORD PacketType, CPacket* pPacket, INT64 SessionID);

    void setGameServerInfo(WCHAR* GameServer_IP, int GameServer_Port);
    void setDBInfo(WCHAR* DB_IP, WCHAR* DB_User, WCHAR* DB_Password, WCHAR* DB_Name, int DB_Port);
    void addBanIP(int BanIP);
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

    WCHAR DB_IP[16];
    WCHAR DB_User[50];
    WCHAR DB_Password[50];
    WCHAR DB_Name[50];
    int DB_Port;

    ULONGLONG lastTime;
    CNetServer* pNetServer;
    unordered_map<INT64, st_Player*> PlayerList; // key : sessionID, value : Player*, 모든 로직 사용
    SRWLOCK PlayerListLock;

    unordered_set<int> BanIPList;
    SRWLOCK BanListLock;

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

        //outParam으로 JoinFlag를 지정함
        //밴된 IP를 찾아봄.
        SOCKADDR_IN sockAddr;
        InetPtonW(AF_INET, IP, &sockAddr.sin_addr);
        int IP_INT = ntohl(sockAddr.sin_addr.s_addr);
        AcquireSRWLockShared(&pLoginServer->BanListLock);
        if (pLoginServer->BanIPList.find(IP_INT) != pLoginServer->BanIPList.end())
        {
            ReleaseSRWLockShared(&pLoginServer->BanListLock);
            return false;
        }
        else
        {
            ReleaseSRWLockShared(&pLoginServer->BanListLock);
            return true;
        }
    }

    virtual void OnClientJoin(INT64 sessionID, int JoinFlag)
    {
        //캐릭터 정보를 담을 구조체 초기화, 안에 넣어줌

        CLoginServer::st_Player* pNewPlayer;
        pLoginServer->PlayerPool.mAlloc(&pNewPlayer);
        pNewPlayer->isPacketRecv = FALSE;
        pNewPlayer->AccountNo = 0;
        pNewPlayer->JoinFlag = JoinFlag;
        wcscpy_s(pNewPlayer->Email.Email, L"NULL");
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
