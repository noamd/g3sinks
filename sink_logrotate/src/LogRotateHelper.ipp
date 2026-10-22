/** ==========================================================================
* 2011 by KjellKod.cc, modified by Vrecan in https://bitbucket.org/vrecan/g2log-dev
* 2015, adopted by KjellKod for g3log at:https://github.com/KjellKod/g3sinks
*
* This code is PUBLIC DOMAIN to use at your own risk and comes
* with no warranties. This code is yours to share, use and modify with no
* strings attached and no restrictions or obligations.
* ============================================================================*
* PUBLIC DOMAIN and Not copywrited. First published at KjellKod.cc
* ********************************************* */

#pragma once

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <string>
#include <memory>
#include <fstream>
#include <algorithm>
#include <future>
#include <cassert>
#include <chrono>
#include <g3log/time.hpp>
#include <zlib.h>
#include <regex>
#include <map>
#include <vector>
#include <ctime>
#include <iostream>
#include <sstream>
#include <filesystem>

#include "g3sinks/LogRotateUtility.h"


using namespace LogRotateUtility;

/** The Real McCoy Background worker, while g3::LogWorker gives the
 * asynchronous API to put job in the background the LogRotateHelper
 * does the actual background thread work
 *
 * Flushing of log entries will happen according to flush policy:
 * 0 is never (system decides, and when there is a log rotation)
 * 1 ... N means every x entry (1 is every time, 2 is every other time etc)
 * Default is to flush every single time
 */
struct LogRotateHelper {
   LogRotateHelper& operator=(const LogRotateHelper&) = delete;
   LogRotateHelper(const LogRotateHelper& other) = delete;
   LogRotateHelper(const std::string& log_prefix, const std::string& log_directory, size_t flush_policy = 1);
   ~LogRotateHelper();

   void setMaxArchiveLogCount(int size);
   int getMaxArchiveLogCount();
   void setMaxLogSize(int size);
   int getMaxLogSize();


   void fileWrite(std::string message);
   void fileWriteWithoutRotate(std::string message);
   void flushPolicy();
   void setFlushPolicy(size_t flush_policy);
   void flush();

   std::string changeLogFile(const std::string& directory, const std::string& new_name = "");
   std::string logFileName();
   bool archiveLog();


   void addLogFileHeader();
   bool rotateLog();
   std::string archiveFileName();
   void checkLogFile();
   bool reopenLogFile(const std::string& note);
   void setLogSizeCounter();
   bool createCompressedFile(std::string file_name, std::string gzip_file_name);
   std::ofstream& filestream() {
      return *(outptr_.get());
   }

   // how often the write path checks that log_file_with_path_ is still the file we write to
   static constexpr std::chrono::seconds kLogFileCheckInterval{1};
   // how long to wait before retrying a failed reopen or rotation
   static constexpr std::chrono::seconds kRetryInterval{30};
   static constexpr const char* kLogRemovedNote =
      "g3log: log file was removed externally, its content since the last rotation was not archived\n";

   std::string log_file_with_path_;
   std::string log_directory_;
   std::string log_prefix_backup_;
   std::unique_ptr<std::ofstream> outptr_;
   steady_time_point steady_start_time_;
   int max_log_size_;
   int max_archive_log_count_;
   std::streamoff cur_log_size_;
   size_t flush_policy_;
   size_t flush_policy_counter_;
   dev_t log_file_dev_;  // identity of the open file, 0 when unknown
   ino_t log_file_ino_;
   steady_time_point next_log_file_check_;
   steady_time_point next_rotation_attempt_;
   std::chrono::system_clock::time_point last_archive_time_;
};

LogRotateHelper::LogRotateHelper(const std::string& log_prefix, const std::string& log_directory, size_t flush_policy)
   : log_file_with_path_(log_directory)
   , log_directory_(log_directory)
   , log_prefix_backup_(log_prefix)
   , outptr_(new std::ofstream)
   , steady_start_time_(std::chrono::steady_clock::now())
   , cur_log_size_(0)
   , flush_policy_(flush_policy)
   , flush_policy_counter_(flush_policy)
   , log_file_dev_(0)
   , log_file_ino_(0)
   , next_log_file_check_()
   , next_rotation_attempt_()
   , last_archive_time_() {
   log_prefix_backup_ = prefixSanityFix(log_prefix);
   max_log_size_ = 524288000;
   max_archive_log_count_ = 10;
   if (!isValidFilename(log_prefix_backup_)) {
      std::cerr << "g3log: forced abort due to illegal log prefix [" << log_prefix << "]" << std::endl;
      abort();
   }
   // set before opening: if the open fails, checkLogFile keeps retrying this path
   log_file_with_path_ = addLogSuffix(createPathToFile(sanityFixPath(log_directory), log_prefix_backup_));
   changeLogFile(log_directory, log_prefix_backup_);
}

/**
 * Max number of archived logs to keep.
 * @param max_size
 */
void LogRotateHelper::setMaxArchiveLogCount(int max_size) {
   max_archive_log_count_ = max_size;
}

int LogRotateHelper::getMaxArchiveLogCount() {
   return max_archive_log_count_;
}


