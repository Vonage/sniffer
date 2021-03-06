#include "voipmonitor.h"
#include <algorithm>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

#include "sql_db.h"
#include "tools.h"
#include "cleanspool.h"
#include "tar.h"


using namespace std;


extern CleanSpool *cleanSpool[2];
extern MySqlStore *sqlStore;
extern int opt_newdir;
extern int opt_pcap_split;
extern int opt_pcap_dump_tar;


#define DISABLE_CLEANSPOOL ((suspended && !critical_low_space) || do_convert_filesindex_flag)


CleanSpool::CleanSpool(int spoolIndex) {
	this->spoolIndex = spoolIndex;
	this->loadOpt();
	sqlDb = NULL;
	maxpoolsize_set = 0;
	critical_low_space = false;
	do_convert_filesindex_flag = false;
	do_convert_filesindex_reason = NULL;
	clean_thread = 0;
	lastCall_reindex_all = 0;
	suspended = false;
	clean_spooldir_run_processing = 0;
}

CleanSpool::~CleanSpool() {
	termCleanThread();
	if(sqlDb) {
		delete sqlDb;
	}
}

void CleanSpool::addFile(const char *ymdh, eTypeSpoolFile typeSpoolFile, const char *file, long long int size) {
	if(!opt_newdir) {
		return;
	}
	string column = string(getSpoolTypeFilesIndex(typeSpoolFile, true)) + "size";
	sqlStore->lock(STORE_PROC_ID_CLEANSPOOL + spoolIndex);
	sqlStore->query( 
	       "INSERT INTO files \
		SET datehour = " + string(ymdh) + ", \
		    spool_index = " + getSpoolIndex_string() + ", \
		    id_sensor = " + getIdSensor_string() + ", \
		    " + column + " = " + intToString(size) + " \
		ON DUPLICATE KEY UPDATE \
		    " + column + " = " + column + " + " + intToString(size),
		STORE_PROC_ID_CLEANSPOOL + spoolIndex);
	string fname = getSpoolDir_string(tsf_main) + "/filesindex/" + column + '/' + ymdh;
	ofstream fname_stream;
	for(int passOpen = 0; passOpen < 2; passOpen++) {
		if(passOpen == 1) {
			size_t posLastDirSeparator = fname.rfind('/');
			if(posLastDirSeparator != string::npos) {
				string fname_path = fname.substr(0, posLastDirSeparator);
				mkdir_r(fname_path, 0777);
			} else {
				break;
			}
		}
		fname_stream.open(fname.c_str(), ios::app | ios::out);
		if(fname_stream.is_open()) {
			break;
		}
	}
	if(fname_stream.is_open()) {
		fname_stream << skipSpoolDir(typeSpoolFile, spoolIndex, file) << ":" << size << "\n";
		fname_stream.close();
	} else {
		syslog(LOG_ERR, "error write to %s", fname.c_str());
	}
	sqlStore->unlock(STORE_PROC_ID_CLEANSPOOL + spoolIndex);
}

void CleanSpool::run() {
	runCleanThread();
}

void CleanSpool::do_convert_filesindex(const char *reason) {
	do_convert_filesindex_flag = true;
	do_convert_filesindex_reason = reason;
}

void CleanSpool::check_filesindex() {
	list<string> date_dirs;
	this->readSpoolDateDirs(&date_dirs);
	if(!date_dirs.size()) {
		return;
	}
	SqlDb *sqlDb = createSqlObject();
	syslog(LOG_NOTICE, "cleanspool[%i]: check_filesindex start", spoolIndex);
	for(list<string>::iterator iter_date_dir = date_dirs.begin(); iter_date_dir != date_dirs.end(); iter_date_dir++) {
		check_index_date(*iter_date_dir, sqlDb);
	}
	syslog(LOG_NOTICE, "cleanspool[%i]: check_filesindex done", spoolIndex);
	delete sqlDb;
}

void CleanSpool::check_index_date(string date, SqlDb *sqlDb) {
	for(int h = 0; h < 24 && !is_terminating(); h++) {
		char hour[8];
		sprintf(hour, "%02d", h);
		string ymdh = string(date.substr(0,4)) + date.substr(5,2) + date.substr(8,2) + hour;
		map<string, long long> typeSize;
		reindex_date_hour(date, h, true, &typeSize, true);
		if(typeSize["sip"] || typeSize["reg"] || typeSize["skinny"] || typeSize["rtp"] || typeSize["graph"] || typeSize["audio"]) {
			bool needReindex = false;
			sqlDb->query(
			       "select * \
				from files \
				where datehour = '" + ymdh + "' and \
				      spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string());
			SqlDb_row row = sqlDb->fetchRow();
			if(row) {
				if((typeSize["sip"] && !atoll(row["sipsize"].c_str())) ||
				   (typeSize["reg"] && !atoll(row["regsize"].c_str())) ||
				   (typeSize["skinny"] && !atoll(row["skinnysize"].c_str())) ||
				   (typeSize["rtp"] && !atoll(row["rtpsize"].c_str())) ||
				   (typeSize["graph"] && !atoll(row["graphsize"].c_str())) ||
				   (typeSize["audio"] && !atoll(row["audiosize"].c_str()))) {
					needReindex = true;
				}
			} else {
				needReindex = true;
			}
			if(!needReindex &&
			   ((typeSize["sip"] && !file_exists(getSpoolDir_string(tsf_main) + "/filesindex/sipsize/" + ymdh)) ||
			    (typeSize["reg"] && !file_exists(getSpoolDir_string(tsf_main) + "/filesindex/regsize/" + ymdh)) ||
			    (typeSize["skinny"] && !file_exists(getSpoolDir_string(tsf_main) + "/filesindex/skinnysize/" + ymdh)) ||
			    (typeSize["rtp"] && !file_exists(getSpoolDir_string(tsf_main) + "/filesindex/rtpsize/" + ymdh)) ||
			    (typeSize["graph"] && !file_exists(getSpoolDir_string(tsf_main) + "/filesindex/graphsize/" + ymdh)) ||
			    (typeSize["audio"] && !file_exists(getSpoolDir_string(tsf_main) + "/filesindex/audiosize/" + ymdh)))) {
				needReindex = true;
			}
			if(needReindex) {
				reindex_date_hour(date, h);
			}
		}
	}
}

string CleanSpool::getMaxSpoolDate() {
	list<string> date_dirs;
	this->readSpoolDateDirs(&date_dirs);
	if(!date_dirs.size()) {
		return("");
	}
	u_int32_t maxDate = 0;
	for(list<string>::iterator iter_date_dir = date_dirs.begin(); iter_date_dir != date_dirs.end(); iter_date_dir++) {
		u_int32_t date = atol((*iter_date_dir).c_str()) * 10000 +
				 atol((*iter_date_dir).c_str() + 5) * 100 +
				 atol((*iter_date_dir).c_str() + 8);
		if(date > maxDate) {
			maxDate = date;
		}
	}
	if(maxDate) {
		char maxDate_str[20];
		sprintf(maxDate_str, "%4i-%02i-%02i", maxDate / 10000, maxDate % 10000 / 100, maxDate % 100);
		return(maxDate_str);
	} else {
		return("");
	}
}

void CleanSpool::run_cleanProcess(int spoolIndex) {
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex)) {
			cleanSpool[i]->cleanThreadProcess();
		}
	}
}

void CleanSpool::run_clean_obsolete(int spoolIndex) {
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex)) {
			cleanSpool[i]->clean_obsolete_dirs();
		}
	}
}

void CleanSpool::run_reindex_all(const char *reason, int spoolIndex) {
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex)) {
			cleanSpool[i]->reindex_all(reason);
		}
	}
}

void CleanSpool::run_reindex_date(string date, int spoolIndex) {
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex)) {
			cleanSpool[i]->reindex_date(date);
		}
	}
}

void CleanSpool::run_reindex_date_hour(string date, int hour, int spoolIndex) {
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex)) {
			cleanSpool[i]->reindex_date_hour(date, hour);
		}
	}
}

bool CleanSpool::suspend(int spoolIndex) {
	bool changeState = false;
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex) &&
		   !cleanSpool[i]->suspended) {
			cleanSpool[i]->suspended = true;
			changeState = true;
		}
	}
	return(changeState);
}

bool CleanSpool::resume(int spoolIndex) {
	bool changeState = false;
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex) &&
		   cleanSpool[i]->suspended) {
			cleanSpool[i]->suspended = false;
			changeState = true;
		}
	}
	return(changeState);
}

void CleanSpool::run_check_filesindex(int spoolIndex) {
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex)) {
			cleanSpool[i]->check_filesindex();
		}
	}
}

void CleanSpool::run_check_spooldir_filesindex(const char *dirfilter, int spoolIndex) {
	for(int i = 0; i < 2; i++) {
		if(cleanSpool[i] &&
		   (spoolIndex == -1 || spoolIndex == cleanSpool[i]->spoolIndex)) {
			cleanSpool[i]->check_spooldir_filesindex(dirfilter);
		}
	}
}

