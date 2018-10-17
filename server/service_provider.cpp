#include "service_provider.h"
#include "clibase.h"
#include "rapidjson/json.hpp"
#include "cloud/const.h"
#include "comm/strparse.h"

ServiceItem::ServiceItem( void ): svrid(0), okcount(0), ngcount(0), tmpnum(0),
	 protocol(0),version(0), weight(0), idc(0), rack(0), enable(false)
{

}

bool ServiceItem::valid( void ) const
{
	return enable && protocol > 0 && svrid > 0 && weight > 0 && !url.empty();
}

int ServiceItem::parse0( const string& svrname, CliBase* cli )
{
	svrid = cli->getIntProperty("svrid");
	ERRLOG_IF1RET_N(svrname.empty() || svrid <= 0, -70, "SERVITEMPARSE| msg=parse fail| cli=%s", cli->m_idProfile.c_str());

	this->svrname = svrname;
	idc = cli->getIntProperty("idc");
	rack = cli->getIntProperty("rack");
	return 0;
}

int ServiceItem::parse( CliBase* cli )
{
	// 固定部份-不会随运行过程改变的
	if (0 == version || 0 == protocol || url.empty())
	{
		url = cli->getProperty(svrname + ":url");
		protocol = cli->getIntProperty(svrname + ":protocol");
		version = cli->getIntProperty(svrname + ":version");		
	}

	// 变化部分
	desc = cli->getProperty(svrname + ":desc");
	okcount = cli->getIntProperty(svrname + ":okcount");
	ngcount = cli->getIntProperty(svrname + ":ngcount");
	weight = cli->getIntProperty(svrname + ":weight");
	enable = cli->getIntProperty(svrname + ":enable");
	return 0;
}

void ServiceItem::getJsonStr( string& strjson, int oweight ) const
{
	strjson.append("{");
	StrParse::PutOneJson(strjson, "svrname", svrname, true);
	StrParse::PutOneJson(strjson, "url", url, true);
	StrParse::PutOneJson(strjson, "desc", desc, true);
	StrParse::PutOneJson(strjson, "svrid", svrid, true);
	StrParse::PutOneJson(strjson, "okcount", okcount, true);
	StrParse::PutOneJson(strjson, "ngcount", ngcount, true);
	StrParse::PutOneJson(strjson, "protocol", protocol, true);
	StrParse::PutOneJson(strjson, "version", version, true);
	// 权重，当服务消费应用调用时，weight=匹配分数值
	StrParse::PutOneJson(strjson, "weight", oweight>0? oweight: weight, true);
	StrParse::PutOneJson(strjson, "idc", idc, true);
	StrParse::PutOneJson(strjson, "rack", rack, true);
	StrParse::PutOneJson(strjson, "enable", enable, false);
	strjson.append("}");
}

void ServiceItem::getCalcJson( string& strjson, int oweight ) const
{
	getJsonStr(strjson, oweight);
}

int ServiceItem::score( short oidc, short orack ) const
{
	static const int match_mult1 = 4;
	static const int match_mult2 = 8;
	int score = 0;
	int calc_weight = this->weight>0? this->weight : 1;

	if (oidc > 0 && oidc == this->idc) // 同一机房
	{
		score += (orack > 0 && orack == this->rack)? calc_weight*match_mult2 : calc_weight*match_mult1;
	}

	score += calc_weight;
	score += (rand()&0x7);
	return score;
}

ServiceProvider::ServiceProvider( const string& svrName ): m_svrName(svrName)
{

}

ServiceProvider::~ServiceProvider( void )
{
	map<CliBase*, ServiceItem*>::iterator itr = m_svrItems.begin();
	for (; itr != m_svrItems.end(); ++itr)
	{
		//CliBase* first = itr->first;
		ServiceItem* second = itr->second;
		IFDELETE(second);
	}
}

int ServiceProvider::setItem( CliBase* cli )
{
	ServiceItem* pitem = NULL;
	map<CliBase*, ServiceItem*>::iterator itr = m_svrItems.find(cli);
	if (m_svrItems.end() == itr)
	{
		pitem = new ServiceItem;
		int ret = pitem->parse0(m_svrName, cli);
		if (ret)
		{
			IFDELETE(pitem);
			return ret;
		}
		m_svrItems[cli] = pitem;
		string provval = cli->getProperty(SVRPROVIDER_CLI_KEY);
		cli->setProperty(SVRPROVIDER_CLI_KEY, provval.empty()? m_svrName: string("+")+provval);
	}
	else
	{
		pitem = itr->second;
	}
	
	return  pitem->parse(cli);
}

void ServiceProvider::removeItme( CliBase* cli )
{
	map<CliBase*, ServiceItem*>::iterator itr = m_svrItems.find(cli);
	if (itr != m_svrItems.end())
	{
		IFDELETE(itr->second);
		m_svrItems.erase(itr);
	}
}

// 返回json-array[]形式
int ServiceProvider::getAllJson( string& strjson ) const
{
	strjson.append("[");
	int i = 0;
	map<CliBase*, ServiceItem*>::const_iterator itr = m_svrItems.begin();
	for (int i = 0; itr != m_svrItems.end(); ++itr, ++i)
	{
		if (i > 0) strjson.append(",");
		itr->second->getJsonStr(strjson);
	}

	strjson.append("]");
	return i;
}

/**
 * @summery: 消息者查询可用服务列表
 * @remark: 调用时采用客户端负载均衡方式
 * @return: 返回可用服务个数
 * @param: strjson [out] 返回json字符串[array格式]
 **/
int ServiceProvider::query( string& strjson, short idc, short rack, short version, short limit ) const
{
	// sort all item by score
	map<int, ServiceItem*> sortItemMap;
	map<CliBase*, ServiceItem*>::const_iterator itr = m_svrItems.begin();
	for (; itr != m_svrItems.end(); ++itr)
	{
		ServiceItem* ptr = itr->second;
		if (!ptr->valid()) continue;
		if (version > 0 && version != ptr->version) continue;

		int score = ptr->score(idc, rack);
		ptr->tmpnum = score;
		while (NULL != sortItemMap[score])
		{
			score++;
		}
		sortItemMap[score] = ptr;
	}
	
	short count = 0;
	strjson.append("[");
	map<int, ServiceItem*>::reverse_iterator ritr = sortItemMap.rbegin();
	for (; count < limit && sortItemMap.rend() != ritr; ++ritr, ++count)
	{
		ServiceItem* ptr = itr->second;
		if (count > 0) strjson.append(",");
		ptr->getCalcJson(strjson, ptr->tmpnum);
	}
	strjson.append("]");
	
	return count;
}