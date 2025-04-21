/*============================================================================
 * Base system information (System and Library dependent)
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2021 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <assert.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)
#include <stdio.h>
#endif

#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif

#if defined HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif

#if defined(HAVE_SYS_SYSINFO_H) && defined(HAVE_SYSINFO)
#include <sys/sysinfo.h>
#endif

#if defined(HAVE_GETPWUID) && defined(HAVE_GETEUID)
#include <pwd.h>
#endif

#if defined(HAVE_CUDA)
#include "cs_base_cuda.h"
#endif

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "bft_printf.h"
#include "cs_log.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_system_info.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*============================================================================
 * Local type definitions
 *============================================================================*/

/*============================================================================
 *  Global variables
 *============================================================================*/

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Remove leading and trailing whitespace from a string.
 *
 * parameters:
 *   s <-> string to be cleaned
 *----------------------------------------------------------------------------*/

static void
_string_clean(char  *s)
{
  assert(s != NULL);

  int l = strlen(s);
  int i = l - 1;
  while (i > -1 && (   s[i] == ' ' || s[i] == '\t'
                    || s[i] == '\n' || s[i] == '\r')) {
    s[i--] = '\0';
  }

  i = 0;
  while (i < l && (   s[i] == ' ' || s[i] == '\t'
                   || s[i] == '\n' || s[i] == '\r')) {
    i++;
  }

  if (i > 0) {
    int j = 0;
    while (i <= l)
      s[j++] = s[i++];
  }
}

/*----------------------------------------------------------------------------
 * Return basic available CPU info depending on system.
 *
 * parameters:
 *   cpu_str     --> string with CPU description
 *   cpu_str_max <-- maximum length of string with CPU description
 *----------------------------------------------------------------------------*/

static void
_sys_info_cpu(char      *cpu_str,
              unsigned   cpu_str_max)
{
  strcpy(cpu_str, "");

#if defined(__linux__) && !defined(__ve__)

  {
    unsigned _cpu_str_max = (cpu_str_max > 0) ? cpu_str_max - 1 : 0;

    FILE *fp;
    char *s;
    int   i;

    fp = fopen("/proc/cpuinfo", "r");

    if (fp != NULL) {

      s = fgets(cpu_str, _cpu_str_max, fp);

      while (s != NULL && strncmp(s, "model name", 10) != 0)
        s = fgets(cpu_str, _cpu_str_max, fp);

      if (s != NULL) {
        for ( ; *s != '\0' && *s != ':' ; s++);
        if (*s == ':')
          s++;
        for ( ; *s != '\0' && *s == ' ' ; s++);
        for (i = strlen(s) - 1;
             i > 0 && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r');
             s[i--] = '\0');
      }

      fclose (fp);

    }
  }

#elif defined(HAVE_SYS_UTSNAME_H)

  {
    struct utsname  sys_config;

    if (uname(&sys_config) != -1)
      strncpy(cpu_str, sys_config.machine, cpu_str_max);
  }

#endif /* HAVE_SYS_UTSNAME_H */
}

/*----------------------------------------------------------------------------
 * Return Linux info based on /etc/issue.
 *
 * Only the information prior to a first escape sequence is returned.
 *
 * parameters:
 *   issue_str     --> string with system description
 *   issue_str_max <-- maximum length of string with system description
 *----------------------------------------------------------------------------*/

static void
_sys_info_issue(char      *issue_str,
                unsigned   issue_str_max)
{
  issue_str[0] = '\0';

#if defined(__linux__)

  {
    unsigned _issue_str_max = (issue_str_max > 0) ? issue_str_max - 1 : 0;

    FILE *fp;
    char *s;

    fp = fopen("/etc/issue", "r");

    if (fp != NULL) {

      issue_str[0] = ' ';
      issue_str[1] = '(';

      s = fgets(issue_str + 2, _issue_str_max - 4, fp);

      if (s != NULL) {
        int l = strlen(s);
        for (int i = 0; i < l; i++)
          if (s[i] == '\\') {
            s[i] = '\0';
            l = i;
          }
        _string_clean(issue_str + 2);
        l = strlen(issue_str);
        if (l > 2) {
          issue_str[l] = ')';
          issue_str[l+1] = '\0';
        }
        else /* If no info was kept, empty string */
          issue_str[0] = '\0';
      }


      fclose (fp);
    }

  }

#endif
}