bool CleanSpool::isSetCleanspoolParameters(int spoolIndex) {
	extern unsigned int opt_maxpoolsize;
	extern unsigned int opt_maxpooldays;
	extern unsigned int opt_maxpoolsipsize;
	extern unsigned int opt_maxpoolsipdays;
	extern unsigned int opt_maxpoolrtpsize;
	extern unsigned int opt_maxpoolrtpdays;
	extern unsigned int opt_maxpoolgraphsize;
	extern unsigned int opt_maxpoolgraphdays;
	extern unsigned int opt_maxpoolaudiosize;
	extern unsigned int opt_maxpoolaudiodays;
	extern unsigned int opt_maxpoolsize_2;
	extern unsigned int opt_maxpooldays_2;
	extern unsigned int opt_maxpoolsipsize_2;
	extern unsigned int opt_maxpoolsipdays_2;
	extern unsigned int opt_maxpoolrtpsize_2;
	extern unsigned int opt_maxpoolrtpdays_2;
	extern unsigned int opt_maxpoolgraphsize_2;
	extern unsigned int opt_maxpoolgraphdays_2;
	extern unsigned int opt_maxpoolaudiosize_2;
	extern unsigned int opt_maxpoolaudiodays_2;
	extern int opt_cleanspool_interval;
	extern int opt_cleanspool_sizeMB;
	extern int opt_autocleanspoolminpercent;
	extern int opt_autocleanmingb;
	return((spoolIndex == 0 ?
		 opt_maxpoolsize ||
		 opt_maxpooldays ||
		 opt_maxpoolsipsize ||
		 opt_maxpoolsipdays ||
		 opt_maxpoolrtpsize ||
		 opt_maxpoolrtpdays ||
		 opt_maxpoolgraphsize ||
		 opt_maxpoolgraphdays ||
		 opt_maxpoolaudiosize ||
		 opt_maxpoolaudiodays :
		 opt_maxpoolsize_2 ||
		 opt_maxpooldays_2 ||
		 opt_maxpoolsipsize_2 ||
		 opt_maxpoolsipdays_2 ||
		 opt_maxpoolrtpsize_2 ||
		 opt_maxpoolrtpdays_2 ||
		 opt_maxpoolgraphsize_2 ||
		 opt_maxpoolgraphdays_2 ||
		 opt_maxpoolaudiosize_2 ||
		 opt_maxpoolaudiodays_2) ||
	       opt_cleanspool_interval ||
	       opt_cleanspool_sizeMB ||
	       opt_autocleanspoolminpercent ||
	       opt_autocleanmingb);
}

bool CleanSpool::isSetCleanspool(int spoolIndex) {
	return(cleanSpool[spoolIndex] != NULL);
}

bool CleanSpool::check_datehour(const char *datehour) {
	if(!datehour || strlen(datehour) != 10) {
		return(false);
	}
	u_int64_t datehour_i = atoll(datehour);
	return(datehour_i / 1000000 > 2000 &&
	       datehour_i / 10000 % 100 >= 1 && datehour_i / 10000 % 100 <= 12 && 
	       datehour_i / 100 % 100 >= 1 && datehour_i / 100 % 100 <= 31 && 
	       datehour_i % 100 < 60);
}

void CleanSpool::loadOpt() {
	extern char opt_spooldir_main[1024];
	extern char opt_spooldir_rtp[1024];
	extern char opt_spooldir_graph[1024];
	extern char opt_spooldir_audio[1024];
	extern char opt_spooldir_2_main[1024];
	extern char opt_spooldir_2_rtp[1024];
	extern char opt_spooldir_2_graph[1024];
	extern char opt_spooldir_2_audio[1024];
	extern unsigned int opt_maxpoolsize;
	extern unsigned int opt_maxpooldays;
	extern unsigned int opt_maxpoolsipsize;
	extern unsigned int opt_maxpoolsipdays;
	extern unsigned int opt_maxpoolrtpsize;
	extern unsigned int opt_maxpoolrtpdays;
	extern unsigned int opt_maxpoolgraphsize;
	extern unsigned int opt_maxpoolgraphdays;
	extern unsigned int opt_maxpoolaudiosize;
	extern unsigned int opt_maxpoolaudiodays;
	extern unsigned int opt_maxpoolsize_2;
	extern unsigned int opt_maxpooldays_2;
	extern unsigned int opt_maxpoolsipsize_2;
	extern unsigned int opt_maxpoolsipdays_2;
	extern unsigned int opt_maxpoolrtpsize_2;
	extern unsigned int opt_maxpoolrtpdays_2;
	extern unsigned int opt_maxpoolgraphsize_2;
	extern unsigned int opt_maxpoolgraphdays_2;
	extern unsigned int opt_maxpoolaudiosize_2;
	extern unsigned int opt_maxpoolaudiodays_2;
	extern int opt_maxpool_clean_obsolete;
	extern int opt_cleanspool_interval;
	extern int opt_cleanspool_sizeMB;
	extern int opt_autocleanspoolminpercent;
	extern int opt_autocleanmingb;
	extern int opt_cleanspool_enable_run_hour_from;
	extern int opt_cleanspool_enable_run_hour_to;
	opt_dirs.main = spoolIndex == 0 ? opt_spooldir_main : opt_spooldir_2_main;
	opt_dirs.rtp = spoolIndex == 0 ? opt_spooldir_rtp : opt_spooldir_2_rtp;
	opt_dirs.graph = spoolIndex == 0 ? opt_spooldir_graph : opt_spooldir_2_graph;
	opt_dirs.audio = spoolIndex == 0 ? opt_spooldir_audio : opt_spooldir_2_audio;
	opt_max.maxpoolsize = spoolIndex == 0 ? opt_maxpoolsize : opt_maxpoolsize_2;
	opt_max.maxpooldays = spoolIndex == 0 ? opt_maxpooldays : opt_maxpooldays_2;
	opt_max.maxpoolsipsize = spoolIndex == 0 ? opt_maxpoolsipsize : opt_maxpoolsipsize_2;
	opt_max.maxpoolsipdays = spoolIndex == 0 ? opt_maxpoolsipdays : opt_maxpoolsipdays_2;
	opt_max.maxpoolrtpsize = spoolIndex == 0 ? opt_maxpoolrtpsize : opt_maxpoolrtpsize_2;
	opt_max.maxpoolrtpdays = spoolIndex == 0 ? opt_maxpoolrtpdays : opt_maxpoolrtpdays_2;
	opt_max.maxpoolgraphsize = spoolIndex == 0 ? opt_maxpoolgraphsize : opt_maxpoolgraphsize_2;
	opt_max.maxpoolgraphdays = spoolIndex == 0 ? opt_maxpoolgraphdays : opt_maxpoolgraphdays_2;
	opt_max.maxpoolaudiosize = spoolIndex == 0 ? opt_maxpoolaudiosize : opt_maxpoolaudiosize_2;
	opt_max.maxpoolaudiodays = spoolIndex == 0 ? opt_maxpoolaudiodays : opt_maxpoolaudiodays_2;
	opt_other.maxpool_clean_obsolete = opt_maxpool_clean_obsolete;
	opt_other.cleanspool_interval = opt_cleanspool_interval;
	opt_other.cleanspool_sizeMB = opt_cleanspool_sizeMB;
	opt_other.autocleanspoolminpercent = opt_autocleanspoolminpercent;
	opt_other.autocleanmingb = opt_autocleanmingb;
	opt_other.cleanspool_enable_run_hour_from = opt_cleanspool_enable_run_hour_from;
	opt_other.cleanspool_enable_run_hour_to = opt_cleanspool_enable_run_hour_to;
}

void CleanSpool::runCleanThread() {
	if(!clean_thread) {
		if(sverb.cleanspool) { 
			syslog(LOG_NOTICE, "cleanspool[%i]: pthread_create - cleanThread", spoolIndex);
		}
		vm_pthread_create("cleanspool",
				  &clean_thread, NULL, cleanThread, this, __FILE__, __LINE__);
	}
}

void CleanSpool::termCleanThread() {
	if(clean_thread) {
		pthread_join(clean_thread, NULL);
		clean_thread = 0;
	}
}

void *CleanSpool::cleanThread(void *cleanSpool) {
	((CleanSpool*)cleanSpool)->cleanThread();
	return(NULL);
}

void CleanSpool::cleanThread() {
	if(sverb.cleanspool) {
		syslog(LOG_NOTICE, "cleanspool[%i]: run cleanThread", spoolIndex);
	}
	while(!is_terminating()) {
		cleanThreadProcess();
		for(int i = 0; i < 2; i++) {
			if(cleanSpool[i] &&
			   cleanSpool[i]->spoolIndex != this->spoolIndex &&
			   !cleanSpool[i]->clean_thread) {
				cleanSpool[i]->cleanThreadProcess();
			}
		}
		for(int i = 0; i < 300 && !is_terminating() && !do_convert_filesindex_flag; i++) {
			sleep(1);
		}
	}
}

