#pragma once

#define df_LOGIN_GOOGLE_LOGIN 1
#define df_LOGIN_SELF_LOGIN 2

#define df_RES_SUCESS 0
#define df_RES_EXPIRED_TOKEN 1
#define df_RES_WRONG_TOKEN 2
#define df_RES_WRONG_EMAIL 3
#define df_RES_BAN_EMAIL 4
#define df_RES_BAN_IP 5

enum en_PROTOCOL_TYPE
{
	
	////////////////////////////////////////////////////////
	//
	//	Client & Server Protocol
	//
	////////////////////////////////////////////////////////

	en_LOGIN_LOGINSERVER_REQ = 1,
	//------------------------------------------------------------
	// Client �� Login Server �α��� ��û
	//
	//	{
	//		SHORT	Type
	//
	//		char    LoginType // google �α���(1), ��ü�α���(2)
	//		WCHAR	Email[50]; // �α��ο� �̸���
	//		short   TokenLength // ���� ���� ��ū ����(LoginType = 2�Ͻ� ����)
	//		char	Token[...]	// ���� ���� ��ū 1~2048 bytes
	//	}
	//
	//------------------------------------------------------------

	en_LOGIN_LOGINSERVER_RES = 2,
	//------------------------------------------------------------
	// Login Server �� Client �α��� ����
	//
	//	{
	//		SHORT	Type
	//
	//		char    ResponseType //  ����(1), �߸�����ū(2), ���°���(3), ������ ����(4), �� �� IP(5), ����(0)
	//		char	Key[64]; // ����Ű. ���Ӽ����� �α��ν� �ش� Ű�� ���� �۽��Ѵ�
	//		WCHAR   IP[20]  // ������ ���Ӽ����� ip
	//		int     Port	// ������ ���Ӽ����� port
	//	}
	//
	//------------------------------------------------------------



	en_LOGIN_GAMESERVER_REQ = 3,
	en_LOGIN_GAMESERVER_RES = 4,

};

