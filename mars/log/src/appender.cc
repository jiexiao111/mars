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

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "mars/log/appender.h"
#include <stdio.h>
#include <map>

#ifdef _WIN32
#define PRIdMAX "lld"
#define snprintf _snprintf
#define strcasecmp _stricmp
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define fileno _fileno
#else
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <sys/mount.h>
#endif

#include <ctype.h>
#include <assert.h>

#include <unistd.h>
#include <zlib.h>

#include <algorithm>

#include "mars/comm/thread/condition.h"
#include "mars/comm/thread/thread.h"
#include "mars/comm/scope_recursion_limit.h"
#include "mars/comm/bootrun.h"
#include "mars/comm/tickcount.h"
#include "mars/comm/autobuffer.h"
#include "mars/comm/ptrbuffer.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/strutil.h"
#include "mars/comm/mmap_util.h"
#include "mars/comm/tickcount.h"
#include "mars/comm/verinfo.h"

#ifdef __APPLE__
#include "mars/comm/objc/data_protect_attr.h"
#endif

#define LOG_EXT "qlog"

extern void log_formater(const XLoggerInfo* _info, const char* _logbody, PtrBuffer& _log);
extern void ConsoleLog(const XLoggerInfo* _info, const char* _log);

static TAppenderMode sg_mode = kAppednerAsync;

static std::string sg_logdir;
static std::string sg_log_head_info;//cirodeng-20180524:add log head info param
static std::string sg_cache_logdir;
static std::string sg_logfileprefix;
static std::string sg_pub_key;
static bool sg_key_logappender_map_destroy = true;

/* 同步写入只会应用于调试阶段, 异步写入只有一个线程负责写入文件, 所以全局文件锁不会产生性能问题 */
static Mutex sg_mutex_log_file; 
static Mutex sg_mutex_key_logappender_map;                   

#ifdef _WIN32
static Condition& sg_cond_buffer_async = *(new Condition());  // 改成引用, 避免在全局释放时执行析构导致crash
#else
static Condition sg_cond_buffer_async;
#endif

#ifdef DEBUG
static bool sg_consolelog_open = true;
#else
static bool sg_consolelog_open = false;
#endif

static uint64_t sg_max_file_size = 0; // 0, will not split log file.
static int sg_cache_log_days = 0;   // 0, will not cache logs

static void __async_log_thread();
static LogAppender* __log_appender_factory(int key);
static Thread sg_thread_async(&__async_log_thread);

static const unsigned int kBufferBlockLength = 150 * 1024;
static const long kMaxLogAliveTime = 10 * 24 * 60 * 60;    // 10 days in second
static const long kMinLogAliveTime = 24 * 60 * 60;    // 1 days in second
static long sg_max_alive_time = kMaxLogAliveTime;

typedef std::map<int, LogAppender* > TKey_Logappender_Map;
TKey_Logappender_Map sg_key_logappender_map;
typedef std::pair<int, LogAppender *> TKey_Logappender_Pair;

namespace {
    class ScopeErrno {
        public:
            ScopeErrno() {m_errno = errno;}
            ~ScopeErrno() {errno = m_errno;}

        private:
            ScopeErrno(const ScopeErrno&);
            const ScopeErrno& operator=(const ScopeErrno&);

        private:
            int m_errno;
    };

#define SCOPE_ERRNO() SCOPE_ERRNO_I(__LINE__)
#define SCOPE_ERRNO_I(line) SCOPE_ERRNO_II(line)
#define SCOPE_ERRNO_II(line) ScopeErrno __scope_errno_##line
}

#define DEFAULT_KEY 0