/**
 * Set the max file size in bytes.
 * @param max_size
 */
void LogRotateHelper::setMaxLogSize(int max_size) {
   max_log_size_ = max_size;
}

int LogRotateHelper::getMaxLogSize() {
   return max_log_size_;
}





LogRotateHelper::~LogRotateHelper() {
   std::ostringstream ss_exit;
   auto now = std::chrono::system_clock::now();
   ss_exit << "\ng3log file shutdown at: " << g3::localtime_formatted(now, g3::internal::time_formatted) << "\n\n";
   filestream() << ss_exit.str() << std::flush;
}

void LogRotateHelper::fileWrite(std::string message) {
   checkLogFile();
   if (cur_log_size_ > max_log_size_) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_rotation_attempt_ && !rotateLog()) {
         // e.g. compression failed: don't redo it on every line
         next_rotation_attempt_ = now + kRetryInterval;
      }
   }
   fileWriteWithoutRotate(message);
}

void LogRotateHelper::fileWriteWithoutRotate(std::string message) {
   std::ofstream& out(filestream());
   out << message;
   flushPolicy();
   cur_log_size_ += message.size();
}



void LogRotateHelper::flushPolicy() {
   if (0 == flush_policy_) return;
   if (0 == --flush_policy_counter_) {
      flush();
      flush_policy_counter_ = flush_policy_;
   }
}


void LogRotateHelper::setFlushPolicy(size_t flush_policy) {
   flush();
   flush_policy_ = flush_policy;
   flush_policy_counter_ = flush_policy;
}


void LogRotateHelper::flush() {
   filestream() << std::flush;
}



std::string LogRotateHelper::changeLogFile(const std::string& directory, const std::string& new_name) {
   std::string file_name = new_name;
   if (file_name.empty()) {
      file_name = log_prefix_backup_;
   }


   auto prospect_path = sanityFixPath(directory);
   auto prospect_log = createPathToFile(prospect_path, file_name);

   namespace fs = std::filesystem;
   fs::path log_dir_path = fs::path(prospect_path);
   std::error_code ec;  // runs on the sink thread, where an exception terminates the process
   if (!fs::exists(log_dir_path, ec)) {
      fs::create_directories(log_dir_path, ec);  // a failure is reported below, when the file cannot be created
   }

   prospect_log = addLogSuffix(prospect_log);

   std::unique_ptr<std::ofstream> log_stream = createLogFile(prospect_log);
   if (nullptr == log_stream) {
      // not fileWrite: it may rotate, and rotation calls back here
      fileWriteWithoutRotate("Unable to change log file. Illegal filename or busy? Unsuccessful log name was:" + prospect_log + "\n");
      return ""; // no success
   }
   log_prefix_backup_ = file_name;
   log_file_with_path_ = prospect_log;
   outptr_ = std::move(log_stream);
   log_directory_ = directory;

   addLogFileHeader();
   setLogSizeCounter();

   return log_file_with_path_;
}

/**
 * Rotate the logs once they have exceeded our set size.
 * @return
 */
bool LogRotateHelper::rotateLog() {
   namespace fs = std::filesystem;
   std::error_code ec;
   const bool log_file_exists = fs::exists(log_file_with_path_, ec);
   if (ec) {
      return false; // cannot tell (e.g. no permission), try again later
   }
   if (!log_file_exists) {
      return reopenLogFile(kLogRemovedNote);
   }

   std::ofstream& is(filestream());
   if (is.is_open()) {
      is << std::flush;
      const std::string gz_file_name = archiveFileName();
      if (!createCompressedFile(log_file_with_path_, gz_file_name)) {
         fileWriteWithoutRotate("Failed to compress log to " + gz_file_name + ", will retry in "
                                + std::to_string(kRetryInterval.count()) + " seconds\n");
         return false;
      }
      is.close();
      std::string note = "Log rotated Archived file name: " + gz_file_name + "\n";
      std::error_code ec_file;
      fs::remove(log_file_with_path_, ec_file);
      if (ec_file) {
         // Its content is archived, so empty it instead. Left as is, the same lines
         // would be archived again on the next rotation.
         const std::string failure = "Failed to remove old log: " + log_file_with_path_ + ". Error: " + ec_file.message();
         fs::resize_file(log_file_with_path_, 0, ec_file);
         if (ec_file) {
            fs::remove(gz_file_name, ec_file);  // keep the lines in one place only, rotate again later
            reopenLogFile(failure + ", will retry in " + std::to_string(kRetryInterval.count()) + " seconds\n");
            return false;
         }
         note += failure + ", emptied it instead\n";
      }
      changeLogFile(log_directory_);
      fileWriteWithoutRotate(note);
      std::ostringstream ss;
      ss << log_prefix_backup_ << ".log";
      expireArchives(log_directory_, ss.str(), max_archive_log_count_);
      return true;
   }
   return false;
}

/**
 * @return the name for the next archive. A rotation within the same second must not overwrite
 * the previous archive, so take a later second rather than adding a suffix: expireArchives reads
 * the time back from the name. Names only move forward, otherwise a second freed by expiring
 * the oldest archive would be reused and the new archive expired first.
 */