void CleanSpool::cleanThreadProcess() {
	if(do_convert_filesindex_flag ||
	   !check_exists_act_records_in_files() ||
	   !check_exists_act_files_in_filesindex()) {
		const char *reason = do_convert_filesindex_flag ? 
				      (do_convert_filesindex_reason ? do_convert_filesindex_reason : "set do_convert_filesindex_flag") :
				      "call from clean_spooldir - not exists act records in files and act files in filesindex";
		do_convert_filesindex_flag = false;
		do_convert_filesindex_reason = NULL;
		reindex_all(reason);
	}
	bool timeOk = false;
	if(opt_other.cleanspool_enable_run_hour_from >= 0 &&
	   opt_other.cleanspool_enable_run_hour_to >= 0) {
		time_t now;
		time(&now);
		struct tm dateTime = time_r(&now);
		if(opt_other.cleanspool_enable_run_hour_to >= opt_other.cleanspool_enable_run_hour_from) {
			if(dateTime.tm_hour >= opt_other.cleanspool_enable_run_hour_from &&
			   dateTime.tm_hour <= opt_other.cleanspool_enable_run_hour_to) {
				timeOk = true;
			}
		} else {
			if((dateTime.tm_hour >= opt_other.cleanspool_enable_run_hour_from && dateTime.tm_hour < 24) ||
			   dateTime.tm_hour <= opt_other.cleanspool_enable_run_hour_to) {
				timeOk = true;
			}
		}
	} else {
		timeOk = true;
	}
	bool criticalLowSpace = false;
	long int maxpoolsize = 0;
	if((opt_other.autocleanspoolminpercent || opt_other.autocleanmingb) &&
	   (!opt_dirs.rtp.length() || opt_dirs.rtp == opt_dirs.main) && 
	   (!opt_dirs.graph.length() || opt_dirs.graph == opt_dirs.main) && 
	   (!opt_dirs.audio.length() || opt_dirs.audio == opt_dirs.main)) {
		double totalSpaceGB = (double)GetTotalDiskSpace(getSpoolDir(tsf_main)) / (1024 * 1024 * 1024);
		double freeSpacePercent = (double)GetFreeDiskSpace(getSpoolDir(tsf_main), true) / 100;
		double freeSpaceGB = (double)GetFreeDiskSpace(getSpoolDir(tsf_main)) / (1024 * 1024 * 1024);
		int _minPercentForAutoReindex = 1;
		int _minGbForAutoReindex = 5;
		if(freeSpacePercent < _minPercentForAutoReindex && 
		   freeSpaceGB < _minGbForAutoReindex) {
			syslog(LOG_NOTICE, "cleanspool[%i]: low spool disk space - executing convert_filesindex", spoolIndex);
			reindex_all("call from clean_spooldir - low spool disk space");
			freeSpacePercent = (double)GetFreeDiskSpace(getSpoolDir(tsf_main), true) / 100;
			freeSpaceGB = (double)GetFreeDiskSpace(getSpoolDir(tsf_main)) / (1024 * 1024 * 1024);
			criticalLowSpace = true;
		}
		if(freeSpacePercent < opt_other.autocleanspoolminpercent ||
		   freeSpaceGB < opt_other.autocleanmingb) {
			SqlDb *sqlDb = createSqlObject();
			sqlDb->query(
			       "SELECT SUM(coalesce(sipsize,0) + \
					   coalesce(regsize,0) + \
					   coalesce(skinnysize,0) + \
					   coalesce(rtpsize,0) + \
					   coalesce(graphsize,0) + \
					   coalesce(audiosize,0)) as sum_size \
				FROM files \
				WHERE spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string());
			SqlDb_row row = sqlDb->fetchRow();
			if(row) {
				double usedSizeGB = atol(row["sum_size"].c_str()) / (1024 * 1024 * 1024);
				maxpoolsize = (usedSizeGB + freeSpaceGB - min(totalSpaceGB * opt_other.autocleanspoolminpercent / 100, (double)opt_other.autocleanmingb)) * 1024;
				if(maxpoolsize > 1000 &&
				   (!opt_max.maxpoolsize || maxpoolsize < opt_max.maxpoolsize)) {
					if(opt_max.maxpoolsize && maxpoolsize < opt_max.maxpoolsize * 0.8) {
						maxpoolsize = opt_max.maxpoolsize * 0.8;
					}
					syslog(LOG_NOTICE, "cleanspool[%i]: %s: %li MB", 
					       spoolIndex,
					       opt_max.maxpoolsize ?
						"low spool disk space - maxpoolsize set to new value" :
						"maxpoolsize set to value",
					       maxpoolsize);
				} else {
					syslog(LOG_ERR, "cleanspool[%i]: incorrect set autocleanspoolminpercent and autocleanspoolmingb", spoolIndex);
					maxpoolsize = 0;
				}
			}
			delete sqlDb;
		}
	}
	if((timeOk && !suspended) || criticalLowSpace) {
		if(sverb.cleanspool) {
			syslog(LOG_NOTICE, "cleanspool[%i]: run clean_spooldir", spoolIndex);
		}
		if(maxpoolsize > 1000) {
			maxpoolsize_set = maxpoolsize;
		}
		critical_low_space = criticalLowSpace;
		clean_spooldir_run();
		maxpoolsize_set = 0;
		critical_low_space = false;
	}
}

bool CleanSpool::check_exists_act_records_in_files() {
	bool ok = false;
	if(!sqlDb) {
		sqlDb = createSqlObject();
	}
	sqlDb->query("select max(calldate) as max_calldate from cdr where calldate > date_add(now(), interval -1 day)");
	SqlDb_row row = sqlDb->fetchRow();
	if(!row || !row["max_calldate"].length()) {
		return(true);
	}
	time_t maxCdrTime = stringToTime(row["max_calldate"].c_str());
	for(int i = 0; i < 12; i++) {
		time_t checkTime = maxCdrTime - i * 60 * 60;
		struct tm checkTimeInfo = time_r(&checkTime);
		char datehour[20];
		strftime(datehour, 20, "%Y%m%d%H", &checkTimeInfo);
		sqlDb->query(
		       "select * \
			from files \
			where datehour ='" + string(datehour) + "' and \
			      spool_index = " + getSpoolIndex_string() + " and \
			      id_sensor = " + getIdSensor_string());
		if(sqlDb->fetchRow()) {
			ok = true;
			break;
		}
	}
	return(ok);
}

bool CleanSpool::check_exists_act_files_in_filesindex() {
	bool ok = false;
	if(!sqlDb) {
		sqlDb = createSqlObject();
	}
	sqlDb->query("select max(calldate) as max_calldate from cdr where calldate > date_add(now(), interval -1 day)");
	SqlDb_row row = sqlDb->fetchRow();
	if(!row || !row["max_calldate"].length()) {
		return(true);
	}
	time_t maxCdrTime = stringToTime(row["max_calldate"].c_str());
	for(int i = 0; i < 12; i++) {
		time_t checkTime = maxCdrTime - i * 60 * 60;
		struct tm checkTimeInfo = time_r(&checkTime);
		char date[20];
		strftime(date, 20, "%Y%m%d", &checkTimeInfo);
		for(int j = 0; j < 24; j++) {
			char datehour[20];
			strcpy(datehour, date);
			sprintf(datehour + strlen(datehour), "%02i", j);
			if(FileExists((char*)(getSpoolDir_string(tsf_main) + "/filesindex/sipsize/" + datehour).c_str())) {
				ok = true;
				break;
			}
		}
		if(ok) {
			break;
		}
	}
	return(ok);
}

void CleanSpool::reindex_all(const char *reason) {
	u_long actTime = getTimeS();
	if(actTime - lastCall_reindex_all < 5 * 60) {
		syslog(LOG_NOTICE,"cleanspool[%i]: suppress run reindex_all - last run before %lus", spoolIndex, actTime - lastCall_reindex_all);
		return;
	}
	lastCall_reindex_all = actTime;
	list<string> date_dirs;
	this->readSpoolDateDirs(&date_dirs);
	if(!date_dirs.size()) {
		return;
	}
	syslog(LOG_NOTICE, "cleanspool[%i]: reindex_all start%s%s", spoolIndex, reason ? " - " : "", reason ? reason : "");
	sqlStore->query_lock(
	       "DELETE FROM files \
		WHERE spool_index = " + getSpoolIndex_string() + " and \
		      id_sensor = " + getIdSensor_string(),
		STORE_PROC_ID_CLEANSPOOL_SERVICE + spoolIndex);
	rmdir_r(getSpoolDir_string(tsf_main) + "/filesindex", true, true);
	for(list<string>::iterator iter_date_dir = date_dirs.begin(); iter_date_dir != date_dirs.end(); iter_date_dir++) {
		reindex_date(*iter_date_dir);
	}
	syslog(LOG_NOTICE, "cleanspool[%i]: reindex_all done", spoolIndex);
	// wait for flush sql store
	while(sqlStore->getSize(STORE_PROC_ID_CLEANSPOOL_SERVICE + spoolIndex) > 0) {
		usleep(100000);
	}
	sleep(1);
}

long long CleanSpool::reindex_date(string date) {
	long long sumDaySize = 0;
	for(int h = 0; h < 24 && !is_terminating(); h++) {
		sumDaySize += reindex_date_hour(date, h);
	}
	if(!sumDaySize && !is_terminating()) {
		rmdir(date.c_str());
	}
	return(sumDaySize);
}

long long CleanSpool::reindex_date_hour(string date, int h, bool readOnly, map<string, long long> *typeSize, bool quickCheck) {
	char hour[3];
	snprintf(hour, 3, "%02d", h);
	if(typeSize) {
		(*typeSize)["sip"] = 0;
		(*typeSize)["reg"] = 0;
		(*typeSize)["skinny"] = 0;
		(*typeSize)["rtp"] = 0;
		(*typeSize)["graph"] = 0;
		(*typeSize)["audio"] = 0;
	}
	map<unsigned, bool> fillMinutes;
	long long sipsize = reindex_date_hour_type(date, h, "sip", readOnly, quickCheck, &fillMinutes);
	long long regsize = reindex_date_hour_type(date, h, "reg", readOnly, quickCheck, &fillMinutes);
	long long skinnysize = reindex_date_hour_type(date, h, "skinny", readOnly, quickCheck, &fillMinutes);
	long long rtpsize = reindex_date_hour_type(date, h, "rtp", readOnly, quickCheck, &fillMinutes);
	long long graphsize = reindex_date_hour_type(date, h, "graph", readOnly, quickCheck, &fillMinutes);
	long long audiosize = reindex_date_hour_type(date, h, "audio", readOnly, quickCheck, &fillMinutes);
	if((sipsize + regsize + skinnysize + rtpsize + graphsize + audiosize) && !readOnly) {
		string dh = date + '/' + hour;
		syslog(LOG_NOTICE, "cleanspool[%i]: reindex_date_hour - %s/%s", spoolIndex, getSpoolDir(tsf_main), dh.c_str());
	}
	if(!readOnly) {
		for(eTypeSpoolFile typeSpoolFile = tsf_sip; typeSpoolFile < tsf_all; typeSpoolFile = (eTypeSpoolFile)((int)typeSpoolFile + 1)) {
			for(unsigned m = 0; m < 60; m++) {
				char min[3];
				snprintf(min, 3, "%02d", m);
				string dhm = getSpoolDir_string(typeSpoolFile) + '/' + date + '/' + hour + '/' + min;
				if(!fillMinutes[m]) {
					rmdir_r(dhm);
				}
			}
		}
		string ymdh = string(date.substr(0,4)) + date.substr(5,2) + date.substr(8,2) + hour;
		if(sipsize + regsize + skinnysize + rtpsize + graphsize + audiosize) {
			sqlStore->query_lock(
			       "INSERT INTO files \
				SET datehour = " + ymdh + ", \
				    spool_index = " + getSpoolIndex_string() + ", \
				    id_sensor = " + getIdSensor_string() + ", \
				    sipsize = " + intToString(sipsize) + ", \
				    regsize = " + intToString(regsize) + ", \
				    skinnysize = " + intToString(skinnysize) + ", \
				    rtpsize = " + intToString(rtpsize) + ", \
				    graphsize = " + intToString(graphsize) + ", \
				    audiosize = " + intToString(audiosize) + " \
				ON DUPLICATE KEY UPDATE \
				    sipsize = " + intToString(sipsize) + ", \
				    regsize = " + intToString(regsize) + ", \
				    skinnysize = " + intToString(skinnysize) + ", \
				    rtpsize = " + intToString(rtpsize) + ", \
				    graphsize = " + intToString(graphsize) + ", \
				    audiosize = " + intToString(audiosize),
				STORE_PROC_ID_CLEANSPOOL_SERVICE + spoolIndex);
		} else {
			sqlStore->query_lock(
			       "DELETE FROM files \
				WHERE datehour = " + ymdh + " and \
				      spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string(),
				STORE_PROC_ID_CLEANSPOOL_SERVICE + spoolIndex);
			for(eTypeSpoolFile typeSpoolFile = tsf_sip; typeSpoolFile < tsf_all; typeSpoolFile = (eTypeSpoolFile)((int)typeSpoolFile + 1)) {
				rmdir_r(getSpoolDir_string(typeSpoolFile) + '/' + date + '/' + hour);
			}
		}
	}
	if(typeSize) {
		(*typeSize)["sip"] = sipsize;
		(*typeSize)["reg"] = regsize;
		(*typeSize)["skinny"] = skinnysize;
		(*typeSize)["rtp"] = rtpsize;
		(*typeSize)["graph"] = graphsize;
		(*typeSize)["audio"] = audiosize;
	}
	return(sipsize + regsize + skinnysize + rtpsize + graphsize + audiosize);
}

long long CleanSpool::reindex_date_hour_type(string date, int h, string type, bool readOnly, bool quickCheck, map<unsigned, bool> *fillMinutes) {
	long long sumsize = 0;
	string filesIndexDirName;
	string spoolDirTypeName;
	string alterSpoolDirTypeName;
	eTypeSpoolFile typeSpoolFile = tsf_main;
	if(type == "sip") {
		filesIndexDirName = "sipsize";
		spoolDirTypeName = "SIP";
		alterSpoolDirTypeName = "ALL";
		typeSpoolFile = tsf_sip;
	} else if(type == "reg") {
		filesIndexDirName = "regsize";
		spoolDirTypeName = "REG";
		typeSpoolFile = tsf_reg;
	} else if(type == "skinny") {
		filesIndexDirName = "skinnysize";
		spoolDirTypeName = "SKINNY";
		typeSpoolFile = tsf_skinny;
	} else if(type == "rtp") {
		filesIndexDirName = "rtpsize";
		spoolDirTypeName = "RTP";
		typeSpoolFile = tsf_rtp;
	} else if(type == "graph") {
		filesIndexDirName = "graphsize";
		spoolDirTypeName = "GRAPH";
		typeSpoolFile = tsf_graph;
	} else if(type == "audio") {
		filesIndexDirName = "audiosize";
		spoolDirTypeName = "AUDIO";
		typeSpoolFile = tsf_audio;
	}
	char hour[3];
	snprintf(hour, 3, "%02d", h);
	string spool_fileindex_path = getSpoolDir_string(tsf_main) + "/filesindex/" + filesIndexDirName;
	mkdir_r(spool_fileindex_path, 0777);
	string ymdh = string(date.substr(0,4)) + date.substr(5,2) + date.substr(8,2) + hour;
	string spool_fileindex = spool_fileindex_path + '/' + ymdh;
	ofstream *spool_fileindex_stream = NULL;
	if(!readOnly) {
		spool_fileindex_stream = new FILE_LINE(2001) ofstream(spool_fileindex.c_str(), ios::trunc | ios::out);
	}
	extern TarQueue *tarQueue[2];
	list<string> listOpenTars;
	if(tarQueue[spoolIndex]) {
		listOpenTars = tarQueue[spoolIndex]->listOpenTars();
	}
	for(unsigned m = 0; m < 60; m++) {
		char min[3];
		snprintf(min, 3, "%02d", m);
		string dhmt = date + '/' + hour + '/' + min + '/' + spoolDirTypeName;
		string spool_dhmt = this->findExistsSpoolDirFile(typeSpoolFile, dhmt);
		bool exists_spool_dhmt = file_exists(spool_dhmt);
		if(!exists_spool_dhmt && !alterSpoolDirTypeName.empty()) {
			dhmt = date + '/' + hour + '/' + min + '/' + alterSpoolDirTypeName;
			spool_dhmt = this->findExistsSpoolDirFile(typeSpoolFile, dhmt);
			exists_spool_dhmt = file_exists(spool_dhmt);
		}
		if(exists_spool_dhmt) {
			bool existsFile = false;
			DIR* dp = opendir(spool_dhmt.c_str());
			if(dp) {
				while(true) {
					dirent *de = readdir(dp);
					if(de == NULL) break;
					if(string(de->d_name) == ".." or string(de->d_name) == ".") continue;
					existsFile = true;
					if(quickCheck) {
						sumsize = 1;
						break;
					}
					string dhmt_file = dhmt + '/' + de->d_name;
					string spool_dhmt_file = spool_dhmt + '/' + de->d_name;
					if(!tarQueue[spoolIndex] ||
					   !fileIsOpenTar(listOpenTars, spool_dhmt_file)) {
						long long size = GetFileSizeDU(spool_dhmt_file, typeSpoolFile, spoolIndex);
						if(size == 0) size = 1;
						sumsize += size;
						if(!readOnly) {
							(*spool_fileindex_stream) << dhmt_file << ":" << size << "\n";
						}
					}
				}
				closedir(dp);
			}
			if(existsFile) {
				(*fillMinutes)[m] = true;
				if(quickCheck) {
					break;
				}
			} else if(!readOnly) {
				rmdir_r(spool_dhmt);
			}
		}
	}
	if(!readOnly) {
		spool_fileindex_stream->close();
		delete spool_fileindex_stream;
		if(!sumsize) {
			unlink(spool_fileindex.c_str());
		}
	}
	return(sumsize);
}

void CleanSpool::unlinkfileslist(eTypeSpoolFile typeSpoolFile, string fname, string callFrom) {
	if(DISABLE_CLEANSPOOL) {
		return;
	}
	syslog(LOG_NOTICE, "cleanspool[%i]: call unlinkfileslist(%s) from %s", spoolIndex, fname.c_str(), callFrom.c_str());
	char buf[4092];
	FILE *fd = fopen((getSpoolDir_string(tsf_main) + '/' + fname).c_str(), "r");
	if(fd) {
		while(fgets(buf, 4092, fd) != NULL) {
			char *pos;
			if((pos = strchr(buf, '\n')) != NULL) {
				*pos = '\0';
			}
			char *posSizeSeparator;
			if((posSizeSeparator = strrchr(buf, ':')) != NULL) {
				bool isSize = true;
				pos = posSizeSeparator + 1;
				while(*pos) {
					if(*pos < '0' || *pos > '9') {
						isSize = false;
						break;
					}
					++pos;
				}
				if(isSize) {
					*posSizeSeparator = '\0';
				}
			}
			unlink(this->findExistsSpoolDirFile(typeSpoolFile, buf).c_str());
			if(DISABLE_CLEANSPOOL) {
				fclose(fd);
				return;
			}
		}
		fclose(fd);
		unlink((getSpoolDir_string(tsf_main) + '/' + fname).c_str());
	}
}

void CleanSpool::unlink_dirs(string datehour, int sip, int reg, int skinny, int rtp, int graph, int audio, string callFrom) {
	if(DISABLE_CLEANSPOOL || !check_datehour(datehour.c_str())) {
		return;
	}
	syslog(LOG_NOTICE, "cleanspool[%i]: call unlink_dirs(%s,%s,%s,%s,%s,%s,%s) from %s", 
	       spoolIndex,
	       datehour.c_str(), 
	       sip == 2 ? "SIP" : sip == 1 ? "sip" : "---",
	       reg == 2 ? "REG" : reg == 1 ? "reg" : "---",
	       skinny == 2 ? "SKINNY" : skinny == 1 ? "skinny" : "---",
	       rtp == 2 ? "RTP" : rtp == 1 ? "rtp" : "---",
	       graph == 2 ? "GRAPH" : graph == 1 ? "graph" : "---",
	       audio == 2 ? "AUDIO" : audio == 1 ? "audio" : "---",
	       callFrom.c_str());
	string d = datehour.substr(0,4) + "-" + datehour.substr(4,2) + "-" + datehour.substr(6,2);
	string dh =  d + '/' + datehour.substr(8,2);
	list<string> spool_dirs;
	this->getSpoolDirs(&spool_dirs);
	for(unsigned m = 0; m < 60 && !DISABLE_CLEANSPOOL; m++) {
		char min[3];
		snprintf(min, 3, "%02d", m);
		string dhm = dh + '/' + min;
		if(sip) {
			rmdir_if_r(this->findExistsSpoolDirFile(tsf_sip,'/' + dhm + "/SIP"),
				   sip == 2);
			rmdir_if_r(this->findExistsSpoolDirFile(tsf_sip,'/' + dhm + "/ALL"),
				   sip == 2);
		}
		if(reg) {
			rmdir_if_r(this->findExistsSpoolDirFile(tsf_reg,'/' + dhm + "/REG"),
				   reg == 2);
		}
		if(skinny) {
			rmdir_if_r(this->findExistsSpoolDirFile(tsf_skinny,'/' + dhm + "/SKINNY"),
				   skinny == 2);
		}
		if(rtp) {
			rmdir_if_r(this->findExistsSpoolDirFile(tsf_rtp, '/' + dhm + "/RTP"),
				   rtp == 2);
		}
		if(graph) {
			rmdir_if_r(this->findExistsSpoolDirFile(tsf_graph, '/' + dhm + "/GRAPH"),
				   graph == 2);
		}
		if(audio) {
			rmdir_if_r(this->findExistsSpoolDirFile(tsf_audio, '/' + dhm + "/AUDIO"),
				   audio == 2);
		}
		// remove minute
		for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
			if(rmdir((*iter_sd + '/' + dhm).c_str()) == 0) {
				syslog(LOG_NOTICE, "cleanspool[%i]: unlink_dirs: remove %s/%s", spoolIndex, (*iter_sd).c_str(), dhm.c_str());
			}
		}
	}
	// remove hour
	for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
		if(rmdir((*iter_sd + '/' + dh).c_str()) == 0) {
			syslog(LOG_NOTICE, "cleanspool[%i]: unlink_dirs: remove %s/%s", spoolIndex, (*iter_sd).c_str(), dh.c_str());
		}
	}
	// remove day
	for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
		if(rmdir((*iter_sd + '/' + d).c_str()) == 0) {
			syslog(LOG_NOTICE, "cleanspool[%i]: unlink_dirs: remove %s/%s", spoolIndex, (*iter_sd).c_str(), d.c_str());
		}
	}
}