/* 调试日志 */
#define TRACE_CALL
#ifdef TRACE_CALL
#include <android/log.h>
#include <sys/syscall.h>
#define __TRACE_INFO(...) __android_log_print(ANDROID_LOG_INFO, __FILE__, __VA_ARGS__);
#define __TARCE_FORMAT(__fmt__) "[%s:%d][%ld] " __fmt__
#define gettid() syscall(__NR_gettid)
#define TRACE_INFO(__fmt__, ...) \
    __TRACE_INFO(__TARCE_FORMAT(__fmt__), __func__, __LINE__, gettid(), ##__VA_ARGS__);
#else
#define TRACE_INFO(fmt, ...) ((void)0)
#endif

static bool __string_compare_greater(const std::string& s1, const std::string& s2) {
    if (s1.length() == s2.length()) {
        return s1 > s2;
    }
    return s1.length() > s2.length();
}

static void __del_timeout_file(const std::string& _log_path) {
    TRACE_INFO("%s", "start");

    /* 遍历删除过期文件及目录 */
    time_t now_time = time(NULL);
    boost::filesystem::path path(_log_path);
    if (boost::filesystem::exists(path) && boost::filesystem::is_directory(path)){
        boost::filesystem::directory_iterator end_iter;
        for (boost::filesystem::directory_iterator iter(path); iter != end_iter; ++iter) {
            time_t file_modify_time = boost::filesystem::last_write_time(iter->path());

            /* 最近修改时间大于 1~10 天 */
            if (now_time > file_modify_time && now_time - file_modify_time > sg_max_alive_time) {

                /* 后缀为 .qlog 的文件以及目录名为 8 个数字的目录 */
                if(boost::filesystem::is_regular_file(iter->status())
                        && iter->path().extension() == (std::string(".") + LOG_EXT)) {
                    boost::filesystem::remove(iter->path());
                } 

                /* 目录名为 8 个数字的目录 */
                if (boost::filesystem::is_directory(iter->status())) {
                    std::string filename = iter->path().filename().string();
                    if (filename.size() == 8 && filename.find_first_not_of("0123456789") == std::string::npos) {
                        boost::filesystem::remove_all(iter->path());
                    }
                }
            }
        }
    }

    TRACE_INFO("%s", "end");
}

static bool __append_file(const std::string& _src_file, const std::string& _dst_file) {
    TRACE_INFO("%s", "start");

    if (_src_file == _dst_file) {
        TRACE_INFO("%s", "end");
        return false;
    }
    if (!boost::filesystem::exists(_src_file)) {
        TRACE_INFO("%s", "end");
        return false;
    }
    if (0 == boost::filesystem::file_size(_src_file)){
        TRACE_INFO("%s", "end");
        return true;
    }

    /* 将 src 的信息写入 dst */
    FILE* src_file = fopen(_src_file.c_str(), "rb");
    if (NULL == src_file) {
        TRACE_INFO("%s", "end");
        return false;
    }
    FILE* dest_file = fopen(_dst_file.c_str(), "ab");
    if (NULL == dest_file) {
        fclose(src_file);
        TRACE_INFO("%s", "end");
        return false;
    }
    fseek(src_file, 0, SEEK_END);
    long src_file_len = ftell(src_file);
    long dst_file_len = ftell(dest_file);
    fseek(src_file, 0, SEEK_SET);
    char buffer[4096] = {0};
    while (true) {
        if (feof(src_file)) {
            break;
        }
        size_t read_ret = fread(buffer, 1, sizeof(buffer), src_file);
        if (read_ret == 0) {
            break;
        }
        if (ferror(src_file)) {
            break;
        }
        fwrite(buffer, 1, read_ret, dest_file);
        if (ferror(dest_file))  {
            break;
        }
    }

    /* 如果 dst 的大小大于 src + dst(写入前), 则放弃本次修改 */
    if (dst_file_len + src_file_len > ftell(dest_file)) {
        ftruncate(fileno(dest_file), dst_file_len);
        fclose(src_file);
        fclose(dest_file);
        TRACE_INFO("%s", "end");
        return false;
    }

    fclose(src_file);
    fclose(dest_file);
    TRACE_INFO("%s", "end");
    return true;
}

static void __move_old_files(const std::string& _src_path,
        const std::string& _dest_path, const std::string& _nameprefix) {
    TRACE_INFO("%s", "start");

    if (_src_path == _dest_path) {
        return;
    }
    boost::filesystem::path path(_src_path);
    if (!boost::filesystem::is_directory(path)) {
        return;
    }

    ScopedLock lock_file(sg_mutex_log_file);
    time_t now_time = time(NULL);
    boost::filesystem::directory_iterator end_iter;
    for (boost::filesystem::directory_iterator iter(path); iter != end_iter; ++iter) {

        /* 前缀和后缀(qlog)符合预期 */
        if (!strutil::StartsWith(iter->path().filename().string(), _nameprefix) 
                || !strutil::EndsWith(iter->path().string(), LOG_EXT)) {
            continue;
        }

        /* 缓存时间是否到期 */
        if (sg_cache_log_days > 0) {
            time_t file_modify_time = boost::filesystem::last_write_time(iter->path());
            if (now_time > file_modify_time && (now_time - file_modify_time) < sg_cache_log_days * 24 * 60 * 60) {
                continue;
            }
        }

        /* 从 cache 移动至正式文件 */
        if (!__append_file(iter->path().string(), sg_logdir + "/" + iter->path().filename().string())) {
            break;
        }

        /* 删除 cache 文件 */
        boost::filesystem::remove(iter->path());
    }

    TRACE_INFO("%s", "end");
}

static bool __logs2file() {
    TRACE_INFO("%s", "start");

    bool ret = true;
    TKey_Logappender_Map::iterator iter;
    LogAppender *logappender;
    for (iter = sg_key_logappender_map.begin(); iter != sg_key_logappender_map.end(); iter++) {

        /* 将 buf 中的数据压缩加密 */
        logappender = iter->second;
        ScopedLock lock_buff(logappender->m_mutex_buffer_async);
        if (NULL == logappender->m_log_buff) {
            ret = false;
            lock_buff.unlock();
            continue;
        }
        AutoBuffer tmp_buff;
        logappender->m_log_buff->Flush(tmp_buff);
        lock_buff.unlock();

        /* 将 buff 中的数据写入文件 */
        if (NULL != tmp_buff.Ptr()) {
            logappender->log2file(tmp_buff.Ptr(), tmp_buff.Length(), true);
        }
        if (sg_key_logappender_map_destroy) {
            ret = false;
            continue;
        }
    }

    TRACE_INFO("%s", "end");
    return ret;
}

static void __async_log_thread() {
    TRACE_INFO("%s", "start");

    while (true) {
        /* 如果出现错误则停止线程 */
        if (!__logs2file()) {
            break;
        }
        sg_cond_buffer_async.wait(15 * 60 * 1000);
    }

    TRACE_INFO("%s", "end");
}

void xlogger_appender(const XLoggerInfo* _info, const char* _log) {
    TRACE_INFO("%s", "start");

    /* 根据 level 获取对应的 LogAppender */
    int log_file_key = (NULL == _info) ? DEFAULT_KEY : _info->level;
    LogAppender *logappender;
    logappender = __log_appender_factory(log_file_key);
    if (NULL == logappender) { 
        TRACE_INFO("%s", "end");
        return;
    }

    /* 调试模式将日志打印至 console */
    if (sg_consolelog_open) {
        ConsoleLog(_info,  _log);
    }

    SCOPE_ERRNO();
    DEFINE_SCOPERECURSIONLIMIT(recursion);
    static Tss s_recursion_str(free);
    if (2 <= (int)recursion.Get() && NULL == s_recursion_str.get()) {

        /* 禁止递归调用，出现递归调用时打印错误信息 */
        if ((int)recursion.Get() > 10) {
            TRACE_INFO("%s", "error");
            return;
        }
        char* strrecursion = (char*)calloc(16 * 1024, 1);
        s_recursion_str.set((void*)(strrecursion));
        XLoggerInfo info = *_info;
        info.level = kLevelFatal;
        char recursive_log[256] = {0};
        snprintf(recursive_log, sizeof(recursive_log),
                "ERROR!!! xlogger_appender Recursive calls!!!, count:%d", (int)recursion.Get());
        PtrBuffer tmp(strrecursion, 0, 16*1024);
        log_formater(&info, recursive_log, tmp);
        strncat(strrecursion, _log, 4096);
        strrecursion[4095] = '\0';
        ConsoleLog(&info,  strrecursion);
    } else {

        /* 重置递归标记 */
        if (NULL != s_recursion_str.get()) {
            char* strrecursion = (char*)s_recursion_str.get();
            s_recursion_str.set(NULL);
            logappender->writetips2file(strrecursion);
            free(strrecursion);
        }

        /* 写入日志 */
        if (kAppednerSync == sg_mode) {
            logappender->appender_sync(_info, _log);
        }
        else {
            logappender->appender_async(_info, _log);
        }
    }

    TRACE_INFO("%s", "end");
}

static void get_mark_info(char* _info, size_t _infoLen) {
    TRACE_INFO("%s", "start");

    struct timeval tv;
    gettimeofday(&tv, 0);
    time_t sec = tv.tv_sec;
    struct tm tm_tmp = *localtime((const time_t*)&sec);
    char tmp_time[64] = {0};
    strftime(tmp_time, sizeof(tmp_time), "%Y-%m-%d %z %H:%M:%S", &tm_tmp);
    snprintf(_info, _infoLen, "[%" PRIdMAX ",%" PRIdMAX "][%s]", xlogger_pid(), xlogger_tid(), tmp_time);

    TRACE_INFO("%s", "end");
}

void appender_open(TAppenderMode _mode, const char* _dir, const char* _nameprefix, const char* _pub_key) {
    TRACE_INFO("%s", "start");

    /* 准备目录 */
    assert(_dir);
    assert(_nameprefix);
    tickcount_t tick;
    tick.gettickcount();
    boost::filesystem::create_directories(_dir);
    Thread(boost::bind(&__del_timeout_file, _dir)).start_after(2 * 60 * 1000);

    /* 初始化全局变量 */
    ScopedLock lock(sg_mutex_key_logappender_map);
    sg_logdir = _dir;
    sg_logfileprefix = _nameprefix;
    sg_pub_key = _pub_key;
    sg_key_logappender_map_destroy = false;
    appender_setmode(_mode);
    lock.unlock();

    /* 创建默认 LogAppender, 并注册打印日志接口 */
    xlogger_SetAppender(&xlogger_appender);

#ifdef __APPLE__
    setAttrProtectionNone(_dir);
#endif

    /* 打印初始化时间 */
    tickcountdiff_t get_mmap_time = tickcount_t().gettickcount() - tick;
    char logmsg[256] = {0};
    snprintf(logmsg, sizeof(logmsg), "get mmap time: %" PRIu64, (int64_t)get_mmap_time);
    xlogger_appender(NULL, logmsg);

    /* 打印编译信息 */
    xlogger_appender(NULL, "MARS_URL: " MARS_URL);
    xlogger_appender(NULL, "MARS_PATH: " MARS_PATH);
    xlogger_appender(NULL, "MARS_REVISION: " MARS_REVISION);
    xlogger_appender(NULL, "MARS_BUILD_TIME: " MARS_BUILD_TIME);
    xlogger_appender(NULL, "MARS_BUILD_JOB: " MARS_TAG);

    /* 打印 cache 目录的容量信息 */
    if (!sg_cache_logdir.empty()) {
        boost::filesystem::space_info info = boost::filesystem::space(sg_cache_logdir);
        snprintf(logmsg, sizeof(logmsg), "cache dir space info, capacity:%" PRIuMAX" free:%" PRIuMAX" available:%" PRIuMAX,
                info.capacity, info.free, info.available);
        xlogger_appender(NULL, logmsg);
    }

    /* 打印日志目录的容量信息 */
    boost::filesystem::space_info info = boost::filesystem::space(sg_logdir);
    snprintf(logmsg, sizeof(logmsg), "log dir space info, capacity:%" PRIuMAX" free:%" PRIuMAX" available:%" PRIuMAX,
            info.capacity, info.free, info.available);
    xlogger_appender(NULL, logmsg);

    TRACE_INFO("%s", "end");
    BOOT_RUN_EXIT(appender_close);
}

void appender_open_with_cache(TAppenderMode _mode, const std::string& _cachedir, const std::string& _logdir,
        const char* _nameprefix, int _cache_days, const char* _pub_key, const char* _log_head_info) {
    TRACE_INFO("%s", "start");
    
    assert(!_cachedir.empty());
    assert(!_logdir.empty());
    assert(_nameprefix);

    sg_logdir = _logdir;
    sg_log_head_info = _log_head_info;//cirodeng-20180524:add log head info param
    sg_cache_log_days = _cache_days;

    /* 定时将 cache 文件移动到正式目录 */
    if (!_cachedir.empty()) {
        sg_cache_logdir = _cachedir;
        boost::filesystem::create_directories(_cachedir);
        Thread(boost::bind(&__del_timeout_file, _cachedir)).start_after(2 * 60 * 1000);
        // "_nameprefix" must explicitly convert to "std::string", or when the thread
        // is ready to run, "_nameprefix" has been released.
        Thread(boost::bind(&__move_old_files, _cachedir, _logdir, std::string(_nameprefix))).start_after(3 * 60 * 1000);
    }

#ifdef __APPLE__
    setAttrProtectionNone(_cachedir.c_str());
#endif
    appender_open(_mode, _logdir.c_str(), _nameprefix, _pub_key);

    TRACE_INFO("%s", "end");
}

void appender_flush() {
    TRACE_INFO("%s", "start");

    sg_cond_buffer_async.notifyAll();

    TRACE_INFO("%s", "end");
}

void appender_flush_sync() {
    TRACE_INFO("%s", "start");

    if (kAppednerSync == sg_mode) {
        return;
    }
    __logs2file();

    TRACE_INFO("%s", "end");
}

void appender_close() {
    TRACE_INFO("%s", "start");

    /* 将 buff 中的日志写入文件 */
    sg_cond_buffer_async.notifyAll();
    if (sg_thread_async.isruning())
        sg_thread_async.join();

    /* 执行 deinit_buff, 释放 LogAppender 对象 */
    ScopedLock lock(sg_mutex_key_logappender_map);
    TKey_Logappender_Map::iterator iter;
    for (iter = sg_key_logappender_map.begin(); iter != sg_key_logappender_map.end(); iter++) {
        iter->second->deinit_buff();
        delete iter->second;
    }
    sg_key_logappender_map_destroy = true;

    TRACE_INFO("%s", "end");
}

void appender_setmode(TAppenderMode _mode) {
    TRACE_INFO("%s", "start");

    sg_mode = _mode;

    sg_cond_buffer_async.notifyAll();

    if (kAppednerAsync == sg_mode && !sg_thread_async.isruning()) {
        sg_thread_async.start();
    }

    TRACE_INFO("%s", "end");
}

bool appender_get_current_log_path(char* _log_path, unsigned int _len) {
    TRACE_INFO("%s", "start");

    if (NULL == _log_path || 0 == _len) {
        return false;
    }
    if (sg_logdir.empty()) {
        return false;
    }
    strncpy(_log_path, sg_logdir.c_str(), _len - 1);
    _log_path[_len - 1] = '\0';

    TRACE_INFO("%s", "end");
    return true;
}

bool appender_get_current_log_cache_path(char* _logPath, unsigned int _len) {
    TRACE_INFO("%s", "start");

    if (NULL == _logPath || 0 == _len) {
        return false;
    }
    if (sg_cache_logdir.empty()) {
        return false;
    }
    strncpy(_logPath, sg_cache_logdir.c_str(), _len - 1);
    _logPath[_len - 1] = '\0';

    TRACE_INFO("%s", "end");
    return true;
}

void appender_set_console_log(bool _is_open) {
    TRACE_INFO("%s", "start");

    sg_consolelog_open = _is_open;

    TRACE_INFO("%s", "end");
}

void appender_set_max_file_size(uint64_t _max_byte_size) {
    TRACE_INFO("%s", "start");

    sg_max_file_size = _max_byte_size;

    TRACE_INFO("%s", "end");
}

void appender_set_max_alive_duration(long _max_time) {
    TRACE_INFO("%s", "start");

    if (_max_time >= kMinLogAliveTime) {
        sg_max_alive_time = _max_time;
    }

    TRACE_INFO("%s", "end");
}

LogAppender::LogAppender(int key) {
    TRACE_INFO("%s", "start");

    m_key = key;
    m_mmmap_file = new boost::iostreams::mapped_file;

    m_log_buff = NULL;
    m_logfile = NULL;

    m_buffer = NULL;
    m_openfiletime = 0;

    m_use_mmap = false;

    m_last_time = 0;
    m_last_tick = 0;
    memset(m_last_file_path, 0, sizeof(m_last_file_path));

    TRACE_INFO("%s", "end");
}

LogAppender::~LogAppender() {
    TRACE_INFO("%s", "start");

    if (NULL != m_mmmap_file) {
        delete m_mmmap_file;
        m_mmmap_file = NULL;
    }

    /* 执行 __closelogfile */
    __closelogfile();

    TRACE_INFO("%s", "end");
}

bool LogAppender::init_buff(const char* _dir, const char* _nameprefix, const char* _pub_key) {
    TRACE_INFO("%s", "start");

    char mmap_file_path[512] = {0};
    snprintf(mmap_file_path, sizeof(mmap_file_path), "%s/%s.mmap%d",
            sg_cache_logdir.empty() ? _dir : sg_cache_logdir.c_str(), _nameprefix, m_key);

    ScopedLock buffer_lock(m_mutex_buffer_async);
    /* 创建 m_log_buff */
    if (NULL != m_mmmap_file && OpenMmapFile(mmap_file_path, kBufferBlockLength, *m_mmmap_file))  {
        m_log_buff = new LogBuffer(m_mmmap_file->data(), kBufferBlockLength, true, _pub_key);
        m_use_mmap = true;
    } else {
        m_buffer = new char[kBufferBlockLength];
        m_log_buff = new LogBuffer(m_buffer, kBufferBlockLength, true, _pub_key);
        m_use_mmap = false;
    }

    /* 异常处理 */
    if (NULL == m_log_buff->GetData().Ptr()) {
        if (m_use_mmap && NULL != m_mmmap_file && m_mmmap_file->is_open()) {
            CloseMmapFile(*m_mmmap_file);
        }
        return false;
    }

    TRACE_INFO("%s", "end");
    return true;
}

bool LogAppender::deinit_buff() {
    TRACE_INFO("%s", "start");

    /* 释放 m_log_buff */
    ScopedLock buffer_lock(m_mutex_buffer_async);
    if (NULL != m_log_buff) {
        delete m_log_buff;
        m_log_buff = NULL;
    }

    /* 释放 m_log_buff 内部资源 */
    if (NULL != m_buffer) {
        delete m_buffer;
    }
    if (NULL != m_mmmap_file && m_mmmap_file->is_open()) {
        if (!m_mmmap_file->operator !()) {
            memset(m_mmmap_file->data(), 0, kBufferBlockLength);
        }
        CloseMmapFile(*m_mmmap_file);
    }

    TRACE_INFO("%s", "end");
    return true;
}

void LogAppender::appender_sync(const XLoggerInfo* _info, const char* _log) {
    TRACE_INFO("%s", "start");

    char temp[16 * 1024] = {0};     // tell perry,ray if you want modify size.
    PtrBuffer log(temp, 0, sizeof(temp));
    log_formater(_info, _log, log);

    AutoBuffer tmp_buff;
    if (!m_log_buff->Write(log.Ptr(), log.Length(), tmp_buff))   return;

    log2file(tmp_buff.Ptr(), tmp_buff.Length(), false);

    TRACE_INFO("%s", "end");
}

void LogAppender::appender_async(const XLoggerInfo* _info, const char* _log) {
    TRACE_INFO("%s", "start");

    ScopedLock lock(m_mutex_buffer_async);
    if (NULL == m_log_buff) {
        return;
    }

    char temp[16*1024] = {0};       //tell perry,ray if you want modify size.
    PtrBuffer log_buff(temp, 0, sizeof(temp));
    log_formater(_info, _log, log_buff);

    if (m_log_buff->GetData().Length() >= kBufferBlockLength*4/5) {
        int ret = snprintf(temp, sizeof(temp), "[F][ sg_buffer_async.Length() >= BUFFER_BLOCK_LENTH*4/5, len: %d\n", (int)m_log_buff->GetData().Length());
        log_buff.Length(ret, ret);
    }

    if (!m_log_buff->Write(log_buff.Ptr(), (unsigned int)log_buff.Length())) {
        return;
    }

    if (m_log_buff->GetData().Length() >= kBufferBlockLength*1/3 || (NULL!=_info && kLevelFatal == _info->level)) {
        sg_cond_buffer_async.notifyAll();
    }

    TRACE_INFO("%s", "end");
}

void LogAppender::writetips2file(const char* _tips_format, ...) {
    TRACE_INFO("%s [%d]", "start", m_key);

    if (NULL == _tips_format) {
        return;
    }

    char tips_info[4096] = {0};
    va_list ap;
    va_start(ap, _tips_format);
    vsnprintf(tips_info, sizeof(tips_info), _tips_format, ap);
    va_end(ap);

    AutoBuffer tmp_buff;
    m_log_buff->Write(tips_info, strnlen(tips_info, sizeof(tips_info)), tmp_buff);

    log2file(tmp_buff.Ptr(), tmp_buff.Length(), false);

    TRACE_INFO("%s", "end");
}

void LogAppender::log2file(const void* _data, size_t _len, bool _move_file) {
    TRACE_INFO("%s [%d]", "start", m_key);

    if (NULL == _data || 0 == _len || sg_logdir.empty()) {
        TRACE_INFO("%s", "end");
        return;
    }

    ScopedLock lock_file(sg_mutex_log_file);

    if (sg_cache_logdir.empty()) {
        if (__openlogfile(sg_logdir)) {
            __writefile(_data, _len, m_logfile);
            if (kAppednerAsync == sg_mode) {
                __closelogfile();
            }
        }
        TRACE_INFO("%s", "end");
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    char logcachefilepath[1024] = {0};
    __make_logfilename(tv, sg_cache_logdir, sg_logfileprefix.c_str(), LOG_EXT, logcachefilepath , 1024);

    bool cache_logs = __cache_logs();
    if ((cache_logs || boost::filesystem::exists(logcachefilepath)) && __openlogfile(sg_cache_logdir)) {
        __writefile(_data, _len, m_logfile);
        if (kAppednerAsync == sg_mode) {
            __closelogfile();
        }

        if (cache_logs || !_move_file) {
            TRACE_INFO("%s", "end");
            return;
        }

        char logfilepath[1024] = {0};
        __make_logfilename(tv, sg_logdir, sg_logfileprefix.c_str(), LOG_EXT, logfilepath , 1024);
        if (__append_file(logcachefilepath, logfilepath)) {
            if (kAppednerSync == sg_mode) {
                __closelogfile();
            }
            boost::filesystem::remove(logcachefilepath);
        }
        TRACE_INFO("%s", "end");
        return;
    }

    bool write_sucess = false;
    bool open_success = __openlogfile(sg_logdir);
    if (open_success) {
        write_sucess = __writefile(_data, _len, m_logfile);
        if (kAppednerAsync == sg_mode) {
            __closelogfile();
        }
    }

    if (!write_sucess) {
        if (open_success && kAppednerSync == sg_mode) {
            __closelogfile();
        }

        if (__openlogfile(sg_cache_logdir)) {
            __writefile(_data, _len, m_logfile);
            if (kAppednerAsync == sg_mode) {
                __closelogfile();
            }
        }
    }

    TRACE_INFO("%s", "end");
}

void LogAppender::__closelogfile() {
    TRACE_INFO("%s", "start");

    if (NULL == m_logfile) {
        return;
    }
    fclose(m_logfile);
    m_logfile = NULL;
    m_openfiletime = 0;

    TRACE_INFO("%s", "end");
}

bool LogAppender::__openlogfile(const std::string& _log_dir) {
    TRACE_INFO("%s", "start");

    if (sg_logdir.empty()) {
        return false;
    }

    /* 处理文件已经打开的情况 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (NULL != m_logfile) {
        time_t sec = tv.tv_sec;
        tm tcur = *localtime((const time_t*)&sec);
        tm filetm = *localtime(&m_openfiletime);
        if (filetm.tm_year == tcur.tm_year && filetm.tm_mon == tcur.tm_mon 
                && filetm.tm_mday == tcur.tm_mday && m_current_dir == _log_dir) {
            return true;
        }

        fclose(m_logfile);
        m_logfile = NULL;
    }

    /* 打开日志文件 */
    uint64_t now_tick = gettickcount();
    time_t now_time = tv.tv_sec;
    m_openfiletime = tv.tv_sec;
    m_current_dir = _log_dir;
    char logfilepath[1024] = {0};
    __make_logfilename(tv, _log_dir, sg_logfileprefix.c_str(), LOG_EXT, logfilepath , 1024);
    if (now_time < m_last_time) {
        m_logfile = fopen(m_last_file_path, "ab");

        if (NULL == m_logfile) {
            __writetips2console("open file error:%d %s, path:%s", errno, strerror(errno), m_last_file_path);
        }
#ifdef __APPLE__
        assert(m_logfile);
#endif
        return NULL != m_logfile;
    }
    m_logfile = fopen(logfilepath, "ab");
    if (NULL == m_logfile) {
        __writetips2console("open file error:%d %s, path:%s", errno, strerror(errno), logfilepath);
    }

    /* 写入设备信息 */
    if (0 == ftell(m_logfile)) {
        //cirodeng-20180524:add common info in the head of each logfile(not encrypted)
        char common_log[4096] = {0};
        snprintf(common_log, sizeof(common_log), "%s\n", sg_log_head_info.c_str());
        AutoBuffer tmp_common_buff;
        m_log_buff->Write(common_log, strnlen(common_log, sizeof(common_log)), tmp_common_buff);
        __writefile(tmp_common_buff.Ptr(), tmp_common_buff.Length(), m_logfile);
    }

    /* 没看懂, 好像是打印调试信息 */
    if (0 != m_last_time && (now_time - m_last_time) > (time_t)((now_tick - m_last_tick) / 1000 + 300)) {

        struct tm tm_tmp = *localtime((const time_t*)&m_last_time);
        char last_time_str[64] = {0};
        strftime(last_time_str, sizeof(last_time_str), "%Y-%m-%d %z %H:%M:%S", &tm_tmp);

        tm_tmp = *localtime((const time_t*)&now_time);
        char now_time_str[64] = {0};
        strftime(now_time_str, sizeof(now_time_str), "%Y-%m-%d %z %H:%M:%S", &tm_tmp);

        char log[1024] = {0};
        snprintf(log, sizeof(log), "[F][ last log file:%s from %s to %s, time_diff:%ld, tick_diff:%" PRIu64 "\n",
                m_last_file_path, last_time_str, now_time_str, now_time-m_last_time, now_tick-m_last_tick);

        AutoBuffer tmp_buff;
        m_log_buff->Write(log, strnlen(log, sizeof(log)), tmp_buff);
        __writefile(tmp_buff.Ptr(), tmp_buff.Length(), m_logfile);
    }
    memcpy(m_last_file_path, logfilepath, sizeof(m_last_file_path));
    m_last_tick = now_tick;
    m_last_time = now_time;
#ifdef __APPLE__
    assert(m_logfile);
#endif

    TRACE_INFO("%s", "end");
    return NULL != m_logfile;
}

void LogAppender::__make_logfilename(const timeval& _tv, const std::string& _logdir,
        const char* _prefix, const std::string& _fileext, char* _filepath, unsigned int _len) {
    TRACE_INFO("%s", "start");

    long index_ = 0;
    std::string logfilenameprefix = __make_logfilenameprefix(_tv, _prefix);
    if (sg_max_file_size > 0) {
        index_ = __get_next_fileindex(logfilenameprefix, _fileext);
    }

    std::string logfilepath = _logdir;
    logfilepath += "/";
    logfilepath += logfilenameprefix;

    if (index_ > 0) {
        char temp[24] = {0};
        snprintf(temp, 24, "_%ld", index_);
        logfilepath += temp;
    }

    logfilepath += ".";
    logfilepath += _fileext;

    strncpy(_filepath, logfilepath.c_str(), _len - 1);
    _filepath[_len - 1] = '\0';

    TRACE_INFO("%s", "end");
}

std::string LogAppender::__make_logfilenameprefix(const timeval& _tv, const char* _prefix) {
    TRACE_INFO("%s", "start");

    time_t sec = _tv.tv_sec;
    tm tcur = *localtime((const time_t*)&sec);

    char temp [64] = {0};
    snprintf(temp, 64, "_%d%02d%02d%02d%02d%02d", 1900 + tcur.tm_year, 1 + tcur.tm_mon,
            tcur.tm_mday, tcur.tm_hour, 0, m_key);//配合logsdk按照小时分割日志，分和秒变为0

    std::string filenameprefix = _prefix;
    filenameprefix += temp;

    TRACE_INFO("%s", "end");
    return filenameprefix;
}

long LogAppender::__get_next_fileindex(const std::string& _fileprefix, const std::string& _fileext) {
    TRACE_INFO("%s", "start");

    std::vector<std::string> filename_vec;
    __get_filenames_by_prefix(sg_logdir, _fileprefix, _fileext, filename_vec);
    if (!sg_cache_logdir.empty()) {
        __get_filenames_by_prefix(sg_cache_logdir, _fileprefix, _fileext, filename_vec);
    }

    long index = 0; // long is enought to hold all indexes in one day.
    if (filename_vec.empty()) {
        return index;
    }
    // high -> low
    std::sort(filename_vec.begin(), filename_vec.end(), __string_compare_greater);
    std::string last_filename = *(filename_vec.begin());
    std::size_t ext_pos = last_filename.rfind("." + _fileext);
    std::size_t index_len = ext_pos - _fileprefix.length();
    if (index_len > 0) {
        std::string index_str = last_filename.substr(_fileprefix.length(), index_len);
        if (strutil::StartsWith(index_str, "_")) {
            index_str = index_str.substr(1);
        }
        index = atol(index_str.c_str());
    }

    uint64_t filesize = 0;
    std::string logfilepath = sg_logdir + "/" + last_filename;
    if (boost::filesystem::exists(logfilepath)) {
        filesize += boost::filesystem::file_size(logfilepath);
    }
    if (!sg_cache_logdir.empty()) {
        logfilepath = sg_cache_logdir + "/" + last_filename;
        if (boost::filesystem::exists(logfilepath)) {
            filesize += boost::filesystem::file_size(logfilepath);
        }
    }

    TRACE_INFO("%s", "end");
    return (filesize > sg_max_file_size) ? index + 1 : index;
}

void LogAppender::__get_filenames_by_prefix(const std::string& _logdir, const std::string& _fileprefix,
        const std::string& _fileext, std::vector<std::string>& _filename_vec) {
    TRACE_INFO("%s", "start");

    boost::filesystem::path path(_logdir);
    if (!boost::filesystem::is_directory(path)) {
        return;
    }

    boost::filesystem::directory_iterator end_iter;
    std::string filename;

    for (boost::filesystem::directory_iterator iter(path); iter != end_iter; ++iter) {
        if (boost::filesystem::is_regular_file(iter->status())) {
            filename = iter->path().filename().string();
            if (strutil::StartsWith(filename, _fileprefix) && strutil::EndsWith(filename, _fileext)) {
                _filename_vec.push_back(filename);
            }
        }
    }

    TRACE_INFO("%s", "end");
}

bool LogAppender::__cache_logs() {
    TRACE_INFO("%s", "start");

    if (sg_cache_logdir.empty() || sg_cache_log_days <= 0) {
        TRACE_INFO("%s", "end");
        return false;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    char logfilepath[1024] = {0};
    __make_logfilename(tv, sg_logdir, sg_logfileprefix.c_str(), LOG_EXT, logfilepath , 1024);
    if (boost::filesystem::exists(logfilepath)) {
        return false;
    }

    static const uintmax_t kAvailableSizeThreshold = (uintmax_t)1 * 1024 * 1024 * 1024;   // 1G
    boost::filesystem::space_info info = boost::filesystem::space(sg_cache_logdir);
    if (info.available < kAvailableSizeThreshold) {
        return false;
    }

    TRACE_INFO("%s", "end");
    return true;
}

bool LogAppender::__writefile(const void* _data, size_t _len, FILE* _file) {
    TRACE_INFO("%s [%d]", "start", m_key);

    if (NULL == _file) {
        assert(false);
        return false;
    }

    long before_len = ftell(_file);
    if (before_len < 0) return false;

    if (1 != fwrite(_data, _len, 1, _file)) {
        int err = ferror(_file);

        __writetips2console("write file error:%d", err);

        ftruncate(fileno(_file), before_len);
        fseek(_file, 0, SEEK_END);

        char err_log[256] = {0};
        snprintf(err_log, sizeof(err_log), "\nwrite file error:%d\n", err);

        AutoBuffer tmp_buff;
        m_log_buff->Write(err_log, strnlen(err_log, sizeof(err_log)), tmp_buff);

        fwrite(tmp_buff.Ptr(), tmp_buff.Length(), 1, _file);

        return false;
    }

    TRACE_INFO("%s", "end");
    return true;
}

void LogAppender::__writetips2console(const char* _tips_format, ...) {
    TRACE_INFO("%s", "start");

    /* 格式化字符串打印至 console */
    if (NULL == _tips_format) {
        return;
    }
    XLoggerInfo info;
    memset(&info, 0, sizeof(XLoggerInfo));
    char tips_info[4096] = {0};
    va_list ap;
    va_start(ap, _tips_format);
    vsnprintf(tips_info, sizeof(tips_info), _tips_format, ap);
    va_end(ap);
    ConsoleLog(&info, tips_info);

    TRACE_INFO("%s", "end");
}

static LogAppender* __log_appender_factory(int key) {
    TRACE_INFO("%s", "start")

    ScopedLock map_lock(sg_mutex_key_logappender_map);
    /* 如果存在直接返回 LogAppender */
    TKey_Logappender_Map::iterator iter;
    iter = sg_key_logappender_map.find(key);
    if (iter != sg_key_logappender_map.end()) {
        TRACE_INFO("%s", "end")
        return iter->second;
    }
    if (sg_key_logappender_map_destroy) {
        TRACE_INFO("%s", "end")
        return NULL;
    }

    TRACE_INFO("Create LogAppender: [%d]", key)

    /* 如果不存在则初始化 LogAppender */
    LogAppender *logappender = new LogAppender(key);
    logappender->init_buff(sg_logdir.c_str(), sg_logfileprefix.c_str(), sg_pub_key.c_str());

    /* 测试接口 */
    AutoBuffer tmp_buffer;
    logappender->m_log_buff->Flush(tmp_buffer);
    char mark_info[512] = {0};
    get_mark_info(mark_info, sizeof(mark_info));
    logappender->writetips2file("~~~~~ begin of mmap ~~~~~\n");
    logappender->writetips2file("LogAppender Key [%d] use_mmap [%d]\n", key, logappender->m_use_mmap );
    logappender->log2file(tmp_buffer.Ptr(), tmp_buffer.Length(), false);
    logappender->writetips2file("%s\n", mark_info);
    logappender->writetips2file("~~~~~ end of mmap ~~~~~\n");
    sg_key_logappender_map.insert(TKey_Logappender_Pair(key, logappender));

    TRACE_INFO("%s", "end")
    return logappender;
}