std::string LogRotateHelper::archiveFileName() {
   namespace fs = std::filesystem;
   auto archive_time = std::max(std::chrono::system_clock::now(), last_archive_time_ + std::chrono::seconds(1));
   std::string gz_file_name;
   std::error_code ec;
   while (true) {
      gz_file_name = log_file_with_path_ + "." + g3::localtime_formatted(archive_time, "%Y-%m-%d-%H-%M-%S") + ".gz";
      if (!fs::exists(gz_file_name, ec)) {
         break;
      }
      archive_time += std::chrono::seconds(1);  // e.g. left by a previous run within the same second
   }
   last_archive_time_ = archive_time;
   return gz_file_name;
}

/**
 * Make sure we still write to the file at log_file_with_path_. Someone may have removed it
 * ("rm"), or replaced it (an editor saving, "mv" plus a new file), or it failed to open earlier.
 * Writing on would put the lines in a file nobody can reach. Also take the size counter from
 * the real file, which truncation or other writers may have changed.
 * Runs at most once per kLogFileCheckInterval.
 */
void LogRotateHelper::checkLogFile() {
   const auto now = std::chrono::steady_clock::now();
   if (now < next_log_file_check_) {
      return;
   }
   next_log_file_check_ = now + kLogFileCheckInterval;

   if (!filestream().is_open()) {
      reopenLogFile("g3log: log file could not be opened earlier, lines logged meanwhile were lost\n");
      return;
   }

   struct stat st {};
   if (stat(log_file_with_path_.c_str(), &st) != 0) {
      if (errno == ENOENT || errno == ENOTDIR) {
         reopenLogFile(kLogRemovedNote);
      }
      return; // other errors: cannot tell, check again later
   }
   if (log_file_ino_ != 0 && (st.st_dev != log_file_dev_ || st.st_ino != log_file_ino_)) {
      reopenLogFile("g3log: log file was replaced externally, continuing in the new file\n");
      return;
   }
   if (filestream().fail()) {
      // a write failed (e.g. disk full) and the stream ignores every later write until reopened
      reopenLogFile("g3log: writing to the log file failed (disk full?), some lines were lost\n");
      return;
   }
   cur_log_size_ = st.st_size;
}

/**
 * Close the current log, which also releases its disk space if it was removed,
 * and start writing to log_file_with_path_ again.
 * @param note written to the new file to explain the reopen
 * @return true if the log file was opened
 */
bool LogRotateHelper::reopenLogFile(const std::string& note) {
   filestream().close();
   if (changeLogFile(log_directory_).empty()) {
      next_log_file_check_ = std::chrono::steady_clock::now() + kRetryInterval;
      return false;
   }
   fileWriteWithoutRotate(note);
   return true;
}



/**
* Update the internal counter for the g3 log size, and remember which file is open
* so that checkLogFile notices when it is removed or replaced
*/
void LogRotateHelper::setLogSizeCounter() {
   flush();  // the size on disk must include the header
   struct stat st {};
   if (stat(log_file_with_path_.c_str(), &st) == 0) {
      cur_log_size_ = st.st_size;
      log_file_dev_ = st.st_dev;
      log_file_ino_ = st.st_ino;  // always 0 on Windows, which disables the replaced-file check
   } else {
      cur_log_size_ = 0;
      log_file_dev_ = 0;
      log_file_ino_ = 0;
   }
   flush_policy_counter_ = flush_policy_;
}


/**
 * Create a compressed file of the current log.
 * @param file_name
 * @param gzip_file_name
 * @return
 */
bool LogRotateHelper::createCompressedFile(std::string file_name, std::string gzip_file_name) {
   const int buffer_size = 16184;
   char buffer[buffer_size];
   FILE* input = fopen(file_name.c_str(), "rb");
   if (input == NULL) {
      return false;
   }
   gzFile output = gzopen(gzip_file_name.c_str(), "wb");
   if (output == NULL) {
      fclose(input);
      return false;
   }

   bool write_status = true;
   size_t N;
   while ((N = fread(buffer, 1, buffer_size, input)) > 0) {
      if (gzwrite(output, buffer, static_cast<unsigned>(N)) == 0) {
         write_status = false;  // e.g. disk full
         break;
      }
   }
   write_status = write_status && (ferror(input) == 0);
   bool close_status = (gzclose(output) == Z_OK);
   close_status = (fclose(input) == 0)  && close_status;

   // the original is deleted next, so the archive must survive a power cut first
   if (!write_status || !close_status || !syncToDisk(gzip_file_name)) {
      std::error_code ec;
      std::filesystem::remove(gzip_file_name, ec);  // don't leave a truncated archive behind
      return false;
   }
   // best effort: makes the archive's directory entry durable, not every file system supports it
   syncToDisk(std::filesystem::path(gzip_file_name).parent_path().string());
   return true;
}

std::string LogRotateHelper::logFileName() {
   return log_file_with_path_;
}

void LogRotateHelper::addLogFileHeader() {
   filestream() << header();
}