void CleanSpool::clean_spooldir_run() {
	if(opt_other.cleanspool_interval && opt_other.cleanspool_sizeMB > 0) {
		opt_max.maxpoolsize = opt_other.cleanspool_sizeMB;
		// if old cleanspool interval is defined convert the config to new config 
		extern char configfile[1024];
		if(FileExists(configfile)) {
			syslog(LOG_NOTICE, "cleanspool[%i]: converting [%s] cleanspool_interval and cleanspool_size to maxpoolsize", spoolIndex, configfile);
			reindex_all("convert configuration");
			string tmpf = "/tmp/VM_pRjSYLAyx.conf";
			FILE *fdr = fopen(configfile, "r");
			FILE *fdw = fopen(tmpf.c_str(), "w");
			if(!fdr or !fdw) {
				syslog(LOG_ERR, "cleanspool[%i]: cannot open config file [%s]", spoolIndex, configfile);
				return;
			}
			char buffer[4092];
			while(!feof(fdr)) {
				if(fgets(buffer, 4092, fdr) != NULL) {
					if(memmem(buffer, strlen("cleanspool_interval"), "cleanspool_interval", strlen("cleanspool_interval")) == NULL) {
						if(memmem(buffer, strlen("cleanspool_size"), "cleanspool_size", strlen("cleanspool_size")) == NULL) {
							fwrite(buffer, 1, strlen(buffer), fdw);
						} else {
						}
					} else {
						stringstream tmp;
						tmp << "\n\n"
						    << "#this is new cleaning implementation\n"
						    << "maxpoolsize            = " << opt_other.cleanspool_sizeMB << "\n"
						    << "#maxpooldays            =\n"
						    << "#maxpoolsipsize         =\n"
						    << "#maxpoolsipdays         =\n"
						    << "#maxpoolrtpsize         =\n"
						    << "#maxpoolrtpdays         =\n"
						    << "#maxpoolgraphsize       =\n"
						    << "#maxpoolgraphdays       =\n";
						fwrite(tmp.str().c_str(), 1, tmp.str().length(), fdw);
					}
				}
			}
			fclose(fdr);
			fclose(fdw);
			move_file(tmpf.c_str(), configfile);

		}
	}
	
	clean_spooldir_run_processing = 1;

	clean_maxpoolsize_all();
	clean_maxpooldays_all();

	clean_maxpoolsize_sip();
	clean_maxpooldays_sip();

	clean_maxpoolsize_rtp();
	clean_maxpooldays_rtp();

	clean_maxpoolsize_graph();
	clean_maxpooldays_graph();

	clean_maxpoolsize_audio();
	clean_maxpooldays_audio();
	
	if(opt_other.maxpool_clean_obsolete) {
		clean_obsolete_dirs();
	}
	
	clean_spooldir_run_processing = 0;
}

