#include "SqliteAppender.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <fstream>

#include <czmq.h>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <log4cplus/loglevel.h>

#include "Base/czmq_defines.h"
#include "czmp_log_protocol.pb.h"

#define THIS_MODULE "SqliteLog"

using namespace log4cplus;
using namespace log4cplus::helpers;

const static int GET_LOG_TIMEOUT = 1000 * 20;

SqliteAppender::SqliteAppender(
	std::string databaseFile, 
	int maxRow, 
	int fileSizeMaintainInterval,
	bool immediateFlush)
{
	mSqliteMaintainThread = new SqliteMaintainThread(
		databaseFile, maxRow, fileSizeMaintainInterval, immediateFlush);
	mSqliteMaintainThread->start();
	mPushSock = zsock_new_push(LOGER_SERVICE_URI);
	assert(mPushSock);
}

SqliteAppender::~SqliteAppender()
{
	close();
	destructorImpl();
}

void SqliteAppender::close()
{
	if (mPushSock) {
		zsock_destroy(&mPushSock);
	}

	if (mSqliteMaintainThread) {
		delete mSqliteMaintainThread;
		mSqliteMaintainThread = nullptr;
	}
}

void SqliteAppender::append(const log4cplus::spi::InternalLoggingEvent & event)
{
	log4cplus::LogLevel level = event.getLogLevel();
	if (level < log4cplus::DEBUG_LOG_LEVEL) {
		return;
	}

	std::string logTime = LOG4CPLUS_TSTRING_TO_STRING(
		helpers::getFormattedTime(L"%Y-%m-%d %H:%M:%S.%q", event.getTimestamp())
	);

	std::string logFile = LOG4CPLUS_TSTRING_TO_STRING(event.getFile());
	int pos = logFile.find_last_of('\\');
	if (pos != std::string::npos) {
		logFile = logFile.substr(pos + 1);
	}
	pos = logFile.find_last_of(".");
	std::string logModule = logFile.substr(0, pos);
	std::string logMsg = LOG4CPLUS_TSTRING_TO_STRING(event.getMessage());
	std::string logData = LOG4CPLUS_TSTRING_TO_STRING(event.getFunction());
	if (logData.size() > MAX_LOG_DATA_SIZE) {
		logData.resize(MAX_LOG_DATA_SIZE);
		logData += "...";
	}

	addLog(logTime, level, logModule, logMsg, logData);
}

std::string &SqliteAppender::trimLog(std::string &log)
{
	std::string strSrc = "'";
	std::string strDes = " ";

	std::string::size_type pos = 0;
	std::string::size_type srcLen = strSrc.size();
	std::string::size_type desLen = strDes.size();

	pos = log.find(strSrc, pos);
	while (pos != std::string::npos) {
		log.replace(pos, srcLen, strDes);
		pos = log.find(strSrc, (pos + desLen));
	}

	pos = log.find_last_of('\n');
	while (pos != std::string::npos) {
		if (pos == log.size() - 1) {
			log = log.substr(0, log.size() - 1);
		}
		else {
			break;
		}
	}

	return log;
}

//产生1条日志
void SqliteAppender::addLog(std::string &logTime, log4cplus::LogLevel flag,
	std::string logModule, std::string &logMsg, std::string &logData)
{
	if (logMsg.empty() || !mPushSock) {
		return;
	}

	//Log 格式: [Log time][Log source][Log type][Log msg][Log data]
	zmsg_t *msg = zmsg_new();
	zmsg_add_stdstring(msg, trimLog(logTime));
	zmsg_add_stdstring(msg, trimLog(logModule));
	zmsg_add_int(msg, flag);
	zmsg_add_stdstring(msg, trimLog(logMsg));
	if (!logData.empty()) {
		zmsg_add_stdstring(msg, trimLog(logData));
	}

	//zmsg_dump(msg);

	zmsg_send(&msg, mPushSock);
}


void SqliteAppender::getLog(const LogRequestParams &param, std::vector<LogItem> &logs)
{
	logs.clear();
	logs.reserve(param.maxRows);
	
	LogGetterParams requestParam;
	requestParam.set_startlevel(param.startLevel);
	requestParam.set_datestart(param.dateStart);
	requestParam.set_dateend(param.dateEnd);
	requestParam.set_fliter(param.fliter);
	requestParam.set_maxrows(param.maxRows);

	std::string paramString = requestParam.SerializePartialAsString();
	if (paramString.empty()) {
		return;
	}

	zmsg_t *msg = requestCmd(GET_LOG_CMD, paramString, GET_LOG_TIMEOUT);
	if (!msg) {
		return;
	}

	std::string logString = zmsg_pop_stdstring(msg);
	zmsg_destroy(&msg);

	if (logString.empty())
		return;

	parseLogs(logString, logs);
}

zmsg_t* SqliteAppender::requestCmd(const char *cmd, const std::string &param, int timeout)
{
	zsock_t *reqSock = zsock_new_req(SQLITE_LOGGER_CTRL_INPROC);
	assert(reqSock);

	zmsg_t *msg = zmsg_new();
	zmsg_addstr(msg, cmd);
	zmsg_addstr(msg, param.c_str());

	zmsg_send(&msg, reqSock);
	zpoller_t *poller = zpoller_new(reqSock, NULL);
	zsock_t *which = (zsock_t *)zpoller_wait(poller, timeout);
	if (which != reqSock) {
		zpoller_destroy(&poller);
		zsock_destroy(&reqSock);

		return nullptr;
	}

	zmsg_t *resMsg = zmsg_recv(reqSock);
	if (resMsg && 
		zmsg_pop_stdstring(resMsg) != std::string(cmd)) {
		zmsg_destroy(&resMsg);
		resMsg = NULL;
	}
	
	zpoller_destroy(&poller);
	zsock_destroy(&reqSock);

	return resMsg;
}

void SqliteAppender::parseLogs(const std::string &logString, std::vector<LogItem> &logs)
{
	LogVector logVector;
	logVector.ParseFromString(logString);

	const ::google::protobuf::RepeatedPtrField<LogDetail> &logRes = logVector.log();
	for(::google::protobuf::RepeatedPtrField<LogDetail>::const_iterator it = logRes.begin(); 
		it != logRes.end(); 
		++it) {
		LogItem item;
		item.logTime = it->logtime();
		item.logModule = it->logmodule();
		item.logLevel = it->loglevel();
		item.logMsg = it->logmsg();
		item.logData = it->logdata();

		logs.emplace_back(item);
	}

	logVector.clear_log();
}