#if defined(HAVE_MPI)

/*----------------------------------------------------------------------------*/
/*!
 * \brief Determine min an max number of ranks per node
 *
 * \param[in]  comm            associated MPI communicator
 * \param[in]  ranks_per_node  number of ranks per node (min and max)
 */
/*----------------------------------------------------------------------------*/

static void
_mpi_ranks_per_node(MPI_Comm  comm,
                    int       ranks_per_node[2])
{
#if (MPI_VERSION < 3)
  ranks_per_node[0] = -1;
  ranks_per_node[1] = -1;
#else

  MPI_Comm sh_comm;
  MPI_Comm_split_type(comm, MPI_COMM_TYPE_SHARED, 0,
                      MPI_INFO_NULL, &sh_comm);
  int sh_ranks;
  MPI_Comm_rank(sh_comm, &sh_ranks);
  sh_ranks += 1;
  MPI_Allreduce(MPI_IN_PLACE, &sh_ranks, 1, MPI_INT, MPI_MAX, sh_comm);
  MPI_Comm_free(&sh_comm);

  MPI_Allreduce(&sh_ranks, ranks_per_node, 1, MPI_INT, MPI_MIN, comm);
  MPI_Allreduce(&sh_ranks, ranks_per_node+1, 1, MPI_INT, MPI_MAX, comm);

#endif
}

#endif /* defined(HAVE_MPI) */

/*----------------------------------------------------------------------------*/
/*!
 * \brief Print available system information.
 *
 * \param[in]  comm  associated MPI communicator
 * \param[in]  log   if true, standard logging; otherwise, single output
 */
/*----------------------------------------------------------------------------*/

#if defined(HAVE_MPI)

static void
_system_info(MPI_Comm comm,
             bool     log)

#else

static void
_system_info(bool  log)

#endif