void CleanSpool::clean_maxpoolsize(bool sip, bool rtp, bool graph, bool audio) {
	unsigned int maxpoolsize = sip && rtp && graph && audio ?
				    opt_max.maxpoolsize :
				   sip ?
				    opt_max.maxpoolsipsize :
				   rtp ?
				    opt_max.maxpoolrtpsize :
				   graph ?
				    opt_max.maxpoolgraphsize :
				   audio ?
				    opt_max.maxpoolaudiosize :
				    0;
	if(maxpoolsize == 0 && maxpoolsize_set == 0) {
		return;
	}
	if(sverb.cleanspool)  {
		cout << "clean_maxpoolsize\n";
	}
	if(!sqlDb) {
		sqlDb = createSqlObject();
	}
	while(!is_terminating() && !DISABLE_CLEANSPOOL) {
		sqlDb->query(
		       "SELECT SUM(sipsize) AS sipsize, \
			       SUM(regsize) AS regsize, \
			       SUM(skinnysize) AS skinnysize, \
			       SUM(rtpsize) AS rtpsize, \
			       SUM(graphsize) as graphsize, \
			       SUM(audiosize) AS audiosize \
			FROM files \
			WHERE spool_index = " + getSpoolIndex_string() + " and \
			      id_sensor = " + getIdSensor_string());
		SqlDb_row row = sqlDb->fetchRow();
		uint64_t sipsize_total = strtoull(row["sipsize"].c_str(), NULL, 0) + 
					 strtoull(row["regsize"].c_str(), NULL, 0) + 
					 strtoull(row["totalsize"].c_str(), NULL, 0);
		uint64_t rtpsize_total = strtoull(row["rtpsize"].c_str(), NULL, 0);
		uint64_t graphsize_total = strtoull(row["graphsize"].c_str(), NULL, 0);
		uint64_t audiosize_total = strtoull(row["audiosize"].c_str(), NULL, 0);
		double total = ((sip ? sipsize_total : 0) + 
				(rtp ? rtpsize_total : 0) + 
				(graph ? graphsize_total : 0) + 
				(audio ? audiosize_total : 0)) / (double)(1024 * 1024);
		if(sverb.cleanspool) {
			cout << "total[" << total << "] = " 
			     << (sip ? intToString(sipsize_total) : "na") << " + " 
			     << (rtp ? intToString(rtpsize_total) : "na") << " + " 
			     << (graph ? intToString(graphsize_total) : "na") << " + " 
			     << (audio ? intToString(audiosize_total) : "na")
			     << " maxpoolsize[" << maxpoolsize;
			if(maxpoolsize_set) {
				cout << " / reduk: " << maxpoolsize_set;
			}
			cout << "]\n";
		}
		unsigned int reduk_maxpoolsize = sip && rtp && graph && audio ? 
						  get_reduk_maxpoolsize(maxpoolsize) :
						  maxpoolsize;
		if(reduk_maxpoolsize == 0 ||
		   total <= reduk_maxpoolsize) {
			break;
		}
		// walk all rows ordered by datehour and delete everything 
		string sizeCond;
		if(!(sip && rtp && graph && audio)) {
			sizeCond = sip ? "(sipsize > 0 or regsize > 0 or skinnysize > 0)" :
				   rtp ? "rtpsize > 0" :
				   graph ? "graphsize > 0" :
					   "audiosize > 0";
			sizeCond = " and " + sizeCond;
		}
		sqlDb->query(
		       "SELECT * \
			FROM files \
			WHERE spool_index = " + getSpoolIndex_string() + " and \
			      id_sensor = " + getIdSensor_string() + " \
			      " + sizeCond + " \
			ORDER BY datehour LIMIT 1");
		row = sqlDb->fetchRow();
		if(!row) {
			break;
		}
		if(!check_datehour(row["datehour"].c_str())) {
			sqlDb->query(
			       "DELETE FROM files \
				WHERE datehour = " + row["datehour"] + " and \
				      spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string());
			continue;
		}
		uint64_t sipsize = strtoull(row["sipsize"].c_str(), NULL, 0) +
				   strtoull(row["regsize"].c_str(), NULL, 0) + 
				   strtoull(row["skinnysize"].c_str(), NULL, 0);
		uint64_t rtpsize = strtoull(row["rtpsize"].c_str(), NULL, 0);
		uint64_t graphsize = strtoull(row["graphsize"].c_str(), NULL, 0);
		uint64_t audiosize = strtoull(row["audiosize"].c_str(), NULL, 0);
		if(sip) {
			unlinkfileslist(tsf_sip, "filesindex/sipsize/" + row["datehour"], "clean_maxpoolsize");
			unlinkfileslist(tsf_reg, "filesindex/regsize/" + row["datehour"], "clean_maxpoolsize");
			unlinkfileslist(tsf_skinny, "filesindex/skinnysize/" + row["datehour"], "clean_maxpoolsize");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(rtp) {
			unlinkfileslist(tsf_rtp, "filesindex/rtpsize/" + row["datehour"], "clean_maxpoolsize");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(graph) {
			unlinkfileslist(tsf_graph, "filesindex/graphsize/" + row["datehour"], "clean_maxpoolsize");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(audio) {
			unlinkfileslist(tsf_audio, "filesindex/audiosize/" + row["datehour"], "clean_maxpoolsize");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(sip && rtp && graph && audio) {
			unlink_dirs(row["datehour"], 2, 2, 2, 2, 2, 2, "clean_maxpoolsize");
		} else {
			unlink_dirs(row["datehour"],
				    sip ? 2 : 1, 
				    sip ? 2 : 1, 
				    sip ? 2 : 1, 
				    rtp ? 2 : 1, 
				    graph ? 2 : 1, 
				    audio ? 2 : 1, 
				    "clean_maxpoolsize");
		}
		if((sip && rtp && graph && audio) ||
		   ((sip ? 0 : sipsize) + 
		    (rtp ? 0 : rtpsize) + 
		    (graph ? 0 : graphsize) +
		    (audio ? 0 : audiosize)) == 0) {
			sqlDb->query(
			       "DELETE FROM files \
				WHERE datehour = " + row["datehour"] + " and \
				      spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string());
		} else {
			string columnSetNul = sip ? "sipsize = 0, regsize = 0, skinnysize = 0" :
					      rtp ? "rtpsize = 0" :
					      graph ? "graphsize = 0" : "audiosize = 0";
			sqlDb->query(
			       "UPDATE files \
				SET " + columnSetNul + " \
				WHERE datehour = " + row["datehour"] + " and \
				      spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string());
		}
	}
}

void CleanSpool::clean_maxpooldays(bool sip, bool rtp, bool graph, bool audio) {
	unsigned int maxpooldays = sip && rtp && graph && audio ?
				    opt_max.maxpooldays :
				   sip ?
				    opt_max.maxpoolsipdays :
				   rtp ?
				    opt_max.maxpoolrtpdays :
				   graph ?
				    opt_max.maxpoolgraphdays :
				   audio ?
				    opt_max.maxpoolaudiodays :
				    0;
	if(maxpooldays == 0) {
		return;
	}
	if(sverb.cleanspool)  {
		cout << "clean_maxpooldays\n";
	}
	if(!sqlDb) {
		sqlDb = createSqlObject();
	}
	while(!is_terminating() && !DISABLE_CLEANSPOOL) {
		string sizeCond;
		if(!(sip && rtp && graph && audio)) {
			sizeCond = sip ? "(sipsize > 0 or regsize > 0 or skinnysize > 0)" :
				   rtp ? "rtpsize > 0" :
				   graph ? "graphsize > 0" :
					   "audiosize > 0";
			sizeCond = " and " + sizeCond;
		}
		sqlDb->query(
		       "SELECT * \
			FROM files \
			WHERE spool_index = " + getSpoolIndex_string() + " and \
			      id_sensor = " + getIdSensor_string() + " and \
			      datehour < DATE_FORMAT(DATE_SUB(NOW(), INTERVAL " + intToString(maxpooldays) + " DAY), '%Y%m%d%H') \
			      " + sizeCond + " \
			      ORDER BY datehour");
		SqlDb_row row = sqlDb->fetchRow();
		if(!row) {
			break;
		}
		uint64_t sipsize = strtoull(row["sipsize"].c_str(), NULL, 0) + 
				   strtoull(row["regsize"].c_str(), NULL, 0) + 
				   strtoull(row["skinnysize"].c_str(), NULL, 0);
		uint64_t rtpsize = strtoull(row["rtpsize"].c_str(), NULL, 0);
		uint64_t graphsize = strtoull(row["graphsize"].c_str(), NULL, 0);
		uint64_t audiosize = strtoull(row["audiosize"].c_str(), NULL, 0);
		if(sip) {
			unlinkfileslist(tsf_sip, "filesindex/sipsize/" + row["datehour"], "clean_maxpooldays");
			unlinkfileslist(tsf_sip, "filesindex/regsize/" + row["datehour"], "clean_maxpooldays");
			unlinkfileslist(tsf_sip, "filesindex/skinnysize/" + row["datehour"], "clean_maxpooldays");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(rtp) {
			unlinkfileslist(tsf_rtp, "filesindex/rtpsize/" + row["datehour"], "clean_maxpooldays");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(graph) {
			unlinkfileslist(tsf_graph, "filesindex/graphsize/" + row["datehour"], "clean_maxpooldays");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(audio) {
			unlinkfileslist(tsf_audio, "filesindex/audiosize/" + row["datehour"], "clean_maxpooldays");
			if(DISABLE_CLEANSPOOL) {
				break;
			}
		}
		if(sip && rtp && graph && audio) {
			unlink_dirs(row["datehour"], 2, 2, 2, 2, 2, 2, "clean_maxpooldays");
		} else {
			unlink_dirs(row["datehour"],
				    sip ? 2 : 1, 
				    sip ? 2 : 1, 
				    sip ? 2 : 1, 
				    rtp ? 2 : 1, 
				    graph ? 2 : 1, 
				    audio ? 2 : 1, 
				    "clean_maxpooldays");
		}
		if((sip && rtp && graph && audio) ||
		   ((sip ? 0 : sipsize) + 
		    (rtp ? 0 : rtpsize) + 
		    (graph ? 0 : graphsize) +
		    (audio ? 0 : audiosize)) == 0) {
			sqlDb->query(
			       "DELETE FROM files \
				WHERE datehour = " + row["datehour"] + " and \
				      spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string());
		} else {
			string columnSetNul = sip ? "sipsize = 0, regsize = 0, skinnysize = 0" :
					      rtp ? "rtpsize = 0" :
					      graph ? "graphsize = 0" : "audiosize = 0";
			sqlDb->query(
			       "UPDATE files \
				SET " + columnSetNul + " \
				WHERE datehour = " + row["datehour"] + " and \
				      spool_index = " + getSpoolIndex_string() + " and \
				      id_sensor = " + getIdSensor_string());
		}
	}
}

void CleanSpool::clean_obsolete_dirs() {
	unsigned int maxDays[10];
	unsigned int defaultMaxPolDays = opt_max.maxpooldays ? opt_max.maxpooldays : 14;
	maxDays[(int)tsf_sip] = opt_max.maxpoolsipdays ? opt_max.maxpoolsipdays : defaultMaxPolDays;
	maxDays[(int)tsf_reg] = opt_max.maxpoolsipdays ? opt_max.maxpoolsipdays : defaultMaxPolDays;
	maxDays[(int)tsf_skinny] = opt_max.maxpoolsipdays ? opt_max.maxpoolsipdays : defaultMaxPolDays;
	maxDays[(int)tsf_rtp] = opt_max.maxpoolrtpdays ? opt_max.maxpoolrtpdays : defaultMaxPolDays;
	maxDays[(int)tsf_graph] = opt_max.maxpoolgraphdays ? opt_max.maxpoolgraphdays : defaultMaxPolDays;
	maxDays[(int)tsf_audio] = opt_max.maxpoolaudiodays ? opt_max.maxpoolaudiodays : defaultMaxPolDays;
	
	list<string> spool_dirs;
	this->getSpoolDirs(&spool_dirs);
	list<string> date_dirs;
	this->readSpoolDateDirs(&date_dirs);
	if(!date_dirs.size()) {
		return;
	}
	if(!sqlDb) {
		sqlDb = createSqlObject();
	}
	for(list<string>::iterator iter_date_dir = date_dirs.begin(); iter_date_dir != date_dirs.end() && !is_terminating() && !DISABLE_CLEANSPOOL; iter_date_dir++) {
		string dateDir = *iter_date_dir;
		int numberOfDayToNow = getNumberOfDayToNow(dateDir.c_str());
		if(numberOfDayToNow > 0) {
			string day_sub_dir = '/' + dateDir;
			bool removeHourDir = false;
			for(int h = 0; h < 24; h++) {
				char hour[3];
				snprintf(hour, 3, "%02d", h);
				string hour_sub_dir = day_sub_dir + '/' + hour;
				bool existsHourDir = false;
				for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
					if(file_exists(*iter_sd + hour_sub_dir)) {
						existsHourDir = true;
					}
				}
				if(existsHourDir) {
					sqlDb->query(
					       "SELECT * \
						FROM files \
						where spool_index = " + getSpoolIndex_string() + " and \
						      id_sensor = " + getIdSensor_string() + " and \
						      datehour = '" + find_and_replace(dateDir.c_str(), "-", "") + hour + "'");
					SqlDb_row row = sqlDb->fetchRow();
					bool removeMinDir = false;
					for(int m = 0; m < 60; m++) {
						char min[3];
						snprintf(min, 3, "%02d", m);
						string min_sub_dir = hour_sub_dir + '/' + min;
						bool existsMinDir = false;
						for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
							if(file_exists(*iter_sd + min_sub_dir)) {
								existsMinDir = true;
							}
						}
						if(existsMinDir) {
							bool removeMinTypeDir = false;
							bool keepMainMinTypeFolder = false;
							for(eTypeSpoolFile typeSpoolFile = tsf_sip; typeSpoolFile < tsf_all; typeSpoolFile = (eTypeSpoolFile)((int)typeSpoolFile + 1)) {
								string mintype_sub_dir = min_sub_dir + '/' + getSpoolTypeDir(typeSpoolFile);
								string mintype_dir = getSpoolDir_string(typeSpoolFile) + '/' + mintype_sub_dir;
								if(file_exists(mintype_dir)) {
									if(row ?
									    !atoi(row[string(getSpoolTypeFilesIndex(typeSpoolFile, false)) + "size"].c_str()) :
									    (unsigned int)numberOfDayToNow > maxDays[(int)typeSpoolFile]) {
										rmdir_r(mintype_dir);
										syslog(LOG_NOTICE, "cleanspool[%i]: clean obsolete dir %s", spoolIndex, mintype_dir.c_str());
										removeMinTypeDir = true;
									} else {
										keepMainMinTypeFolder = true;
									}
								}
							}
							if(!keepMainMinTypeFolder) {
								for(eTypeSpoolFile typeSpoolFile = tsf_sip; typeSpoolFile < tsf_all; typeSpoolFile = (eTypeSpoolFile)((int)typeSpoolFile + 1)) {
									string mintype_sub_dir = min_sub_dir + '/' + getSpoolTypeDir(typeSpoolFile);
									string mintype_dir = getSpoolDir_string(typeSpoolFile) + '/' + mintype_sub_dir;
									if(file_exists(mintype_dir)) {
										rmdir_r(mintype_dir);
										syslog(LOG_NOTICE, "cleanspool[%i]: clean obsolete dir %s", spoolIndex, mintype_dir.c_str());
										removeMinTypeDir = true;
									}
								}
							}
							if(removeMinTypeDir) {
								removeMinDir = true;
								for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
									string remove_dir = *iter_sd + '/' + min_sub_dir;
									if(file_exists(remove_dir)) {
										if(rmdir(remove_dir.c_str()) == 0) {
											syslog(LOG_NOTICE, "cleanspool[%i]: clean obsolete dir %s", spoolIndex, remove_dir.c_str());
										} else {
											removeMinDir = false;
										}
									}
								}
							}
						}
					}
					if(removeMinDir) {
						removeHourDir = true;
						for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
							string remove_dir = *iter_sd + '/' + hour_sub_dir;
							if(file_exists(remove_dir)) {
								if(rmdir(remove_dir.c_str()) == 0) {
									syslog(LOG_NOTICE, "cleanspool[%i]: clean obsolete dir %s", spoolIndex, remove_dir.c_str());
								} else {
									removeHourDir = false;
								}
							}
						}
					}
				}
			}
			if(removeHourDir) {
				for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
					string remove_dir = *iter_sd + '/' + day_sub_dir;
					if(file_exists(remove_dir)) {
						if(rmdir(remove_dir.c_str()) == 0) {
							syslog(LOG_NOTICE, "cleanspool[%i]: clean obsolete dir %s", spoolIndex, remove_dir.c_str());
						}
					}
				}
			}
		}
	}
}

void CleanSpool::check_spooldir_filesindex(const char *dirfilter) {
	list<string> spool_dirs;
	this->getSpoolDirs(&spool_dirs);
	list<string> date_dirs;
	this->readSpoolDateDirs(&date_dirs);
	if(!date_dirs.size()) {
		return;
	}
	if(!sqlDb) {
		sqlDb = createSqlObject();
	}
	for(list<string>::iterator iter_date_dir = date_dirs.begin(); iter_date_dir != date_dirs.end() && !is_terminating() && !DISABLE_CLEANSPOOL; iter_date_dir++) {
		string dateDir = *iter_date_dir;
		if((!dirfilter || strstr(dateDir.c_str(), dirfilter))) {
			syslog(LOG_NOTICE, "cleanspool[%i]: check files in %s", spoolIndex, dateDir.c_str());
			for(int h = 0; h < 24; h++) {
				long long sumSizeMissingFilesInIndex[2] = {0, 0};
				char hour[8];
				sprintf(hour, "%02d", h);
				syslog(LOG_NOTICE, "cleanspool[%i]: - hour %s", spoolIndex, hour);
				string ymd = dateDir;
				string ymdh = string(ymd.substr(0,4)) + ymd.substr(5,2) + ymd.substr(8,2) + hour;
				long long sumSize[2][10];
				for(eTypeSpoolFile typeSpoolFile = tsf_sip; typeSpoolFile < tsf_all; typeSpoolFile = (eTypeSpoolFile)((int)typeSpoolFile + 1)) {
					vector<string> filesInIndex;
					sumSize[0][(int)typeSpoolFile] = 0;
					sumSize[1][(int)typeSpoolFile] = 0;
					if(getSpoolTypeFilesIndex(typeSpoolFile, false)) {
						FILE *fd = fopen((getSpoolDir_string(tsf_main) + "/filesindex/" + getSpoolTypeFilesIndex(typeSpoolFile, false) + "size/" + ymdh).c_str(), "r");
						if(fd) {
							char buf[4092];
							while(fgets(buf, 4092, fd) != NULL) {
								char *pos;
								if((pos = strchr(buf, '\n')) != NULL) {
									*pos = '\0';
								}
								char *posSizeSeparator;
								if((posSizeSeparator = strrchr(buf, ':')) != NULL) {
									bool isSize = true;
									pos = posSizeSeparator + 1;
									while(*pos) {
										if(*pos < '0' || *pos > '9') {
											isSize = false;
											break;
										}
										++pos;
									}
									if(isSize) {
										*posSizeSeparator = '\0';
									} else {
										posSizeSeparator = NULL;
									}
								}
								string file = buf;
								filesInIndex.push_back(file);
								long long unsigned size = posSizeSeparator ? atoll(posSizeSeparator + 1) : 0;
								eTypeSpoolFile rsltTypeSpoolFile;
								long long unsigned fileSize = GetFileSizeDU(this->findExistsSpoolDirFile(typeSpoolFile, file, &rsltTypeSpoolFile), rsltTypeSpoolFile, spoolIndex);
								if(fileSize == 0) {
									fileSize = 1;
								}
								sumSize[0][(int)typeSpoolFile] += size;
								sumSize[1][(int)typeSpoolFile] += fileSize;
								if(fileSize == (long long unsigned)-1) {
									syslog(LOG_NOTICE, "cleanspool[%i]: ERROR - missing file from index %s", spoolIndex, file.c_str());
								} else {
									if(size != fileSize) {
										syslog(LOG_NOTICE, "cleanspool[%i]: ERROR - diff file size [%s - %llu i / %llu r]", spoolIndex, file.c_str(), size, fileSize);
									}
								}
							}
							fclose(fd);
						}
					}
					if(filesInIndex.size()) {
						std::sort(filesInIndex.begin(), filesInIndex.end());
					}
					vector<string> filesInFolder;
					for(int m = 0; m < 60; m++) {
						char min[8];
						sprintf(min, "%02d", m);
						string timetypedir = dateDir + '/' + hour + '/' + min + '/' + getSpoolTypeDir(typeSpoolFile);
						DIR* dp = opendir(this->findExistsSpoolDirFile(typeSpoolFile, timetypedir).c_str());
						if(!dp) {
							continue;
						}
						dirent* de2;
						while((de2 = readdir(dp)) != NULL) {
							if(de2->d_type != 4 && string(de2->d_name) != ".." && string(de2->d_name) != ".") {
								filesInFolder.push_back(timetypedir + '/' + de2->d_name);
							}
						}
						closedir(dp);
					}
					for(uint j = 0; j < filesInFolder.size(); j++) {
						if(!std::binary_search(filesInIndex.begin(), filesInIndex.end(), filesInFolder[j])) {
							long long size = GetFileSize(getSpoolDir_string(typeSpoolFile) + '/' + filesInFolder[j]);
							long long sizeDU = GetFileSizeDU(getSpoolDir_string(typeSpoolFile) + '/' + filesInFolder[j], typeSpoolFile, spoolIndex);
							sumSizeMissingFilesInIndex[0] += size;
							sumSizeMissingFilesInIndex[1] += sizeDU;
							syslog(LOG_NOTICE, "cleanspool[%i]: ERROR - %s %s - %llu / %llu",
							       spoolIndex,
							       "missing file in index", 
							       filesInFolder[j].c_str(),
							       size,
							       sizeDU);
						}
					}
				}
				if(sumSize[0][(int)tsf_sip] || sumSize[0][(int)tsf_reg] || sumSize[0][(int)tsf_skinny] ||
				   sumSize[0][(int)tsf_rtp] || sumSize[0][(int)tsf_graph] || sumSize[0][(int)tsf_audio] ||
				   sumSize[1][(int)tsf_sip] || sumSize[1][(int)tsf_reg] || sumSize[1][(int)tsf_skinny] ||
				   sumSize[1][(int)tsf_rtp] || sumSize[1][(int)tsf_graph] || sumSize[1][(int)tsf_audio]) {
					sqlDb->query(
					       "SELECT SUM(sipsize) AS sipsize,\
						       SUM(regsize) AS regsize,\
						       SUM(skinnysize) AS skinnysize,\
						       SUM(rtpsize) AS rtpsize,\
						       SUM(graphsize) AS graphsize,\
						       SUM(audiosize) AS audiosize,\
						       count(*) as cnt\
						FROM files\
						WHERE datehour like '" + dateDir.substr(0, 4) + 
									 dateDir.substr(5, 2) + 
									 dateDir.substr(8, 2) + hour + "%' and \
						      spool_index = " + getSpoolIndex_string() + " and \
						      id_sensor = " + getIdSensor_string());
					SqlDb_row rowSum = sqlDb->fetchRow();
					if(rowSum && atol(rowSum["cnt"].c_str()) > 0) {
						if(atoll(rowSum["sipsize"].c_str()) == sumSize[0][(int)tsf_sip] &&
						   atoll(rowSum["regsize"].c_str()) == sumSize[0][(int)tsf_reg] &&
						   atoll(rowSum["skinnysize"].c_str()) == sumSize[0][(int)tsf_skinny] &&
						   atoll(rowSum["rtpsize"].c_str()) == sumSize[0][(int)tsf_rtp] &&
						   atoll(rowSum["graphsize"].c_str()) == sumSize[0][(int)tsf_graph] &&
						   atoll(rowSum["audiosize"].c_str()) == sumSize[0][(int)tsf_audio] &&
						   atoll(rowSum["sipsize"].c_str()) == sumSize[1][(int)tsf_sip] &&
						   atoll(rowSum["regsize"].c_str()) == sumSize[1][(int)tsf_reg] &&
						   atoll(rowSum["skinnysize"].c_str()) == sumSize[1][(int)tsf_skinny] &&
						   atoll(rowSum["rtpsize"].c_str()) == sumSize[1][(int)tsf_rtp] &&
						   atoll(rowSum["graphsize"].c_str()) == sumSize[1][(int)tsf_graph] &&
						   atoll(rowSum["audiosize"].c_str()) == sumSize[1][(int)tsf_audio]) {
							syslog(LOG_NOTICE, "cleanspool[%i]: # OK sum in files by index", spoolIndex);
						} else {
							if(atoll(rowSum["sipsize"].c_str()) != sumSize[0][(int)tsf_sip]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum sipsize in files [ %llu ii / %llu f ]", spoolIndex, sumSize[0][(int)tsf_sip], atoll(rowSum["sipsize"].c_str()));
							}
							if(atoll(rowSum["sipsize"].c_str()) != sumSize[1][(int)tsf_sip]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum sipsize in files [ %llu ri / %llu f ]", spoolIndex, sumSize[1][(int)tsf_sip], atoll(rowSum["sipsize"].c_str()));
							}
							if(atoll(rowSum["regsize"].c_str()) != sumSize[0][(int)tsf_reg]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum regsize in files [ %llu ii / %llu f ]", spoolIndex, sumSize[0][(int)tsf_reg], atoll(rowSum["regsize"].c_str()));
							}
							if(atoll(rowSum["skinnysize"].c_str()) != sumSize[1][(int)tsf_skinny]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum skinnysize in files [ %llu ri / %llu f ]", spoolIndex, sumSize[1][(int)tsf_skinny], atoll(rowSum["skinnysize"].c_str()));
							}
							if(atoll(rowSum["rtpsize"].c_str()) != sumSize[0][(int)tsf_rtp]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum rtpsize in files [ %llu ii / %llu f ]", spoolIndex, sumSize[0][(int)tsf_rtp], atoll(rowSum["rtpsize"].c_str()));
							}
							if(atoll(rowSum["rtpsize"].c_str()) != sumSize[1][(int)tsf_rtp]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum rtpsize in files [ %llu ri / %llu f ]", spoolIndex, sumSize[1][(int)tsf_rtp], atoll(rowSum["rtpsize"].c_str()));
							}
							if(atoll(rowSum["graphsize"].c_str()) != sumSize[0][(int)tsf_graph]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum graphsize in files [ %llu ii / %llu f ]", spoolIndex, sumSize[0][(int)tsf_graph], atoll(rowSum["graphsize"].c_str()));
							}
							if(atoll(rowSum["graphsize"].c_str()) != sumSize[1][(int)tsf_graph]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum graphsize in files [ %llu ri / %llu f ]", spoolIndex, sumSize[1][(int)tsf_graph], atoll(rowSum["graphsize"].c_str()));
							}
							if(atoll(rowSum["audiosize"].c_str()) != sumSize[0][(int)tsf_audio]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum audiosize in files [ %llu ii / %llu f ]", spoolIndex, sumSize[0][(int)tsf_audio], atoll(rowSum["audiosize"].c_str()));
							}
							if(atoll(rowSum["audiosize"].c_str()) != sumSize[1][(int)tsf_audio]) {
								syslog(LOG_NOTICE, "cleanspool[%i]: # ERROR sum audiosize in files [ %llu ri / %llu f ]", spoolIndex, sumSize[1][(int)tsf_audio], atoll(rowSum["audiosize"].c_str()));
							}
						}
					} else {
						syslog(LOG_NOTICE, "cleanspool[%i]: # MISSING record in files", spoolIndex);
					}
				}
				
				if(sumSizeMissingFilesInIndex[0] || sumSizeMissingFilesInIndex[1]) {
					syslog(LOG_NOTICE, "cleanspool[%i]: sum size of missing file in index: %llu / %llu", spoolIndex, sumSizeMissingFilesInIndex[0], sumSizeMissingFilesInIndex[1]);
				}
			}
		}
	}
}

unsigned int CleanSpool::get_reduk_maxpoolsize(unsigned int maxpoolsize) {
	unsigned int reduk_maxpoolsize = maxpoolsize_set ? maxpoolsize_set : 
					 maxpoolsize ? maxpoolsize : opt_max.maxpoolsize;
	extern TarQueue *tarQueue[2];
	if(tarQueue[spoolIndex]) {
		unsigned int open_tars_size = tarQueue[spoolIndex]->sumSizeOpenTars() / (1204 * 1024);
		if(open_tars_size < reduk_maxpoolsize) {
			reduk_maxpoolsize -= open_tars_size;
		} else {
			return(0);
		}
	}
	return(reduk_maxpoolsize);
}

bool CleanSpool::fileIsOpenTar(list<string> &listOpenTars, string &file) {
	list<string>::iterator iter;
	for(iter = listOpenTars.begin(); iter != listOpenTars.end(); iter++) {
		if(iter->find(file) != string::npos) {
			return(true);
		}
	}
	return(false);
}

void CleanSpool::readSpoolDateDirs(list<string> *dirs) {
	dirs->clear();
	list<string> spool_dirs;
	this->getSpoolDirs(&spool_dirs);
	for(list<string>::iterator iter_sd = spool_dirs.begin(); iter_sd != spool_dirs.end(); iter_sd++) {
		DIR* dp = opendir((*iter_sd).c_str());
		if(dp) {
			dirent* de;
			while((de = readdir(dp)) != NULL) {
				if(de->d_name[0] == '2' && strlen(de->d_name) == 10) {
					bool exists = false;
					for(list<string>::iterator iter_dir = (*dirs).begin(); iter_dir != (*dirs).end(); iter_dir++) {
						if(de->d_name == *iter_dir) {
							exists = true;
							break;
						}
					}
					if(!exists) {
						dirs->push_back(de->d_name);
					}
				}
			}
			closedir(dp);
		}
	}
}

void CleanSpool::getSpoolDirs(list<string> *spool_dirs) {
	spool_dirs->clear();
	for(eTypeSpoolFile typeSpoolFile = tsf_sip; typeSpoolFile < tsf_all; typeSpoolFile = (eTypeSpoolFile)((int)typeSpoolFile + 1)) {
		string spoolDir = getSpoolDir(typeSpoolFile);
		bool exists = false;
		for(list<string>::iterator iter_sd = spool_dirs->begin(); iter_sd != spool_dirs->end(); iter_sd++) {
			if(spoolDir == *iter_sd) {
				exists = true;
				break;
			}
		}
		if(!exists) {
			spool_dirs->push_back(spoolDir);
		}
	}
}

string CleanSpool::findExistsSpoolDirFile(eTypeSpoolFile typeSpoolFile, string pathFile, eTypeSpoolFile *rsltTypeSpoolFile) {
	if(rsltTypeSpoolFile) {
		*rsltTypeSpoolFile = typeSpoolFile;
	}
	string spool_dir;
	for(int i = 0; i < 2; i++) {
		eTypeSpoolFile checkTypeSpoolFile = i == 0 ? typeSpoolFile : tsf_main;
		if(i == 1 && rsltTypeSpoolFile) {
			*rsltTypeSpoolFile = checkTypeSpoolFile;
		}
		spool_dir = getSpoolDir_string(checkTypeSpoolFile) + '/' + pathFile;
		if(i == 0 && file_exists(spool_dir)) {
			break;
		}
	}
	return(spool_dir);
}
