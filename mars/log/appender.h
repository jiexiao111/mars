// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.


/*
 * appender.h
 *
 *  Created on: 2013-3-7
 *      Author: yerungui
 */

#ifndef APPENDER_H_
#define APPENDER_H_

#include <string>
#include <vector>
#include <stdint.h>

#include "boost/bind.hpp"
#include "boost/iostreams/device/mapped_file.hpp"
#include "boost/filesystem.hpp"

#include "mars/comm/thread/lock.h"
#include "mars/comm/xlogger/xloggerbase.h"

#include "log_buffer.h"

enum TAppenderMode
{
    kAppednerAsync,
    kAppednerSync,
};

void appender_open(TAppenderMode _mode, const char* _dir, const char* _nameprefix, const char* _pub_key);
//cirodeng-20180524:add log head info param
void appender_open_with_cache(TAppenderMode _mode, const std::string& _cachedir,
        const std::string& _logdir, const char* _nameprefix, int _cache_days,
        const char* _pub_key, const char* _log_head_info);
void appender_flush();
void appender_flush_sync();
void appender_close();
void appender_setmode(TAppenderMode _mode);
/* bool appender_getfilepath_from_timespan(int _timespan, const char* _prefix, */
/*         std::vector<std::string>& _filepath_vec); */
/* bool appender_make_logfile_name(int _timespan, const char* _prefix, std::vector<std::string>& _filepath_vec); */
bool appender_get_current_log_path(char* _log_path, unsigned int _len);
bool appender_get_current_log_cache_path(char* _logPath, unsigned int _len);
void appender_set_console_log(bool _is_open);

/*
 * By default, all logs will write to one file everyday. You can split logs to multi-file by changing max_file_size.
 * 
 * @param _max_byte_size    Max byte size of single log file, default is 0, meaning do not split.
 */
void appender_set_max_file_size(uint64_t _max_byte_size);

/*
 * By default, all logs lives 10 days at most.
 *
 * @param _max_time    Max alive duration of a single log file in seconds, default is 10 days
 */
void appender_set_max_alive_duration(long _max_time);

class LogAppender {

    public:

        LogAppender(const char *key);
        ~LogAppender();

        void appender_sync(const XLoggerInfo* _info, const char* _log);
        void appender_async(const XLoggerInfo* _info, const char* _log);

        void writetips2file(const char* _tips_format, ...);
        void log2file(const void* _data, size_t _len, bool _move_file);

        bool init_buff(const char* _dir, const char* _nameprefix, const char* _pub_key);
        bool deinit_buff();

        LogBuffer *m_log_buff;                        // 日志先写入 LogBuffer, 满足一定条件才写入文件
        Mutex m_mutex_buffer_async;                   // buffer 锁
        bool m_use_mmap;                              // 是否使用 mmap

    private:

        bool __openlogfile(const std::string& _log_dir);
        void __closelogfile();

        void __make_logfilename(const timeval& _tv, const std::string& _logdir,
                const char* _prefix, const std::string& _fileext, char* _filepath, unsigned int _len);
        std::string __make_logfilenameprefix(const timeval& _tv, const char* _prefix);
        long __get_next_fileindex(const std::string& _fileprefix, const std::string& _fileext);
        void __get_filenames_by_prefix(const std::string& _logdir, const std::string& _fileprefix,
                const std::string& _fileext, std::vector<std::string>& _filename_vec);

        bool __cache_logs();

        bool __writefile(const void* _data, size_t _len, FILE* _file);
        void __writetips2console(const char* _tips_format, ...);

    private:

        std::string m_key;                                    // 表示写入日志文件的类别和级别

        FILE *m_logfile;                              // 日志文件描述符
        time_t m_openfiletime;                        // 日志文件的打开时间
        std::string m_current_dir;                    // 日志文件存放目录

        time_t m_last_time;                           // 上次打开文件的时间
        uint64_t m_last_tick;                         // 上次打开文件的 tick, 没搞懂作用
        char m_last_file_path[1024];                  // 上次打开文件的路径

        boost::iostreams::mapped_file *m_mmmap_file;  // 使用 mmap 时，日志的存储空间
        char* m_buffer;                               // 不使用 mmap 时, 日志的存储空间
};

#endif /* APPENDER_H_ */