{
  time_t          date;
  size_t          ram;
  int             n_logs = (log) ? 2 : 1;

  cs_log_t logs[] = {CS_LOG_DEFAULT, CS_LOG_PERFORMANCE};

#if defined(HAVE_UNAME)
  struct utsname  sys_config;
#endif
#if defined(HAVE_GETPWUID) && defined(HAVE_GETEUID)
  struct passwd   *pwd_user = NULL;
#endif

#if !defined(PATH_MAX)
#define PATH_MAX 1024
#endif

  char  str_date[81];
  char  str_cpu[81];
  char  str_issue[81];
  char  str_directory[PATH_MAX] = "";

# if defined(HAVE_MPI)
  int mpi_flag = 0;
#endif

  /* Date */

  if (   time(&date) == -1
      || strftime(str_date, 80, "%c", localtime(&date)) == 0)
    strcpy(str_date, "");

  /* Working directory */

#if defined(HAVE_GETCWD)
  if (getcwd(str_directory, 1024) == NULL)
    strcpy(str_directory, "");
#endif

  /* Print local configuration */
  /*---------------------------*/

  if (log) {
    for (int log_id = 0; log_id < n_logs; log_id++)
      cs_log_printf(logs[log_id],
                    "\n%s\n", _("Local case configuration:\n"));
  }

  for (int log_id = 0; log_id < n_logs; log_id++)
    cs_log_printf(logs[log_id],
                  "  %s%s\n", _("Date:                "), str_date);

  /* System and machine */

  _sys_info_issue(str_issue, 81);

#if defined(HAVE_UNAME)

  if (uname(&sys_config) != -1) {
    for (int log_id = 0; log_id < n_logs; log_id++) {
      cs_log_printf(logs[log_id],
                    "  %s%s %s%s\n", _("System:              "),
                    sys_config.sysname, sys_config.release,
                    str_issue);
      cs_log_printf(logs[log_id],
                    "  %s%s\n", _("Machine:             "),
                    sys_config.nodename);
    }
  }
#endif

  _sys_info_cpu(str_cpu, 81);

  for (int log_id = 0; log_id < n_logs; log_id++)
    cs_log_printf(logs[log_id],
                  "  %s%s\n", _("Processor:           "), str_cpu);

  /* Available memory */

#if defined(__linux__) \
   && defined(HAVE_SYS_SYSINFO_H) && defined(HAVE_SYSINFO)
  {
    struct sysinfo info;
    sysinfo(&info);
    ram = info.totalram / (size_t)(1024*1024);
  }
#else
  ram = 0; /* TODO: complete for Windows systems */
#endif

  if (ram > 0) {
    for (int log_id = 0; log_id < n_logs; log_id++)
      cs_log_printf(logs[log_id],
                    "  %s%llu %s\n", _("Memory:              "),
                    (unsigned long long)ram, _("MB"));
  }

  /* User info */

#if defined(HAVE_GETPWUID) && defined(HAVE_GETEUID)

  /* Functions not available on Cray XT,
     but a stub may exist, so we make sure we ignore it */
#if   defined(_CRAYC) \
   || defined(__CRAYXT) || defined(__CRAYXE) || defined(__CRAYXC)
  pwd_user = NULL;
#else
  pwd_user = getpwuid(geteuid());
#endif

  if (pwd_user != NULL) {

    size_t l_info = 0;

    cs_log_printf(CS_LOG_DEFAULT,
                  "  %s%s", _("User:                "), pwd_user->pw_name);

    if (pwd_user->pw_gecos != NULL) {
      for (l_info = 0;
           (   pwd_user->pw_gecos[l_info] != '\0'
            && pwd_user->pw_gecos[l_info] != ',');
           l_info++);
      if (pwd_user->pw_gecos[l_info] == ',')
        pwd_user->pw_gecos[l_info] = '\0';
      cs_log_printf(CS_LOG_DEFAULT,
                    " (%s)", pwd_user->pw_gecos);
    }

    cs_log_printf(CS_LOG_DEFAULT, "\n");
  }

#endif /* defined(HAVE_GETPWUID) && defined(HAVE_GETEUID) */

  /* Directory info */

  for (int log_id = 0; log_id < n_logs; log_id++)
    cs_log_printf(logs[log_id],
                  "  %s%s\n", _("Directory:           "), str_directory);

  /* MPI Info */

#if defined(HAVE_MPI)

  MPI_Initialized(&mpi_flag);

  if (mpi_flag != 0) {

    int n_ranks = 1, n_world_ranks = 1;

    MPI_Comm_size(comm, &n_ranks);
    MPI_Comm_size(MPI_COMM_WORLD, &n_world_ranks);

    int ranks_per_node[2];
    _mpi_ranks_per_node(comm, ranks_per_node);

    {
      int appnum = -1;

#     if defined(MPI_VERSION) && (MPI_VERSION >= 2)
      void *attp = NULL;
      int flag = 0;
      MPI_Comm_get_attr(MPI_COMM_WORLD, MPI_APPNUM, &attp, &flag);
      if (flag != 0)
        appnum = *(int *)attp;
#     endif

      for (int log_id = 0; log_id < n_logs; log_id++) {
        if (appnum > -1 && log_id == 0)
          cs_log_printf(logs[log_id],
                        "  %s%d (%s %d)\n",
                        _("MPI ranks:           "), n_ranks,
                        _("appnum attribute:"), appnum);
        else
          cs_log_printf(logs[log_id],
                        "  %s%d\n", _("MPI ranks:           "), n_ranks);

        if (ranks_per_node[0] > 0 && ranks_per_node[0] < n_ranks) {
          if (ranks_per_node[0] == ranks_per_node[1])
            cs_log_printf(logs[log_id],
                          "  %s%d\n", _("MPI ranks per node:  "),
                          ranks_per_node[0]);
          else
            cs_log_printf(logs[log_id],
                          "  %s%d - %d\n", _("MPI ranks per node:  "),
                          ranks_per_node[0], ranks_per_node[1]);
        }
        if (n_world_ranks > n_ranks)
          cs_log_printf(logs[log_id],
                        "  %s%d\n", _("MPI_COMM_WORLD size: "),
                        n_world_ranks);
      }
    }

  }

#endif /* defined(HAVE_MPI) */

#if defined(HAVE_OPENMP)
  {
    int t_id = omp_get_thread_num();
    if (t_id == 0) {
      for (int log_id = 0; log_id < n_logs; log_id++) {
        cs_log_printf(logs[log_id],
                      "  %s%d\n", _("OpenMP threads:      "),
                      omp_get_max_threads());
        if (omp_get_dynamic())
          cs_log_printf(logs[log_id],
                        "  %s\n", _("Dynamic scheduling allowed"));
        cs_log_printf(logs[log_id],
                      "  %s%d\n", _("Processors/node:     "),
                      omp_get_num_procs());
      }
    }
  }
#endif

#if defined(HAVE_CUDA)
  for (int log_id = 0; log_id < n_logs; log_id++)
    cs_base_cuda_device_info(log_id);
#endif

#if    defined(CS_CC_VERSION_STRING) || defined(CS_CXX_VERSION_STRING) \
    || defined(CS_FC_VERSION_STRING) || defined(CS_NVCC_VERSION_STRING)
  for (int log_id = 0; log_id < n_logs; log_id++) {
    cs_log_printf(logs[log_id], "\n  Compilers used for build:\n");
#   if defined(CS_CC_VERSION_STRING)
    cs_log_printf(logs[log_id],
                  "    %s%s\n", _("C compiler:        "), CS_CC_VERSION_STRING);
#   endif
#   if defined(CS_CXX_VERSION_STRING)
    cs_log_printf(logs[log_id],
                  "    %s%s\n", _("C++ compiler:      "), CS_CXX_VERSION_STRING);
#   endif
#   if defined(CS_FC_VERSION_STRING)
    cs_log_printf(logs[log_id],
                  "    %s%s\n", _("Fortran compiler:  "), CS_FC_VERSION_STRING);
#   endif
#   if defined(CS_NVCC_VERSION_STRING)
    cs_log_printf(logs[log_id],
                  "    %s%s\n", _("CUDA compiler:     "), CS_NVCC_VERSION_STRING);
#   endif
  }
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Print available MPI library information.
 *
 * \param[in]  log   if true, standard logging; otherwise, single output
 */
/*----------------------------------------------------------------------------*/

#if defined(HAVE_MPI)

static void
_mpi_version_info(bool  log)
{
#if defined(MPI_SUBVERSION)

  char mpi_vendor_lib[32] = "";
  char mpi_lib[32] = "";

  /* Base MPI library information */

#if defined(MPI_VENDOR_NAME)

#if defined(OMPI_MAJOR_VERSION)
  snprintf(mpi_lib, 31, "%s %d.%d.%d",
           MPI_VENDOR_NAME,
           OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION, OMPI_RELEASE_VERSION);
#elif defined(MPICH2_VERSION)
  snprintf(mpi_lib, 31, "%s %s", MPI_VENDOR_NAME, MPICH2_VERSION);
#elif defined(MPICH_VERSION)
  snprintf(mpi_lib, 31, "%s %s", MPI_VENDOR_NAME, MPICH_VERSION);
#else
  snprintf(mpi_lib, 31, "%s", MPI_VENDOR_NAME);
#endif

#elif defined(OPEN_MPI)
#if defined(OMPI_MAJOR_VERSION)
  snprintf(mpi_lib, 31, "Open MPI %d.%d.%d",
           OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION, OMPI_RELEASE_VERSION);
#else
  snprintf(mpi_lib, 31, "Open MPI");
#endif

#elif defined(MPICH2)
#if defined(MPICH2_VERSION)
  snprintf(mpi_lib, 31, "MPICH2 %s", MPICH2_VERSION);
#else
  snprintf(mpi_lib, 31, "MPICH2");
#endif
#elif defined(MPICH_NAME)
#if defined(MPICH_VERSION)
  snprintf(mpi_lib, 31, "MPICH %s", MPICH_VERSION);
#else
  snprintf(mpi_lib, 31, "MPICH");
#endif
#endif

  mpi_lib[31] = '\0';

  /* Possible additional MPI vendor information */

#if defined(MVAPICH2_VERSION)
  snprintf(mpi_vendor_lib, 31, "MVAPICH2 %s", MVAPICH2_VERSION);
#elif defined(MSMPI_VER)
  snprintf(mpi_vendor_lib, 31, "MS-MPI");
#elif defined(PLATFORM_MPI)
  {
    int v, v0, v1, v2, v3;
    v0 =  PLATFORM_MPI>>24;
    v =  (PLATFORM_MPI - (v0<<24));
    v1 =  v>>16;
    v =  (v - (v1<<16));
    v2 =  v>>8;
    v3 =  (v - (v2<<8));
    snprintf(mpi_vendor_lib, 31, "Platform MPI %x.%x.%x.%x\n",
             v0, v1, v2, v3);
  }
#endif

  mpi_vendor_lib[31] = '\0';

  int  n_logs = (log) ? 2 : 1;
  cs_log_t logs[] = {CS_LOG_DEFAULT, CS_LOG_PERFORMANCE};

  for (int log_id = 0; log_id < n_logs; log_id++) {

    if (mpi_vendor_lib[0] != '\0') {
      if (mpi_lib[0] != '\0')
        cs_log_printf(logs[log_id],
                      _("\n  MPI version %d.%d (%s, based on %s)\n"),
                      MPI_VERSION, MPI_SUBVERSION, mpi_vendor_lib, mpi_lib);
      else
        cs_log_printf(logs[log_id],
                      _("\n  MPI version %d.%d (%s)\n"),
                      MPI_VERSION, MPI_SUBVERSION, mpi_vendor_lib);
    }
    else {
      if (mpi_lib[0] != '\0')
        cs_log_printf(logs[log_id],
                      _("\n  MPI version %d.%d (%s)\n"),
                      MPI_VERSION, MPI_SUBVERSION, mpi_lib);
      else
        cs_log_printf(logs[log_id],
                      _("\n  MPI version %d.%d\n"),
                      MPI_VERSION, MPI_SUBVERSION);
    }

  }

#endif /* defined(MPI_SUBVERSION) */
}

#endif /* (HAVE_MPI) */

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Print available system information.
 *
 * \param[in]  comm  associated MPI communicator
 */
/*----------------------------------------------------------------------------*/

#if defined(HAVE_MPI)

void
cs_system_info(MPI_Comm comm)

#else

void
cs_system_info(void)

#endif

{
#if defined(HAVE_MPI)
  _system_info(comm, true);
  _mpi_version_info(false);
#else
  _system_info(true);
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Print available system information, without additional logging
 *
 * \param[in]  comm  associated MPI communicator
 */
/*----------------------------------------------------------------------------*/

#if defined(HAVE_MPI)

void
cs_system_info_no_log(MPI_Comm comm)

#else

void
cs_system_info_no_log(void)

#endif

{
#if defined(HAVE_MPI)
  _system_info(comm, false);
  _mpi_version_info(false);
#else
  _system_info(false);
#endif
}

/*-----------------------------------------------------------------------------*/

END_C_DECLS
