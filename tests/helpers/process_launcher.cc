/* Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "process_launcher.h"

#include <chrono>
#include <string>
#include <system_error>
#include <thread>

#ifdef WIN32
#  include <windows.h>
#  include <tchar.h>
#  include <stdio.h>
#else
#  include <stdio.h>
#  include <unistd.h>
#  include <sys/types.h>
#  include <stdlib.h>
#  include <string.h>
#  include <sys/wait.h>
#  include <string.h>
#  include <poll.h>
#  include <errno.h>
#  include <signal.h>
#  include <fcntl.h>
#endif

#ifdef _WIN32

void ProcessLauncher::start() {
  SECURITY_ATTRIBUTES saAttr;

  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  if (!CreatePipe(&child_out_rd, &child_out_wr, &saAttr, 0))
    report_error("Failed to create child_out_rd");

  if (!SetHandleInformation(child_out_rd, HANDLE_FLAG_INHERIT, 0))
    report_error("Failed to create child_out_rd");

  // force non blocking IO in Windows
  DWORD mode = PIPE_NOWAIT;
  //BOOL res = SetNamedPipeHandleState(child_out_rd, &mode, NULL, NULL);

  if (!CreatePipe(&child_in_rd, &child_in_wr, &saAttr, 0))
    report_error("Failed to create child_in_rd");

  if (!SetHandleInformation(child_in_wr, HANDLE_FLAG_INHERIT, 0))
    report_error("Failed to created child_in_wr");

  // Create Process
  std::string s = this->cmd_line;
  const char **pc = args;
  while (*++pc != NULL) {
    s += " ";
    s += *pc;
  }
  char *sz_cmd_line = (char *)malloc(s.length() + 1);
  if (!sz_cmd_line)
    report_error("Cannot assign memory for command line in ProcessLauncher::start");
  _tcscpy(sz_cmd_line, s.c_str());

  BOOL bSuccess = FALSE;

  ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

  ZeroMemory(&si, sizeof(STARTUPINFO));
  si.cb = sizeof(STARTUPINFO);
  if (redirect_stderr)
    si.hStdError = child_out_wr;
  si.hStdOutput = child_out_wr;
  si.hStdInput = child_in_rd;
  si.dwFlags |= STARTF_USESTDHANDLES;

  bSuccess = CreateProcess(
    NULL,          // lpApplicationName
    sz_cmd_line,     // lpCommandLine
    NULL,          // lpProcessAttributes
    NULL,          // lpThreadAttributes
    TRUE,          // bInheritHandles
    0,             // dwCreationFlags
    NULL,          // lpEnvironment
    NULL,          // lpCurrentDirectory
    &si,           // lpStartupInfo
    &pi);          // lpProcessInformation

  if (!bSuccess)
    report_error(NULL);
  else
    is_alive = true;

  CloseHandle(child_out_wr);
  CloseHandle(child_in_rd);

  //DWORD res1 = WaitForInputIdle(pi.hProcess, 100);
  //res1 = WaitForSingleObject(pi.hThread, 100);
  free(sz_cmd_line);
}

uint64_t ProcessLauncher::get_pid() {
  return (uint64_t)pi.hProcess;
}

int ProcessLauncher::wait(unsigned int timeout_ms) {
  DWORD dwExit = 0;
  BOOL get_ret{FALSE};
  if (get_ret = GetExitCodeProcess(pi.hProcess, &dwExit)) {
    if (dwExit == STILL_ACTIVE) {
      auto wait_ret = WaitForSingleObject(pi.hProcess, timeout_ms);
      if (wait_ret == 0) {
        get_ret = GetExitCodeProcess(pi.hProcess, &dwExit);
      }
      else {
        throw std::runtime_error("Error waiting for process exit: " + std::to_string(wait_ret));
      }
    }
  }
  if (get_ret == FALSE) {
    DWORD dwError = GetLastError();
    if (dwError != ERROR_INVALID_HANDLE)  // not closed already?
      report_error(NULL);
    else
      dwExit = 128; // Invalid handle
  }
  return dwExit;
}

void ProcessLauncher::close() {
  DWORD dwExit;
  if (GetExitCodeProcess(pi.hProcess, &dwExit)) {
    if (dwExit == STILL_ACTIVE) {
      if (!TerminateProcess(pi.hProcess, 0))
        report_error(NULL);
      // TerminateProcess is async, wait for process to end.
      WaitForSingleObject(pi.hProcess, INFINITE);
    }
  } else {
    if (is_alive)
      report_error(NULL);
  }

  if (!CloseHandle(pi.hProcess))
    report_error(NULL);
  if (!CloseHandle(pi.hThread))
    report_error(NULL);

  if (!CloseHandle(child_out_rd))
    report_error(NULL);
  if (!CloseHandle(child_in_wr))
    report_error(NULL);

  is_alive = false;
}

int ProcessLauncher::read(char *buf, size_t count, unsigned timeout_ms) {
  DWORD dwBytesRead;
  COMMTIMEOUTS timeouts = { 0, 0, timeout_ms, 0, 0 };
  SetCommTimeouts(child_out_rd, &timeouts);

  BOOL bSuccess = ReadFile(child_out_rd, buf, count, &dwBytesRead, NULL);
  if (bSuccess == FALSE) {
    DWORD dwCode = GetLastError();
    if (dwCode == ERROR_NO_DATA || dwCode == ERROR_BROKEN_PIPE)
      return EOF;
    else
      report_error(NULL);
  }

  return dwBytesRead;
}

int ProcessLauncher::write(const char *buf, size_t count) {
  DWORD dwBytesWritten;
  BOOL bSuccess = FALSE;
  bSuccess = WriteFile(child_in_wr, buf, count, &dwBytesWritten, NULL);
  if (!bSuccess) {
    if (GetLastError() != ERROR_NO_DATA)  // otherwise child process just died.
      report_error(NULL);
  } else {
    // When child input buffer is full, this returns zero in NO_WAIT mode.
    return dwBytesWritten;
  }
  return 0; // so the compiler does not cry
}

void ProcessLauncher::report_error(const char *msg, const char* prefix) {
  DWORD dwCode = GetLastError();
  LPTSTR lpMsgBuf;

  if (msg != NULL) {
    throw std::system_error(dwCode, std::generic_category(), msg);
  } else {
    FormatMessage(
      FORMAT_MESSAGE_ALLOCATE_BUFFER |
      FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL,
      dwCode,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPTSTR)&lpMsgBuf,
      0, NULL);
    std::string msgerr;
    if (prefix != "") {
      msgerr += std::string(prefix) + "; ";
    }
    msgerr += "SystemError: ";
    msgerr += lpMsgBuf;
    msgerr += "with error code %d" + std::to_string(dwCode) + ".";
    throw std::system_error(dwCode, std::generic_category(), msgerr);
  }
}

uint64_t ProcessLauncher::get_fd_write() {
  return (uint64_t)child_in_wr;
}

uint64_t ProcessLauncher::get_fd_read() {
  return (uint64_t)child_out_rd;
}

#else

void ProcessLauncher::start()
{
  if( pipe(fd_in) < 0 )
  {
    report_error(NULL, "ProcessLauncher::start() pipe(fd_in)");
  }
  if( pipe(fd_out) < 0 )
  {
    report_error(NULL, "ProcessLauncher::start() pipe(fd_out)");
  }

  // Ignore broken pipe signal
  signal(SIGPIPE, SIG_IGN);

  childpid = fork();
  if(childpid == -1)
  {
    report_error(NULL, "ProcessLauncher::start() fork()");
  }

  if(childpid == 0)
  {
#ifdef LINUX
    prctl(PR_SET_PDEATHSIG, SIGHUP);
#endif

    ::close(fd_out[0]);
    ::close(fd_in[1]);
    while( dup2(fd_out[1], STDOUT_FILENO) == -1 )
    {
      if(errno == EINTR) continue;
      else report_error(NULL, "ProcessLauncher::start() dup2()");
    }

    if(redirect_stderr)
    {
      while( dup2(fd_out[1], STDERR_FILENO) == -1 )
      {
        if(errno == EINTR) continue;
        else report_error(NULL, "ProcessLauncher::start() dup2()");
      }
    }
    while( dup2(fd_in[0], STDIN_FILENO) == -1 )
    {
      if(errno == EINTR) continue;
      else report_error(NULL, "ProcessLauncher::start() dup2()");
    }

    fcntl(fd_out[1], F_SETFD, FD_CLOEXEC);
    fcntl(fd_in[0], F_SETFD, FD_CLOEXEC);

    execvp(cmd_line, (char * const *)args);
    // if exec returns, there is an error.
    int my_errno = errno;
    fprintf(stderr, "%s could not be executed: %s (errno %d)\n", cmd_line, strerror(my_errno), my_errno);

    // we need to identify an ENOENT and since some programs return 2 as exit-code
    // we need to return a non-existent code, 128 is a general convention used to indicate
    // a failure to execute another program in a subprocess
    if (my_errno == 2)
      my_errno = 128;

    exit(my_errno);
  }
  else
  {
    ::close(fd_out[1]);
    ::close(fd_in[0]);

    is_alive = true;
  }
}

void ProcessLauncher::close()
{
  if(::kill(childpid, SIGTERM) < 0 && errno != ESRCH) {
    report_error(NULL, "close()");
  }
  if(errno != ESRCH)
  {
    sleep(1);
    if(::kill(childpid, SIGKILL) < 0 && errno != ESRCH)
      report_error(NULL, "kill()");
  }

  ::close(fd_out[0]);
  ::close(fd_in[1]);
  wait();
  is_alive = false;
}

int ProcessLauncher::read(char *buf, size_t count, unsigned timeout_ms)
{
  int n;
  fd_set set;
  struct timeval timeout;
  memset (&timeout, 0x0, sizeof(timeout));
  timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);

  FD_ZERO(&set);
  FD_SET(fd_out[0], &set);

  int res = select(fd_out[0] + 1, &set, NULL, NULL, &timeout);
  if (res < 0) report_error(nullptr, "select()");
  if (res == 0) return 0;

  if((n = (int)::read(fd_out[0], buf, count)) >= 0)
    return n;

  report_error(nullptr, "read");
  return -1;
}

int ProcessLauncher::write(const char *buf, size_t count)
{
  int n;
  if ((n = (int)::write(fd_in[1], buf, count)) >= 0)
    return n;
  if (errno == EPIPE) return 0;
  report_error(NULL, "write");
  return -1;
}

void ProcessLauncher::report_error(const char *msg, const char *prefix)
{
  char sys_err[ 64 ] = {'\0'};
  int errnum = errno;
  if (msg == NULL) {

    // we do this #ifdef dance because on unix systems strerror_r() will generate
    // a warning if we don't collect the result (warn_unused_result attribute)
  #if ((defined _POSIX_C_SOURCE && (_POSIX_C_SOURCE >= 200112L)) ||    \
         (defined _XOPEN_SOURCE && (_XOPEN_SOURCE >= 600)))      &&    \
        ! defined _GNU_SOURCE
    int r = strerror_r(errno, sys_err, sizeof(sys_err));
    (void)r;  // silence unused variable;
  #elif defined(_GNU_SOURCE) && defined(__GLIBC__)
    const char *r = strerror_r(errno, sys_err, sizeof(sys_err));
    (void)r;  // silence unused variable;
  #else
    strerror_r(errno, sys_err, sizeof(sys_err));
  #endif

    std::string s = std::string(prefix) + "; " + std::string(sys_err) +
        "with errno ." + std::to_string(errnum);
    throw std::system_error(errnum, std::generic_category(), s);
  }
  else {
    throw std::system_error(errnum, std::generic_category(), msg);
  }
}

uint64_t ProcessLauncher::get_pid()
{
  return (uint64_t)childpid;
}

int ProcessLauncher::wait(unsigned int timeout_ms)
{
  int status;
  int exited;
  int exitstatus;
  pid_t ret;
  const unsigned SLEEP_MS = 100;

  do
  {
    ret = ::waitpid(childpid, &status, WNOHANG);

    exited = WIFEXITED(status);
    exitstatus = WEXITSTATUS(status);
    if (ret == 0) {
      auto sleep_for = std::min(timeout_ms, SLEEP_MS);
      if (sleep_for > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_for));
        timeout_ms -= sleep_for;
      } else {
        report_error("Timed out waiting for the process to exit");
      }
    } else if (ret == -1)
    {
      if (errno == ECHILD) {
        break; // no children left
      }
      if((exited == 0) || (exitstatus != 0))
      {
        report_error(NULL, "waitpid()");
      }
    } else {
      break;
    }
  }
  while(true);

  return exitstatus;
}

uint64_t ProcessLauncher::get_fd_write()
{
  return (uint64_t)fd_in[1];
}

uint64_t ProcessLauncher::get_fd_read()
{
  return (uint64_t)fd_out[0];
}

#endif

void ProcessLauncher::kill() {
  close();
}